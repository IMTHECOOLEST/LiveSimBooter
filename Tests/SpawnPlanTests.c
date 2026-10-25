#define LSB_SPAWN_SHIM_TESTING 1
#include "../Guest/SpawnShim.c"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>

static unsigned transportCalls;
static int transportFailure;
static int pathSearch;
static int internalWakeFD = -1;
static int creationMaskError;
static uint32_t capturedCreationMask = 027;
int LSBGuestChildWakeFD(void) { return internalWakeFD; }
// CreationMaskTests exercises the kernel-aware snapshot; this suite verifies serialization and error propagation.
int LSBReadCreationMask(uint32_t *mask) {
    if (creationMaskError) return creationMaskError;
    *mask = capturedCreationMask;
    return 0;
}

int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid) {
    assert(LSBValidateSpawnPlan(plan) == 0);
    assert(plan->creationMask == capturedCreationMask);
    for (uint32_t i = 0; i < plan->fdCount; ++i) assert(fcntl(fds[i], F_GETFD) >= 0);
    ++transportCalls;
    if (pathSearch) {
        assert(!strcmp(plan->strings + plan->path, transportCalls == 1 ? "/first/tool" : "/second/tool"));
        if (transportCalls == 1) return ENOENT;
    }
    if (transportFailure) return transportFailure;
    *pid = 4242;
    return 0;
}

static LSBSpawnPlan *newPlan(void) {
    LSBSpawnPlan *plan = calloc(1, sizeof(*plan));
    assert(plan);
    plan->version = LSB_PLAN_VERSION;
    plan->argc = 1;
    assert(LSBPlanAddString(plan, "/bin/test", &plan->path) == 0);
    plan->argv[0] = plan->path;
    assert(LSBPlanAddString(plan, "/", &plan->cwd) == 0);
    return plan;
}

static void emptyFDs(int fds[LSB_MAX_FDS]) { for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1; }
static void importFD(LSBSpawnPlan *plan, int fds[LSB_MAX_FDS], int source, int destination, int flags) {
    assert(plan->fdCount < LSB_MAX_FDS);
    fds[plan->fdCount] = dup(source);
    assert(fds[plan->fdCount] >= 0);
    plan->fds[plan->fdCount++] = (LSBInheritedFD){destination, (uint32_t)flags};
}

#define CHECK(condition) do { if (!(condition)) _exit((__LINE__ % 200) + 1); } while (0)

static void runChild(void (*test)(void), const char *name) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { test(); _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "%s failed: status=%d\n", name, status);
        abort();
    }
}

static void validation(void) {
    LSBSpawnPlan *plan = newPlan();
    assert(LSBValidateSpawnPlan(plan) == 0);
    plan->argc = LSB_MAX_VECTOR + 1;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argc = 1;
    plan->argv[0] = plan->stringBytes;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argv[0] = plan->path + 1;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argv[0] = plan->path;
    plan->strings[plan->stringBytes - 1] = 'x';
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->strings[plan->stringBytes - 1] = 0;
    plan->spawnFlags = POSIX_SPAWN_SETEXEC;
    assert(LSBValidateSpawnPlan(plan) == ENOTSUP);
    plan->spawnFlags = 0;
    plan->actionCount = 1;
    plan->actions[0] = (LSBFileAction){.kind = 100, .fd = 0};
    assert(LSBValidateSpawnPlan(plan) == ENOTSUP);
    plan->actionCount = 0;
    plan->fdCount = 2;
    plan->fds[0].number = plan->fds[1].number = 10;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->fdCount = 0;
    uint32_t unused;
    char *tooLong = malloc(LSB_STRING_BYTES + 1);
    assert(tooLong);
    memset(tooLong, 'x', LSB_STRING_BYTES);
    tooLong[LSB_STRING_BYTES] = 0;
    assert(LSBPlanAddString(plan, tooLong, &unused) == E2BIG);
    free(tooLong);
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    received[0] = open("/dev/null", O_RDONLY);
    assert(received[0] >= 0);
    int copied = received[0];
    plan->fdCount = 1;
    plan->version = 0;
    assert(LSBApplySpawnPlan(plan, received) == EINVAL);
    assert(received[0] == -1 && fcntl(copied, F_GETFD) == -1 && errno == EBADF);
    free(plan);
}

static void cyclesAndOrdering(void) {
    int a[2], b[2];
    CHECK(!pipe(a) && !pipe(b));
    CHECK(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
    CHECK(dup2(a[0], 61) == 61 && dup2(b[0], 60) == 60);
    LSBSpawnPlan *plan = newPlan();
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    // Imported descriptors themselves occupy each other's destination numbers.
    received[0] = 61;
    received[1] = 60;
    plan->fdCount = 2;
    plan->fds[0] = (LSBInheritedFD){60, 0};
    plan->fds[1] = (LSBInheritedFD){61, 0};
    plan->actionCount = 4;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 62};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 61, .target = 60};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 62, .target = 61};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = 62};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    char first, second;
    CHECK(read(60, &first, 1) == 1 && first == 'b');
    CHECK(read(61, &second, 1) == 1 && second == 'a');
    CHECK(fcntl(62, F_GETFD) == -1 && errno == EBADF);
    CHECK(received[0] == -1 && received[1] == -1);
}

static void closeOnExecVariant(bool defaults) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = defaults ? POSIX_SPAWN_CLOEXEC_DEFAULT : 0;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    int source = open("/dev/null", O_RDWR);
    CHECK(source >= 0);
    importFD(plan, received, source, 60, FD_CLOEXEC);
    importFD(plan, received, source, 62, 0);
    importFD(plan, received, source, 63, FD_CLOEXEC);
    importFD(plan, received, source, 64, FD_CLOEXEC);
    plan->actionCount = 3;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 61};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = 63};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 64, .target = 64};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(fcntl(60, F_GETFD) == -1);
    CHECK(fcntl(61, F_GETFD) == 0);
    CHECK(defaults ? fcntl(62, F_GETFD) == -1 : fcntl(62, F_GETFD) == 0);
    CHECK(fcntl(63, F_GETFD) == 0);
    CHECK(fcntl(64, F_GETFD) == -1);
}

static void normalCloseOnExec(void) { closeOnExecVariant(false); }
static void defaultCloseOnExec(void) { closeOnExecVariant(true); }

static void preservedInternalDescriptor(void) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    int source = open("/dev/null", O_RDWR);
    CHECK(source >= 0);
    internalWakeFD = fcntl(source, F_DUPFD_CLOEXEC, 90);
    CHECK(internalWakeFD >= 90);
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    importFD(plan, received, source, 60, 0);
    CHECK(LSBApplySpawnPlanPreservingFD(plan, received, internalWakeFD) == 0);
    CHECK(fcntl(internalWakeFD, F_GETFD) == FD_CLOEXEC);
    CHECK(fcntl(60, F_GETFD) == -1 && errno == EBADF);
    LSBSpawnPlan *snapshot = newPlan();
    emptyFDs(received);
    CHECK(snapshotFDs(snapshot, received) == 0 && snapshot->fdCount == 0);
    CHECK(close(internalWakeFD) == 0);
}

static char workingDirectory[LSB_PATH_BYTES];
static void opensAndDirectories(void) {
    LSBSpawnPlan *plan = newPlan();
    CHECK(LSBPlanAddString(plan, workingDirectory, &plan->cwd) == 0);
    plan->creationMask = 0027;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    int directory = open(workingDirectory, O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0);
    importFD(plan, received, directory, 62, 0);
    uint32_t nested, made, after;
    CHECK(LSBPlanAddString(plan, "nested", &nested) == 0);
    CHECK(LSBPlanAddString(plan, "made", &made) == 0);
    CHECK(LSBPlanAddString(plan, "after", &after) == 0);
    plan->actionCount = 6;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_CHDIR, .path = nested};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = 60, .flags = O_CREAT | O_WRONLY, .mode = 0666, .path = made};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 61};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = 60};
    plan->actions[4] = (LSBFileAction){.kind = LSB_ACTION_FCHDIR, .fd = 62};
    plan->actions[5] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = 63, .flags = O_CREAT | O_WRONLY, .mode = 0600, .path = after};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(write(61, "made", 4) == 4 && write(63, "after", 5) == 5);
    struct stat info;
    CHECK(!stat("nested/made", &info) && (info.st_mode & 0777) == 0640 && info.st_size == 4);
    CHECK(!stat("after", &info) && info.st_size == 5);
    char cwd[LSB_PATH_BYTES];
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, workingDirectory));
}

static void attributesAndFailure(void) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_RESETIDS;
    plan->signalMask = 1u << (SIGUSR1 - 1);
    plan->signalIgnored = (1u << (SIGUSR2 - 1)) | (1u << (SIGPIPE - 1));
    plan->signalDefault = 1u << (SIGUSR2 - 1);
    plan->processGroup = 0;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(getpgrp() == getpid());
    sigset_t mask;
    CHECK(!sigprocmask(SIG_SETMASK, NULL, &mask) && sigismember(&mask, SIGUSR1));
    struct sigaction action;
    CHECK(!sigaction(SIGUSR2, NULL, &action) && action.sa_handler == SIG_DFL);
    CHECK(!sigaction(SIGPIPE, NULL, &action) && action.sa_handler == SIG_IGN);
    plan->actionCount = 1;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 70, .target = 71};
    CHECK(LSBApplySpawnPlan(plan, received) == EBADF);
}

static void defaultProcessGroup(void) {
    LSBSpawnPlan *plan = newPlan();
    pid_t original = getpgrp();
    // The requested parent's group is metadata unless SETPGROUP is explicitly present.
    plan->processGroup = INT_MAX;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(LSBApplySpawnPlan(plan, received) == 0 && getpgrp() == original);
}

static void shimSerialization(void) {
    LSBSpawnPlan *plan = newPlan();
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(readAttributes(plan, &attributes) == 0);
    assert(!posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC));
    assert(readAttributes(plan, &attributes) == ENOTSUP);
    assert(!posix_spawnattr_setflags(&attributes, 0));
    cpu_type_t architecture = CPU_TYPE_ARM64;
    size_t count;
    assert(!posix_spawnattr_setbinpref_np(&attributes, 1, &architecture, &count));
    assert(readAttributes(plan, &attributes) == ENOTSUP);
    assert(!posix_spawnattr_destroy(&attributes));
    free(plan);

    posix_spawn_file_actions_t actions;
    assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawn_file_actions_addopen(&actions, 60, "output", O_CREAT | O_WRONLY, 0640));
    assert(!posix_spawn_file_actions_adddup2(&actions, 60, 61));
    assert(!posix_spawn_file_actions_addclose(&actions, 60));
    plan = newPlan();
    assert(readActions(plan, &actions) == 0 && plan->actionCount == 3);
    assert(plan->actions[0].kind == LSB_ACTION_OPEN && plan->actions[0].mode == 0640);
    assert(!strcmp(plan->strings + plan->actions[0].path, "output"));
    assert(plan->actions[1].kind == LSB_ACTION_DUP2 && plan->actions[1].fd == 60 && plan->actions[1].target == 61);
    assert(plan->actions[2].kind == LSB_ACTION_CLOSE && plan->actions[2].fd == 60);
    assert(!posix_spawn_file_actions_destroy(&actions));
    free(plan);

    char *argv[] = {"tool", "one argument", NULL}, *environment[] = {"KEY=value", NULL};
    pid_t pid = 123;
    errno = EDOM;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == 0 && pid == 4242 && errno == EDOM);
    unsigned beforeCalls = transportCalls;
    creationMaskError = EBUSY;
    pid = 123;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == EBUSY && pid == 123 && errno == EDOM);
    assert(transportCalls == beforeCalls);
    creationMaskError = 0;
    transportFailure = EACCES;
    pid = 123;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == EACCES && pid == 123 && errno == EDOM);
    transportFailure = 0;
    pathSearch = 1;
    transportCalls = 0;
    assert(!setenv("PATH", "/first:/second", 1));
    assert(LSBPosixSpawnP(&pid, "tool", NULL, NULL, argv, environment) == 0 && transportCalls == 2 && pid == 4242 && errno == EDOM);
}

int main(void) {
    validation();
    runChild(cyclesAndOrdering, "descriptor cycles and ordered actions");
    runChild(normalCloseOnExec, "close-on-exec");
    runChild(defaultCloseOnExec, "close-on-exec default");
    runChild(preservedInternalDescriptor, "internal wake descriptor survives and is not inherited");
    runChild(attributesAndFailure, "signal attributes and action errors");
    runChild(defaultProcessGroup, "default extension process group");
    strcpy(workingDirectory, "/tmp/LiveSimBooter-spawn-tests.XXXXXX");
    assert(mkdtemp(workingDirectory));
    char resolved[LSB_PATH_BYTES];
    assert(realpath(workingDirectory, resolved));
    strcpy(workingDirectory, resolved);
    char childPath[LSB_PATH_BYTES + 32];
    snprintf(childPath, sizeof(childPath), "%s/nested", workingDirectory);
    assert(!mkdir(childPath, 0700));
    runChild(opensAndDirectories, "open, chdir and fchdir ordering");
    snprintf(childPath, sizeof(childPath), "%s/nested/made", workingDirectory);
    assert(!unlink(childPath));
    snprintf(childPath, sizeof(childPath), "%s/nested", workingDirectory);
    assert(!rmdir(childPath));
    snprintf(childPath, sizeof(childPath), "%s/after", workingDirectory);
    assert(!unlink(childPath) && !rmdir(workingDirectory));
    runChild(shimSerialization, "guest attribute and file-action serialization");
    puts("Spawn plan tests passed");
    return 0;
}
