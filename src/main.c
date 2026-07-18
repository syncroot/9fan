#include "curve.h"
#include "smc.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NINEFAN_VERSION "1.0.0"
#define SAMPLE_INTERVAL_MS 2000

static volatile sig_atomic_t termination_requested;

typedef struct {
    struct termios original;
    int active;
} ninefan_terminal;

typedef struct {
    int active;
    int write_fd;
    pid_t pid;
} ninefan_watchdog;

typedef struct {
    const ninefan_curve *curve;
    int manual_active;
    float held_temperature;
    float current_temperature;
    char hottest_key[5];
    int temperature_valid;
    float requested_fraction;
} ninefan_controller;

static void request_termination(int signal_number) {
    (void)signal_number;
    termination_requested = 1;
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_termination;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static long long monotonic_milliseconds(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void terminal_enter(ninefan_terminal *terminal) {
    memset(terminal, 0, sizeof(*terminal));
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &terminal->original) != 0) return;
    struct termios raw = terminal->original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
    terminal->active = 1;
    printf("\033[?1049h\033[?25l\033[2J");
    fflush(stdout);
}

static void terminal_leave(ninefan_terminal *terminal) {
    if (!terminal || !terminal->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal->original);
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
    terminal->active = 0;
}

static void read_sysctl_string(const char *name, char *output, size_t output_size) {
    if (!output || output_size == 0) return;
    output[0] = '\0';
    size_t size = output_size;
    if (sysctlbyname(name, output, &size, NULL, 0) != 0 || size == 0) output[0] = '\0';
    output[output_size - 1] = '\0';
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
        printf("  %-12s %s\n", ninefan_curves[index].slug, ninefan_curves[index].summary);
    }
}

static void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "  9fan                         Interactive monitor and curve selector\n"
        "  sudo 9fan quiet|balanced|performance|max\n"
        "  sudo 9fan default           Restore Apple's automatic control\n"
        "  9fan status                 Read-only hardware and fan status\n"
        "  sudo 9fan self-test         Verify safe SMC control, then restore default\n"
        "  9fan curves                 Print the built-in curves\n"
        "  9fan --version\n");
}

static int refresh_snapshot(
    ninefan_smc *smc, ninefan_controller *controller, char *message, size_t message_size) {
    int result = 0;
    if (ninefan_smc_refresh_fans(smc) != 0) {
        snprintf(message, message_size, "%s", ninefan_smc_error(smc));
        result = -1;
    }
    float temperature = NAN;
    char key[5] = "----";
    if (ninefan_smc_hottest_temperature(smc, &temperature, key) == 0) {
        controller->current_temperature = temperature;
        controller->temperature_valid = 1;
        memcpy(controller->hottest_key, key, 5);
    } else {
        controller->current_temperature = NAN;
        controller->temperature_valid = 0;
        memcpy(controller->hottest_key, "----", 5);
        if (result == 0) {
            snprintf(message, message_size, "%s", ninefan_smc_error(smc));
            result = -1;
        }
    }
    return result;
}

static int restore_controller(
    ninefan_smc *smc, ninefan_controller *controller, char *message, size_t message_size) {
    const int result = ninefan_smc_restore_default(smc);
    controller->manual_active = 0;
    controller->held_temperature = NAN;
    controller->requested_fraction = NAN;
    if (result != 0) {
        snprintf(message, message_size, "Default restore failed: %s", ninefan_smc_error(smc));
        return -1;
    }
    snprintf(message, message_size, "Apple automatic control restored");
    return 0;
}

static int update_controller(
    ninefan_smc *smc, ninefan_controller *controller, char *message, size_t message_size) {
    if (!controller->curve) return 0;

    if (controller->temperature_valid) {
        if (!isfinite(controller->held_temperature)
            || controller->current_temperature >= controller->held_temperature) {
            controller->held_temperature = controller->current_temperature;
        } else {
            /* Fast ramp-up and slow release prevent RPM hunting as temperatures fall. */
            controller->held_temperature =
                fmaxf(controller->current_temperature, controller->held_temperature - 1.5f);
        }
    }

    const int needs_manual =
        !controller->temperature_valid
        || controller->held_temperature
               >= controller->curve->activation_c
                      - (controller->manual_active ? controller->curve->release_hysteresis_c : 0.0f);

    if (!needs_manual) {
        if (controller->manual_active) return restore_controller(smc, controller, message, message_size);
        controller->requested_fraction = NAN;
        return 0;
    }

    const float fraction = controller->temperature_valid
        ? ninefan_curve_fraction(controller->curve, controller->held_temperature)
        : 1.0f;
    controller->requested_fraction = fminf(1.0f, fmaxf(0.0f, fraction));

    for (int index = 0; index < smc->fan_count; index++) {
        if (smc->fans[index].mode != 1
            && ninefan_smc_enable_manual(smc, index) != 0) {
            snprintf(message, message_size, "Fan %d manual mode failed: %s",
                index, ninefan_smc_error(smc));
            ninefan_smc_restore_default(smc);
            controller->manual_active = 0;
            return -1;
        }
    }
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        const float target = fan->minimum_rpm
            + controller->requested_fraction * (fan->maximum_rpm - fan->minimum_rpm);
        if (!controller->manual_active || fabsf(target - fan->target_rpm) >= 75.0f) {
            if (ninefan_smc_set_target(smc, index, target) != 0) {
                snprintf(message, message_size, "Fan %d target failed: %s",
                    index, ninefan_smc_error(smc));
                ninefan_smc_restore_default(smc);
                controller->manual_active = 0;
                return -1;
            }
        }
    }
    controller->manual_active = 1;
    if (!controller->temperature_valid) {
        snprintf(message, message_size, "SENSOR FAIL-SAFE: fans forced to maximum");
    }
    return 0;
}

static void watchdog_child(int read_fd) {
    setpgid(0, 0);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    char clean_shutdown = 0;
    ssize_t count;
    do {
        count = read(read_fd, &clean_shutdown, 1);
    } while (count < 0 && errno == EINTR);
    close(read_fd);
    if (count != 1 || clean_shutdown != 1) {
        ninefan_smc smc;
        if (ninefan_smc_open(&smc) == 0) {
            ninefan_smc_restore_default(&smc);
            ninefan_smc_close(&smc);
        }
    }
    _exit(0);
}

static int watchdog_start(
    ninefan_watchdog *watchdog, ninefan_smc *smc, char *message, size_t message_size) {
    if (watchdog->active) return 0;

    /*
     * Reopen the parent's AppleSMC connection after fork. The child therefore
     * owns no inherited IOKit connection and can open a clean one if needed.
     */
    ninefan_smc_close(smc);
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        snprintf(message, message_size, "Could not create safety watchdog: %s", strerror(errno));
        (void)ninefan_smc_open(smc);
        return -1;
    }
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        snprintf(message, message_size, "Could not start safety watchdog: %s", strerror(errno));
        (void)ninefan_smc_open(smc);
        return -1;
    }
    if (pid == 0) {
        close(descriptors[1]);
        watchdog_child(descriptors[0]);
    }

    close(descriptors[0]);
    watchdog->active = 1;
    watchdog->write_fd = descriptors[1];
    watchdog->pid = pid;
    if (ninefan_smc_open(smc) != 0) {
        snprintf(message, message_size, "AppleSMC reopen failed: %s", ninefan_smc_error(smc));
        const char clean = 1;
        (void)write(watchdog->write_fd, &clean, 1);
        close(watchdog->write_fd);
        waitpid(watchdog->pid, NULL, 0);
        memset(watchdog, 0, sizeof(*watchdog));
        return -1;
    }
    return 0;
}

static void watchdog_clean_stop(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return;
    const char clean = 1;
    (void)write(watchdog->write_fd, &clean, 1);
    close(watchdog->write_fd);
    waitpid(watchdog->pid, NULL, 0);
    memset(watchdog, 0, sizeof(*watchdog));
}

static void watchdog_trigger_restore(ninefan_watchdog *watchdog) {
    if (!watchdog || !watchdog->active) return;
    /* EOF, without the clean byte, tells the child to perform its own restore. */
    close(watchdog->write_fd);
    waitpid(watchdog->pid, NULL, 0);
    memset(watchdog, 0, sizeof(*watchdog));
}

static void render_ui(
    const ninefan_smc *smc, const ninefan_controller *controller, const char *message,
    int privileged) {
    char model[64], chip[64];
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    if (!model[0]) snprintf(model, sizeof(model), "Apple Silicon Mac");
    if (!chip[0]) snprintf(chip, sizeof(chip), "Apple Silicon");

    printf("\033[H\033[2J");
    printf("\033[30;46;1m 9fan %-8s  %-18s  %-20s \033[0m\r\n",
        NINEFAN_VERSION, model, chip);
    if (controller->temperature_valid) {
        printf("\r\n  Hotspot   \033[1m%5.1f C\033[0m  (%s, hottest CPU/GPU/memory sensor)\r\n",
            controller->current_temperature, controller->hottest_key);
    } else {
        printf("\r\n  Hotspot   \033[31;1mUNAVAILABLE - MAXIMUM FAIL-SAFE\033[0m\r\n");
    }
    printf("  Profile   \033[1m%s\033[0m",
        controller->curve ? controller->curve->name : "Apple default");
    if (controller->curve && !controller->manual_active) {
        printf("  (below %.0f C; Apple remains in control)", controller->curve->activation_c);
    } else if (controller->manual_active && isfinite(controller->requested_fraction)) {
        printf("  (%.0f%% of min-to-max range)", controller->requested_fraction * 100.0f);
    }
    printf("\r\n\r\n");

    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        printf("  Fan %d  %5.0f RPM   target %5.0f   range %4.0f-%4.0f   %-6s\r\n",
            index, fan->actual_rpm, fan->target_rpm, fan->minimum_rpm, fan->maximum_rpm,
            mode_name(fan->mode));
    }

    printf("\r\n  \033[96;1mCURVES\033[0m\r\n");
    printf("  [a] Apple default   system thermal controller, including true 0 RPM\r\n");
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        printf("  [%zu] %-13s %s\r\n", index + 1, ninefan_curves[index].name,
            ninefan_curves[index].summary);
    }
    printf("\r\n  [q] Restore Apple default and quit\r\n");
    printf("  Safety: SMC limits only; sensor loss -> max; exit/UI crash -> Apple default\r\n");
    if (!privileged) {
        printf("\r\n  \033[33;1mRead-only. Run sudo \"$(command -v 9fan)\" to select curves.\033[0m\r\n");
    }
    if (message && message[0]) printf("\r\n  %s\r\n", message);
    fflush(stdout);
}

static void print_snapshot_line(
    const ninefan_smc *smc, const ninefan_controller *controller) {
    if (controller->temperature_valid) {
        printf("%.1fC %-4s  %-11s", controller->current_temperature,
            controller->hottest_key, controller->curve ? controller->curve->slug : "default");
    } else {
        printf("TEMP-FAIL      %-11s", controller->curve ? controller->curve->slug : "default");
    }
    for (int index = 0; index < smc->fan_count; index++) {
        printf("  F%d %.0f/%.0f RPM %s", index, smc->fans[index].actual_rpm,
            smc->fans[index].target_rpm, mode_name(smc->fans[index].mode));
    }
    putchar('\n');
    fflush(stdout);
}

static int select_curve(
    const ninefan_curve *curve, ninefan_controller *controller, ninefan_watchdog *watchdog,
    ninefan_smc *smc, char *message, size_t message_size, int privileged) {
    if (!privileged) {
        snprintf(message, message_size,
            "Curve selection requires root; relaunch with sudo \"$(command -v 9fan)\"");
        return -1;
    }
    if (watchdog_start(watchdog, smc, message, message_size) != 0) return -1;
    controller->curve = curve;
    controller->held_temperature = NAN;
    snprintf(message, message_size, "%s curve selected", curve->name);
    return 0;
}

static int select_default(
    ninefan_controller *controller, ninefan_watchdog *watchdog, ninefan_smc *smc,
    char *message, size_t message_size, int privileged) {
    if (!privileged) {
        snprintf(message, message_size,
            "Restoring default requires root; run sudo \"$(command -v 9fan)\" default");
        return -1;
    }
    controller->curve = NULL;
    const int result = restore_controller(smc, controller, message, message_size);
    if (result == 0) watchdog_clean_stop(watchdog);
    else watchdog_trigger_restore(watchdog);
    return result;
}

static int run_ui(ninefan_smc *smc, const ninefan_curve *initial_curve) {
    const int privileged = geteuid() == 0;
    ninefan_terminal terminal;
    ninefan_watchdog watchdog = {0};
    ninefan_controller controller = {
        .curve = NULL,
        .manual_active = 0,
        .held_temperature = NAN,
        .current_temperature = NAN,
        .hottest_key = "----",
        .temperature_valid = 0,
        .requested_fraction = NAN,
    };
    char message[256] = {0};
    const int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    if (initial_curve
        && select_curve(initial_curve, &controller, &watchdog, smc,
               message, sizeof(message), privileged) != 0) {
        fprintf(stderr, "9fan: %s\n", message);
        return 1;
    }
    if (!interactive && !initial_curve) {
        refresh_snapshot(smc, &controller, message, sizeof(message));
        print_snapshot_line(smc, &controller);
        return 0;
    }

    terminal_enter(&terminal);
    long long next_sample = 0;
    int exit_code = 0;

    while (!termination_requested) {
        const long long now = monotonic_milliseconds();
        if (now >= next_sample) {
            const int snapshot_result =
                refresh_snapshot(smc, &controller, message, sizeof(message));
            if (controller.curve) {
                if (snapshot_result != 0 && smc->fan_count == 0) {
                    exit_code = 1;
                    break;
                }
                if (update_controller(smc, &controller, message, sizeof(message)) != 0) {
                    controller.curve = NULL;
                    watchdog_trigger_restore(&watchdog);
                    exit_code = 1;
                }
                (void)ninefan_smc_refresh_fans(smc);
            }
            if (interactive) render_ui(smc, &controller, message, privileged);
            else print_snapshot_line(smc, &controller);
            next_sample = now + SAMPLE_INTERVAL_MS;
        }

        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = interactive ? POLLIN : 0,
            .revents = 0,
        };
        const int poll_result = poll(&descriptor, interactive ? 1 : 0, 200);
        if (interactive && poll_result > 0 && (descriptor.revents & POLLIN)) {
            unsigned char key = 0;
            if (read(STDIN_FILENO, &key, 1) != 1) continue;
            if (key == 'q' || key == 3) break;
            if (key == 'a' || key == '0') {
                select_default(&controller, &watchdog, smc,
                    message, sizeof(message), privileged);
                next_sample = 0;
            } else if (key >= '1' && key <= '0' + (int)ninefan_curve_count) {
                const ninefan_curve *curve = &ninefan_curves[key - '1'];
                select_curve(curve, &controller, &watchdog, smc,
                    message, sizeof(message), privileged);
                next_sample = 0;
            }
        }
    }

    if (watchdog.active) {
        if (ninefan_smc_restore_default(smc) != 0) {
            exit_code = 1;
            snprintf(message, sizeof(message), "Default restore failed: %s",
                ninefan_smc_error(smc));
            watchdog_trigger_restore(&watchdog);
        } else {
            watchdog_clean_stop(&watchdog);
        }
    }
    terminal_leave(&terminal);
    if (exit_code != 0 && message[0]) fprintf(stderr, "9fan: %s\n", message);
    return exit_code;
}

static int print_status(ninefan_smc *smc) {
    ninefan_controller controller = {
        .held_temperature = NAN,
        .current_temperature = NAN,
        .hottest_key = "----",
        .requested_fraction = NAN,
    };
    char message[256] = {0};
    refresh_snapshot(smc, &controller, message, sizeof(message));

    char model[64], chip[64];
    read_sysctl_string("hw.model", model, sizeof(model));
    read_sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    printf("9fan %s\n", NINEFAN_VERSION);
    printf("Model:       %s\n", model[0] ? model : "unknown");
    printf("Chip:        %s\n", chip[0] ? chip : "Apple Silicon");
    printf("Privileges:  %s\n", geteuid() == 0 ? "root (control enabled)" : "user (read-only)");
    printf("Mode key:    %s\n", smc->mode_key_format);
    printf("Ftst unlock: %s\n", smc->ftst_available ? "available" : "not present (M5 direct mode)");
    printf("Temp keys:   %zu\n", smc->temperature_key_count);
    if (controller.temperature_valid) {
        printf("Hotspot:     %.1f C (%s)\n", controller.current_temperature, controller.hottest_key);
    } else {
        printf("Hotspot:     unavailable\n");
    }
    for (int index = 0; index < smc->fan_count; index++) {
        const ninefan_fan *fan = &smc->fans[index];
        printf("Fan %d:       %.0f RPM, target %.0f, range %.0f-%.0f, mode %s (%u)\n",
            index, fan->actual_rpm, fan->target_rpm, fan->minimum_rpm,
            fan->maximum_rpm, mode_name(fan->mode), fan->mode);
    }
    if (message[0]) fprintf(stderr, "9fan: %s\n", message);
    return controller.temperature_valid ? 0 : 1;
}

static int confirm_self_test(void) {
    /* Naming the explicit self-test command is sufficient in a non-TTY script. */
    if (!isatty(STDIN_FILENO)) return 1;
    printf("This briefly sets every fan to its SMC minimum, verifies the target, "
           "then restores Apple control.\nContinue? [y/N] ");
    fflush(stdout);
    char answer[8] = {0};
    return fgets(answer, sizeof(answer), stdin)
        && (answer[0] == 'y' || answer[0] == 'Y');
}

static int run_self_test(ninefan_smc *smc) {
    if (geteuid() != 0) {
        fprintf(stderr, "9fan: self-test requires sudo\n");
        return 1;
    }
    if (!confirm_self_test()) {
        fprintf(stderr, "9fan: self-test cancelled\n");
        return 1;
    }

    ninefan_watchdog watchdog = {0};
    char message[256] = {0};
    if (watchdog_start(&watchdog, smc, message, sizeof(message)) != 0) {
        fprintf(stderr, "9fan: %s\n", message);
        return 1;
    }

    int result = 0;
    for (int index = 0; index < smc->fan_count; index++) {
        if (ninefan_smc_enable_manual(smc, index) != 0
            || ninefan_smc_set_target(smc, index, smc->fans[index].minimum_rpm) != 0) {
            fprintf(stderr, "9fan: fan %d control failed: %s\n",
                index, ninefan_smc_error(smc));
            result = 1;
            break;
        }
    }
    if (result == 0 && ninefan_smc_refresh_fans(smc) == 0) {
        for (int index = 0; index < smc->fan_count; index++) {
            const ninefan_fan *fan = &smc->fans[index];
            const int accepted =
                fan->mode == 1 && fabsf(fan->target_rpm - fan->minimum_rpm) <= 100.0f;
            printf("Fan %d: mode=%s, target=%.0f, minimum=%.0f — %s\n",
                index, mode_name(fan->mode), fan->target_rpm, fan->minimum_rpm,
                accepted ? "accepted" : "FAILED");
            if (!accepted) result = 1;
        }
    } else if (result == 0) {
        fprintf(stderr, "9fan: verification read failed: %s\n", ninefan_smc_error(smc));
        result = 1;
    }

    if (ninefan_smc_restore_default(smc) != 0) {
        fprintf(stderr, "9fan: default restore failed: %s\n", ninefan_smc_error(smc));
        result = 1;
        watchdog_trigger_restore(&watchdog);
    } else {
        watchdog_clean_stop(&watchdog);
    }
    if (result == 0) puts("Self-test passed; Apple automatic control restored.");
    return result;
}

int main(int argc, char **argv) {
    install_signal_handlers();

    if (argc > 2) {
        print_usage(stderr);
        return 2;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        puts("9fan " NINEFAN_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "curves") == 0) {
        print_curves();
        return 0;
    }

    const ninefan_curve *initial_curve = NULL;
    if (argc == 2 && strcmp(argv[1], "default") != 0 && strcmp(argv[1], "status") != 0
        && strcmp(argv[1], "doctor") != 0 && strcmp(argv[1], "self-test") != 0) {
        initial_curve = ninefan_curve_named(argv[1]);
        if (!initial_curve) {
            fprintf(stderr, "9fan: unknown curve '%s'\n", argv[1]);
            print_curves();
            return 2;
        }
    }

    ninefan_smc smc;
    if (ninefan_smc_open(&smc) != 0) {
        fprintf(stderr, "9fan: %s\n", ninefan_smc_error(&smc));
        return 1;
    }

    int result;
    if (argc == 2 && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "doctor") == 0)) {
        result = print_status(&smc);
    } else if (argc == 2 && strcmp(argv[1], "default") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "9fan: restoring default requires sudo\n");
            result = 1;
        } else if (ninefan_smc_restore_default(&smc) != 0) {
            fprintf(stderr, "9fan: default restore failed: %s\n", ninefan_smc_error(&smc));
            result = 1;
        } else {
            puts("Apple automatic fan control restored.");
            result = 0;
        }
    } else if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        result = run_self_test(&smc);
    } else {
        result = run_ui(&smc, initial_curve);
    }

    ninefan_smc_close(&smc);
    return result;
}
