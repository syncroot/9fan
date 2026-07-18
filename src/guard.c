#include "smc_codec.h"
#include "lease.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GUARD_HEARTBEAT_BYTE 'H'
#define GUARD_CLEAN_BYTE 'C'
#define GUARD_ARM_BYTE 'A'
#define GUARD_MAXIMUM_BYTE 'M'
#define GUARD_READY_BYTE 'R'
#define GUARD_TIMEOUT_MS 6000
#define GUARD_RESTORE_ATTEMPTS 120
#define GUARD_RESTORE_RETRY_NS 500000000L
#define GUARD_MAX_FANS 8

typedef struct {
    uint8_t major, minor, build, reserved;
    uint16_t release;
} guard_smc_version;

typedef struct {
    uint16_t version, length;
    uint32_t cpu_limit, gpu_limit, memory_limit;
} guard_smc_limits;

typedef struct {
    uint32_t size, type;
    uint8_t attributes;
} guard_smc_key_info;

typedef struct {
    uint32_t key;
    guard_smc_version version;
    guard_smc_limits limits;
    guard_smc_key_info info;
    uint8_t result, status, command;
    uint32_t data32;
    uint8_t bytes[32];
} guard_smc_key_data;

typedef struct {
    io_connect_t connection;
    int fan_count;
    char mode_key_format[6];
    int ftst_available;
} guard_smc;

_Static_assert(
    sizeof(guard_smc_key_data) == 80,
    "AppleSMC parameter structure must be 80 bytes");

enum {
    GUARD_SMC_SELECTOR = 2,
    GUARD_SMC_READ_BYTES = 5,
    GUARD_SMC_WRITE_BYTES = 6,
    GUARD_SMC_READ_INFO = 9,
};

static uint32_t fourcc(const char key[4]) {
    return ((uint32_t)(uint8_t)key[0] << 24)
        | ((uint32_t)(uint8_t)key[1] << 16)
        | ((uint32_t)(uint8_t)key[2] << 8)
        | (uint32_t)(uint8_t)key[3];
}

static int smc_call(
    guard_smc *smc,
    const guard_smc_key_data *input,
    guard_smc_key_data *output) {
    size_t output_size = sizeof(*output);
    memset(output, 0, sizeof(*output));
    const kern_return_t result = IOConnectCallStructMethod(
        smc->connection,
        GUARD_SMC_SELECTOR,
        input,
        sizeof(*input),
        output,
        &output_size);
    return result == KERN_SUCCESS
        && output_size == sizeof(*output)
        && output->result == 0
        ? 0
        : -1;
}

static int key_info(
    guard_smc *smc, uint32_t key, guard_smc_key_info *info) {
    guard_smc_key_data input = {0}, output = {0};
    input.key = key;
    input.command = GUARD_SMC_READ_INFO;
    if (smc_call(smc, &input, &output) != 0) return -1;
    *info = output.info;
    return 0;
}

static int read_raw(
    guard_smc *smc,
    const char name[4],
    uint8_t bytes[32],
    guard_smc_key_info *info) {
    guard_smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    if (key_info(smc, input.key, &input.info) != 0
        || input.info.size == 0
        || input.info.size > sizeof(output.bytes)) {
        return -1;
    }
    input.command = GUARD_SMC_READ_BYTES;
    if (smc_call(smc, &input, &output) != 0) return -1;
    memcpy(bytes, output.bytes, sizeof(output.bytes));
    if (info) *info = input.info;
    return 0;
}

static int write_raw(
    guard_smc *smc,
    const char name[4],
    const uint8_t *bytes,
    size_t byte_count) {
    guard_smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    if (!bytes
        || key_info(smc, input.key, &input.info) != 0
        || input.info.size == 0
        || input.info.size > sizeof(input.bytes)
        || input.info.size != byte_count) {
        return -1;
    }
    input.command = GUARD_SMC_WRITE_BYTES;
    memcpy(input.bytes, bytes, byte_count);
    return smc_call(smc, &input, &output);
}

static int read_u8(guard_smc *smc, const char name[4], uint8_t *value) {
    uint8_t bytes[32] = {0};
    guard_smc_key_info info = {0};
    if (!value
        || read_raw(smc, name, bytes, &info) != 0
        || info.size != 1) {
        return -1;
    }
    *value = bytes[0];
    return 0;
}

static int write_u8(guard_smc *smc, const char name[4], uint8_t value) {
    const uint8_t bytes[1] = {value};
    return write_raw(smc, name, bytes, sizeof(bytes));
}

static int read_number(
    guard_smc *smc, const char name[4], float *value) {
    uint8_t bytes[32] = {0};
    guard_smc_key_info info = {0};
    return read_raw(smc, name, bytes, &info) == 0
        ? ninefan_smc_decode_number(info.type, info.size, bytes, value)
        : -1;
}

static int write_number(
    guard_smc *smc, const char name[4], float value) {
    guard_smc_key_info info = {0};
    uint8_t bytes[32] = {0};
    if (!isfinite(value)
        || key_info(smc, fourcc(name), &info) != 0
        || ninefan_smc_encode_number(info.type, info.size, value, bytes) != 0) {
        return -1;
    }
    return write_raw(smc, name, bytes, info.size);
}

static void fan_key(
    char output[5], const char suffix[3], int fan_index) {
    output[0] = 'F';
    output[1] = (char)('0' + fan_index);
    output[2] = suffix[0];
    output[3] = suffix[1];
    output[4] = '\0';
}

static void short_sleep(long nanoseconds) {
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {}
}

static int guard_smc_open(guard_smc *smc) {
    memset(smc, 0, sizeof(*smc));
    io_iterator_t iterator = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSMC");
    if (!matching
        || IOServiceGetMatchingServices(
               kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) {
        return -1;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_name_t name = {0};
        (void)IORegistryEntryGetName(service, name);
        if (strcmp(name, "AppleSMCKeysEndpoint") == 0) {
            const kern_return_t result = IOServiceOpen(
                service, mach_task_self(), 0, &smc->connection);
            IOObjectRelease(service);
            if (result != KERN_SUCCESS) {
                IOObjectRelease(iterator);
                return -1;
            }
            break;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (smc->connection == IO_OBJECT_NULL) return -1;

    uint8_t fan_count = 0;
    if (read_u8(smc, "FNum", &fan_count) != 0
        || fan_count == 0
        || fan_count > GUARD_MAX_FANS) {
        IOServiceClose(smc->connection);
        smc->connection = IO_OBJECT_NULL;
        return -1;
    }
    smc->fan_count = fan_count;

    uint8_t probe = 0;
    if (read_u8(smc, "F0md", &probe) == 0) {
        memcpy(smc->mode_key_format, "F%dmd", 6);
    } else if (read_u8(smc, "F0Md", &probe) == 0) {
        memcpy(smc->mode_key_format, "F%dMd", 6);
    } else {
        IOServiceClose(smc->connection);
        smc->connection = IO_OBJECT_NULL;
        return -1;
    }
    smc->ftst_available = read_u8(smc, "Ftst", &probe) == 0;
    return 0;
}

static void guard_smc_close(guard_smc *smc) {
    if (smc->connection != IO_OBJECT_NULL) {
        IOServiceClose(smc->connection);
    }
    smc->connection = IO_OBJECT_NULL;
}

static int mode_is_system_controlled(
    guard_smc *smc, const char key[5]) {
    uint8_t mode = 0xff;
    return read_u8(smc, key, &mode) == 0
        && (mode == 0 || mode == 3);
}

static void best_effort_maximum_target(
    guard_smc *smc, int fan_index) {
    char maximum_key[5], target_key[5];
    float maximum_rpm = NAN;
    fan_key(maximum_key, "Mx", fan_index);
    fan_key(target_key, "Tg", fan_index);
    if (read_number(smc, maximum_key, &maximum_rpm) == 0
        && isfinite(maximum_rpm)
        && maximum_rpm > 0.0f
        && maximum_rpm <= 20000.0f) {
        (void)write_number(smc, target_key, maximum_rpm);
    }
}

static int guard_restore_default(guard_smc *smc) {
    int failed = 0;
    for (int index = 0; index < smc->fan_count; index++) {
        char mode_key[5];
        fan_key(
            mode_key, &smc->mode_key_format[3], index);
        int automatic = 0;
        for (int attempt = 0; attempt < 5 && !automatic; attempt++) {
            if (write_u8(smc, mode_key, 0) == 0) {
                short_sleep(50000000L);
                automatic = mode_is_system_controlled(smc, mode_key);
            }
        }
        if (!automatic) {
            failed = 1;
            best_effort_maximum_target(smc, index);
        }
    }

    if (smc->ftst_available) {
        uint8_t ftst = 0xff;
        if (write_u8(smc, "Ftst", 0) != 0
            || read_u8(smc, "Ftst", &ftst) != 0
            || ftst != 0) {
            failed = 1;
        }
    }

    for (int index = 0; index < smc->fan_count; index++) {
        char mode_key[5];
        fan_key(
            mode_key, &smc->mode_key_format[3], index);
        if (!mode_is_system_controlled(smc, mode_key)) {
            failed = 1;
            best_effort_maximum_target(smc, index);
        }
    }
    return failed ? -1 : 0;
}

static int parse_fd(const char *text) {
    if (!text || !text[0]) return -1;
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    return errno == 0
        && end
        && *end == '\0'
        && value >= 0
        && value <= INT_MAX
        ? (int)value
        : -1;
}

static int parse_duration_ms(const char *text, uint64_t *value_out) {
    if (!text || !text[0] || !value_out) return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0
        || !end
        || *end != '\0'
        || value == 0
        || value > NINEFAN_CURVE_LEASE_MAX_MS) {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int monitor_parent(int read_fd, ninefan_lease *lease) {
    uint64_t last_heartbeat_ns = ninefan_continuous_time_ns();
    int armed = 0;
    if (last_heartbeat_ns == 0) return 1;
    for (;;) {
        const uint64_t before_poll_ns = ninefan_continuous_time_ns();
        if (ninefan_lease_check(
                lease, before_poll_ns, NINEFAN_SUSPEND_GAP_MS)
                != NINEFAN_LEASE_OK
            || before_poll_ns < last_heartbeat_ns
            || before_poll_ns - last_heartbeat_ns
                >= (uint64_t)GUARD_TIMEOUT_MS * 1000000ULL) {
            return 1;
        }
        struct pollfd descriptor = {
            .fd = read_fd,
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        const int poll_result = poll(&descriptor, 1, 1000);
        if (poll_result < 0 && errno == EINTR) continue;
        if (poll_result < 0) return 1;

        const uint64_t after_poll_ns = ninefan_continuous_time_ns();
        if (ninefan_lease_check(
                lease, after_poll_ns, NINEFAN_SUSPEND_GAP_MS)
                != NINEFAN_LEASE_OK
            || after_poll_ns < last_heartbeat_ns
            || after_poll_ns - last_heartbeat_ns
                >= (uint64_t)GUARD_TIMEOUT_MS * 1000000ULL) {
            return 1;
        }
        if (poll_result == 0) continue;

        char bytes[64];
        const ssize_t count = read(read_fd, bytes, sizeof(bytes));
        if (count <= 0) return 1;
        for (ssize_t index = 0; index < count; index++) {
            if (bytes[index] == GUARD_CLEAN_BYTE) return armed;
            if (bytes[index] == GUARD_ARM_BYTE) {
                armed = 1;
                continue;
            }
            if (bytes[index] == GUARD_MAXIMUM_BYTE) {
                if (ninefan_lease_shorten(
                        lease, NINEFAN_MAX_CURVE_LEASE_MAX_MS,
                        after_poll_ns) != 0) {
                    return 1;
                }
            } else if (bytes[index] != GUARD_HEARTBEAT_BYTE) {
                return 1;
            }
        }
        last_heartbeat_ns = after_poll_ns;
    }
}

static int restore_with_retries(void) {
    for (int attempt = 0; attempt < GUARD_RESTORE_ATTEMPTS; attempt++) {
        guard_smc smc;
        if (guard_smc_open(&smc) == 0) {
            const int result = guard_restore_default(&smc);
            guard_smc_close(&smc);
            if (result == 0) return 0;
        }
        short_sleep(GUARD_RESTORE_RETRY_NS);
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 9
        || strcmp(argv[1], "--watch-fd") != 0
        || strcmp(argv[3], "--ready-fd") != 0
        || strcmp(argv[5], "--protocol") != 0
        || strcmp(argv[6], "2") != 0
        || strcmp(argv[7], "--lease-ms") != 0
        || geteuid() != 0) {
        return 2;
    }
    const int read_fd = parse_fd(argv[2]);
    const int ready_fd = parse_fd(argv[4]);
    uint64_t duration_ms = 0;
    struct stat read_metadata = {0}, ready_metadata = {0};
    if (read_fd < 0
        || ready_fd < 0
        || read_fd == ready_fd
        || parse_duration_ms(argv[8], &duration_ms) != 0
        || fstat(read_fd, &read_metadata) != 0
        || fstat(ready_fd, &ready_metadata) != 0
        || !S_ISFIFO(read_metadata.st_mode)
        || !S_ISFIFO(ready_metadata.st_mode)) {
        return 2;
    }
    ninefan_lease lease;
    if (ninefan_lease_start(
            &lease, duration_ms, NINEFAN_CURVE_LEASE_MAX_MS,
            ninefan_continuous_time_ns()) != 0) {
        return 2;
    }

    (void)setpgid(0, 0);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    const char ready = GUARD_READY_BYTE;
    if (write(ready_fd, &ready, 1) != 1) {
        close(ready_fd);
        close(read_fd);
        return 2;
    }
    close(ready_fd);

    const int restore_required = monitor_parent(read_fd, &lease);
    close(read_fd);
    if (!restore_required) return 0;

    const int result = restore_with_retries();
    if (result != 0) {
        (void)dprintf(
            STDERR_FILENO,
            "9fan-guard: Apple fan-control restoration failed after retries\n");
    }
    return result;
}
