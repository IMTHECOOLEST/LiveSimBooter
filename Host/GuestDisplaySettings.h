#pragma once
#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSNotificationName const LSBGuestDisplaySettingsDidChangeNotification;
FOUNDATION_EXPORT NSArray<NSString *> *LSBPerformanceHUDNames(void);

// Main-thread UI state. All guest access happens on a private queue through the fixed display-settings helper.
@interface LSBGuestDisplaySettings : NSObject
@property (class, nonatomic, readonly) LSBGuestDisplaySettings *sharedController;
@property (nonatomic, readonly) BOOL pending, outcomeUnknown;
@property (nonatomic, readonly) NSInteger hudMode, touches; // -1 means unavailable; never an optimistic requested value.
@property (nonatomic, readonly, copy) NSString *statusText;
- (instancetype)initWithDataRoot:(NSURL *)dataRoot timeout:(NSTimeInterval)timeout;
- (void)refresh;
- (void)setHUDMode:(NSInteger)mode;
- (void)setVisualizeTouches:(BOOL)enabled;
@end
