#ifndef LSB_WORKQUEUE_BOOTSTRAP_H
#define LSB_WORKQUEUE_BOOTSTRAP_H

#include <stdint.h>

// Captured in native code before cache detachment. The feature getter does not initialize the native workqueue.
#define LSB_HOST_WORKQUEUE_START_ENV "LSB_HOST_WORKQUEUE_START"
#define LSB_HOST_WORKQUEUE_FEATURES_ENV "LSB_HOST_WORKQUEUE_FEATURES"
// Default on: reserve one inherited execution breakpoint until process exit; kernel worker reuse needs it on every entry.
// Exact value "1" enables; any other explicit value disables. Select "0" with LSB_RETAIN_HOST_CACHE=0 for legacy fault mode.
#define LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "LSB_WORKQUEUE_HARDWARE_BREAKPOINT"

// Called only by the validated duplicate libpthread registration adapter; zero disables the adapter.
void LSBWorkqueueRegister(uintptr_t simulatorStart);
int LSBWorkqueueSupported(void);
int LSBWorkqueueOpen(void);

#endif
