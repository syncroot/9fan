#include "channel.h"
#include "curve.h"
#include "frontend_policy.h"
#include "hot_policy.h"
#include "platform_policy.h"
#include "protocol.h"
#include "signal_guard.h"
#include "smc.h"
#include "thermal_guard.h"
#include "tui.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ENGINE_PATH "/usr/local/libexec/9fan-engine"
#define ENGINE_AUTHORIZATION_TIMEOUT_STEPS 600
#define ENGINE_AUTHORIZATION_POLL_MS 200
#define ENGINE_SHUTDOWN_TIMEOUT_MS 2000
#define MONITOR_INPUT_POLL_MS 200

typedef struct {
    struct termios original;
    int active;
    int raw;
} ninefan_terminal;

typedef struct {
    int exit_code;
    ninefan_exit_reason exit_reason;
    char message[NINEFAN_PROTOCOL_MESSAGE_SIZE];
} ninefan_session_result;

typedef enum {
    NINEFAN_MONITOR_QUIT = 0,
    NINEFAN_MONITOR_START_CONTROL = 1,
    NINEFAN_MONITOR_RESTORE_DEFAULT = 2,
    NINEFAN_MONITOR_ERROR = -1,
} ninefan_monitor_result;

static volatile sig_atomic_t termination_requested;
static ninefan_terminal *active_terminal;

static int run_maintenance_engine(int self_test);

static void terminal_restore(void) {
    if (!active_terminal || !active_terminal->active) return;
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &active_terminal->original);
    static const char sequence[] = "\033[?25h\033[?1049l";
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    active_terminal->active = 0;
    active_terminal->raw = 0;
    active_terminal = NULL;
}

static void request_termination(int signal_number) {
    (void)signal_number;
    termination_requested = 1;
}

static void fatal_signal(int signal_number) {
    terminal_restore();
    signal(signal_number, SIG_DFL);
    (void)kill(getpid(), signal_number);
    _exit(128 + signal_number);
}

static void terminal_enter(ninefan_terminal *terminal) {
    memset(terminal, 0, sizeof(*terminal));
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &terminal->original) != 0) return;
    struct termios raw = terminal->original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
    terminal->active = 1;
    terminal->raw = 1;
    active_terminal = terminal;
    printf("\033[?1049h\033[?25l\033[2J");
    fflush(stdout);
}

static int terminal_suspend_input(ninefan_terminal *terminal) {
    if (!terminal || !terminal->active || !terminal->raw) return -1;
    if (tcsetattr(
            STDIN_FILENO, TCSAFLUSH, &terminal->original) != 0) {
        return -1;
    }
    static const char sequence[] = "\033[?25h";
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    terminal->raw = 0;
    return 0;
}

static int terminal_resume_input(ninefan_terminal *terminal) {
    if (!terminal || !terminal->active || terminal->raw) return -1;
    struct termios raw = terminal->original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    static const char sequence[] = "\033[?25l";
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    terminal->raw = 1;
    return 0;
}

static void terminal_leave(ninefan_terminal *terminal) {
    if (!terminal || !terminal->active) return;
    terminal_restore();
}

static int wait_child_bounded(
    pid_t child, int *status, int timeout_ms) {
    if (child <= 0 || !status || timeout_ms < 0) return -1;
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 50) {
        pid_t result;
        do {
            result = waitpid(child, status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == child) return 1;
        if (result < 0) return -1;
        if (elapsed == timeout_ms) break;
        const int remaining = timeout_ms - elapsed;
        (void)poll(NULL, 0, remaining < 50 ? remaining : 50);
    }
    return 0;
}

static int reap_child_with_escalation(
    pid_t child, int *status, int terminate_first) {
    if (terminate_first) (void)kill(child, SIGTERM);
    int result =
        wait_child_bounded(child, status, ENGINE_SHUTDOWN_TIMEOUT_MS);
    if (result != 0) return result;
    if (!terminate_first) (void)kill(child, SIGTERM);
    result = wait_child_bounded(
        child, status, ENGINE_SHUTDOWN_TIMEOUT_MS);
    if (result != 0) return result;
    (void)kill(child, SIGKILL);
    return wait_child_bounded(
        child, status, ENGINE_SHUTDOWN_TIMEOUT_MS);
}

static int discard_terminal_burst(void) {
    for (int attempt = 0; attempt < 64; attempt++) {
        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0,
        };
        const int result = poll(&descriptor, 1, 25);
        if (result <= 0 || !(descriptor.revents & POLLIN)) return 1;
        unsigned char discarded[64];
        if (read(
                STDIN_FILENO, discarded, sizeof(discarded)) <= 0) {
            return 1;
        }
    }
    return 0;
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
        case 0: return "auto";
        case 1: return "manual";
        case 3: return "system";
        default: return "other";
    }
}

static void print_curves(void) {
    puts("9fan curves (percentages are within each fan's SMC min/max range):");
    puts("  default      Apple's thermal controller; 0 RPM remains available");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        printf("  %-12s %s\n",
            ninefan_curves[index].slug, ninefan_curves[index].summary);
    }
}

static void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "  9fan                                  Interactive selector\n"
        "  9fan quiet|balanced|performance|max   Start a bounded session\n"
        "  9fan CURVE --duration MINUTES         Shorten its safety lease\n"
        "  9fan default                          Restore Apple control\n"
        "  9fan status                           Read-only status\n"
        "  9fan self-test                        Guarded hardware test\n"
        "  9fan self-test --yes                  Confirm non-interactively\n"
        "  9fan curves\n"
        "  9fan --version\n"
        "\nRun 9fan as your normal user. It authorizes only the small control engine.\n");
}

static int trusted_engine_path(char *message, size_t message_size) {
    struct stat prefix = {0}, directory = {0}, engine = {0};
    if (lstat("/usr/local", &prefix) != 0
        || !S_ISDIR(prefix.st_mode)
        || prefix.st_uid != 0
        || (prefix.st_mode & 0777) != 0755
        || lstat("/usr/local/libexec", &directory) != 0
        || !S_ISDIR(directory.st_mode)
        || directory.st_uid != 0
        || (directory.st_mode & 0777) != 0755
        || lstat(ENGINE_PATH, &engine) != 0
        || !S_ISREG(engine.st_mode)
        || engine.st_uid != 0
        || engine.st_nlink != 1
        || (engine.st_mode & 07777) != 0755) {
        snprintf(message, message_size,
            "the root-owned control engine is missing or unsafe; reinstall 9fan");
        return -1;
    }
    return 0;
}

static void print_event_line(ninefan_event *event) {
    ninefan_protocol_sanitize_text(
        event->hottest_key, sizeof(event->hottest_key));
    if (event->temperature_valid) {
        printf("%.1fC %-4s", event->hotspot_c, event->hottest_key);
    } else {
        printf("TEMP-FAIL");
    }
    printf(" curve=%u lease=%us",
        event->selected_curve, event->lease_remaining_seconds);
    for (uint8_t index = 0; index < event->fan_count; index++) {
        const ninefan_protocol_fan *fan = &event->fans[index];
        printf(" F%u=%.0f/%.0fRPM(%s)",
            index, fan->actual_rpm, fan->target_rpm,
            mode_name(fan->mode));
    }
    putchar('\n');
    fflush(stdout);
}

static int parse_duration_minutes(
    const char *text, uint64_t maximum_ms, uint64_t *duration_ms) {
    if (!text || !text[0] || !duration_ms) return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0
        || value > maximum_ms / 60000ULL) {
        return -1;
    }
    *duration_ms = (uint64_t)value * 60000ULL;
    return 0;
}

static int print_status(void) {
    ninefan_smc smc;
    if (ninefan_smc_open(&smc) != 0) {
        fprintf(stderr, "9fan: %s\n", ninefan_smc_error(&smc));
        return 1;
    }
    int result = ninefan_smc_refresh_fans(&smc);
    float hotspot = NAN;
    char hottest_key[5] = "----";
    if (ninefan_smc_hottest_temperature(
            &smc, &hotspot, hottest_key) != 0) {
        result = -1;
    }
    char fingerprint[17] = {0};
    if (ninefan_smc_validation_fingerprint(
            &smc, fingerprint, sizeof(fingerprint)) != 0) {
        result = -1;
    }
    char model[64] = {0}, chip[64] = {0}, os_build[64] = {0};
    size_t model_size = sizeof(model);
    size_t chip_size = sizeof(chip);
    size_t os_size = sizeof(os_build);
    if (sysctlbyname("hw.model", model, &model_size, NULL, 0) != 0) {
        model[0] = '\0';
    }
    if (sysctlbyname(
            "machdep.cpu.brand_string", chip, &chip_size, NULL, 0) != 0) {
        chip[0] = '\0';
    }
    if (sysctlbyname(
            "kern.osversion", os_build, &os_size, NULL, 0) != 0) {
        os_build[0] = '\0';
    }
    model[sizeof(model) - 1] = '\0';
    chip[sizeof(chip) - 1] = '\0';
    os_build[sizeof(os_build) - 1] = '\0';
    const ninefan_platform_identity identity = {
        .model = model,
        .chip = chip,
        .os_build = os_build,
        .fan_count = smc.fan_count,
        .mode_key_format = smc.mode_key_format,
        .schema = fingerprint,
    };
    const int supported =
        ninefan_platform_profile_match(&identity) != NULL;
    printf("9fan %s\n", NINEFAN_VERSION);
    printf("Model:       %s\n", model[0] ? model : "unknown");
    printf("Chip:        %s\n", chip[0] ? chip : "unknown");
    printf("OS build:    %s\n", os_build[0] ? os_build : "unknown");
    printf("Supported:   %s\n",
        supported ? "yes (verified profile)" : "no (read-only/recovery only)");
    printf("Privileges:  normal user (frontend cannot write AppleSMC)\n");
    printf("Mode key:    %s\n", smc.mode_key_format);
    printf("Temp keys:   %zu%s\n", smc.temperature_key_count,
        smc.temperature_keys_saturated ? " (INCOMPLETE)" : "");
    printf("Schema:      %s\n", fingerprint[0] ? fingerprint : "unavailable");
    const ninefan_thermal_state thermal = ninefan_thermal_state_current();
    printf("Thermal:     %s\n", ninefan_thermal_state_name(thermal));
    if (isfinite(hotspot)) {
        printf("Hotspot:     %.1f C (%s)\n", hotspot, hottest_key);
    } else {
        puts("Hotspot:     unavailable");
    }
    for (int index = 0; index < smc.fan_count; index++) {
        const ninefan_fan *fan = &smc.fans[index];
        printf("Fan %d:       %.0f RPM, target %.0f, range %.0f-%.0f, mode %s (%u)\n",
            index, fan->actual_rpm, fan->target_rpm,
            fan->minimum_rpm, fan->maximum_rpm,
            mode_name(fan->mode), fan->mode);
        if (!fan->valid) result = -1;
    }
    const int temperature_keys_saturated =
        smc.temperature_keys_saturated;
    ninefan_smc_close(&smc);
    return result == 0 && !temperature_keys_saturated
        && ninefan_thermal_state_allows_control(thermal) ? 0 : 1;
}

static void render_event(const ninefan_event *event) {
    char frame[NINEFAN_TUI_FRAME_SIZE];
    const int frame_size = ninefan_tui_render(
        frame, sizeof(frame), event,
        ninefan_tui_terminal_columns(STDOUT_FILENO), 1);
    if (frame_size > 0) {
        (void)fwrite(frame, 1, (size_t)frame_size, stdout);
        fflush(stdout);
    }
}

static ninefan_session_result run_engine_session(
    uint16_t initial_command,
    uint64_t duration_ms,
    ninefan_terminal *terminal) {
    ninefan_session_result session = {
        .exit_code = 1,
        .exit_reason = NINEFAN_EXIT_ERROR,
    };
    char error[192] = {0};
    if (trusted_engine_path(error, sizeof(error)) != 0) {
        snprintf(session.message, sizeof(session.message), "%s", error);
        return session;
    }
    ninefan_channel_listener listener = {
        .listener_fd = -1,
    };
    if (ninefan_channel_listener_open(&listener) != 0) {
        snprintf(session.message, sizeof(session.message),
            "Could not create the private engine channel");
        return session;
    }
    const int terminal_was_active =
        terminal && terminal->active;
    if (terminal_was_active
        && terminal_suspend_input(terminal) != 0) {
        snprintf(session.message, sizeof(session.message),
            "Could not prepare the terminal for authorization");
        ninefan_channel_listener_cleanup(&listener);
        return session;
    }
    const pid_t child = fork();
    if (child < 0) {
        snprintf(session.message, sizeof(session.message),
            "Could not start the privileged engine: %s", strerror(errno));
        ninefan_channel_listener_cleanup(&listener);
        if (terminal_was_active) {
            (void)terminal_resume_input(terminal);
        }
        return session;
    }
    if (child == 0) {
        const int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0) _exit(127);
        if (null_input != STDIN_FILENO) close(null_input);
        char initial[16], duration[32];
        snprintf(initial, sizeof(initial), "%u", initial_command);
        snprintf(duration, sizeof(duration), "%llu",
            (unsigned long long)duration_ms);
        execl("/usr/bin/sudo", "sudo", "--", ENGINE_PATH,
            "--session", initial, "--lease-ms", duration,
            "--channel", listener.socket_path, (char *)NULL);
        _exit(127);
    }

    int channel_fd = -1;
    int child_status = 0;
    int child_reaped = 0;
    int child_status_valid = 0;
    for (int attempt = 0;
         attempt < ENGINE_AUTHORIZATION_TIMEOUT_STEPS
            && !termination_requested;
         attempt++) {
        channel_fd = ninefan_channel_accept(
            &listener, 0, ENGINE_AUTHORIZATION_POLL_MS,
            &termination_requested);
        if (channel_fd >= 0) break;
        if (channel_fd != NINEFAN_CHANNEL_ACCEPT_TIMEOUT) break;
        const pid_t wait_result = waitpid(child, &child_status, WNOHANG);
        if (wait_result == child) {
            child_reaped = 1;
            child_status_valid = 1;
            break;
        }
        if (wait_result < 0 && errno != EINTR) {
            if (errno == ECHILD) child_reaped = 1;
            break;
        }
    }
    ninefan_channel_listener_cleanup(&listener);
    if (channel_fd < 0) {
        if (!child_reaped) {
            child_status_valid =
                reap_child_with_escalation(
                    child, &child_status, 1) == 1;
        }
        if (!termination_requested) {
            snprintf(session.message, sizeof(session.message),
                "Privileged engine did not establish a secure channel");
        } else {
            session.exit_reason = NINEFAN_EXIT_TERMINATED;
            snprintf(session.message, sizeof(session.message),
                "Frontend terminated during authorization");
        }
        session.exit_code = child_status_valid && WIFEXITED(child_status)
            ? WEXITSTATUS(child_status)
            : 1;
        if (terminal_was_active
            && terminal_resume_input(terminal) != 0) {
            session.exit_code = 1;
            session.exit_reason = NINEFAN_EXIT_ERROR;
            snprintf(session.message, sizeof(session.message),
                "Could not restore interactive terminal input");
        }
        return session;
    }

    if (terminal) {
        if (terminal_was_active) {
            if (terminal_resume_input(terminal) != 0) {
                ninefan_command quit = {
                    .magic = NINEFAN_PROTOCOL_MAGIC,
                    .version = NINEFAN_PROTOCOL_VERSION,
                    .kind = NINEFAN_COMMAND_QUIT,
                };
                (void)ninefan_protocol_write_full(
                    channel_fd, &quit, sizeof(quit),
                    NINEFAN_PROTOCOL_IO_TIMEOUT_MS, NULL);
                close(channel_fd);
                int status = 0;
                (void)reap_child_with_escalation(child, &status, 1);
                snprintf(session.message, sizeof(session.message),
                    "Could not restore interactive terminal input");
                return session;
            }
        } else {
            terminal_enter(terminal);
            if (!terminal->active) {
                ninefan_command quit = {
                    .magic = NINEFAN_PROTOCOL_MAGIC,
                    .version = NINEFAN_PROTOCOL_VERSION,
                    .kind = NINEFAN_COMMAND_QUIT,
                };
                (void)ninefan_protocol_write_full(
                    channel_fd, &quit, sizeof(quit),
                    NINEFAN_PROTOCOL_IO_TIMEOUT_MS, NULL);
                close(channel_fd);
                int status = 0;
                (void)reap_child_with_escalation(child, &status, 1);
                snprintf(session.message, sizeof(session.message),
                    "Could not enter interactive terminal mode");
                return session;
            }
        }
    }
    int engine_done = 0;
    int suppress_terminal_input = 0;
    char final_message[NINEFAN_PROTOCOL_MESSAGE_SIZE] = {0};
    while (!termination_requested && !engine_done) {
        if (suppress_terminal_input) {
            suppress_terminal_input = !discard_terminal_burst();
        }
        struct pollfd descriptors[2] = {
            {.fd = channel_fd, .events = POLLIN | POLLHUP, .revents = 0},
            {.fd = STDIN_FILENO,
             .events = terminal && terminal->active && terminal->raw
                ? POLLIN
                : 0,
             .revents = 0},
        };
        int poll_result;
        do {
            poll_result = poll(descriptors, 2, 500);
        } while (poll_result < 0 && errno == EINTR && !termination_requested);
        if (poll_result < 0) break;
        if (descriptors[0].revents & POLLIN) {
            ninefan_event event = {0};
            if (ninefan_protocol_read_full(
                    channel_fd, &event, sizeof(event),
                    NINEFAN_PROTOCOL_IO_TIMEOUT_MS,
                    &termination_requested) != 0
                || !ninefan_protocol_event_valid(&event)) {
                break;
            }
            if (event.kind == NINEFAN_EVENT_SNAPSHOT) {
                if (terminal && terminal->active && terminal->raw) {
                    render_event(&event);
                }
                else print_event_line(&event);
            } else {
                ninefan_protocol_sanitize_text(
                    event.message, sizeof(event.message));
                if (event.kind == NINEFAN_EVENT_EXIT) {
                    snprintf(final_message, sizeof(final_message), "%s",
                        event.message);
                    session.exit_code = event.status;
                    session.exit_reason =
                        (ninefan_exit_reason)event.exit_reason;
                    engine_done = 1;
                } else if (event.message[0]) {
                    FILE *stream = event.status == 0 ? stdout : stderr;
                    fprintf(stream, "9fan: %s\n", event.message);
                }
            }
        }
        if (terminal && terminal->active && terminal->raw
            && (descriptors[1].revents & POLLIN)) {
            unsigned char input[64] = {0};
            const ssize_t input_size =
                read(STDIN_FILENO, input, sizeof(input));
            if (input_size == 1 && input[0] != 27) {
                ninefan_command command;
                if (ninefan_frontend_command_for_key(input[0], &command)
                    && ninefan_protocol_write_full(
                           channel_fd, &command, sizeof(command),
                           NINEFAN_PROTOCOL_IO_TIMEOUT_MS,
                           &termination_requested) != 0) {
                    break;
                }
            } else if (input_size > 0) {
                suppress_terminal_input = !discard_terminal_burst();
            }
        }
        if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    if (!engine_done) {
        ninefan_command quit = {
            .magic = NINEFAN_PROTOCOL_MAGIC,
            .version = NINEFAN_PROTOCOL_VERSION,
            .kind = NINEFAN_COMMAND_QUIT,
        };
        (void)ninefan_protocol_write_full(
            channel_fd, &quit, sizeof(quit),
            NINEFAN_PROTOCOL_IO_TIMEOUT_MS, NULL);
    }
    close(channel_fd);
    int status = 0;
    const int child_wait_result =
        reap_child_with_escalation(child, &status, !engine_done);
    if (!engine_done) {
        if (child_wait_result == 1 && WIFEXITED(status)) {
            session.exit_code = WEXITSTATUS(status);
        }
        else session.exit_code = 1;
        session.exit_reason = termination_requested
            ? NINEFAN_EXIT_TERMINATED
            : NINEFAN_EXIT_ERROR;
        if (!final_message[0]) {
            snprintf(final_message, sizeof(final_message), "%s",
                termination_requested
                    ? "Frontend terminated; Apple control recovery requested"
                    : "Privileged engine channel closed; "
                      "Apple control recovery requested");
        }
    } else if (child_wait_result != 1
        || !WIFEXITED(status)
        || WEXITSTATUS(status) != session.exit_code) {
        session.exit_code = 1;
        session.exit_reason = NINEFAN_EXIT_ERROR;
        snprintf(final_message, sizeof(final_message),
            "Privileged engine exit did not match its final event; "
            "Apple control recovery requested");
    }
    if (final_message[0]) {
        snprintf(session.message, sizeof(session.message), "%s",
            final_message);
    }
    return session;
}

static long long frontend_monotonic_milliseconds(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    if (now.tv_sec > LLONG_MAX / 1000LL) return LLONG_MAX;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void initialize_monitor_event(
    ninefan_event *event,
    const char *message) {
    if (!event) return;
    *event = (ninefan_event) {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = NINEFAN_EVENT_SNAPSHOT,
        .hotspot_c = NAN,
        .requested_fraction = NAN,
        .selected_curve = NINEFAN_COMMAND_DEFAULT,
        .monitor_only = 1,
    };
    const ninefan_thermal_state thermal_state =
        ninefan_thermal_state_current();
    snprintf(event->thermal_state, sizeof(event->thermal_state), "%s",
        ninefan_thermal_state_name(thermal_state));
    if (message) {
        snprintf(event->message, sizeof(event->message), "%s", message);
    }
}

static int refresh_monitor_event(
    ninefan_smc *smc,
    ninefan_event *event,
    const char *message) {
    if (!smc || !event) return -1;
    initialize_monitor_event(event, message);
    if (!smc->is_open && ninefan_smc_open(smc) != 0) {
        event->status = 1;
        snprintf(event->message, sizeof(event->message),
            "Read-only telemetry unavailable: %s",
            ninefan_smc_error(smc));
        return -1;
    }
    if (ninefan_smc_refresh_fans(smc) != 0) {
        event->status = 1;
        snprintf(event->message, sizeof(event->message),
            "Read-only fan telemetry failed: %s",
            ninefan_smc_error(smc));
        ninefan_smc_close(smc);
        return -1;
    }

    event->fan_count = (uint8_t)smc->fan_count;
    for (int index = 0;
         index < smc->fan_count && index < NINEFAN_MAX_FANS;
         index++) {
        const ninefan_fan *fan = &smc->fans[index];
        event->fans[index] = (ninefan_protocol_fan) {
            .actual_rpm = fan->actual_rpm,
            .target_rpm = fan->target_rpm,
            .minimum_rpm = fan->minimum_rpm,
            .maximum_rpm = fan->maximum_rpm,
            .mode = fan->mode,
            .valid = (uint8_t)(fan->valid != 0),
        };
        if (fan->mode == 1) event->manual_active = 1;
    }

    float hotspot = NAN;
    char hottest_key[5] = "----";
    if (ninefan_smc_hottest_temperature(
            smc, &hotspot, hottest_key) == 0
        && isfinite(hotspot)) {
        event->hotspot_c = hotspot;
        event->temperature_valid = 1;
        memcpy(event->hottest_key, hottest_key, 5);
    } else {
        event->status = 1;
        snprintf(event->message, sizeof(event->message),
            "Read-only temperature telemetry failed: %s",
            ninefan_smc_error(smc));
    }
    if (event->manual_active) {
        event->status = 1;
        snprintf(event->message, sizeof(event->message),
            "Read-only monitor detected manual fan mode outside this session");
    }
    return event->status == 0 ? 0 : -1;
}

static ninefan_monitor_result run_read_only_monitor(
    ninefan_terminal *terminal,
    const char *initial_message,
    uint16_t *selected_command) {
    if (!terminal || !terminal->active || !terminal->raw
        || !selected_command) {
        return NINEFAN_MONITOR_ERROR;
    }
    *selected_command = NINEFAN_COMMAND_DEFAULT;

    ninefan_smc smc = {0};
    ninefan_event event = {0};
    char message[NINEFAN_PROTOCOL_MESSAGE_SIZE] = {0};
    snprintf(message, sizeof(message), "%s",
        initial_message && initial_message[0]
            ? initial_message
            : "Read-only monitor active; select 1-4 to authorize control");
    long long next_sample_ms = 0;
    int suppress_terminal_input = 0;
    ninefan_monitor_result result = NINEFAN_MONITOR_QUIT;

    while (!termination_requested) {
        if (suppress_terminal_input) {
            suppress_terminal_input = !discard_terminal_burst();
        }
        const long long now_ms = frontend_monotonic_milliseconds();
        if (now_ms < 0) {
            result = NINEFAN_MONITOR_ERROR;
            break;
        }
        if (now_ms >= next_sample_ms) {
            (void)refresh_monitor_event(&smc, &event, message);
            render_event(&event);
            next_sample_ms = now_ms
                + ninefan_hot_policy_sample_interval_ms(
                    event.temperature_valid, event.hotspot_c);
        }

        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        int poll_result;
        do {
            poll_result = poll(
                &descriptor, 1, MONITOR_INPUT_POLL_MS);
        } while (poll_result < 0 && errno == EINTR
            && !termination_requested);
        if (poll_result < 0) {
            result = termination_requested
                ? NINEFAN_MONITOR_QUIT
                : NINEFAN_MONITOR_ERROR;
            break;
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN)) {
            unsigned char input[64] = {0};
            const ssize_t input_size =
                read(STDIN_FILENO, input, sizeof(input));
            if (input_size == 1 && input[0] != 27) {
                ninefan_command command;
                if (ninefan_frontend_command_for_key(
                        input[0], &command)) {
                    if (command.kind == NINEFAN_COMMAND_QUIT) {
                        result = NINEFAN_MONITOR_QUIT;
                        break;
                    }
                    if (command.kind == NINEFAN_COMMAND_DEFAULT) {
                        event.status = 0;
                        snprintf(event.message, sizeof(event.message),
                            "Authorizing Apple restore; "
                            "sudo may request your password");
                        render_event(&event);
                        result = NINEFAN_MONITOR_RESTORE_DEFAULT;
                        break;
                    } else {
                        event.status = 0;
                        snprintf(event.message, sizeof(event.message),
                            "Authorizing %s; "
                            "sudo may request your password",
                            ninefan_curves[command.kind - 1].name);
                        render_event(&event);
                        *selected_command = command.kind;
                        result = NINEFAN_MONITOR_START_CONTROL;
                        break;
                    }
                }
            } else if (input_size > 0) {
                suppress_terminal_input = !discard_terminal_burst();
            }
        }
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            result = NINEFAN_MONITOR_ERROR;
            break;
        }
    }

    ninefan_smc_close(&smc);
    return result;
}

static int run_interactive_maintenance_engine(
    ninefan_terminal *terminal) {
    if (terminal_suspend_input(terminal) != 0) return 1;
    const int result = run_maintenance_engine(0);
    if (terminal_resume_input(terminal) != 0) return 1;
    return result;
}

static int report_session_result(
    const ninefan_session_result *session) {
    if (!session) return 1;
    if (session->message[0]) {
        FILE *stream = session->exit_code == 0 ? stdout : stderr;
        fprintf(stream, "9fan: %s\n", session->message);
    }
    return session->exit_code;
}

static int run_interactive_frontend(
    uint16_t initial_command,
    uint64_t initial_duration_ms,
    int start_with_control) {
    uint16_t pending_command = initial_command;
    uint64_t pending_duration_ms = initial_duration_ms;
    char monitor_message[NINEFAN_PROTOCOL_MESSAGE_SIZE] = {0};
    int should_start = start_with_control;
    ninefan_terminal terminal = {0};
    ninefan_session_result final_session = {0};
    int report_final_session = 0;
    int result = 0;

    if (!start_with_control) {
        terminal_enter(&terminal);
        if (!terminal.active) {
            fprintf(stderr,
                "9fan: could not enter interactive terminal mode\n");
            return 1;
        }
    }

    for (;;) {
        if (should_start) {
            const ninefan_session_result session =
                run_engine_session(
                    pending_command, pending_duration_ms, &terminal);
            if (session.exit_reason == NINEFAN_EXIT_USER_QUIT
                || session.exit_reason == NINEFAN_EXIT_TERMINATED
                || termination_requested) {
                final_session = session;
                report_final_session = 1;
                result = session.exit_code;
                break;
            }
            if (!ninefan_frontend_returns_to_monitor(
                    session.exit_reason)) {
                final_session = session;
                report_final_session = 1;
                result = session.exit_code;
                break;
            }
            snprintf(monitor_message, sizeof(monitor_message), "%s",
                session.exit_reason == NINEFAN_EXIT_LEASE_EXPIRED
                    ? "Safety lease ended; Apple control is active. "
                      "Select 1-4 for fresh authorization"
                    : session.message[0]
                        ? session.message
                        : "Control engine ended; read-only monitor is active");
        } else if (!monitor_message[0]) {
            snprintf(monitor_message, sizeof(monitor_message),
                "Read-only monitor active; select 1-4 to authorize control");
        }

        if (!terminal.active) {
            terminal_enter(&terminal);
            if (!terminal.active) {
                result = 1;
                break;
            }
        }
        const ninefan_monitor_result monitor_result =
            run_read_only_monitor(
                &terminal, monitor_message, &pending_command);
        if (monitor_result == NINEFAN_MONITOR_QUIT) {
            result = 0;
            break;
        }
        if (monitor_result == NINEFAN_MONITOR_RESTORE_DEFAULT) {
            const int restore_result =
                run_interactive_maintenance_engine(&terminal);
            snprintf(monitor_message, sizeof(monitor_message), "%s",
                restore_result == 0
                    ? "Apple automatic control restored; "
                      "read-only monitor is active"
                    : "Apple automatic restore failed; "
                      "read-only monitor is active");
            should_start = 0;
            continue;
        }
        if (monitor_result != NINEFAN_MONITOR_START_CONTROL) {
            result = 1;
            break;
        }
        pending_duration_ms =
            ninefan_frontend_default_lease_ms(pending_command);
        should_start = 1;
    }
    terminal_leave(&terminal);
    if (report_final_session) {
        return report_session_result(&final_session);
    }
    return result;
}

static int confirm_self_test(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    printf("This briefly raises both allowlisted fans to their reported maximum, "
           "verifies response, and restores Apple control.\nContinue? [y/N] ");
    fflush(stdout);
    char answer[8] = {0};
    return fgets(answer, sizeof(answer), stdin)
        && (answer[0] == 'y' || answer[0] == 'Y');
}

static int run_maintenance_engine(int self_test) {
    char error[192] = {0};
    if (trusted_engine_path(error, sizeof(error)) != 0) {
        fprintf(stderr, "9fan: %s\n", error);
        return 1;
    }
    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "9fan: could not start engine: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        if (self_test) {
            execl("/usr/bin/sudo", "sudo", "--", ENGINE_PATH,
                "--self-test", "--yes", "--lease-ms", "120000",
                (char *)NULL);
        } else {
            execl("/usr/bin/sudo", "sudo", "--", ENGINE_PATH,
                "--default", (char *)NULL);
        }
        _exit(127);
    }
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, 0);
        if (waited == child) break;
        if (waited < 0 && errno == EINTR) {
            if (termination_requested) (void)kill(child, SIGTERM);
            continue;
        }
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv) {
    if (geteuid() == 0) {
        fprintf(stderr,
            "9fan: run the frontend without sudo; it authorizes only the "
            "minimal engine when control is requested\n");
        return 2;
    }
    if (ninefan_signal_guard_install(
            request_termination, fatal_signal) != 0) {
        fprintf(stderr, "9fan: could not install signal handlers\n");
        return 1;
    }
    atexit(terminal_restore);

    if (argc == 2
        && (strcmp(argv[1], "--help") == 0
            || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 2
        && (strcmp(argv[1], "--version") == 0
            || strcmp(argv[1], "-v") == 0)) {
        puts("9fan " NINEFAN_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "curves") == 0) {
        print_curves();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        return print_status();
    }
    if (argc == 1
        && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        return print_status();
    }

    if (argc >= 2 && strcmp(argv[1], "self-test") == 0) {
        if (argc > 3
            || (argc == 3 && strcmp(argv[2], "--yes") != 0)) {
            print_usage(stderr);
            return 2;
        }
        const int confirmed =
            argc == 3 && strcmp(argv[2], "--yes") == 0;
        if (!confirmed && !confirm_self_test()) {
            fprintf(stderr,
                "9fan: self-test cancelled; use --yes only for intentional "
                "non-interactive operation\n");
            return 1;
        }
        return run_maintenance_engine(1);
    }

    uint16_t initial = NINEFAN_COMMAND_DEFAULT;
    uint64_t duration_ms = 30ULL * 60ULL * 1000ULL;
    if (argc >= 2) {
        if (strcmp(argv[1], "default") == 0) {
            if (argc != 2) {
                print_usage(stderr);
                return 2;
            }
            return run_maintenance_engine(0);
        }
        const ninefan_curve *curve = ninefan_curve_named(argv[1]);
        if (!curve) {
            fprintf(stderr, "9fan: unknown command or curve '%s'\n", argv[1]);
            print_usage(stderr);
            return 2;
        }
        initial = (uint16_t)(curve - ninefan_curves + 1);
        if (initial == NINEFAN_COMMAND_MAXIMUM) {
            duration_ms = 10ULL * 60ULL * 1000ULL;
        }
    }
    if (argc == 2
        && initial != NINEFAN_COMMAND_DEFAULT
        && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr,
            "9fan: non-interactive curve sessions require --duration\n");
        return 2;
    }
    if (argc == 4 && strcmp(argv[2], "--duration") == 0) {
        const uint64_t maximum =
            initial == NINEFAN_COMMAND_MAXIMUM
                ? 10ULL * 60ULL * 1000ULL
                : 30ULL * 60ULL * 1000ULL;
        if (parse_duration_minutes(argv[3], maximum, &duration_ms) != 0) {
            fprintf(stderr,
                "9fan: duration must be whole minutes from 1 to %llu\n",
                (unsigned long long)(maximum / 60000ULL));
            return 2;
        }
    } else if (argc != 1 && argc != 2) {
        print_usage(stderr);
        return 2;
    }
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        return run_interactive_frontend(
            initial, duration_ms,
            initial != NINEFAN_COMMAND_DEFAULT);
    }
    const ninefan_session_result session =
        run_engine_session(initial, duration_ms, NULL);
    return report_session_result(&session);
}
