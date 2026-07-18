#ifndef NINEFAN_PROTOCOL_H
#define NINEFAN_PROTOCOL_H

#include "smc.h"

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#define NINEFAN_PROTOCOL_MAGIC 0x3946414eu
#define NINEFAN_PROTOCOL_VERSION 1u
#define NINEFAN_PROTOCOL_MESSAGE_SIZE 192
#define NINEFAN_PROTOCOL_IO_TIMEOUT_MS 2000

typedef enum {
    NINEFAN_COMMAND_DEFAULT = 0,
    NINEFAN_COMMAND_QUIET = 1,
    NINEFAN_COMMAND_BALANCED = 2,
    NINEFAN_COMMAND_PERFORMANCE = 3,
    NINEFAN_COMMAND_MAXIMUM = 4,
    NINEFAN_COMMAND_QUIT = 5,
} ninefan_command_kind;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
} ninefan_command;

typedef enum {
    NINEFAN_EVENT_SNAPSHOT = 1,
    NINEFAN_EVENT_MESSAGE = 2,
    NINEFAN_EVENT_EXIT = 3,
} ninefan_event_kind;

typedef struct {
    float actual_rpm;
    float target_rpm;
    float minimum_rpm;
    float maximum_rpm;
    uint8_t mode;
    uint8_t valid;
    uint8_t reserved[2];
} ninefan_protocol_fan;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    int32_t status;
    uint32_t lease_remaining_seconds;
    float hotspot_c;
    float requested_fraction;
    uint8_t temperature_valid;
    uint8_t fan_count;
    uint8_t selected_curve;
    uint8_t manual_active;
    char hottest_key[5];
    char thermal_state[16];
    char message[NINEFAN_PROTOCOL_MESSAGE_SIZE];
    ninefan_protocol_fan fans[NINEFAN_MAX_FANS];
} ninefan_event;

_Static_assert(
    sizeof(ninefan_command) == 8,
    "The fixed command wire format changed");
_Static_assert(
    sizeof(ninefan_protocol_fan) == 20,
    "The fixed fan telemetry wire format changed");
_Static_assert(
    sizeof(ninefan_event) == 404,
    "The fixed event wire format changed");

int ninefan_protocol_command_valid(const ninefan_command *command);
int ninefan_protocol_event_valid(const ninefan_event *event);
int ninefan_protocol_read_full(
    int fd,
    void *buffer,
    size_t size,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested);
int ninefan_protocol_write_full(
    int fd,
    const void *buffer,
    size_t size,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested);
void ninefan_protocol_sanitize_text(char *text, size_t size);

#endif
