#import "GuestDeviceSettings.h"
#import "Broker.h"
#include <errno.h>
#include <string.h>

NSNotificationName const LSBGuestDeviceSettingsDidChangeNotification = @"LSBGuestDeviceSettingsDidChange";

@implementation LSBGuestDeviceSettings {
    id _sessionObserver;
}
+ (instancetype)sharedController {
    static LSBGuestDeviceSettings *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [self new]; });
    return controller;
}
- (instancetype)init {
    if ((self = [super init])) {
        _deviceTypes = LSBGuestDeviceTypes();
        _statusText = @"Apply changes the simulator display and restarts its session. App data is preserved.";
        __weak LSBGuestDeviceSettings *weakSelf = self;
        _sessionObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostSessionStateDidChangeNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf refresh]; }];
        [self refresh];
    }
    return self;
}
- (void)dealloc { if (_sessionObserver) [NSNotificationCenter.defaultCenter removeObserver:_sessionObserver]; }
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBGuestDeviceSettingsDidChangeNotification object:self];
}
- (void)refresh {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    _currentIdentifier = LSBHostCurrentDeviceType().identifier;
    _canApply = !_pending && !_outcomeUnknown && LSBHostCanApplyDeviceType();
    [self publish];
}
- (void)applyDeviceIdentifier:(NSString *)identifier {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || _outcomeUnknown) return;
    LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(identifier);
    if (!type || [identifier isEqualToString:LSBHostCurrentDeviceType().identifier]) return;
    _pending = YES;
    _canApply = NO;
    _statusText = @"Applying device type and restarting the simulator…";
    [self publish];
    LSBHostApplyDeviceType(type, ^(int error, BOOL restarted) {
        self->_pending = NO;
        if (error == ENOENT) self->_statusText = @"This device's original capabilities profile is missing. Stage the device profiles during setup.";
        else if (error == EBUSY) self->_statusText = @"The simulator is still starting or restarting. Wait, then try Apply again.";
        else if (error) self->_statusText = [NSString stringWithFormat:@"Device change failed: %s (%d). Reopen the host if the session stopped.",
                                            strerror(error), error];
        else self->_statusText = restarted ? @"Device applied. The simulator is starting with the new display." :
            @"Device selected. Tap Start on IOSurface to boot it.";
        [self refresh];
    });
}
@end
