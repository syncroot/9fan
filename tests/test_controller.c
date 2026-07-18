#include "../src/controller.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int near(float left, float right) {
    return fabsf(left - right) < 0.001f;
}

int main(void) {
    ninefan_controller controller;
    ninefan_controller_init(&controller);
    assert(controller.curve == NULL);
    assert(!controller.manual_active);
    assert(isnan(controller.held_temperature));

    const ninefan_curve *balanced = ninefan_curve_named("balanced");
    assert(balanced != NULL);
    ninefan_controller_select(&controller, balanced);

    controller.temperature_valid = 1;
    controller.current_temperature = 50.0f;
    assert(ninefan_controller_compute(&controller) == 0);
    assert(isnan(controller.requested_fraction));

    controller.current_temperature = 67.0f;
    assert(ninefan_controller_compute(&controller) == 1);
    assert(near(controller.requested_fraction, 0.35f));

    controller.manual_active = 1;
    controller.current_temperature = 50.0f;
    assert(ninefan_controller_compute(&controller) == 1);
    assert(near(controller.held_temperature, 65.5f));

    for (int step = 0; step < 9; step++) {
        (void)ninefan_controller_compute(&controller);
    }
    assert(ninefan_controller_compute(&controller) == 0);

    controller.temperature_valid = 0;
    assert(ninefan_controller_compute(&controller) == 0);
    assert(isnan(controller.requested_fraction));
    controller.temperature_valid = 1;
    controller.current_temperature = NAN;
    assert(ninefan_controller_compute(&controller) == 0);
    assert(isnan(controller.requested_fraction));

    controller.temperature_valid = 1;
    controller.requested_fraction = 0.0f;
    controller.manual_active = 0;
    assert(near(ninefan_controller_target(
        &controller, 2300.0f, 7800.0f, 4100.0f, 4000.0f), 4100.0f));
    controller.manual_active = 1;
    assert(near(ninefan_controller_target(
        &controller, 2300.0f, 7800.0f, 4100.0f, 4000.0f), 2300.0f));
    assert(isnan(ninefan_controller_target(
        &controller, 7800.0f, 2300.0f, 0.0f, 0.0f)));
    assert(isnan(ninefan_controller_target(
        &controller, 0.0f, 7800.0f, 0.0f, 0.0f)));

    ninefan_controller_reset_runtime(&controller);
    assert(!controller.manual_active);
    assert(isnan(controller.held_temperature));
    assert(isnan(controller.requested_fraction));

    puts("controller tests passed");
    return 0;
}
