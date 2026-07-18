#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

int ninefan_protocol_command_valid(const ninefan_command *command) {
    return command
        && command->magic == NINEFAN_PROTOCOL_MAGIC
        && command->version == NINEFAN_PROTOCOL_VERSION
        && command->kind <= NINEFAN_COMMAND_QUIT;
}

int ninefan_protocol_event_valid(const ninefan_event *event) {
    return event
        && event->magic == NINEFAN_PROTOCOL_MAGIC
        && event->version == NINEFAN_PROTOCOL_VERSION
        && event->kind >= NINEFAN_EVENT_SNAPSHOT
        && event->kind <= NINEFAN_EVENT_EXIT
        && event->fan_count <= NINEFAN_MAX_FANS
        && event->selected_curve <= NINEFAN_COMMAND_MAXIMUM;
}

static long long monotonic_milliseconds(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    if (now.tv_sec > LLONG_MAX / 1000LL) return LLONG_MAX;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int wait_for_io(
    int fd,
    short event,
    long long deadline_ms,
    const volatile sig_atomic_t *abort_requested) {
    for (;;) {
        if (abort_requested && *abort_requested) return -1;
        const long long now = monotonic_milliseconds();
        if (now < 0 || now >= deadline_ms) return -1;
        const long long remaining = deadline_ms - now;
        const int timeout =
            remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {
            .fd = fd,
            .events = event,
            .revents = 0,
        };
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        if (descriptor.revents & event) return 0;
        return -1;
    }
}

static int io_parameters_valid(
    int fd, const void *buffer, size_t size, int timeout_ms) {
    return fd >= 0
        && (buffer || size == 0)
        && size <= PIPE_BUF
        && timeout_ms > 0;
}

int ninefan_protocol_read_full(
    int fd,
    void *buffer,
    size_t size,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested) {
    if (!io_parameters_valid(fd, buffer, size, timeout_ms)) return -1;
    const long long started = monotonic_milliseconds();
    if (started < 0 || started > LLONG_MAX - timeout_ms) return -1;
    const long long deadline = started + timeout_ms;
    size_t total = 0;
    while (total < size) {
        if (wait_for_io(
                fd, POLLIN, deadline, abort_requested) != 0) {
            return -1;
        }
        const ssize_t count =
            read(fd, (unsigned char *)buffer + total, size - total);
        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0
            && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (abort_requested && *abort_requested) return -1;
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

int ninefan_protocol_write_full(
    int fd,
    const void *buffer,
    size_t size,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested) {
    if (!io_parameters_valid(fd, buffer, size, timeout_ms)) return -1;
    const long long started = monotonic_milliseconds();
    if (started < 0 || started > LLONG_MAX - timeout_ms) return -1;
    const long long deadline = started + timeout_ms;
    size_t total = 0;
    while (total < size) {
        if (wait_for_io(
                fd, POLLOUT, deadline, abort_requested) != 0) {
            return -1;
        }
        const ssize_t count =
            write(fd, (const unsigned char *)buffer + total, size - total);
        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0
            && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (abort_requested && *abort_requested) return -1;
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

void ninefan_protocol_sanitize_text(char *text, size_t size) {
    if (!text || size == 0) return;
    text[size - 1] = '\0';
    for (size_t index = 0; index < size && text[index]; index++) {
        const unsigned char value = (unsigned char)text[index];
        if (!isprint(value) && value != '\t') text[index] = '?';
    }
}
