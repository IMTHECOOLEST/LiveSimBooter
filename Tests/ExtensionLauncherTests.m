#define NSExtension LSBMockExtension
#define PKHost LSBMockHost
#define PKHostPlugIn LSBMockHostPlugIn
#define PKExternalProviders LSBMockExternalProviders
#define PKRunningBoardProvider LSBMockRunningBoardProvider
#define PKProcessAssertionProxy LSBMockProcessAssertionProxy
#define xpc_init_services LSBMockXPCInitServices
#import "../Host/ExtensionLauncher.m"
#include <assert.h>

static void (^testReady)(NSError *, NSArray<NSNumber *> *, NSArray<NSUUID *> *);
static unsigned testMode;
static NSUUID *testPluginUUID;
static NSUUID *lastInstanceUUID;
static PKHostPlugIn *testPlugin;
static PKProcessAssertionProxy *testAssertion;
static void (^testInvalidation)(void);
static unsigned assertionReleaseCount;

@implementation LSBMockExtension
+ (instancetype)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error {
    if ([identifier isEqualToString:@"missing"]) {
        *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:nil];
        return nil;
    }
    return [self new];
}
- (PKHostPlugIn *)_plugIn { return [PKHostPlugIn new]; }
@end
@implementation PKHostPlugIn
- (PKHostPlugIn *)createInstanceWithUUID:(NSUUID *)uuid {
    assert(uuid && ![uuid isEqual:lastInstanceUUID]);
    lastInstanceUUID = uuid;
    testPlugin = [PKHostPlugIn new];
    return testPlugin;
}
- (PKHost *)host { return [PKHost new]; }
- (PKExternalProviders *)external { return [PKExternalProviders new]; }
- (NSString *)sandboxProfile { return nil; }
- (NSUUID *)uuid { return testPluginUUID; }
@end
@implementation PKExternalProviders
- (PKRunningBoardProvider *)runningboard { return [PKRunningBoardProvider new]; }
@end
@implementation PKRunningBoardProvider
- (PKProcessAssertionProxy *)processAssertionWithPID:(pid_t)pid flags:(uint32_t)flags reason:(uint32_t)reason name:(NSString *)name {
    assert(pid == 4242 && flags == 7 && reason == 9 && [name isEqualToString:@"com.apple.extension.session"]);
    testAssertion = [PKProcessAssertionProxy new];
    return testAssertion;
}
@end
@implementation PKProcessAssertionProxy
- (void (^)(void))invalidationHandler { return testInvalidation; }
- (void)setInvalidationHandler:(void (^)(void))handler { testInvalidation = handler; }
- (BOOL)acquire { return testMode != 2; }
- (void)invalidate { ++assertionReleaseCount; if (testInvalidation) testInvalidation(); }
@end
@implementation PKHost
- (void)readyPlugIns:(NSArray<PKHostPlugIn *> *)plugins synchronously:(BOOL)synchronously environment:(NSDictionary *)environment
          languages:(NSArray<NSString *> *)languages persona:(id)persona sandbox:(NSString *)sandbox
              ready:(void (^)(NSError *, NSArray<NSNumber *> *, NSArray<NSUUID *> *))ready {
    assert(plugins.count == 1 && plugins[0] == testPlugin);
    assert(!synchronously && !environment && !languages.count && !persona && !sandbox);
    testReady = [ready copy];
    if (testMode == 1) {
        ready(nil, @[@4242], @[testPluginUUID]);
        @throw [NSException exceptionWithName:@"test" reason:@"submission failed" userInfo:nil];
    }
}
@end
void xpc_init_services(void) {}

static unsigned count;
static int lastError;
static pid_t lastPID;
static uint64_t lastRequest;
static int expectedContext;
static void Record(uint64_t requestID, pid_t pid, int error, void *context) {
    assert(pthread_main_np() && context == &expectedContext);
    ++count;
    lastError = error;
    lastPID = pid;
    lastRequest = requestID;
}
static void Drain(void) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
static void Ready(void) { testReady(nil, @[@4242], @[testPluginUUID]); }

int main(void) {
    @autoreleasepool {
        testPluginUUID = [NSUUID UUID];
        assert(LSBLaunchExtension(NULL, 1, Record, &expectedContext) == EINVAL);
        assert(LSBLaunchExtension("test", 1, NULL, &expectedContext) == EINVAL);
        assert(LSBLaunchExtension("missing", 1, Record, &expectedContext) == ENOENT);
        assert(LSBLaunchExtension("test", 1, Record, &expectedContext) == 0);
        assert(LSBLaunchExtension("test", 1, Record, &expectedContext) == EEXIST);
        assert(LSBExtensionProcessIdentifier(1) == 0);
        Ready();
        assert(count == 0);
        Drain();
        assert(count == 1 && lastRequest == 1 && lastPID == 4242 && lastError == 0);
        assert(LSBExtensionProcessIdentifier(1) == 4242);
        Ready();
        Drain();
        assert(count == 1);
        testInvalidation();
        testInvalidation();
        Drain();
        assert(count == 2 && lastError == ECANCELED && lastPID == 0);
        LSBReleaseExtension(1);
        assert(assertionReleaseCount == 1 && LSBExtensionProcessIdentifier(1) == 0);
        assert(LSBLaunchExtension("test", 2, Record, &expectedContext) == 0);
        Ready();
        LSBReleaseExtension(2);
        Drain();
        assert(count == 2);
        testMode = 1;
        assert(LSBLaunchExtension("test", 3, Record, &expectedContext) == EIO);
        Drain();
        assert(count == 2);
        testMode = 2;
        assert(LSBLaunchExtension("test", 4, Record, &expectedContext) == 0);
        Ready();
        Drain();
        assert(count == 3 && lastError == EACCES && LSBExtensionProcessIdentifier(4) == 0);
        LSBReleaseExtension(4);
        testMode = 0;
        assert(LSBLaunchExtension("test", 5, Record, &expectedContext) == 0);
        testReady(nil, nil, nil);
        Drain();
        assert(count == 4 && lastError == EPROTO && lastRequest == 5);
        LSBReleaseExtension(5);
        assert(LSBLaunchExtension("test", 6, Record, &expectedContext) == 0);
        testReady([NSError errorWithDomain:@"test" code:100 userInfo:@{
            NSUnderlyingErrorKey: [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOEXEC userInfo:nil]
        }], nil, nil);
        Drain();
        assert(count == 5 && lastError == ENOEXEC);
        LSBReleaseExtension(6);
        assert(LSBLaunchExtension("test", 7, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], @[[NSUUID UUID]]);
        Drain();
        assert(count == 6 && lastError == EPROTO);
        LSBReleaseExtension(7);
        assert(LSBLaunchExtension("test", 8, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], nil);
        Drain();
        assert(count == 7 && lastError == 0 && lastPID == 4242 && LSBExtensionProcessIdentifier(8) == 4242);
        LSBReleaseExtension(8);
        assert(LSBLaunchExtension("test", 9, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], @[]);
        Drain();
        assert(count == 8 && lastError == EPROTO);
        LSBReleaseExtension(9);
        puts("launcher lifecycle tests passed");
    }
    return 0;
}
