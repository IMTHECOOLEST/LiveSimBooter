#include "../ProcessSupport/SpawnTransport.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Audited against the iOS 26 runtime's host support libkernel: action stride 0x410, attributes size 0x100.
// These in-process opaque layouts are translated to SpawnPlan; native pointers are never sent to another process.
typedef struct {
    int32_t kind, fd;
    union { struct { int32_t flags; uint16_t mode; char path[1024]; } open; int32_t target; char directory[1024]; } value;
} NativeAction;
typedef struct { int32_t allocated, count; NativeAction actions[]; } NativeActions;
_Static_assert(sizeof(NativeAction) == 1040, "Pinned Darwin file-action layout changed");
_Static_assert(offsetof(NativeAction, value.open.path) == 14, "Pinned Darwin open-action path offset changed");
_Static_assert(sizeof(void *) == 8, "Pinned spawn attributes require the 64-bit Darwin ABI");
_Static_assert(sizeof(sigset_t) == sizeof(uint32_t), "Spawn wire signal sets require Darwin's 32-bit sigset_t");

static int readAttributes(LSBSpawnPlan *plan, const posix_spawnattr_t *attributes) {
    if (sigprocmask(SIG_SETMASK, NULL, (sigset_t *)&plan->signalMask)) return errno;
    for (int number = 1; number < NSIG; ++number) {
        if (number == SIGKILL || number == SIGSTOP) continue;
        struct sigaction action;
        if (sigaction(number, NULL, &action)) return errno;
        if (action.sa_handler == SIG_IGN) plan->signalIgnored |= 1u << (number - 1);
    }
    plan->processGroup = getpgrp();
    if (!attributes) return 0;
    if (!*attributes) return EINVAL;
    short flags;
    int error = posix_spawnattr_getflags(attributes, &flags);
    if (error) return error;
    plan->spawnFlags = (uint16_t)flags;
    uint32_t supported = POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (plan->spawnFlags & ~supported) return ENOTSUP;
    // Compare initialized nonstandard fields with their native defaults. Gaps are ABI padding left uninitialized by libkernel.
    static const uint16_t fields[][2] = {{16, 64}, {64, 66}, {68, 107}, {108, 156}, {160, 169}, {172, 177}, {180, 256}};
    posix_spawnattr_t defaults;
    error = posix_spawnattr_init(&defaults);
    if (error) return error;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (memcmp((const char *)*attributes + fields[i][0], (const char *)defaults + fields[i][0], fields[i][1] - fields[i][0])) {
            error = ENOTSUP;
            break;
        }
    }
    posix_spawnattr_destroy(&defaults);
    if (error) return error;
    if ((flags & POSIX_SPAWN_SETSIGMASK) && (error = posix_spawnattr_getsigmask(attributes, (sigset_t *)&plan->signalMask))) return error;
    if ((flags & POSIX_SPAWN_SETSIGDEF) && (error = posix_spawnattr_getsigdefault(attributes, (sigset_t *)&plan->signalDefault))) return error;
    if ((flags & POSIX_SPAWN_SETPGROUP) && (error = posix_spawnattr_getpgroup(attributes, &plan->processGroup))) return error;
    return 0;
}

static int readActions(LSBSpawnPlan *plan, const posix_spawn_file_actions_t *actions) {
    if (!actions) return 0;
    if (!*actions) return EINVAL;
    const NativeActions *native = (const NativeActions *)*actions;
    if (native->count < 0 || native->allocated < native->count) return EINVAL;
    if (native->count > LSB_MAX_ACTIONS) return E2BIG;
    plan->actionCount = (uint32_t)native->count;
    for (uint32_t i = 0; i < plan->actionCount; ++i) {
        const NativeAction *source = &native->actions[i];
        LSBFileAction *action = &plan->actions[i];
        action->fd = source->fd;
        const char *path = NULL;
        switch (source->kind) {
            case 0:
                action->kind = LSB_ACTION_OPEN;
                action->flags = source->value.open.flags;
                action->mode = source->value.open.mode;
                path = source->value.open.path;
                break;
            case 1: action->kind = LSB_ACTION_CLOSE; break;
            case 2: action->kind = LSB_ACTION_DUP2; action->target = source->value.target; break;
            case 3: action->kind = LSB_ACTION_INHERIT; break;
            case 5: action->kind = LSB_ACTION_CHDIR; path = source->value.directory; break;
            case 6: action->kind = LSB_ACTION_FCHDIR; break;
            default: return ENOTSUP;
        }
        if (path) {
            if (strnlen(path, 1024) == 1024) return ENAMETOOLONG;
            int error = LSBPlanAddString(plan, path, &action->path);
            if (error) return error;
        }
    }
    return 0;
}

static int addVector(LSBSpawnPlan *plan, char *const vector[], uint32_t *count, uint32_t offsets[LSB_MAX_VECTOR]) {
    if (!vector) return EINVAL;
    while (vector[*count]) {
        if (*count == LSB_MAX_VECTOR) return E2BIG;
        int error = LSBPlanAddString(plan, vector[*count], &offsets[*count]);
        if (error) return error;
        ++*count;
    }
    return 0;
}

static int snapshotFDs(LSBSpawnPlan *plan, int descriptors[LSB_MAX_FDS]) {
    int limit = getdtablesize();
    int highest = 2;
    for (int fd = 0; fd < limit; ++fd) {
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0) { if (errno == EBADF) continue; return errno; }
        if (plan->fdCount == LSB_MAX_FDS) return EMFILE;
        plan->fds[plan->fdCount++] = (LSBInheritedFD){fd, (uint32_t)flags};
        highest = fd;
    }
    // Enumerate before duplicating, so staging descriptors cannot be mistaken for guest descriptors.
    for (uint32_t i = 0; i < plan->fdCount; ++i) {
        descriptors[i] = fcntl(plan->fds[i].number, F_DUPFD_CLOEXEC, highest + 1);
        if (descriptors[i] < 0) return errno;
    }
    return 0;
}

static int spawnOne(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                    char *const argv[], char *const envp[]) {
    if (!path || !*path || !argv || !argv[0] || !envp) return EINVAL;
    if (strnlen(path, LSB_PATH_BYTES) == LSB_PATH_BYTES) return ENAMETOOLONG;
    LSBSpawnPlan *plan = calloc(1, sizeof(*plan));
    if (!plan) return ENOMEM;
    plan->version = LSB_PLAN_VERSION;
    int descriptors[LSB_MAX_FDS];
    for (uint32_t i = 0; i < LSB_MAX_FDS; ++i) descriptors[i] = -1;
    int error = LSBPlanAddString(plan, path, &plan->path);
    char directory[LSB_PATH_BYTES];
    if (!error && !getcwd(directory, sizeof(directory))) error = errno;
    if (!error) error = LSBPlanAddString(plan, directory, &plan->cwd);
    mode_t (*queryMask)(void) = (mode_t (*)(void))dlsym(RTLD_DEFAULT, "getumask");
    if (!error && !queryMask) error = ENOTSUP;
    if (!error) plan->creationMask = queryMask();
    if (!error) error = addVector(plan, argv, &plan->argc, plan->argv);
    if (!error) error = addVector(plan, envp, &plan->envc, plan->envp);
    if (!error) error = readAttributes(plan, attributes);
    if (!error) error = readActions(plan, actions);
    if (!error) error = snapshotFDs(plan, descriptors);
    if (!error) error = LSBValidateSpawnPlan(plan);
    pid_t child = -1;
    if (!error) error = LSBTransportSpawn(plan, descriptors, &child);
    for (uint32_t i = 0; i < plan->fdCount; ++i) if (descriptors[i] >= 0) close(descriptors[i]);
    free(plan);
    if (!error && pid) *pid = child;
    return error;
}

int LSBPosixSpawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                  char *const argv[], char *const envp[]) {
    int saved = errno;
    int result = spawnOne(pid, path, actions, attributes, argv, envp);
    errno = saved;
    return result;
}

int LSBPosixSpawnP(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                   char *const argv[], char *const envp[]) {
    int saved = errno, result = ENOENT;
    if (!file || !*file) result = EINVAL;
    else if (strchr(file, '/')) result = spawnOne(pid, file, actions, attributes, argv, envp);
    else {
        const char *search = getenv("PATH");
        if (!search) search = "/usr/bin:/bin";
        bool denied = false;
        do {
            const char *end = strchr(search, ':');
            size_t length = end ? (size_t)(end - search) : strlen(search);
            char candidate[LSB_PATH_BYTES];
            size_t fileLength = strnlen(file, sizeof(candidate));
            if (length + fileLength + 2 > sizeof(candidate)) result = ENAMETOOLONG;
            else {
                size_t offset = 0;
                if (length) { memcpy(candidate, search, length); candidate[length] = '/'; offset = length + 1; }
                memcpy(candidate + offset, file, fileLength + 1);
                result = spawnOne(pid, candidate, actions, attributes, argv, envp);
            }
            if (!result) break;
            if (result == EACCES) denied = true;
            else if (result != ENOENT && result != ENOTDIR) break;
            if (!end) { if (denied) result = EACCES; break; }
            search = end + 1;
        } while (true);
    }
    errno = saved;
    return result;
}

#ifndef LSB_SPAWN_SHIM_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBPosixSpawn, (const void *)&posix_spawn}, {(const void *)&LSBPosixSpawnP, (const void *)&posix_spawnp}
};
#endif
