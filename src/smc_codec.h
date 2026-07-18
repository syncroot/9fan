#ifndef NINEFAN_SMC_CODEC_H
#define NINEFAN_SMC_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define NINEFAN_SMC_FLOAT 0x666c7420U
#define NINEFAN_SMC_FPE2 0x66706532U

int ninefan_smc_decode_number(
    uint32_t type, size_t size, const uint8_t bytes[32], float *value);
int ninefan_smc_encode_number(
    uint32_t type, size_t size, float value, uint8_t bytes[32]);

#endif
