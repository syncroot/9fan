#ifndef NINEFAN_GUARD_PROTOCOL_H
#define NINEFAN_GUARD_PROTOCOL_H

#define NINEFAN_GUARD_PROTOCOL_TEXT "4"
#define NINEFAN_GUARD_HEARTBEAT_BYTE 'H'
#define NINEFAN_GUARD_CLEAN_BYTE 'C'
#define NINEFAN_GUARD_ARM_BYTE 'A'
#define NINEFAN_GUARD_DISARM_BYTE 'D'
#define NINEFAN_GUARD_MAXIMUM_BYTE 'M'
#define NINEFAN_GUARD_HOT_START_BYTE 'T'
#define NINEFAN_GUARD_READY_BYTE 'R'
#define NINEFAN_GUARD_TIMEOUT_MS 6000

typedef struct {
    int armed;
} ninefan_guard_protocol_state;

typedef enum {
    NINEFAN_GUARD_CONTINUE = 0,
    NINEFAN_GUARD_LIMIT_MAXIMUM = 1,
    NINEFAN_GUARD_LIMIT_HOT_START = 2,
    NINEFAN_GUARD_CLEAN_NO_RESTORE = 3,
    NINEFAN_GUARD_CLEAN_RESTORE = 4,
    NINEFAN_GUARD_INVALID = 5,
} ninefan_guard_protocol_action;

ninefan_guard_protocol_action ninefan_guard_protocol_process(
    ninefan_guard_protocol_state *state,
    char byte);

#endif
