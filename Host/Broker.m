#import <Foundation/Foundation.h>
#include "Broker.h"
#include "ExtensionLauncher.h"
#include "ChildMonitor.h"
#include "RunningBoardMonitor.h"
#include "ChildStatus.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#import "TerminalSession.h"
#include "FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

enum { JobQueued, JobStarting, JobDispatched, JobRunning };
@interface LSBJob : NSObject
@property uint64_t identifier;
@property int phase;
@property pid_t pid;
@property uint32_t pidVersion;
@property pid_t extensionPID;
@property LSBReceivedMessage *request;
@property dispatch_source_t exitMonitor;
@property (nonatomic, strong) LSBRunningBoardMonitor *runningBoardMonitor;
@property audit_token_t audit;
@property uint64_t parentIdentifier;
@property LSBChildTicket parentTicket;
@property BOOL parentStatusPublished;
@property LSBChildStatus *childStatus;
@property int statusFD, wakeReadFD, wakeWriteFD;
@property BOOL terminalSession;
@end
@implementation LSBJob
- (instancetype)init {
    if ((self = [super init])) _statusFD = _wakeReadFD = _wakeWriteFD = -1;
    return self;
}
- (void)dealloc {
    if (_childStatus) munmap(_childStatus, LSB_CHILD_STATUS_MAPPING_SIZE);
    if (_statusFD >= 0) close(_statusFD);
    if (_wakeReadFD >= 0) close(_wakeReadFD);
    if (_wakeWriteFD >= 0) close(_wakeWriteFD);
}
@end

static NSMutableArray<LSBJob *> *jobs;
static LSBJob *startingJob;
static NSString *runtimeRoot;
static uint64_t nextIdentifier = 1;
static BOOL launchLaneFailed;
static unsigned finishingDepth;
static LSBTerminalSession *hostTerminal;
static void startNextJob(void);

LSBTerminalSession *LSBHostTerminal(void) { return hostTerminal; }

static LSBJob *findJob(uint64_t identifier) {
    for (LSBJob *job in jobs) if (job.identifier == identifier) return job;
    return nil;
}

static void disposeMessage(LSBReceivedMessage *message) {
    if (!message) return;
    LSBTransportDisposeMessage(message);
    free(message);
}

static void replyError(LSBReceivedMessage *message, int error) {
    if (message->object) LSBTransportReply(message, error, 0, message->requestID, NULL, NULL, NULL);
    disposeMessage(message);
}

static int prepareChildStatus(LSBJob *job) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/.child-status.XXXXXX", runtimeRoot.stringByDeletingLastPathComponent.fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof(path)) return ENAMETOOLONG;
    job.statusFD = mkstemp(path);
    if (job.statusFD < 0) return errno;
    int unlinkError = unlink(path) ? errno : 0;
    if (unlinkError) return unlinkError;
    if (fcntl(job.statusFD, F_SETFD, FD_CLOEXEC) < 0 || ftruncate(job.statusFD, LSB_CHILD_STATUS_MAPPING_SIZE) < 0) return errno;
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, job.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    job.childStatus = mapping;
    LSBChildStatusInitialize(job.childStatus);
    int wake[2];
    if (pipe(wake)) return errno;
    job.wakeReadFD = wake[0];
    job.wakeWriteFD = wake[1];
    for (unsigned i = 0; i < 2; ++i) {
        if (fcntl(wake[i], F_SETFD, FD_CLOEXEC) < 0 || fcntl(wake[i], F_SETFL, O_NONBLOCK) < 0) return errno;
    }
    if (fcntl(wake[1], F_SETNOSIGPIPE, 1) < 0) return errno;
    return 0;
}

static void wakeParent(LSBJob *parent) {
    const char hint = 1;
    ssize_t written;
    do { written = write(parent.wakeWriteFD, &hint, 1); } while (written < 0 && errno == EINTR);
    if (written < 0 && errno != EAGAIN) fprintf(stderr, "[broker] Child wake failed: %s\n", strerror(errno));
}

static void finishJob(LSBJob *job, int error) {
    if (!findJob(job.identifier)) return;
    ++finishingDepth;
    fprintf(stderr, "[broker] request=%llu pid=%d finished error=%d\n", job.identifier, job.pid, error);
    // A timed-out launch can still start later. Never let that unbound child claim another request's plan.
    if (job.phase == JobStarting && !job.pid) {
        launchLaneFailed = YES;
        fprintf(stderr, "[broker] Unbound extension launch failed; restart host before submitting another launch\n");
    }
    if (job.request) {
        if (job.request->object) LSBTransportReply(job.request, error ? error : ECHILD, 0, job.identifier, NULL, NULL, NULL);
        disposeMessage(job.request);
        job.request = NULL;
    }
    if (job.exitMonitor) { dispatch_source_cancel(job.exitMonitor); job.exitMonitor = nil; }
    if (job.runningBoardMonitor) { LSBRunningBoardMonitorCancel(job.runningBoardMonitor); job.runningBoardMonitor = nil; }
    LSBJob *parent = findJob(job.parentIdentifier);
    if (parent && !job.parentStatusPublished && !LSBChildStatusAbandon(parent.childStatus, job.parentTicket)) wakeParent(parent);
    // No other process inherits the simulated parent's children. Running orphans retain their own monitor and ledger.
    // Pending spawns have no surviving caller; cancel them before starting another queued extension.
    for (LSBJob *child in [jobs copy]) {
        if (child.parentIdentifier != job.identifier) continue;
        child.parentIdentifier = 0;
        if (child.phase != JobRunning) finishJob(child, ECANCELED);
    }
    LSBReleaseExtension(job.identifier);
    if (startingJob == job) startingJob = nil;
    [jobs removeObject:job];
    BOOL terminalAlive = NO;
    for (LSBJob *remaining in jobs) if (remaining.terminalSession) terminalAlive = YES;
    if (job.terminalSession && !terminalAlive) {
        // Let the socket reader drain output before EOF closes its endpoint and removes the shared state file.
        hostTerminal.signalHandler = nil;
    }
    --finishingDepth;
    startNextJob();
}

static void recordChildExit(uint64_t identifier, int waitStatus) {
    LSBJob *job = findJob(identifier);
    if (!job) return;
    fprintf(stderr, "[broker] request=%llu pid=%d exited wait-status=0x%x\n", identifier, job.pid, waitStatus);
    LSBJob *parent = findJob(job.parentIdentifier);
    if (job.phase == JobRunning && parent) {
        int error = LSBChildStatusPublishExit(parent.childStatus, job.parentTicket, waitStatus);
        if (error) fprintf(stderr, "[broker] Could not publish child status: %s\n", strerror(error));
        else {
            job.parentStatusPublished = YES;
            wakeParent(parent);
            audit_token_t identity = parent.audit;
            int signalError = LSBChildSignal(&identity, SIGCHLD);
            if (signalError == EACCES || signalError == EPERM) {
                signalError = LSBSignalExtension(parent.identifier, SIGCHLD);
                if (!signalError) fprintf(stderr, "[broker] SIGCHLD submitted through launchd for request=%llu pid=%d\n", parent.identifier, parent.pid);
            }
            if (signalError && signalError != ESRCH) fprintf(stderr, "[broker] SIGCHLD delivery failed: %s\n", strerror(signalError));
        }
    }
    finishJob(job, job.phase == JobRunning ? 0 : ECHILD);
}

static void extensionEvent(uint64_t identifier, pid_t pid, int error, void *context) {
    (void)context;
    LSBJob *job = findJob(identifier);
    if (!job) return;
    if (pid > 0) {
        job.extensionPID = pid;
        if (job.pid && job.pid != pid) { finishJob(job, EACCES); return; }
    }
    if (error && job.phase != JobRunning) finishJob(job, error);
}

static void startNextJob(void) {
    if (startingJob || finishingDepth) return;
    if (launchLaneFailed) {
        for (LSBJob *job in [jobs copy]) if (job.phase == JobQueued) { finishJob(job, EBUSY); return; }
        return;
    }
    for (LSBJob *job in jobs) {
        if (job.phase != JobQueued) continue;
        startingJob = job;
        job.phase = JobStarting;
        uint64_t identifier = job.identifier;
        int error = LSBLaunchExtension(LSB_EXTENSION_IDENTIFIER, identifier, extensionEvent, NULL);
        if (error) { job.phase = JobQueued; finishJob(job, error); return; }
        fprintf(stderr, "[broker] extension request=%llu submitted\n", identifier);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            LSBJob *pending = findJob(identifier);
            if (pending && pending.phase != JobRunning) finishJob(pending, ETIMEDOUT);
        });
        return;
    }
}

static void enqueueSpawn(LSBReceivedMessage *message, LSBJob *parent) {
    if (launchLaneFailed) { replyError(message, EBUSY); return; }
    if (jobs.count >= 16) { replyError(message, EAGAIN); return; }
    LSBJob *job = [LSBJob new];
    int error = prepareChildStatus(job);
    if (error) { replyError(message, error); return; }
    if (parent) {
        LSBChildTicket ticket;
        error = LSBChildStatusReserve(parent.childStatus, &ticket);
        if (error) { replyError(message, error); return; }
        job.parentIdentifier = parent.identifier;
        job.parentTicket = ticket;
    }
    job.identifier = nextIdentifier++;
    job.terminalSession = parent ? parent.terminalSession : hostTerminal != nil;
    job.request = message;
    [jobs addObject:job];
    startNextJob();
}

static int signalJob(LSBJob *child, int signalNumber) {
    audit_token_t identity = child.audit;
    // A retained job can outlive the process until its queued exit notification runs. Validate the full PID generation.
    // proc_signal_with_audittoken does not support signal 0, so the identity lookup also implements the existence check.
    int error = LSBRunningBoardValidateIdentity(&identity);
    if (error || !signalNumber) return error;
    error = LSBChildSignal(&identity, signalNumber);
    if (error == EACCES || error == EPERM) {
        error = LSBSignalExtension(child.identifier, signalNumber);
        if (!error) fprintf(stderr, "[broker] signal=%d submitted through launchd for request=%llu pid=%d\n",
                            signalNumber, child.identifier, child.pid);
    }
    return error;
}

static int signalChild(LSBJob *parent, pid_t targetPID, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) return EINVAL;
    if (targetPID <= 0) return ENOTSUP;
    for (LSBJob *job in jobs) {
        if (job.pid == targetPID && job.parentIdentifier == parent.identifier && job.phase == JobRunning) return signalJob(job, signalNumber);
    }
    return ESRCH;
}

static int signalTerminal(pid_t foreground, int signalNumber) {
    (void)foreground;
    // This first terminal runs with job control disabled: the shell and its descendants share terminal signals.
    // It does not implement kernel process groups or stopped/continued job notifications.
    if (signalNumber != SIGINT && signalNumber != SIGQUIT && signalNumber != SIGWINCH) return ENOTSUP;
    int firstError = 0;
    BOOL found = NO;
    for (LSBJob *job in jobs) {
        if (!job.terminalSession || job.phase != JobRunning) continue;
        found = YES;
        int error = signalJob(job, signalNumber);
        if (error && error != ESRCH && !firstError) firstError = error;
    }
    return found ? firstError : ESRCH;
}

static void handleMessage(LSBReceivedMessage *message) {
    pid_t pid = (pid_t)message->audit.val[5];
    uint32_t generation = message->audit.val[7];
    LSBJob *sender = nil;
    for (LSBJob *job in jobs) if (job.pid == pid && job.pidVersion == generation) { sender = job; break; }
    switch (message->kind) {
        case LSB_REQUEST_SPAWN:
            if (!sender || sender.phase != JobRunning) { replyError(message, EACCES); return; }
            enqueueSpawn(message, sender);
            return;
        case LSB_REQUEST_KILL: {
            if (!sender || sender.phase != JobRunning) { replyError(message, EACCES); return; }
            int error = signalChild(sender, message->targetPID, message->signalNumber);
            LSBTransportReply(message, error, error ? 0 : message->targetPID, 0, NULL, NULL, NULL);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_CHECKIN: {
            // PlugInKit launches our registered extension. Match its PID to the kernel's sender identity; iOS denies
            // cross-process SecTask entitlement queries even for our own extension.
            LSBJob *job = startingJob;
            if (sender || !job || job.phase != JobStarting) {
                replyError(message, ESRCH); return;
            }
            if (!job.extensionPID) job.extensionPID = LSBExtensionProcessIdentifier(job.identifier);
            if (job.extensionPID <= 0) { replyError(message, EAGAIN); return; }
            if (job.extensionPID != pid) { replyError(message, EACCES); return; }
            job.pid = pid;
            job.pidVersion = generation;
            job.audit = message->audit;
            job.phase = JobDispatched;
            uint64_t identifier = job.identifier;
            int monitor = -1;
            int error = LSBChildMonitorCreate(pid, &monitor);
            audit_token_t identity = job.audit;
            if (error == EACCES || error == EPERM) {
                LSBRunningBoardMonitor *fallback = nil;
                error = LSBRunningBoardMonitorStart(&identity, ^(int waitStatus, int monitorError) {
                    LSBJob *exited = findJob(identifier);
                    if (!exited) return;
                    if (monitorError) finishJob(exited, monitorError);
                    else recordChildExit(identifier, waitStatus);
                }, &fallback);
                if (error) { replyError(message, error); finishJob(job, error); return; }
                job.runningBoardMonitor = fallback;
                fprintf(stderr, "[broker] Using RunningBoard exit monitor for pid=%d\n", pid);
            } else {
                if (error) { replyError(message, error); finishJob(job, error); return; }
                // kqueue registers by PID. Verify the audited generation after registering; RBS already uses the full token.
                error = LSBRunningBoardValidateIdentity(&identity);
                if (error) { close(monitor); replyError(message, error); finishJob(job, error); return; }
                job.exitMonitor = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)monitor, 0, dispatch_get_main_queue());
                if (!job.exitMonitor) { close(monitor); replyError(message, ENOMEM); finishJob(job, ENOMEM); return; }
                dispatch_source_set_cancel_handler(job.exitMonitor, ^{ close(monitor); });
                dispatch_source_set_event_handler(job.exitMonitor, ^{
                    LSBJob *exited = findJob(identifier);
                    if (!exited) return;
                    int waitStatus = 0;
                    int monitorError = LSBChildMonitorRead(monitor, pid, &waitStatus);
                    if (!monitorError) recordChildExit(identifier, waitStatus);
                    else if (monitorError != EAGAIN) {
                        fprintf(stderr, "[broker] Exit monitor failed: %s\n", strerror(monitorError));
                        finishJob(exited, monitorError);
                    }
                });
                dispatch_resume(job.exitMonitor);
            }
            error = LSBTransportReplyBootstrap(message, 0, pid, identifier, runtimeRoot.fileSystemRepresentation,
                                               &job.request->plan, job.request->fds, job.statusFD, job.wakeReadFD);
            disposeMessage(message);
            // Don't keep pipe writers alive while waiting for READY; the child now owns copies.
            for (unsigned i = 0; i < job.request->plan.fdCount; ++i) {
                if (job.request->fds[i] >= 0) close(job.request->fds[i]);
                job.request->fds[i] = -1;
            }
            fprintf(stderr, "[broker] check-in request=%llu pid=%d error=%d\n", identifier, pid, error);
            if (error) finishJob(job, error);
            return;
        }
        case LSB_REQUEST_READY: {
            if (!sender || sender.identifier != message->requestID || sender.phase != JobDispatched) {
                replyError(message, ESRCH); return;
            }
            int error = message->error;
            LSBJob *parent = findJob(sender.parentIdentifier);
            // The child is still blocked in READY: capture its real group before it can run and exit.
            if (!error && parent) {
                pid_t group = getpgid(pid);
                error = group < 0 ? errno : LSBChildStatusPublishRunning(parent.childStatus, sender.parentTicket, pid, group);
            }
            int replyError = LSBTransportReply(message, message->error ? 0 : error, pid, sender.identifier, NULL, NULL, NULL);
            disposeMessage(message);
            if (error || replyError) { finishJob(sender, error ? error : replyError); return; }
            sender.phase = JobRunning;
            if (sender.request->object) {
                error = LSBTransportReply(sender.request, 0, pid, sender.identifier, NULL, NULL, NULL);
                if (error) { finishJob(sender, error); return; }
            }
            disposeMessage(sender.request);
            sender.request = NULL;
            fprintf(stderr, "[broker] spawn ready pid=%d request=%llu\n", pid, sender.identifier);
            startingJob = nil;
            startNextJob();
            return;
        }
        default: replyError(message, EINVAL); return;
    }
}

static int prepareRuntimeRoot(void) {
    NSFileManager *manager = NSFileManager.defaultManager;
    NSURL *group = [manager containerURLForSecurityApplicationGroupIdentifier:@LSB_APP_GROUP];
    if (!group) { fprintf(stderr, "[broker] App Group entitlement/container unavailable: %s\n", LSB_APP_GROUP); return EACCES; }
    runtimeRoot = [[group URLByAppendingPathComponent:@"RuntimeRoot" isDirectory:YES] path];
    NSString *oldRoot = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/RuntimeRoot"];
    if (![manager fileExistsAtPath:runtimeRoot] && [manager fileExistsAtPath:oldRoot]) {
        NSError *error = nil;
        if (![manager moveItemAtPath:oldRoot toPath:runtimeRoot error:&error]) {
            fprintf(stderr, "[broker] RuntimeRoot migration failed: %s\n", error.description.UTF8String); return EIO;
        }
    }
    BOOL directory = NO;
    if (![manager fileExistsAtPath:runtimeRoot isDirectory:&directory] || !directory) return ENOENT;
    char canonical[PATH_MAX];
    if (!realpath(runtimeRoot.fileSystemRepresentation, canonical)) return errno;
    runtimeRoot = [NSString stringWithUTF8String:canonical];
    if (strlen(runtimeRoot.fileSystemRepresentation) >= LSB_PATH_BYTES) return ENAMETOOLONG;
    fprintf(stderr, "[broker] RuntimeRoot=%s\n", runtimeRoot.fileSystemRepresentation);
    return 0;
}

static int initialPlan(LSBReceivedMessage *message, int argc, const char *argv[], const char *envp[]) {
    LSBSpawnPlan *plan = &message->plan;
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    plan->version = LSB_PLAN_VERSION;
    const char *defaultArgs[] = {"/bin/fish", "-i", "-N", "-P", "-C",
        "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end", NULL};
    BOOL terminal = argc == 1 || (argc > 1 && !strcmp(argv[1], "--terminal"));
    int firstArgument = terminal && argc > 1 ? 2 : 1;
    const char *const *arguments = argc > firstArgument ? argv + firstArgument : defaultArgs;
    int count = argc > firstArgument ? argc - firstArgument : 6;
    int error = LSBPlanAddString(plan, arguments[0], &plan->path);
    if (!error) error = LSBPlanAddString(plan, runtimeRoot.fileSystemRepresentation, &plan->cwd);
    for (int i = 0; !error && i < count; ++i) {
        if (plan->argc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, arguments[i], &plan->argv[plan->argc++]);
    }
    for (const char **item = envp; !error && item && *item; ++item) {
        if (terminal && (!strncmp(*item, "TERM=", 5) || !strncmp(*item, "PATH=", 5) || !strncmp(*item, "HOME=", 5) ||
                         !strncmp(*item, LSB_FAKE_PTY_ENV "=", sizeof(LSB_FAKE_PTY_ENV)))) continue;
        if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, *item, &plan->envp[plan->envc++]);
    }
    plan->creationMask = 022;
    sigprocmask(0, NULL, (sigset_t *)&plan->signalMask);
    if (terminal && !error) {
        NSString *home = [runtimeRoot stringByAppendingPathComponent:@"home/mobile"];
        NSError *directoryError = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:home withIntermediateDirectories:YES attributes:nil error:&directoryError])
            error = EIO;
        NSArray<NSString *> *environment = @[@"TERM=xterm-256color", [@"HOME=" stringByAppendingString:home],
            [NSString stringWithFormat:@"PATH=%@/bin:%@/usr/bin", runtimeRoot, runtimeRoot]];
        for (NSString *entry in environment) {
            if (error) break;
            if (plan->envc == LSB_MAX_VECTOR) { error = E2BIG; break; }
            error = LSBPlanAddString(plan, entry.UTF8String, &plan->envp[plan->envc++]);
        }
        if (!error) hostTerminal = [[LSBTerminalSession alloc] initWithDirectory:runtimeRoot.stringByDeletingLastPathComponent error:&error];
        if (!error) error = [hostTerminal attachToMessage:message];
        if (error) { [hostTerminal close]; hostTerminal = nil; }
    }
    for (int number = 0; !terminal && !error && number < 3; ++number) {
        int flags = fcntl(number, F_GETFD);
        if (flags < 0) continue;
        int duplicate = fcntl(number, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) return errno;
        unsigned index = plan->fdCount++;
        plan->fds[index] = (LSBInheritedFD){number, (uint32_t)flags};
        message->fds[index] = duplicate;
    }
    return error ? error : LSBValidateSpawnPlan(plan);
}

int LSBStartHost(int argc, const char *argv[], const char *envp[]) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return EPERM;
        if (jobs) return EALREADY;
        if (argc < 1 || !argv || !argv[0]) return EINVAL;
        int error = prepareRuntimeRoot();
        if (error) { fprintf(stderr, "[broker] Startup failed: %s\n", strerror(error)); return error; }
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        if (!initial) return ENOMEM;
        error = initialPlan(initial, argc, argv, envp);
        if (error) { disposeMessage(initial); fprintf(stderr, "[broker] Invalid initial plan: %s\n", strerror(error)); return error; }
        mach_port_t service = MACH_PORT_NULL;
        kern_return_t status = bootstrap_check_in(bootstrap_port, LSB_BROKER_SERVICE, &service);
        if (status) { disposeMessage(initial); fprintf(stderr, "[broker] bootstrap_check_in: %d\n", status); return EIO; }
        jobs = [NSMutableArray new];
        hostTerminal.signalHandler = ^int(pid_t foreground, int number) { return signalTerminal(foreground, number); };
        [hostTerminal start];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            unsigned receiveFailures = 0;
            for (;;) {
                LSBReceivedMessage *message = calloc(1, sizeof(*message));
                if (!message) abort();
                int receiveError = LSBTransportReceive(service, message);
                if (receiveError) {
                    free(message);
                    // Report the first failed receive in a burst, then at most once per second under a malformed-message flood.
                    if (!(receiveFailures++ % 100)) fprintf(stderr, "[broker] receive failed: error=%d port=%u\n", receiveError, service);
                    mach_port_type_t type = 0;
                    if (mach_port_type(mach_task_self(), service, &type) || !(type & MACH_PORT_TYPE_RECEIVE)) {
                        fprintf(stderr, "[broker] Receive right is no longer valid; stopping receiver\n");
                        return;
                    }
                    usleep(10000);
                    continue;
                }
                receiveFailures = 0;
                dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { handleMessage(message); } });
            }
        });
        enqueueSpawn(initial, nil);
        return 0;
    }
}
