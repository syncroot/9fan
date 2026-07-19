#include "guard_protocol.h"

ninefan_guard_protocol_action ninefan_guard_protocol_process(
    ninefan_guard_protocol_state *state,
    char byte) {
    if (!state) return NINEFAN_GUARD_INVALID;
    switch (byte) {
        case NINEFAN_GUARD_HEARTBEAT_BYTE:
            return NINEFAN_GUARD_CONTINUE;
        case NINEFAN_GUARD_ARM_BYTE:
            state->armed = 1;
            return NINEFAN_GUARD_CONTINUE;
        case NINEFAN_GUARD_DISARM_BYTE:
            state->armed = 0;
            return NINEFAN_GUARD_CONTINUE;
        case NINEFAN_GUARD_MAXIMUM_BYTE:
            return NINEFAN_GUARD_LIMIT_MAXIMUM;
        case NINEFAN_GUARD_CLEAN_BYTE:
            return state->armed
                ? NINEFAN_GUARD_CLEAN_RESTORE
                : NINEFAN_GUARD_CLEAN_NO_RESTORE;
        default:
            return NINEFAN_GUARD_INVALID;
    }
}
