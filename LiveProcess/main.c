#include "DyldSimLoader.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "ChildStatus.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static LSBBootstrapReply launch;
static LSBMappedImage loader, executable;
static LSBChildStatus *childStatus;

static void closeStatus(void) {
    if (launch.statusFD >= 0) close(launch.statusFD);
    if (launch.wakeFD >= 0) close(launch.wakeFD);
    launch.statusFD = launch.wakeFD = -1;
    if (childStatus) munmap(childStatus, LSB_CHILD_STATUS_MAP_SIZE);
    childStatus = NULL;
}

static int prepareStatus(void) {
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, launch.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    childStatus = mapping;
    close(launch.statusFD);
    launch.statusFD = -1;
    int error = LSBChildStatusValidate(childStatus);
    if (error) return error;
    int highest = 2;
    for (unsigned i = 0; i < launch.plan.fdCount; ++i) if (launch.plan.fds[i].number > highest) highest = launch.plan.fds[i].number;
    for (unsigned i = 0; i < launch.plan.actionCount; ++i) {
        const LSBFileAction *action = &launch.plan.actions[i];
        if (action->kind != LSB_ACTION_CHDIR && action->fd > highest) highest = action->fd;
        if (action->kind == LSB_ACTION_DUP2 && action->target > highest) highest = action->target;
    }
    // Keep the private wake endpoint out of the guest's ordered file-action destinations and both close sweeps.
    int staged = fcntl(launch.wakeFD, F_DUPFD_CLOEXEC, highest + 1);
    if (staged < 0) return errno;
    close(launch.wakeFD);
    launch.wakeFD = staged;
    return 0;
}

static int resolveExecutable(char output[PATH_MAX]) {
    const char *path = launch.plan.strings + launch.plan.path;
    size_t rootLength = strlen(launch.runtimeRoot);
    char candidate[PATH_MAX];
    int size;
    if (!strncmp(path, launch.runtimeRoot, rootLength) && path[rootLength] == '/') size = snprintf(candidate, sizeof(candidate), "%s", path);
    else if (path[0] == '/') size = snprintf(candidate, sizeof(candidate), "%s%s", launch.runtimeRoot, path);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return errno;
        size = snprintf(candidate, sizeof(candidate), "%s/%s", cwd, path);
    }
    if (size < 0 || (size_t)size >= sizeof(candidate)) return ENAMETOOLONG;
    if (!realpath(candidate, output)) return errno;
    if (strncmp(output, launch.runtimeRoot, rootLength) || output[rootLength] != '/') return EACCES;
    return 0;
}

int NSExtensionMain(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    (void)argc; (void)argv; (void)envp;
    // Cancel launchd's extension-alive timer through its C API before any broker retry.
    launch_extension_check_in_live_4UIKit();
    int error = LSBRequireSingleThread();
    if (error) { fprintf(stderr, "[LiveProcess] Native worker at startup: %s\n", strerror(error)); return 1; }
    int received[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) received[i] = -1;
    error = EAGAIN;
    // PlugInKit can launch our C entry before the host-side connection exposes its PID.
    for (unsigned attempt = 0; attempt < 100 && error == EAGAIN; ++attempt) {
        error = LSBTransportCheckIn(&launch, received);
        if (error == EAGAIN) usleep(100000);
    }
    if (error) { fprintf(stderr, "[LiveProcess] Check-in failed: %s\n", strerror(error)); return 1; }
    error = prepareStatus();
    if (!error) error = LSBApplySpawnPlanPreservingFD(&launch.plan, received, launch.wakeFD);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) if (received[i] >= 0) { close(received[i]); received[i] = -1; }
    char executablePath[PATH_MAX], dyldPath[PATH_MAX], insertLibrary[PATH_MAX + 32], brokerEnvironment[64], detail[1024] = {0};
    char statusEnvironment[64], wakeEnvironment[64];
    mach_port_t brokerPort = MACH_PORT_NULL;
    if (!error) error = resolveExecutable(executablePath);
    int length = snprintf(dyldPath, sizeof(dyldPath), "%s/usr/lib/dyld_sim", launch.runtimeRoot);
    if (!error && (length < 0 || (size_t)length >= sizeof(dyldPath))) error = ENAMETOOLONG;
    length = snprintf(insertLibrary, sizeof(insertLibrary), "DYLD_INSERT_LIBRARIES=%s/usr/lib/libLiveSimSpawn.dylib", launch.runtimeRoot);
    if (!error && (length < 0 || (size_t)length >= sizeof(insertLibrary))) error = ENAMETOOLONG;
    if (!error && !LSBMapDyldSim(dyldPath, &loader, detail, sizeof(detail))) error = ENOEXEC;
    if (!error && !LSBMapSimExecutable(executablePath, &executable, detail, sizeof(detail))) error = ENOEXEC;
    // Keep only a kernel send right across detachment; simulator libxpc cannot look up services without simulator launchd.
    if (!error) error = LSBTransportCopyBrokerPort(&brokerPort);
    snprintf(brokerEnvironment, sizeof(brokerEnvironment), "LSB_BROKER_PORT=%u", brokerPort);
    snprintf(statusEnvironment, sizeof(statusEnvironment), LSB_CHILD_STATUS_ENV "=0x%llx", (unsigned long long)(uintptr_t)childStatus);
    snprintf(wakeEnvironment, sizeof(wakeEnvironment), LSB_CHILD_WAKE_FD_ENV "=%d", launch.wakeFD);
    if (!error) error = LSBRequireSingleThread();
    int readyError = LSBTransportReady(launch.requestID, error);
    if (error || readyError) {
        if (MACH_PORT_VALID(brokerPort)) mach_port_deallocate(mach_task_self(), brokerPort);
        closeStatus();
        fprintf(stderr, "[LiveProcess] Preparation failed: %s %s\n", strerror(error ? error : readyError), detail);
        return 1;
    }
    // All native XPC objects have been released. No native dispatch queue has been created.
    if ((error = LSBRequireSingleThread())) {
        mach_port_deallocate(mach_task_self(), brokerPort);
        closeStatus();
        fprintf(stderr, "[LiveProcess] Native worker appeared before handoff: %s\n", strerror(error)); return 1;
    }
    const char *arguments[LSB_MAX_VECTOR + 1], *environment[LSB_MAX_VECTOR + 5];
    for (unsigned i = 0; i < launch.plan.argc; ++i) arguments[i] = launch.plan.strings + launch.plan.argv[i];
    arguments[launch.plan.argc] = NULL;
    unsigned count = 0;
    for (unsigned i = 0; i < launch.plan.envc; ++i) {
        const char *entry = launch.plan.strings + launch.plan.envp[i];
        if (strncmp(entry, "DYLD_INSERT_LIBRARIES=", 22) && strncmp(entry, "LSB_BROKER_PORT=", 16) &&
            strncmp(entry, LSB_CHILD_STATUS_ENV "=", sizeof(LSB_CHILD_STATUS_ENV)) &&
            strncmp(entry, LSB_CHILD_WAKE_FD_ENV "=", sizeof(LSB_CHILD_WAKE_FD_ENV))) environment[count++] = entry;
    }
    environment[count++] = insertLibrary;
    environment[count++] = brokerEnvironment;
    environment[count++] = statusEnvironment;
    environment[count++] = wakeEnvironment;
    environment[count] = NULL;
    fprintf(stderr, "[LiveProcess] pid=%d entering %s with %u arguments\n", getpid(), executablePath, launch.plan.argc);
    int result = LSBEnterDyldSim(&loader, executable.header, executablePath, dyldPath, launch.runtimeRoot,
                               (int)launch.plan.argc, arguments, environment, apple);
    // Successful detachment never returns; only native preparation/handoff failures reach this cleanup.
    mach_port_deallocate(mach_task_self(), brokerPort);
    closeStatus();
    return result;
}
