#ifndef NINEFAN_THERMAL_GUARD_H
#define NINEFAN_THERMAL_GUARD_H

typedef enum {
    NINEFAN_THERMAL_UNKNOWN = -1,
    NINEFAN_THERMAL_NOMINAL = 0,
    NINEFAN_THERMAL_FAIR = 1,
    NINEFAN_THERMAL_SERIOUS = 2,
    NINEFAN_THERMAL_CRITICAL = 3,
} ninefan_thermal_state;

ninefan_thermal_state ninefan_thermal_state_current(void);
const char *ninefan_thermal_state_name(ninefan_thermal_state state);
int ninefan_thermal_state_allows_control(ninefan_thermal_state state);

#endif
