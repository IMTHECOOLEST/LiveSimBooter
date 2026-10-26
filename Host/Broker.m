#import <Foundation/Foundation.h>
#include "Broker.h"
#include "ExtensionLauncher.h"
#include "ChildMonitor.h"
#include "RunningBoardMonitor.h"
#include "ChildStatus.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "LaunchdHandshake.h"
#include "LaunchdProbe.h"
#include "LaunchdConfiguration.h"
#include "LaunchdEndpoint.h"
#include "NativeMetalControl.h"
#import "IndigoHIDServer.h"
#import "FramebufferServer.h"
#import "IOSurfaceServer.h"
#import "TerminalSession.h"
#include "FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { JobQueued, JobStarting, JobDispatched, JobRunning };
@interface LSBJob : NSObject
@property uint64_t identifier;
@property int phase;
@property uint32_t executionRole;
@property pid_t pid;
@property pid_t guestPID;
@property uint64_t replacesIdentifier, replacementIdentifier;
@property BOOL executed;
@property uint32_t pidVersion;
@property pid_t extensionPID;
@property LSBReceivedMessage *request;
@property dispatch_source_t exitMonitor;
@property (nonatomic, strong) LSBRunningBoardMonitor *runningBoardMonitor;
@property audit_token_t audit;
@property uint64_t parentIdentifier;
@property LSBChildTicket parentTicket;
@property BOOL parentStatusPublished, parentStatusPending, retiredForExec;
@property LSBChildStatus *childStatus;
@property int statusFD, wakeReadFD, wakeWriteFD;
@property BOOL terminalSession;
@property BOOL simulatorLaunchd;
@property LSBLaunchdHandshake *launchdHandshake;
@property dispatch_source_t launchdSource;
@property (nonatomic, strong) LSBIndigoHIDServer *indigo;
@property (nonatomic, strong) LSBFramebufferServer *framebuffer;
@property (nonatomic, strong) LSBIOSurfaceServer *iosurface;
@property BOOL servicesPending;
@property int servicesError;
@property uint64_t indigoAuthorizedIdentifier;
@property uint64_t framebufferAuthorizedIdentifier;
@property uint64_t iosurfaceAuthorizedIdentifier;
@property uint64_t nativeMetalServiceIdentifier, nativeMetalAuthorizedIdentifier;
@property LSBNativeMetalControl *nativeMetalControl;
@property mach_port_t nativeServicePort;
@property BOOL finishing;
@end
@implementation LSBJob
- (instancetype)init {
    if ((self = [super init])) _statusFD = _wakeReadFD = _wakeWriteFD = -1;
    return self;
}
- (void)dealloc {
    if (_nativeMetalControl) LSBNativeMetalControlUnmap(_nativeMetalControl);
    if (MACH_PORT_VALID(_nativeServicePort)) mach_port_deallocate(mach_task_self(), _nativeServicePort);
    if (_launchdHandshake) { LSBLaunchdHandshakeDispose(_launchdHandshake); free(_launchdHandshake); }
    if (_childStatus) munmap(_childStatus, LSB_CHILD_STATUS_MAPPING_SIZE);
    if (_statusFD >= 0) close(_statusFD);
    if (_wakeReadFD >= 0) close(_wakeReadFD);
    if (_wakeWriteFD >= 0) close(_wakeWriteFD);
}
@end

@interface LSBPendingAdoption : NSObject
@property uint64_t childIdentifier, parentIdentifier;
@property LSBChildObservation observation;
@end
@implementation LSBPendingAdoption
@end

static NSMutableArray<LSBJob *> *jobs;
static NSMutableArray<LSBPendingAdoption *> *pendingAdoptions;
static dispatch_source_t adoptionRetrySource;
static LSBJob *startingJob;
static NSString *runtimeRoot;
static uint64_t nextIdentifier = 1;
static BOOL launchLaneFailed;
static unsigned finishingDepth;
static LSBTerminalSession *hostTerminal;
static BOOL enableIndigo;
// Allow every launchd child to overlap its exec replacement, plus simulator launchd and its native Metal helper.
enum { BrokerJobCapacity = 2 * LSB_CHILD_STATUS_CAPACITY + 2, DisplayRegistrationTimeoutSeconds = 30,
       RetiredIdentityCapacity = 256, AuditResolutionLogLimit = 32 };
typedef struct { audit_token_t audit; pid_t guestPID; } LSBRetiredIdentity;
static LSBRetiredIdentity retiredIdentities[RetiredIdentityCapacity];
static unsigned retiredIdentityCount, auditResolutionLogs;
static BOOL retiredIdentitiesFull;
NSString *const LSBHostIndigoDidChangeNotification = @"LSBHostIndigoDidChange";
NSString *const LSBHostFramebufferDidChangeNotification = @"LSBHostFramebufferDidChange";
static void startNextJob(void);
static int signalJob(LSBJob *child, int signalNumber);

LSBTerminalSession *LSBHostTerminal(void) { return hostTerminal; }
int LSBHostUsesSimulatorDisplay(void) { return enableIndigo; }
LSBIndigoHIDServer *LSBHostIndigoHIDServer(void) {
    for (LSBJob *job in jobs) if (job.simulatorLaunchd && job.indigo) return job.indigo;
    return nil;
}
LSBFramebufferServer *LSBHostFramebufferServer(void) {
    for (LSBJob *job in jobs) if (job.simulatorLaunchd && job.framebuffer) return job.framebuffer;
    return nil;
}

static LSBJob *findJob(uint64_t identifier) {
    for (LSBJob *job in jobs) if (job.identifier == identifier) return job;
    return nil;
}

static BOOL isBackboardProgram(const LSBReceivedMessage *request) {
    const char *program = request->plan.strings + request->plan.path;
    if (!strcmp(program, "/usr/libexec/backboardd")) return YES;
    NSString *path = [runtimeRoot stringByAppendingPathComponent:@"usr/libexec/backboardd"];
    return !strcmp(program, path.fileSystemRepresentation);
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

static int inheritChildStatus(LSBJob *job, LSBJob *old) {
    // An exec retains the process's children. Both native instances briefly map the same ledger and wake endpoint.
    job.statusFD = fcntl(old.statusFD, F_DUPFD_CLOEXEC, 3);
    if (job.statusFD < 0) return errno;
    job.wakeReadFD = fcntl(old.wakeReadFD, F_DUPFD_CLOEXEC, 3);
    if (job.wakeReadFD < 0) return errno;
    job.wakeWriteFD = fcntl(old.wakeWriteFD, F_DUPFD_CLOEXEC, 3);
    if (job.wakeWriteFD < 0) return errno;
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, job.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    job.childStatus = mapping;
    return LSBChildStatusValidate(job.childStatus);
}

static void wakeParent(LSBJob *parent) {
    const char hint = 1;
    ssize_t written;
    do { written = write(parent.wakeWriteFD, &hint, 1); } while (written < 0 && errno == EINTR);
    if (written < 0 && errno != EAGAIN) fprintf(stderr, "[broker] Child wake failed: %s\n", strerror(errno));
}

static void signalChildExit(LSBJob *parent) {
    wakeParent(parent);
    audit_token_t identity = parent.audit;
    int error = LSBChildSignal(&identity, SIGCHLD);
    if (error == EACCES || error == EPERM) {
        error = LSBSignalExtension(parent.identifier, SIGCHLD);
        if (!error) fprintf(stderr, "[broker] SIGCHLD submitted through launchd for request=%llu pid=%d\n", parent.identifier, parent.pid);
    }
    if (error && error != ESRCH) fprintf(stderr, "[broker] SIGCHLD delivery failed: %s\n", strerror(error));
}

static LSBPendingAdoption *pendingAdoption(uint64_t childIdentifier) {
    for (LSBPendingAdoption *pending in pendingAdoptions) if (pending.childIdentifier == childIdentifier) return pending;
    return nil;
}

static void retryAdoptions(void) {
    for (LSBPendingAdoption *pending in [pendingAdoptions copy]) {
        LSBJob *parent = findJob(pending.parentIdentifier), *child = findJob(pending.childIdentifier);
        if (!parent || parent.phase != JobRunning || (!pending.observation.exited && !child)) {
            if (child) {
                child.parentIdentifier = 0;
                child.parentStatusPending = NO;
                LSBChildStatusSetParentPID(child.childStatus, 0);
            }
            [pendingAdoptions removeObject:pending];
            continue;
        }
        LSBChildTicket ticket;
        LSBChildObservation observation = pending.observation;
        int error = LSBChildStatusImport(parent.childStatus, &observation, &ticket);
        if (error == EAGAIN) continue;
        if (error) {
            fprintf(stderr, "[broker] Orphan import guest=%d failed: %s\n", observation.guestPID, strerror(error));
            continue;
        }
        if (child) { child.parentTicket = ticket; child.parentStatusPending = NO; }
        [pendingAdoptions removeObject:pending];
        if (observation.exited) signalChildExit(parent);
        else wakeParent(parent);
        fprintf(stderr, "[broker] adopted guest=%d by guest=%d native=%d exited=%d\n",
                observation.guestPID, parent.guestPID, observation.nativePID, observation.exited);
    }
    if (!pendingAdoptions.count && adoptionRetrySource) {
        dispatch_source_cancel(adoptionRetrySource);
        adoptionRetrySource = nil;
    }
}

static void queueAdoption(LSBJob *parent, LSBJob *child, const LSBChildObservation *observation) {
    if (!pendingAdoptions) pendingAdoptions = [NSMutableArray new];
    LSBPendingAdoption *pending = [LSBPendingAdoption new];
    pending.parentIdentifier = parent.identifier;
    pending.childIdentifier = child.identifier;
    pending.observation = *observation;
    [pendingAdoptions addObject:pending];
    if (child) {
        child.parentIdentifier = parent.identifier;
        child.parentStatusPending = YES;
        LSBChildStatusSetParentPID(child.childStatus, parent.guestPID);
    }
    retryAdoptions();
    if (pendingAdoptions.count && !adoptionRetrySource) {
        // Keep copied statuses until launchd reaps a slot. Backpressure on new spawns bounds this backlog.
        adoptionRetrySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(adoptionRetrySource, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(adoptionRetrySource, ^{ retryAdoptions(); });
        dispatch_resume(adoptionRetrySource);
    }
}

static LSBJob *adoptiveLaunchd(LSBJob *exiting) {
    // Simulator launchd is a host-rooted process. Requiring that root also excludes a descendant or an adoption cycle.
    for (LSBJob *job in jobs)
        if (job != exiting && job.simulatorLaunchd && job.phase == JobRunning && !job.parentIdentifier) return job;
    return nil;
}

static void retireAuditIdentity(LSBJob *job) {
    if (job.pid <= 0 || job.audit.val[5] != (uint32_t)job.pid) return;
    if (retiredIdentityCount < RetiredIdentityCapacity) {
        retiredIdentities[retiredIdentityCount++] = (LSBRetiredIdentity){job.audit, job.guestPID};
    } else if (!retiredIdentitiesFull) {
        // Never evict an old identity and accidentally let it alias a guest job through the native fallback.
        retiredIdentitiesFull = YES;
        fprintf(stderr, "[broker] Audit identity history full; unmanaged owner resolution now fails closed\n");
    }
}

static int resolveAuditPID(const audit_token_t *audit, pid_t *result) {
    *result = 0;
    if (!audit || !audit->val[5] || audit->val[5] > INT_MAX) return EINVAL;
    pid_t native = (pid_t)audit->val[5];
    // Exact full-token matching precedes PID alias checks, including when a native PID has been reused legitimately.
    for (LSBJob *job in jobs) {
        audit_token_t current = job.audit;
        if (memcmp(audit, &current, sizeof(current))) continue;
        if (job.phase != JobRunning || job.retiredForExec || job.guestPID <= 0) return ESRCH;
        *result = job.guestPID;
        if (native != *result && auditResolutionLogs < AuditResolutionLogLimit) {
            fprintf(stderr, "[broker] audit owner native=%d version=%u -> guest=%d\n", native, audit->val[7], *result);
            if (++auditResolutionLogs == AuditResolutionLogLimit) fprintf(stderr, "[broker] Further audit owner resolution logs suppressed\n");
        }
        return 0;
    }
    // A different credential/generation, a pending replacement, or a native identity equal to a stable guest PID cannot inherit ownership.
    for (LSBJob *job in jobs) if (native == job.pid || native == job.guestPID) return ESRCH;
    for (unsigned index = 0; index < retiredIdentityCount; ++index) {
        const LSBRetiredIdentity *old = &retiredIdentities[index];
        if (native == (pid_t)old->audit.val[5] || native == old->guestPID) return ESRCH;
    }
    if (retiredIdentitiesFull) return ESRCH;
    // Only authenticated simulator launchd can supply this token, copied from its own kernel-audited request.
    *result = native;
    return 0;
}

static void finishJob(LSBJob *job, int error) {
    if (!findJob(job.identifier) || job.finishing) return;
    job.finishing = YES;
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
    if (job.launchdSource) { dispatch_source_cancel(job.launchdSource); job.launchdSource = nil; }
    if (job.indigo) [job.indigo close];
    BOOL framebufferChanged = job.framebuffer != nil;
    if (job.framebuffer) [job.framebuffer close];
    if (job.iosurface) [job.iosurface close];
    // This helper belongs to the isolated launchd session, never to its simulated child ledger.
    // Revoke before terminating either endpoint owner; a queued native request must stop authenticating immediately.
    if (job.nativeMetalControl) {
        int revokeError = LSBNativeMetalControlPublish(job.nativeMetalControl, NULL);
        if (revokeError) fprintf(stderr, "[Metal] authorization revoke failed: %s\n", strerror(revokeError));
        if (job.pid > 0) {
            int stopError = signalJob(job, SIGKILL);
            if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] helper termination failed: %s\n", strerror(stopError));
        }
        uint64_t authorized = job.nativeMetalAuthorizedIdentifier;
        job.nativeMetalAuthorizedIdentifier = 0;
        if (MACH_PORT_VALID(job.nativeServicePort)) {
            mach_port_deallocate(mach_task_self(), job.nativeServicePort);
            job.nativeServicePort = MACH_PORT_NULL;
        }
        for (LSBJob *dependent in [jobs copy]) if (dependent.nativeMetalServiceIdentifier == job.identifier) {
            dependent.servicesPending = NO;
            dependent.servicesError = error ? error : EPIPE;
            if (dependent.phase != JobRunning) finishJob(dependent, dependent.servicesError);
        }
        LSBJob *owner = findJob(authorized);
        if (owner && !owner.finishing) {
            int stopError = signalJob(owner, SIGKILL);
            if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] owner termination failed: %s\n", strerror(stopError));
            finishJob(owner, EPIPE);
        }
    }
    for (LSBJob *helper in [jobs copy]) {
        if (helper.finishing || helper.executionRole != LSB_EXECUTION_NATIVE_METAL_SERVICE) continue;
        if (helper.identifier != job.nativeMetalServiceIdentifier && helper.nativeMetalAuthorizedIdentifier != job.identifier) continue;
        int revokeError = LSBNativeMetalControlPublish(helper.nativeMetalControl, NULL);
        if (helper.nativeMetalAuthorizedIdentifier == job.identifier) helper.nativeMetalAuthorizedIdentifier = 0;
        if (revokeError) fprintf(stderr, "[Metal] authorization revoke failed: %s\n", strerror(revokeError));
        finishJob(helper, ECANCELED);
    }
    for (LSBJob *owner in jobs) {
        if (owner.indigoAuthorizedIdentifier == job.identifier) {
            [owner.indigo authorizeClientWithAuditToken:NULL];
            owner.indigoAuthorizedIdentifier = 0;
        }
        if (owner.framebufferAuthorizedIdentifier == job.identifier) {
            [owner.framebuffer authorizeClientWithAuditToken:NULL];
            owner.framebufferAuthorizedIdentifier = 0;
            framebufferChanged = YES;
        }
        if (owner.iosurfaceAuthorizedIdentifier == job.identifier) {
            [owner.iosurface authorizeClientWithAuditToken:NULL];
            owner.iosurfaceAuthorizedIdentifier = 0;
            framebufferChanged = YES;
        }
    }
    if (job.runningBoardMonitor) { LSBRunningBoardMonitorCancel(job.runningBoardMonitor); job.runningBoardMonitor = nil; }
    LSBJob *old = findJob(job.replacesIdentifier);
    if (old && old.replacementIdentifier == job.identifier) old.replacementIdentifier = 0;
    LSBJob *replacement = findJob(job.replacementIdentifier);
    job.replacementIdentifier = 0;
    if (replacement) finishJob(replacement, ECANCELED);
    LSBJob *parent = findJob(job.parentIdentifier);
    if (parent && !job.parentStatusPublished && !job.parentStatusPending && !LSBChildStatusAbandon(parent.childStatus, job.parentTicket)) wakeParent(parent);
    LSBPendingAdoption *pending = pendingAdoption(job.identifier);
    if (pending) [pendingAdoptions removeObject:pending]; // Unknown monitor failure must not invent a successful exit.
    LSBJob *adopter = job.retiredForExec ? nil : adoptiveLaunchd(job);
    for (LSBJob *child in [jobs copy]) {
        if (child.parentIdentifier != job.identifier) continue;
        if (child.phase != JobRunning) {
            child.parentIdentifier = 0;
            finishJob(child, ECANCELED);
            continue;
        }
        LSBChildObservation observation;
        int observed = LSBChildStatusObserve(job.childStatus, child.guestPID, &observation);
        if (adopter && !observed) queueAdoption(adopter, child, &observation);
        else {
            child.parentIdentifier = 0;
            child.parentStatusPending = NO;
            LSBChildStatusSetParentPID(child.childStatus, 0);
            fprintf(stderr, "[broker] orphan guest=%d has no simulated wait owner; getppid=0\n", child.guestPID);
        }
        if (!observed) LSBChildStatusAbandon(job.childStatus, observation.ticket);
    }
    // Exited but unreaped children no longer have LSBJob objects. Preserve their statuses as well.
    if (!job.retiredForExec && adopter) for (unsigned index = 0; index < LSB_CHILD_STATUS_CAPACITY; ++index) {
        LSBChildObservation observation;
        if (!LSBChildStatusObserveIndex(job.childStatus, index, &observation) && observation.exited) {
            queueAdoption(adopter, nil, &observation);
            LSBChildStatusAbandon(job.childStatus, observation.ticket);
        }
    }
    LSBReleaseExtension(job.identifier);
    retireAuditIdentity(job);
    if (startingJob == job) startingJob = nil;
    [jobs removeObject:job];
    if (framebufferChanged) [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
    retryAdoptions();
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
        LSBPendingAdoption *pending = job.parentStatusPending ? pendingAdoption(job.identifier) : nil;
        if (pending) {
            LSBChildObservation observation = pending.observation;
            observation.exited = 1;
            observation.status = waitStatus;
            pending.observation = observation;
            pending.childIdentifier = 0;
            job.parentStatusPublished = YES;
        } else {
            int error = LSBChildStatusPublishExit(parent.childStatus, job.parentTicket, waitStatus);
            if (error) fprintf(stderr, "[broker] Could not publish child status: %s\n", strerror(error));
            else { job.parentStatusPublished = YES; signalChildExit(parent); }
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
    if (!error) return;
    if (job.phase != JobRunning) { finishJob(job, error); return; }
    if (job.executionRole == LSB_EXECUTION_SIMULATOR) return;
    audit_token_t identity = job.audit;
    // Process exit also invalidates assertions. Keep its monitor alive to report the actual exit, and never signal a reused PID.
    if (LSBRunningBoardValidateIdentity(&identity) == ESRCH) return;
    fprintf(stderr, "[Metal] native request=%llu lost its process assertion: %s\n", identifier, strerror(error));
    // Service cleanup revokes its shared authorization before terminating. The construction probe has no shared control page.
    if (!job.nativeMetalControl) {
        int stopError = signalJob(job, SIGKILL);
        if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] probe termination failed: %s\n", strerror(stopError));
    }
    finishJob(job, error);
}

static void startNextJob(void) {
    if (startingJob || finishingDepth) return;
    if (launchLaneFailed) {
        for (LSBJob *job in [jobs copy]) if (job.phase == JobQueued) { finishJob(job, EBUSY); return; }
        return;
    }
    for (LSBJob *job in jobs) {
        if (job.phase != JobQueued) continue;
        if (job.nativeMetalServiceIdentifier) {
            LSBJob *helper = findJob(job.nativeMetalServiceIdentifier);
            if (!helper) { finishJob(job, EPIPE); return; }
            if (helper.phase != JobRunning) continue;
            if (!MACH_PORT_VALID(helper.nativeServicePort)) { finishJob(job, EPIPE); return; }
        }
        LSBJob *old = findJob(job.replacesIdentifier);
        LSBJob *parent = findJob(old ? old.parentIdentifier : job.parentIdentifier);
        // Let xpcproxy return READY so launchd can answer registration before SETEXEC or a direct backboardd launch.
        BOOL needsServices = old || isBackboardProgram(job.request);
        if (needsServices && parent.servicesPending) continue;
        if (needsServices && parent.servicesError) { finishJob(job, parent.servicesError); return; }
        startingJob = job;
        job.phase = JobStarting;
        uint64_t identifier = job.identifier;
        uint32_t launchOptions = job.executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE || job.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE
            ? LSB_EXTENSION_LAUNCH_GPU : 0;
        int error = LSBLaunchExtensionWithOptions(LSB_EXTENSION_IDENTIFIER, identifier, launchOptions, extensionEvent, NULL);
        if (error) { job.phase = JobQueued; finishJob(job, error); return; }
        fprintf(stderr, "[broker] extension request=%llu submitted\n", identifier);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            LSBJob *pending = findJob(identifier);
            if (pending && pending.phase != JobRunning) finishJob(pending, ETIMEDOUT);
        });
        return;
    }
}

static LSBJob *enqueueJob(LSBReceivedMessage *message, LSBJob *parent, uint32_t executionRole) {
    if (executionRole > LSB_EXECUTION_NATIVE_METAL_SERVICE) { replyError(message, EINVAL); return nil; }
    // Only a locally constructed host request can select the native role. Guest RPCs never carry this authority.
    if (executionRole != LSB_EXECUTION_SIMULATOR &&
        (parent || message->object || message->ports.registeredPresent || message->ports.bootstrapPresent)) {
        replyError(message, EACCES); return nil;
    }
    retryAdoptions();
    if (pendingAdoptions.count) { replyError(message, EAGAIN); return nil; }
    if (launchLaneFailed) { replyError(message, EBUSY); return nil; }
    if (jobs.count >= BrokerJobCapacity) { replyError(message, EAGAIN); return nil; }
    LSBJob *job = [LSBJob new];
    job.executionRole = executionRole;
    int error = prepareChildStatus(job);
    if (error) { replyError(message, error); return nil; }
    if (parent) {
        LSBChildTicket ticket;
        error = LSBChildStatusReserve(parent.childStatus, &ticket);
        if (error) { replyError(message, error); return nil; }
        job.parentIdentifier = parent.identifier;
        job.parentTicket = ticket;
    }
    job.identifier = nextIdentifier++;
    job.terminalSession = parent ? parent.terminalSession : hostTerminal != nil;
    if (!parent && !strcmp(message->plan.strings + message->plan.path, "/sbin/launchd_sim")) {
        for (unsigned i = 0; i < message->plan.envc; ++i)
            if (!strcmp(message->plan.strings + message->plan.envp[i], "LSB_SIMULATOR_LAUNCHD=1")) job.simulatorLaunchd = YES;
    }
    job.request = message;
    [jobs addObject:job];
    startNextJob();
    return findJob(job.identifier);
}

static void enqueueSpawn(LSBReceivedMessage *message, LSBJob *parent) {
    enqueueJob(message, parent, LSB_EXECUTION_SIMULATOR);
}

static void enqueueExec(LSBReceivedMessage *message, LSBJob *old) {
    if (launchLaneFailed) { replyError(message, EBUSY); return; }
    if (old.replacementIdentifier || old.simulatorLaunchd) { replyError(message, ENOTSUP); return; }
    // A graphics replacement would discard live event ports, surfaces and gestures; token rollback cannot restore those capabilities.
    for (LSBJob *owner in jobs) if (owner.indigoAuthorizedIdentifier == old.identifier || owner.framebufferAuthorizedIdentifier == old.identifier ||
                                  owner.iosurfaceAuthorizedIdentifier == old.identifier || owner.nativeMetalAuthorizedIdentifier == old.identifier) {
        replyError(message, ENOTSUP); return;
    }
    if (jobs.count >= BrokerJobCapacity) { replyError(message, EAGAIN); return; }
    LSBJob *job = [LSBJob new];
    int error = inheritChildStatus(job, old);
    if (error) { replyError(message, error); return; }
    job.identifier = nextIdentifier++;
    job.guestPID = old.guestPID;
    job.terminalSession = old.terminalSession;
    job.replacesIdentifier = old.identifier;
    old.replacementIdentifier = job.identifier;
    job.request = message;
    [jobs addObject:job];
    fprintf(stderr, "[broker] exec guest=%d old-native=%d replacement-request=%llu\n", old.guestPID, old.pid, job.identifier);
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
        if (job.guestPID == targetPID && job.parentIdentifier == parent.identifier && job.phase == JobRunning) return signalJob(job, signalNumber);
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

static int registerDisplayEndpoint(mach_port_t service, const audit_token_t *audit, const char *name,
                                   mach_port_t endpoint, int64_t deadline, kern_return_t *status) {
    *status = -1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return errno;
    int64_t remaining = deadline - (int64_t)now.tv_sec * NSEC_PER_SEC - now.tv_nsec;
    if (remaining <= 0) return ETIMEDOUT;
    uint32_t milliseconds = (uint32_t)((remaining + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
    return LSBLaunchdRegisterEndpointWithTimeout(service, audit, name, endpoint, milliseconds, status);
}

static void registerDisplayServices(LSBJob *active, mach_port_t service) {
    if (!active.servicesPending) return;
    LSBJob *helper = findJob(active.nativeMetalServiceIdentifier);
    mach_port_t metal = helper.nativeServicePort;
    if (active.nativeMetalServiceIdentifier && (!helper || helper.phase != JobRunning || !MACH_PORT_VALID(metal))) {
        active.servicesPending = NO;
        active.servicesError = EPIPE;
        startNextJob();
        return;
    }
    kern_return_t copied = mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, 1);
    if (copied) { active.servicesPending = NO; active.servicesError = EPIPE; startNextJob(); return; }
    if (MACH_PORT_VALID(metal) && mach_port_mod_refs(mach_task_self(), metal, MACH_PORT_RIGHT_SEND, 1)) {
        mach_port_deallocate(mach_task_self(), service);
        active.servicesPending = NO;
        active.servicesError = EPIPE;
        startNextJob();
        return;
    }
    audit_token_t expected = active.audit;
    uint64_t identifier = active.identifier;
    uint64_t helperIdentifier = helper.identifier;
    LSBIndigoHIDServer *indigo = active.indigo;
    LSBFramebufferServer *framebuffer = active.framebuffer;
    LSBIOSurfaceServer *iosurface = active.iosurface;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // Launchd can spend several seconds synchronously spawning its initial xpcproxy batch before servicing the final registration.
        // Keep each original request/reply alive within one bounded startup budget; timeout is an unknown outcome, not retry permission.
        struct timespec now = {0};
        int clockError = clock_gettime(CLOCK_MONOTONIC, &now) ? errno : 0;
        int64_t deadline = ((int64_t)now.tv_sec + DisplayRegistrationTimeoutSeconds) * NSEC_PER_SEC + now.tv_nsec;
        kern_return_t indigoStatus = -1, iosurfaceStatus = -1, framebufferStatus = -1, metalStatus = -1, backboardMetalStatus = -1;
        int indigoError = clockError ? clockError : indigo ? registerDisplayEndpoint(service, &expected, "IndigoHIDRegistrationPort",
                                                                                    indigo.registrationPort, deadline, &indigoStatus) : 0;
        int endpointError = indigoError, iosurfaceError = ECANCELED, framebufferError = ECANCELED;
        if (!endpointError && iosurface) {
            iosurfaceError = registerDisplayEndpoint(service, &expected, "com.apple.IOSurface.Remote",
                                                     iosurface.registrationPort, deadline, &iosurfaceStatus);
            endpointError = iosurfaceError;
        }
        if (!endpointError && framebuffer) {
            framebufferError = registerDisplayEndpoint(service, &expected, "com.apple.CoreSimulator.SimFramebufferServer",
                                                        framebuffer.registrationPort, deadline, &framebufferStatus);
            endpointError = framebufferError;
        }
        int metalError = ECANCELED, backboardMetalError = ECANCELED;
        if (!endpointError && MACH_PORT_VALID(metal)) {
            metalError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator", metal, deadline, &metalStatus);
            endpointError = metalError;
        }
        if (!endpointError && MACH_PORT_VALID(metal)) {
            backboardMetalError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator.backboardd",
                                                          metal, deadline, &backboardMetalStatus);
            endpointError = backboardMetalError;
        }
        if (MACH_PORT_VALID(metal)) mach_port_deallocate(mach_task_self(), metal);
        mach_port_deallocate(mach_task_self(), service);
        dispatch_async(dispatch_get_main_queue(), ^{
            LSBJob *current = findJob(identifier);
            if (!current) return;
            current.servicesPending = NO;
            LSBJob *currentHelper = findJob(helperIdentifier);
            // A delayed registration reply must not clear an error from helper death or authorize a replacement endpoint.
            if (!current.servicesError) current.servicesError = helperIdentifier && (!currentHelper || currentHelper.phase != JobRunning) ? EPIPE : endpointError;
            if (indigo) fprintf(stderr, "[Indigo] registration error=%d bootstrap=%d\n", indigoError, indigoStatus);
            if (iosurface) fprintf(stderr, "[IOSurface] registration error=%d bootstrap=%d\n", iosurfaceError, iosurfaceStatus);
            if (framebuffer) fprintf(stderr, "[framebuffer] registration error=%d bootstrap=%d\n", framebufferError, framebufferStatus);
            if (helperIdentifier) fprintf(stderr, "[Metal] registration error=%d bootstrap=%d backboard-error=%d bootstrap=%d\n",
                                          metalError, metalStatus, backboardMetalError, backboardMetalStatus);
            startNextJob();
        });
    });
}

static int prepareLaunchdHandshake(LSBJob *job) {
    if (enableIndigo) {
        int error = 0;
        job.indigo = [[LSBIndigoHIDServer alloc] initWithError:&error];
        if (!job.indigo) return error ? error : ENOMEM;
        job.framebuffer = [[LSBFramebufferServer alloc] initWithError:&error];
        if (!job.framebuffer) return error ? error : ENOMEM;
        job.iosurface = [[LSBIOSurfaceServer alloc] initWithError:&error];
        if (!job.iosurface) return error ? error : ENOMEM;
        job.servicesPending = YES;
        [NSNotificationCenter.defaultCenter postNotificationName:LSBHostIndigoDidChangeNotification object:nil];
        [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
    }
    job.launchdHandshake = calloc(1, sizeof(*job.launchdHandshake));
    if (!job.launchdHandshake) return ENOMEM;
    audit_token_t audit = job.audit;
    int error = LSBLaunchdHandshakeInitialize(job.launchdHandshake, &audit);
    if (error) return error;
    mach_port_t tank = LSBLaunchdHandshakeTankPort(job.launchdHandshake);
    job.launchdSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, tank, 0, dispatch_get_main_queue());
    if (!job.launchdSource) return ENOMEM;
    uint64_t identifier = job.identifier;
    // Retain the state until its kevent registration has retired; successful MIG transfer leaves the service send right alive.
    dispatch_source_set_cancel_handler(job.launchdSource, ^{ LSBLaunchdHandshakeCloseTank(job.launchdHandshake); });
    dispatch_source_set_event_handler(job.launchdSource, ^{
        LSBJob *active = findJob(identifier);
        if (!active || !active.launchdHandshake) return;
        int result = LSBLaunchdHandshakeHandleNext(active.launchdHandshake);
        if (result == EAGAIN) return;
        if (!result || result == EALREADY) {
            fprintf(stderr, "[launchd] transferred service receive right to pid=%d; host send port=%u\n", active.pid,
                    LSBLaunchdHandshakeServicePort(active.launchdHandshake));
            const char *probe = getenv("LSB_LAUNCHD_HEALTHCHECK");
            mach_port_t service = LSBLaunchdHandshakeServicePort(active.launchdHandshake);
            if (active.servicesPending) registerDisplayServices(active, service);
            if (probe && !strcmp(probe, "1") && !mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, 1)) {
                audit_token_t expected = active.audit;
                pid_t process = active.pid;
                // This bounded diagnostic must not stall UIKit or the broker while launchd finishes entering its event loop.
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    kern_return_t bootstrapStatus = -1;
                    int probeError = LSBLaunchdProbeService(service, &expected, &bootstrapStatus);
                    fprintf(stderr, "[launchd] service lookup pid=%d error=%d bootstrap=%d\n", process, probeError, bootstrapStatus);
                    mach_port_deallocate(mach_task_self(), service);
                });
            }
            dispatch_source_cancel(active.launchdSource);
            active.launchdSource = nil;
        } else fprintf(stderr, "[launchd] tank request rejected for pid=%d: %s\n", active.pid, strerror(result));
    });
    dispatch_resume(job.launchdSource);
    return 0;
}

static void handleMessage(LSBReceivedMessage *message) {
    pid_t pid = (pid_t)message->audit.val[5];
    uint32_t generation = message->audit.val[7];
    LSBJob *sender = nil;
    for (LSBJob *job in jobs) if (job.pid == pid && job.pidVersion == generation) { sender = job; break; }
    switch (message->kind) {
        case LSB_REQUEST_SPAWN:
            if (!sender || sender.phase != JobRunning || sender.executionRole != LSB_EXECUTION_SIMULATOR) { replyError(message, EACCES); return; }
            enqueueSpawn(message, sender);
            return;
        case LSB_REQUEST_EXEC:
            if (!sender || sender.phase != JobRunning || sender.executionRole != LSB_EXECUTION_SIMULATOR) { replyError(message, EACCES); return; }
            enqueueExec(message, sender);
            return;
        case LSB_REQUEST_KILL: {
            if (!sender || sender.phase != JobRunning || sender.executionRole != LSB_EXECUTION_SIMULATOR) { replyError(message, EACCES); return; }
            int error = signalChild(sender, message->targetPID, message->signalNumber);
            LSBTransportReply(message, error, error ? 0 : message->targetPID, 0, NULL, NULL, NULL);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_RESOLVE_AUDIT_PID: {
            if (!sender || !sender.simulatorLaunchd || sender.phase != JobRunning || sender.retiredForExec) { replyError(message, EACCES); return; }
            audit_token_t senderAudit = sender.audit;
            if (memcmp(&message->audit, &senderAudit, sizeof(senderAudit))) { replyError(message, EACCES); return; }
            pid_t resolved = 0;
            int error = resolveAuditPID(&message->targetAudit, &resolved);
            LSBTransportReply(message, error, error ? 0 : resolved, 0, NULL, NULL, NULL);
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
            // A retired native PID can be reused while its guest identity still belongs to an exec replacement.
            if (!job.guestPID) for (LSBJob *active in jobs) {
                if (active != job && active.executed && active.guestPID == pid && active.pid != pid) {
                    replyError(message, EAGAIN); finishJob(job, EAGAIN); return;
                }
            }
            job.pid = pid;
            if (!job.guestPID) job.guestPID = pid;
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
            if (job.simulatorLaunchd) {
                error = prepareLaunchdHandshake(job);
                if (error) { replyError(message, error); finishJob(job, error); return; }
            }
            LSBJob *old = findJob(job.replacesIdentifier);
            LSBJob *parent = findJob(old ? old.parentIdentifier : job.parentIdentifier);
            if (parent.nativeMetalServiceIdentifier && isBackboardProgram(job.request)) {
                LSBJob *helper = findJob(parent.nativeMetalServiceIdentifier);
                audit_token_t backboardIdentity = job.audit;
                error = !helper || helper.phase != JobRunning || !MACH_PORT_VALID(helper.nativeServicePort) ? EPIPE :
                    helper.nativeMetalAuthorizedIdentifier ? EBUSY : LSBNativeMetalControlPublish(helper.nativeMetalControl, &backboardIdentity);
                if (error) { replyError(message, error); finishJob(job, error); return; }
                helper.nativeMetalAuthorizedIdentifier = job.identifier;
                fprintf(stderr, "[Metal] authorized backboard native=%d version=%u helper=%d\n", pid, generation, helper.pid);
            }
            if ((parent.indigo || parent.framebuffer || parent.iosurface) && isBackboardProgram(job.request)) {
                audit_token_t backboardIdentity = job.audit;
                if (parent.indigo) {
                    [parent.indigo authorizeClientWithAuditToken:&backboardIdentity];
                    parent.indigoAuthorizedIdentifier = job.identifier;
                }
                if (parent.framebuffer) {
                    [parent.framebuffer authorizeClientWithAuditToken:&backboardIdentity];
                    parent.framebufferAuthorizedIdentifier = job.identifier;
                    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
                }
                if (parent.iosurface) {
                    [parent.iosurface authorizeClientWithAuditToken:&backboardIdentity];
                    parent.iosurfaceAuthorizedIdentifier = job.identifier;
                }
            }
            LSBChildStatusSetParentPID(job.childStatus, parent ? parent.guestPID : old ? LSBChildStatusGetParentPID(old.childStatus) : getpid());
            LSBBootstrapMetadata metadata = { .guestPID = job.guestPID, .parentPID = LSBChildStatusGetParentPID(job.childStatus),
                .launchdTankPort = job.simulatorLaunchd ? LSBLaunchdHandshakeTankPort(job.launchdHandshake) : MACH_PORT_NULL,
                .ports = &job.request->ports, .executionRole = job.executionRole };
            error = LSBTransportReplyBootstrapWithMetadata(message, 0, pid, identifier, runtimeRoot.fileSystemRepresentation,
                &job.request->plan, job.request->fds, job.statusFD, job.wakeReadFD, &metadata);
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
            audit_token_t expectedAudit = sender.audit;
            BOOL nativeService = sender.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE;
            if (nativeService && memcmp(&message->audit, &expectedAudit, sizeof(expectedAudit))) { replyError(message, EACCES); return; }
            if (MACH_PORT_VALID(message->nativeServicePort) && (!nativeService || error)) error = EPROTO;
            if (!error && nativeService && !MACH_PORT_VALID(message->nativeServicePort)) error = EPROTO;
            if (!error && nativeService) {
                mach_port_type_t type = 0;
                if (mach_port_type(mach_task_self(), message->nativeServicePort, &type) || !(type & MACH_PORT_TYPE_SEND)) error = EPIPE;
            }
            if (!error && nativeService) {
                // Receive owns one send reference. Move it into the helper before disposing READY and advancing the launch lane.
                sender.nativeServicePort = message->nativeServicePort;
                message->nativeServicePort = MACH_PORT_NULL;
            }
            LSBJob *parent = findJob(sender.parentIdentifier);
            LSBJob *old = findJob(sender.replacesIdentifier);
            LSBChildExecUpdate execution = {0};
            BOOL preparedExec = NO;
            if (sender.replacesIdentifier && (!old || old.phase != JobRunning || old.replacementIdentifier != sender.identifier)) error = ESRCH;
            if (!error && old) {
                parent = findJob(old.parentIdentifier);
                if (parent && !old.parentStatusPending) {
                    error = LSBChildStatusPrepareExec(parent.childStatus, old.parentTicket, pid, &execution);
                    preparedExec = !error;
                } else if (old.parentStatusPending) {
                    LSBPendingAdoption *pending = pendingAdoption(old.identifier);
                    if (!pending) error = ESRCH;
                    else if (pending.observation.execGeneration == UINT32_MAX) error = EOVERFLOW;
                }
            }
            // The child is still blocked in READY: capture its real group before it can run and exit.
            if (!error && parent && !old) {
                pid_t group = getpgid(pid);
                error = group < 0 ? errno : LSBChildStatusPublishRunning(parent.childStatus, sender.parentTicket, sender.guestPID, group);
            }
            int replyError = LSBTransportReply(message, message->error ? 0 : error, pid, sender.identifier, NULL, NULL, NULL);
            disposeMessage(message);
            if (error || replyError) {
                if (preparedExec) LSBChildStatusCancelExec(parent.childStatus, &execution);
                finishJob(sender, error ? error : replyError);
                return;
            }
            sender.phase = JobRunning;
            if (old) {
                // This main-queue transaction owns Replacing until publication; exit callbacks cannot interleave it.
                // READY can release the child first: its subsequent owner-resolution RPC is also handled on this serial main queue,
                // so it cannot observe the candidate until this commit and retirement finish. A failed READY still preserves the old job.
                if (preparedExec) error = LSBChildStatusCommitExec(parent.childStatus, &execution);
                if (error) {
                    signalJob(sender, SIGKILL);
                    finishJob(sender, error);
                    return;
                }
                sender.parentIdentifier = old.parentIdentifier;
                sender.executed = YES;
                sender.parentTicket = old.parentTicket;
                sender.parentStatusPending = old.parentStatusPending;
                if (old.parentStatusPending) {
                    LSBPendingAdoption *pending = pendingAdoption(old.identifier);
                    LSBChildObservation observation = pending.observation;
                    observation.nativePID = pid;
                    ++observation.execGeneration;
                    pending.observation = observation;
                    pending.childIdentifier = sender.identifier;
                    old.parentStatusPending = NO;
                }
                old.retiredForExec = YES;
                sender.replacesIdentifier = 0;
                old.replacementIdentifier = 0;
                old.parentIdentifier = 0;
                for (LSBJob *child in jobs) if (child.parentIdentifier == old.identifier) child.parentIdentifier = sender.identifier;
                if (parent) wakeParent(parent);
                fprintf(stderr, "[broker] exec committed guest=%d native=%d -> %d\n", sender.guestPID, old.pid, pid);
            }
            if (sender.request->object) {
                error = LSBTransportReply(sender.request, 0, sender.guestPID, sender.identifier, NULL, NULL, NULL);
                // Once committed, a dead old RPC peer cannot roll the replacement back.
                if (error && !old) { finishJob(sender, error); return; }
            }
            disposeMessage(sender.request);
            sender.request = NULL;
            if (old) {
                int retireError = signalJob(old, SIGKILL);
                if (retireError && retireError != ESRCH) fprintf(stderr, "[broker] retire old exec instance pid=%d: %s\n", old.pid, strerror(retireError));
                finishJob(old, 0);
            }
            fprintf(stderr, "[broker] spawn ready guest=%d native=%d request=%llu\n", sender.guestPID, pid, sender.identifier);
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

static int initialExecutionRole(int argc, const char *argv[], uint32_t *role) {
    if (!role || argc < 1 || !argv || !argv[0]) return EINVAL;
    *role = LSB_EXECUTION_SIMULATOR;
    if (argc > 1 && !strcmp(argv[1], "--native-metal-probe")) {
        if (argc != 2) return EINVAL;
        *role = LSB_EXECUTION_NATIVE_METAL_PROBE;
    }
    return 0;
}

static int initialPlan(LSBReceivedMessage *message, int argc, const char *argv[], const char *envp[]) {
    LSBSpawnPlan *plan = &message->plan;
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    uint32_t executionRole;
    int roleError = initialExecutionRole(argc, argv, &executionRole);
    if (roleError) return roleError;
    BOOL nativeProbe = executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE;
    if (argc > 1 && !strcmp(argv[1], "--launchd"))
        return argc == 2 ? LSBPrepareLaunchdPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    if (argc > 1 && !strcmp(argv[1], "--launchd-df"))
        return argc == 2 ? LSBPrepareLaunchdDFPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    if (argc > 1 && !strcmp(argv[1], "--launchd-backboardd")) {
        enableIndigo = YES;
        return argc == 2 ? LSBPrepareLaunchdBackboardPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    }
    if (argc > 1 && !strcmp(argv[1], "--launchd-services")) {
        const char *defaults[] = {"com.apple.notifyd", "com.apple.lsd", "com.apple.runningboardd", "com.apple.containermanagerd",
                                  "com.apple.fontservicesd", "com.apple.containermanagerd.system", "com.apple.distnoted.xpc.daemon",
                                  "com.apple.configd_sim", NULL};
        enableIndigo = YES;
        return LSBPrepareLaunchdServicesPlan(message, runtimeRoot.fileSystemRepresentation, envp, argc > 2 ? argv + 2 : defaults);
    }
    if (argc > 1 && !strcmp(argv[1], "--launchd-lifecycle"))
        return argc == 2 ? LSBPrepareLaunchdLifecyclePlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    plan->version = LSB_PLAN_VERSION;
    const char *defaultArgs[] = {"/bin/fish", "-i", "-N", "-P", "-C",
        "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end", NULL};
    const char *nativeArgs[] = { "/__LiveSimBooterNativeMetalProbe", NULL };
    BOOL terminal = argc == 1 || (argc > 1 && !strcmp(argv[1], "--terminal"));
    int firstArgument = terminal && argc > 1 ? 2 : 1;
    const char *const *arguments = nativeProbe ? nativeArgs : argc > firstArgument ? argv + firstArgument : defaultArgs;
    int count = nativeProbe ? 1 : argc > firstArgument ? argc - firstArgument : 6;
    int error = LSBPlanAddString(plan, arguments[0], &plan->path);
    if (!error) error = LSBPlanAddString(plan, runtimeRoot.fileSystemRepresentation, &plan->cwd);
    for (int i = 0; !error && i < count; ++i) {
        if (plan->argc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, arguments[i], &plan->argv[plan->argc++]);
    }
    for (const char **item = nativeProbe ? NULL : envp; !error && item && *item; ++item) {
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

static int enqueueInitialJob(LSBReceivedMessage *initial, uint32_t executionRole) {
    if (!enableIndigo) { enqueueJob(initial, nil, executionRole); return 0; }
    // Only the host's graphics launch modes can provision a native service. Neither the plan pathname nor guest environment selects it.
    const char *arguments[] = { "LiveSimBooter", "--native-metal-probe", NULL };
    LSBReceivedMessage *request = calloc(1, sizeof(*request));
    if (!request) { disposeMessage(initial); return ENOMEM; }
    LSBNativeMetalControl *control = NULL;
    int readOnlyFD = -1;
    int error = initialPlan(request, 2, arguments, NULL);
    if (!error) error = LSBPlanAddString(&request->plan, "/__LiveSimBooterNativeMetalService", &request->plan.path);
    request->plan.argv[0] = request->plan.path;
    if (!error) error = LSBNativeMetalControlCreate(runtimeRoot.stringByDeletingLastPathComponent.fileSystemRepresentation, &control, &readOnlyFD);
    if (!error) {
        unsigned index = request->plan.fdCount++;
        request->plan.fds[index] = (LSBInheritedFD){3, 0};
        request->fds[index] = readOnlyFD;
        error = LSBValidateSpawnPlan(&request->plan);
    }
    if (error) {
        LSBNativeMetalControlUnmap(control);
        disposeMessage(request);
        disposeMessage(initial);
        return error;
    }
    // Install the dependency before either extension is submitted, including synchronous launch-error paths.
    ++finishingDepth;
    LSBJob *helper = enqueueJob(request, nil, LSB_EXECUTION_NATIVE_METAL_SERVICE);
    if (helper) helper.nativeMetalControl = control;
    else LSBNativeMetalControlUnmap(control);
    LSBJob *launchd = helper ? enqueueJob(initial, nil, executionRole) : nil;
    if (!helper) disposeMessage(initial);
    if (launchd) launchd.nativeMetalServiceIdentifier = helper.identifier;
    else if (helper) finishJob(helper, ECANCELED);
    --finishingDepth;
    startNextJob();
    return launchd ? 0 : EIO;
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
        uint32_t executionRole = LSB_EXECUTION_SIMULATOR;
        error = initialPlan(initial, argc, argv, envp);
        if (!error) error = initialExecutionRole(argc, argv, &executionRole);
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
        error = enqueueInitialJob(initial, executionRole);
        if (error) fprintf(stderr, "[broker] Initial service setup failed: %s\n", strerror(error));
        return error;
    }
}
