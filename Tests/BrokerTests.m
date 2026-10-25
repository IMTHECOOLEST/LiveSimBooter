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
    pid_t guestPID, parentPID;
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
static void (^readyReplyHook)(void);

// Keep these tests on broker decisions; IndigoHIDServerTests exercises its Mach authentication and receive lifecycle.
@interface RecordedIndigoServer : LSBIndigoHIDServer
@property unsigned authorizations, revocations;
@property audit_token_t authorizedAudit;
@end
@implementation RecordedIndigoServer
- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit {
    if (audit) { self.authorizedAudit = *audit; ++self.authorizations; }
    else { self.authorizedAudit = (audit_token_t){{0}}; ++self.revocations; }
}
- (pid_t)connectedClientPID { return 0; }
- (void)close {}
@end

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
    replies[replyCount++] = (RecordedReply){identifier, message->kind, error, sendError, pid, requestID, plan != NULL, MACH_PORT_NULL, 0, 0};
    releaseMessageObject(message->object);
    message->object = NULL;
    if (message->kind == LSB_REQUEST_READY && readyReplyHook) readyReplyHook();
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
int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata) {
    assert(metadata && metadata->guestPID > 0 && metadata->parentPID >= 0 && metadata->ports);
    int result;
    if (metadata->launchdTankPort)
        result = LSBTransportReplyLaunchdBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD, metadata->launchdTankPort);
    else result = LSBTransportReplyBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD);
    replies[replyCount - 1].guestPID = metadata->guestPID;
    replies[replyCount - 1].parentPID = metadata->parentPID;
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
    if (adoptionRetrySource) dispatch_source_cancel(adoptionRetrySource);
    adoptionRetrySource = nil;
    pendingAdoptions = [NSMutableArray new];
    for (LSBJob *job in jobs) {
        if (job.exitMonitor) dispatch_source_cancel(job.exitMonitor);
        job.exitMonitor = nil;
        if (job.launchdSource) dispatch_source_cancel(job.launchdSource);
        job.launchdSource = nil;
        if (job.runningBoardMonitor) LSBRunningBoardMonitorCancel(job.runningBoardMonitor);
        job.runningBoardMonitor = nil;
        [job.indigo close];
        if (job.request) disposeMessage(job.request);
        job.request = NULL;
    }
    startingJob = nil;
    jobs = [NSMutableArray new];
    runtimeRoot = @"/tmp/RuntimeRoot";
    nextIdentifier = 1;
    launchLaneFailed = NO;
    enableIndigo = NO;
    retiredIdentityCount = auditResolutionLogs = 0;
    retiredIdentitiesFull = NO;
    memset(retiredIdentities, 0, sizeof(retiredIdentities));
    readyReplyHook = nil;
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

static LSBJob *auditJob(pid_t nativePID, pid_t guestPID, unsigned generation, BOOL launchd) {
    LSBJob *job = [LSBJob new];
    job.identifier = nextIdentifier++;
    job.pid = nativePID;
    job.guestPID = guestPID;
    job.pidVersion = generation;
    job.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)nativePID, 7, generation}};
    job.phase = JobRunning;
    job.simulatorLaunchd = launchd;
    [jobs addObject:job];
    return job;
}

static RecordedReply *auditReply(audit_token_t caller, audit_token_t target) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_RESOLVE_AUDIT_PID, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetAudit = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && reply->kind == LSB_REQUEST_RESOLVE_AUDIT_PID && !reply->requestID && !reply->deliveredPlan);
    return reply;
}

static void testAuditOwnerAuthenticationAndIdentity(void) {
    resetBroker();
    LSBJob *launchd = auditJob(8100, 8100, 11, YES), *target = auditJob(8200, 8150, 12, NO);
    audit_token_t targetAudit = target.audit, caller = launchd.audit;
    RecordedReply *reply = auditReply(caller, targetAudit);
    assert(!reply->error && reply->pid == target.guestPID);
    assert(auditReply(targetAudit, targetAudit)->error == EACCES);
    for (unsigned index = 0; index < 8; ++index) {
        audit_token_t changed = caller;
        changed.val[index] ^= 1;
        assert(auditReply(changed, targetAudit)->error == EACCES);
        if (index == 5) continue; // An unrelated native PID is not a managed target; trusted launchd supplies its actual audit token.
        changed = targetAudit;
        changed.val[index] ^= 1;
        assert(auditReply(caller, changed)->error == ESRCH);
    }
    launchd.phase = JobDispatched;
    assert(auditReply(caller, targetAudit)->error == EACCES);
    launchd.phase = JobRunning;
    launchd.retiredForExec = YES;
    assert(auditReply(caller, targetAudit)->error == EACCES);
    launchd.retiredForExec = NO;
    target.phase = JobDispatched;
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    target.phase = JobRunning;
    target.retiredForExec = YES;
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    target.retiredForExec = NO;
    audit_token_t unknown = {{8, 7, 6, 5, 4, 8300, 2, 1}};
    reply = auditReply(caller, unknown);
    assert(!reply->error && reply->pid == 8300);
    unknown.val[5] = target.guestPID;
    assert(auditReply(caller, unknown)->error == ESRCH); // Native fallback cannot become a guest job by colliding with its stable PID.
    unknown.val[5] = 0;
    assert(auditReply(caller, unknown)->error == EINVAL);
    unknown.val[5] = UINT32_MAX;
    assert(auditReply(caller, unknown)->error == EINVAL);
    finishJob(target, 0);
    assert(retiredIdentityCount == 1 && !findJob(target.identifier));
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    unknown.val[5] = target.guestPID;
    assert(auditReply(caller, unknown)->error == ESRCH);
    LSBJob *reused = auditJob(target.pid, 8400, 13, NO);
    reply = auditReply(caller, reused.audit);
    assert(!reply->error && reply->pid == 8400); // A newly committed exact identity remains usable even when its native PID was retired.
    assert(auditReply(caller, targetAudit)->error == ESRCH);
}

static void testAuditIdentityHistoryNeverEvicts(void) {
    resetBroker();
    LSBJob *launchd = auditJob(8100, 8100, 11, YES), *current = auditJob(8200, 8150, 12, NO);
    LSBJob *old = [LSBJob new];
    for (unsigned index = 0; index < RetiredIdentityCapacity + 3; ++index) {
        old.pid = 9000 + index;
        old.guestPID = 10000 + index;
        old.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)old.pid, 7, 200 + index}};
        retireAuditIdentity(old);
    }
    assert(retiredIdentitiesFull && retiredIdentityCount == RetiredIdentityCapacity);
    assert(retiredIdentities[0].audit.val[5] == 9000);
    assert(retiredIdentities[RetiredIdentityCapacity - 1].audit.val[5] == 9000 + RetiredIdentityCapacity - 1);
    audit_token_t unknown = {{1, 2, 3, 4, 5, 20000, 7, 30}};
    assert(auditReply(launchd.audit, unknown)->error == ESRCH);
    assert(auditReply(launchd.audit, old.audit)->error == ESRCH); // Even an identity beyond the bound cannot escape through fallback.
    RecordedReply *reply = auditReply(launchd.audit, current.audit);
    assert(!reply->error && reply->pid == current.guestPID);
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

static void testExecReplacement(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 100);
    LSBJob *old = runTestJob(parent, 101);
    LSBJob *grandchild = runTestJob(old, 102);
    pid_t guestPID = old.guestPID;
    LSBChildTicket ticket = old.parentTicket;
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    request->kind = LSB_REQUEST_EXEC;
    request->audit = old.audit;
    handleMessage(request);
    LSBJob *replacement = startingJob;
    assert(replacement && replacement != old && replacement.guestPID == guestPID && old.replacementIdentifier == replacement.identifier);
    assert(replacement.childStatus != old.childStatus);
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(replacement.childStatus, grandchild.guestPID, &observation));
    assert(!observation.execGeneration && !observation.exited);
    reportedPID = getpid() + 200;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 103, 0));
    assert(replies[replyCount - 1].guestPID == guestPID && replies[replyCount - 1].parentPID == parent.guestPID);
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 103, replacement.identifier));
    assert(!findJob(old.identifier) && findJob(replacement.identifier) && !startingJob);
    assert(replacement.parentIdentifier == parent.identifier && grandchild.parentIdentifier == replacement.identifier);
    assert(replacement.parentTicket.index == ticket.index && replacement.parentTicket.generation == ticket.generation);
    assert(replyFor(object) && !replyFor(object)->error && replyFor(object)->pid == guestPID);
    assert(lastSignal == SIGKILL && signaledAudit.val[7] == 101);
    assert(!LSBChildStatusObserve(parent.childStatus, guestPID, &observation));
    assert(observation.nativePID == reportedPID && observation.execGeneration == 1 && !observation.exited);
    assert(!signalChild(parent, guestPID, SIGTERM) && signaledAudit.val[5] == (unsigned)reportedPID && signaledAudit.val[7] == 103);
    pid_t waited = -1;
    int status = -1;
    assert(LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(old.identifier, SIGKILL); // A queued notification from the retired native instance is ignored.
    assert(LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(replacement.identifier, 23 << 8);
    assert(!LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) && waited == guestPID && status == (23 << 8));
    assert(grandchild.parentIdentifier == 0);
}

static void testExecFailurePreservesCaller(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 110);
    LSBJob *old = runTestJob(parent, 111);
    for (unsigned failReply = 0; failReply < 2; ++failReply) {
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = old.audit;
        handleMessage(request);
        LSBJob *replacement = startingJob;
        reportedPID = getpid() + 200;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 112 + failReply, 0));
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, reportedPID, 112 + failReply, replacement.identifier);
        if (failReply) readySendError = EPIPE;
        else ready->error = ENOEXEC;
        handleMessage(ready);
        readySendError = 0;
        assert(findJob(old.identifier) && !old.replacementIdentifier && !findJob(replacement.identifier));
        assert(!deliveredSignals && replyFor(object)->error == (failReply ? EPIPE : ENOEXEC));
        LSBChildObservation observation;
        assert(!LSBChildStatusObserve(parent.childStatus, old.guestPID, &observation));
        assert(!observation.execGeneration && !observation.exited && observation.nativePID == old.pid);
    }
}

static void setRequestProgram(LSBReceivedMessage *request, const char *program) {
    assert(!LSBPlanAddString(&request->plan, program, &request->plan.path));
    request->plan.argv[0] = request->plan.path;
}

static void testIndigoRegistrationGatesExecOnly(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        resetBroker();
        LSBJob *parent = runTestJob(nil, 200);
        parent.simulatorLaunchd = YES;
        RecordedIndigoServer *indigo = [RecordedIndigoServer new];
        parent.indigo = indigo;
        parent.servicesPending = YES;
        int fd;
        unsigned proxyObject;
        LSBReceivedMessage *request = spawnRequest(&fd, &proxyObject);
        setRequestProgram(request, "/usr/libexec/xpcproxy_sim");
        unsigned launches = submittedLaunches;
        enqueueSpawn(request, parent);
        LSBJob *proxy = startingJob;
        // Launchd cannot answer endpoint-registration requests until its initial spawn returns from READY.
        assert(proxy && proxy.phase == JobStarting && submittedLaunches == launches + 1 && parent.servicesPending);
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 201, 0));
        handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 201, proxy.identifier));
        assert(proxy.phase == JobRunning && !startingJob && replyFor(proxyObject) && !replyFor(proxyObject)->error);
        assert(!indigo.authorizations && !parent.indigoAuthorizedIdentifier);

        unsigned execObject;
        request = spawnRequest(&fd, &execObject);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = proxy.audit;
        setRequestProgram(request, scenario == 1 ? "/tmp/RuntimeRoot/usr/libexec/backboardd" : "/usr/libexec/backboardd");
        handleMessage(request);
        LSBJob *replacement = findJob(proxy.replacementIdentifier);
        assert(replacement && replacement.phase == JobQueued && !startingJob && submittedLaunches == launches + 1);
        assert(!replyFor(execObject) && fcntl(fd, F_GETFD) >= 0 && !indigo.authorizations);
        startNextJob();
        assert(!startingJob && replacement.phase == JobQueued && !replyFor(execObject));

        parent.servicesPending = NO;
        parent.servicesError = scenario == 2 ? EACCES : 0;
        startNextJob();
        if (parent.servicesError) {
            assert(replyFor(execObject) && replyFor(execObject)->error == EACCES && !replyFor(execObject)->pid);
            assert(!findJob(replacement.identifier) && !proxy.replacementIdentifier && findJob(proxy.identifier));
            assert(submittedLaunches == launches + 1 && !indigo.authorizations && !indigo.revocations && !deliveredSignals);
            assertClosed(fd);
            continue;
        }
        assert(startingJob == replacement && replacement.phase == JobStarting && submittedLaunches == launches + 2);
        reportedPID = getpid() + 200;
        LSBReceivedMessage *checkIn = messageFor(LSB_REQUEST_CHECKIN, reportedPID, 202, 0);
        checkIn->audit.val[0] = 91;
        checkIn->audit.val[6] = 92;
        audit_token_t identity = checkIn->audit;
        handleMessage(checkIn);
        audit_token_t authorized = indigo.authorizedAudit;
        assert(indigo.authorizations == 1 && !indigo.revocations && !memcmp(&authorized, &identity, sizeof(identity)));
        assert(parent.indigoAuthorizedIdentifier == replacement.identifier && !indigo.connectedClientPID);
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, reportedPID, 202, replacement.identifier);
        ready->audit = identity;
        ready->error = scenario == 3 ? ENOEXEC : 0;
        handleMessage(ready);
        if (scenario == 3) {
            assert(replyFor(execObject)->error == ENOEXEC && findJob(proxy.identifier) && !proxy.replacementIdentifier);
            assert(!findJob(replacement.identifier) && indigo.revocations == 1 && !parent.indigoAuthorizedIdentifier);
            assert(!deliveredSignals);
            continue;
        }
        assert(!findJob(proxy.identifier) && replacement.phase == JobRunning && !replyFor(execObject)->error);
        // Retiring xpcproxy must not revoke the newly authorized replacement.
        assert(indigo.authorizations == 1 && !indigo.revocations && parent.indigoAuthorizedIdentifier == replacement.identifier);
        recordChildExit(proxy.identifier, SIGKILL);
        assert(!indigo.revocations);
        // Death before the first Indigo connection still revokes this exact native identity.
        assert(!indigo.connectedClientPID);
        recordChildExit(replacement.identifier, SIGTERM);
        authorized = indigo.authorizedAudit;
        audit_token_t empty = {{0}};
        assert(indigo.revocations == 1 && !memcmp(&authorized, &empty, sizeof(empty)) && !parent.indigoAuthorizedIdentifier);
        recordChildExit(replacement.identifier, SIGTERM);
        assert(indigo.revocations == 1);
    }
}

static void testIndigoOwnerExecIsRejectedWithoutChangingAuthorization(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 210);
    parent.simulatorLaunchd = YES;
    RecordedIndigoServer *indigo = [RecordedIndigoServer new];
    parent.indigo = indigo;
    int fd;
    unsigned firstObject;
    LSBReceivedMessage *request = spawnRequest(&fd, &firstObject);
    setRequestProgram(request, "/usr/libexec/backboardd");
    enqueueSpawn(request, parent);
    LSBJob *old = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 211, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 211, old.identifier));
    audit_token_t identity = old.audit;
    assert(old.phase == JobRunning && indigo.authorizations == 1 && parent.indigoAuthorizedIdentifier == old.identifier);
    unsigned launches = submittedLaunches;
    uint64_t next = nextIdentifier;
    LSBChildStatus *ledger = old.childStatus;
    for (unsigned differentProgram = 0; differentProgram < 2; ++differentProgram) {
        unsigned execObject;
        request = spawnRequest(&fd, &execObject);
        setRequestProgram(request, differentProgram ? "/bin/df" : "/usr/libexec/backboardd");
        request->kind = LSB_REQUEST_EXEC;
        request->audit = identity;
        handleMessage(request);
        assert(replyFor(execObject) && replyFor(execObject)->error == ENOTSUP && !replyFor(execObject)->pid);
        assert(findJob(old.identifier) && old.phase == JobRunning && !old.replacementIdentifier && old.childStatus == ledger);
        assert(jobs.count == 2 && !startingJob && submittedLaunches == launches && nextIdentifier == next && !deliveredSignals);
        audit_token_t authorized = indigo.authorizedAudit;
        assert(parent.indigoAuthorizedIdentifier == old.identifier && !memcmp(&authorized, &identity, sizeof(identity)));
        assert(indigo.authorizations == 1 && !indigo.revocations);
        assertClosed(fd);
        LSBChildObservation observation;
        assert(!LSBChildStatusObserve(parent.childStatus, old.guestPID, &observation));
        assert(!observation.execGeneration && !observation.exited && observation.nativePID == old.pid);
    }
    recordChildExit(old.identifier, SIGTERM);
    assert(!parent.indigoAuthorizedIdentifier && indigo.revocations == 1);
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
    assert(LSBChildStatusGetParentPID(running.childStatus) == 0);
    assert(jobs.count == 1 && !launchLaneFailed && !startingJob);
    recordChildExit(running.identifier, 7 << 8);
    assert(!jobs.count && !deliveredSignals);
}

// Adoption cases use distinct logical/native identities without launching real subprocesses.
static LSBJob *managedJob(LSBJob *parent, pid_t identity) {
    LSBJob *job = [LSBJob new];
    job.identifier = nextIdentifier++;
    job.phase = JobRunning;
    job.pid = job.guestPID = identity;
    job.pidVersion = (uint32_t)job.identifier;
    job.audit = (audit_token_t){{0, 0, 0, 0, 0, (unsigned)identity, 0, job.pidVersion}};
    assert(!prepareChildStatus(job));
    assert(!LSBChildStatusSetParentPID(job.childStatus, parent ? parent.guestPID : getpid()));
    if (parent) {
        LSBChildTicket ticket;
        assert(!LSBChildStatusReserve(parent.childStatus, &ticket));
        job.parentTicket = ticket;
        assert(!LSBChildStatusPublishRunning(parent.childStatus, job.parentTicket, identity, identity));
        job.parentIdentifier = parent.identifier;
    }
    [jobs addObject:job];
    return job;
}

static void testLaunchdAdoptsRunningAndZombieChildren(void) {
    resetBroker();
    LSBJob *init = managedJob(nil, 7001);
    init.simulatorLaunchd = YES;
    LSBJob *parent = managedJob(init, 7002), *child = managedJob(parent, 7003), *grandchild = managedJob(child, 7004);
    LSBJob *zombie = managedJob(parent, 7005);
    recordChildExit(zombie.identifier, 23 << 8);
    assert(!LSBChildStatusPublishExec(parent.childStatus, child.parentTicket, 7103));
    child.pid = 7103;
    child.executed = YES;
    recordChildExit(parent.identifier, 0);
    assert(child.parentIdentifier == init.identifier && grandchild.parentIdentifier == child.identifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == init.guestPID);
    assert(LSBChildStatusGetParentPID(grandchild.childStatus) == child.guestPID);
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(init.childStatus, child.guestPID, &observation));
    assert(observation.nativePID == 7103 && observation.execGeneration == 1 && !observation.exited);
    assert(!LSBChildStatusObserve(init.childStatus, zombie.guestPID, &observation) && observation.exited && observation.status == (23 << 8));
    assert(!pendingAdoptions.count);
    recordChildExit(child.identifier, 37 << 8);
    assert(grandchild.parentIdentifier == init.identifier && LSBChildStatusGetParentPID(grandchild.childStatus) == init.guestPID);
    pid_t waited;
    int status;
    assert(!LSBChildStatusTryWait(init.childStatus, child.guestPID, 0, &waited, &status) && status == (37 << 8));
    assert(!LSBChildStatusTryWait(init.childStatus, zombie.guestPID, 0, &waited, &status) && status == (23 << 8));
    assert(!LSBChildStatusTryWait(init.childStatus, parent.guestPID, 0, &waited, &status) && status == 0);
}

static void fillExitedLedger(LSBJob *parent) {
    for (unsigned index = 0; index < LSB_CHILD_STATUS_CAPACITY; ++index) {
        LSBChildTicket ticket;
        assert(!LSBChildStatusReserve(parent.childStatus, &ticket));
        assert(!LSBChildStatusPublishRunning(parent.childStatus, ticket, 10000 + index, 10000 + index));
        assert(!LSBChildStatusPublishExit(parent.childStatus, ticket, 0));
    }
}

static void testAdoptionCapacityPreservesExitAndExec(void) {
    resetBroker();
    LSBJob *init = managedJob(nil, 7201);
    init.simulatorLaunchd = YES;
    fillExitedLedger(init);
    LSBJob *parent = managedJob(nil, 7202), *child = managedJob(parent, 7203), *zombie = managedJob(parent, 7204);
    recordChildExit(zombie.identifier, 41 << 8);
    recordChildExit(parent.identifier, 0);
    assert(pendingAdoptions.count == 2 && child.parentStatusPending && child.parentIdentifier == init.identifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == init.guestPID);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), child);
    assert(replyFor(object)->error == EAGAIN && !startingJob);
    LSBReceivedMessage *exec = spawnRequest(&fd, &object);
    exec->kind = LSB_REQUEST_EXEC;
    exec->audit = child.audit;
    handleMessage(exec);
    LSBJob *replacement = startingJob;
    reportedPID = getpid() + 400;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 7205, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 7205, replacement.identifier));
    assert(!findJob(child.identifier) && replacement.parentStatusPending && replacement.parentIdentifier == init.identifier);
    assert(LSBChildStatusGetParentPID(replacement.childStatus) == init.guestPID);
    LSBPendingAdoption *pending = pendingAdoption(replacement.identifier);
    assert(pending && pending.observation.nativePID == reportedPID && pending.observation.execGeneration == 1);
    recordChildExit(replacement.identifier, SIGTERM);
    assert(pendingAdoptions.count == 2);
    pid_t waited;
    int status;
    assert(!LSBChildStatusTryWait(init.childStatus, 10000, 0, &waited, &status));
    retryAdoptions();
    assert(pendingAdoptions.count == 1);
    assert(!LSBChildStatusTryWait(init.childStatus, child.guestPID, 0, &waited, &status) && status == SIGTERM);
    retryAdoptions();
    assert(!pendingAdoptions.count && !adoptionRetrySource);
    assert(!LSBChildStatusTryWait(init.childStatus, zombie.guestPID, 0, &waited, &status) && status == (41 << 8));
}

static void testLaunchdDeathAndCyclesDoNotAdopt(void) {
    resetBroker();
    LSBJob *parent = managedJob(nil, 7301), *init = managedJob(parent, 7302), *child = managedJob(parent, 7303);
    init.simulatorLaunchd = YES; // A descendant cannot become its sibling's ancestor through adoption.
    recordChildExit(parent.identifier, 0);
    assert(!child.parentIdentifier && !init.parentIdentifier && LSBChildStatusGetParentPID(child.childStatus) == 0);
    resetBroker();
    init = managedJob(nil, 7401);
    init.simulatorLaunchd = YES;
    fillExitedLedger(init);
    parent = managedJob(nil, 7402);
    child = managedJob(parent, 7403);
    recordChildExit(parent.identifier, 0);
    assert(pendingAdoptions.count == 1);
    recordChildExit(init.identifier, SIGKILL);
    assert(!pendingAdoptions.count && !adoptionRetrySource && !child.parentIdentifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == 0);
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

static void testAuditResolutionSeesCommittedExecOnly(void) {
    for (unsigned failReady = 0; failReady < 2; ++failReady) {
        resetBroker();
        LSBJob *launchd = runTestJob(nil, 800), *old = runTestJob(launchd, 801);
        launchd.simulatorLaunchd = YES;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = old.audit;
        handleMessage(request);
        LSBJob *replacement = startingJob;
        reportedPID = getpid() + 200;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 802, 0));
        assert(replacement.phase == JobDispatched);
        assert(auditReply(launchd.audit, replacement.audit)->error == ESRCH);
        RecordedReply *reply = auditReply(launchd.audit, old.audit);
        assert(!reply->error && reply->pid == old.guestPID);
        __block BOOL queried = NO;
        readyReplyHook = ^{
            assert(replacement.phase == JobDispatched); // The kernel may wake the candidate here, before publication.
            dispatch_async(dispatch_get_main_queue(), ^{
                RecordedReply *newReply = auditReply(launchd.audit, replacement.audit);
                RecordedReply *oldReply = auditReply(launchd.audit, old.audit);
                assert(failReady ? newReply->error == ESRCH : (!newReply->error && newReply->pid == old.guestPID));
                assert(failReady ? (!oldReply->error && oldReply->pid == old.guestPID) : oldReply->error == ESRCH);
                queried = YES;
            });
        };
        readySendError = failReady ? EPIPE : 0;
        handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 802, replacement.identifier));
        readyReplyHook = nil;
        readySendError = 0;
        assert(!queried);
        runMainQueueUntil(^BOOL { return queried; });
        assert(failReady ? findJob(old.identifier) != nil : findJob(replacement.identifier) != nil);
        assert(retiredIdentityCount == 1);
    }
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
        testExecReplacement();
        testExecFailurePreservesCaller();
        testAuditOwnerAuthenticationAndIdentity();
        testAuditIdentityHistoryNeverEvicts();
        testAuditResolutionSeesCommittedExecOnly();
        testIndigoRegistrationGatesExecOnly();
        testIndigoOwnerExecIsRejectedWithoutChangingAuthorization();
        testMonitorFailureAndEarlyExit();
        testAuditedGenerationDisappearsBeforeBootstrap();
        testRunningBoardFallback();
        testParentDeathDetachesOrphans();
        testLaunchdAdoptsRunningAndZombieChildren();
        testAdoptionCapacityPreservesExitAndExec();
        testLaunchdDeathAndCyclesDoNotAdopt();
        testKillAuthenticationAndExit();
        testKillFallbackErrors();
        testLaunchdModeRequiresInitialPathAndMarker();
        testLaunchdHandshakeDispatchAndRights();
        testLaunchdCancellationAndFailedBootstrap();
        resetBroker();
        puts("Broker: authentication, FD ownership, exit/kill, orphan cleanup, Indigo exec gating, Mach handoff, and source cancellation passed");
    }
    return 0;
}
