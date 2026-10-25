#include "NativeMetalControl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { ControlMagic = 0x4c53424d, ReadAttempts = 8, ReservedWords = (LSB_NATIVE_METAL_CONTROL_SIZE - 64) / 4 };
struct LSBNativeMetalControl {
    _Atomic(uint32_t) magic, version, size, reservedHeader;
    _Atomic(uint64_t) sequence;
    _Atomic(uint32_t) authorized, reservedFlags;
    _Atomic(uint32_t) audit[8];
    _Atomic(uint32_t) reserved[ReservedWords];
};
_Static_assert(sizeof(LSBNativeMetalControl) == LSB_NATIVE_METAL_CONTROL_SIZE, "Control ABI must occupy exactly one 4 KiB record");
_Static_assert(_Alignof(LSBNativeMetalControl) == 8 && offsetof(LSBNativeMetalControl, sequence) == 16 &&
               offsetof(LSBNativeMetalControl, audit) == 32 && offsetof(LSBNativeMetalControl, reserved) == 64, "Control ABI layout changed");
_Static_assert(sizeof(audit_token_t) == 32 && ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "Authorization requires lock-free, process-shared atomics");

static int alignedControl(const LSBNativeMetalControl *control, size_t size) {
    return control && size == LSB_NATIVE_METAL_CONTROL_SIZE && !((uintptr_t)control % _Alignof(LSBNativeMetalControl));
}

int LSBNativeMetalControlInitialize(LSBNativeMetalControl *control, size_t size) {
    if (!alignedControl(control, size)) return EINVAL;
    atomic_init(&control->magic, ControlMagic);
    atomic_init(&control->version, LSB_NATIVE_METAL_CONTROL_VERSION);
    atomic_init(&control->size, LSB_NATIVE_METAL_CONTROL_SIZE);
    atomic_init(&control->reservedHeader, 0);
    atomic_init(&control->sequence, 0);
    atomic_init(&control->authorized, 0);
    atomic_init(&control->reservedFlags, 0);
    for (unsigned i = 0; i < 8; ++i) atomic_init(&control->audit[i], 0);
    for (unsigned i = 0; i < ReservedWords; ++i) atomic_init(&control->reserved[i], 0);
    return 0;
}

int LSBNativeMetalControlValidate(const LSBNativeMetalControl *control, size_t size) {
    if (!alignedControl(control, size)) return EINVAL;
    if (atomic_load_explicit(&control->magic, memory_order_relaxed) != ControlMagic ||
        atomic_load_explicit(&control->version, memory_order_relaxed) != LSB_NATIVE_METAL_CONTROL_VERSION ||
        atomic_load_explicit(&control->size, memory_order_relaxed) != LSB_NATIVE_METAL_CONTROL_SIZE ||
        atomic_load_explicit(&control->reservedHeader, memory_order_relaxed) ||
        atomic_load_explicit(&control->reservedFlags, memory_order_relaxed)) return EPROTO;
    for (unsigned i = 0; i < ReservedWords; ++i)
        if (atomic_load_explicit(&control->reserved[i], memory_order_relaxed)) return EPROTO;
    return 0;
}

int LSBNativeMetalControlPublish(LSBNativeMetalControl *control, const audit_token_t *audit) {
    int error = LSBNativeMetalControlValidate(control, LSB_NATIVE_METAL_CONTROL_SIZE);
    if (error) return error;
    if (audit && (!audit->val[5] || audit->val[5] > INT_MAX)) return EINVAL;
    uint64_t sequence = atomic_load(&control->sequence);
    if (sequence == UINT64_MAX) return EOVERFLOW;
    if ((sequence & 1) || !atomic_compare_exchange_strong(&control->sequence, &sequence, sequence + 1)) return EBUSY;
    // SC operations give all sequence and payload accesses a single order. Atomic payload words avoid C11 data races even on rejected reads.
    bool exhausted = sequence == UINT64_MAX - 1;
    atomic_store(&control->authorized, audit && !exhausted ? 1u : 0u);
    for (unsigned i = 0; i < 8; ++i) atomic_store(&control->audit[i], audit && !exhausted ? audit->val[i] : 0);
    if (exhausted) return EOVERFLOW; // Leave sequence permanently odd/MAX; no wraparound can make an old observation appear current.
    atomic_store(&control->sequence, sequence + 2);
    return 0;
}

int LSBNativeMetalControlRead(const LSBNativeMetalControl *control, LSBNativeMetalAuthorization *authorization) {
    if (!authorization) return EINVAL;
    memset(authorization, 0, sizeof(*authorization));
    int error = LSBNativeMetalControlValidate(control, LSB_NATIVE_METAL_CONTROL_SIZE);
    if (error) return error;
    for (unsigned attempt = 0; attempt < ReadAttempts; ++attempt) {
        uint64_t sequence = atomic_load(&control->sequence);
        if (sequence == UINT64_MAX) return EOVERFLOW;
        if (sequence & 1) continue;
        uint32_t authorized = atomic_load(&control->authorized);
        audit_token_t audit = {{0}};
        for (unsigned i = 0; i < 8; ++i) audit.val[i] = atomic_load(&control->audit[i]);
        if (atomic_load(&control->sequence) != sequence) continue;
        if (authorized > 1 || (authorized && (!audit.val[5] || audit.val[5] > INT_MAX))) return EPROTO;
        if (!authorized) for (unsigned i = 0; i < 8; ++i) if (audit.val[i]) return EPROTO;
        authorization->generation = sequence / 2;
        authorization->authorized = authorized != 0;
        authorization->audit = audit;
        return 0;
    }
    return EAGAIN;
}

int LSBNativeMetalControlCreate(const char *directory, LSBNativeMetalControl **writer, int *readOnlyFD) {
    if (writer) *writer = NULL;
    if (readOnlyFD) *readOnlyFD = -1;
    if (!writer || !readOnlyFD || !directory || directory[0] != '/') return EINVAL;
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/lsb-native-metal.XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return ENAMETOOLONG;
    int writable = mkstemp(path);
    if (writable < 0) return errno;
    int readonly = -1, error = 0;
    LSBNativeMetalControl *mapping = MAP_FAILED;
    struct stat original, imported;
    if (fcntl(writable, F_SETFD, FD_CLOEXEC) || fchmod(writable, 0600) || ftruncate(writable, LSB_NATIVE_METAL_CONTROL_SIZE) ||
        fstat(writable, &original)) { error = errno; goto done; }
    readonly = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (readonly < 0) { error = errno; goto done; }
    if (fstat(readonly, &imported)) { error = errno; goto done; }
    if (original.st_dev != imported.st_dev || original.st_ino != imported.st_ino) { error = ESTALE; goto done; }
    if (unlink(path)) { error = errno; goto done; }
    path[0] = '\0';
    mapping = mmap(NULL, LSB_NATIVE_METAL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, writable, 0);
    if (mapping == MAP_FAILED) { error = errno; goto done; }
    error = LSBNativeMetalControlInitialize(mapping, LSB_NATIVE_METAL_CONTROL_SIZE);
done:
    if (path[0]) unlink(path);
    close(writable);
    if (error) {
        if (mapping != MAP_FAILED) munmap(mapping, LSB_NATIVE_METAL_CONTROL_SIZE);
        if (readonly >= 0) close(readonly);
    } else { *writer = mapping; *readOnlyFD = readonly; }
    return error;
}

int LSBNativeMetalControlImport(int readOnlyFD, const LSBNativeMetalControl **reader) {
    if (!reader) return EINVAL;
    *reader = NULL;
    int flags = fcntl(readOnlyFD, F_GETFL);
    if (flags < 0) return errno;
    if ((flags & O_ACCMODE) != O_RDONLY) return EACCES;
    struct stat info;
    if (fstat(readOnlyFD, &info)) return errno;
    if (!S_ISREG(info.st_mode) || info.st_size != LSB_NATIVE_METAL_CONTROL_SIZE) return EPROTO;
    const LSBNativeMetalControl *mapping = mmap(NULL, LSB_NATIVE_METAL_CONTROL_SIZE, PROT_READ, MAP_SHARED, readOnlyFD, 0);
    if (mapping == MAP_FAILED) return errno;
    int error = LSBNativeMetalControlValidate(mapping, LSB_NATIVE_METAL_CONTROL_SIZE);
    if (error) munmap((void *)mapping, LSB_NATIVE_METAL_CONTROL_SIZE);
    else *reader = mapping;
    return error;
}

void LSBNativeMetalControlUnmap(const LSBNativeMetalControl *control) {
    if (control) munmap((void *)control, LSB_NATIVE_METAL_CONTROL_SIZE);
}
