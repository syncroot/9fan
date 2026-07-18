#ifndef NINEFAN_RESPONSE_MONITOR_H
#define NINEFAN_RESPONSE_MONITOR_H

#define NINEFAN_RESPONSE_MAX_FANS 8
#define NINEFAN_RESPONSE_GRACE_MS 8000
#define NINEFAN_RESPONSE_FAILURE_LIMIT 3
#define NINEFAN_RESPONSE_TARGET_TOLERANCE_RPM 100.0f

typedef enum {
    NINEFAN_RESPONSE_OK = 0,
    NINEFAN_RESPONSE_GRACE = 1,
    NINEFAN_RESPONSE_INVALID = -1,
    NINEFAN_RESPONSE_TARGET_CHANGED = -2,
    NINEFAN_RESPONSE_STALLED = -3,
} ninefan_response_result;

typedef struct {
    float expected_target[NINEFAN_RESPONSE_MAX_FANS];
    long long grace_until_ms[NINEFAN_RESPONSE_MAX_FANS];
    unsigned int failures[NINEFAN_RESPONSE_MAX_FANS];
    int armed[NINEFAN_RESPONSE_MAX_FANS];
} ninefan_response_monitor;

void ninefan_response_monitor_init(ninefan_response_monitor *monitor);
int ninefan_response_monitor_note_target(
    ninefan_response_monitor *monitor,
    int fan_index,
    float target_rpm,
    long long now_ms);
ninefan_response_result ninefan_response_monitor_observe(
    ninefan_response_monitor *monitor,
    int fan_index,
    float minimum_rpm,
    float actual_rpm,
    float reported_target_rpm,
    long long now_ms,
    float *required_rpm);

#endif
