#include "SpawnAttributes.h"

#include <errno.h>
#include <mach/machine.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Audited against the iOS 26 23A343 host support libsystem_kernel.dylib, including its init/destroy and setter routines.
enum { NativeAttributeSize = 0x100, NativePortActionsOffset = 0xc8,
       NativeImportanceWatchPortAction = 3, NativeRegisteredPortAction = 4, NativeMaxImportanceWatchPorts = 128 };
typedef struct {
    int32_t kind;
    uint32_t mask;
    mach_port_t port;
    int32_t behavior, flavor, which;
} NativePortAction;
typedef struct { int32_t allocated, count; NativePortAction actions[]; } NativePortActions;
_Static_assert(sizeof(void *) == 8, "Pinned spawn attributes require the 64-bit Darwin ABI");
_Static_assert(sizeof(NativePortAction) == 24, "Pinned port-action stride changed");
_Static_assert(offsetof(NativePortActions, actions) == 8, "Pinned port-action header changed");

static int readArchitecture(const unsigned char native[NativeAttributeSize]) {
    cpu_type_t cpus[4];
    cpu_subtype_t subtypes[4];
    memcpy(cpus, native + 0x10, sizeof(cpus));
    memcpy(subtypes, native + 0x70, sizeof(subtypes));
    for (size_t i = 0; i < 4; ++i) {
        if (!cpus[i] && subtypes[i] == CPU_SUBTYPE_MULTIPLE) continue;
        if (i == 0 && cpus[i] == CPU_TYPE_ARM64 &&
            (subtypes[i] == CPU_SUBTYPE_ARM64_ALL || subtypes[i] == CPU_SUBTYPE_MULTIPLE)) continue;
        // Stock launchd appends x86_64 even for BinaryOrderPreference="arm64". The loader must still find its first arm64 slice.
        if (i == 1 && cpus[0] == CPU_TYPE_ARM64 && cpus[i] == CPU_TYPE_X86_64 && subtypes[i] == CPU_SUBTYPE_MULTIPLE) continue;
        return ENOTSUP;
    }
    return 0;
}

static int readPorts(const unsigned char native[NativeAttributeSize], LSBSpawnPorts *ports) {
    const NativePortActions *actions;
    memcpy(&actions, native + NativePortActionsOffset, sizeof(actions));
    if (!actions) return 0;
    if (actions->count < 0 || actions->allocated < actions->count) return EINVAL;
    if (actions->count > NativeMaxImportanceWatchPorts + LSB_MAX_REGISTERED_PORTS + 1) return E2BIG;
    unsigned importanceCount = 0;
    for (int32_t i = 0; i < actions->count; ++i) {
        const NativePortAction *action = &actions->actions[i];
        if (action->mask || action->behavior || action->flavor) return EINVAL;
        if (action->kind == NativeImportanceWatchPortAction) {
            if (action->which) return EINVAL;
            if (++importanceCount > NativeMaxImportanceWatchPorts) return E2BIG;
            // launchd adds one importance-watch hint per MachService. These request kernel scheduling boosts,
            // which the extension broker cannot reproduce; they neither transfer service rights nor register ports.
            // The native setter writes kind=3, mask=behavior=flavor=which=0 and borrows the supplied port name.
            continue;
        } else if (action->kind == NativeRegisteredPortAction) {
            if (action->which) return EINVAL;
            if (ports->count == LSB_MAX_REGISTERED_PORTS) return E2BIG;
            ports->registeredPresent = true;
            ports->registered[ports->count++] = action->port;
        } else if (action->kind == 0 && action->which == TASK_BOOTSTRAP_PORT) {
            if (action->port == MACH_PORT_DEAD || ports->bootstrapPresent) return ENOTSUP;
            ports->bootstrapPresent = true;
            ports->bootstrapPort = action->port;
        } else return ENOTSUP;
    }
    return 0;
}

int LSBReadAdditionalSpawnAttributes(const posix_spawnattr_t *attributes, LSBAdditionalSpawnAttributes *result) {
    if (!result) return EINVAL;
    memset(result, 0, sizeof(*result));
    if (!attributes) return 0;
    if (!*attributes) return EINVAL;
    const unsigned char *native = (const unsigned char *)*attributes;
    int error = readArchitecture(native);
    if (error) return error;

    // Compare only initialized fields, excluding decoded fields and the standard POSIX fields. Gaps are uninitialized ABI padding.
    static const uint16_t unchanged[][2] = {
        {32, 36}, {40, 64}, {64, 66}, {68, 88}, {92, 107}, {108, 112}, {128, 156}, {160, 169}, {172, 177}, {180, 200}, {208, 256}
    };
    posix_spawnattr_t defaults;
    error = posix_spawnattr_init(&defaults);
    if (error) return error;
    for (size_t i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i) {
        size_t start = unchanged[i][0], length = unchanged[i][1] - start;
        if (memcmp(native + start, (const unsigned char *)defaults + start, length)) { error = ENOTSUP; break; }
    }
    posix_spawnattr_destroy(&defaults);
    if (error) return error;

    LSBAdditionalSpawnAttributes decoded = {0};
    error = readPorts(native, &decoded.ports);
    if (error) return error;
    memcpy(&decoded.processType, native + 0x24, sizeof(decoded.processType));
    memcpy(&decoded.darwinRole, native + 0x58, sizeof(decoded.darwinRole));
    *result = decoded;
    return 0;
}
