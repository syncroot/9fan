#include "lease.h"

#include <limits.h>
#include <mach/mach_time.h>
#include <string.h>

uint64_t ninefan_continuous_time_ns(void) {
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0
        && mach_timebase_info(&timebase) != KERN_SUCCESS) {
        return 0;
    }
    if (timebase.denom == 0) return 0;
    const __uint128_t nanoseconds =
        (__uint128_t)mach_continuous_time() * timebase.numer / timebase.denom;
    return nanoseconds > UINT64_MAX ? UINT64_MAX : (uint64_t)nanoseconds;
}

int ninefan_lease_start(
    ninefan_lease *lease,
    uint64_t duration_ms,
    uint64_t maximum_ms,
    uint64_t now_ns) {
    if (!lease
        || now_ns == 0
        || duration_ms == 0
        || maximum_ms == 0
        || duration_ms > maximum_ms
        || duration_ms > UINT64_MAX / 1000000ULL) {
        return -1;
    }
    const uint64_t duration_ns = duration_ms * 1000000ULL;
    if (now_ns > UINT64_MAX - duration_ns) return -1;
    memset(lease, 0, sizeof(*lease));
    lease->started_ns = now_ns;
    lease->deadline_ns = now_ns + duration_ns;
    lease->last_check_ns = now_ns;
    lease->duration_ms = duration_ms;
    lease->active = 1;
    return 0;
}

int ninefan_lease_shorten(
    ninefan_lease *lease,
    uint64_t maximum_remaining_ms,
    uint64_t now_ns) {
    if (!lease
        || !lease->active
        || maximum_remaining_ms == 0
        || maximum_remaining_ms > UINT64_MAX / 1000000ULL
        || now_ns == 0
        || now_ns < lease->started_ns) {
        return -1;
    }
    const uint64_t remaining_ns =
        maximum_remaining_ms * 1000000ULL;
    if (now_ns > UINT64_MAX - remaining_ns) return -1;
    const uint64_t shortened_deadline = now_ns + remaining_ns;
    if (shortened_deadline < lease->deadline_ns) {
        lease->deadline_ns = shortened_deadline;
    }
    return 0;
}

ninefan_lease_result ninefan_lease_check(
    ninefan_lease *lease,
    uint64_t now_ns,
    uint64_t maximum_gap_ms) {
    if (!lease
        || !lease->active
        || now_ns == 0
        || maximum_gap_ms > UINT64_MAX / 1000000ULL
        || now_ns < lease->last_check_ns
        || lease->deadline_ns < lease->started_ns) {
        return NINEFAN_LEASE_INVALID;
    }
    const uint64_t elapsed = now_ns - lease->last_check_ns;
    lease->last_check_ns = now_ns;
    if (now_ns >= lease->deadline_ns) return NINEFAN_LEASE_EXPIRED;
    if (maximum_gap_ms > 0
        && elapsed > maximum_gap_ms * 1000000ULL) {
        return NINEFAN_LEASE_SUSPEND_GAP;
    }
    return NINEFAN_LEASE_OK;
}

uint64_t ninefan_lease_remaining_ms(
    const ninefan_lease *lease,
    uint64_t now_ns) {
    if (!lease || !lease->active || now_ns >= lease->deadline_ns) return 0;
    return (lease->deadline_ns - now_ns) / 1000000ULL;
}
