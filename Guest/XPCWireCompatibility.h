#pragma once

#include <mach/mach.h>
#include <stddef.h>

// Bracket only the synchronous broker routine. The state belongs to the calling thread and owns no Mach rights.
int LSBXPCWireBegin(mach_port_t broker);
void LSBXPCWireEnd(void);

#if defined(LSB_XPC_WIRE_TESTING)
typedef struct { unsigned sentInline, sentOOL, receivedInline, receivedOOL, receiveRetries; } LSBXPCWireStatistics;
LSBXPCWireStatistics LSBXPCWireTestStatistics(void);
int LSBXPCWireTestTranslate(mach_msg_header_t *message, size_t capacity, unsigned from, unsigned to);
#endif
