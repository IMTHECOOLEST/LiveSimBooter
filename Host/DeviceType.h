#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LSBGuestDeviceType : NSObject
@property (nonatomic, copy, readonly) NSString *identifier;
@property (nonatomic, copy, readonly) NSString *displayName;
@property (nonatomic, copy, readonly) NSString *detailText;
@property (nonatomic, copy, readonly) NSString *modelIdentifier;
@property (nonatomic, copy, readonly) NSString *productClass;
@property (nonatomic, readonly) NSUInteger width, height, scale, pitch;
@property (nonatomic, readonly) BOOL experimental;
- (NSURL *)capabilitiesURLInRuntimeRoot:(NSURL *)root;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

NSArray<LSBGuestDeviceType *> *LSBGuestDeviceTypes(void);
LSBGuestDeviceType * _Nullable LSBGuestDeviceTypeForIdentifier(NSString * _Nullable identifier);
LSBGuestDeviceType *LSBDefaultGuestDeviceType(void);

// Validate an original capabilities plist against the selected catalog entry. No fallback, mutation, or global selected state.
// Bounds the input to 2 MiB and returns EINVAL for malformed/mismatched input. Caller errno is preserved.
int LSBValidateGuestDeviceCapabilities(NSData * _Nullable data, LSBGuestDeviceType * _Nullable device);

NS_ASSUME_NONNULL_END
