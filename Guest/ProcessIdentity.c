#include "ProcessIdentity.h"
#include "ChildWait.h"
#include <limits.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic(pid_t) guestPID, parentPID;
static _Atomic(bool) parentKnown;

typedef struct { _Atomic(uintptr_t) image, begin, end; } NativeIdentityRange;
static NativeIdentityRange nativeIdentityRanges[16];
static _Atomic(unsigned) nativeIdentitySequence;

static void forgetNativeIdentityImage(const struct mach_header *image) {
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < 16; ++i) if (atomic_load_explicit(&nativeIdentityRanges[i].image, memory_order_relaxed) == (uintptr_t)image)
        atomic_store_explicit(&nativeIdentityRanges[i].image, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_release);
}

static void __attribute__((unused)) rememberNativeIdentityImage(const struct mach_header *image, intptr_t slide, const char *path) {
    const char *leaf = path ? strrchr(path, '/') : NULL;
    if (!image || image->magic != MH_MAGIC_64 || !leaf || strcmp(leaf + 1, "libxpc.dylib")) return;
    const struct mach_header_64 *header = (const struct mach_header_64 *)image;
    const unsigned char *command = (const unsigned char *)(header + 1), *end = command + header->sizeofcmds;
    forgetNativeIdentityImage(image);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < header->ncmds && (size_t)(end - command) >= sizeof(struct load_command); ++i) {
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > (size_t)(end - command)) break;
        if (load->cmd == LC_SEGMENT_64 && load->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
            bool validSlide = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
            uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
            if (validSlide && segment->vmsize && segment->vmsize <= UINTPTR_MAX - begin && (segment->initprot & VM_PROT_EXECUTE)) {
                for (unsigned slot = 0; slot < 16; ++slot) {
                    NativeIdentityRange *range = &nativeIdentityRanges[slot];
                    if (atomic_load_explicit(&range->image, memory_order_relaxed)) continue;
                    atomic_store_explicit(&range->begin, begin, memory_order_relaxed);
                    atomic_store_explicit(&range->end, begin + segment->vmsize, memory_order_relaxed);
                    atomic_store_explicit(&range->image, (uintptr_t)image, memory_order_relaxed);
                    break;
                }
            }
        }
        command += load->cmdsize;
    }
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_release);
}

static bool callerNeedsNativeIdentity(uintptr_t caller) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        unsigned sequence = atomic_load_explicit(&nativeIdentitySequence, memory_order_acquire);
        if (sequence & 1) continue;
        bool found = false;
        for (unsigned i = 0; i < 16; ++i) {
            NativeIdentityRange *range = &nativeIdentityRanges[i];
            if (!atomic_load_explicit(&range->image, memory_order_relaxed)) continue;
            uintptr_t begin = atomic_load_explicit(&range->begin, memory_order_relaxed), end = atomic_load_explicit(&range->end, memory_order_relaxed);
            if (caller >= begin && caller < end) { found = true; break; }
        }
        if (sequence == atomic_load_explicit(&nativeIdentitySequence, memory_order_acquire)) return found;
    }
    return false;
}

#ifndef LSB_PROCESS_IDENTITY_TESTING
static void identityImageAdded(const struct mach_header *image, intptr_t slide) {
    Dl_info info;
    if (dladdr(image, &info)) rememberNativeIdentityImage(image, slide, info.dli_fname);
}
static void identityImageRemoved(const struct mach_header *image, intptr_t slide) {
    (void)slide;
    forgetNativeIdentityImage(image);
}
#endif

static pid_t environmentPID(const char *key) {
    const char *text = getenv(key);
    if (!text || !*text) return 0;
    unsigned long value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (unsigned)(*p - '0')) / 10u) return 0;
        value = value * 10 + (unsigned)(*p - '0');
    }
    return (pid_t)value;
}

__attribute__((constructor)) static void initializeIdentity(void) {
#ifndef LSB_PROCESS_IDENTITY_TESTING
    // Existing images are enumerated synchronously before publishing the guest PID. Early recursive calls retain native identity.
    _dyld_register_func_for_add_image(identityImageAdded);
    _dyld_register_func_for_remove_image(identityImageRemoved);
#endif
    atomic_store_explicit(&guestPID, environmentPID("LSB_GUEST_PID"), memory_order_release);
    pid_t parent = environmentPID("LSB_GUEST_PPID");
    const char *text = getenv("LSB_GUEST_PPID");
    atomic_store_explicit(&parentPID, parent, memory_order_release);
    atomic_store_explicit(&parentKnown, parent > 0 || (text && text[0] == '0' && !text[1]), memory_order_release);
}

pid_t LSBNativePID(void) { return getpid(); }

pid_t LSBGuestPID(void) {
    pid_t pid = atomic_load_explicit(&guestPID, memory_order_acquire);
    return pid ? pid : LSBNativePID();
}

__attribute__((noinline)) pid_t LSBGetPID(void) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    return callerNeedsNativeIdentity(caller) ? LSBNativePID() : LSBGuestPID();
}

kern_return_t LSBTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task) {
    if (target == mach_task_self() && pid > 0 && pid == LSBGuestPID()) pid = LSBNativePID();
    return task_name_for_pid(target, pid, task);
}

pid_t LSBGuestParentPID(void) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    pid_t current = ledger ? LSBChildStatusGetParentPID(ledger) : -1;
    if (current >= 0) return current;
    pid_t pid = atomic_load_explicit(&parentPID, memory_order_acquire);
    return atomic_load_explicit(&parentKnown, memory_order_acquire) ? pid : getppid();
}

#ifndef LSB_PROCESS_IDENTITY_TESTING
// libxpc compares getpid() against kernel audit tokens. Its direct calls retain native identity; guest code
// sees the stable logical PID. A call through another image or a tail call cannot be classified as libxpc here.
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBGetPID, (const void *)&getpid},
    {(const void *)&LSBTaskNameForPID, (const void *)&task_name_for_pid},
    {(const void *)&LSBGuestParentPID, (const void *)&getppid}
};
#endif
