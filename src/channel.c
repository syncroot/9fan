#include "channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NINEFAN_CHANNEL_PREFIX "/private/tmp/9fan-channel."
#define NINEFAN_CHANNEL_TEMPLATE NINEFAN_CHANNEL_PREFIX "XXXXXX"
#define NINEFAN_CHANNEL_SOCKET_NAME "control.sock"
#define NINEFAN_CHANNEL_HANDSHAKE 0x39
#define NINEFAN_CHANNEL_HANDSHAKE_TIMEOUT_MS 2000

static long long monotonic_milliseconds(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int configure_socket(int fd) {
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);
    return status_flags >= 0
        && descriptor_flags >= 0
        && fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0
        && fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0
        ? 0
        : -1;
}

static int socket_is_stream(int fd) {
    int type = 0;
    socklen_t size = sizeof(type);
    struct stat metadata = {0};
    return fstat(fd, &metadata) == 0
        && S_ISSOCK(metadata.st_mode)
        && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0
        && size == sizeof(type)
        && type == SOCK_STREAM;
}

static int private_tmp_is_safe(void) {
    struct stat metadata = {0};
    return lstat("/private/tmp", &metadata) == 0
        && S_ISDIR(metadata.st_mode)
        && metadata.st_uid == 0
        && (metadata.st_mode & 07777) == 01777;
}

static int ascii_alphanumeric(unsigned char value) {
    return (value >= '0' && value <= '9')
        || (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z');
}

static int channel_path_parts(
    const char *socket_path,
    char directory[PATH_MAX]) {
    if (!socket_path || !directory) return -1;
    const size_t prefix_length = strlen(NINEFAN_CHANNEL_PREFIX);
    const size_t suffix_length = 6;
    const size_t socket_name_length = strlen(NINEFAN_CHANNEL_SOCKET_NAME);
    const size_t expected_length =
        prefix_length + suffix_length + 1 + socket_name_length;
    if (strlen(socket_path) != expected_length
        || strncmp(socket_path, NINEFAN_CHANNEL_PREFIX, prefix_length) != 0
        || socket_path[prefix_length + suffix_length] != '/'
        || strcmp(
            socket_path + prefix_length + suffix_length + 1,
            NINEFAN_CHANNEL_SOCKET_NAME) != 0) {
        return -1;
    }
    for (size_t index = 0; index < suffix_length; index++) {
        const unsigned char value =
            (unsigned char)socket_path[prefix_length + index];
        if (!ascii_alphanumeric(value)) return -1;
    }
    const int count = snprintf(
        directory, PATH_MAX, "%.*s",
        (int)(prefix_length + suffix_length), socket_path);
    return count > 0 && count < PATH_MAX ? 0 : -1;
}

static int channel_path_is_safe(
    const char *socket_path,
    uid_t expected_owner,
    struct stat *socket_metadata_out) {
    char directory[PATH_MAX] = {0};
    struct stat directory_metadata = {0};
    struct stat socket_metadata = {0};
    const int valid = private_tmp_is_safe()
        && channel_path_parts(socket_path, directory) == 0
        && lstat(directory, &directory_metadata) == 0
        && S_ISDIR(directory_metadata.st_mode)
        && directory_metadata.st_uid == expected_owner
        && (directory_metadata.st_mode & 07777) == 0700
        && lstat(socket_path, &socket_metadata) == 0
        && S_ISSOCK(socket_metadata.st_mode)
        && socket_metadata.st_uid == expected_owner
        && (socket_metadata.st_mode & 07777) == 0600;
    if (valid && socket_metadata_out) {
        *socket_metadata_out = socket_metadata;
    }
    return valid;
}

static int peer_uid_matches(int fd, uid_t expected_peer_uid) {
    uid_t peer_uid = (uid_t)-1;
    gid_t peer_gid = (gid_t)-1;
    return getpeereid(fd, &peer_uid, &peer_gid) == 0
        && peer_uid == expected_peer_uid;
}

static int wait_for_socket(
    int fd,
    short events,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested) {
    if (timeout_ms <= 0) return -1;
    const long long start = monotonic_milliseconds();
    if (start < 0) return -1;
    const long long deadline = start + timeout_ms;
    for (;;) {
        if (abort_requested && *abort_requested) return -1;
        const long long now = monotonic_milliseconds();
        if (now < 0 || now >= deadline) return 0;
        const long long remaining = deadline - now;
        struct pollfd descriptor = {
            .fd = fd,
            .events = events,
            .revents = 0,
        };
        const int result = poll(
            &descriptor, 1,
            remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return result;
        return (descriptor.revents & events) != 0 ? 1 : -1;
    }
}

int ninefan_channel_listener_open(ninefan_channel_listener *listener) {
    if (!listener || !private_tmp_is_safe()) return -1;
    memset(listener, 0, sizeof(*listener));
    listener->listener_fd = -1;
    const int copied = snprintf(
        listener->directory, sizeof(listener->directory),
        "%s", NINEFAN_CHANNEL_TEMPLATE);
    if (copied <= 0
        || (size_t)copied >= sizeof(listener->directory)
        || !mkdtemp(listener->directory)) {
        return -1;
    }
    struct stat directory_metadata = {0};
    if (lstat(listener->directory, &directory_metadata) != 0
        || !S_ISDIR(directory_metadata.st_mode)
        || directory_metadata.st_uid != geteuid()
        || (directory_metadata.st_mode & 07777) != 0700) {
        ninefan_channel_listener_cleanup(listener);
        return -1;
    }
    const int path_count = snprintf(
        listener->socket_path, sizeof(listener->socket_path),
        "%s/%s", listener->directory, NINEFAN_CHANNEL_SOCKET_NAME);
    if (path_count <= 0
        || (size_t)path_count >= sizeof(listener->socket_path)
        || strlen(listener->socket_path)
            >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        ninefan_channel_listener_cleanup(listener);
        return -1;
    }
    listener->listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener->listener_fd < 0
        || configure_socket(listener->listener_fd) != 0) {
        ninefan_channel_listener_cleanup(listener);
        return -1;
    }
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };
    const size_t path_size = strlen(listener->socket_path) + 1;
    memcpy(address.sun_path, listener->socket_path, path_size);
    const socklen_t address_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_size);
#ifdef __APPLE__
    address.sun_len = (uint8_t)address_size;
#endif
    if (bind(
            listener->listener_fd,
            (const struct sockaddr *)&address, address_size) != 0
        || chmod(listener->socket_path, 0600) != 0
        || !channel_path_is_safe(
            listener->socket_path, geteuid(), NULL)
        || listen(listener->listener_fd, 1) != 0) {
        ninefan_channel_listener_cleanup(listener);
        return -1;
    }
    return 0;
}

int ninefan_channel_accept(
    ninefan_channel_listener *listener,
    uid_t expected_peer_uid,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested) {
    if (!listener
        || listener->listener_fd < 0
        || !channel_path_is_safe(
            listener->socket_path, geteuid(), NULL)) {
        return -1;
    }
    const int wait_result = wait_for_socket(
        listener->listener_fd, POLLIN, timeout_ms, abort_requested);
    if (wait_result == 0) return NINEFAN_CHANNEL_ACCEPT_TIMEOUT;
    if (wait_result < 0) return -1;
    const int fd = accept(listener->listener_fd, NULL, NULL);
    if (fd < 0
        || configure_socket(fd) != 0
        || !socket_is_stream(fd)
        || !peer_uid_matches(fd, expected_peer_uid)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (wait_for_socket(
            fd, POLLIN, NINEFAN_CHANNEL_HANDSHAKE_TIMEOUT_MS,
            abort_requested) <= 0) {
        close(fd);
        return -1;
    }
    unsigned char handshake = 0;
    if (read(fd, &handshake, sizeof(handshake)) != sizeof(handshake)
        || handshake != NINEFAN_CHANNEL_HANDSHAKE) {
        close(fd);
        return -1;
    }
    return fd;
}

int ninefan_channel_connect(
    const char *socket_path,
    uid_t expected_peer_uid,
    int timeout_ms,
    const volatile sig_atomic_t *abort_requested) {
    struct stat socket_before = {0};
    if (!channel_path_is_safe(
            socket_path, expected_peer_uid, &socket_before)
        || strlen(socket_path)
            >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || configure_socket(fd) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };
    const size_t path_size = strlen(socket_path) + 1;
    memcpy(address.sun_path, socket_path, path_size);
    const socklen_t address_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_size);
#ifdef __APPLE__
    address.sun_len = (uint8_t)address_size;
#endif
    int connect_result =
        connect(fd, (const struct sockaddr *)&address, address_size);
    if (connect_result != 0 && errno == EINPROGRESS) {
        connect_result = wait_for_socket(
            fd, POLLOUT, timeout_ms, abort_requested);
        if (connect_result > 0) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            connect_result =
                getsockopt(
                    fd, SOL_SOCKET, SO_ERROR,
                    &socket_error, &error_size) == 0
                    && error_size == sizeof(socket_error)
                    && socket_error == 0
                ? 0
                : -1;
        } else {
            connect_result = -1;
        }
    }
    struct stat socket_after = {0};
    if (connect_result != 0
        || !socket_is_stream(fd)
        || !peer_uid_matches(fd, expected_peer_uid)
        || !channel_path_is_safe(
            socket_path, expected_peer_uid, &socket_after)
        || socket_before.st_dev != socket_after.st_dev
        || socket_before.st_ino != socket_after.st_ino
        || wait_for_socket(
            fd, POLLOUT, timeout_ms, abort_requested) <= 0) {
        close(fd);
        return -1;
    }
    const unsigned char handshake = NINEFAN_CHANNEL_HANDSHAKE;
    if (write(fd, &handshake, sizeof(handshake)) != sizeof(handshake)) {
        close(fd);
        return -1;
    }
    return fd;
}

void ninefan_channel_listener_cleanup(ninefan_channel_listener *listener) {
    if (!listener) return;
    if (listener->listener_fd >= 0) {
        close(listener->listener_fd);
        listener->listener_fd = -1;
    }
    if (listener->socket_path[0]) {
        (void)unlink(listener->socket_path);
        listener->socket_path[0] = '\0';
    }
    if (listener->directory[0]) {
        (void)rmdir(listener->directory);
        listener->directory[0] = '\0';
    }
}
