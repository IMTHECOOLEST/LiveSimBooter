#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int64_t milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int writeOutput(const char *bytes, size_t size) {
    while (size) {
        ssize_t written = write(STDOUT_FILENO, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        bytes += written;
        size -= (size_t)written;
    }
    return 0;
}

static int spawnAndRead(void) {
    int endpoints[2];
    if (pipe(endpoints)) { perror("[spawn-probe] pipe"); return 1; }
    // Keep source descriptors above stdio even if the caller starts with a closed standard descriptor.
    for (unsigned i = 0; i < 2; ++i) {
        int staged = fcntl(endpoints[i], F_DUPFD_CLOEXEC, 3);
        if (staged < 0) { perror("[spawn-probe] duplicate pipe"); close(endpoints[0]); close(endpoints[1]); return 1; }
        close(endpoints[i]);
        endpoints[i] = staged;
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        fprintf(stderr, "[spawn-probe] file actions: %s\n", strerror(error));
        close(endpoints[0]); close(endpoints[1]);
        return 1;
    }
    error = posix_spawn_file_actions_adddup2(&actions, endpoints[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, endpoints[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, endpoints[1]);
    char *arguments[] = {"/bin/df", "-h", NULL};
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, "/bin/df", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(endpoints[1]);
    if (error) {
        fprintf(stderr, "[spawn-probe] posix_spawn: %s (%d)\n", strerror(error), error);
        close(endpoints[0]);
        return 2;
    }
    fprintf(stderr, "[spawn-probe] parent=%d spawned pid=%d; reading redirected stdout\n", getpid(), child);
    int64_t start = milliseconds();
    if (start < 0) { perror("[spawn-probe] clock"); close(endpoints[0]); return 3; }
    int64_t deadline = start + 15000;
    size_t total = 0;
    for (;;) {
        int64_t now = milliseconds();
        if (now < 0 || now >= deadline) {
            fprintf(stderr, "[spawn-probe] timed out before EOF; received %zu bytes\n", total);
            close(endpoints[0]);
            return 3;
        }
        struct pollfd descriptor = {.fd = endpoints[0], .events = POLLIN};
        int ready = poll(&descriptor, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("[spawn-probe] poll"); close(endpoints[0]); return 4; }
        if (!ready) continue;
        char output[4096];
        ssize_t count = read(endpoints[0], output, sizeof(output));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { perror("[spawn-probe] read"); close(endpoints[0]); return 4; }
        if (!count) break;
        if (writeOutput(output, (size_t)count)) { perror("[spawn-probe] stdout"); close(endpoints[0]); return 4; }
        total += (size_t)count;
    }
    close(endpoints[0]);
    fprintf(stderr, "[spawn-probe] pid=%d stdout reached EOF after %zu bytes\n", child, total);
    // The broker supplies a real extension PID, but guest waitpid is intentionally not emulated yet.
    return total ? 0 : 5;
}

static int usage(const char *program) {
    fprintf(stderr, "usage: %s [--stop-before-spawn] [--debug-delay N] [--repeat N]\n"
                    "       debug delay: 1 to 60 seconds; repeat: 1 to 16\n", program);
    return 64;
}

static int parseCount(const char *number, unsigned maximum, unsigned *result) {
    if (!number[0] || strspn(number, "0123456789") != strlen(number)) return -1;
    char *end;
    errno = 0;
    unsigned long value = strtoul(number, &end, 10);
    if (errno || *end || value < 1 || value > maximum) return -1;
    *result = (unsigned)value;
    return 0;
}

int main(int argc, char *argv[]) {
    unsigned repeat = 1, debugDelay = 0;
    int stopBeforeSpawn = 0, hasRepeat = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--stop-before-spawn") && !stopBeforeSpawn) stopBeforeSpawn = 1;
        else if (!strcmp(argv[i], "--repeat") && !hasRepeat && i + 1 < argc) {
            if (parseCount(argv[++i], 16, &repeat)) return usage(argv[0]);
            hasRepeat = 1;
        } else if (!strcmp(argv[i], "--debug-delay") && !debugDelay && i + 1 < argc) {
            if (parseCount(argv[++i], 60, &debugDelay)) return usage(argv[0]);
        } else return usage(argv[0]);
    }
    if (stopBeforeSpawn) {
        fprintf(stderr, "[spawn-probe] pid=%d stopping before first spawn; resume with SIGCONT or LLDB\n", getpid());
        fflush(stderr);
        if (raise(SIGSTOP)) { perror("[spawn-probe] SIGSTOP"); return 1; }
    }
    if (debugDelay) {
        // Device debugserver may require a running target to attach; use this instead of SIGSTOP in that case.
        fprintf(stderr, "[spawn-probe] pid=%d delaying first spawn for %u seconds; attach LLDB while running\n", getpid(), debugDelay);
        fflush(stderr);
        for (unsigned remaining = debugDelay; remaining; ) remaining = sleep(remaining);
    }
    for (unsigned i = 0; i < repeat; ++i) {
        if (repeat > 1) fprintf(stderr, "[spawn-probe] iteration %u/%u\n", i + 1, repeat);
        int result = spawnAndRead();
        if (result) return result;
    }
    return 0;
}
