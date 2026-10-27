#include "ProcessIdentity.h"
#ifdef LSB_GUEST_RUNTIME
#include "PeerProcessCompatibility.h"
#endif
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
static char nativeNotifydPath[PATH_MAX];
static char nativeBaseBoardPath[PATH_MAX];
static char nativeRunningBoardServicesPath[PATH_MAX];
static char nativeFrontBoardPath[PATH_MAX];
static char nativeUIKitCorePath[PATH_MAX];
static char nativeFrontBoardServicesPath[PATH_MAX];
static char nativeBackBoardServicesPath[PATH_MAX];
static char nativeExtensionFoundationPath[PATH_MAX];
static char nativeCoreServicesPath[PATH_MAX];
static const char baseBoardInstallPath[] = "/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard";
static const char runningBoardServicesInstallPath[] =
    "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices";
static const char frontBoardInstallPath[] = "/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard";
static const char uiKitCoreInstallPath[] = "/System/Library/PrivateFrameworks/UIKitCore.framework/UIKitCore";
static const char frontBoardServicesInstallPath[] = "/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices";
static const char backBoardServicesInstallPath[] = "/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices";
static const char extensionFoundationInstallPath[] = "/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation";
static const char coreServicesInstallPath[] = "/System/Library/Frameworks/CoreServices.framework/CoreServices";

// 23A343 CoreServices compares getpid with a genuine audit-token PID in these two bundle-record lookup paths.
// Reverse-engineered from +[LSBundleRecord _bundleRecordForAuditToken:checkNSBundleMainBundle:error:] and
// _LSCopyBundleURLForAuditToken. Keep all other CoreServices callers on guest identity; see docs/springboard-services.md.
enum { CoreServicesRecordSelfReturn = 0x6f294, CoreServicesBundleSelfReturn = 0xd0524 };
static const unsigned coreServicesSelfReturns[] = {CoreServicesRecordSelfReturn, CoreServicesBundleSelfReturn};
static const unsigned char coreServicesUUID[16] = {0x73, 0xf4, 0x11, 0xd9, 0x84, 0xa9, 0x33, 0xfe,
                                                 0x84, 0xc9, 0x8c, 0xd5, 0x7c, 0xa6, 0x33, 0xdc};
static _Atomic(uintptr_t) coreServicesSelfImage;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Early identity caller checks must not require runtime initialization");

static bool pinnedCoreServicesSelfImage(const struct mach_header_64 *header, intptr_t slide) {
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->cpusubtype != CPU_SUBTYPE_ARM64_ALL ||
        header->filetype != MH_DYLIB || !header->ncmds || header->ncmds > 2048 || header->sizeofcmds > 1048576) return false;
    const unsigned char *command = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    uintptr_t base = (uintptr_t)header;
    bool uuid = false, text = false, instructions = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return false;
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining) return false;
        if (load->cmd == LC_UUID) {
            if (uuid || load->cmdsize != sizeof(struct uuid_command) ||
                memcmp(((const struct uuid_command *)load)->uuid, coreServicesUUID, sizeof(coreServicesUUID))) return false;
            uuid = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            if (load->cmdsize < sizeof(struct segment_command_64)) return false;
            const struct segment_command_64 *segment = (const struct segment_command_64 *)load;
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname))) {
                uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
                bool valid = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
                uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
                if (text || !valid || begin != base || segment->vmsize > UINTPTR_MAX - base ||
                    segment->filesize > segment->vmsize || segment->filesize < sizeof(*header) + header->sizeofcmds ||
                    (segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE) ||
                    segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(struct section_64)) return false;
                text = true;
                const struct section_64 *sections = (const struct section_64 *)(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j) {
                    const struct section_64 *section = &sections[j];
                    if (strncmp(section->sectname, "__text", sizeof(section->sectname))) continue;
                    if (instructions || strncmp(section->segname, "__TEXT", sizeof(section->segname)) ||
                        section->addr < segment->vmaddr) return false;
                    uint64_t relative = section->addr - segment->vmaddr;
                    if (relative > segment->filesize || section->size > segment->filesize - relative) return false;
                    for (unsigned k = 0; k < sizeof(coreServicesSelfReturns) / sizeof(*coreServicesSelfReturns); ++k) {
                        uint64_t start = coreServicesSelfReturns[k] - 16, end = coreServicesSelfReturns[k] + 8;
                        if (start < relative || end > relative + section->size) return false;
                    }
                    instructions = true;
                }
            }
        }
        command += load->cmdsize;
        remaining -= load->cmdsize;
    }
    if (!uuid || !text || !instructions || remaining) return false;
    // These windows, including BL getpid, are byte-identical in the stock image and the rebuilt development cache.
    static const uint32_t expected[][6] = {
        {0xaa1303e0, 0x94018282, 0xaa0003f5, 0x94044839, 0x6b0002bf, 0x54000961},
        {0x97ff3e44, 0x940554ca, 0x370002e0, 0x9402c395, 0xaa0003f4, 0xb81dc3bf}
    };
    for (unsigned i = 0; i < sizeof(coreServicesSelfReturns) / sizeof(*coreServicesSelfReturns); ++i)
        if (memcmp((const unsigned char *)header + coreServicesSelfReturns[i] - 16, expected[i], sizeof(expected[i]))) return false;
    return true;
}

static void configureNativeImagePath(char path[PATH_MAX], const char *suffix) {
    path[0] = '\0';
    const char *root = getenv("DYLD_ROOT_PATH");
    if (!root || root[0] != '/') return;
    size_t length = strnlen(root, PATH_MAX), suffixSize = strlen(suffix) + 1;
    if (length < 2 || suffixSize > PATH_MAX || length > PATH_MAX - suffixSize || root[length - 1] == '/' || strchr(root, ':')) return;
    // Launchd configuration supplies a canonical host path. Reject aliases and root lists without doing filesystem work in dyld callbacks.
    for (const char *component = root + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t size = end ? (size_t)(end - component) : strlen(component);
        if (!size || (size == 1 && component[0] == '.') || (size == 2 && component[0] == '.' && component[1] == '.')) return;
        component += size + (end != NULL);
    }
    memcpy(path, root, length);
    memcpy(path + length, suffix, suffixSize);
}

static void configureNativeNotifydIdentity(void) {
    nativeNotifydPath[0] = '\0';
    const char *enabled = getenv("LSB_SIMULATE_NOTIFICATION_SHM");
    if (enabled && !strcmp(enabled, "1")) configureNativeImagePath(nativeNotifydPath, "/usr/sbin/notifyd");
}

typedef struct { _Atomic(uintptr_t) image, begin, end; } NativeIdentityRange;
static NativeIdentityRange nativeIdentityRanges[16];
static _Atomic(unsigned) nativeIdentitySequence;

static void forgetNativeIdentityImage(const struct mach_header *image) {
    uintptr_t expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&coreServicesSelfImage, &expected, 0, memory_order_acq_rel, memory_order_acquire);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < 16; ++i) if (atomic_load_explicit(&nativeIdentityRanges[i].image, memory_order_relaxed) == (uintptr_t)image)
        atomic_store_explicit(&nativeIdentityRanges[i].image, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_release);
}

static void __attribute__((unused)) rememberNativeIdentityImage(const struct mach_header *image, intptr_t slide, const char *path) {
    const char *leaf = path ? strrchr(path, '/') : NULL;
    if (!image || image->magic != MH_MAGIC_64 || !leaf) return;
    if (!strcmp(path, coreServicesInstallPath) || (nativeCoreServicesPath[0] && !strcmp(path, nativeCoreServicesPath))) {
        forgetNativeIdentityImage(image);
        if (pinnedCoreServicesSelfImage((const struct mach_header_64 *)image, slide))
            atomic_store_explicit(&coreServicesSelfImage, (uintptr_t)image, memory_order_release);
        return;
    }
    bool notifyd = nativeNotifydPath[0] && !strcmp(path, nativeNotifydPath);
    // BaseBoard's versioned PID, sandbox and task-handle APIs compare getpid() with genuine kernel audit identities.
    bool baseBoard = !strcmp(path, baseBoardInstallPath) || (nativeBaseBoardPath[0] && !strcmp(path, nativeBaseBoardPath));
    // RunningBoardServices queries the caller's native sandbox before connecting and exchanges kernel process identities.
    bool runningBoard = !strcmp(path, runningBoardServicesInstallPath) ||
        (nativeRunningBoardServicesPath[0] && !strcmp(path, nativeRunningBoardServicesPath));
    // FrontBoard keys its workspace/process dictionaries with RBS native handles and permits repeated registration only for native self.
    bool frontBoard = !strcmp(path, frontBoardInstallPath) || (nativeFrontBoardPath[0] && !strcmp(path, nativeFrontBoardPath));
    // iOS 26 23A343 UIKitCore +0x115ea68 feeds getpid directly into an RBSTarget visibility assertion; its background assertions do likewise.
    bool uiKitCore = !strcmp(path, uiKitCoreInstallPath) || (nativeUIKitCorePath[0] && !strcmp(path, nativeUIKitCorePath));
    // Matching FrontBoardServices +0x54d74 compares native audit PID with getpid; +0x71900 publishes the scene host PID.
    // See docs/springboard-services.md and the pinned disassembly analysis in tmp/app-launch-fix/routine712-runningboard-pid-source.md.
    bool frontBoardServices = !strcmp(path, frontBoardServicesInstallPath) ||
        (nativeFrontBoardServicesPath[0] && !strcmp(path, nativeFrontBoardServicesPath));
    // iOS 26 23A343 BackBoardServices +0x673b4 registers the HID client's PID; +0xcc58 and +0x15bbc construct HID dispatch targets.
    // Those identities, and the delivery-policy self comparison at +0x183f0, must match the kernel audit PID used by the event client.
    bool backBoardServices = !strcmp(path, backBoardServicesInstallPath) ||
        (nativeBackBoardServicesPath[0] && !strcmp(path, nativeBackBoardServicesPath));
    // 23A343 ExtensionFoundation +0x5c658 feeds getpid into the RBS host identifier for _EXHostConfiguration.rbsProcessIdentity.
    // Its extension identity must name the same native process that RunningBoard resolves from audit tokens and task handles.
    bool extensionFoundation = !strcmp(path, extensionFoundationInstallPath) ||
        (nativeExtensionFoundationPath[0] && !strcmp(path, nativeExtensionFoundationPath));
    if (strcmp(leaf + 1, "libxpc.dylib") && !notifyd && !baseBoard && !runningBoard && !frontBoard &&
        !uiKitCore && !frontBoardServices && !backBoardServices && !extensionFoundation) return;
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
    uintptr_t coreServices = atomic_load_explicit(&coreServicesSelfImage, memory_order_acquire);
    if (coreServices && caller >= coreServices &&
        (caller - coreServices == CoreServicesRecordSelfReturn || caller - coreServices == CoreServicesBundleSelfReturn)) return true;
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
    configureNativeNotifydIdentity();
    configureNativeImagePath(nativeBaseBoardPath, baseBoardInstallPath);
    configureNativeImagePath(nativeRunningBoardServicesPath, runningBoardServicesInstallPath);
    configureNativeImagePath(nativeFrontBoardPath, frontBoardInstallPath);
    configureNativeImagePath(nativeUIKitCorePath, uiKitCoreInstallPath);
    configureNativeImagePath(nativeFrontBoardServicesPath, frontBoardServicesInstallPath);
    configureNativeImagePath(nativeBackBoardServicesPath, backBoardServicesInstallPath);
    configureNativeImagePath(nativeExtensionFoundationPath, extensionFoundationInstallPath);
    configureNativeImagePath(nativeCoreServicesPath, coreServicesInstallPath);
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
#ifdef LSB_GUEST_RUNTIME
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    kern_return_t childResult;
    if (LSBLaunchdProcessTaskName(caller, target, pid, task, &childResult)) return childResult;
#endif
    if (target == mach_task_self() && pid > 0 && pid == LSBGuestPID()) pid = LSBNativePID();
    kern_return_t result = task_name_for_pid(target, pid, task);
#ifdef LSB_GUEST_RUNTIME
    return LSBPeerProcessTaskName(caller, target, pid, task, result);
#else
    return result;
#endif
}

pid_t LSBGuestParentPID(void) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    pid_t current = ledger ? LSBChildStatusGetParentPID(ledger) : -1;
    if (current >= 0) return current;
    pid_t pid = atomic_load_explicit(&parentPID, memory_order_acquire);
    return atomic_load_explicit(&parentKnown, memory_order_acquire) ? pid : getppid();
}

#ifndef LSB_PROCESS_IDENTITY_TESTING
// libxpc and the scoped UI/process frameworks use kernel identities. Opted-in notifyd publishes its native PID to sibling PROC watches.
// Only direct calls from those images retain native identity; other guest code sees the stable logical PID.
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBGetPID, (const void *)&getpid},
    {(const void *)&LSBTaskNameForPID, (const void *)&task_name_for_pid},
    {(const void *)&LSBGuestParentPID, (const void *)&getppid}
};
#endif
