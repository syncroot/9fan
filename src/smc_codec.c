#include "smc_codec.h"

#include <math.h>
#include <string.h>

int ninefan_smc_decode_number(
    uint32_t type, size_t size, const uint8_t bytes[32], float *value) {
    if (!bytes || !value) return -1;
    if (size == 4 && type == NINEFAN_SMC_FLOAT) {
        memcpy(value, bytes, sizeof(*value));
        return isfinite(*value) ? 0 : -1;
    }
    if (size == 2 && type == NINEFAN_SMC_FPE2) {
        const uint16_t raw =
            (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
        *value = (float)raw / 4.0f;
        return 0;
    }
    return -1;
}

int ninefan_smc_encode_number(
    uint32_t type, size_t size, float value, uint8_t bytes[32]) {
    if (!bytes || !isfinite(value)) return -1;
    memset(bytes, 0, 32);
    if (size == 4 && type == NINEFAN_SMC_FLOAT) {
        memcpy(bytes, &value, sizeof(value));
        return 0;
    }
    if (size == 2 && type == NINEFAN_SMC_FPE2) {
        if (value < 0.0f || value > 16383.75f) return -1;
        const uint16_t raw = (uint16_t)lroundf(value * 4.0f);
        bytes[0] = (uint8_t)(raw >> 8);
        bytes[1] = (uint8_t)(raw & 0xff);
        return 0;
    }
    return -1;
}
