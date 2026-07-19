#include "../src/channel.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    ninefan_channel_listener listener = {
        .listener_fd = -1,
    };
    assert(ninefan_channel_listener_open(&listener) == 0);
    assert(listener.listener_fd >= 0);
    assert(strncmp(
        listener.socket_path,
        "/private/tmp/9fan-channel.", 26) == 0);

    char directory[PATH_MAX] = {0};
    char socket_path[PATH_MAX] = {0};
    assert(snprintf(
        directory, sizeof(directory), "%s", listener.directory) > 0);
    assert(snprintf(
        socket_path, sizeof(socket_path), "%s", listener.socket_path) > 0);

    struct stat metadata = {0};
    assert(lstat(directory, &metadata) == 0);
    assert((metadata.st_mode & 07777) == 0700);
    assert(lstat(socket_path, &metadata) == 0);
    assert((metadata.st_mode & 07777) == 0600);

    assert(ninefan_channel_accept(
        &listener, geteuid(), 10, NULL)
        == NINEFAN_CHANNEL_ACCEPT_TIMEOUT);
    assert(ninefan_channel_connect(
        "/tmp/not-a-9fan-channel/control.sock",
        geteuid(), 10, NULL) == -1);

    assert(chmod(directory, 0755) == 0);
    assert(ninefan_channel_connect(
        socket_path, geteuid(), 10, NULL) == -1);
    assert(chmod(directory, 0700) == 0);
    assert(chmod(socket_path, 0644) == 0);
    assert(ninefan_channel_connect(
        socket_path, geteuid(), 10, NULL) == -1);
    assert(chmod(socket_path, 0600) == 0);

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(listener.listener_fd);
        const int fd = ninefan_channel_connect(
            socket_path, geteuid(), 2000, NULL);
        if (fd < 0) _exit(10);
        const unsigned char value = 0x39;
        if (write(fd, &value, sizeof(value)) != sizeof(value)) _exit(11);
        close(fd);
        _exit(0);
    }

    const int fd = ninefan_channel_accept(
        &listener, geteuid(), 2000, NULL);
    assert(fd >= 0);
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);
    assert(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0);
    assert(descriptor_flags >= 0
        && (descriptor_flags & FD_CLOEXEC) != 0);

    struct pollfd descriptor = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0,
    };
    assert(poll(&descriptor, 1, 2000) == 1);
    unsigned char received = 0;
    assert(read(fd, &received, sizeof(received)) == sizeof(received));
    assert(received == 0x39);

    close(fd);
    ninefan_channel_listener_cleanup(&listener);
    assert(access(socket_path, F_OK) != 0);
    assert(access(directory, F_OK) != 0);

    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);

    puts("channel tests passed");
    return 0;
}
