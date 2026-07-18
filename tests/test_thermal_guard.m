#include "../src/thermal_guard.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(strcmp(
        ninefan_thermal_state_name(NINEFAN_THERMAL_NOMINAL),
        "nominal") == 0);
    assert(strcmp(
        ninefan_thermal_state_name(NINEFAN_THERMAL_CRITICAL),
        "critical") == 0);
    assert(strcmp(
        ninefan_thermal_state_name(NINEFAN_THERMAL_UNKNOWN),
        "unknown") == 0);
    assert(ninefan_thermal_state_allows_control(
        NINEFAN_THERMAL_NOMINAL));
    assert(ninefan_thermal_state_allows_control(
        NINEFAN_THERMAL_FAIR));
    assert(!ninefan_thermal_state_allows_control(
        NINEFAN_THERMAL_SERIOUS));
    assert(!ninefan_thermal_state_allows_control(
        NINEFAN_THERMAL_CRITICAL));
    assert(!ninefan_thermal_state_allows_control(
        NINEFAN_THERMAL_UNKNOWN));

    const ninefan_thermal_state current =
        ninefan_thermal_state_current();
    assert(current >= NINEFAN_THERMAL_NOMINAL);
    assert(current <= NINEFAN_THERMAL_CRITICAL);

    puts("thermal guard tests passed");
    return 0;
}
