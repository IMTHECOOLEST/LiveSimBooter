#import <Foundation/Foundation.h>
#import "../Host/GuestDeviceSettings.h"
#import "../Host/Broker.h"
#include <assert.h>
#include <errno.h>

NSString *const LSBHostSessionStateDidChangeNotification = @"LSBHostSessionStateDidChangeTests";
static LSBGuestDeviceType *currentDevice, *requestedDevice;
static BOOL available;
static unsigned requestCount;
static int immediateError;
static void (^pendingCompletion)(int, BOOL);

@interface MemoryDeviceDefaults : NSUserDefaults
@property(nonatomic) NSMutableDictionary<NSString *, NSNumber *> *values;
@property(nonatomic) unsigned writes;
@end
@implementation MemoryDeviceDefaults
- (instancetype)init { if ((self = [super init])) _values = [NSMutableDictionary new]; return self; }
- (BOOL)boolForKey:(NSString *)key { return [self.values[key] boolValue]; }
- (void)setBool:(BOOL)value forKey:(NSString *)key { self.values[key] = value ? @YES : @NO; ++self.writes; }
@end

static LSBGuestDeviceSettings *newModel(void) {
    return [[LSBGuestDeviceSettings alloc] initWithDefaults:[MemoryDeviceDefaults new]];
}

LSBGuestDeviceType *LSBHostCurrentDeviceType(void) { return currentDevice; }
BOOL LSBHostCanApplyDeviceType(void) { return available; }
void LSBHostApplyDeviceType(LSBGuestDeviceType *type, void (^completion)(int, BOOL)) {
    assert(NSThread.isMainThread && type && completion && !pendingCompletion);
    ++requestCount;
    requestedDevice = type;
    if (immediateError) completion(immediateError, NO);
    else pendingCompletion = [completion copy];
}

static void spinUntil(BOOL (^condition)(void)) {
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!condition() && limit.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    assert(condition());
}

static void notifyBroker(void) {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostSessionStateDidChangeNotification object:nil];
}

static void complete(int error, BOOL restarted) {
    assert(pendingCompletion);
    void (^completion)(int, BOOL) = pendingCompletion;
    pendingCompletion = nil;
    if (!error) currentDevice = requestedDevice;
    completion(error, restarted);
}

static void testPendingAndBrokerReadiness(void) {
    currentDevice = LSBDefaultGuestDeviceType();
    available = YES;
    LSBGuestDeviceSettings *model = newModel();
    assert([model.currentIdentifier isEqual:currentDevice.identifier] && model.canApply && !model.pending && !model.outcomeUnknown);
    assert(model.deviceTypes.count == 7 && [model.deviceTypes isEqual:LSBGuestDeviceTypes()]);
    __block unsigned changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:model
        queue:nil usingBlock:^(NSNotification *notification) { (void)notification; assert(NSThread.isMainThread); ++changes; }];
    unsigned count = requestCount;
    [model applyDeviceIdentifier:@"unknown"];
    [model applyDeviceIdentifier:model.currentIdentifier];
    assert(requestCount == count && !model.pending && changes == 0);

    [model applyDeviceIdentifier:@"iphone-se-2"];
    assert(requestCount == count + 1 && [requestedDevice.identifier isEqual:@"iphone-se-2"]);
    assert(model.pending && !model.canApply && changes == 1);
    assert([model.currentIdentifier isEqual:@"iphone-15-pro-max"]); // No optimistic selection before the broker commits it.
    NSString *pendingStatus = [model.statusText copy];
    assert([pendingStatus containsString:@"Applying"]);
    [model applyDeviceIdentifier:@"ipad-9"];
    [model applyDeviceIdentifier:@"iphone-se-2"];
    assert(requestCount == count + 1);

    available = NO;
    notifyBroker();
    spinUntil(^BOOL { return changes >= 2; });
    assert(model.pending && !model.canApply && [model.statusText isEqual:pendingStatus]);
    available = YES;
    notifyBroker();
    spinUntil(^BOOL { return changes >= 3; });
    assert(model.pending && !model.canApply); // A readiness notification cannot clear an in-flight Apply.

    available = NO; // Completion acknowledges enqueue; the fresh session is still starting.
    complete(0, YES);
    assert(!model.pending && !model.canApply && [model.currentIdentifier isEqual:@"iphone-se-2"]);
    assert([model.statusText containsString:@"starting with the new display"]);
    available = YES;
    notifyBroker();
    spinUntil(^BOOL { return model.canApply; });
    assert(!model.pending && [model.currentIdentifier isEqual:@"iphone-se-2"]);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
}

static void testColdSelectionAndRefresh(void) {
    currentDevice = LSBDefaultGuestDeviceType();
    available = YES;
    LSBGuestDeviceSettings *model = newModel();
    [model applyDeviceIdentifier:@"iphone-11"];
    assert(model.pending && [model.currentIdentifier isEqual:@"iphone-15-pro-max"]);
    complete(0, NO); // A validated choice before Start changes preferences, without starting a session.
    assert([model.currentIdentifier isEqual:@"iphone-11"] && !model.pending && model.canApply);
    assert([model.statusText containsString:@"Tap Start"]);
    currentDevice = LSBGuestDeviceTypeForIdentifier(@"ipad-a16");
    notifyBroker(); // Broker returns configured preferences when no running jobs own a selected type.
    spinUntil(^BOOL { return [model.currentIdentifier isEqual:@"ipad-a16"]; });
    assert(!model.pending && model.canApply);
    available = NO;
    [model refresh];
    assert(!model.canApply);
    available = YES;
    notifyBroker();
    spinUntil(^BOOL { return model.canApply; });
}

static void testErrorsLeaveSelectionUnchanged(void) {
    currentDevice = LSBDefaultGuestDeviceType();
    available = YES;
    LSBGuestDeviceSettings *model = newModel();
    const struct { int error; const char *text; BOOL restarted; } cases[] = {
        {ENOENT, "profile is missing", NO}, {EBUSY, "still starting or restarting", NO},
        {EINVAL, "Device change failed", NO}, {EIO, "Reopen the host", YES}, {ETIMEDOUT, "Reopen the host", YES}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        unsigned before = requestCount;
        [model applyDeviceIdentifier:@"iphone-8"];
        assert(model.pending && requestCount == before + 1);
        available = NO;
        complete(cases[i].error, cases[i].restarted);
        assert(!model.pending && !model.canApply && [model.currentIdentifier isEqual:@"iphone-15-pro-max"]);
        assert([model.statusText containsString:@(cases[i].text)]);
        available = YES;
        notifyBroker();
        spinUntil(^BOOL { return model.canApply; });
        assert(!model.pending);
    }
    // The broker may reject synchronously after the UI's last readiness snapshot.
    immediateError = EBUSY;
    available = NO;
    unsigned before = requestCount;
    [model applyDeviceIdentifier:@"ipad-9"];
    assert(requestCount == before + 1 && !pendingCompletion && !model.pending && !model.canApply);
    assert([model.statusText containsString:@"still starting or restarting"]);
    immediateError = 0;
}

static void testFullWidthIsPersistentHostPresentationOnly(void) {
    currentDevice = LSBGuestDeviceTypeForIdentifier(@"iphone-se-2");
    available = YES;
    MemoryDeviceDefaults *defaults = [MemoryDeviceDefaults new];
    LSBGuestDeviceSettings *model = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    assert(!model.fullWidth && !defaults.writes);
    __block unsigned changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:model
        queue:nil usingBlock:^(NSNotification *notification) { assert(notification.object == model && NSThread.isMainThread); ++changes; }];
    unsigned count = requestCount;
    [model setFullWidth:NO];
    assert(!defaults.writes && !changes && requestCount == count);
    [model setFullWidth:YES];
    assert(model.fullWidth && defaults.writes == 1 && changes == 1 && requestCount == count);
    assert(defaults.values.count == 1 && [defaults.values[@"LSBSimulatorFullWidth"] isEqual:@YES]);
    [model setFullWidth:YES];
    assert(defaults.writes == 1 && changes == 1 && requestCount == count);
    LSBGuestDeviceSettings *reopened = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    assert(reopened.fullWidth && defaults.writes == 1);

    // Changing device eligibility never destroys the user's saved presentation choice.
    for (NSString *identifier in @[@"iphone-11", @"ipad-a16", @"iphone-se-2"]) {
        currentDevice = LSBGuestDeviceTypeForIdentifier(identifier);
        [model refresh];
        assert(model.fullWidth && [model.currentIdentifier isEqual:identifier] && defaults.writes == 1 && requestCount == count);
    }
    [model applyDeviceIdentifier:@"ipad-9"];
    assert(model.pending && requestCount == count + 1);
    NSString *pendingStatus = model.statusText;
    [model setFullWidth:NO];
    assert(!model.fullWidth && model.pending && !model.canApply && [model.statusText isEqual:pendingStatus]);
    assert(defaults.writes == 2 && requestCount == count + 1);
    complete(0, YES);
    assert(!model.pending && !model.fullWidth && requestCount == count + 1);
    reopened = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    assert(!reopened.fullWidth && defaults.writes == 2);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
}

static void testObserverDoesNotRetainController(void) {
    __weak LSBGuestDeviceSettings *weakModel;
    @autoreleasepool {
        LSBGuestDeviceSettings *model = newModel();
        weakModel = model;
        [model refresh];
    }
    assert(!weakModel);
    notifyBroker();
}

int main(void) {
    @autoreleasepool {
        testPendingAndBrokerReadiness();
        testColdSelectionAndRefresh();
        testErrorsLeaveSelectionUnchanged();
        testFullWidthIsPersistentHostPresentationOnly();
        testObserverDoesNotRetainController();
        assert(!pendingCompletion);
        assert(LSBGuestDeviceSettings.sharedController == LSBGuestDeviceSettings.sharedController);
        puts("Guest device-settings controller tests passed");
    }
    return 0;
}
