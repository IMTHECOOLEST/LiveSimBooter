#pragma once

#import <Foundation/Foundation.h>
#include <mach/mach.h>

@class LSBRunningBoardMonitor;
typedef void (^LSBRunningBoardExitHandler)(int waitStatus, int error);

int LSBRunningBoardValidateIdentity(const audit_token_t *audit);
// Main-thread API. RunningBoard authenticates the full audited PID generation and permits monitoring hosted extensions.
// Completion is delivered on the main queue. An unavailable/empty exit context is an error, never an invented success.
int LSBRunningBoardMonitorStart(const audit_token_t *audit, LSBRunningBoardExitHandler handler, LSBRunningBoardMonitor **monitor);
void LSBRunningBoardMonitorCancel(LSBRunningBoardMonitor *monitor);
