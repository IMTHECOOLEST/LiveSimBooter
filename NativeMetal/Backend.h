#ifndef LSB_NATIVE_METAL_BACKEND_H
#define LSB_NATIVE_METAL_BACKEND_H
#include <mach/mach.h>

// Native-only backend loaded from the fixed extension bundle directory. The read-only authorization FD is borrowed.
typedef int (*LSBMetalServicePrepare)(const char *absoluteFolder, int controlFD, mach_port_t *registrationPort);
typedef void (*LSBMetalServiceRun)(void);
int LSBMetalNativeServicePrepare(const char *absoluteFolder, int controlFD, mach_port_t *registrationPort);
void LSBMetalNativeServiceRun(void);
#endif
