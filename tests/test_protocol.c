#include "../src/protocol.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    ninefan_command command = {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = NINEFAN_COMMAND_BALANCED,
    };
    assert(ninefan_protocol_command_valid(&command));
    command.kind = 99;
    assert(!ninefan_protocol_command_valid(&command));

    ninefan_event event = {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = NINEFAN_EVENT_SNAPSHOT,
        .fan_count = 2,
        .selected_curve = NINEFAN_COMMAND_QUIET,
    };
    assert(ninefan_protocol_event_valid(&event));
    event.fan_count = NINEFAN_MAX_FANS + 1;
    assert(!ninefan_protocol_event_valid(&event));
    event.fan_count = 2;
    event.kind = NINEFAN_EVENT_EXIT;
    assert(!ninefan_protocol_event_valid(&event));
    event.exit_reason = NINEFAN_EXIT_LEASE_EXPIRED;
    assert(ninefan_protocol_event_valid(&event));
    event.monitor_only = 1;
    assert(!ninefan_protocol_event_valid(&event));
    event.monitor_only = 2;
    assert(!ninefan_protocol_event_valid(&event));
    event.monitor_only = 0;
    event.reserved[0] = 1;
    assert(!ninefan_protocol_event_valid(&event));
    event.reserved[0] = 0;
    event.kind = NINEFAN_EVENT_SNAPSHOT;
    assert(!ninefan_protocol_event_valid(&event));
    event.exit_reason = NINEFAN_EXIT_NONE;
    event.monitor_only = 1;
    assert(ninefan_protocol_event_valid(&event));
    event.status = 2;
    assert(!ninefan_protocol_event_valid(&event));
    event.status = 0;
    event.exit_reason = NINEFAN_EXIT_TERMINATED + 1;
    assert(!ninefan_protocol_event_valid(&event));
    event.exit_reason = NINEFAN_EXIT_NONE;

    char text[8] = {'o', 'k', '\033', 'x', '\0'};
    ninefan_protocol_sanitize_text(text, sizeof(text));
    assert(strcmp(text, "ok?x") == 0);

    int descriptors[2];
    assert(pipe(descriptors) == 0);
    command.kind = NINEFAN_COMMAND_MAXIMUM;
    assert(ninefan_protocol_write_full(
        descriptors[1], &command, sizeof(command), 100, NULL) == 0);
    ninefan_command received = {0};
    assert(ninefan_protocol_read_full(
        descriptors[0], &received, sizeof(received), 100, NULL) == 0);
    assert(received.kind == NINEFAN_COMMAND_MAXIMUM);
    const unsigned char partial = 0x39;
    assert(write(descriptors[1], &partial, 1) == 1);
    assert(ninefan_protocol_read_full(
        descriptors[0], &received, sizeof(received), 10, NULL) != 0);
    volatile sig_atomic_t abort_requested = 1;
    assert(ninefan_protocol_read_full(
        descriptors[0], &received, sizeof(received), 100,
        &abort_requested) != 0);
    unsigned char oversized[NINEFAN_PROTOCOL_MAX_FRAME_SIZE + 1] = {0};
    assert(ninefan_protocol_write_full(
        descriptors[1], oversized, sizeof(oversized), 100, NULL) != 0);
    close(descriptors[0]);
    close(descriptors[1]);
    puts("protocol tests passed");
    return 0;
}
