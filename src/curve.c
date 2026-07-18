#include "curve.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

const ninefan_curve ninefan_curves[] = {
    {
        .name = "Quiet",
        .slug = "quiet",
        .summary = "Apple auto <65C; 65C min, 75C 35%, 82C 70%, 90C max",
        .activation_c = 65.0f,
        .release_hysteresis_c = 4.0f,
        .points = {{65.0f, 0.00f}, {75.0f, 0.35f}, {82.0f, 0.70f}, {90.0f, 1.00f}},
        .point_count = 4,
    },
    {
        .name = "Balanced",
        .slug = "balanced",
        .summary = "Apple auto <55C; 55C min, 67C 35%, 78C 70%, 88C max",
        .activation_c = 55.0f,
        .release_hysteresis_c = 4.0f,
        .points = {{55.0f, 0.00f}, {67.0f, 0.35f}, {78.0f, 0.70f}, {88.0f, 1.00f}},
        .point_count = 4,
    },
    {
        .name = "Performance",
        .slug = "performance",
        .summary = "Apple auto <40C; 40C min, 55C 45%, 68C 75%, 82C max",
        .activation_c = 40.0f,
        .release_hysteresis_c = 3.0f,
        .points = {{40.0f, 0.00f}, {55.0f, 0.45f}, {68.0f, 0.75f}, {82.0f, 1.00f}},
        .point_count = 4,
    },
    {
        .name = "Maximum",
        .slug = "max",
        .summary = "Maximum SMC-reported RPM at every temperature",
        .activation_c = 0.0f,
        .release_hysteresis_c = 0.0f,
        .points = {{0.0f, 1.00f}},
        .point_count = 1,
    },
};

const size_t ninefan_curve_count = sizeof(ninefan_curves) / sizeof(ninefan_curves[0]);

static int equal_ignoring_case(const char *left, const char *right) {
    if (!left || !right) return 0;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const ninefan_curve *ninefan_curve_named(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        if (equal_ignoring_case(name, ninefan_curves[index].slug)
            || equal_ignoring_case(name, ninefan_curves[index].name)) {
            return &ninefan_curves[index];
        }
    }
    return NULL;
}

float ninefan_curve_fraction(const ninefan_curve *curve, float temperature_c) {
    if (!curve || curve->point_count == 0 || !isfinite(temperature_c)) return 1.0f;
    if (temperature_c <= curve->points[0].temperature_c) {
        return curve->points[0].fan_fraction;
    }

    for (size_t index = 1; index < curve->point_count; index++) {
        const ninefan_curve_point left = curve->points[index - 1];
        const ninefan_curve_point right = curve->points[index];
        if (temperature_c <= right.temperature_c) {
            const float span = right.temperature_c - left.temperature_c;
            if (span <= 0.0f) return right.fan_fraction;
            const float position = (temperature_c - left.temperature_c) / span;
            return left.fan_fraction + position * (right.fan_fraction - left.fan_fraction);
        }
    }
    return curve->points[curve->point_count - 1].fan_fraction;
}

int ninefan_curve_is_valid(const ninefan_curve *curve) {
    if (!curve || !curve->name || !curve->slug || !curve->summary
        || curve->point_count == 0 || curve->point_count > NINEFAN_MAX_POINTS
        || !isfinite(curve->activation_c)
        || !isfinite(curve->release_hysteresis_c)
        || curve->release_hysteresis_c < 0.0f) {
        return 0;
    }
    float previous_temperature = -INFINITY;
    float previous_fraction = 0.0f;
    for (size_t index = 0; index < curve->point_count; index++) {
        const ninefan_curve_point point = curve->points[index];
        if (!isfinite(point.temperature_c) || !isfinite(point.fan_fraction)) return 0;
        if (index > 0 && point.temperature_c <= previous_temperature) return 0;
        if (point.fan_fraction < 0.0f || point.fan_fraction > 1.0f) return 0;
        if (index > 0 && point.fan_fraction < previous_fraction) return 0;
        previous_temperature = point.temperature_c;
        previous_fraction = point.fan_fraction;
    }
    return curve->points[0].temperature_c == curve->activation_c
        && curve->points[curve->point_count - 1].fan_fraction == 1.0f;
}
