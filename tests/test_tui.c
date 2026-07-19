#include "../src/tui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ninefan_event sample_event(void) {
    ninefan_event event = {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = NINEFAN_EVENT_SNAPSHOT,
        .status = 0,
        .lease_remaining_seconds = 1784,
        .hotspot_c = 48.5f,
        .temperature_valid = 1,
        .fan_count = 2,
        .selected_curve = NINEFAN_COMMAND_PERFORMANCE,
        .manual_active = 1,
    };
    memcpy(event.hottest_key, "Tp0K", 5);
    snprintf(event.thermal_state,
        sizeof(event.thermal_state), "nominal");
    snprintf(event.message, sizeof(event.message),
        "Performance curve selected\033bad");
    for (uint8_t index = 0; index < event.fan_count; index++) {
        event.fans[index] = (ninefan_protocol_fan) {
            .actual_rpm = index == 0 ? 3776.0f : 3858.0f,
            .target_rpm = 3721.0f,
            .minimum_rpm = 2317.0f,
            .maximum_rpm = 7826.0f,
            .mode = 1,
            .valid = 1,
        };
    }
    return event;
}

static void assert_line_widths(
    const char *frame, size_t columns) {
    const char *cursor = strstr(frame, "\033[2J");
    assert(cursor);
    cursor += strlen("\033[2J");
    size_t lines = 0;
    while (*cursor) {
        const char *ending = strstr(cursor, "\r\n");
        assert(ending);
        const size_t width = (size_t)(ending - cursor);
        assert(width <= columns);
        cursor = ending + 2;
        lines++;
    }
    assert(lines <= 30);
}

int main(int argc, char **argv) {
    ninefan_event event = sample_event();
    char frame[NINEFAN_TUI_FRAME_SIZE];
    const int rendered = ninefan_tui_render(
        frame, sizeof(frame), &event, 59, 0);
    assert(rendered > 0);
    assert((size_t)rendered == strlen(frame));
    assert(strstr(frame, "> [3] Performance"));
    assert(strstr(frame, "auto <40C | max 82C"));
    assert(strstr(frame, "Performance curve selected?bad"));
    assert(!strstr(frame, "Apple auto <40C;"));
    assert_line_widths(frame, 59);

    char narrow[NINEFAN_TUI_FRAME_SIZE];
    assert(ninefan_tui_render(
        narrow, sizeof(narrow), &event, 42, 0) > 0);
    assert(strstr(narrow, "> [3] Performance"));
    assert(!strstr(narrow, "auto <40C"));
    assert_line_widths(narrow, 42);

    const unsigned widths[] = {1, 16, 27, 47, 48, 59, 72, 88, 100};
    for (size_t index = 0;
         index < sizeof(widths) / sizeof(widths[0]);
         index++) {
        char responsive[NINEFAN_TUI_FRAME_SIZE];
        assert(ninefan_tui_render(
            responsive, sizeof(responsive), &event,
            widths[index], 0) > 0);
        const size_t expected =
            widths[index] > 88 ? 88 : widths[index];
        assert_line_widths(responsive, expected);
    }

    event.selected_curve = NINEFAN_COMMAND_DEFAULT;
    event.manual_active = 0;
    event.temperature_valid = 0;
    event.message[0] = '\0';
    char automatic[NINEFAN_TUI_FRAME_SIZE];
    assert(ninefan_tui_render(
        automatic, sizeof(automatic), &event, 59, 0) > 0);
    assert(strstr(automatic, "> [A] Apple default"));
    assert(strstr(automatic, "APPLE AUTO"));
    assert(strstr(automatic, "Hotspot unavailable"));
    assert_line_widths(automatic, 59);

    event = sample_event();
    char colored[NINEFAN_TUI_FRAME_SIZE];
    assert(ninefan_tui_render(
        colored, sizeof(colored), &event, 59, 1) > 0);
    assert(strstr(colored, "\033[30;46;1m"));

    char tiny[16];
    assert(ninefan_tui_render(
        tiny, sizeof(tiny), &event, 59, 0) == -1);
    assert(tiny[0] == '\0');
    assert(ninefan_tui_render(
        NULL, sizeof(frame), &event, 80, 0) == -1);
    assert(ninefan_tui_render(
        frame, 0, &event, 80, 0) == -1);
    assert(ninefan_tui_render(
        frame, sizeof(frame), NULL, 80, 0) == -1);
    assert(ninefan_tui_terminal_columns(-1) == 80);

    if (argc == 2 && strcmp(argv[1], "--preview") == 0) {
        fputs(colored, stdout);
        return 0;
    }
    puts("TUI tests passed");
    return 0;
}
