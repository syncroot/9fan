#include "../src/signal_guard.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t termination_seen;

static void termination_handler(int signal_number) {
    termination_seen = signal_number;
}

static void fatal_handler(int signal_number) {
    (void)signal_number;
}

int main(void) {
    sigset_t blocked;
    assert(sigemptyset(&blocked) == 0);
    assert(sigaddset(&blocked, SIGTERM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0);

    assert(ninefan_signal_guard_install(
        termination_handler, fatal_handler) == 0);

    sigset_t current;
    assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    assert(sigismember(&current, SIGTERM) == 0);
    assert(raise(SIGTERM) == 0);
    assert(termination_seen == SIGTERM);

    assert(ninefan_signal_guard_install(NULL, fatal_handler) != 0);
    puts("signal guard tests passed");
    return 0;
}
