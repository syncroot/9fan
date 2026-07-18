#ifndef NINEFAN_SIGNAL_GUARD_H
#define NINEFAN_SIGNAL_GUARD_H

typedef void (*ninefan_signal_handler)(int);

int ninefan_signal_guard_install(
    ninefan_signal_handler termination_handler,
    ninefan_signal_handler fatal_handler);

#endif
