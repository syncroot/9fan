#include "frontend_policy.h"

#include "lease.h"

int ninefan_frontend_command_for_key(
    unsigned char key,
    ninefan_command *command) {
    if (!command) return 0;
    uint16_t kind;
    if (key == 'a' || key == 'A' || key == '0') {
        kind = NINEFAN_COMMAND_DEFAULT;
    } else if (key >= '1' && key <= '4') {
        kind = (uint16_t)(key - '0');
    } else if (key == 'q' || key == 'Q' || key == 3) {
        kind = NINEFAN_COMMAND_QUIT;
    } else {
        return 0;
    }
    *command = (ninefan_command) {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = kind,
    };
    return 1;
}

int ninefan_frontend_returns_to_monitor(
    ninefan_exit_reason reason) {
    return reason == NINEFAN_EXIT_LEASE_EXPIRED
        || reason == NINEFAN_EXIT_MONITOR_REQUESTED
        || reason == NINEFAN_EXIT_ERROR;
}

uint64_t ninefan_frontend_default_lease_ms(
    uint16_t command) {
    return command == NINEFAN_COMMAND_MAXIMUM
        ? NINEFAN_MAX_CURVE_LEASE_MAX_MS
        : NINEFAN_CURVE_LEASE_DEFAULT_MS;
}
