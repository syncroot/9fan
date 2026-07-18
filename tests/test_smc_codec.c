#include "../src/smc_codec.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float left, float right) {
    return fabsf(left - right) < 0.01f;
}

int main(void) {
    uint8_t bytes[32] = {0};
    float decoded = 0.0f;

    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FPE2, 2, 2317.0f, bytes) == 0);
    assert(ninefan_smc_decode_number(
        NINEFAN_SMC_FPE2, 2, bytes, &decoded) == 0);
    assert(near(decoded, 2317.0f));

    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FPE2, 2, 16383.75f, bytes) == 0);
    assert(bytes[0] == 0xff && bytes[1] == 0xff);
    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FPE2, 2, 16384.0f, bytes) != 0);
    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FPE2, 2, -1.0f, bytes) != 0);
    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FPE2, 2, NAN, bytes) != 0);

    const float source = 72.5f;
    assert(ninefan_smc_encode_number(
        NINEFAN_SMC_FLOAT, 4, source, bytes) == 0);
    assert(ninefan_smc_decode_number(
        NINEFAN_SMC_FLOAT, 4, bytes, &decoded) == 0);
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);

    assert(ninefan_smc_encode_number(0, 4, 1.0f, bytes) != 0);
    assert(ninefan_smc_decode_number(0, 4, bytes, &decoded) != 0);
    assert(ninefan_smc_decode_number(
        NINEFAN_SMC_FLOAT, 4, bytes, NULL) != 0);

    puts("SMC codec tests passed");
    return 0;
}
