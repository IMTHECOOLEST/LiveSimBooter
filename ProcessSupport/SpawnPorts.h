#ifndef LSB_SPAWN_PORTS_H
#define LSB_SPAWN_PORTS_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>

#define LSB_MAX_REGISTERED_PORTS 3

typedef struct {
    bool registeredPresent;
    uint32_t count;
    mach_port_t registered[LSB_MAX_REGISTERED_PORTS];
    bool bootstrapPresent;
    mach_port_t bootstrapPort;
} LSBSpawnPorts;

// Absent registered ports mean inherit at the guest snapshot stage; present/count=0 explicitly clears them.
// Absent bootstrap means inherit before snapshot; present/null explicitly clears TASK_BOOTSTRAP_PORT. Other task ports are unsupported.
// Initialize to zero. Transport inputs borrow names; decoded results own one send reference per nonnull slot, including duplicates.
// Only dispose owned copies. Cleanup includes all slots so a partially decoded result is safe and repeated disposal is harmless.
static inline void LSBSpawnPortsDispose(LSBSpawnPorts *ports) {
    if (!ports) return;
    for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) {
        if (MACH_PORT_VALID(ports->registered[i])) mach_port_deallocate(mach_task_self(), ports->registered[i]);
        ports->registered[i] = MACH_PORT_NULL;
    }
    if (MACH_PORT_VALID(ports->bootstrapPort)) mach_port_deallocate(mach_task_self(), ports->bootstrapPort);
    ports->bootstrapPort = MACH_PORT_NULL;
    ports->bootstrapPresent = false;
    ports->registeredPresent = false;
    ports->count = 0;
}

#endif
