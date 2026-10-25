#import <Foundation/Foundation.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/wait.h>
#include "../Host/Broker.m"

typedef struct {
    unsigned objectID;
    uint32_t kind;
    int error, sendError;
    pid_t pid;
    uint64_t requestID;
    bool deliveredPlan;
    mach_port_t launchdTank;
} RecordedReply;

typedef struct { unsigned identifier; int retainedFD; } TestMessageObject;

static RecordedReply replies[128];
static unsigned replyCount, disposedMessages, submittedLaunches, releasedLaunches, objectSequence;
static pid_t reportedPID;
static int submitError, readySendError, launchdBootstrapSendError, monitorCreateError, identityCheckError, runningBoardError;
static unsigned deliveredSignals, identityChecks, runningBoardStarts, runningBoardCancels;
static unsigned submittedExtensionSignals;
static uint64_t signaledRequest;
static int directSignalError, extensionSignalError, lastSignal;
static audit_token_t signaledAudit;
static LSBExtensionEvent extensionCallback;

int LSBRunningBoardValidateIdentity(const audit_token_t *audit) {
    ++identityChecks;
    bool found = false;
    for (LSBJob *job in jobs) if (audit->val[5] == (unsigned)job.pid && audit->val[7] == job.pidVersion) found = true;
    assert(found);
    return identityCheckError;
}
@interface LSBRunningBoardMonitor : NSObject
@property (nonatomic, copy) LSBRunningBoardExitHandler completion;
@end
@implementation LSBRunningBoardMonitor
@end
int LSBRunningBoardMonitorStart(const audit_token_t *audit, LSBRunningBoardExitHandler handler, LSBRunningBoardMonitor **monitor) {
    assert(audit && handler && monitor);
    ++runningBoardStarts;
    if (runningBoardError) return runningBoardError;
    *monitor = [LSBRunningBoardMonitor new];
    (*monitor).completion = handler;
    return 0;
}
void LSBRunningBoardMonitorCancel(LSBRunningBoardMonitor *monitor) { monitor.completion = nil; ++runningBoardCancels; }

int LSBChildMonitorCreate(pid_t pid, int *descriptor) {
    assert(pid > 0 && descriptor);
    *descriptor = -1;
    if (monitorCreateError) return monitorCreateError;
    *descriptor = kqueue();
    assert(*descriptor >= 0);
    return 0;
}
int LSBChildMonitorRead(int descriptor, pid_t pid, int *status) {
    (void)descriptor; (void)pid; (void)status;
    return EAGAIN;
}
int LSBChildSignal(const audit_token_t *audit, int signal) {
    assert(audit && signal > 0 && signal < NSIG);
    signaledAudit = *audit;
    lastSignal = signal;
    ++deliveredSignals;
    return directSignalError;
}

static void releaseMessageObject(void *pointer) {
    TestMessageObject *object = pointer;
    if (!object) return;
    if (object->retainedFD >= 0) close(object->retainedFD);
    free(object);
}

int LSBLaunchExtension(const char *identifier, uint64_t requestID, LSBExtensionEvent event, void *context) {
    assert(!strcmp(identifier, LSB_EXTENSION_IDENTIFIER) && requestID && context == NULL);
    ++submittedLaunches;
    extensionCallback = event;
    int error = submitError;
    submitError = 0;
    return error;
}
pid_t LSBExtensionProcessIdentifier(uint64_t requestID) { assert(requestID); return reportedPID; }
void LSBReleaseExtension(uint64_t requestID) { assert(requestID); ++releasedLaunches; }
int LSBSignalExtension(uint64_t requestID, int signal) {
    assert(requestID && signal > 0 && signal < NSIG);
    signaledRequest = requestID;
    lastSignal = signal;
    ++submittedExtensionSignals;
    return extensionSignalError;
}

int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message) { (void)port; (void)message; return ENOSYS; }
int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    assert(message && message->object && replyCount < 128);
    int sendError = message->kind == LSB_REQUEST_READY ? readySendError : 0;
    if (plan) {
        assert(root && !strcmp(root, "/tmp/RuntimeRoot") && fds);
        for (unsigned i = 0; i < plan->fdCount; ++i) assert(fcntl(fds[i], F_GETFD) >= 0);
    }
    unsigned identifier = ((TestMessageObject *)message->object)->identifier;
    replies[replyCount++] = (RecordedReply){identifier, message->kind, error, sendError, pid, requestID, plan != NULL, MACH_PORT_NULL};
    releaseMessageObject(message->object);
    message->object = NULL;
    return sendError;
}
int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD) {
    assert(statusFD >= 0 && wakeFD >= 0 && (fcntl(wakeFD, F_GETFL) & O_NONBLOCK));
    struct stat metadata;
    assert(!fstat(statusFD, &metadata) && metadata.st_size == LSB_CHILD_STATUS_MAPPING_SIZE);
    return LSBTransportReply(message, error, pid, requestID, root, plan, fds);
}
int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tank) {
    mach_port_type_t type = 0;
    assert(tank && mach_port_type(mach_task_self(), tank, &type) == KERN_SUCCESS && (type & MACH_PORT_TYPE_SEND));
    LSBJob *job = findJob(requestID);
    assert(job && job.simulatorLaunchd && job.launchdSource && job.launchdHandshake);
    assert(tank == LSBLaunchdHandshakeTankPort(job.launchdHandshake) && !job.launchdHandshake->completed);
    assert(!memcmp(&message->audit, &job.launchdHandshake->expectedAudit, sizeof(message->audit)));
    int result = LSBTransportReplyBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD);
    replies[replyCount - 1].launchdTank = tank;
    if (launchdBootstrapSendError) replies[replyCount - 1].sendError = result = launchdBootstrapSendError;
    return result;
}
void LSBTransportDisposeMessage(LSBReceivedMessage *message) {
    assert(message);
    releaseMessageObject(message->object);
    message->object = NULL;
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) {
        if (message->fds[i] >= 0) close(message->fds[i]);
        message->fds[i] = -1;
    }
    ++disposedMessages;
}

static void resetBroker(void) {
    for (LSBJob *job in jobs) {
        if (job.exitMonitor) dispatch_source_cancel(job.exitMonitor);
        job.exitMonitor = nil;
        if (job.launchdSource) dispatch_source_cancel(job.launchdSource);
        job.launchdSource = nil;
        if (job.runningBoardMonitor) LSBRunningBoardMonitorCancel(job.runningBoardMonitor);
        job.runningBoardMonitor = nil;
        if (job.request) disposeMessage(job.request);
        job.request = NULL;
    }
    startingJob = nil;
    jobs = [NSMutableArray new];
    runtimeRoot = @"/tmp/RuntimeRoot";
    nextIdentifier = 1;
    launchLaneFailed = NO;
    reportedPID = getpid();
    submitError = readySendError = launchdBootstrapSendError = monitorCreateError = identityCheckError = 0;
    runningBoardError = EACCES;
    deliveredSignals = identityChecks = finishingDepth = 0;
    runningBoardStarts = runningBoardCancels = 0;
    submittedExtensionSignals = 0;
    signaledRequest = 0;
    directSignalError = extensionSignalError = lastSignal = 0;
    memset(&signaledAudit, 0, sizeof(signaledAudit));
    replyCount = disposedMessages = submittedLaunches = releasedLaunches = 0;
    memset(replies, 0, sizeof(replies));
}

static LSBReceivedMessage *messageFor(uint32_t kind, pid_t pid, uint32_t generation, uint64_t requestID) {
    LSBReceivedMessage *message = calloc(1, sizeof(*message));
    assert(message);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    TestMessageObject *object = malloc(sizeof(*object));
    assert(object);
    *object = (TestMessageObject){++objectSequence, -1};
    message->object = object;
    message->kind = kind;
    message->requestID = requestID;
    message->audit.val[5] = (unsigned)pid;
    message->audit.val[7] = generation;
    return message;
}

static LSBReceivedMessage *spawnRequest(int *ownedFD, unsigned *objectID) {
    LSBReceivedMessage *message = messageFor(LSB_REQUEST_SPAWN, getpid() + 100, 8, 0);
    message->plan.version = LSB_PLAN_VERSION;
    message->plan.argc = 1;
    assert(!LSBPlanAddString(&message->plan, "/bin/df", &message->plan.path));
    message->plan.argv[0] = message->plan.path;
    assert(!LSBPlanAddString(&message->plan, "/tmp/RuntimeRoot", &message->plan.cwd));
    message->plan.fdCount = 1;
    message->plan.fds[0].number = STDOUT_FILENO;
    message->fds[0] = open("/dev/null", O_RDWR);
    assert(message->fds[0] >= 0);
    *ownedFD = message->fds[0];
    *objectID = ((TestMessageObject *)message->object)->identifier;
    return message;
}

static RecordedReply *replyFor(unsigned objectID) {
    for (unsigned i = 0; i < replyCount; ++i) if (replies[i].objectID == objectID) return &replies[i];
    return NULL;
}
static unsigned planReplies(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < replyCount; ++i) if (replies[i].deliveredPlan) ++count;
    return count;
}
static void assertClosed(int fd) { errno = 0; assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF); }

static void testIdentityAndSuccessfulHandoff(void) {
    resetBroker();
    int fd;
    unsigned parentObject;
    enqueueSpawn(spawnRequest(&fd, &parentObject), nil);
    assert(startingJob && startingJob.phase == JobStarting && submittedLaunches == 1);
    uint64_t identifier = startingJob.identifier;
    reportedPID = 0;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(replies[replyCount - 1].error == EAGAIN && startingJob.phase == JobStarting && !planReplies());
    reportedPID = getpid();
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid() + 1, 3, 0));
    assert(replies[replyCount - 1].error == EACCES && !planReplies() && !startingJob.pid);
    assert(fcntl(fd, F_GETFD) >= 0);
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(planReplies() == 1 && startingJob.phase == JobDispatched && startingJob.pidVersion == 3);
    assertClosed(fd);
    assert(!replyFor(parentObject));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 4, identifier));
    assert(replies[replyCount - 1].error == ESRCH && replies[replyCount - 1].requestID == identifier && !replyFor(parentObject));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 3, identifier));
    assert(!startingJob && jobs.count == 1 && jobs[0].phase == JobRunning && jobs[0].request == NULL);
    RecordedReply *reply = replyFor(parentObject);
    assert(reply && !reply->error && reply->pid == getpid());
    assert(disposedMessages == 6);
}

static void testReadySendFailure(void) {
    resetBroker();
    int fd;
    unsigned parentObject;
    enqueueSpawn(spawnRequest(&fd, &parentObject), nil);
    uint64_t identifier = startingJob.identifier;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    readySendError = EPIPE;
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 3, identifier));
    RecordedReply *reply = replyFor(parentObject);
    assert(reply && reply->error == EPIPE && !reply->pid && !jobs.count && !startingJob && !launchLaneFailed);
    assertClosed(fd);
}

static void testChildReadyFailure(int childError) {
    resetBroker();
    int firstFD, nextFD, pipeFDs[2];
    unsigned parentObject, nextObject;
    LSBReceivedMessage *request = spawnRequest(&firstFD, &parentObject);
    assert(!pipe(pipeFDs));
    close(firstFD);
    request->fds[0] = pipeFDs[1];
    // An XPC FD object retains its own writer even after the broker closes the imported descriptor.
    TestMessageObject *object = request->object;
    object->retainedFD = dup(pipeFDs[1]);
    assert(object->retainedFD >= 0 && !fcntl(pipeFDs[0], F_SETFL, O_NONBLOCK));
    enqueueSpawn(request, nil);
    uint64_t identifier = startingJob.identifier;
    enqueueSpawn(spawnRequest(&nextFD, &nextObject), nil);
    assert(jobs.count == 2 && submittedLaunches == 1);

    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(startingJob.phase == JobDispatched && startingJob.request->fds[0] == -1 && !replyFor(parentObject));
    char byte;
    errno = 0;
    assert(read(pipeFDs[0], &byte, 1) < 0 && errno == EAGAIN);
    LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, getpid(), 3, identifier);
    unsigned readyObject = ((TestMessageObject *)ready->object)->identifier;
    ready->error = childError;
    handleMessage(ready);

    RecordedReply *parentReply = replyFor(parentObject), *childReply = replyFor(readyObject);
    assert(parentReply && parentReply->error == childError && !parentReply->pid && parentReply->requestID == identifier);
    assert(childReply && !childReply->error && !childReply->sendError && childReply->requestID == identifier);
    assert(disposedMessages == 3 && releasedLaunches == 1 && !findJob(identifier) && !launchLaneFailed);
    assert(read(pipeFDs[0], &byte, 1) == 0);
    close(pipeFDs[0]);

    assert(jobs.count == 1 && startingJob && startingJob.phase == JobStarting && startingJob.identifier != identifier);
    assert(submittedLaunches == 2 && !replyFor(nextObject) && fcntl(nextFD, F_GETFD) >= 0);
    uint64_t nextIdentifier = startingJob.identifier;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 4, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 4, nextIdentifier));
    RecordedReply *nextReply = replyFor(nextObject);
    assert(nextReply && !nextReply->error && nextReply->pid == getpid());
    assert(jobs.count == 1 && jobs[0].phase == JobRunning && !startingJob && !launchLaneFailed);
    assertClosed(nextFD);
}

static void testUnboundFailureStopsQueue(void) {
    resetBroker();
    int firstFD, secondFD, rejectedFD;
    unsigned firstObject, secondObject, rejectedObject;
    reportedPID = 0;
    enqueueSpawn(spawnRequest(&firstFD, &firstObject), nil);
    uint64_t identifier = startingJob.identifier;
    enqueueSpawn(spawnRequest(&secondFD, &secondObject), nil);
    assert(jobs.count == 2 && submittedLaunches == 1);
    extensionCallback(identifier, 0, ETIMEDOUT, NULL);
    assert(launchLaneFailed && !startingJob && !jobs.count && submittedLaunches == 1);
    assert(replyFor(firstObject)->error == ETIMEDOUT && replyFor(secondObject)->error == EBUSY);
    assertClosed(firstFD);
    assertClosed(secondFD);
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(replies[replyCount - 1].error == ESRCH && !planReplies());
    enqueueSpawn(spawnRequest(&rejectedFD, &rejectedObject), nil);
    assert(replyFor(rejectedObject)->error == EBUSY && submittedLaunches == 1);
    assertClosed(rejectedFD);
}

static void testSubmissionFailureAndSpawnAuthentication(void) {
    resetBroker();
    int fd;
    unsigned objectID;
    submitError = ENOENT;
    enqueueSpawn(spawnRequest(&fd, &objectID), nil);
    assert(!launchLaneFailed && !jobs.count && replyFor(objectID)->error == ENOENT);
    assertClosed(fd);

    LSBJob *parent = [LSBJob new];
    parent.identifier = 99;
    parent.pid = getpid() + 100;
    parent.pidVersion = 8;
    parent.phase = JobRunning;
    assert(!prepareChildStatus(parent));
    [jobs addObject:parent];
    LSBReceivedMessage *wrongGeneration = spawnRequest(&fd, &objectID);
    wrongGeneration->audit.val[7] = 9;
    handleMessage(wrongGeneration);
    assert(replyFor(objectID)->error == EACCES && jobs.count == 1);
    assertClosed(fd);
    handleMessage(spawnRequest(&fd, &objectID));
    assert(startingJob && jobs.count == 2 && submittedLaunches == 2 && !replyFor(objectID));
    finishJob(startingJob, ECANCELED);
    assert(replyFor(objectID)->error == ECANCELED);
    assertClosed(fd);
}

static void testInitialGuestArguments(void) {
    resetBroker();
    char directory[] = "/tmp/LiveSimBooter-terminal-test.XXXXXX";
    assert(mkdtemp(directory));
    runtimeRoot = [[NSString stringWithUTF8String:directory] stringByAppendingPathComponent:@"RuntimeRoot"];
    const char *environment[] = { "LSB_TEST=preserved", NULL };
    const char *defaultArguments[] = { "LiveSimBooter", NULL };
    const char *customArguments[] = { "LiveSimBooter", "/bin/df", "-h", NULL };
    for (unsigned custom = 0; custom < 2; ++custom) {
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        assert(initial);
        assert(!initialPlan(initial, custom ? 3 : 1, custom ? customArguments : defaultArguments, environment));
        LSBSpawnPlan *plan = &initial->plan;
        assert(plan->argc == (custom ? 2 : 6) && plan->envc == (custom ? 1 : 5));
        assert(!strcmp(plan->strings + plan->path, custom ? "/bin/df" : "/bin/fish"));
        assert(!strcmp(plan->strings + plan->argv[0], plan->strings + plan->path));
        if (custom) assert(!strcmp(plan->strings + plan->argv[1], "-h"));
        else {
            assert(hostTerminal && plan->fdCount == 3);
            struct stat first, other;
            assert(!fstat(initial->fds[0], &first) && S_ISSOCK(first.st_mode));
            for (unsigned i = 1; i < 3; ++i) {
                assert(!fstat(initial->fds[i], &other) && first.st_dev == other.st_dev && first.st_ino == other.st_ino);
            }
            assert(!strcmp(plan->strings + plan->argv[1], "-i"));
            assert(!strncmp(plan->strings + plan->envp[4], LSB_FAKE_PTY_ENV "=", sizeof(LSB_FAKE_PTY_ENV)));
        }
        assert(!strcmp(plan->strings + plan->envp[0], "LSB_TEST=preserved"));
        disposeMessage(initial);
        [hostTerminal close]; hostTerminal = nil;
    }
    assert([NSFileManager.defaultManager removeItemAtPath:[NSString stringWithUTF8String:directory] error:nil]);
}

static LSBJob *runTestJob(LSBJob *parent, uint32_t generation) {
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    enqueueSpawn(request, parent);
    assert(startingJob);
    LSBJob *job = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), generation, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), generation, job.identifier));
    assert(job.phase == JobRunning && !startingJob && replyFor(object) && !replyFor(object)->error);
    return job;
}

static void testExitStatusesAndRepeatedReaping(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 10);
    parent.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)getpid(), 7, 10}};
    for (unsigned iteration = 0; iteration < LSB_CHILD_STATUS_CAPACITY + 3; ++iteration) {
        LSBJob *child = runTestJob(parent, 11 + iteration);
        pid_t waited = -1;
        int status = -1;
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == EAGAIN);
        int expected = iteration % 2 ? SIGTERM : (42 << 8);
        recordChildExit(child.identifier, expected);
        assert(!findJob(child.identifier) && deliveredSignals == iteration + 1);
        audit_token_t expectedAudit = parent.audit;
        assert(!memcmp(&signaledAudit, &expectedAudit, sizeof(signaledAudit)));
        char hint;
        assert(read(parent.wakeReadFD, &hint, 1) == 1);
        assert(!LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status));
        assert(waited == child.pid && status == expected);
        assert(iteration % 2 ? WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM : WIFEXITED(status) && WEXITSTATUS(status) == 42);
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
    }
    recordChildExit(parent.identifier, 0);
    assert(!jobs.count);
}

static void testMonitorFailureAndEarlyExit(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 20);
    for (unsigned earlyExit = 0; earlyExit < 2; ++earlyExit) {
        int fd;
        unsigned object;
        enqueueSpawn(spawnRequest(&fd, &object), parent);
        uint64_t identifier = startingJob.identifier;
        monitorCreateError = earlyExit ? 0 : EACCES;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 21 + earlyExit, 0));
        if (earlyExit) recordChildExit(identifier, 99 << 8);
        assert(replyFor(object) && replyFor(object)->error == (earlyExit ? ECHILD : EACCES));
        assert(!findJob(identifier) && jobs.count == 1 && !deliveredSignals);
        pid_t waited;
        int status;
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
    }
}

static void testAuditedGenerationDisappearsBeforeBootstrap(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 25);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), parent);
    uint64_t identifier = startingJob.identifier;
    unsigned beforePlans = planReplies();
    identityCheckError = ESRCH;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 26, 0));
    assert(identityChecks == 2 && !findJob(identifier) && jobs.count == 1);
    assert(planReplies() == beforePlans && replyFor(object)->error == ESRCH && !deliveredSignals);
    assertClosed(fd);
    pid_t waited;
    int status;
    assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
}

static void testRunningBoardFallback(void) {
    resetBroker();
    monitorCreateError = EACCES;
    runningBoardError = 0;
    LSBJob *parent = runTestJob(nil, 27);
    LSBJob *child = runTestJob(parent, 28);
    assert(runningBoardStarts == 2 && !identityChecks && !child.exitMonitor && child.runningBoardMonitor);
    directSignalError = EPERM;
    child.runningBoardMonitor.completion(37 << 8, 0);
    assert(!findJob(child.identifier) && runningBoardCancels == 1 && deliveredSignals == 1);
    assert(submittedExtensionSignals == 1 && signaledRequest == parent.identifier);
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &pid, &status));
    assert(pid == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    LSBJob *unknown = runTestJob(parent, 29);
    unknown.runningBoardMonitor.completion(0, EPROTO);
    assert(!findJob(unknown.identifier) && deliveredSignals == 1);
    assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &pid, &status) == ECHILD);
}

static void testParentDeathDetachesOrphans(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 30);
    LSBJob *running = runTestJob(parent, 31);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), parent);
    uint64_t pending = startingJob.identifier;
    // Bind the pending process first so cancellation does not poison the unrelated launch lane.
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 32, 0));
    recordChildExit(parent.identifier, SIGKILL);
    assert(!findJob(parent.identifier) && !findJob(pending));
    assert(replyFor(object)->error == ECANCELED && !running.parentIdentifier);
    assert(jobs.count == 1 && !launchLaneFailed && !startingJob);
    recordChildExit(running.identifier, 7 << 8);
    assert(!jobs.count && !deliveredSignals);
}

static RecordedReply *requestKill(LSBJob *parent, pid_t targetPID, int signalNumber) {
    LSBReceivedMessage *message = messageFor(LSB_REQUEST_KILL, parent.pid, parent.pidVersion, 0);
    unsigned object = ((TestMessageObject *)message->object)->identifier;
    message->targetPID = targetPID;
    message->signalNumber = signalNumber;
    handleMessage(message);
    RecordedReply *reply = replyFor(object);
    assert(reply && !reply->requestID && !reply->deliveredPlan && (!reply->error || !reply->pid));
    return reply;
}

static void testKillAuthenticationAndExit(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 40);
    LSBJob *child = runTestJob(parent, 41);
    parent.pid = getpid() + 100;
    audit_token_t parentAudit = parent.audit;
    parentAudit.val[5] = (unsigned)parent.pid;
    parent.audit = parentAudit;
    LSBJob *other = runTestJob(nil, 42);
    other.pid = getpid() + 200;
    unsigned checks = identityChecks;
    assert(!requestKill(parent, child.pid, 0)->error && identityChecks == checks + 1 && !deliveredSignals && !submittedExtensionSignals);
    assert(requestKill(parent, child.pid, -1)->error == EINVAL);
    assert(requestKill(parent, child.pid, NSIG)->error == EINVAL);
    assert(requestKill(parent, 0, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, -1, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, -20, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, other.pid, SIGTERM)->error == ESRCH);
    assert(requestKill(parent, parent.pid, SIGTERM)->error == ESRCH);
    assert(requestKill(parent, getpid() + 300, SIGTERM)->error == ESRCH);
    LSBJob *impostor = [LSBJob new];
    impostor.pid = parent.pid;
    impostor.pidVersion = parent.pidVersion + 1;
    assert(requestKill(impostor, child.pid, SIGTERM)->error == EACCES);
    parent.phase = JobDispatched;
    assert(requestKill(parent, child.pid, SIGTERM)->error == EACCES);
    parent.phase = JobRunning;
    child.phase = JobDispatched;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
    child.phase = JobRunning;
    assert(identityChecks == checks + 1 && !deliveredSignals && !submittedExtensionSignals);
    identityCheckError = ESRCH;
    assert(requestKill(parent, child.pid, 0)->error == ESRCH);
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
    assert(!deliveredSignals && !submittedExtensionSignals);
    identityCheckError = 0;
    RecordedReply *reply = requestKill(parent, child.pid, SIGTERM);
    assert(!reply->error && reply->pid == child.pid && deliveredSignals == 1 && !submittedExtensionSignals && lastSignal == SIGTERM);
    audit_token_t childAudit = child.audit;
    assert(!memcmp(&signaledAudit, &childAudit, sizeof(childAudit)));
    // Successful signaling does not invent an exit. Only the later monitor event makes waitpid reapable.
    pid_t waited;
    int status;
    assert(findJob(child.identifier) && LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(child.identifier, SIGTERM);
    assert(!LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status));
    assert(waited == child.pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(requestKill(parent, child.pid, 0)->error == ESRCH && requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
}

static void testKillFallbackErrors(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 50);
    LSBJob *child = runTestJob(parent, 51);
    directSignalError = ESRCH;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == 1 && !submittedExtensionSignals);
    directSignalError = EINVAL;
    assert(requestKill(parent, child.pid, SIGTERM)->error == EINVAL && deliveredSignals == 2 && !submittedExtensionSignals);
    directSignalError = EPERM;
    assert(!requestKill(parent, child.pid, SIGTERM)->error && submittedExtensionSignals == 1 && signaledRequest == child.identifier);
    assert(lastSignal == SIGTERM);
    directSignalError = EACCES;
    assert(!requestKill(parent, child.pid, SIGKILL)->error && submittedExtensionSignals == 2 && lastSignal == SIGKILL);
    extensionSignalError = ENOMEM;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ENOMEM && submittedExtensionSignals == 3);
    extensionSignalError = ESRCH;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH && submittedExtensionSignals == 4);
    assert(findJob(child.identifier) && jobs.count == 2);
}

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(count == TASK_AUDIT_TOKEN_COUNT && token.val[5] == (unsigned)getpid());
    return token;
}

static bool hasRight(mach_port_t port, mach_port_type_t right) {
    mach_port_type_t type = 0;
    return !mach_port_type(mach_task_self(), port, &type) && (type & right);
}

static void runMainQueueUntil(BOOL (^done)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!done() && deadline.timeIntervalSinceNow > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    assert(done());
}

static LSBReceivedMessage *launchdRequest(int *fd, unsigned *object, const char *path, const char *marker) {
    LSBReceivedMessage *message = spawnRequest(fd, object);
    assert(!LSBPlanAddString(&message->plan, path, &message->plan.path));
    message->plan.argv[0] = message->plan.path;
    if (marker) {
        assert(!LSBPlanAddString(&message->plan, marker, &message->plan.envp[0]));
        message->plan.envc = 1;
    }
    return message;
}

static LSBJob *checkInLaunchd(unsigned *parentObject) {
    int fd;
    enqueueSpawn(launchdRequest(&fd, parentObject, "/sbin/launchd_sim", "LSB_SIMULATOR_LAUNCHD=1"), nil);
    LSBJob *job = startingJob;
    assert(job.simulatorLaunchd);
    audit_token_t audit = ownAudit();
    LSBReceivedMessage *checkIn = messageFor(LSB_REQUEST_CHECKIN, getpid(), audit.val[7], 0);
    checkIn->audit = audit;
    handleMessage(checkIn);
    assertClosed(fd);
    return job;
}

static void testLaunchdModeRequiresInitialPathAndMarker(void) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        resetBroker();
        LSBJob *parent = nil;
        if (variant == 3) {
            parent = [LSBJob new];
            parent.identifier = 99;
            assert(!prepareChildStatus(parent));
        }
        int fd;
        unsigned object;
        const char *path = variant == 0 ? "/bin/df" : "/sbin/launchd_sim";
        const char *marker = variant == 1 ? NULL : variant == 2 ? "LSB_SIMULATOR_LAUNCHD=0" : "LSB_SIMULATOR_LAUNCHD=1";
        enqueueSpawn(launchdRequest(&fd, &object, path, marker), parent);
        assert(startingJob && !startingJob.simulatorLaunchd);
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 71, 0));
        assert(planReplies() == 1 && !replies[replyCount - 1].launchdTank && !startingJob.launchdHandshake && !startingJob.launchdSource);
        finishJob(startingJob, ECANCELED);
        assert(replyFor(object)->error == ECANCELED);
        assertClosed(fd);
    }
}

static void testLaunchdHandshakeDispatchAndRights(void) {
    resetBroker();
    __weak LSBJob *retired;
    mach_port_t service, replyPort;
    @autoreleasepool {
        unsigned object;
        LSBJob *job = checkInLaunchd(&object);
        retired = job;
        mach_port_t tank = replies[replyCount - 1].launchdTank;
        service = job.launchdHandshake->serviceSend;
        assert(tank && job.launchdSource && hasRight(tank, MACH_PORT_TYPE_RECEIVE) && !replyFor(object));
        handleMessage(messageFor(LSB_REQUEST_READY, getpid(), job.pidVersion, job.identifier));
        assert(job.phase == JobRunning && replyFor(object) && !replyFor(object)->error);
        assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort));
        mach_msg_header_t request = {
            .msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
            .msgh_size = sizeof(request), .msgh_remote_port = tank, .msgh_local_port = replyPort, .msgh_id = 708
        };
        assert(!mach_msg(&request, MACH_SEND_MSG, sizeof(request), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
        runMainQueueUntil(^BOOL{ return job.launchdHandshake->completed && !job.launchdHandshake->tankReceive; });
        assert(!job.launchdSource && !job.launchdHandshake->tankSend && !job.launchdHandshake->serviceReceive);
        assert(!hasRight(tank, MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_RECEIVE));
        assert(hasRight(service, MACH_PORT_TYPE_SEND) && !hasRight(service, MACH_PORT_TYPE_RECEIVE));
        struct {
            mach_msg_header_t header;
            mach_msg_body_t body;
            mach_msg_port_descriptor_t service;
            mach_msg_trailer_t trailer;
        } response = {0};
        assert(!mach_msg(&response.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(response), replyPort, 0, MACH_PORT_NULL));
        assert(response.header.msgh_id == 808 && response.header.msgh_size == 40 && (response.header.msgh_bits & MACH_MSGH_BITS_COMPLEX));
        assert(response.body.msgh_descriptor_count == 1 && response.service.name == service);
        assert(response.service.type == MACH_MSG_PORT_DESCRIPTOR && response.service.disposition == MACH_MSG_TYPE_MOVE_RECEIVE);
        assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_SEND));
        finishJob(job, 0);
        // Only the service send right belongs to the broker now; the received endpoint must survive job disposal.
        assert(!jobs.count && !startingJob && hasRight(service, MACH_PORT_TYPE_SEND));
    }
    runMainQueueUntil(^BOOL{ return retired == nil; });
    assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && !hasRight(service, MACH_PORT_TYPE_SEND));
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void testLaunchdCancellationAndFailedBootstrap(void) {
    for (unsigned failedSend = 0; failedSend < 2; ++failedSend) {
        resetBroker();
        __weak LSBJob *retired;
        mach_port_t tank, service;
        @autoreleasepool {
            unsigned object;
            launchdBootstrapSendError = failedSend ? EPIPE : 0;
            LSBJob *job = checkInLaunchd(&object);
            retired = job;
            tank = job.launchdHandshake->tankReceive;
            service = job.launchdHandshake->serviceReceive;
            assert(tank && service && !job.launchdHandshake->completed);
            if (!failedSend) finishJob(job, ECANCELED);
            assert(!jobs.count && !startingJob && !job.launchdSource && !launchLaneFailed);
            assert(replyFor(object)->error == (failedSend ? EPIPE : ECANCELED));
            // finishJob cancels the watch; its cancellation handler owns the later RECEIVE-right destruction.
            assert(hasRight(tank, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_RECEIVE));
            runMainQueueUntil(^BOOL{ return !job.launchdHandshake->tankReceive; });
            assert(!job.launchdHandshake->tankSend && hasRight(service, MACH_PORT_TYPE_RECEIVE));
        }
        runMainQueueUntil(^BOOL{ return retired == nil; });
        assert(!hasRight(tank, MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_DEAD_NAME));
        assert(!hasRight(service, MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_DEAD_NAME));
    }
}

int main(void) {
    @autoreleasepool {
        alarm(10);
        testIdentityAndSuccessfulHandoff();
        testReadySendFailure();
        testChildReadyFailure(ENOEXEC);
        testChildReadyFailure(EBADF);
        testUnboundFailureStopsQueue();
        testSubmissionFailureAndSpawnAuthentication();
        testInitialGuestArguments();
        testExitStatusesAndRepeatedReaping();
        testMonitorFailureAndEarlyExit();
        testAuditedGenerationDisappearsBeforeBootstrap();
        testRunningBoardFallback();
        testParentDeathDetachesOrphans();
        testKillAuthenticationAndExit();
        testKillFallbackErrors();
        testLaunchdModeRequiresInitialPathAndMarker();
        testLaunchdHandshakeDispatchAndRights();
        testLaunchdCancellationAndFailedBootstrap();
        resetBroker();
        puts("Broker: authentication, FD ownership, exit/kill, orphan cleanup, launchd mode, Mach handoff, and source cancellation passed");
    }
    return 0;
}
