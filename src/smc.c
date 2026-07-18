#include "smc.h"
#include "smc_codec.h"

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

_Static_assert(NINEFAN_MAX_FANS <= 10, "Single-digit SMC fan keys are required");

#ifndef NINEFAN_SMC_READ_ONLY
static void short_sleep(long nanoseconds);
#endif

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
    if (output_size != sizeof(*output)) {
        snprintf(smc->error, sizeof(smc->error),
            "%s returned %zu bytes instead of %zu", operation, output_size, sizeof(*output));
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
    if (input.info.size == 0 || input.info.size > sizeof(output.bytes)) {
        snprintf(smc->error, sizeof(smc->error),
            "%.4s has invalid SMC read size %u", name, input.info.size);
        return -1;
    }
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

#ifndef NINEFAN_SMC_READ_ONLY
static int write_raw(
    ninefan_smc *smc, const char name[4], const uint8_t *bytes, size_t byte_count) {
    smc_key_data input = {0}, output = {0};
    input.key = fourcc(name);
    if (key_info(smc, input.key, &input.info) != 0) return -1;
    if (!bytes || input.info.size == 0 || input.info.size > sizeof(input.bytes)
        || input.info.size != byte_count) {
        snprintf(smc->error, sizeof(smc->error),
            "%.4s has unexpected SMC write size %u (expected %zu)",
            name, input.info.size, byte_count);
        return -1;
    }
    input.command = SMC_WRITE_BYTES;
    memcpy(input.bytes, bytes, byte_count);
    return smc_call(smc, &input, &output, "SMC write");
}
#endif

static int read_number(ninefan_smc *smc, const char name[4], float *value) {
    uint8_t bytes[32] = {0};
    smc_key_info info = {0};
    if (read_raw(smc, name, bytes, &info) != 0) return -1;
    if (ninefan_smc_decode_number(info.type, info.size, bytes, value) == 0) return 0;
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
    return ninefan_smc_decode_number(info.type, info.size, bytes, value);
}

static int read_u8(ninefan_smc *smc, const char name[4], uint8_t *value) {
    uint8_t bytes[32] = {0};
    smc_key_info info = {0};
    if (read_raw(smc, name, bytes, &info) != 0) return -1;
    if (info.size != 1) {
        snprintf(smc->error, sizeof(smc->error),
            "%.4s has invalid uint8 size %u", name, info.size);
        return -1;
    }
    *value = bytes[0];
    return 0;
}

#ifndef NINEFAN_SMC_READ_ONLY
static int write_u8(ninefan_smc *smc, const char name[4], uint8_t value) {
    const uint8_t bytes[1] = {value};
    return write_raw(smc, name, bytes, sizeof(bytes));
}

static int write_number(ninefan_smc *smc, const char name[4], float value) {
    if (!isfinite(value)) {
        snprintf(smc->error, sizeof(smc->error), "%.4s received a non-finite value", name);
        return -1;
    }
    smc_key_info info = {0};
    if (key_info(smc, fourcc(name), &info) != 0) return -1;
    uint8_t bytes[32] = {0};
    if (ninefan_smc_encode_number(info.type, info.size, value, bytes) != 0) {
        snprintf(smc->error, sizeof(smc->error), "%.4s has unsupported SMC format", name);
        return -1;
    }
    return write_raw(smc, name, bytes, info.size);
}
#endif

static void fan_key(char output[5], const char suffix[3], int fan_index) {
    output[0] = 'F';
    output[1] = (char)('0' + fan_index);
    output[2] = suffix[0];
    output[3] = suffix[1];
    output[4] = '\0';
}

static int is_temperature_key(const char key[5]) {
    if (key[0] != 'T') return 0;
    for (size_t index = 2; index < 4; index++) {
        const unsigned char byte = (unsigned char)key[index];
        if (byte < 0x21 || byte > 0x7e) return 0;
    }
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
    if (read_raw(smc, "#KEY", count_bytes, &count_info) != 0 || count_info.size < 4) {
        smc->temperature_keys_saturated = 1;
        return;
    }
    const uint32_t count = ((uint32_t)count_bytes[0] << 24)
                         | ((uint32_t)count_bytes[1] << 16)
                         | ((uint32_t)count_bytes[2] << 8)
                         | (uint32_t)count_bytes[3];
    if (count == 0 || count > 100000) {
        smc->temperature_keys_saturated = 1;
        return;
    }

    for (uint32_t index = 0; index < count; index++) {
        smc_key_data input = {0}, output = {0};
        input.command = SMC_READ_INDEX;
        input.data32 = index;
        if (smc_call(smc, &input, &output, "SMC enumerate") != 0) {
            smc->temperature_keys_saturated = 1;
            continue;
        }
        char key[5] = {
            (char)(output.key >> 24), (char)(output.key >> 16),
            (char)(output.key >> 8), (char)output.key, '\0'
        };
        if (!is_temperature_key(key)) continue;
        smc_key_info info = {0};
        if (key_info(smc, output.key, &info) != 0) {
            smc->temperature_keys_saturated = 1;
            continue;
        }
        if (info.size != 4 || info.type != NINEFAN_SMC_FLOAT) continue;
        if (smc->temperature_key_count == NINEFAN_MAX_TEMP_KEYS) {
            smc->temperature_keys_saturated = 1;
            continue;
        }
        const size_t stored_index = smc->temperature_key_count++;
        memcpy(smc->temperature_keys[stored_index], key, 5);
        smc->temperature_sizes[stored_index] = info.size;
        smc->temperature_types[stored_index] = info.type;
        smc->temperature_attributes[stored_index] = info.attributes;
    }
}

int ninefan_smc_open_recovery(ninefan_smc *smc) {
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
    smc->error[0] = '\0';
    return 0;
}

int ninefan_smc_open(ninefan_smc *smc) {
    if (ninefan_smc_open_recovery(smc) != 0) return -1;
    discover_temperature_keys(smc);
    if (ninefan_smc_refresh_fans(smc) != 0) {
        char error[sizeof(smc->error)];
        memcpy(error, smc->error, sizeof(error));
        ninefan_smc_close(smc);
        memcpy(smc->error, error, sizeof(smc->error));
        return -1;
    }
    return 0;
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
    int failed = 0;
    char first_error[sizeof(smc->error)] = {0};
    for (int index = 0; index < smc->fan_count; index++) {
        ninefan_fan *fan = &smc->fans[index];
        fan->valid = 0;
        ninefan_fan refreshed = {.index = index};
        char key[5];
        fan_key(key, "Ac", index);
        if (read_number(smc, key, &refreshed.actual_rpm) != 0) goto fan_failed;
        fan_key(key, "Tg", index);
        if (read_number(smc, key, &refreshed.target_rpm) != 0) goto fan_failed;
        fan_key(key, "Mn", index);
        if (read_number(smc, key, &refreshed.minimum_rpm) != 0) goto fan_failed;
        fan_key(key, "Mx", index);
        if (read_number(smc, key, &refreshed.maximum_rpm) != 0) goto fan_failed;
        fan_key(key, &smc->mode_key_format[3], index);
        if (read_u8(smc, key, &refreshed.mode) != 0) goto fan_failed;
        if (!isfinite(refreshed.actual_rpm) || !isfinite(refreshed.target_rpm)
            || !isfinite(refreshed.minimum_rpm) || !isfinite(refreshed.maximum_rpm)
            || refreshed.actual_rpm < 0.0f || refreshed.actual_rpm > 50000.0f
            || refreshed.target_rpm < 0.0f || refreshed.target_rpm > 50000.0f
            || refreshed.minimum_rpm <= 0.0f
            || refreshed.maximum_rpm <= 0.0f
            || refreshed.maximum_rpm > 20000.0f
            || refreshed.maximum_rpm < refreshed.minimum_rpm) {
            snprintf(smc->error, sizeof(smc->error),
                "Fan %d returned invalid RPM limits or telemetry", index);
            goto fan_failed;
        }
        refreshed.valid = 1;
        *fan = refreshed;
        continue;

fan_failed:
        if (!failed) memcpy(first_error, smc->error, sizeof(first_error));
        failed = 1;
    }
    if (failed) memcpy(smc->error, first_error, sizeof(smc->error));
    return failed ? -1 : 0;
}

int ninefan_smc_hottest_temperature(
    ninefan_smc *smc, float *temperature_c, char hottest_key[5]) {
    if (!smc || !temperature_c) return -1;
    float hottest = -INFINITY;
    char key_for_hottest[5] = "----";
    for (size_t index = 0; index < smc->temperature_key_count; index++) {
        float candidate = NAN;
        if (read_cached_float(smc, index, &candidate) != 0 || !isfinite(candidate)) {
            snprintf(smc->error, sizeof(smc->error),
                "Temperature sensor %.4s could not be read safely",
                smc->temperature_keys[index]);
            return -1;
        }
        if (candidate < 5.0f) continue;
        if (candidate > hottest) {
            hottest = candidate;
            memcpy(key_for_hottest, smc->temperature_keys[index], 5);
        }
    }
    if (!isfinite(hottest)) {
        snprintf(smc->error, sizeof(smc->error),
            "No valid safety temperature is available");
        return -1;
    }
    *temperature_c = hottest;
    if (hottest_key) memcpy(hottest_key, key_for_hottest, 5);
    return 0;
}

static void fingerprint_bytes(
    uint64_t *hash, const void *bytes, size_t byte_count) {
    const uint8_t *cursor = bytes;
    for (size_t index = 0; index < byte_count; index++) {
        *hash ^= cursor[index];
        *hash *= UINT64_C(1099511628211);
    }
}

static int fingerprint_key_schema(
    ninefan_smc *smc, const char key[5], uint64_t *hash) {
    smc_key_info info = {0};
    if (key_info(smc, fourcc(key), &info) != 0) return -1;
    fingerprint_bytes(hash, key, 4);
    fingerprint_bytes(hash, &info.size, sizeof(info.size));
    fingerprint_bytes(hash, &info.type, sizeof(info.type));
    fingerprint_bytes(hash, &info.attributes, sizeof(info.attributes));
    return 0;
}

int ninefan_smc_validation_fingerprint(
    ninefan_smc *smc, char *output, size_t output_size) {
    if (!smc || !smc->is_open || !output || output_size < 17) return -1;
    if (ninefan_smc_refresh_fans(smc) != 0) return -1;

    uint64_t hash = UINT64_C(14695981039346656037);
    static const char domain[] = "9fan-hardware-schema-v2";
    fingerprint_bytes(&hash, domain, sizeof(domain));
    fingerprint_bytes(&hash, &smc->fan_count, sizeof(smc->fan_count));
    fingerprint_bytes(
        &hash, smc->mode_key_format, sizeof(smc->mode_key_format));
    fingerprint_bytes(
        &hash, &smc->ftst_available, sizeof(smc->ftst_available));

    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        fingerprint_bytes(
            &hash, &fan->minimum_rpm, sizeof(fan->minimum_rpm));
        fingerprint_bytes(
            &hash, &fan->maximum_rpm, sizeof(fan->maximum_rpm));
        const char *suffixes[] = {
            "Ac", "Tg", "Mn", "Mx", &smc->mode_key_format[3],
        };
        for (size_t suffix_index = 0;
             suffix_index < sizeof(suffixes) / sizeof(suffixes[0]);
             suffix_index++) {
            char key[5];
            fan_key(key, suffixes[suffix_index], index);
            if (fingerprint_key_schema(smc, key, &hash) != 0) return -1;
        }
    }

    fingerprint_bytes(
        &hash, &smc->temperature_key_count,
        sizeof(smc->temperature_key_count));
    for (size_t index = 0; index < smc->temperature_key_count; index++) {
        fingerprint_bytes(&hash, smc->temperature_keys[index], 4);
        fingerprint_bytes(
            &hash, &smc->temperature_sizes[index],
            sizeof(smc->temperature_sizes[index]));
        fingerprint_bytes(
            &hash, &smc->temperature_types[index],
            sizeof(smc->temperature_types[index]));
        fingerprint_bytes(
            &hash, &smc->temperature_attributes[index],
            sizeof(smc->temperature_attributes[index]));
    }

    const int count = snprintf(
        output, output_size, "%016llx", (unsigned long long)hash);
    return count == 16 ? 0 : -1;
}

#ifndef NINEFAN_SMC_READ_ONLY
static void short_sleep(long nanoseconds) {
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
        /* Resume the remaining delay after a signal. */
    }
}

static int mode_is(ninefan_smc *smc, const char key[5], uint8_t expected) {
    uint8_t mode = 0xff;
    if (read_u8(smc, key, &mode) != 0) return 0;
    if (mode == expected) return 1;
    snprintf(smc->error, sizeof(smc->error),
        "%.4s reported mode %u instead of %u", key, mode, expected);
    return 0;
}

static int mode_is_system_controlled(ninefan_smc *smc, const char key[5]) {
    uint8_t mode = 0xff;
    if (read_u8(smc, key, &mode) != 0) return 0;
    if (mode == 0 || mode == 3) return 1;
    snprintf(smc->error, sizeof(smc->error),
        "%.4s remained outside Apple-controlled mode (reported %u)", key, mode);
    return 0;
}

int ninefan_smc_enable_manual(
    ninefan_smc *smc, int fan_index,
    const volatile sig_atomic_t *cancel_requested,
    ninefan_smc_progress_callback progress,
    void *progress_context) {
    if (!smc || fan_index < 0 || fan_index >= smc->fan_count) return -1;
    char mode_key[5];
    fan_key(mode_key, &smc->mode_key_format[3], fan_index);
    if (mode_is(smc, mode_key, 1)) return 0;
    if (write_u8(smc, mode_key, 1) == 0) {
        short_sleep(50000000L);
        if (mode_is(smc, mode_key, 1)) return 0;
    }
    if (!smc->ftst_available) {
        snprintf(smc->error, sizeof(smc->error),
            "Fan %d rejected direct manual mode and no Ftst fallback is available",
            fan_index);
        return -1;
    }

    if (write_u8(smc, "Ftst", 1) != 0) return -1;
    if (progress && progress(progress_context) != 0) {
        snprintf(smc->error, sizeof(smc->error),
            "Safety watchdog heartbeat failed during manual-mode transition");
        return -1;
    }
    short_sleep(500000000L);
    for (int attempt = 0; attempt < 60; attempt++) {
        if (cancel_requested && *cancel_requested) {
            snprintf(smc->error, sizeof(smc->error),
                "Manual-mode transition cancelled for fan %d", fan_index);
            return -1;
        }
        if (progress && progress(progress_context) != 0) {
            snprintf(smc->error, sizeof(smc->error),
                "Safety watchdog heartbeat failed during manual-mode transition");
            return -1;
        }
        if (write_u8(smc, mode_key, 1) == 0) {
            short_sleep(50000000L);
            if (mode_is(smc, mode_key, 1)) return 0;
        }
        short_sleep(100000000L);
    }
    snprintf(smc->error, sizeof(smc->error), "Timed out enabling manual mode for fan %d", fan_index);
    return -1;
}

int ninefan_smc_set_target(ninefan_smc *smc, int fan_index, float rpm) {
    if (!smc || fan_index < 0 || fan_index >= smc->fan_count || !isfinite(rpm)) return -1;
    const ninefan_fan *fan = &smc->fans[fan_index];
    if (!fan->valid
        || !isfinite(fan->minimum_rpm) || !isfinite(fan->maximum_rpm)
        || fan->minimum_rpm <= 0.0f
        || fan->maximum_rpm <= 0.0f
        || fan->maximum_rpm > 20000.0f
        || fan->maximum_rpm < fan->minimum_rpm) {
        snprintf(smc->error, sizeof(smc->error),
            "Fan %d has unsafe RPM limits", fan_index);
        return -1;
    }
    const float bounded = fminf(fan->maximum_rpm, fmaxf(fan->minimum_rpm, rpm));
    if (!(bounded > 0.0f)) {
        snprintf(smc->error, sizeof(smc->error),
            "Fan %d manual target must be above zero RPM", fan_index);
        return -1;
    }
    char mode_key[5], target_key[5];
    fan_key(mode_key, &smc->mode_key_format[3], fan_index);
    fan_key(target_key, "Tg", fan_index);
    if (!mode_is(smc, mode_key, 1)) return -1;
    if (write_number(smc, target_key, bounded) != 0) return -1;
    for (int attempt = 0; attempt < 20; attempt++) {
        short_sleep(100000000L);
        float accepted = NAN;
        if (mode_is(smc, mode_key, 1)
            && read_number(smc, target_key, &accepted) == 0
            && fabsf(accepted - bounded) <= 25.0f) {
            return 0;
        }
    }
    snprintf(smc->error, sizeof(smc->error),
        "Fan %d did not accept target %.0f RPM", fan_index, bounded);
    return -1;
}

static void best_effort_maximum_target(ninefan_smc *smc, int fan_index) {
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

int ninefan_smc_restore_default(ninefan_smc *smc) {
    if (!smc || !smc->is_open) return -1;
    int failed = 0;
    char first_error[sizeof(smc->error)] = {0};
    for (int index = 0; index < smc->fan_count; index++) {
        char key[5];
        fan_key(key, &smc->mode_key_format[3], index);
        int automatic = 0;
        for (int attempt = 0; attempt < 5 && !automatic; attempt++) {
            if (write_u8(smc, key, 0) == 0) {
                short_sleep(50000000L);
                automatic = mode_is_system_controlled(smc, key);
            }
        }
        if (!automatic && !failed) {
            failed = 1;
            memcpy(first_error, smc->error, sizeof(first_error));
        }
        if (!automatic) {
            /*
             * Never lower a target when automatic-mode restoration failed.
             * Best-effort maximum cooling is safer than leaving an unknown
             * manual target in place.
             */
            best_effort_maximum_target(smc, index);
        }
    }
    if (smc->ftst_available) {
        uint8_t ftst = 0xff;
        int clear_result = write_u8(smc, "Ftst", 0);
        if (clear_result == 0) clear_result = read_u8(smc, "Ftst", &ftst);
        if (clear_result == 0 && ftst != 0) {
            snprintf(smc->error, sizeof(smc->error),
                "Ftst did not return to its default state");
            clear_result = -1;
        }
        if (clear_result != 0 && !failed) {
            failed = 1;
            memcpy(first_error, smc->error, sizeof(first_error));
        }
    }
    /*
     * Clearing Ftst or another controller could change mode after the first
     * verification. Recheck at the end and choose maximum cooling on failure.
     * We intentionally never write a zero target; Apple owns that decision in
     * verified automatic mode.
     */
    for (int index = 0; index < smc->fan_count; index++) {
        char mode_key[5];
        fan_key(mode_key, &smc->mode_key_format[3], index);
        if (!mode_is_system_controlled(smc, mode_key)) {
            if (!failed) {
                failed = 1;
                memcpy(first_error, smc->error, sizeof(first_error));
            }
            best_effort_maximum_target(smc, index);
        }
    }
    if (failed) memcpy(smc->error, first_error, sizeof(smc->error));
    return failed ? -1 : 0;
}
#endif
