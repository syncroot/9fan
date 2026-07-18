#include "../src/response_monitor.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    ninefan_response_monitor monitor;
    ninefan_response_monitor_init(&monitor);

    float required = 0.0f;
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 0.0f, 2300.0f, 0, &required)
        == NINEFAN_RESPONSE_INVALID);
    assert(isnan(required));

    assert(ninefan_response_monitor_note_target(
        &monitor, 0, 7800.0f, 1000) == 0);
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 0.0f, 7800.0f, 8999, &required)
        == NINEFAN_RESPONSE_GRACE);
    assert(isnan(required));

    assert(ninefan_response_monitor_note_target(
        &monitor, 0, 7900.0f, 8500) == 0);
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 5100.0f, 7900.0f, 9000, &required)
        == NINEFAN_RESPONSE_OK);
    assert(fabsf(required - 5100.0f) < 0.01f);

    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 1000.0f, 7900.0f, 11000, NULL)
        == NINEFAN_RESPONSE_OK);
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 1000.0f, 7900.0f, 13000, NULL)
        == NINEFAN_RESPONSE_OK);
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 1000.0f, 7900.0f, 15000, NULL)
        == NINEFAN_RESPONSE_STALLED);

    assert(ninefan_response_monitor_note_target(
        &monitor, 0, 2300.0f, 20000) == 0);
    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 1200.0f, 2300.0f, 28000, &required)
        == NINEFAN_RESPONSE_OK);
    assert(fabsf(required - 1150.0f) < 0.01f);

    assert(ninefan_response_monitor_observe(
        &monitor, 0, 2300.0f, 2300.0f, 2500.1f, 30000, NULL)
        == NINEFAN_RESPONSE_TARGET_CHANGED);
    assert(ninefan_response_monitor_note_target(
        &monitor, -1, 2300.0f, 0) != 0);
    assert(ninefan_response_monitor_note_target(
        &monitor, 0, NAN, 0) != 0);

    puts("response monitor tests passed");
    return 0;
}
