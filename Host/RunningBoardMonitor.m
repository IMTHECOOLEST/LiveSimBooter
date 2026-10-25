#import "RunningBoardMonitor.h"
#import "Private/RunningBoardPrivate.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>

@interface LSBRunningBoardMonitor : NSObject
@property (nonatomic, strong) RBSProcessHandle *handle;
@property (nonatomic, copy) LSBRunningBoardExitHandler completion;
@end
@implementation LSBRunningBoardMonitor
@end

static int exitStatus(RBSProcessExitContext *context, int *status) {
    // RBS gives voluntary exits type 1 but intentionally returns nil from -status (domain 0).
    // Its error path fabricates an empty context: type 0, no timestamp. Never interpret that as exit(0).
    if (!context.timestamp || (context.type != 1 && !(context.type == 2 && context.status.isValid))) return EPROTO;
    int value = context.legacyCode;
    if (value < 0 || value > UINT16_MAX || (!WIFEXITED(value) && !WIFSIGNALED(value))) return EPROTO;
    if (WIFSIGNALED(value) && (WTERMSIG(value) <= 0 || WTERMSIG(value) >= NSIG)) return EPROTO;
    *status = value;
    return 0;
}

static RBSProcessHandle *validatedHandle(const audit_token_t *audit, int *error) {
    NSError *lookupError = nil;
    RBSProcessHandle *handle = [RBSProcessHandle handleForAuditToken:*audit error:&lookupError];
    if (!handle || handle.pid != (pid_t)audit->val[5]) {
        fprintf(stderr, "[broker] RunningBoard process lookup failed: %s\n", lookupError.description.UTF8String ?: "PID mismatch");
        *error = [lookupError.domain isEqualToString:NSPOSIXErrorDomain] && lookupError.code > 0 ? (int)lookupError.code : ESRCH;
        return nil;
    }
    *error = 0;
    return handle;
}

int LSBRunningBoardValidateIdentity(const audit_token_t *audit) {
    if (![NSThread isMainThread]) return EPERM;
    if (!audit || !audit->val[5]) return EINVAL;
    int error = 0;
    validatedHandle(audit, &error);
    return error;
}

int LSBRunningBoardMonitorStart(const audit_token_t *audit, LSBRunningBoardExitHandler handler, LSBRunningBoardMonitor **result) {
    if (![NSThread isMainThread]) return EPERM;
    if (!audit || !handler || !result || !audit->val[5]) return EINVAL;
    *result = nil;
    int lookupError = 0;
    RBSProcessHandle *handle = validatedHandle(audit, &lookupError);
    if (!handle) return lookupError;
    LSBRunningBoardMonitor *monitor = [LSBRunningBoardMonitor new];
    monitor.handle = handle;
    monitor.completion = handler;
    pid_t expectedPID = (pid_t)audit->val[5];
    __weak LSBRunningBoardMonitor *weakMonitor = monitor;
    [handle monitorForDeath:^(RBSProcessHandle *exited, RBSProcessExitContext *context) {
        int status = 0;
        int error = exited.pid == expectedPID ? exitStatus(context, &status) : EPROTO;
        if (error) fprintf(stderr, "[broker] Invalid RunningBoard exit context: %s\n", context.description.UTF8String ?: "missing");
        dispatch_async(dispatch_get_main_queue(), ^{
            LSBRunningBoardMonitor *retained = weakMonitor;
            LSBRunningBoardExitHandler completion = retained.completion;
            retained.completion = nil;
            if (completion) completion(status, error);
        });
    }];
    *result = monitor;
    return 0;
}

void LSBRunningBoardMonitorCancel(LSBRunningBoardMonitor *monitor) {
    NSCAssert([NSThread isMainThread], @"RunningBoard monitor cancellation requires the main thread");
    monitor.completion = nil;
    monitor.handle = nil;
}
