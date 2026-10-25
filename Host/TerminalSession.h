#pragma once

#import <Foundation/Foundation.h>
#include "SpawnTransport.h"

// Native host only. The guest endpoint uses the pure-C FakePTY shim and ordinary inherited file descriptors.
@interface LSBTerminalSession : NSObject
@property (nonatomic, copy) void (^outputHandler)(NSData *data);
@property (nonatomic, copy) int (^signalHandler)(pid_t foreground, int signalNumber);
@property (nonatomic, readonly) NSString *statePath;
- (instancetype)initWithDirectory:(NSString *)directory error:(int *)error;
- (int)attachToMessage:(LSBReceivedMessage *)message;
- (void)start;
- (int)sendInput:(NSData *)data;
- (void)resizeRows:(unsigned short)rows columns:(unsigned short)columns;
- (void)close;
@end

// Main-thread UI access; nil for a launch without --terminal.
LSBTerminalSession *LSBHostTerminal(void);
