#ifndef LSB_SPAWN_ATTRIBUTES_H
#define LSB_SPAWN_ATTRIBUTES_H

#include "../ProcessSupport/SpawnPorts.h"

#include <spawn.h>
#include <stdint.h>

typedef struct {
    LSBSpawnPorts ports;
    uint32_t processType;
    uint32_t darwinRole;
} LSBAdditionalSpawnAttributes;

// Decode the pinned iOS 26 libkernel layout. Ports remain borrowed; this function acquires no Mach rights.
// The caller must separately apply or reject processType/darwinRole; decoding them does not apply scheduling policy.
// Standard POSIX flags, signal sets and process group remain the caller's responsibility.
int LSBReadAdditionalSpawnAttributes(const posix_spawnattr_t *attributes, LSBAdditionalSpawnAttributes *result);

#endif
