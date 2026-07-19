#ifndef NINEFAN_TUI_H
#define NINEFAN_TUI_H

#include "protocol.h"

#include <stddef.h>

#define NINEFAN_TUI_FRAME_SIZE 8192

unsigned ninefan_tui_terminal_columns(int fd);

int ninefan_tui_render(
    char *output,
    size_t output_size,
    const ninefan_event *event,
    unsigned columns,
    int color);

#endif
