/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include "daemon.h"
#include "pie_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_BATCH 64

extern char **environ;
static int ready_fd = -1;
static dispatch_queue_t manager_queue;
static dispatch_source_t socket_source, manager_timer;

void
pie_start(int argc, const char *argv[])
{
    int foreground = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-d")) foreground = 1;
    int lock = open("/var/run/syslog.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) exit(1);
    fcntl(lock, F_SETFD, FD_CLOEXEC);
    if (foreground) return;
    int channel[2];
    if (pipe(channel) != 0) exit(1);
    pid_t child = fork();
    if (child < 0) exit(1);
    if (child > 0) {
        close(channel[1]);
        close(lock);
        struct pollfd p = { .fd = channel[0], .events = POLLIN };
        char ready = 0;
        int ok = poll(&p, 1, 30000) > 0 && read(channel[0], &ready, 1) == 1 && ready == 1;
        close(channel[0]);
        if (!ok) kill(child, SIGTERM);
        _exit(ok ? 0 : 1);
    }
    close(channel[0]);
    ready_fd = channel[1];
    fcntl(ready_fd, F_SETFD, FD_CLOEXEC);
    if (setsid() < 0 || chdir("/") != 0) exit(1);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) exit(1);
    for (int fd = 0; fd < 3; fd++) if (dup2(nullfd, fd) < 0) exit(1);
    if (nullfd > 2) close(nullfd);
}

static void
run_manager(void)
{
    pid_t child;
    char *args[] = { "/usr/sbin/aslmanager", "-q", NULL };
    int error = posix_spawn(&child, args[0], NULL, NULL, args, environ);
    if (error != 0) {
        asldebug("retention start failed: %s\n", strerror(error));
        return;
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) asldebug("retention failed: %d\n", status);
}

int
asl_trigger_aslmanager(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        manager_queue = dispatch_queue_create("Retention", NULL);
        manager_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, manager_queue);
        dispatch_source_set_timer(manager_timer, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC),
            300 * NSEC_PER_SEC, 10 * NSEC_PER_SEC);
        dispatch_source_set_event_handler(manager_timer, ^{ run_manager(); });
        dispatch_resume(manager_timer);
    });
    return 0;
}

void
pie_inputs(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) exit(1);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) exit(1);
    struct sockaddr_un address = { .sun_len = sizeof(address), .sun_family = AF_UNIX };
    strlcpy(address.sun_path, "/var/run/syslog", sizeof(address.sun_path));
    unlink(address.sun_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || chmod(address.sun_path, 0666) != 0) exit(1);
    socket_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, global.work_queue);
    if (socket_source == NULL) exit(1);
    dispatch_source_set_event_handler(socket_source, ^{
        char line[8193];
        ssize_t length;
        for (int n = 0; n < INPUT_BATCH && (length = recv(fd, line, sizeof(line) - 1, 0)) > 0; n++) {
            line[length] = '\0';
            asl_msg_t *message = asl_input_parse(line, (int)length, NULL, SOURCE_UNKNOWN);
            process_message(message, SOURCE_UNKNOWN);
        }
    });
    dispatch_resume(socket_source);
    asl_trigger_aslmanager();
}

void
pie_ready(void)
{
    if (ready_fd < 0) return;
    char ready = 1;
    if (write(ready_fd, &ready, 1) != 1) exit(1);
    close(ready_fd);
    ready_fd = -1;
}
