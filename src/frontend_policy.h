#ifndef NINEFAN_FRONTEND_POLICY_H
#define NINEFAN_FRONTEND_POLICY_H

#include "protocol.h"

#include <stdint.h>

int ninefan_frontend_command_for_key(
    unsigned char key,
    ninefan_command *command);
int ninefan_frontend_returns_to_monitor(
    ninefan_exit_reason reason);
uint64_t ninefan_frontend_default_lease_ms(
    uint16_t command);

#endif
