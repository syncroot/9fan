#include "../src/hot_policy.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    ninefan_hot_policy policy;
    ninefan_hot_policy_init(&policy);
    assert(ninefan_hot_policy_allows_manual(&policy));

    assert(ninefan_hot_policy_observe(&policy, 1, 81.9f)
        == NINEFAN_HOT_NORMAL);
    assert(ninefan_hot_policy_observe(&policy, 1, 82.0f)
        == NINEFAN_HOT_MAXIMUM_STARTED);
    assert(ninefan_hot_result_forces_maximum(
        NINEFAN_HOT_MAXIMUM_STARTED));
    assert(ninefan_hot_policy_observe(&policy, 1, 89.9f)
        == NINEFAN_HOT_MAXIMUM_ACTIVE);
    assert(ninefan_hot_result_forces_maximum(
        NINEFAN_HOT_MAXIMUM_ACTIVE));
    assert(ninefan_hot_policy_observe(&policy, 1, 81.9f)
        == NINEFAN_HOT_MAXIMUM_ENDED);
    assert(!ninefan_hot_result_forces_maximum(
        NINEFAN_HOT_MAXIMUM_ENDED));
    assert(ninefan_hot_policy_observe(&policy, 1, 82.0f)
        == NINEFAN_HOT_MAXIMUM_STARTED);

    assert(ninefan_hot_policy_observe(&policy, 1, 90.0f)
        == NINEFAN_HOT_HANDOFF);
    assert(!ninefan_hot_policy_allows_manual(&policy));
    assert(ninefan_hot_policy_hot_start_available(&policy));
    assert(ninefan_hot_policy_consume_hot_start(&policy));
    assert(!ninefan_hot_policy_hot_start_available(&policy));
    assert(!ninefan_hot_policy_consume_hot_start(&policy));
    assert(ninefan_hot_policy_observe(&policy, 1, 89.0f)
        == NINEFAN_HOT_HANDOFF_LOCKED);
    assert(!ninefan_hot_policy_allows_manual(&policy));
    assert(ninefan_hot_policy_observe(&policy, 1, 80.1f)
        == NINEFAN_HOT_HANDOFF_LOCKED);
    assert(ninefan_hot_policy_observe(&policy, 1, 80.0f)
        == NINEFAN_HOT_REARMED);
    assert(ninefan_hot_policy_allows_manual(&policy));
    assert(!ninefan_hot_policy_hot_start_available(&policy));

    assert(ninefan_hot_policy_observe(&policy, 0, 50.0f)
        == NINEFAN_HOT_INVALID);
    assert(!ninefan_hot_policy_allows_manual(&policy));
    assert(ninefan_hot_policy_hot_start_available(&policy));
    assert(ninefan_hot_policy_observe(&policy, 1, NAN)
        == NINEFAN_HOT_INVALID);
    assert(ninefan_hot_policy_observe(&policy, 1, 79.0f)
        == NINEFAN_HOT_REARMED);
    assert(ninefan_hot_policy_allows_manual(&policy));

    assert(ninefan_hot_policy_observe(NULL, 1, 70.0f)
        == NINEFAN_HOT_INVALID);
    assert(!ninefan_hot_policy_allows_manual(NULL));
    assert(!ninefan_hot_policy_hot_start_available(NULL));
    assert(!ninefan_hot_policy_consume_hot_start(NULL));
    assert(!ninefan_hot_result_forces_maximum(NINEFAN_HOT_HANDOFF));
    assert(ninefan_hot_policy_sample_interval_ms(1, 74.9f)
        == NINEFAN_NORMAL_SAMPLE_INTERVAL_MS);
    assert(ninefan_hot_policy_sample_interval_ms(1, 75.0f)
        == NINEFAN_HOT_SAMPLE_INTERVAL_MS);
    assert(ninefan_hot_policy_sample_interval_ms(0, 90.0f)
        == NINEFAN_NORMAL_SAMPLE_INTERVAL_MS);
    assert(ninefan_hot_policy_sample_interval_ms(1, NAN)
        == NINEFAN_NORMAL_SAMPLE_INTERVAL_MS);
    assert(ninefan_hot_start_fan_eligible(
        1, 0, 4000.0f, 3900.0f, 7800.0f, 100.0f));
    assert(ninefan_hot_start_fan_eligible(
        1, 3, 7801.0f, 7899.0f, 7800.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 1, 4000.0f, 3900.0f, 7800.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 0, 7802.0f, 3900.0f, 7800.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 0, 4000.0f, 7901.0f, 7800.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 0, NAN, 3900.0f, 7800.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 0, 4000.0f, 3900.0f, 0.0f, 100.0f));
    assert(!ninefan_hot_start_fan_eligible(
        1, 0, 4000.0f, 3900.0f, 7800.0f, -1.0f));

    puts("hot policy tests passed");
    return 0;
}
