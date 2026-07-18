#include "curve.h"
#include "controller.h"
#include "response_monitor.h"
#include "signal_guard.h"
#include "smc.h"
#include "thermal_guard.h"

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
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NINEFAN_VERSION "1.3.1"
#define SAMPLE_INTERVAL_MS 2000
#define WATCHDOG_HEARTBEAT_INTERVAL_MS 2000
#define WATCHDOG_HEARTBEAT_BYTE 'H'
#define WATCHDOG_CLEAN_BYTE 'C'
#define WATCHDOG_READY_BYTE 'R'
#define WATCHDOG_READY_TIMEOUT_MS 2000
#define APPLE_HANDOFF_TEMPERATURE_C 90.0f
#define HARDWARE_VALIDATION_PATH "/var/db/9fan.validation"
#define HARDWARE_VALIDATION_RECORD_SIZE 512

static volatile sig_atomic_t termination_requested;

typedef struct {
    struct termios original;
    int active;
} ninefan_terminal;

static ninefan_terminal *active_terminal;

typedef struct {
    int active;
    int write_fd;
    pid_t pid;
    long long next_heartbeat_ms;
} ninefan_watchdog;

static int watchdog_heartbeat(ninefan_watchdog *watchdog, int force);

static int watchdog_progress(void *context) {
    return watchdog_heartbeat((ninefan_watchdog *)context, 1);
}

static void request_termination(int signal_number) {
    (void)signal_number;
    termination_requested = 1;
}

static void emergency_terminal_restore(void) {
    if (!active_terminal || !active_terminal->active) return;
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &active_terminal->original);
    static const char sequence[] = "\033[?25h\033[?1049l";
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    active_terminal->active = 0;
    active_terminal = NULL;
}

static void fatal_signal(int signal_number) {
    emergency_terminal_restore();
    signal(signal_number, SIG_DFL);
    (void)kill(getpid(), signal_number);
    _exit(128 + signal_number);
}

static long long monotonic_milliseconds(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int trusted_guard_path(
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
    char *separator = strrchr(resolved, '/');
    if (!separator || separator == resolved) {
        snprintf(message, message_size, "9fan has no trusted executable directory");
        return -1;
    }
    *separator = '\0';

    struct stat directory = {0};
    if (lstat(resolved, &directory) != 0
        || !S_ISDIR(directory.st_mode)
        || directory.st_uid != 0
        || (directory.st_mode & 0777) != 0755) {
        snprintf(message, message_size,
            "9fan executable directory is not safely root-owned");
        return -1;
    }

    const int count =
        snprintf(output, PATH_MAX, "%s/9fan-guard", resolved);
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

static void terminal_enter(ninefan_terminal *terminal) {
    memset(terminal, 0, sizeof(*terminal));
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &terminal->original) != 0) return;
    struct termios raw = terminal->original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
    terminal->active = 1;
    active_terminal = terminal;
    printf("\033[?1049h\033[?25l\033[2J");
    fflush(stdout);
}

static void terminal_leave(ninefan_terminal *terminal) {
    if (!terminal || !terminal->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal->original);
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
    terminal->active = 0;
    if (active_terminal == terminal) active_terminal = NULL;
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

static int any_fan_manual(const ninefan_smc *smc) {
    if (!smc) return 0;
    for (int index = 0; index < smc->fan_count; index++) {
        if (smc->fans[index].valid && smc->fans[index].mode == 1) return 1;
    }
    return 0;
}

static void print_curves(void) {
    puts("9fan curves (percentages are within each fan's SMC min/max range):");
    puts("  default      Apple's thermal controller; 0 RPM remains available");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        printf("  %-12s %s\n", ninefan_curves[index].slug, ninefan_curves[index].summary);
    }
}

static void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "  9fan                         Interactive monitor and curve selector\n"
        "  sudo /usr/local/bin/9fan quiet|balanced|performance|max\n"
        "  sudo /usr/local/bin/9fan default\n"
        "  9fan status                 Read-only hardware and fan status\n"
        "  sudo /usr/local/bin/9fan self-test\n"
        "  sudo /usr/local/bin/9fan self-test --yes\n"
        "  9fan curves                 Print the built-in curves\n"
        "  9fan --version\n");
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

    if (!needs_manual) {
        if (controller->manual_active) {
            return restore_controller(
                smc, controller, response_monitor, message, message_size);
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
    if (!watchdog || !watchdog->active) return 0;
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

static void wait_for_watchdog(pid_t pid) {
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        /* Keep the child accounted for when a termination signal interrupts waitpid. */
    }
}

static int watchdog_start(
    ninefan_watchdog *watchdog, ninefan_smc *smc, int control_lock_fd,
    char *message, size_t message_size) {
    if (watchdog->active) return 0;

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
        active_terminal = NULL;
        close(heartbeat_descriptors[1]);
        close(ready_descriptors[0]);
        if (control_lock_fd >= 0) close(control_lock_fd);
        if (fcntl(heartbeat_descriptors[0], F_SETFD, 0) != 0
            || fcntl(ready_descriptors[1], F_SETFD, 0) != 0) {
            _exit(127);
        }
        char heartbeat_fd[24], ready_fd[24];
        snprintf(heartbeat_fd, sizeof(heartbeat_fd), "%d", heartbeat_descriptors[0]);
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready_descriptors[1]);
        execl(guard_path, guard_path,
            "--watch-fd", heartbeat_fd,
            "--ready-fd", ready_fd,
            "--protocol", "1",
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

static void render_ui(
    const ninefan_smc *smc, const ninefan_controller *controller, const char *message,
    int privileged) {
    char model[64], chip[64];
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    if (!model[0]) snprintf(model, sizeof(model), "Apple Silicon Mac");
    if (!chip[0]) snprintf(chip, sizeof(chip), "Apple Silicon");

    printf("\033[H\033[2J");
    printf("\033[30;46;1m 9fan %-8s  %-18s  %-20s \033[0m\r\n",
        NINEFAN_VERSION, model, chip);
    const ninefan_thermal_state thermal_state =
        ninefan_thermal_state_current();
    printf("\r\n  System    thermal state %-8s",
        ninefan_thermal_state_name(thermal_state));
    if (!ninefan_thermal_state_allows_control(thermal_state)) {
        printf("  \033[31;1mAPPLE HANDOFF\033[0m");
    }
    if (controller->temperature_valid) {
        printf("\r\n  Hotspot   \033[1m%5.1f C\033[0m  (%s, hottest CPU/GPU/memory sensor)\r\n",
            controller->current_temperature, controller->hottest_key);
    } else {
        printf("\r\n  Hotspot   \033[31;1mUNAVAILABLE - APPLE HANDOFF\033[0m\r\n");
    }
    printf("  Profile   \033[1m%s\033[0m",
        controller->curve ? controller->curve->name
            : (any_fan_manual(smc) ? "External/unknown manual" : "Apple default"));
    if (controller->curve && !controller->manual_active) {
        printf("  (below %.0f C; Apple remains in control)", controller->curve->activation_c);
    } else if (controller->manual_active && isfinite(controller->requested_fraction)) {
        printf("  (%.0f%% of min-to-max range)", controller->requested_fraction * 100.0f);
    }
    printf("\r\n\r\n");

    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        printf("  Fan %d  %5.0f RPM   target %5.0f   range %4.0f-%4.0f   %-6s\r\n",
            index, fan->actual_rpm, fan->target_rpm, fan->minimum_rpm, fan->maximum_rpm,
            mode_name(fan->mode));
    }

    printf("\r\n  \033[96;1mCURVES\033[0m\r\n");
    printf("  [a] Apple default   system thermal controller, including true 0 RPM\r\n");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        printf("  [%zu] %-13s %s\r\n", index + 1, ninefan_curves[index].name,
            ninefan_curves[index].summary);
    }
    printf("\r\n  [q] Restore Apple default and quit\r\n");
    printf("  Safety: serious thermal state, sensor loss, or 90C -> Apple\r\n");
    printf("          exit/UI crash -> independent Apple-control guard\r\n");
    if (!privileged) {
        printf("\r\n  \033[33;1mRead-only. Install root-owned, then run sudo /usr/local/bin/9fan.\033[0m\r\n");
    }
    if (message && message[0]) printf("\r\n  %s\r\n", message);
    fflush(stdout);
}

static void print_snapshot_line(
    const ninefan_smc *smc, const ninefan_controller *controller) {
    if (controller->temperature_valid) {
        printf("%.1fC %-4s  %-11s", controller->current_temperature,
            controller->hottest_key,
            controller->curve ? controller->curve->slug
                : (any_fan_manual(smc) ? "external" : "default"));
    } else {
        printf("TEMP-FAIL      %-11s",
            controller->curve ? controller->curve->slug
                : (any_fan_manual(smc) ? "external" : "default"));
    }
    for (int index = 0; index < smc->fan_count; index++) {
        printf("  F%d %.0f/%.0f RPM %s", index, smc->fans[index].actual_rpm,
            smc->fans[index].target_rpm, mode_name(smc->fans[index].mode));
    }
    putchar('\n');
    fflush(stdout);
}

static int select_curve(
    const ninefan_curve *curve, ninefan_controller *controller, ninefan_watchdog *watchdog,
    ninefan_smc *smc, char *message, size_t message_size, int privileged,
    int control_lock_fd) {
    if (!privileged) {
        snprintf(message, message_size,
            "Curve selection requires root; use sudo /usr/local/bin/9fan");
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
            "sudo /usr/local/bin/9fan self-test first");
        return -1;
    }
    if (watchdog_start(
            watchdog, smc, control_lock_fd, message, message_size) != 0) {
        return -1;
    }
    if (smc->temperature_keys_saturated
        || !hardware_validation_matches(smc)) {
        snprintf(message, message_size,
            "Hardware or temperature discovery changed during watchdog startup; "
            "custom control is refused");
        watchdog_clean_stop(watchdog);
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
    char *message, size_t message_size, int privileged) {
    if (!privileged) {
        snprintf(message, message_size,
            "Restoring default requires root; run sudo /usr/local/bin/9fan default");
        return -1;
    }
    controller->curve = NULL;
    const int result = restore_controller(
        smc, controller, response_monitor, message, message_size);
    if (result == 0) watchdog_clean_stop(watchdog);
    else watchdog_trigger_restore(watchdog);
    return result;
}

static int run_ui(
    ninefan_smc *smc, const ninefan_curve *initial_curve, int control_lock_fd) {
    const int privileged = geteuid() == 0;
    ninefan_terminal terminal;
    ninefan_watchdog watchdog = {0};
    ninefan_controller controller;
    ninefan_response_monitor response_monitor;
    ninefan_controller_init(&controller);
    ninefan_response_monitor_init(&response_monitor);
    char message[256] = {0};
    const int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    if (initial_curve
        && select_curve(initial_curve, &controller, &watchdog, smc,
               message, sizeof(message), privileged, control_lock_fd) != 0) {
        fprintf(stderr, "9fan: %s\n", message);
        return 1;
    }
    if (!interactive && !initial_curve) {
        refresh_snapshot(smc, &controller, message, sizeof(message));
        print_snapshot_line(smc, &controller);
        return 0;
    }

    terminal_enter(&terminal);
    long long next_sample = 0;
    int exit_code = 0;

    while (!termination_requested) {
        if (controller.curve) {
            const ninefan_thermal_state thermal_state =
                ninefan_thermal_state_current();
            if (!ninefan_thermal_state_allows_control(thermal_state)) {
                snprintf(message, sizeof(message),
                    "System thermal state became %s; surrendering to Apple control",
                    ninefan_thermal_state_name(thermal_state));
                controller.curve = NULL;
                if (ninefan_smc_restore_default(smc) == 0) {
                    ninefan_response_monitor_init(&response_monitor);
                    watchdog_clean_stop(&watchdog);
                } else {
                    watchdog_trigger_restore(&watchdog);
                }
                exit_code = 1;
                break;
            }
        }
        if (watchdog_heartbeat(&watchdog, 0) != 0) {
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
                const int emergency_temperature =
                    controller.temperature_valid
                    && controller.current_temperature
                        >= APPLE_HANDOFF_TEMPERATURE_C;
                if (snapshot_result != 0 || emergency_temperature) {
                    if (emergency_temperature) {
                        snprintf(message, sizeof(message),
                            "Hotspot reached %.0f C; surrendering to Apple control",
                            controller.current_temperature);
                    }
                    controller.curve = NULL;
                    if (ninefan_smc_restore_default(smc) == 0) {
                        ninefan_response_monitor_init(&response_monitor);
                        watchdog_clean_stop(&watchdog);
                    } else {
                        watchdog_trigger_restore(&watchdog);
                    }
                    exit_code = 1;
                    break;
                }
                const int update_result = update_controller(
                    smc, &controller, &response_monitor, &watchdog,
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
            }
            if (interactive) render_ui(smc, &controller, message, privileged);
            else print_snapshot_line(smc, &controller);
            next_sample = now + SAMPLE_INTERVAL_MS;
        }

        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = interactive ? POLLIN : 0,
            .revents = 0,
        };
        const int poll_result = poll(&descriptor, interactive ? 1 : 0, 200);
        if (interactive && poll_result > 0 && (descriptor.revents & POLLIN)) {
            unsigned char key = 0;
            if (read(STDIN_FILENO, &key, 1) != 1) continue;
            if (key == 'q' || key == 3) break;
            if (key == 'a' || key == '0') {
                if (select_default(
                        &controller, &response_monitor, &watchdog, smc,
                        message, sizeof(message), privileged) != 0) {
                    exit_code = 1;
                    break;
                }
                next_sample = 0;
            } else if (key >= '1' && key <= '0' + (int)ninefan_curve_count) {
                const ninefan_curve *curve = &ninefan_curves[key - '1'];
                if (select_curve(curve, &controller, &watchdog, smc,
                        message, sizeof(message), privileged,
                        control_lock_fd) != 0
                    && !smc->is_open) {
                    exit_code = 1;
                    break;
                }
                next_sample = 0;
            }
        }
    }

    if (watchdog.active) {
        if (ninefan_smc_restore_default(smc) != 0) {
            exit_code = 1;
            snprintf(message, sizeof(message), "Default restore failed: %s",
                ninefan_smc_error(smc));
            watchdog_trigger_restore(&watchdog);
        } else {
            watchdog_clean_stop(&watchdog);
        }
    }
    terminal_leave(&terminal);
    if (exit_code != 0 && message[0]) fprintf(stderr, "9fan: %s\n", message);
    return exit_code;
}

static int print_status(ninefan_smc *smc) {
    ninefan_controller controller;
    ninefan_controller_init(&controller);
    char message[256] = {0};
    refresh_snapshot(smc, &controller, message, sizeof(message));

    char model[64], chip[64];
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    printf("9fan %s\n", NINEFAN_VERSION);
    printf("Model:       %s\n", model[0] ? model : "unknown");
    printf("Chip:        %s\n", chip[0] ? chip : "Apple Silicon");
    printf("Privileges:  %s\n", geteuid() == 0 ? "root (control enabled)" : "user (read-only)");
    printf("Mode key:    %s\n", smc->mode_key_format);
    printf("Ftst unlock: %s\n", smc->ftst_available ? "available" : "not present (M5 direct mode)");
    printf("Temp keys:   %zu\n", smc->temperature_key_count);
    printf("Key cap:     %s\n",
        smc->temperature_keys_saturated
            ? "INCOMPLETE/SATURATED (unsafe to control)"
            : "complete");
    char fingerprint[17] = {0};
    const int fingerprint_valid =
        ninefan_smc_validation_fingerprint(
            smc, fingerprint, sizeof(fingerprint)) == 0;
    printf("Schema:      %s\n",
        fingerprint_valid ? fingerprint : "unavailable");
    const ninefan_thermal_state thermal_state =
        ninefan_thermal_state_current();
    printf("Thermal:     %s%s\n",
        ninefan_thermal_state_name(thermal_state),
        ninefan_thermal_state_allows_control(thermal_state)
            ? ""
            : " (Apple handoff required)");
    if (controller.temperature_valid) {
        printf("Hotspot:     %.1f C (%s)\n", controller.current_temperature, controller.hottest_key);
    } else {
        printf("Hotspot:     unavailable\n");
    }
    int fans_valid = 1;
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        if (!fan->valid) fans_valid = 0;
        printf("Fan %d:       %.0f RPM, target %.0f, range %.0f-%.0f, mode %s (%u)\n",
            index, fan->actual_rpm, fan->target_rpm, fan->minimum_rpm,
            fan->maximum_rpm, mode_name(fan->mode), fan->mode);
    }
    if (message[0]) fprintf(stderr, "9fan: %s\n", message);
    return controller.temperature_valid && fans_valid && fingerprint_valid
        && !smc->temperature_keys_saturated ? 0 : 1;
}

static int confirm_self_test(int explicitly_confirmed) {
    if (explicitly_confirmed) return 1;
    if (!isatty(STDIN_FILENO)) return 0;
    printf("This briefly raises every fan to its SMC-reported maximum, verifies "
           "target and fan response, then restores Apple control.\nContinue? [y/N] ");
    fflush(stdout);
    char answer[8] = {0};
    return fgets(answer, sizeof(answer), stdin)
        && (answer[0] == 'y' || answer[0] == 'Y');
}

static int run_self_test(
    ninefan_smc *smc, int explicitly_confirmed, int control_lock_fd) {
    if (geteuid() != 0) {
        fprintf(stderr, "9fan: self-test requires sudo\n");
        return 1;
    }
    if (!confirm_self_test(explicitly_confirmed)) {
        fprintf(stderr, "9fan: self-test cancelled\n");
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "9fan: use 'self-test --yes' for an intentional non-interactive run\n");
        }
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
    if (watchdog_start(
            &watchdog, smc, control_lock_fd, message, sizeof(message)) != 0) {
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
        || ninefan_smc_refresh_fans(smc) != 0) {
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
        if (ninefan_smc_enable_manual(
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
            printf("Fan %d target: mode=%s, target=%.0f, maximum=%.0f — %s\n",
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
            printf("Fan %d response: actual=%.0f RPM — %s\n",
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

int main(int argc, char **argv) {
    if (ninefan_signal_guard_install(
            request_termination, fatal_signal) != 0) {
        fprintf(stderr, "9fan: could not install safety signal handlers\n");
        return 1;
    }
    atexit(emergency_terminal_restore);

    if (argc > 3) {
        print_usage(stderr);
        return 2;
    }
    const int self_test_confirmed =
        argc == 3 && strcmp(argv[1], "self-test") == 0 && strcmp(argv[2], "--yes") == 0;
    if (argc == 3 && !self_test_confirmed) {
        print_usage(stderr);
        return 2;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        puts("9fan " NINEFAN_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "curves") == 0) {
        print_curves();
        return 0;
    }

    for (size_t index = 0; index < ninefan_curve_count; index++) {
        if (!ninefan_curve_is_valid(&ninefan_curves[index])) {
            fprintf(stderr, "9fan: built-in curve '%s' is invalid\n",
                ninefan_curves[index].name);
            return 1;
        }
    }

    const ninefan_curve *initial_curve = NULL;
    if (argc == 2 && strcmp(argv[1], "default") != 0 && strcmp(argv[1], "status") != 0
        && strcmp(argv[1], "self-test") != 0) {
        initial_curve = ninefan_curve_named(argv[1]);
        if (!initial_curve) {
            fprintf(stderr, "9fan: unknown curve '%s'\n", argv[1]);
            print_curves();
            return 2;
        }
    }

    int control_lock = -1;
    const int is_self_test =
        argc >= 2 && strcmp(argv[1], "self-test") == 0;
    const int may_control = geteuid() == 0
        && (argc == 1 || initial_curve != NULL || is_self_test);
    if (may_control) {
        control_lock = acquire_control_lock();
        if (control_lock < 0) return 1;
    }

    const int is_default = argc == 2 && strcmp(argv[1], "default") == 0;
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
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        result = print_status(&smc);
    } else if (argc == 2 && strcmp(argv[1], "default") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "9fan: restoring default requires sudo\n");
            result = 1;
        } else if (ninefan_smc_restore_default(&smc) != 0) {
            fprintf(stderr, "9fan: default restore failed: %s\n", ninefan_smc_error(&smc));
            result = 1;
        } else {
            puts("Apple automatic fan control restored.");
            result = 0;
        }
    } else if (argc >= 2 && strcmp(argv[1], "self-test") == 0) {
        result = run_self_test(&smc, self_test_confirmed, control_lock);
    } else {
        result = run_ui(&smc, initial_curve, control_lock);
    }

    ninefan_smc_close(&smc);
    if (control_lock >= 0) close(control_lock);
    return result;
}
