#ifndef NINEFAN_SMC_H
#define NINEFAN_SMC_H

#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define NINEFAN_MAX_FANS 8
#define NINEFAN_MAX_TEMP_KEYS 512

typedef struct {
    int index;
    float actual_rpm;
    float target_rpm;
    float minimum_rpm;
    float maximum_rpm;
    uint8_t mode;
    int valid;
} ninefan_fan;

typedef struct {
    io_connect_t connection;
    int is_open;
    int fan_count;
    ninefan_fan fans[NINEFAN_MAX_FANS];
    char mode_key_format[6];
    int ftst_available;
    char temperature_keys[NINEFAN_MAX_TEMP_KEYS][5];
    uint32_t temperature_sizes[NINEFAN_MAX_TEMP_KEYS];
    uint32_t temperature_types[NINEFAN_MAX_TEMP_KEYS];
    uint8_t temperature_attributes[NINEFAN_MAX_TEMP_KEYS];
    size_t temperature_key_count;
    int temperature_keys_saturated;
    char error[192];
} ninefan_smc;

typedef int (*ninefan_smc_progress_callback)(void *context);

int ninefan_smc_open(ninefan_smc *smc);
int ninefan_smc_open_recovery(ninefan_smc *smc);
void ninefan_smc_close(ninefan_smc *smc);
const char *ninefan_smc_error(const ninefan_smc *smc);

int ninefan_smc_refresh_fans(ninefan_smc *smc);
int ninefan_smc_hottest_temperature(
    ninefan_smc *smc, float *temperature_c, char hottest_key[5]);
int ninefan_smc_validation_fingerprint(
    ninefan_smc *smc, char *output, size_t output_size);

int ninefan_smc_enable_manual(
    ninefan_smc *smc, int fan_index,
    const volatile sig_atomic_t *cancel_requested,
    ninefan_smc_progress_callback progress,
    void *progress_context);
int ninefan_smc_set_target(ninefan_smc *smc, int fan_index, float rpm);
int ninefan_smc_restore_default(ninefan_smc *smc);

#endif
