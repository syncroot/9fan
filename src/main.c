#include "channel.h"
#include "curve.h"
#include "platform_policy.h"
#include "protocol.h"
#include "signal_guard.h"
#include "smc.h"
#include "thermal_guard.h"
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
#include <unistd.h>

#define ENGINE_PATH "/usr/local/libexec/9fan-engine"
#define ENGINE_AUTHORIZATION_TIMEOUT_STEPS 600
#define ENGINE_AUTHORIZATION_POLL_MS 200
#define ENGINE_SHUTDOWN_TIMEOUT_MS 2000

typedef struct {
    struct termios original;
    int active;
} ninefan_terminal;

static volatile sig_atomic_t termination_requested;
static ninefan_terminal *active_terminal;

static void terminal_restore(void) {
    if (!active_terminal || !active_terminal->active) return;
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &active_terminal->original);
    static const char sequence[] = "\033[?25h\033[?1049l";
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    active_terminal->active = 0;
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
    active_terminal = terminal;
    printf("\033[?1049h\033[?25l\033[2J");
    fflush(stdout);
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

static void render_event(ninefan_event *event) {
    ninefan_protocol_sanitize_text(event->message, sizeof(event->message));
    ninefan_protocol_sanitize_text(
        event->thermal_state, sizeof(event->thermal_state));
    ninefan_protocol_sanitize_text(
        event->hottest_key, sizeof(event->hottest_key));
    printf("\033[H\033[2J");
    printf("\033[30;46;1m 9fan %-8s  PRIVILEGE-SEPARATED CONTROL \033[0m\r\n",
        NINEFAN_VERSION);
    printf("\r\n  Engine    root-only, fixed commands\r\n");
    printf("  Thermal   %-12s", event->thermal_state);
    if (event->temperature_valid) {
        printf("  Hotspot %.1f C (%s)\r\n",
            event->hotspot_c, event->hottest_key);
    } else {
        printf("  Hotspot unavailable\r\n");
    }
    const char *profile = "Apple default";
    if (event->selected_curve >= NINEFAN_COMMAND_QUIET
        && event->selected_curve <= NINEFAN_COMMAND_MAXIMUM) {
        profile = ninefan_curves[event->selected_curve - 1].name;
    }
    printf("  Profile   %-13s  Lease %u:%02u\r\n\r\n",
        profile,
        event->lease_remaining_seconds / 60,
        event->lease_remaining_seconds % 60);
    for (uint8_t index = 0; index < event->fan_count; index++) {
        const ninefan_protocol_fan *fan = &event->fans[index];
        printf("  Fan %u  %5.0f RPM   target %5.0f   range %4.0f-%4.0f   %-6s\r\n",
            index, fan->actual_rpm, fan->target_rpm,
            fan->minimum_rpm, fan->maximum_rpm, mode_name(fan->mode));
    }
    printf("\r\n  [a] Apple default\r\n");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        printf("  [%zu] %-13s %s\r\n",
            index + 1, ninefan_curves[index].name,
            ninefan_curves[index].summary);
    }
    printf("\r\n  [q] Restore Apple default and quit\r\n");
    printf("  Safety lease cannot be extended during this session.\r\n");
    if (event->message[0]) printf("\r\n  %s\r\n", event->message);
    fflush(stdout);
}

static int key_to_command(unsigned char key, ninefan_command *command) {
    if (!command) return 0;
    uint16_t kind;
    if (key == 'a' || key == '0') {
        kind = NINEFAN_COMMAND_DEFAULT;
    } else if (key >= '1' && key <= '4') {
        kind = (uint16_t)(key - '0');
    } else if (key == 'q' || key == 3) {
        kind = NINEFAN_COMMAND_QUIT;
    } else {
        return 0;
    }
    *command = (ninefan_command) {
        .magic = NINEFAN_PROTOCOL_MAGIC,
        .version = NINEFAN_PROTOCOL_VERSION,
        .kind = kind,
    };
    return 1;
}

static int run_engine_session(
    uint16_t initial_command,
    uint64_t duration_ms) {
    char error[192] = {0};
    if (trusted_engine_path(error, sizeof(error)) != 0) {
        fprintf(stderr, "9fan: %s\n", error);
        return 1;
    }
    ninefan_channel_listener listener = {
        .listener_fd = -1,
    };
    if (ninefan_channel_listener_open(&listener) != 0) {
        fprintf(stderr, "9fan: could not create the private engine channel\n");
        return 1;
    }
    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "9fan: could not start engine: %s\n", strerror(errno));
        ninefan_channel_listener_cleanup(&listener);
        return 1;
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
            fprintf(stderr,
                "9fan: privileged engine did not establish a secure channel\n");
        }
        return child_status_valid && WIFEXITED(child_status)
            ? WEXITSTATUS(child_status)
            : 1;
    }

    ninefan_terminal terminal = {0};
    terminal_enter(&terminal);
    int exit_code = 1;
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
             .events = terminal.active ? POLLIN : 0, .revents = 0},
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
                if (terminal.active) render_event(&event);
                else print_event_line(&event);
            } else {
                ninefan_protocol_sanitize_text(
                    event.message, sizeof(event.message));
                if (event.kind == NINEFAN_EVENT_EXIT) {
                    snprintf(final_message, sizeof(final_message), "%s",
                        event.message);
                    exit_code = event.status;
                    engine_done = 1;
                } else if (event.message[0]) {
                    FILE *stream = event.status == 0 ? stdout : stderr;
                    fprintf(stream, "9fan: %s\n", event.message);
                }
            }
        }
        if (terminal.active && (descriptors[1].revents & POLLIN)) {
            unsigned char input[64] = {0};
            const ssize_t input_size =
                read(STDIN_FILENO, input, sizeof(input));
            if (input_size == 1 && input[0] != 27) {
                ninefan_command command;
                if (key_to_command(input[0], &command)
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
    terminal_leave(&terminal);
    int status = 0;
    const int child_wait_result =
        reap_child_with_escalation(child, &status, !engine_done);
    if (!engine_done) {
        if (child_wait_result == 1 && WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        }
        else exit_code = 1;
    } else if (child_wait_result != 1) {
        exit_code = 1;
    }
    if (final_message[0]) {
        FILE *stream = exit_code == 0 ? stdout : stderr;
        fprintf(stream, "9fan: %s\n", final_message);
    }
    return exit_code;
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
    return run_engine_session(initial, duration_ms);
}
