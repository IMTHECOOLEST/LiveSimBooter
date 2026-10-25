#pragma once

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <sys/types.h>

@interface RBSProcessExitStatus : NSObject
@property (nonatomic, readonly, getter=isValid) BOOL valid;
@end

@interface RBSProcessExitContext : NSObject
@property (nonatomic, readonly) RBSProcessExitStatus *status;
@property (nonatomic, readonly) NSDate *timestamp;
@property (nonatomic, readonly) NSUInteger type;
@property (nonatomic, readonly) int legacyCode;
@end

@interface RBSProcessHandle : NSObject
+ (instancetype)handleForAuditToken:(audit_token_t)token error:(NSError **)error;
@property (nonatomic, readonly) pid_t pid;
- (void)monitorForDeath:(void (^)(RBSProcessHandle *handle, RBSProcessExitContext *context))handler;
@end
