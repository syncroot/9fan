#ifndef NINEFAN_LEASE_H
#define NINEFAN_LEASE_H

#include <stdint.h>

#define NINEFAN_CURVE_LEASE_DEFAULT_MS (30ULL * 60ULL * 1000ULL)
#define NINEFAN_CURVE_LEASE_MAX_MS (30ULL * 60ULL * 1000ULL)
#define NINEFAN_MAX_CURVE_LEASE_MAX_MS (10ULL * 60ULL * 1000ULL)
#define NINEFAN_SELF_TEST_LEASE_MS (2ULL * 60ULL * 1000ULL)
#define NINEFAN_SUSPEND_GAP_MS 15000ULL

typedef struct {
    uint64_t started_ns;
    uint64_t deadline_ns;
    uint64_t last_check_ns;
    uint64_t duration_ms;
    int active;
} ninefan_lease;

typedef enum {
    NINEFAN_LEASE_OK = 0,
    NINEFAN_LEASE_EXPIRED,
    NINEFAN_LEASE_SUSPEND_GAP,
    NINEFAN_LEASE_INVALID,
} ninefan_lease_result;

uint64_t ninefan_continuous_time_ns(void);

int ninefan_lease_start(
    ninefan_lease *lease,
    uint64_t duration_ms,
    uint64_t maximum_ms,
    uint64_t now_ns);

int ninefan_lease_shorten(
    ninefan_lease *lease,
    uint64_t maximum_remaining_ms,
    uint64_t now_ns);

ninefan_lease_result ninefan_lease_check(
    ninefan_lease *lease,
    uint64_t now_ns,
    uint64_t maximum_gap_ms);

uint64_t ninefan_lease_remaining_ms(
    const ninefan_lease *lease,
    uint64_t now_ns);

#endif
