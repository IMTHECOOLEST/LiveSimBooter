#include "../ProcessSupport/SpawnTransport.h"
#include "ChildWait.h"
#include "CreationMask.h"
#include "SpawnAttributes.h"
#include "ProcessSession.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/fileport.h>
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

static int readAttributes(LSBSpawnPlan *plan, const posix_spawnattr_t *attributes, LSBAdditionalSpawnAttributes *additional) {
    if (sigprocmask(SIG_SETMASK, NULL, (sigset_t *)&plan->signalMask)) return errno;
    for (int number = 1; number < NSIG; ++number) {
        if (number == SIGKILL || number == SIGSTOP) continue;
        struct sigaction action;
        if (sigaction(number, NULL, &action)) return errno;
        if (action.sa_handler == SIG_IGN) plan->signalIgnored |= 1u << (number - 1);
    }
    plan->processGroup = LSBGuestProcessGroup();
    memset(additional, 0, sizeof(*additional));
    if (!attributes) return 0;
    if (!*attributes) return EINVAL;
    short flags;
    int error = posix_spawnattr_getflags(attributes, &flags);
    if (error) return error;
    plan->spawnFlags = (uint16_t)flags;
    uint32_t supported = POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
                         POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETEXEC;
    if (plan->spawnFlags & ~supported) return ENOTSUP;
    if (flags & POSIX_SPAWN_SETPGROUP) {
        pid_t session, group;
        error = LSBProcessSessionSnapshot(&session, &group);
        // Logical sessions have no broker group model. Do not apply a guest PGID to the native extension process.
        if (error != ENOTSUP) return error ? error : ENOTSUP;
    }
    error = LSBReadAdditionalSpawnAttributes(attributes, additional);
    if (error) return error;
    // The extension keeps its native scheduler policy. Stock launchd's DAEMON_ADAPTIVE (0x600) hint is accepted but not applied.
    if ((additional->processType && additional->processType != 0x600) || additional->darwinRole) return ENOTSUP;
    if ((flags & POSIX_SPAWN_SETSIGMASK) && (error = posix_spawnattr_getsigmask(attributes, (sigset_t *)&plan->signalMask))) return error;
    if ((flags & POSIX_SPAWN_SETSIGDEF) && (error = posix_spawnattr_getsigdefault(attributes, (sigset_t *)&plan->signalDefault))) return error;
    if ((flags & POSIX_SPAWN_SETPGROUP) && (error = posix_spawnattr_getpgroup(attributes, &plan->processGroup))) return error;
    return 0;
}

static int readActions(LSBSpawnPlan *plan, const posix_spawn_file_actions_t *actions, int temporaryFDs[LSB_MAX_ACTIONS]) {
    if (!actions) return 0;
    if (!*actions) return EINVAL;
    const NativeActions *native = (const NativeActions *)*actions;
    if (native->count < 0 || native->allocated < native->count) return EINVAL;
    if (native->count > LSB_MAX_ACTIONS) return E2BIG;
    int highest = 2;
    for (int32_t i = 0; i < native->count; ++i) {
        const NativeAction *action = &native->actions[i];
        if (action->kind != 4 && action->kind != 5 && action->fd > highest) highest = action->fd;
        if ((action->kind == 2 || action->kind == 4) && action->value.target > highest) highest = action->value.target;
    }
    if (highest >= getdtablesize() - 1) return EMFILE;
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
            case 4: {
                // fileportdup2 stores the borrowed port name in fd and the target fd in value.target (pinned stride 0x410).
                int imported = fileport_makefd((mach_port_t)source->fd);
                if (imported < 0) return errno;
                int staged = fcntl(imported, F_DUPFD_CLOEXEC, highest + 1);
                int error = staged < 0 ? errno : 0;
                close(imported);
                if (error) return error;
                temporaryFDs[i] = staged;
                action->kind = LSB_ACTION_DUP2;
                action->fd = staged;
                action->target = source->value.target;
                break;
            }
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

static int snapshotPorts(const LSBSpawnPorts *requested, LSBSpawnPorts *owned) {
    memset(owned, 0, sizeof(*owned));
    if (requested->registeredPresent) {
        owned->registeredPresent = true;
        for (uint32_t i = 0; i < requested->count; ++i) {
            mach_port_t port = requested->registered[i];
            if (MACH_PORT_VALID(port) && mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1)) return EINVAL;
            owned->registered[owned->count++] = port;
        }
    } else {
        mach_port_array_t inherited = NULL;
        mach_msg_type_number_t count = 0;
        kern_return_t result = mach_ports_lookup(mach_task_self(), &inherited, &count);
        if (result != KERN_SUCCESS) return EIO;
        if (count > LSB_MAX_REGISTERED_PORTS) {
            for (mach_msg_type_number_t i = 0; i < count; ++i)
                if (MACH_PORT_VALID(inherited[i])) mach_port_deallocate(mach_task_self(), inherited[i]);
            if (inherited) vm_deallocate(mach_task_self(), (vm_address_t)inherited, count * sizeof(*inherited));
            return E2BIG;
        }
        owned->registeredPresent = true;
        owned->count = count;
        for (mach_msg_type_number_t i = 0; i < count; ++i) owned->registered[i] = inherited[i];
        if (inherited) vm_deallocate(mach_task_self(), (vm_address_t)inherited, count * sizeof(*inherited));
    }
    if (requested->bootstrapPresent) {
        if (MACH_PORT_VALID(requested->bootstrapPort) &&
            mach_port_mod_refs(mach_task_self(), requested->bootstrapPort, MACH_PORT_RIGHT_SEND, 1)) return EINVAL;
        owned->bootstrapPort = requested->bootstrapPort;
    } else if (task_get_bootstrap_port(mach_task_self(), &owned->bootstrapPort) != KERN_SUCCESS) return EIO;
    owned->bootstrapPresent = true;
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

static int setEnvironment(LSBSpawnPlan *plan, const char *key, pid_t value) {
    size_t keyLength = strlen(key);
    unsigned index = 0;
    for (; index < plan->envc; ++index) {
        const char *entry = plan->strings + plan->envp[index];
        if (!strncmp(entry, key, keyLength) && entry[keyLength] == '=') break;
    }
    if (index == LSB_MAX_VECTOR) return E2BIG;
    char entry[80];
    int length = snprintf(entry, sizeof(entry), "%s=%d", key, value);
    if (length < 0 || (size_t)length >= sizeof(entry)) return E2BIG;
    int error = LSBPlanAddString(plan, entry, &plan->envp[index]);
    if (!error && index == plan->envc) ++plan->envc;
    return error;
}

static int snapshotSession(LSBSpawnPlan *plan) {
    pid_t sid, group;
    int error = LSBProcessSessionSnapshot(&sid, &group);
    if (error == ENOTSUP) return 0;
    if (!error) error = setEnvironment(plan, LSB_SESSION_ENABLE_ENV, 1);
    if (!error) error = setEnvironment(plan, LSB_SESSION_ID_ENV, sid);
    if (!error) error = setEnvironment(plan, LSB_PROCESS_GROUP_ENV, group);
    return error;
}

static int snapshotFDs(LSBSpawnPlan *plan, int descriptors[LSB_MAX_FDS]) {
    int limit = getdtablesize();
    int wakeFD = LSBGuestChildWakeFD();
    int highest = 2;
    for (int fd = 0; fd < limit; ++fd) {
        if (fd == wakeFD) continue;
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
    int temporaryFDs[LSB_MAX_ACTIONS];
    for (uint32_t i = 0; i < LSB_MAX_ACTIONS; ++i) temporaryFDs[i] = -1;
    LSBAdditionalSpawnAttributes additional = {0};
    LSBSpawnPorts ports = {0};
    int error = LSBPlanAddString(plan, path, &plan->path);
    char directory[LSB_PATH_BYTES];
    if (!error && !getcwd(directory, sizeof(directory))) error = errno;
    if (!error) error = LSBPlanAddString(plan, directory, &plan->cwd);
    if (!error) error = LSBReadCreationMask(&plan->creationMask);
    if (!error) error = addVector(plan, argv, &plan->argc, plan->argv);
    if (!error) error = addVector(plan, envp, &plan->envc, plan->envp);
    if (!error) error = snapshotSession(plan);
    if (!error) error = readAttributes(plan, attributes, &additional);
    if (!error) error = snapshotPorts(&additional.ports, &ports);
    bool replace = (plan->spawnFlags & POSIX_SPAWN_SETEXEC) != 0;
    plan->spawnFlags &= ~POSIX_SPAWN_SETEXEC;
    if (!error) error = readActions(plan, actions, temporaryFDs);
    if (!error) error = snapshotFDs(plan, descriptors);
    if (!error) error = LSBValidateSpawnPlan(plan);
    pid_t child = -1;
    if (!error) {
        if (replace) error = LSBTransportExecWithPorts(plan, descriptors, &ports);
        else error = LSBTransportSpawnWithPorts(plan, descriptors, &ports, &child);
    }
    for (uint32_t i = 0; i < plan->fdCount; ++i) if (descriptors[i] >= 0) close(descriptors[i]);
    for (uint32_t i = 0; i < LSB_MAX_ACTIONS; ++i) if (temporaryFDs[i] >= 0) close(temporaryFDs[i]);
    LSBSpawnPortsDispose(&ports);
    free(plan);
    // The broker retires this process after accepting the replacement. Never resume the proxy's old image on a successful SETEXEC.
    if (!error && replace) _exit(0);
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
