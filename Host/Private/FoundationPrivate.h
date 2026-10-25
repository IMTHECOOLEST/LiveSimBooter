#pragma once

#import <Foundation/Foundation.h>
#import "PlugInKitPrivate.h"

NS_ASSUME_NONNULL_BEGIN

// NSExtension performs registration/discovery; PKHost performs launch without an Objective-C child context handshake.
@interface NSExtension : NSObject
+ (nullable instancetype)extensionWithIdentifier:(NSString *)identifier error:(NSError * _Nullable * _Nullable)error;
- (nullable PKHostPlugIn *)_plugIn;
@end

NS_ASSUME_NONNULL_END
