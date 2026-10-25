#ifndef LSB_CREATION_MASK_H
#define LSB_CREATION_MASK_H

#include <stdint.h>

// Returns an errno value and preserves errno. Older kernels require a single-threaded, signal-blocked snapshot.
int LSBReadCreationMask(uint32_t *mask);

#endif
