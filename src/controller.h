#ifndef NINEFAN_CONTROLLER_H
#define NINEFAN_CONTROLLER_H

#include "curve.h"

typedef struct {
    const ninefan_curve *curve;
    int manual_active;
    float held_temperature;
    float current_temperature;
    char hottest_key[5];
    int temperature_valid;
    float requested_fraction;
} ninefan_controller;

void ninefan_controller_init(ninefan_controller *controller);
void ninefan_controller_select(
    ninefan_controller *controller, const ninefan_curve *curve);
void ninefan_controller_reset_runtime(ninefan_controller *controller);

/*
 * Updates temperature hold/hysteresis and returns 1 when manual control is
 * required, 0 when Apple automatic control should remain active.
 */
int ninefan_controller_compute(ninefan_controller *controller);
int ninefan_controller_force_maximum(ninefan_controller *controller);
float ninefan_controller_target(
    const ninefan_controller *controller,
    float minimum_rpm,
    float maximum_rpm,
    float current_target_rpm,
    float actual_rpm);

#endif
