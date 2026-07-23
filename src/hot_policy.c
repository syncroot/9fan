#include "hot_policy.h"

#include <math.h>
#include <string.h>

void ninefan_hot_policy_init(ninefan_hot_policy *policy) {
    if (!policy) return;
    memset(policy, 0, sizeof(*policy));
}

ninefan_hot_result ninefan_hot_policy_observe(
    ninefan_hot_policy *policy,
    int temperature_valid,
    float temperature_c) {
    if (!policy || !temperature_valid || !isfinite(temperature_c)) {
        if (policy) {
            policy->handoff_latched = 1;
            policy->maximum_active = 0;
        }
        return NINEFAN_HOT_INVALID;
    }

    if (policy->handoff_latched) {
        if (temperature_c <= NINEFAN_MANUAL_REARM_C) {
            policy->handoff_latched = 0;
            policy->maximum_active = 0;
            policy->hot_start_consumed = 0;
            return NINEFAN_HOT_REARMED;
        }
        return NINEFAN_HOT_HANDOFF_LOCKED;
    }

    if (temperature_c >= NINEFAN_APPLE_HANDOFF_C) {
        policy->handoff_latched = 1;
        policy->maximum_active = 0;
        return NINEFAN_HOT_HANDOFF;
    }

    if (temperature_c >= NINEFAN_PREEMPTIVE_MAXIMUM_C) {
        if (!policy->maximum_active) {
            policy->maximum_active = 1;
            return NINEFAN_HOT_MAXIMUM_STARTED;
        }
        return NINEFAN_HOT_MAXIMUM_ACTIVE;
    }

    if (policy->maximum_active) {
        policy->maximum_active = 0;
        return NINEFAN_HOT_MAXIMUM_ENDED;
    }
    return NINEFAN_HOT_NORMAL;
}

int ninefan_hot_policy_allows_manual(const ninefan_hot_policy *policy) {
    return policy && !policy->handoff_latched;
}

int ninefan_hot_policy_hot_start_available(
    const ninefan_hot_policy *policy) {
    return policy
        && policy->handoff_latched
        && !policy->hot_start_consumed;
}

int ninefan_hot_policy_consume_hot_start(ninefan_hot_policy *policy) {
    if (!ninefan_hot_policy_hot_start_available(policy)) return 0;
    policy->hot_start_consumed = 1;
    return 1;
}

int ninefan_hot_result_forces_maximum(ninefan_hot_result result) {
    return result == NINEFAN_HOT_MAXIMUM_STARTED
        || result == NINEFAN_HOT_MAXIMUM_ACTIVE;
}

unsigned int ninefan_hot_policy_sample_interval_ms(
    int temperature_valid,
    float temperature_c) {
    return temperature_valid
        && isfinite(temperature_c)
        && temperature_c >= NINEFAN_FAST_SAMPLE_C
            ? NINEFAN_HOT_SAMPLE_INTERVAL_MS
            : NINEFAN_NORMAL_SAMPLE_INTERVAL_MS;
}

int ninefan_hot_start_fan_eligible(
    int fan_valid,
    unsigned int fan_mode,
    float target_rpm,
    float actual_rpm,
    float maximum_rpm,
    float actual_tolerance_rpm) {
    return fan_valid
        && (fan_mode == 0U || fan_mode == 3U)
        && isfinite(target_rpm)
        && isfinite(actual_rpm)
        && isfinite(maximum_rpm)
        && isfinite(actual_tolerance_rpm)
        && maximum_rpm > 0.0f
        && actual_tolerance_rpm >= 0.0f
        && target_rpm <= maximum_rpm + 1.0f
        && actual_rpm <= maximum_rpm + actual_tolerance_rpm;
}
