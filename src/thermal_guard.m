#include "thermal_guard.h"

#import <Foundation/Foundation.h>

ninefan_thermal_state ninefan_thermal_state_current(void) {
    @autoreleasepool {
        switch ([NSProcessInfo processInfo].thermalState) {
            case NSProcessInfoThermalStateNominal:
                return NINEFAN_THERMAL_NOMINAL;
            case NSProcessInfoThermalStateFair:
                return NINEFAN_THERMAL_FAIR;
            case NSProcessInfoThermalStateSerious:
                return NINEFAN_THERMAL_SERIOUS;
            case NSProcessInfoThermalStateCritical:
                return NINEFAN_THERMAL_CRITICAL;
        }
    }
    return NINEFAN_THERMAL_UNKNOWN;
}

const char *ninefan_thermal_state_name(ninefan_thermal_state state) {
    switch (state) {
        case NINEFAN_THERMAL_NOMINAL:
            return "nominal";
        case NINEFAN_THERMAL_FAIR:
            return "fair";
        case NINEFAN_THERMAL_SERIOUS:
            return "serious";
        case NINEFAN_THERMAL_CRITICAL:
            return "critical";
        case NINEFAN_THERMAL_UNKNOWN:
            return "unknown";
    }
    return "unknown";
}

int ninefan_thermal_state_allows_control(ninefan_thermal_state state) {
    return state == NINEFAN_THERMAL_NOMINAL
        || state == NINEFAN_THERMAL_FAIR;
}
