#include "../src/lease.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint64_t start = 1000000000ULL;
    ninefan_lease lease;
    assert(ninefan_lease_start(&lease, 10000, 10000, start) == 0);
    assert(ninefan_lease_remaining_ms(&lease, start) == 10000);
    assert(ninefan_lease_check(
        &lease, start + 1000000000ULL, 2000) == NINEFAN_LEASE_OK);
    assert(ninefan_lease_remaining_ms(
        &lease, start + 1000000000ULL) == 9000);
    assert(ninefan_lease_shorten(
        &lease, 2000, start + 1000000000ULL) == 0);
    assert(ninefan_lease_remaining_ms(
        &lease, start + 1000000000ULL) == 2000);
    assert(ninefan_lease_shorten(
        &lease, 5000, start + 1000000000ULL) == 0);
    assert(ninefan_lease_remaining_ms(
        &lease, start + 1000000000ULL) == 2000);
    assert(ninefan_lease_check(
        &lease, start + 3000000000ULL, 2000)
        == NINEFAN_LEASE_EXPIRED);

    assert(ninefan_lease_start(&lease, 10000, 10000, start) == 0);
    assert(ninefan_lease_check(
        &lease, start + 4000000001ULL, 2000)
        == NINEFAN_LEASE_SUSPEND_GAP);
    assert(ninefan_lease_start(&lease, 10000, 10000, start) == 0);
    assert(ninefan_lease_check(
        &lease, start + 10000000000ULL, 2000)
        == NINEFAN_LEASE_EXPIRED);
    assert(ninefan_lease_remaining_ms(
        &lease, start + 10000000000ULL) == 0);

    assert(ninefan_lease_start(&lease, 10001, 10000, start) != 0);
    assert(ninefan_lease_start(&lease, 0, 10000, start) != 0);
    assert(ninefan_lease_start(NULL, 1, 1, start) != 0);
    puts("lease tests passed");
    return 0;
}
