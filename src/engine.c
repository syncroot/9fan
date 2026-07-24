#include "channel.h"
#include "curve.h"
#include "controller.h"
#include "guard_protocol.h"
#include "hot_policy.h"
#include "lease.h"
#include "platform_policy.h"
#include "protocol.h"
#include "response_monitor.h"
#include "signal_guard.h"
#include "smc.h"
#include "thermal_guard.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WATCHDOG_HEARTBEAT_INTERVAL_MS 2000
#define WATCHDOG_HEARTBEAT_BYTE NINEFAN_GUARD_HEARTBEAT_BYTE
#define WATCHDOG_CLEAN_BYTE NINEFAN_GUARD_CLEAN_BYTE
#define WATCHDOG_ARM_BYTE NINEFAN_GUARD_ARM_BYTE
#define WATCHDOG_DISARM_BYTE NINEFAN_GUARD_DISARM_BYTE
#define WATCHDOG_MAXIMUM_BYTE NINEFAN_GUARD_MAXIMUM_BYTE
#define WATCHDOG_HOT_START_BYTE NINEFAN_GUARD_HOT_START_BYTE
#define WATCHDOG_READY_BYTE NINEFAN_GUARD_READY_BYTE
#define WATCHDOG_READY_TIMEOUT_MS 2000
#define HARDWARE_VALIDATION_PATH "/var/db/9fan.validation"
#define HARDWARE_VALIDATION_RECORD_SIZE 512

_Static_assert(
    NINEFAN_PROTOCOL_IO_TIMEOUT_MS < NINEFAN_GUARD_TIMEOUT_MS,
    "Protocol I/O must time out before the independent guard");
_Static_assert(
    NINEFAN_HOT_START_CONTROL_MS < NINEFAN_HOT_START_GUARD_MS,
    "The independent hot-start guard must outlive the engine deadline");

static volatile sig_atomic_t termination_requested;

typedef struct {
    int active;
    int armed;
    int write_fd;
    pid_t pid;
    long long next_heartbeat_ms;
} ninefan_watchdog;

static int watchdog_heartbeat(ninefan_watchdog *watchdog, int force);
static int watchdog_arm(ninefan_watchdog *watchdog);
static int watchdog_disarm(ninefan_watchdog *watchdog);

static int watchdog_progress(void *context) {
    return watchdog_heartbeat((ninefan_watchdog *)context, 1);
}

static void request_termination(int signal_number) {
    (void)signal_number;
    termination_requested = 1;
}

static void fatal_signal(int signal_number) {
    signal(signal_number, SIG_DFL);
    (void)kill(getpid(), signal_number);
    _exit(128 + signal_number);
}

static long long monotonic_milliseconds(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int trusted_engine_directory(
    char output[PATH_MAX], char *message, size_t message_size) {
    char executable[PATH_MAX] = {0};
    uint32_t executable_size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &executable_size) != 0) {
        snprintf(message, message_size, "Could not locate the 9fan executable");
        return -1;
    }

    char resolved[PATH_MAX] = {0};
    if (!realpath(executable, resolved)) {
        snprintf(message, message_size,
            "Could not resolve the 9fan executable: %s", strerror(errno));
        return -1;
    }
    if (strcmp(resolved, "/usr/local/libexec/9fan-engine") != 0) {
        snprintf(message, message_size,
            "9fan-engine must run from its fixed trusted installation path");
        return -1;
    }
    struct stat self = {0};
    if (lstat(resolved, &self) != 0
        || !S_ISREG(self.st_mode)
        || self.st_uid != 0
        || self.st_nlink != 1
        || (self.st_mode & 07777) != 0755) {
        snprintf(message, message_size,
            "9fan-engine is not a safe root-owned executable");
        return -1;
    }
    char *separator = strrchr(resolved, '/');
    if (!separator || separator == resolved) {
        snprintf(message, message_size, "9fan has no trusted executable directory");
        return -1;
    }
    *separator = '\0';

    struct stat prefix = {0}, directory = {0};
    if (lstat("/usr/local", &prefix) != 0
        || !S_ISDIR(prefix.st_mode)
        || prefix.st_uid != 0
        || (prefix.st_mode & 0777) != 0755
        || lstat(resolved, &directory) != 0
        || !S_ISDIR(directory.st_mode)
        || directory.st_uid != 0
        || (directory.st_mode & 0777) != 0755) {
        snprintf(message, message_size,
            "9fan executable directory is not safely root-owned");
        return -1;
    }

    const int count = snprintf(output, PATH_MAX, "%s", resolved);
    if (count <= 0 || count >= PATH_MAX) {
        snprintf(message, message_size,
            "9fan-engine trusted directory path is invalid");
        return -1;
    }
    return 0;
}

static int trusted_guard_path(
    char output[PATH_MAX], char *message, size_t message_size) {
    char directory[PATH_MAX] = {0};
    if (trusted_engine_directory(
            directory, message, message_size) != 0) {
        return -1;
    }
    const int count =
        snprintf(output, PATH_MAX, "%s/9fan-guard", directory);
    struct stat guard = {0};
    if (count <= 0
        || count >= PATH_MAX
        || lstat(output, &guard) != 0
        || !S_ISREG(guard.st_mode)
        || guard.st_uid != 0
        || guard.st_nlink != 1
        || (guard.st_mode & 07777) != 0755) {
        snprintf(message, message_size,
            "9fan-guard is missing or is not a safe root-owned executable");
        return -1;
    }
    return 0;
}

static int acquire_control_lock(void) {
    const char *path = "/var/run/9fan.lock";
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "9fan: could not open control lock %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat metadata = {0};
    if (fstat(fd, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != 0
        || metadata.st_nlink != 1
        || fchmod(fd, 0600) != 0) {
        fprintf(stderr, "9fan: control lock is not a safe root-owned file\n");
        close(fd);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char holder[32] = {0};
        const ssize_t count = read(fd, holder, sizeof(holder) - 1);
        if (count > 0) {
            fprintf(stderr, "9fan: another controller is active (PID %s)\n", holder);
        } else {
            fprintf(stderr, "9fan: another controller is active\n");
        }
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0) {
        (void)dprintf(fd, "%ld", (long)getpid());
    }
    return fd;
}

static void read_sysctl_string(const char *name, char *output, size_t output_size) {
    if (!output || output_size == 0) return;
    output[0] = '\0';
    size_t size = output_size;
    if (sysctlbyname(name, output, &size, NULL, 0) != 0 || size == 0) output[0] = '\0';
    output[output_size - 1] = '\0';
}

static int platform_policy_allows(
    ninefan_smc *smc, char *message, size_t message_size) {
    if (!smc) return 0;
    char model[64] = {0};
    char chip[64] = {0};
    char os_build[64] = {0};
    char schema[17] = {0};
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    read_sysctl_string("kern.osversion", os_build, sizeof(os_build));
    if (ninefan_smc_validation_fingerprint(
            smc, schema, sizeof(schema)) != 0) {
        if (message && message_size > 0) {
            snprintf(message, message_size,
                "SMC schema could not be identified; control is refused");
        }
        return 0;
    }
    const ninefan_platform_identity identity = {
        .model = model,
        .chip = chip,
        .os_build = os_build,
        .fan_count = smc->fan_count,
        .mode_key_format = smc->mode_key_format,
        .schema = schema,
    };
    return ninefan_platform_policy_explain(
        &identity, message, message_size);
}

static int hardware_validation_record(
    ninefan_smc *smc, char *output, size_t output_size) {
    if (!smc || !output || output_size == 0) return -1;
    char model[64], chip[64], os_build[64], fingerprint[17];
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    read_sysctl_string("kern.osversion", os_build, sizeof(os_build));
    if (!model[0]
        || !chip[0]
        || !os_build[0]
        || ninefan_smc_validation_fingerprint(
               smc, fingerprint, sizeof(fingerprint)) != 0) {
        return -1;
    }
    const int count = snprintf(output, output_size,
        "format=2\nversion=%s\nmodel=%s\nchip=%s\nos=%s\n"
        "fans=%d\nmode=%s\nschema=%s\n",
        NINEFAN_VERSION, model, chip, os_build,
        smc->fan_count, smc->mode_key_format, fingerprint);
    return count > 0 && (size_t)count < output_size ? count : -1;
}

static int hardware_validation_matches(ninefan_smc *smc) {
    char expected[HARDWARE_VALIDATION_RECORD_SIZE];
    const int expected_size =
        hardware_validation_record(smc, expected, sizeof(expected));
    if (expected_size <= 0) return 0;
    const int fd = open(
        HARDWARE_VALIDATION_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat metadata = {0};
    char actual[sizeof(expected)] = {0};
    ssize_t total = 0;
    int safe = fstat(fd, &metadata) == 0
        && S_ISREG(metadata.st_mode)
        && metadata.st_uid == 0
        && metadata.st_nlink == 1
        && (metadata.st_mode & 077) == 0
        && metadata.st_size == expected_size;
    while (safe && total < expected_size) {
        const ssize_t count =
            read(fd, actual + total, (size_t)(expected_size - total));
        if (count > 0) {
            total += count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            safe = 0;
        }
    }
    close(fd);
    return safe && total == expected_size
        && memcmp(actual, expected, (size_t)expected_size) == 0;
}

static int write_hardware_validation(ninefan_smc *smc) {
    char record[HARDWARE_VALIDATION_RECORD_SIZE];
    const int record_size =
        hardware_validation_record(smc, record, sizeof(record));
    if (record_size <= 0) return -1;
    const int fd = open(HARDWARE_VALIDATION_PATH,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat metadata = {0};
    int result = 0;
    if (fstat(fd, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != 0
        || metadata.st_nlink != 1
        || fchmod(fd, 0600) != 0) {
        result = -1;
    }
    ssize_t total = 0;
    while (result == 0 && total < record_size) {
        const ssize_t count =
            write(fd, record + total, (size_t)(record_size - total));
        if (count > 0) {
            total += count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            result = -1;
        }
    }
    if (result == 0 && fsync(fd) != 0) result = -1;
    if (close(fd) != 0) result = -1;
    if (result != 0) (void)unlink(HARDWARE_VALIDATION_PATH);
    return result;
}

static void invalidate_hardware_validation(void) {
    (void)unlink(HARDWARE_VALIDATION_PATH);
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
        case 0: return "auto";
        case 1: return "manual";
        case 3: return "system";
        default: return "other";
    }
}

static int refresh_snapshot(
    ninefan_smc *smc, ninefan_controller *controller, char *message, size_t message_size) {
    int result = 0;
    if (ninefan_smc_refresh_fans(smc) != 0) {
        snprintf(message, message_size, "%s", ninefan_smc_error(smc));
        result = -1;
    }
    float temperature = NAN;
    char key[5] = "----";
    if (ninefan_smc_hottest_temperature(smc, &temperature, key) == 0) {
        controller->current_temperature = temperature;
        controller->temperature_valid = 1;
        memcpy(controller->hottest_key, key, 5);
    } else {
        controller->current_temperature = NAN;
        controller->temperature_valid = 0;
        memcpy(controller->hottest_key, "----", 5);
        if (result == 0) {
            snprintf(message, message_size, "%s", ninefan_smc_error(smc));
            result = -1;
        }
    }
    return result;
}

static int restore_controller(
    ninefan_smc *smc,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    char *message,
    size_t message_size) {
    const int result = ninefan_smc_restore_default(smc);
    if (result != 0) {
        snprintf(message, message_size, "Default restore failed: %s", ninefan_smc_error(smc));
        return -1;
    }
    ninefan_controller_reset_runtime(controller);
    ninefan_response_monitor_init(response_monitor);
    snprintf(message, message_size, "Apple automatic control restored");
    return 0;
}

static void restore_after_control_failure(
    ninefan_smc *smc,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    char *message, size_t message_size) {
    controller->curve = NULL;
    if (ninefan_smc_restore_default(smc) == 0) {
        controller->manual_active = 0;
        ninefan_response_monitor_init(response_monitor);
        return;
    }
    const size_t used = strnlen(message, message_size);
    if (used < message_size) {
        snprintf(message + used, message_size - used,
            "; Apple restore also failed: %s", ninefan_smc_error(smc));
    }
}

static int update_controller(
    ninefan_smc *smc,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    ninefan_watchdog *watchdog,
    int force_maximum,
    char *message, size_t message_size) {
    if (!controller->curve) return 0;

    for (int index = 0; index < smc->fan_count; index++) {
        if (!smc->fans[index].valid) {
            snprintf(message, message_size,
                "Fan %d telemetry is invalid; surrendering to Apple control", index);
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
        if (!controller->manual_active && smc->fans[index].mode == 1) {
            if (watchdog_disarm(watchdog) != 0) {
                snprintf(message, message_size,
                    "Fan %d entered manual mode under another controller, "
                    "but the safety guard could not disarm",
                    index);
                return -1;
            }
            snprintf(message, message_size,
                "Fan %d entered manual mode under another controller; "
                "stopping without overwriting it",
                index);
            return -2;
        }
        if (controller->manual_active && smc->fans[index].mode != 1) {
            snprintf(message, message_size,
                "Fan %d left 9fan manual mode; surrendering to Apple control",
                index);
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
    }

    const int needs_manual = ninefan_controller_compute(controller);
    if (needs_manual && force_maximum
        && !ninefan_controller_force_maximum(controller)) {
        snprintf(message, message_size,
            "Pre-handoff maximum request failed; restoring Apple control");
        restore_after_control_failure(
            smc, controller, response_monitor, message, message_size);
        return -1;
    }

    if (!needs_manual) {
        if (controller->manual_active) {
            if (restore_controller(
                    smc, controller, response_monitor,
                    message, message_size) != 0) {
                return -1;
            }
            if (watchdog_disarm(watchdog) != 0) {
                snprintf(message, message_size,
                    "Apple control was restored, but the safety guard "
                    "could not disarm");
                return -1;
            }
        }
        return 0;
    }

    for (int index = 0; index < smc->fan_count; index++) {
        if (watchdog_heartbeat(watchdog, 1) != 0) {
            snprintf(message, message_size,
                "Safety watchdog stopped; restoring Apple control");
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
        const ninefan_fan *fan = &smc->fans[index];
        const float target = ninefan_controller_target(
            controller, fan->minimum_rpm, fan->maximum_rpm,
            fan->target_rpm, fan->actual_rpm);
        if (!isfinite(target)) {
            snprintf(message, message_size,
                "Fan %d target calculation failed; restoring Apple control", index);
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
        const int enabled_now = !controller->manual_active;
        if (enabled_now && watchdog_arm(watchdog) != 0) {
            snprintf(message, message_size,
                "Safety guard could not arm; restoring Apple control");
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
        if (enabled_now
            && ninefan_smc_enable_manual(
                   smc, index, &termination_requested,
                   watchdog_progress, watchdog) != 0) {
            snprintf(message, message_size, "Fan %d manual mode failed: %s",
                index, ninefan_smc_error(smc));
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
        if (enabled_now || !controller->manual_active
            || fabsf(target - fan->target_rpm) >= 75.0f) {
            if (ninefan_smc_set_target(smc, index, target) != 0) {
                snprintf(message, message_size, "Fan %d target failed: %s",
                    index, ninefan_smc_error(smc));
                restore_after_control_failure(
                    smc, controller, response_monitor, message, message_size);
                return -1;
            }
            if (ninefan_response_monitor_note_target(
                    response_monitor, index, target,
                    monotonic_milliseconds()) != 0) {
                snprintf(message, message_size,
                    "Fan %d response monitor could not arm; restoring Apple control",
                    index);
                restore_after_control_failure(
                    smc, controller, response_monitor, message, message_size);
                return -1;
            }
        }
        if (watchdog_heartbeat(watchdog, 1) != 0) {
            snprintf(message, message_size,
                "Safety watchdog stopped; restoring Apple control");
            restore_after_control_failure(
                smc, controller, response_monitor, message, message_size);
            return -1;
        }
    }
    controller->manual_active = 1;
    return 0;
}

static int verify_fan_response(
    ninefan_smc *smc,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    char *message,
    size_t message_size) {
    if (!controller->manual_active) return 0;
    const long long now = monotonic_milliseconds();
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        float required_rpm = NAN;
        const ninefan_response_result result =
            ninefan_response_monitor_observe(
                response_monitor,
                index,
                fan->minimum_rpm,
                fan->actual_rpm,
                fan->target_rpm,
                now,
                &required_rpm);
        if (result == NINEFAN_RESPONSE_OK
            || result == NINEFAN_RESPONSE_GRACE) {
            continue;
        }
        if (result == NINEFAN_RESPONSE_TARGET_CHANGED) {
            snprintf(message, message_size,
                "Fan %d target changed outside 9fan; surrendering to Apple control",
                index);
        } else if (result == NINEFAN_RESPONSE_STALLED) {
            snprintf(message, message_size,
                "Fan %d stayed below %.0f RPM while target was %.0f; "
                "surrendering to Apple control",
                index, required_rpm, fan->target_rpm);
        } else {
            snprintf(message, message_size,
                "Fan %d response could not be verified; surrendering to Apple control",
                index);
        }
        restore_after_control_failure(
            smc, controller, response_monitor, message, message_size);
        return -1;
    }
    return 0;
}

static int watchdog_heartbeat(ninefan_watchdog *watchdog, int force) {
    if (!watchdog || !watchdog->active) return -1;
    const long long now = monotonic_milliseconds();
    if (!force && now < watchdog->next_heartbeat_ms) return 0;
    const char heartbeat = WATCHDOG_HEARTBEAT_BYTE;
    ssize_t written;
    do {
        written = write(watchdog->write_fd, &heartbeat, 1);
    } while (written < 0 && errno == EINTR);
    if (written != 1) return -1;
    watchdog->next_heartbeat_ms = now + WATCHDOG_HEARTBEAT_INTERVAL_MS;
    return 0;
}

static int watchdog_arm(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return -1;
    if (watchdog->armed) return 0;
    const char arm = WATCHDOG_ARM_BYTE;
    ssize_t written;
    do {
        written = write(watchdog->write_fd, &arm, 1);
    } while (written < 0 && errno == EINTR);
    if (written != 1) return -1;
    watchdog->armed = 1;
    return 0;
}

static int watchdog_disarm(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return -1;
    if (!watchdog->armed) return 0;
    const char disarm = WATCHDOG_DISARM_BYTE;
    ssize_t written;
    do {
        written = write(watchdog->write_fd, &disarm, 1);
    } while (written < 0 && errno == EINTR);
    if (written != 1) return -1;
    watchdog->armed = 0;
    return 0;
}

static int watchdog_limit_maximum(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return -1;
    const char limit = WATCHDOG_MAXIMUM_BYTE;
    ssize_t written;
    do {
        written = write(watchdog->write_fd, &limit, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1 ? 0 : -1;
}

static int watchdog_limit_hot_start(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return -1;
    const char limit = WATCHDOG_HOT_START_BYTE;
    ssize_t written;
    do {
        written = write(watchdog->write_fd, &limit, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1 ? 0 : -1;
}

static void wait_for_watchdog(pid_t pid) {
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        /* Keep the child accounted for when a termination signal interrupts waitpid. */
    }
}

static int watchdog_start(
    ninefan_watchdog *watchdog, ninefan_smc *smc, int control_lock_fd,
    uint64_t lease_remaining_ms,
    char *message, size_t message_size) {
    if (watchdog->active) return 0;
    if (lease_remaining_ms == 0
        || lease_remaining_ms > NINEFAN_CURVE_LEASE_MAX_MS) {
        snprintf(message, message_size,
            "Safety lease is invalid or has expired");
        return -1;
    }

    char guard_path[PATH_MAX] = {0};
    if (trusted_guard_path(guard_path, message, message_size) != 0) {
        return -1;
    }

    /* The guard starts without inheriting the parent's IOKit connection. */
    ninefan_smc_close(smc);
    int heartbeat_descriptors[2] = {-1, -1};
    int ready_descriptors[2] = {-1, -1};
    if (pipe(heartbeat_descriptors) != 0
        || pipe(ready_descriptors) != 0) {
        if (heartbeat_descriptors[0] >= 0) close(heartbeat_descriptors[0]);
        if (heartbeat_descriptors[1] >= 0) close(heartbeat_descriptors[1]);
        if (ready_descriptors[0] >= 0) close(ready_descriptors[0]);
        if (ready_descriptors[1] >= 0) close(ready_descriptors[1]);
        snprintf(message, message_size, "Could not create safety watchdog: %s", strerror(errno));
        (void)ninefan_smc_open(smc);
        return -1;
    }
    if (fcntl(heartbeat_descriptors[0], F_SETFD, FD_CLOEXEC) != 0
        || fcntl(heartbeat_descriptors[1], F_SETFD, FD_CLOEXEC) != 0
        || fcntl(heartbeat_descriptors[1], F_SETFL, O_NONBLOCK) != 0
        || fcntl(ready_descriptors[0], F_SETFD, FD_CLOEXEC) != 0
        || fcntl(ready_descriptors[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(heartbeat_descriptors[0]);
        close(heartbeat_descriptors[1]);
        close(ready_descriptors[0]);
        close(ready_descriptors[1]);
        snprintf(message, message_size,
            "Could not configure safety watchdog: %s", strerror(errno));
        (void)ninefan_smc_open(smc);
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(heartbeat_descriptors[0]);
        close(heartbeat_descriptors[1]);
        close(ready_descriptors[0]);
        close(ready_descriptors[1]);
        snprintf(message, message_size, "Could not start safety watchdog: %s", strerror(errno));
        (void)ninefan_smc_open(smc);
        return -1;
    }
    if (pid == 0) {
        close(heartbeat_descriptors[1]);
        close(ready_descriptors[0]);
        if (control_lock_fd >= 0) close(control_lock_fd);
        if (fcntl(heartbeat_descriptors[0], F_SETFD, 0) != 0
            || fcntl(ready_descriptors[1], F_SETFD, 0) != 0) {
            _exit(127);
        }
        char heartbeat_fd[24], ready_fd[24], lease_ms[32];
        snprintf(heartbeat_fd, sizeof(heartbeat_fd), "%d", heartbeat_descriptors[0]);
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready_descriptors[1]);
        snprintf(lease_ms, sizeof(lease_ms), "%llu",
            (unsigned long long)lease_remaining_ms);
        execl(guard_path, guard_path,
            "--watch-fd", heartbeat_fd,
            "--ready-fd", ready_fd,
            "--protocol", NINEFAN_GUARD_PROTOCOL_TEXT,
            "--lease-ms", lease_ms,
            (char *)NULL);
        _exit(127);
    }

    close(heartbeat_descriptors[0]);
    close(ready_descriptors[1]);

    struct pollfd ready_poll = {
        .fd = ready_descriptors[0],
        .events = POLLIN | POLLHUP,
        .revents = 0,
    };
    int poll_result;
    do {
        poll_result =
            poll(&ready_poll, 1, WATCHDOG_READY_TIMEOUT_MS);
    } while (poll_result < 0 && errno == EINTR);
    char ready = '\0';
    const ssize_t ready_count =
        poll_result > 0 ? read(ready_descriptors[0], &ready, 1) : -1;
    close(ready_descriptors[0]);
    if (ready_count != 1 || ready != WATCHDOG_READY_BYTE) {
        close(heartbeat_descriptors[1]);
        wait_for_watchdog(pid);
        snprintf(message, message_size,
            "Independent safety guard did not become ready");
        (void)ninefan_smc_open(smc);
        return -1;
    }

    watchdog->active = 1;
    watchdog->write_fd = heartbeat_descriptors[1];
    watchdog->pid = pid;
    if (ninefan_smc_open(smc) != 0) {
        snprintf(message, message_size, "AppleSMC reopen failed: %s", ninefan_smc_error(smc));
        const char clean = WATCHDOG_CLEAN_BYTE;
        (void)write(watchdog->write_fd, &clean, 1);
        close(watchdog->write_fd);
        wait_for_watchdog(watchdog->pid);
        memset(watchdog, 0, sizeof(*watchdog));
        return -1;
    }
    if (watchdog_heartbeat(watchdog, 1) != 0) {
        snprintf(message, message_size, "Safety watchdog stopped unexpectedly");
        close(watchdog->write_fd);
        wait_for_watchdog(watchdog->pid);
        memset(watchdog, 0, sizeof(*watchdog));
        return -1;
    }
    return 0;
}

static void watchdog_clean_stop(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return;
    const char clean = WATCHDOG_CLEAN_BYTE;
    (void)write(watchdog->write_fd, &clean, 1);
    close(watchdog->write_fd);
    wait_for_watchdog(watchdog->pid);
    memset(watchdog, 0, sizeof(*watchdog));
}

static void watchdog_trigger_restore(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return;
    /* EOF, without the clean byte, tells the child to perform its own restore. */
    close(watchdog->write_fd);
    wait_for_watchdog(watchdog->pid);
    memset(watchdog, 0, sizeof(*watchdog));
}

static int send_event(
    int kind,
    int status,
    ninefan_exit_reason exit_reason,
    const ninefan_smc *smc,
    const ninefan_controller *controller,
    const ninefan_lease *lease,
    const char *message) {
    ninefan_event event = {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = (uint16_t)kind,
        .status = status,
        .exit_reason = (uint8_t)exit_reason,
        .hotspot_c = NAN,
        .requested_fraction = NAN,
    };
    if (smc) {
        event.fan_count = (uint8_t)smc->fan_count;
        for (int index = 0;
             index < smc->fan_count && index < NINEFAN_MAX_FANS;
             index++) {
            const ninefan_fan *fan = &smc->fans[index];
            event.fans[index] = (ninefan_protocol_fan) {
                .actual_rpm = fan->actual_rpm,
                .target_rpm = fan->target_rpm,
                .minimum_rpm = fan->minimum_rpm,
                .maximum_rpm = fan->maximum_rpm,
                .mode = fan->mode,
                .valid = (uint8_t)(fan->valid != 0),
            };
        }
    }
    if (controller) {
        event.hotspot_c = controller->current_temperature;
        event.requested_fraction = controller->requested_fraction;
        event.temperature_valid = (uint8_t)controller->temperature_valid;
        event.manual_active = (uint8_t)controller->manual_active;
        memcpy(event.hottest_key, controller->hottest_key, 5);
        if (controller->curve) {
            const ptrdiff_t curve_index =
                controller->curve - ninefan_curves;
            if (curve_index >= 0
                && (size_t)curve_index < ninefan_curve_count) {
                event.selected_curve = (uint8_t)(curve_index + 1);
            }
        }
    }
    if (lease && lease->active) {
        const uint64_t remaining = ninefan_lease_remaining_ms(
            lease, ninefan_continuous_time_ns());
        event.lease_remaining_seconds =
            remaining / 1000ULL > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)(remaining / 1000ULL);
    }
    const ninefan_thermal_state thermal_state =
        ninefan_thermal_state_current();
    snprintf(event.thermal_state, sizeof(event.thermal_state), "%s",
        ninefan_thermal_state_name(thermal_state));
    if (message) {
        snprintf(event.message, sizeof(event.message), "%s", message);
    }
    return ninefan_protocol_write_full(
        STDOUT_FILENO, &event, sizeof(event),
        NINEFAN_PROTOCOL_IO_TIMEOUT_MS, &termination_requested);
}

static int hot_start_preflight(
    const ninefan_smc *smc,
    const ninefan_controller *controller,
    char *message,
    size_t message_size) {
    if (!smc
        || !controller
        || !controller->temperature_valid
        || !isfinite(controller->current_temperature)
        || controller->current_temperature < NINEFAN_APPLE_HANDOFF_C) {
        snprintf(message, message_size,
            "Hot-start maximum requires a valid hotspot at or above %.0f C",
            NINEFAN_APPLE_HANDOFF_C);
        return -1;
    }
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        if (!ninefan_hot_start_fan_eligible(
                fan->valid, fan->mode, fan->target_rpm,
                fan->actual_rpm, fan->maximum_rpm,
                1.0f)) {
            snprintf(message, message_size,
                "Fan %d is not safely eligible for a hot-start maximum; "
                "Apple control must remain active",
                index);
            return -1;
        }
    }
    return 0;
}

static int select_curve(
    const ninefan_curve *curve, ninefan_controller *controller, ninefan_watchdog *watchdog,
    ninefan_smc *smc, ninefan_lease *lease,
    ninefan_hot_policy *hot_policy,
    int *hot_start_requested,
    char *message, size_t message_size, int control_lock_fd) {
    if (geteuid() != 0) return -1;
    if (hot_start_requested) *hot_start_requested = 0;
    if (!platform_policy_allows(smc, message, message_size)) return -1;
    const uint64_t remaining_ms = ninefan_lease_remaining_ms(
        lease, ninefan_continuous_time_ns());
    if (remaining_ms == 0) {
        snprintf(message, message_size,
            "Safety lease expired; Apple control remains active");
        return -1;
    }
    const ninefan_thermal_state thermal_state =
        ninefan_thermal_state_current();
    if (!ninefan_thermal_state_allows_control(thermal_state)) {
        snprintf(message, message_size,
            "System thermal state is %s; custom control is refused",
            ninefan_thermal_state_name(thermal_state));
        return -1;
    }
    if (refresh_snapshot(smc, controller, message, message_size) != 0) {
        return -1;
    }
    (void)ninefan_hot_policy_observe(
        hot_policy, controller->temperature_valid,
        controller->current_temperature);
    if (!ninefan_hot_policy_allows_manual(hot_policy)
        && curve != ninefan_curve_named("max")) {
        snprintf(message, message_size,
            "Hot-temperature lockout is active; press 4 for one guarded "
            "hot-start maximum, or wait until the hotspot falls to %.0f C",
            NINEFAN_MANUAL_REARM_C);
        return -1;
    }
    if (!ninefan_hot_policy_allows_manual(hot_policy)
        && !ninefan_hot_policy_hot_start_available(hot_policy)) {
        snprintf(message, message_size,
            "The guarded hot-start maximum was already used during this "
            "lockout; Apple control remains active until %.0f C",
            NINEFAN_MANUAL_REARM_C);
        return -1;
    }
    if (!ninefan_hot_policy_allows_manual(hot_policy)
        && (hot_start_preflight(
                smc, controller, message, message_size) != 0
            || !hot_start_requested)) {
        return -1;
    }
    if (!ninefan_hot_policy_allows_manual(hot_policy)) {
        *hot_start_requested = 1;
    }
    if (smc->temperature_keys_saturated) {
        snprintf(message, message_size,
            "Temperature-key discovery is incomplete or saturated; "
            "custom control is refused");
        return -1;
    }
    if (!watchdog->active && !controller->manual_active) {
        for (int index = 0; index < smc->fan_count; index++) {
            if (smc->fans[index].valid && smc->fans[index].mode == 1) {
                snprintf(message, message_size,
                    "Fan %d is already in manual mode; stop the other controller "
                    "or restore Apple control first",
                    index);
                return -1;
            }
        }
    }
    if (!hardware_validation_matches(smc)) {
        snprintf(message, message_size,
            "Hardware validation is missing or stale; run "
            "/usr/local/bin/9fan self-test first");
        return -1;
    }
    const int guard_started_here = !watchdog->active;
    if (watchdog_start(
            watchdog, smc, control_lock_fd, remaining_ms,
            message, message_size) != 0) {
        return -1;
    }
    const ninefan_thermal_state rechecked_thermal_state =
        ninefan_thermal_state_current();
    if (!ninefan_thermal_state_allows_control(rechecked_thermal_state)
        || refresh_snapshot(smc, controller, message, message_size) != 0
        || smc->temperature_keys_saturated
        || !hardware_validation_matches(smc)
        || !platform_policy_allows(smc, message, message_size)) {
        snprintf(message, message_size,
            "Platform, hardware, thermal state, or telemetry changed during "
            "guard startup; custom control is refused");
        if (guard_started_here) watchdog_clean_stop(watchdog);
        return -1;
    }
    (void)ninefan_hot_policy_observe(
        hot_policy, controller->temperature_valid,
        controller->current_temperature);
    if ((hot_start_requested && *hot_start_requested
            && (hot_start_preflight(
                    smc, controller, message, message_size) != 0
                || !ninefan_hot_policy_hot_start_available(hot_policy)))
        || ((!hot_start_requested || !*hot_start_requested)
            && !ninefan_hot_policy_allows_manual(hot_policy))) {
        if (!hot_start_requested || !*hot_start_requested) {
            snprintf(message, message_size,
                "Hotspot crossed %.0f C during guard startup; "
                "Apple control remains active",
                NINEFAN_APPLE_HANDOFF_C);
        }
        if (guard_started_here) watchdog_clean_stop(watchdog);
        return -1;
    }
    ninefan_controller_select(controller, curve);
    snprintf(message, message_size, "%s curve selected", curve->name);
    return 0;
}

static int select_default(
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    ninefan_watchdog *watchdog,
    ninefan_smc *smc,
    char *message, size_t message_size) {
    if (geteuid() != 0) return -1;
    controller->curve = NULL;
    const int result = restore_controller(
        smc, controller, response_monitor, message, message_size);
    if (result == 0) watchdog_clean_stop(watchdog);
    else watchdog_trigger_restore(watchdog);
    return result;
}

static const ninefan_curve *command_curve(uint16_t command) {
    if (command < NINEFAN_COMMAND_QUIET
        || command > NINEFAN_COMMAND_MAXIMUM) {
        return NULL;
    }
    const size_t index = command - NINEFAN_COMMAND_QUIET;
    return index < ninefan_curve_count ? &ninefan_curves[index] : NULL;
}

static int restore_session(
    ninefan_smc *smc,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    ninefan_watchdog *watchdog,
    char *message,
    size_t message_size) {
    controller->curve = NULL;
    if (ninefan_smc_restore_default(smc) != 0) {
        snprintf(message, message_size, "Default restore failed: %s",
            ninefan_smc_error(smc));
        watchdog_trigger_restore(watchdog);
        return -1;
    }
    ninefan_controller_reset_runtime(controller);
    ninefan_response_monitor_init(response_monitor);
    watchdog_clean_stop(watchdog);
    return 0;
}

static int select_session_curve(
    uint16_t command,
    ninefan_controller *controller,
    ninefan_response_monitor *response_monitor,
    ninefan_watchdog *watchdog,
    ninefan_smc *smc,
    ninefan_lease *lease,
    ninefan_hot_policy *hot_policy,
    int *hot_start_active,
    uint64_t *hot_start_deadline_ns,
    char *message,
    size_t message_size,
    int control_lock_fd) {
    const ninefan_curve *curve = command_curve(command);
    if (!curve) {
        snprintf(message, message_size, "Invalid fixed curve command");
        return -1;
    }
    const int control_was_active =
        watchdog->active || controller->manual_active || controller->curve;
    int hot_start_requested = 0;
    if (select_curve(
            curve, controller, watchdog, smc, lease, hot_policy,
            &hot_start_requested,
            message, message_size, control_lock_fd) != 0) {
        if (control_was_active) {
            (void)restore_session(
                smc, controller, response_monitor, watchdog,
                message, message_size);
            return -2;
        }
        return -1;
    }
    if (command == NINEFAN_COMMAND_MAXIMUM) {
        const uint64_t now_ns = ninefan_continuous_time_ns();
        if (ninefan_lease_shorten(
                lease, NINEFAN_MAX_CURVE_LEASE_MAX_MS, now_ns) != 0
            || watchdog_limit_maximum(watchdog) != 0) {
            snprintf(message, message_size,
                "Could not enforce the shorter maximum-speed lease");
            (void)restore_session(
                smc, controller, response_monitor, watchdog,
                message, message_size);
            return -2;
        }
    }
    if (hot_start_requested) {
        const uint64_t now_ns = ninefan_continuous_time_ns();
        const uint64_t duration_ns =
            NINEFAN_HOT_START_CONTROL_MS * 1000000ULL;
        if (!hot_start_active
            || !hot_start_deadline_ns
            || now_ns == 0
            || now_ns > UINT64_MAX - duration_ns
            || watchdog_limit_hot_start(watchdog) != 0
            || !ninefan_hot_policy_consume_hot_start(hot_policy)) {
            snprintf(message, message_size,
                "Could not enforce the independent hot-start safety limit");
            (void)restore_session(
                smc, controller, response_monitor, watchdog,
                message, message_size);
            return -2;
        }
        *hot_start_active = 1;
        *hot_start_deadline_ns = now_ns + duration_ns;
        snprintf(message, message_size,
            "Guarded hot-start maximum active for at most %llu seconds",
            (unsigned long long)(NINEFAN_HOT_START_CONTROL_MS / 1000ULL));
    } else if (hot_start_active && hot_start_deadline_ns) {
        *hot_start_active = 0;
        *hot_start_deadline_ns = 0;
    }
    return 0;
}

static int run_session(
    ninefan_smc *smc,
    uint16_t initial_command,
    uint64_t duration_ms,
    int control_lock_fd) {
    ninefan_watchdog watchdog = {0};
    ninefan_controller controller;
    ninefan_response_monitor response_monitor;
    ninefan_hot_policy hot_policy;
    ninefan_controller_init(&controller);
    ninefan_response_monitor_init(&response_monitor);
    ninefan_hot_policy_init(&hot_policy);
    int hot_start_active = 0;
    uint64_t hot_start_deadline_ns = 0;
    char message[256] = {0};
    ninefan_lease lease;
    const uint64_t maximum_ms =
        initial_command == NINEFAN_COMMAND_MAXIMUM
            ? NINEFAN_MAX_CURVE_LEASE_MAX_MS
            : NINEFAN_CURVE_LEASE_MAX_MS;
    if (ninefan_lease_start(
            &lease, duration_ms, maximum_ms,
            ninefan_continuous_time_ns()) != 0) {
        (void)send_event(
            NINEFAN_EVENT_EXIT, 1, NINEFAN_EXIT_ERROR,
            smc, &controller, NULL, "Invalid safety lease");
        return 1;
    }
    if (initial_command != NINEFAN_COMMAND_DEFAULT
        && select_session_curve(
               initial_command, &controller, &response_monitor,
               &watchdog, smc, &lease, &hot_policy,
               &hot_start_active, &hot_start_deadline_ns,
               message, sizeof(message),
               control_lock_fd) != 0) {
        (void)send_event(
            NINEFAN_EVENT_EXIT, 1, NINEFAN_EXIT_ERROR,
            smc, &controller, &lease, message);
        return 1;
    }

    long long next_sample = 0;
    int exit_code = 0;
    int channel_usable = 1;
    ninefan_exit_reason exit_reason = NINEFAN_EXIT_ERROR;

    while (!termination_requested) {
        if (hot_start_active) {
            const uint64_t hot_now_ns = ninefan_continuous_time_ns();
            if (hot_now_ns == 0
                || hot_now_ns >= hot_start_deadline_ns) {
                snprintf(message, sizeof(message),
                    "Hot-start maximum completed; Apple control is active");
                if (restore_session(
                        smc, &controller, &response_monitor, &watchdog,
                        message, sizeof(message)) != 0) {
                    exit_code = 1;
                    break;
                }
                hot_start_active = 0;
                hot_start_deadline_ns = 0;
                next_sample = 0;
                continue;
            }
        }
        const ninefan_lease_result lease_result =
            ninefan_lease_check(
                &lease, ninefan_continuous_time_ns(),
                NINEFAN_SUSPEND_GAP_MS);
        if (lease_result != NINEFAN_LEASE_OK) {
            snprintf(message, sizeof(message), "%s",
                lease_result == NINEFAN_LEASE_EXPIRED
                    ? "Safety lease expired; restoring Apple control"
                    : lease_result == NINEFAN_LEASE_SUSPEND_GAP
                        ? "Sleep or scheduling gap detected; restoring Apple control"
                        : "Safety lease became invalid; restoring Apple control");
            exit_code = lease_result == NINEFAN_LEASE_EXPIRED ? 0 : 1;
            exit_reason = lease_result == NINEFAN_LEASE_EXPIRED
                ? NINEFAN_EXIT_LEASE_EXPIRED
                : NINEFAN_EXIT_ERROR;
            if (restore_session(
                    smc, &controller, &response_monitor, &watchdog,
                    message, sizeof(message)) != 0) {
                exit_code = 1;
                exit_reason = NINEFAN_EXIT_ERROR;
            }
            break;
        }
        if (controller.curve) {
            const ninefan_thermal_state thermal_state =
                ninefan_thermal_state_current();
            if (!ninefan_thermal_state_allows_control(thermal_state)) {
                snprintf(message, sizeof(message),
                    "System thermal state became %s; Apple control is active",
                    ninefan_thermal_state_name(thermal_state));
                if (restore_session(
                    smc, &controller, &response_monitor, &watchdog,
                    message, sizeof(message)) != 0) {
                    exit_code = 1;
                    break;
                }
                hot_start_active = 0;
                hot_start_deadline_ns = 0;
                next_sample = 0;
                continue;
            }
        }
        if (controller.curve
            && watchdog_heartbeat(&watchdog, 0) != 0) {
            snprintf(message, sizeof(message),
                "Safety watchdog stopped; restoring Apple control");
            controller.curve = NULL;
            (void)ninefan_smc_restore_default(smc);
            watchdog_trigger_restore(&watchdog);
            exit_code = 1;
            break;
        }
        const long long now = monotonic_milliseconds();
        if (now >= next_sample) {
            const int snapshot_result =
                refresh_snapshot(smc, &controller, message, sizeof(message));
            const ninefan_hot_result hot_result =
                ninefan_hot_policy_observe(
                    &hot_policy,
                    snapshot_result == 0 && controller.temperature_valid,
                    controller.current_temperature);
            if (controller.curve) {
                if (snapshot_result == 0
                    && verify_fan_response(
                           smc, &controller, &response_monitor,
                           message, sizeof(message)) != 0) {
                    controller.curve = NULL;
                    watchdog_trigger_restore(&watchdog);
                    exit_code = 1;
                    break;
                }
                if (snapshot_result != 0) {
                    controller.curve = NULL;
                    (void)restore_session(
                        smc, &controller, &response_monitor, &watchdog,
                        message, sizeof(message));
                    exit_code = 1;
                    break;
                }
                if ((hot_result == NINEFAN_HOT_HANDOFF
                        || hot_result == NINEFAN_HOT_HANDOFF_LOCKED)
                    && !hot_start_active) {
                    snprintf(message, sizeof(message),
                        "Hotspot reached %.0f C; Apple emergency control is active. "
                        "Press 4 for one guarded hot-start maximum; "
                        "other curves unlock at %.0f C",
                        controller.current_temperature,
                        NINEFAN_MANUAL_REARM_C);
                    if (restore_session(
                            smc, &controller, &response_monitor, &watchdog,
                            message, sizeof(message)) != 0) {
                        exit_code = 1;
                        break;
                    }
                    next_sample = 0;
                    continue;
                }
                if (hot_start_active
                    && hot_result == NINEFAN_HOT_REARMED) {
                    snprintf(message, sizeof(message),
                        "Hotspot cooled to %.0f C; hot-start maximum completed "
                        "and Apple control is active",
                        controller.current_temperature);
                    if (restore_session(
                            smc, &controller, &response_monitor, &watchdog,
                            message, sizeof(message)) != 0) {
                        exit_code = 1;
                        break;
                    }
                    hot_start_active = 0;
                    hot_start_deadline_ns = 0;
                    next_sample = 0;
                    continue;
                }
                const int update_result = update_controller(
                    smc, &controller, &response_monitor, &watchdog,
                    hot_start_active
                        || ninefan_hot_result_forces_maximum(hot_result),
                    message, sizeof(message));
                if (update_result != 0) {
                    controller.curve = NULL;
                    if (update_result == -2) {
                        watchdog_clean_stop(&watchdog);
                    } else {
                        watchdog_trigger_restore(&watchdog);
                    }
                    exit_code = 1;
                    break;
                }
                if (hot_start_active) {
                    const uint64_t message_now_ns =
                        ninefan_continuous_time_ns();
                    const uint64_t remaining_ns =
                        message_now_ns > 0
                            && hot_start_deadline_ns > message_now_ns
                        ? hot_start_deadline_ns
                            - message_now_ns
                        : 0;
                    snprintf(message, sizeof(message),
                        "Guarded hot-start maximum active; %llu seconds remain",
                        (unsigned long long)(
                            (remaining_ns + 999999999ULL) / 1000000000ULL));
                } else if (hot_result == NINEFAN_HOT_MAXIMUM_STARTED) {
                    snprintf(message, sizeof(message),
                        "Hotspot reached %.0f C; pre-handoff maximum cooling active",
                        controller.current_temperature);
                } else if (hot_result == NINEFAN_HOT_MAXIMUM_ENDED) {
                    snprintf(message, sizeof(message),
                        "Hotspot eased to %.0f C; selected curve resumed",
                        controller.current_temperature);
                }
                if (watchdog_heartbeat(&watchdog, 1) != 0) {
                    snprintf(message, sizeof(message),
                        "Safety watchdog stopped; restoring Apple control");
                    controller.curve = NULL;
                    (void)ninefan_smc_restore_default(smc);
                    watchdog_trigger_restore(&watchdog);
                    exit_code = 1;
                    break;
                }
                if (ninefan_smc_refresh_fans(smc) != 0) {
                    snprintf(message, sizeof(message),
                        "Fan telemetry refresh failed after target update; "
                        "surrendering to Apple control");
                    restore_after_control_failure(
                        smc, &controller, &response_monitor,
                        message, sizeof(message));
                    controller.curve = NULL;
                    watchdog_trigger_restore(&watchdog);
                    exit_code = 1;
                    break;
                }
            } else if (hot_result == NINEFAN_HOT_HANDOFF) {
                snprintf(message, sizeof(message),
                    "Hotspot reached %.0f C; Apple emergency control is active. "
                    "Press 4 for one guarded hot-start maximum; "
                    "other curves unlock at %.0f C",
                    controller.current_temperature,
                    NINEFAN_MANUAL_REARM_C);
            } else if (hot_result == NINEFAN_HOT_REARMED) {
                snprintf(message, sizeof(message),
                    "Hotspot cooled to %.0f C; manual curves are available again",
                    controller.current_temperature);
            }
            if (send_event(
                    NINEFAN_EVENT_SNAPSHOT, 0, NINEFAN_EXIT_NONE,
                    smc, &controller,
                    &lease, message) != 0) {
                snprintf(message, sizeof(message),
                    "Frontend disconnected; restoring Apple control");
                channel_usable = 0;
                exit_code = 1;
                break;
            }
            next_sample = now
                + ninefan_hot_policy_sample_interval_ms(
                    controller.temperature_valid,
                    controller.current_temperature);
        }

        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        int poll_result;
        do {
            poll_result = poll(&descriptor, 1, 200);
        } while (poll_result < 0 && errno == EINTR
            && !termination_requested);
        if (poll_result < 0) {
            if (termination_requested) break;
            snprintf(message, sizeof(message),
                "Frontend channel failed; restoring Apple control");
            channel_usable = 0;
            exit_code = 1;
            break;
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN)) {
            ninefan_command command = {0};
            if (ninefan_protocol_read_full(
                    STDIN_FILENO, &command, sizeof(command),
                    NINEFAN_PROTOCOL_IO_TIMEOUT_MS,
                    &termination_requested) != 0
                || !ninefan_protocol_command_valid(&command)) {
                snprintf(message, sizeof(message),
                    "Invalid or closed frontend channel; restoring Apple control");
                channel_usable = 0;
                exit_code = 1;
                break;
            }
            if (command.kind == NINEFAN_COMMAND_QUIT) {
                snprintf(message, sizeof(message),
                    "Session ended; Apple automatic control restored");
                exit_reason = NINEFAN_EXIT_USER_QUIT;
                break;
            }
            if (command.kind == NINEFAN_COMMAND_DEFAULT) {
                if (select_default(
                        &controller, &response_monitor, &watchdog, smc,
                        message, sizeof(message)) != 0) {
                    exit_code = 1;
                    break;
                }
                hot_start_active = 0;
                hot_start_deadline_ns = 0;
                exit_reason = NINEFAN_EXIT_MONITOR_REQUESTED;
                break;
            } else {
                const int selection_result = select_session_curve(
                    command.kind, &controller, &response_monitor,
                    &watchdog, smc, &lease, &hot_policy,
                    &hot_start_active, &hot_start_deadline_ns,
                    message, sizeof(message),
                    control_lock_fd);
                if (selection_result == -2
                    || (selection_result != 0 && !smc->is_open)) {
                    exit_code = 1;
                    break;
                }
                next_sample = 0;
            }
        }
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            snprintf(message, sizeof(message),
                "Frontend disconnected; restoring Apple control");
            channel_usable = 0;
            exit_code = 1;
            break;
        }
    }

    if (watchdog.active || controller.manual_active || controller.curve) {
        if (restore_session(
                smc, &controller, &response_monitor, &watchdog,
                message, sizeof(message)) != 0) {
            exit_code = 1;
            exit_reason = NINEFAN_EXIT_ERROR;
        }
    }
    if (termination_requested) {
        snprintf(message, sizeof(message),
            "Engine terminated; Apple control restored");
        exit_reason = NINEFAN_EXIT_TERMINATED;
    }
    if (channel_usable) {
        (void)send_event(
            NINEFAN_EVENT_EXIT, exit_code, exit_reason,
            smc, &controller, &lease, message);
    }
    return exit_code;
}

static int run_self_test(
    ninefan_smc *smc, int control_lock_fd) {
    if (geteuid() != 0) {
        fprintf(stderr, "9fan-engine: self-test requires root\n");
        return 1;
    }
    char policy_message[192] = {0};
    if (!platform_policy_allows(
            smc, policy_message, sizeof(policy_message))) {
        fprintf(stderr, "9fan: self-test refused: %s\n", policy_message);
        return 1;
    }
    ninefan_lease lease;
    if (ninefan_lease_start(
            &lease, NINEFAN_SELF_TEST_LEASE_MS,
            NINEFAN_CURVE_LEASE_MAX_MS,
            ninefan_continuous_time_ns()) != 0) {
        fprintf(stderr, "9fan: self-test safety lease could not start\n");
        return 1;
    }

    const ninefan_thermal_state initial_thermal_state =
        ninefan_thermal_state_current();
    if (!ninefan_thermal_state_allows_control(initial_thermal_state)) {
        fprintf(stderr,
            "9fan: self-test refused because system thermal state is %s\n",
            ninefan_thermal_state_name(initial_thermal_state));
        return 1;
    }

    float hotspot = NAN;
    char hotspot_key[5] = "----";
    if (ninefan_smc_hottest_temperature(smc, &hotspot, hotspot_key) != 0
        || !isfinite(hotspot) || hotspot > 80.0f) {
        fprintf(stderr,
            "9fan: self-test refused because the hotspot is unavailable or above 80 C\n");
        return 1;
    }
    if (smc->temperature_keys_saturated) {
        fprintf(stderr,
            "9fan: self-test refused because temperature discovery is incomplete "
            "or saturated\n");
        return 1;
    }
    if (ninefan_smc_refresh_fans(smc) != 0) {
        fprintf(stderr, "9fan: self-test refused because fan telemetry is incomplete: %s\n",
            ninefan_smc_error(smc));
        return 1;
    }
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        if (!fan->valid || (fan->mode != 0 && fan->mode != 3)) {
            fprintf(stderr,
                "9fan: self-test refused because fan %d is not under Apple control; "
                "stop other fan controllers and restore Apple control first\n",
                index);
            return 1;
        }
    }
    invalidate_hardware_validation();

    ninefan_watchdog watchdog = {0};
    char message[256] = {0};
    const uint64_t lease_remaining_ms = ninefan_lease_remaining_ms(
        &lease, ninefan_continuous_time_ns());
    if (watchdog_start(
            &watchdog, smc, control_lock_fd, lease_remaining_ms,
            message, sizeof(message)) != 0) {
        fprintf(stderr, "9fan: %s\n", message);
        return 1;
    }
    hotspot = NAN;
    memcpy(hotspot_key, "----", 5);
    const ninefan_thermal_state rechecked_thermal_state =
        ninefan_thermal_state_current();
    if (!ninefan_thermal_state_allows_control(rechecked_thermal_state)
        || ninefan_smc_hottest_temperature(smc, &hotspot, hotspot_key) != 0
        || !isfinite(hotspot) || hotspot > 80.0f
        || smc->temperature_keys_saturated
        || ninefan_smc_refresh_fans(smc) != 0
        || !platform_policy_allows(
               smc, policy_message, sizeof(policy_message))
        || ninefan_lease_check(
               &lease, ninefan_continuous_time_ns(),
               NINEFAN_SUSPEND_GAP_MS) != NINEFAN_LEASE_OK) {
        fprintf(stderr,
            "9fan: self-test preflight changed after the watchdog reconnect; "
            "the test is refused\n");
        watchdog_clean_stop(&watchdog);
        return 1;
    }
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        if (!fan->valid || (fan->mode != 0 && fan->mode != 3)) {
            fprintf(stderr,
                "9fan: fan %d left Apple control during self-test startup; "
                "the test is refused\n",
                index);
            watchdog_clean_stop(&watchdog);
            return 1;
        }
    }

    int result = 0;
    for (int index = 0; index < smc->fan_count; index++) {
        if (ninefan_lease_check(
                &lease, ninefan_continuous_time_ns(),
                NINEFAN_SUSPEND_GAP_MS) != NINEFAN_LEASE_OK
            || watchdog_arm(&watchdog) != 0
            || ninefan_smc_enable_manual(
                smc, index, &termination_requested,
                watchdog_progress, &watchdog) != 0
            || ninefan_smc_set_target(smc, index, smc->fans[index].maximum_rpm) != 0) {
            fprintf(stderr, "9fan: fan %d control failed: %s\n",
                index, ninefan_smc_error(smc));
            result = 1;
            break;
        }
        if (watchdog_heartbeat(&watchdog, 1) != 0) {
            fprintf(stderr, "9fan: safety watchdog stopped unexpectedly\n");
            result = 1;
            break;
        }
    }
    if (result == 0 && ninefan_smc_refresh_fans(smc) == 0) {
        for (int index = 0; index < smc->fan_count; index++) {
            const ninefan_fan *fan = &smc->fans[index];
            const int accepted =
                fan->mode == 1 && fabsf(fan->target_rpm - fan->maximum_rpm) <= 100.0f;
            printf("Fan %d target: mode=%s, target=%.0f, maximum=%.0f: %s\n",
                index, mode_name(fan->mode), fan->target_rpm, fan->maximum_rpm,
                accepted ? "accepted" : "FAILED");
            if (!accepted) result = 1;
        }
    } else if (result == 0) {
        fprintf(stderr, "9fan: verification read failed: %s\n", ninefan_smc_error(smc));
        result = 1;
    }

    int fans_responded[NINEFAN_MAX_FANS] = {0};
    unsigned int response_streak[NINEFAN_MAX_FANS] = {0};
    if (result == 0) {
        for (int attempt = 0; attempt < 40 && !termination_requested; attempt++) {
            if (ninefan_lease_check(
                    &lease, ninefan_continuous_time_ns(),
                    NINEFAN_SUSPEND_GAP_MS) != NINEFAN_LEASE_OK) {
                fprintf(stderr,
                    "9fan: self-test lease expired or a sleep gap occurred\n");
                result = 1;
                break;
            }
            if (ninefan_smc_refresh_fans(smc) != 0) {
                fprintf(stderr, "9fan: fan-response read failed: %s\n",
                    ninefan_smc_error(smc));
                result = 1;
                break;
            }
            int all_responded = 1;
            for (int index = 0; index < smc->fan_count; index++) {
                const ninefan_fan *fan = &smc->fans[index];
                const float response_floor = fan->minimum_rpm
                    + 0.50f * (fan->maximum_rpm - fan->minimum_rpm);
                if (fan->valid && fan->actual_rpm >= response_floor) {
                    response_streak[index]++;
                } else {
                    response_streak[index] = 0;
                }
                fans_responded[index] = response_streak[index] >= 3;
                if (!fans_responded[index]) all_responded = 0;
            }
            if (all_responded) break;
            if (watchdog_heartbeat(&watchdog, 1) != 0) {
                fprintf(stderr, "9fan: safety watchdog stopped unexpectedly\n");
                result = 1;
                break;
            }
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000L};
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR
                && !termination_requested) {}
        }
        if (termination_requested) result = 1;
        for (int index = 0; index < smc->fan_count; index++) {
            printf("Fan %d response: actual=%.0f RPM: %s\n",
                index, smc->fans[index].actual_rpm,
                fans_responded[index] ? "accepted" : "FAILED");
            if (!fans_responded[index]) result = 1;
        }
    }

    if (ninefan_smc_restore_default(smc) != 0) {
        fprintf(stderr, "9fan: default restore failed: %s\n", ninefan_smc_error(smc));
        result = 1;
        watchdog_trigger_restore(&watchdog);
    } else {
        watchdog_clean_stop(&watchdog);
        if (result == 0 && write_hardware_validation(smc) != 0) {
            fprintf(stderr,
                "9fan: hardware passed, but the root-owned validation marker "
                "could not be saved\n");
            result = 1;
        }
    }
    if (result == 0) {
        puts("Self-test passed; Apple control restored and this hardware/OS build validated.");
    }
    return result;
}

static int parse_unsigned(
    const char *text, uint64_t maximum, uint64_t *value_out) {
    if (!text || !text[0] || !value_out || maximum == 0) return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0
        || !end
        || *end != '\0'
        || value == 0
        || value > maximum) {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int parse_command(const char *text, uint16_t *command_out) {
    if (!text || !text[0] || !command_out) return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0
        || !end
        || *end != '\0'
        || value > NINEFAN_COMMAND_MAXIMUM) {
        return -1;
    }
    *command_out = (uint16_t)value;
    return 0;
}

static int sudo_invoking_uid(uid_t *uid_out) {
    if (!uid_out) return -1;
    const char *text = getenv("SUDO_UID");
    if (!text || !text[0]) return -1;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor;
         cursor++) {
        if (*cursor < '0' || *cursor > '9') return -1;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    const uid_t uid = (uid_t)value;
    if (errno != 0
        || !end
        || *end != '\0'
        || value == 0
        || (unsigned long)uid != value) {
        return -1;
    }
    *uid_out = uid;
    return 0;
}

static int configure_protocol_descriptor(int fd) {
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);
    return status_flags >= 0
        && descriptor_flags >= 0
        && fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0
        && fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0
        ? 0
        : -1;
}

static int install_protocol_channel(int channel_fd) {
    if (channel_fd < 0
        || dup2(channel_fd, STDIN_FILENO) < 0
        || dup2(channel_fd, STDOUT_FILENO) < 0) {
        return -1;
    }
    if (channel_fd != STDIN_FILENO && channel_fd != STDOUT_FILENO) {
        close(channel_fd);
    }
    return configure_protocol_descriptor(STDIN_FILENO) == 0
        && configure_protocol_descriptor(STDOUT_FILENO) == 0
        ? 0
        : -1;
}

static int protocol_descriptors_are_socket(void) {
    struct stat input = {0}, output = {0};
    const int input_flags = fcntl(STDIN_FILENO, F_GETFL);
    const int output_flags = fcntl(STDOUT_FILENO, F_GETFL);
    const int input_descriptor_flags = fcntl(STDIN_FILENO, F_GETFD);
    const int output_descriptor_flags = fcntl(STDOUT_FILENO, F_GETFD);
    int input_type = 0, output_type = 0;
    socklen_t input_type_size = sizeof(input_type);
    socklen_t output_type_size = sizeof(output_type);
    return fstat(STDIN_FILENO, &input) == 0
        && fstat(STDOUT_FILENO, &output) == 0
        && S_ISSOCK(input.st_mode)
        && S_ISSOCK(output.st_mode)
        && input.st_dev == output.st_dev
        && input.st_ino == output.st_ino
        && input_flags >= 0
        && output_flags >= 0
        && input_descriptor_flags >= 0
        && output_descriptor_flags >= 0
        && (input_flags & O_NONBLOCK) != 0
        && (output_flags & O_NONBLOCK) != 0
        && (input_descriptor_flags & FD_CLOEXEC) != 0
        && (output_descriptor_flags & FD_CLOEXEC) != 0
        && getsockopt(
            STDIN_FILENO, SOL_SOCKET, SO_TYPE,
            &input_type, &input_type_size) == 0
        && getsockopt(
            STDOUT_FILENO, SOL_SOCKET, SO_TYPE,
            &output_type, &output_type_size) == 0
        && input_type_size == sizeof(input_type)
        && output_type_size == sizeof(output_type)
        && input_type == SOCK_STREAM
        && output_type == SOCK_STREAM;
}

int main(int argc, char **argv) {
    if (ninefan_signal_guard_install(
            request_termination, fatal_signal) != 0) {
        fprintf(stderr,
            "9fan-engine: could not install safety signal handlers\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "9fan-engine: privileged engine requires root\n");
        return 2;
    }
    char trusted_directory[PATH_MAX] = {0};
    char trust_message[192] = {0};
    if (trusted_engine_directory(
            trusted_directory, trust_message, sizeof(trust_message)) != 0) {
        fprintf(stderr, "9fan-engine: %s\n", trust_message);
        return 1;
    }

    for (size_t index = 0; index < ninefan_curve_count; index++) {
        if (!ninefan_curve_is_valid(&ninefan_curves[index])) {
            fprintf(stderr, "9fan-engine: built-in curve '%s' is invalid\n",
                ninefan_curves[index].name);
            return 1;
        }
    }

    const int is_default =
        argc == 2 && strcmp(argv[1], "--default") == 0;
    const int is_session =
        argc == 7
        && strcmp(argv[1], "--session") == 0
        && strcmp(argv[3], "--lease-ms") == 0
        && strcmp(argv[5], "--channel") == 0;
    const int is_self_test =
        argc == 5
        && strcmp(argv[1], "--self-test") == 0
        && strcmp(argv[2], "--yes") == 0
        && strcmp(argv[3], "--lease-ms") == 0;
    if (!is_default && !is_session && !is_self_test) {
        fprintf(stderr,
            "9fan-engine: internal fixed-protocol invocation required\n");
        return 2;
    }
    if (is_session || is_self_test) {
        char guard_path[PATH_MAX] = {0};
        if (trusted_guard_path(
                guard_path, trust_message, sizeof(trust_message)) != 0) {
            fprintf(stderr, "9fan-engine: %s\n", trust_message);
            return 1;
        }
    }

    uint64_t duration_ms = 0;
    uint16_t initial_command = NINEFAN_COMMAND_DEFAULT;
    if (is_session) {
        if (parse_command(argv[2], &initial_command) != 0
            || parse_unsigned(
                argv[4], NINEFAN_CURVE_LEASE_MAX_MS,
                &duration_ms) != 0
            || (initial_command == NINEFAN_COMMAND_MAXIMUM
                && duration_ms > NINEFAN_MAX_CURVE_LEASE_MAX_MS)) {
            fprintf(stderr,
                "9fan-engine: invalid session command or lease\n");
            return 2;
        }
    } else if (is_self_test
        && (parse_unsigned(
                 argv[4], NINEFAN_SELF_TEST_LEASE_MS,
                 &duration_ms) != 0
            || duration_ms != NINEFAN_SELF_TEST_LEASE_MS)) {
        fprintf(stderr, "9fan-engine: invalid self-test lease\n");
        return 2;
    }

    if (is_session) {
        uid_t invoking_uid = (uid_t)-1;
        if (sudo_invoking_uid(&invoking_uid) != 0) {
            fprintf(stderr,
                "9fan-engine: could not identify the invoking sudo user\n");
            return 2;
        }
        const int channel_fd = ninefan_channel_connect(
            argv[6], invoking_uid, NINEFAN_PROTOCOL_IO_TIMEOUT_MS,
            &termination_requested);
        if (channel_fd < 0) {
            fprintf(stderr,
                "9fan-engine: private protocol channel is invalid\n");
            return 2;
        }
        if (install_protocol_channel(channel_fd) != 0
            || !protocol_descriptors_are_socket()) {
            fprintf(stderr,
                "9fan-engine: private protocol channel is invalid\n");
            return 2;
        }
    }

    int control_lock = -1;
    if (is_session || is_self_test) {
        control_lock = acquire_control_lock();
        if (control_lock < 0) return 1;
    }

    ninefan_smc smc;
    const int open_result = is_default
        ? ninefan_smc_open_recovery(&smc)
        : ninefan_smc_open(&smc);
    if (open_result != 0) {
        fprintf(stderr, "9fan: %s\n", ninefan_smc_error(&smc));
        if (control_lock >= 0) close(control_lock);
        return 1;
    }

    int result;
    if (is_default) {
        if (ninefan_smc_restore_default(&smc) != 0) {
            fprintf(stderr, "9fan: default restore failed: %s\n", ninefan_smc_error(&smc));
            result = 1;
        } else {
            puts("Apple automatic fan control restored.");
            result = 0;
        }
    } else if (is_self_test) {
        result = run_self_test(&smc, control_lock);
    } else {
        result = run_session(
            &smc, initial_command, duration_ms, control_lock);
    }

    ninefan_smc_close(&smc);
    if (control_lock >= 0) close(control_lock);
    return result;
}
