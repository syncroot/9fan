#include "response_monitor.h"

#include <math.h>
#include <string.h>

void ninefan_response_monitor_init(ninefan_response_monitor *monitor) {
    if (!monitor) return;
    memset(monitor, 0, sizeof(*monitor));
    for (int index = 0; index < NINEFAN_RESPONSE_MAX_FANS; index++) {
        monitor->expected_target[index] = NAN;
    }
}

int ninefan_response_monitor_note_target(
    ninefan_response_monitor *monitor,
    int fan_index,
    float target_rpm,
    long long now_ms) {
    if (!monitor
        || fan_index < 0
        || fan_index >= NINEFAN_RESPONSE_MAX_FANS
        || !isfinite(target_rpm)
        || target_rpm <= 0.0f
        || now_ms < 0) {
        return -1;
    }
    if (!monitor->armed[fan_index]) {
        monitor->grace_until_ms[fan_index] =
            now_ms + NINEFAN_RESPONSE_GRACE_MS;
        monitor->failures[fan_index] = 0;
        monitor->armed[fan_index] = 1;
    } else if (target_rpm <= monitor->expected_target[fan_index]) {
        monitor->failures[fan_index] = 0;
    }
    monitor->expected_target[fan_index] = target_rpm;
    return 0;
}

ninefan_response_result ninefan_response_monitor_observe(
    ninefan_response_monitor *monitor,
    int fan_index,
    float minimum_rpm,
    float actual_rpm,
    float reported_target_rpm,
    long long now_ms,
    float *required_rpm) {
    if (required_rpm) *required_rpm = NAN;
    if (!monitor
        || fan_index < 0
        || fan_index >= NINEFAN_RESPONSE_MAX_FANS
        || !monitor->armed[fan_index]
        || !isfinite(monitor->expected_target[fan_index])
        || !isfinite(minimum_rpm)
        || !isfinite(actual_rpm)
        || !isfinite(reported_target_rpm)
        || minimum_rpm <= 0.0f
        || actual_rpm < 0.0f
        || reported_target_rpm <= 0.0f
        || now_ms < 0) {
        return NINEFAN_RESPONSE_INVALID;
    }

    const float expected = monitor->expected_target[fan_index];
    if (fabsf(reported_target_rpm - expected)
        > NINEFAN_RESPONSE_TARGET_TOLERANCE_RPM) {
        return NINEFAN_RESPONSE_TARGET_CHANGED;
    }
    if (now_ms < monitor->grace_until_ms[fan_index]) {
        return NINEFAN_RESPONSE_GRACE;
    }

    const float target_span = fmaxf(0.0f, expected - minimum_rpm);
    const float required = target_span <= NINEFAN_RESPONSE_TARGET_TOLERANCE_RPM
        ? minimum_rpm * 0.50f
        : minimum_rpm + target_span * 0.50f;
    if (required_rpm) *required_rpm = required;

    if (actual_rpm + NINEFAN_RESPONSE_TARGET_TOLERANCE_RPM >= required) {
        monitor->failures[fan_index] = 0;
        return NINEFAN_RESPONSE_OK;
    }
    monitor->failures[fan_index]++;
    return monitor->failures[fan_index] >= NINEFAN_RESPONSE_FAILURE_LIMIT
        ? NINEFAN_RESPONSE_STALLED
        : NINEFAN_RESPONSE_OK;
}
