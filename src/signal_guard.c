#include "signal_guard.h"

#include <signal.h>
#include <string.h>

static int add_handled_signals(sigset_t *signals) {
    return sigemptyset(signals) == 0
        && sigaddset(signals, SIGINT) == 0
        && sigaddset(signals, SIGTERM) == 0
        && sigaddset(signals, SIGHUP) == 0
        && sigaddset(signals, SIGQUIT) == 0
        && sigaddset(signals, SIGABRT) == 0
        && sigaddset(signals, SIGBUS) == 0
        && sigaddset(signals, SIGFPE) == 0
        && sigaddset(signals, SIGILL) == 0
        && sigaddset(signals, SIGSEGV) == 0
        ? 0
        : -1;
}

static int set_handler(int signal_number, ninefan_signal_handler handler) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    return sigemptyset(&action.sa_mask) == 0
        && sigaction(signal_number, &action, NULL) == 0
        ? 0
        : -1;
}

int ninefan_signal_guard_install(
    ninefan_signal_handler termination_handler,
    ninefan_signal_handler fatal_handler) {
    if (!termination_handler || !fatal_handler) return -1;

    sigset_t handled_signals;
    if (add_handled_signals(&handled_signals) != 0
        || sigprocmask(SIG_UNBLOCK, &handled_signals, NULL) != 0
        || set_handler(SIGINT, termination_handler) != 0
        || set_handler(SIGTERM, termination_handler) != 0
        || set_handler(SIGHUP, termination_handler) != 0
        || set_handler(SIGQUIT, termination_handler) != 0
        || set_handler(SIGABRT, fatal_handler) != 0
        || set_handler(SIGBUS, fatal_handler) != 0
        || set_handler(SIGFPE, fatal_handler) != 0
        || set_handler(SIGILL, fatal_handler) != 0
        || set_handler(SIGSEGV, fatal_handler) != 0
        || signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return -1;
    }
    return 0;
}
