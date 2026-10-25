#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// Confirmed in the iOS 26.1 native Metal symbols; this probe calls it only on the native AGX device.
@protocol LSBMetalNativeDevice <MTLDevice>
- (void)allowLibrariesFromOtherPlatforms;
@end

// The original export allocates _MTLDeserializer and returns initWithDevice:generateResourceRefs:'s retained result.
typedef id (*LSBCreateMetalDeserializer)(id<MTLDevice> device, BOOL generateResourceRefs) NS_RETURNS_RETAINED;

__attribute__((visibility("default"))) int LSBMetalNativeProbe(const char *absoluteFolder);
