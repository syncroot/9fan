#ifndef NINEFAN_CURVE_H
#define NINEFAN_CURVE_H

#include <stddef.h>

#define NINEFAN_MAX_POINTS 5

typedef struct {
    float temperature_c;
    float fan_fraction;
} ninefan_curve_point;

typedef struct {
    const char *name;
    const char *slug;
    const char *summary;
    float activation_c;
    float release_hysteresis_c;
    ninefan_curve_point points[NINEFAN_MAX_POINTS];
    size_t point_count;
} ninefan_curve;

extern const ninefan_curve ninefan_curves[];
extern const size_t ninefan_curve_count;

const ninefan_curve *ninefan_curve_named(const char *name);
float ninefan_curve_fraction(const ninefan_curve *curve, float temperature_c);
int ninefan_curve_is_valid(const ninefan_curve *curve);

#endif
