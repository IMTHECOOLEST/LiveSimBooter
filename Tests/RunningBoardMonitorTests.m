#import <Foundation/Foundation.h>
#include <assert.h>
#include <signal.h>
#define RBSProcessExitStatus LSBTestProcessExitStatus
#define RBSProcessExitContext LSBTestProcessExitContext
#define RBSProcessHandle LSBTestProcessHandle
#include "../Host/RunningBoardMonitor.m"

@interface RBSProcessExitStatus ()
@property (nonatomic, readwrite, getter=isValid) BOOL valid;
@end
@implementation RBSProcessExitStatus
@end
@interface RBSProcessExitContext ()
@property (nonatomic, readwrite) RBSProcessExitStatus *status;
@property (nonatomic, readwrite) NSDate *timestamp;
@property (nonatomic, readwrite) NSUInteger type;
@property (nonatomic, readwrite) int legacyCode;
@end
@implementation RBSProcessExitContext
@end
@interface RBSProcessHandle ()
@property (nonatomic, readwrite) pid_t pid;
@end
static RBSProcessHandle *lookedUpHandle;
static audit_token_t lookedUpToken;
static void (^deathCallback)(RBSProcessHandle *, RBSProcessExitContext *);
@implementation RBSProcessHandle
+ (instancetype)handleForAuditToken:(audit_token_t)token error:(NSError **)error {
    lookedUpToken = token;
    if (!lookedUpHandle && error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ESRCH userInfo:nil];
    return lookedUpHandle;
}
- (void)monitorForDeath:(void (^)(RBSProcessHandle *, RBSProcessExitContext *))handler { deathCallback = handler; }
@end

static RBSProcessExitContext *contextFor(NSUInteger type, int status, BOOL valid) {
    RBSProcessExitContext *context = [RBSProcessExitContext new];
    context.type = type;
    context.legacyCode = status;
    context.timestamp = [NSDate date];
    if (valid) { context.status = [RBSProcessExitStatus new]; context.status.valid = YES; }
    return context;
}

static void testStatusValidation(void) {
    int status = -1;
    assert(!exitStatus(contextFor(1, 0, NO), &status) && status == 0);
    assert(!exitStatus(contextFor(1, 37 << 8, NO), &status) && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(!exitStatus(contextFor(2, SIGTERM, YES), &status) && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(!exitStatus(contextFor(2, SIGABRT | 0x80, YES), &status) && WIFSIGNALED(status) && WCOREDUMP(status));
    assert(exitStatus([RBSProcessExitContext new], &status) == EPROTO);
    assert(exitStatus(contextFor(0, 0, NO), &status) == EPROTO);
    assert(exitStatus(contextFor(2, 0, NO), &status) == EPROTO);
    assert(exitStatus(contextFor(1, 0x7f, NO), &status) == EPROTO);
    assert(exitStatus(contextFor(1, 0x10000, NO), &status) == EPROTO);
    assert(exitStatus(contextFor(2, NSIG, YES), &status) == EPROTO);
    RBSProcessExitContext *noTimestamp = contextFor(1, 0, NO);
    noTimestamp.timestamp = nil;
    assert(exitStatus(noTimestamp, &status) == EPROTO);
}

static void drainMainQueue(void) {
    __block BOOL finished = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ finished = YES; });
    while (!finished) [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

static void testSubscriptionLifecycle(void) {
    audit_token_t token = {{11, 22, 33, 44, 55, 1234, 77, 888}};
    lookedUpHandle = [RBSProcessHandle new];
    lookedUpHandle.pid = 1234;
    __block unsigned delivered = 0;
    __block int actualStatus = -1, actualError = -1;
    LSBRunningBoardExitHandler handler = ^(int status, int error) { ++delivered; actualStatus = status; actualError = error; };
    LSBRunningBoardMonitor *monitor = nil;
    assert(!LSBRunningBoardMonitorStart(&token, handler, &monitor) && monitor && deathCallback);
    assert(!memcmp(&token, &lookedUpToken, sizeof(token)));
    // The callback must retain only the copied PID, never the caller's temporary audit-token pointer.
    token.val[5] = 9999;
    deathCallback(lookedUpHandle, contextFor(1, 37 << 8, NO));
    drainMainQueue();
    assert(delivered == 1 && actualStatus == (37 << 8) && !actualError);
    deathCallback(lookedUpHandle, contextFor(1, 0, NO));
    drainMainQueue();
    assert(delivered == 1);

    token.val[5] = 1234;
    assert(!LSBRunningBoardMonitorStart(&token, handler, &monitor));
    LSBRunningBoardMonitorCancel(monitor);
    deathCallback(lookedUpHandle, contextFor(1, 0, NO));
    drainMainQueue();
    assert(delivered == 1);

    assert(!LSBRunningBoardMonitorStart(&token, handler, &monitor));
    deathCallback(lookedUpHandle, [RBSProcessExitContext new]);
    drainMainQueue();
    assert(delivered == 2 && actualError == EPROTO);
    lookedUpHandle = nil;
    assert(LSBRunningBoardMonitorStart(&token, handler, &monitor) == ESRCH && !monitor);
    deathCallback = nil;
}

int main(void) {
    @autoreleasepool {
        alarm(10);
        testStatusValidation();
        testSubscriptionLifecycle();
        puts("RunningBoard monitor: voluntary/signal raw status, empty-context rejection, audited identity, cancellation, and one-shot delivery passed");
    }
    return 0;
}
