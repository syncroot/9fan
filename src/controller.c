#include "controller.h"

#include <math.h>
#include <string.h>

void ninefan_controller_init(ninefan_controller *controller) {
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    controller->held_temperature = NAN;
    controller->current_temperature = NAN;
    controller->requested_fraction = NAN;
    memcpy(controller->hottest_key, "----", 5);
}

void ninefan_controller_select(
    ninefan_controller *controller, const ninefan_curve *curve) {
    if (!controller) return;
    controller->curve = curve;
    controller->held_temperature = NAN;
    controller->requested_fraction = NAN;
}

void ninefan_controller_reset_runtime(ninefan_controller *controller) {
    if (!controller) return;
    controller->manual_active = 0;
    controller->held_temperature = NAN;
    controller->requested_fraction = NAN;
}

int ninefan_controller_compute(ninefan_controller *controller) {
    if (!controller || !controller->curve) return 0;

    const int temperature_is_valid =
        controller->temperature_valid
        && isfinite(controller->current_temperature);
    if (!temperature_is_valid) {
        controller->requested_fraction = NAN;
        return 0;
    }
    if (!isfinite(controller->held_temperature)
        || controller->current_temperature >= controller->held_temperature) {
        controller->held_temperature = controller->current_temperature;
    } else {
        controller->held_temperature =
            fmaxf(controller->current_temperature, controller->held_temperature - 1.5f);
    }

    const int needs_manual =
        controller->held_temperature
        >= controller->curve->activation_c
               - (controller->manual_active
                       ? controller->curve->release_hysteresis_c
                       : 0.0f);
    if (!needs_manual) {
        controller->requested_fraction = NAN;
        return 0;
    }

    const float fraction =
        ninefan_curve_fraction(controller->curve, controller->held_temperature);
    controller->requested_fraction = fminf(1.0f, fmaxf(0.0f, fraction));
    return 1;
}

float ninefan_controller_target(
    const ninefan_controller *controller,
    float minimum_rpm,
    float maximum_rpm,
    float current_target_rpm,
    float actual_rpm) {
    if (!controller || !isfinite(controller->requested_fraction)
        || !isfinite(minimum_rpm) || !isfinite(maximum_rpm)
        || minimum_rpm <= 0.0f || maximum_rpm < minimum_rpm) {
        return NAN;
    }
    float target = minimum_rpm
        + controller->requested_fraction * (maximum_rpm - minimum_rpm);
    if (!controller->manual_active) {
        if (isfinite(current_target_rpm)) target = fmaxf(target, current_target_rpm);
        if (isfinite(actual_rpm)) target = fmaxf(target, actual_rpm);
    }
    return fminf(maximum_rpm, fmaxf(minimum_rpm, target));
}
