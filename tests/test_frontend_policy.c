#include "../src/frontend_policy.h"
#include "../src/lease.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    ninefan_command command = {0};
    assert(ninefan_frontend_command_for_key('a', &command));
    assert(command.kind == NINEFAN_COMMAND_DEFAULT);
    assert(ninefan_frontend_command_for_key('A', &command));
    assert(command.kind == NINEFAN_COMMAND_DEFAULT);
    assert(ninefan_frontend_command_for_key('0', &command));
    assert(command.kind == NINEFAN_COMMAND_DEFAULT);
    for (unsigned char key = '1'; key <= '4'; key++) {
        assert(ninefan_frontend_command_for_key(key, &command));
        assert(command.kind == (uint16_t)(key - '0'));
    }
    assert(ninefan_frontend_command_for_key('q', &command));
    assert(command.kind == NINEFAN_COMMAND_QUIT);
    assert(ninefan_frontend_command_for_key('Q', &command));
    assert(command.kind == NINEFAN_COMMAND_QUIT);
    assert(ninefan_frontend_command_for_key(3, &command));
    assert(command.kind == NINEFAN_COMMAND_QUIT);
    assert(!ninefan_frontend_command_for_key('x', &command));
    assert(!ninefan_frontend_command_for_key('1', NULL));

    assert(ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_LEASE_EXPIRED));
    assert(ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_MONITOR_REQUESTED));
    assert(ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_ERROR));
    assert(!ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_USER_QUIT));
    assert(!ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_TERMINATED));
    assert(!ninefan_frontend_returns_to_monitor(
        NINEFAN_EXIT_NONE));

    assert(ninefan_frontend_default_lease_ms(
        NINEFAN_COMMAND_BALANCED)
        == NINEFAN_CURVE_LEASE_DEFAULT_MS);
    assert(ninefan_frontend_default_lease_ms(
        NINEFAN_COMMAND_MAXIMUM)
        == NINEFAN_MAX_CURVE_LEASE_MAX_MS);

    puts("frontend policy tests passed");
    return 0;
}
