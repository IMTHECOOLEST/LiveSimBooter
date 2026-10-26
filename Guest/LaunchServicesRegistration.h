#pragma once

#import <Foundation/Foundation.h>

// Matching 23A343 CoreServices implements registerApplication: by calling LSRegisterURL (not a fabricated install dictionary).
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@property(nonatomic, readonly) NSString *bundleIdentifier;
@property(nonatomic, readonly) NSURL *bundleURL;
@property(nonatomic, readonly, getter=isInstalled) BOOL installed;
@end
