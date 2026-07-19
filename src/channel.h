#ifndef NINEFAN_CHANNEL_H
#define NINEFAN_CHANNEL_H

#include <limits.h>
#include <signal.h>
#include <sys/types.h>

#define NINEFAN_CHANNEL_ACCEPT_TIMEOUT (-2)

typedef struct {
    int listener_fd;
    char directory[PATH_MAX];
    char socket_path[PATH_MAX];
} ninefan_channel_listener;

int ninefan_channel_listener_open(ninefan_channel_listener *listener);
int ninefan_channel_accept(
    ninefan_channel_listener *listener,
    uid_t expected_peer_uid,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested);
int ninefan_channel_connect(
    const char *socket_path,
    uid_t expected_peer_uid,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested);
void ninefan_channel_listener_cleanup(ninefan_channel_listener *listener);

#endif
