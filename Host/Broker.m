#import <Foundation/Foundation.h>
#include "Broker.h"
#include "ExtensionLauncher.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
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
@end
@implementation LSBJob
@end

static NSMutableArray<LSBJob *> *jobs;
static LSBJob *startingJob;
static NSString *runtimeRoot;
static uint64_t nextIdentifier = 1;
static BOOL launchLaneFailed;
static void startNextJob(void);

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

static void finishJob(LSBJob *job, int error) {
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
    LSBReleaseExtension(job.identifier);
    if (startingJob == job) startingJob = nil;
    [jobs removeObject:job];
    startNextJob();
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
    if (startingJob) return;
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

static void enqueueSpawn(LSBReceivedMessage *message) {
    if (launchLaneFailed) { replyError(message, EBUSY); return; }
    if (jobs.count >= 16) { replyError(message, EAGAIN); return; }
    LSBJob *job = [LSBJob new];
    job.identifier = nextIdentifier++;
    job.request = message;
    [jobs addObject:job];
    startNextJob();
}

static void handleMessage(LSBReceivedMessage *message) {
    pid_t pid = (pid_t)message->audit.val[5];
    uint32_t generation = message->audit.val[7];
    LSBJob *sender = nil;
    for (LSBJob *job in jobs) if (job.pid == pid && job.pidVersion == generation) { sender = job; break; }
    switch (message->kind) {
        case LSB_REQUEST_SPAWN:
            if (!sender || sender.phase != JobRunning) { replyError(message, EACCES); return; }
            enqueueSpawn(message);
            return;
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
            job.phase = JobDispatched;
            uint64_t identifier = job.identifier;
            job.exitMonitor = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
            if (!job.exitMonitor) { replyError(message, ENOMEM); finishJob(job, ENOMEM); return; }
            dispatch_source_set_event_handler(job.exitMonitor, ^{
                LSBJob *exited = findJob(identifier);
                if (exited) finishJob(exited, 0);
            });
            dispatch_resume(job.exitMonitor);
            int error = LSBTransportReply(message, 0, pid, identifier, runtimeRoot.fileSystemRepresentation,
                                          &job.request->plan, job.request->fds);
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
            int replyError = LSBTransportReply(message, 0, pid, sender.identifier, NULL, NULL, NULL);
            disposeMessage(message);
            if (error || replyError) { finishJob(sender, error ? error : replyError); return; }
            sender.phase = JobRunning;
            if (sender.request->object) LSBTransportReply(sender.request, 0, pid, sender.identifier, NULL, NULL, NULL);
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
    const char *defaultArgs[] = { "/bin/spawn-probe", NULL };
    const char *const *arguments = argc > 1 ? argv + 1 : defaultArgs;
    int count = argc > 1 ? argc - 1 : 1;
    int error = LSBPlanAddString(plan, arguments[0], &plan->path);
    if (!error) error = LSBPlanAddString(plan, runtimeRoot.fileSystemRepresentation, &plan->cwd);
    for (int i = 0; !error && i < count; ++i) {
        if (plan->argc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, arguments[i], &plan->argv[plan->argc++]);
    }
    for (const char **item = envp; !error && item && *item; ++item) {
        if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, *item, &plan->envp[plan->envc++]);
    }
    plan->creationMask = 022;
    sigprocmask(0, NULL, (sigset_t *)&plan->signalMask);
    for (int number = 0; !error && number < 3; ++number) {
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
        enqueueSpawn(initial);
        return 0;
    }
}
