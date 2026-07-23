#ifndef NINEFAN_HOT_POLICY_H
#define NINEFAN_HOT_POLICY_H

#define NINEFAN_PREEMPTIVE_MAXIMUM_C 82.0f
#define NINEFAN_APPLE_HANDOFF_C 90.0f
#define NINEFAN_MANUAL_REARM_C 80.0f
#define NINEFAN_FAST_SAMPLE_C 75.0f
#define NINEFAN_NORMAL_SAMPLE_INTERVAL_MS 2000U
#define NINEFAN_HOT_SAMPLE_INTERVAL_MS 500U

typedef struct {
    int handoff_latched;
    int maximum_active;
    int hot_start_consumed;
} ninefan_hot_policy;

typedef enum {
    NINEFAN_HOT_NORMAL = 0,
    NINEFAN_HOT_MAXIMUM_STARTED,
    NINEFAN_HOT_MAXIMUM_ACTIVE,
    NINEFAN_HOT_MAXIMUM_ENDED,
    NINEFAN_HOT_HANDOFF,
    NINEFAN_HOT_HANDOFF_LOCKED,
    NINEFAN_HOT_REARMED,
    NINEFAN_HOT_INVALID,
} ninefan_hot_result;

void ninefan_hot_policy_init(ninefan_hot_policy *policy);
ninefan_hot_result ninefan_hot_policy_observe(
    ninefan_hot_policy *policy,
    int temperature_valid,
    float temperature_c);
int ninefan_hot_policy_allows_manual(const ninefan_hot_policy *policy);
int ninefan_hot_policy_hot_start_available(
    const ninefan_hot_policy *policy);
int ninefan_hot_policy_consume_hot_start(ninefan_hot_policy *policy);
int ninefan_hot_result_forces_maximum(ninefan_hot_result result);
unsigned int ninefan_hot_policy_sample_interval_ms(
    int temperature_valid,
    float temperature_c);
int ninefan_hot_start_fan_eligible(
    int fan_valid,
    unsigned int fan_mode,
    float target_rpm,
    float actual_rpm,
    float maximum_rpm,
    float actual_tolerance_rpm);

#endif
