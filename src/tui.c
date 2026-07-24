#include "tui.h"

#include "curve.h"
#include "version.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TUI_MAX_COLUMNS 88u
#define TUI_COMPACT_COLUMNS 48u

#define ANSI_RESET "\033[0m"
#define ANSI_HEADER "\033[30;46;1m"
#define ANSI_SECTION "\033[36;1m"
#define ANSI_SELECTED "\033[30;46;1m"
#define ANSI_GOOD "\033[32;1m"
#define ANSI_WARNING "\033[33;1m"
#define ANSI_DIM "\033[2m"

typedef struct {
    char *output;
    size_t capacity;
    size_t length;
    unsigned columns;
    int color;
    int failed;
} tui_buffer;

static void append_bytes(
    tui_buffer *buffer, const char *text, size_t length) {
    if (!buffer || buffer->failed || !text) return;
    if (buffer->length >= buffer->capacity
        || length >= buffer->capacity - buffer->length) {
        buffer->failed = 1;
        return;
    }
    memcpy(buffer->output + buffer->length, text, length);
    buffer->length += length;
    buffer->output[buffer->length] = '\0';
}

static void append_text(tui_buffer *buffer, const char *text) {
    if (!text) return;
    append_bytes(buffer, text, strlen(text));
}

static void format_text(
    char *output, size_t output_size, const char *format, ...) {
    if (!output || output_size == 0 || !format) return;
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
    if (result < 0) output[0] = '\0';
    output[output_size - 1] = '\0';
}

static void append_line(
    tui_buffer *buffer,
    const char *style,
    const char *text,
    int pad_to_width) {
    if (!buffer || buffer->failed || !text) return;
    size_t visible = strnlen(text, buffer->columns);
    if (buffer->color && style) append_text(buffer, style);
    append_bytes(buffer, text, visible);
    if (pad_to_width) {
        static const char spaces[] =
            "                                                                "
            "                        ";
        while (visible < buffer->columns) {
            const size_t remaining = buffer->columns - visible;
            const size_t count =
                remaining < sizeof(spaces) - 1
                    ? remaining
                    : sizeof(spaces) - 1;
            append_bytes(buffer, spaces, count);
            visible += count;
        }
    }
    if (buffer->color && style) append_text(buffer, ANSI_RESET);
    append_text(buffer, "\r\n");
}

static void append_rule(tui_buffer *buffer) {
    char rule[TUI_MAX_COLUMNS + 1];
    memset(rule, '-', buffer->columns);
    rule[buffer->columns] = '\0';
    append_line(buffer, ANSI_DIM, rule, 0);
}

static const char *fan_mode_name(uint8_t mode) {
    switch (mode) {
        case 0: return "auto";
        case 1: return "manual";
        case 3: return "system";
        default: return "other";
    }
}

static const char *profile_name(uint8_t selected_curve) {
    if (selected_curve >= NINEFAN_COMMAND_QUIET
        && selected_curve <= NINEFAN_COMMAND_MAXIMUM) {
        return ninefan_curves[selected_curve - 1].name;
    }
    return "Apple default";
}

static void append_header(tui_buffer *buffer) {
    char header[TUI_MAX_COLUMNS + 1];
    const char *right =
        buffer->columns >= 72
            ? "PRIVILEGE-SEPARATED"
            : buffer->columns >= TUI_COMPACT_COLUMNS
                ? "SAFE CONTROL"
                : "SAFE";
    char left[32];
    format_text(left, sizeof(left), " 9fan %s", NINEFAN_VERSION);
    const size_t left_length = strlen(left);
    const size_t right_length = strlen(right);
    if (left_length + right_length + 1 <= buffer->columns) {
        const size_t padding =
            buffer->columns - left_length - right_length;
        format_text(header, sizeof(header), "%s%*s%s",
            left, (int)padding, "", right);
    } else {
        format_text(header, sizeof(header), "%s", left);
    }
    append_line(buffer, ANSI_HEADER, header, 1);
}

static void append_status(tui_buffer *buffer, const ninefan_event *event) {
    char line[192];
    append_line(buffer, ANSI_SECTION, " STATUS", 0);
    if (buffer->columns >= TUI_COMPACT_COLUMNS) {
        if (event->temperature_valid) {
            format_text(line, sizeof(line),
                " Thermal  %-10s Hotspot %5.1f C (%s)",
                event->thermal_state, event->hotspot_c,
                event->hottest_key);
        } else {
            format_text(line, sizeof(line),
                " Thermal  %-10s Hotspot unavailable",
                event->thermal_state);
        }
        append_line(buffer, NULL, line, 0);
        if (event->monitor_only) {
            format_text(line, sizeof(line),
                " Profile  %-13s READ-ONLY MONITOR",
                profile_name(event->selected_curve));
        } else {
            format_text(line, sizeof(line),
                " Profile  %-13s Lease %u:%02u  %s",
                profile_name(event->selected_curve),
                event->lease_remaining_seconds / 60,
                event->lease_remaining_seconds % 60,
                event->manual_active ? "MANUAL" : "APPLE AUTO");
        }
        append_line(buffer,
            event->manual_active ? ANSI_WARNING : ANSI_GOOD,
            line, 0);
    } else {
        format_text(line, sizeof(line),
            " Thermal  %s", event->thermal_state);
        append_line(buffer, NULL, line, 0);
        if (event->temperature_valid) {
            format_text(line, sizeof(line),
                " Hotspot  %.1f C (%s)",
                event->hotspot_c, event->hottest_key);
        } else {
            format_text(line, sizeof(line), " Hotspot  unavailable");
        }
        append_line(buffer, NULL, line, 0);
        format_text(line, sizeof(line),
            " Profile  %s", profile_name(event->selected_curve));
        append_line(buffer,
            event->manual_active ? ANSI_WARNING : ANSI_GOOD,
            line, 0);
        if (event->monitor_only) {
            format_text(line, sizeof(line),
                " State    READ-ONLY MONITOR");
        } else {
            format_text(line, sizeof(line),
                " Lease    %u:%02u  %s",
                event->lease_remaining_seconds / 60,
                event->lease_remaining_seconds % 60,
                event->manual_active ? "MANUAL" : "AUTO");
        }
        append_line(buffer,
            event->manual_active ? ANSI_WARNING : ANSI_GOOD,
            line, 0);
    }
}

static void append_fans(tui_buffer *buffer, const ninefan_event *event) {
    char line[192];
    append_line(buffer, ANSI_SECTION, " FANS", 0);
    if (buffer->columns >= TUI_COMPACT_COLUMNS) {
        append_line(buffer, ANSI_DIM,
            " #   ACTUAL  TARGET    RANGE       MODE", 0);
        for (uint8_t index = 0; index < event->fan_count; index++) {
            const ninefan_protocol_fan *fan = &event->fans[index];
            format_text(line, sizeof(line),
                " %u   %5.0f   %5.0f   %4.0f-%-4.0f  %-6s",
                index, fan->actual_rpm, fan->target_rpm,
                fan->minimum_rpm, fan->maximum_rpm,
                fan_mode_name(fan->mode));
            append_line(buffer, NULL, line, 0);
        }
    } else {
        append_line(buffer, ANSI_DIM,
            " #   ACTUAL/TARGET RPM   MODE", 0);
        for (uint8_t index = 0; index < event->fan_count; index++) {
            const ninefan_protocol_fan *fan = &event->fans[index];
            format_text(line, sizeof(line),
                " %u   %5.0f/%-5.0f      %s",
                index, fan->actual_rpm, fan->target_rpm,
                fan_mode_name(fan->mode));
            append_line(buffer, NULL, line, 0);
        }
    }
}

static void option_description(
    char *output, size_t output_size, size_t index) {
    if (index >= ninefan_curve_count) {
        if (output && output_size > 0) output[0] = '\0';
        return;
    }
    const ninefan_curve *curve = &ninefan_curves[index];
    if (curve->activation_c <= 0.0f) {
        format_text(output, output_size, "full SMC-reported speed");
        return;
    }
    const float maximum_c =
        curve->points[curve->point_count - 1].temperature_c;
    format_text(output, output_size,
        "auto <%.0fC | max %.0fC", curve->activation_c, maximum_c);
}

static void append_option(
    tui_buffer *buffer,
    const ninefan_event *event,
    unsigned command,
    const char *key,
    const char *name,
    const char *description) {
    char line[192];
    const int selected = event->selected_curve == command;
    if (buffer->columns >= TUI_COMPACT_COLUMNS && description) {
        format_text(line, sizeof(line),
            "%c [%s] %-13s %s",
            selected ? '>' : ' ', key, name, description);
    } else {
        format_text(line, sizeof(line),
            "%c [%s] %s",
            selected ? '>' : ' ', key, name);
    }
    append_line(buffer,
        selected ? ANSI_SELECTED : NULL,
        line, selected);
}

static void append_profiles(
    tui_buffer *buffer, const ninefan_event *event) {
    char description[64];
    append_line(buffer, ANSI_SECTION, " PROFILES", 0);
    append_option(buffer, event, NINEFAN_COMMAND_DEFAULT,
        "A", "Apple default", "macOS-managed cooling");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        option_description(description, sizeof(description), index);
        char key[2] = {(char)('1' + index), '\0'};
        append_option(buffer, event, (unsigned)(index + 1),
            key, ninefan_curves[index].name, description);
    }
}

static void append_footer(
    tui_buffer *buffer, const ninefan_event *event) {
    append_rule(buffer);
    append_line(buffer, NULL, event->monitor_only
        ? " [Q] Quit read-only monitor"
        : " [Q] Restore Apple control and quit", 0);
    if (event->monitor_only) {
        append_line(buffer, ANSI_DIM,
            " Select 1-4 to request a new authorized safety lease.", 0);
    } else if (buffer->columns >= TUI_COMPACT_COLUMNS) {
        append_line(buffer, ANSI_DIM,
            " Session lease is fixed and cannot be extended.", 0);
    }
    if (event->message[0]) {
        char line[256];
        format_text(line, sizeof(line), " > %s", event->message);
        append_line(buffer,
            event->status == 0 ? ANSI_GOOD : ANSI_WARNING,
            line, 0);
    }
}

unsigned ninefan_tui_terminal_columns(int fd) {
    struct winsize size = {0};
    if (fd >= 0
        && ioctl(fd, TIOCGWINSZ, &size) == 0
        && size.ws_col > 0) {
        return size.ws_col;
    }
    return 80;
}

int ninefan_tui_render(
    char *output,
    size_t output_size,
    const ninefan_event *event,
    unsigned columns,
    int color) {
    if (!output || output_size == 0 || !event) return -1;
    output[0] = '\0';
    if (columns == 0) columns = 80;
    if (columns > TUI_MAX_COLUMNS) columns = TUI_MAX_COLUMNS;

    ninefan_event safe = *event;
    ninefan_protocol_sanitize_text(
        safe.message, sizeof(safe.message));
    ninefan_protocol_sanitize_text(
        safe.thermal_state, sizeof(safe.thermal_state));
    ninefan_protocol_sanitize_text(
        safe.hottest_key, sizeof(safe.hottest_key));

    tui_buffer buffer = {
        .output = output,
        .capacity = output_size,
        .length = 0,
        .columns = columns,
        .color = color != 0,
        .failed = 0,
    };
    append_text(&buffer, "\033[H\033[2J");
    append_header(&buffer);
    append_line(&buffer, NULL, "", 0);
    append_status(&buffer, &safe);
    append_line(&buffer, NULL, "", 0);
    append_fans(&buffer, &safe);
    append_line(&buffer, NULL, "", 0);
    append_profiles(&buffer, &safe);
    append_line(&buffer, NULL, "", 0);
    append_footer(&buffer, &safe);
    if (buffer.failed || buffer.length > (size_t)INT_MAX) {
        output[0] = '\0';
        return -1;
    }
    return (int)buffer.length;
}
