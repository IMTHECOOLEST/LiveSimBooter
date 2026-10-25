#ifndef LSB_WORKQUEUE_BOOTSTRAP_H
#define LSB_WORKQUEUE_BOOTSTRAP_H

#include <stdint.h>

// Captured in native code before cache detachment. The feature getter does not initialize the native workqueue.
#define LSB_HOST_WORKQUEUE_START_ENV "LSB_HOST_WORKQUEUE_START"
#define LSB_HOST_WORKQUEUE_FEATURES_ENV "LSB_HOST_WORKQUEUE_FEATURES"

// Called only by the validated duplicate libpthread registration adapter; zero disables the adapter.
void LSBWorkqueueRegister(uintptr_t simulatorStart);
int LSBWorkqueueSupported(void);
int LSBWorkqueueOpen(void);

#endif
