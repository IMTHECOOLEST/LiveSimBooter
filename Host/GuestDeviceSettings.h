#pragma once
#import <Foundation/Foundation.h>
#import "DeviceType.h"

FOUNDATION_EXPORT NSNotificationName const LSBGuestDeviceSettingsDidChangeNotification;

// Main-thread UI state. Selection is a draft until Apply; restarting uses the broker's owned-session drain.
@interface LSBGuestDeviceSettings : NSObject
@property (class, nonatomic, readonly) LSBGuestDeviceSettings *sharedController;
@property (nonatomic, readonly, copy) NSArray<LSBGuestDeviceType *> *deviceTypes;
@property (nonatomic, readonly, copy) NSString *currentIdentifier, *statusText;
@property (nonatomic, readonly) BOOL pending, canApply, outcomeUnknown;
- (void)refresh;
- (void)applyDeviceIdentifier:(NSString *)identifier;
@end
