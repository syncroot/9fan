#include "smc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t major, minor, build, reserved;
    uint16_t release;
} smc_version;

typedef struct {
    uint16_t version, length;
    uint32_t cpu_limit, gpu_limit, memory_limit;
} smc_limits;

typedef struct {
    uint32_t size, type;
    uint8_t attributes;
} smc_key_info;

typedef struct {
    uint32_t key;
    smc_version version;
    smc_limits limits;
    smc_key_info info;
    uint8_t result, status, command;
    uint32_t data32;
    uint8_t bytes[32];
} smc_key_data;

_Static_assert(sizeof(smc_key_data) == 80, "AppleSMC parameter structure must be 80 bytes");

enum {
    SMC_SELECTOR = 2,
    SMC_READ_BYTES = 5,
    SMC_WRITE_BYTES = 6,
    SMC_READ_INDEX = 8,
    SMC_READ_INFO = 9,
};

static const uint32_t SMC_FLOAT = 0x666c7420; /* "flt " */
static const uint32_t SMC_FPE2 = 0x66706532;  /* "fpe2" */

static uint32_t fourcc(const char key[4]) {
    return ((uint32_t)(uint8_t)key[0] << 24) | ((uint32_t)(uint8_t)key[1] << 16)
         | ((uint32_t)(uint8_t)key[2] << 8) | (uint32_t)(uint8_t)key[3];
}

static void set_error(ninefan_smc *smc, const char *operation, int code) {
    snprintf(smc->error, sizeof(smc->error), "%s failed (0x%x)", operation, code);
}

static int smc_call(
    ninefan_smc *smc, const smc_key_data *input, smc_key_data *output,
    const char *operation) {
    size_t output_size = sizeof(*output);
    memset(output, 0, sizeof(*output));
    const kern_return_t result = IOConnectCallStructMethod(
        smc->connection, SMC_SELECTOR, input, sizeof(*input), output, &output_size);
    if (result != KERN_SUCCESS) {
        set_error(smc, operation, result);
        return -1;
    }
    if (output->result != 0) {
        set_error(smc, operation, output->result);
        return -1;
    }
    return 0;
}

static int key_info(ninefan_smc *smc, uint32_t key, smc_key_info *info) {
    smc_key_data input = {0}, output = {0};
    input.key = key;
    input.command = SMC_READ_INFO;
    if (smc_call(smc, &input, &output, "SMC key info") != 0) return -1;
    *info = output.info;
    return 0;
}

static int read_raw(
    ninefan_smc *smc, const char name[4], uint8_t bytes[32], smc_key_info *info) {
    smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    if (key_info(smc, input.key, &input.info) != 0) return -1;
    input.command = SMC_READ_BYTES;
    if (smc_call(smc, &input, &output, "SMC read") != 0) return -1;
    memcpy(bytes, output.bytes, sizeof(output.bytes));
    if (info) *info = input.info;
    return 0;
}

static int read_raw_with_info(
    ninefan_smc *smc, const char name[4], smc_key_info info, uint8_t bytes[32]) {
    smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    input.info = info;
    input.command = SMC_READ_BYTES;
    if (smc_call(smc, &input, &output, "SMC read") != 0) return -1;
    memcpy(bytes, output.bytes, sizeof(output.bytes));
    return 0;
}

static int write_raw(ninefan_smc *smc, const char name[4], const uint8_t *bytes) {
    smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    if (key_info(smc, input.key, &input.info) != 0) return -1;
    input.command = SMC_WRITE_BYTES;
    memcpy(input.bytes, bytes, input.info.size > 32 ? 32 : input.info.size);
    return smc_call(smc, &input, &output, "SMC write");
}

static int read_number(ninefan_smc *smc, const char name[4], float *value) {
    uint8_t bytes[32] = {0};
    smc_key_info info = {0};
    if (read_raw(smc, name, bytes, &info) != 0) return -1;
    if (info.size == 4 && info.type == SMC_FLOAT) {
        memcpy(value, bytes, sizeof(*value));
        return isfinite(*value) ? 0 : -1;
    }
    if (info.size == 2 && info.type == SMC_FPE2) {
        const uint16_t raw = ((uint16_t)bytes[0] << 8) | bytes[1];
        *value = (float)raw / 4.0f;
        return 0;
    }
    snprintf(smc->error, sizeof(smc->error), "%.4s has unsupported SMC format", name);
    return -1;
}

static int read_cached_float(
    ninefan_smc *smc, size_t index, float *value) {
    smc_key_info info = {
        .size = smc->temperature_sizes[index],
        .type = smc->temperature_types[index],
        .attributes = smc->temperature_attributes[index],
    };
    uint8_t bytes[32] = {0};
    if (read_raw_with_info(smc, smc->temperature_keys[index], info, bytes) != 0) return -1;
    if (info.size != 4 || info.type != SMC_FLOAT) return -1;
    memcpy(value, bytes, sizeof(*value));
    return isfinite(*value) ? 0 : -1;
}

static int read_u8(ninefan_smc *smc, const char name[4], uint8_t *value) {
    uint8_t bytes[32] = {0};
    smc_key_info info = {0};
    if (read_raw(smc, name, bytes, &info) != 0) return -1;
    if (info.size < 1) {
        snprintf(smc->error, sizeof(smc->error), "%.4s is empty", name);
        return -1;
    }
    *value = bytes[0];
    return 0;
}

static int write_u8(ninefan_smc *smc, const char name[4], uint8_t value) {
    const uint8_t bytes[1] = {value};
    return write_raw(smc, name, bytes);
}

static int write_number(ninefan_smc *smc, const char name[4], float value) {
    smc_key_info info = {0};
    if (key_info(smc, fourcc(name), &info) != 0) return -1;
    uint8_t bytes[32] = {0};
    if (info.size == 4 && info.type == SMC_FLOAT) {
        memcpy(bytes, &value, sizeof(value));
    } else if (info.size == 2 && info.type == SMC_FPE2) {
        const uint16_t raw = (uint16_t)lroundf(value * 4.0f);
        bytes[0] = (uint8_t)(raw >> 8);
        bytes[1] = (uint8_t)(raw & 0xff);
    } else {
        snprintf(smc->error, sizeof(smc->error), "%.4s has unsupported SMC format", name);
        return -1;
    }
    return write_raw(smc, name, bytes);
}

static void fan_key(char output[5], const char *format, int fan_index) {
    snprintf(output, 5, format, fan_index);
}

static int is_temperature_key(const char key[5]) {
    if (key[0] != 'T') return 0;
    switch (key[1]) {
        case 'e':
        case 'f':
        case 'g':
        case 'm':
        case 'p':
        case 's':
            return 1;
        default:
            return 0;
    }
}

static void discover_temperature_keys(ninefan_smc *smc) {
    uint8_t count_bytes[32] = {0};
    smc_key_info count_info = {0};
    if (read_raw(smc, "#KEY", count_bytes, &count_info) != 0 || count_info.size < 4) return;
    const uint32_t count = ((uint32_t)count_bytes[0] << 24)
                         | ((uint32_t)count_bytes[1] << 16)
                         | ((uint32_t)count_bytes[2] << 8)
                         | (uint32_t)count_bytes[3];

    for (uint32_t index = 0;
         index < count && smc->temperature_key_count < NINEFAN_MAX_TEMP_KEYS;
         index++) {
        smc_key_data input = {0}, output = {0};
        input.command = SMC_READ_INDEX;
        input.data32 = index;
        if (smc_call(smc, &input, &output, "SMC enumerate") != 0) continue;
        char key[5] = {
            (char)(output.key >> 24), (char)(output.key >> 16),
            (char)(output.key >> 8), (char)output.key, '\0'
        };
        if (!is_temperature_key(key)) continue;
        smc_key_info info = {0};
        if (key_info(smc, output.key, &info) != 0) continue;
        if (info.size != 4 || info.type != SMC_FLOAT) continue;
        const size_t stored_index = smc->temperature_key_count++;
        memcpy(smc->temperature_keys[stored_index], key, 5);
        smc->temperature_sizes[stored_index] = info.size;
        smc->temperature_types[stored_index] = info.type;
        smc->temperature_attributes[stored_index] = info.attributes;
    }
}

int ninefan_smc_open(ninefan_smc *smc) {
    if (!smc) return -1;
    memset(smc, 0, sizeof(*smc));

    io_iterator_t iterator = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSMC");
    if (!matching
        || IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
               != KERN_SUCCESS) {
        snprintf(smc->error, sizeof(smc->error), "AppleSMC service was not found");
        return -1;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_name_t name = {0};
        IORegistryEntryGetName(service, name);
        if (strcmp(name, "AppleSMCKeysEndpoint") == 0) {
            const kern_return_t result =
                IOServiceOpen(service, mach_task_self(), 0, &smc->connection);
            IOObjectRelease(service);
            if (result != KERN_SUCCESS) {
                IOObjectRelease(iterator);
                set_error(smc, "Open AppleSMC", result);
                return -1;
            }
            break;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (smc->connection == IO_OBJECT_NULL) {
        snprintf(smc->error, sizeof(smc->error), "AppleSMC keys endpoint was not found");
        return -1;
    }
    smc->is_open = 1;

    uint8_t fan_count = 0;
    if (read_u8(smc, "FNum", &fan_count) != 0 || fan_count == 0
        || fan_count > NINEFAN_MAX_FANS) {
        snprintf(smc->error, sizeof(smc->error), "No supported fans were reported by AppleSMC");
        ninefan_smc_close(smc);
        return -1;
    }
    smc->fan_count = fan_count;

    uint8_t probe = 0;
    if (read_u8(smc, "F0md", &probe) == 0) {
        memcpy(smc->mode_key_format, "F%dmd", 6);
    } else if (read_u8(smc, "F0Md", &probe) == 0) {
        memcpy(smc->mode_key_format, "F%dMd", 6);
    } else {
        snprintf(smc->error, sizeof(smc->error), "No supported fan mode key was found");
        ninefan_smc_close(smc);
        return -1;
    }
    smc->ftst_available = read_u8(smc, "Ftst", &probe) == 0;
    discover_temperature_keys(smc);
    return ninefan_smc_refresh_fans(smc);
}

void ninefan_smc_close(ninefan_smc *smc) {
    if (!smc) return;
    if (smc->connection != IO_OBJECT_NULL) IOServiceClose(smc->connection);
    smc->connection = IO_OBJECT_NULL;
    smc->is_open = 0;
}

const char *ninefan_smc_error(const ninefan_smc *smc) {
    return smc && smc->error[0] ? smc->error : "Unknown AppleSMC error";
}

int ninefan_smc_refresh_fans(ninefan_smc *smc) {
    if (!smc || !smc->is_open) return -1;
    for (int index = 0; index < smc->fan_count; index++) {
        ninefan_fan *fan = &smc->fans[index];
        fan->index = index;
        char key[5];
        fan_key(key, "F%dAc", index);
        if (read_number(smc, key, &fan->actual_rpm) != 0) return -1;
        fan_key(key, "F%dTg", index);
        if (read_number(smc, key, &fan->target_rpm) != 0) return -1;
        fan_key(key, "F%dMn", index);
        if (read_number(smc, key, &fan->minimum_rpm) != 0) return -1;
        fan_key(key, "F%dMx", index);
        if (read_number(smc, key, &fan->maximum_rpm) != 0) return -1;
        fan_key(key, smc->mode_key_format, index);
        if (read_u8(smc, key, &fan->mode) != 0) return -1;
    }
    return 0;
}

int ninefan_smc_hottest_temperature(
    ninefan_smc *smc, float *temperature_c, char hottest_key[5]) {
    if (!smc || !temperature_c) return -1;
    float hottest = -INFINITY;
    char key_for_hottest[5] = "----";
    for (size_t index = 0; index < smc->temperature_key_count; index++) {
        float candidate = NAN;
        if (read_cached_float(smc, index, &candidate) != 0) continue;
        if (!isfinite(candidate) || candidate < 5.0f || candidate > 115.0f) continue;
        if (candidate > hottest) {
            hottest = candidate;
            memcpy(key_for_hottest, smc->temperature_keys[index], 5);
        }
    }
    if (!isfinite(hottest)) {
        snprintf(smc->error, sizeof(smc->error), "No valid CPU/GPU temperature is available");
        return -1;
    }
    *temperature_c = hottest;
    if (hottest_key) memcpy(hottest_key, key_for_hottest, 5);
    return 0;
}

static void short_sleep(long nanoseconds) {
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
        /* Resume the remaining delay after a signal. */
    }
}

int ninefan_smc_enable_manual(ninefan_smc *smc, int fan_index) {
    if (!smc || fan_index < 0 || fan_index >= smc->fan_count) return -1;
    char mode_key[5];
    fan_key(mode_key, smc->mode_key_format, fan_index);
    uint8_t mode = 0;
    if (read_u8(smc, mode_key, &mode) == 0 && mode == 1) return 0;
    if (write_u8(smc, mode_key, 1) == 0) return 0;
    if (!smc->ftst_available) return -1;

    if (write_u8(smc, "Ftst", 1) != 0) return -1;
    short_sleep(500000000L);
    for (int attempt = 0; attempt < 100; attempt++) {
        if (write_u8(smc, mode_key, 1) == 0) return 0;
        short_sleep(100000000L);
    }
    snprintf(smc->error, sizeof(smc->error), "Timed out enabling manual mode for fan %d", fan_index);
    return -1;
}

int ninefan_smc_set_target(ninefan_smc *smc, int fan_index, float rpm) {
    if (!smc || fan_index < 0 || fan_index >= smc->fan_count || !isfinite(rpm)) return -1;
    const ninefan_fan *fan = &smc->fans[fan_index];
    const float bounded = fminf(fan->maximum_rpm, fmaxf(fan->minimum_rpm, rpm));
    char key[5];
    fan_key(key, "F%dTg", fan_index);
    return write_number(smc, key, bounded);
}

int ninefan_smc_restore_default(ninefan_smc *smc) {
    if (!smc || !smc->is_open) return -1;
    int failed = 0;
    char first_error[sizeof(smc->error)] = {0};
    for (int index = 0; index < smc->fan_count; index++) {
        char key[5];
        fan_key(key, smc->mode_key_format, index);
        if (write_u8(smc, key, 0) != 0 && !failed) {
            failed = 1;
            memcpy(first_error, smc->error, sizeof(first_error));
        }
        fan_key(key, "F%dTg", index);
        /*
         * Target reset is best effort: once mode 0 is accepted, M5 firmware may
         * hand control to thermalmonitord before this second write arrives.
         */
        (void)write_number(smc, key, 0.0f);
    }
    if (smc->ftst_available) {
        uint8_t ftst = 0;
        if (read_u8(smc, "Ftst", &ftst) == 0 && ftst == 1
            && write_u8(smc, "Ftst", 0) != 0 && !failed) {
            failed = 1;
            memcpy(first_error, smc->error, sizeof(first_error));
        }
    }
    if (failed) memcpy(smc->error, first_error, sizeof(smc->error));
    return failed ? -1 : 0;
}
