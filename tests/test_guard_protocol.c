#include "../src/guard_protocol.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    ninefan_guard_protocol_state state = {0};

    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_HEARTBEAT_BYTE)
        == NINEFAN_GUARD_CONTINUE);
    assert(!state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_CLEAN_BYTE)
        == NINEFAN_GUARD_CLEAN_NO_RESTORE);

    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_ARM_BYTE)
        == NINEFAN_GUARD_CONTINUE);
    assert(state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_MAXIMUM_BYTE)
        == NINEFAN_GUARD_LIMIT_MAXIMUM);
    assert(state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_HOT_START_BYTE)
        == NINEFAN_GUARD_LIMIT_HOT_START);
    assert(state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_CLEAN_BYTE)
        == NINEFAN_GUARD_CLEAN_RESTORE);

    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_DISARM_BYTE)
        == NINEFAN_GUARD_CONTINUE);
    assert(!state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_CLEAN_BYTE)
        == NINEFAN_GUARD_CLEAN_NO_RESTORE);

    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_ARM_BYTE)
        == NINEFAN_GUARD_CONTINUE);
    assert(state.armed);
    assert(ninefan_guard_protocol_process(
        &state, NINEFAN_GUARD_CLEAN_BYTE)
        == NINEFAN_GUARD_CLEAN_RESTORE);

    assert(ninefan_guard_protocol_process(&state, 'X')
        == NINEFAN_GUARD_INVALID);
    assert(ninefan_guard_protocol_process(NULL, 'H')
        == NINEFAN_GUARD_INVALID);

    puts("guard protocol tests passed");
    return 0;
}
