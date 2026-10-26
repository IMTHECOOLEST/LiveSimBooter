#include "PeerProcessCompatibility.h"
#include "ProcessIdentity.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern int sandbox_check(pid_t pid, const char *operation, int type, ...);
extern const int SANDBOX_CHECK_NO_REPORT;
extern int proc_pidpath(int pid, void *buffer, uint32_t size);
// The simulator SDK omits mach/mach_vm.h; libSystem still exports the normal kernel copy API.
extern kern_return_t mach_vm_write(vm_map_t task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t size);

static const char runningBoardExecutable[] = "/usr/libexec/runningboardd";
static const char servicesInstallPath[] = "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices";
static bool peerInspectionEnabled;
static char peerRuntimeRoot[PATH_MAX];
static char servicesPhysicalPath[PATH_MAX];
typedef struct { _Atomic(uintptr_t) image, begin, end; } PeerCallerRange;
static PeerCallerRange peerCallerRanges[8];
static _Atomic(unsigned) peerCallerSequence;

static bool canonicalPath(const char *path) {
    if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) == PATH_MAX || strchr(path, ':')) return false;
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (end && !end[1]) return false;
        part += size + (end != NULL);
    }
    return true;
}

static void configurePeerScope(const char *enabled, const char *root, const char *executable) {
    peerInspectionEnabled = false;
    peerRuntimeRoot[0] = '\0';
    servicesPhysicalPath[0] = '\0';
    if (!enabled || strcmp(enabled, "1") || !canonicalPath(root) || !canonicalPath(executable)) return;
    size_t rootSize = strlen(root);
    if (rootSize + sizeof(servicesInstallPath) > sizeof(servicesPhysicalPath) ||
        strncmp(executable, root, rootSize) || strcmp(executable + rootSize, runningBoardExecutable)) return;
    memcpy(servicesPhysicalPath, root, rootSize);
    memcpy(servicesPhysicalPath + rootSize, servicesInstallPath, sizeof(servicesInstallPath));
    memcpy(peerRuntimeRoot, root, rootSize + 1);
    peerInspectionEnabled = true;
}

static void forgetPeerCaller(const struct mach_header *image) {
    atomic_fetch_add_explicit(&peerCallerSequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < 8; ++i) if (atomic_load_explicit(&peerCallerRanges[i].image, memory_order_relaxed) == (uintptr_t)image)
        atomic_store_explicit(&peerCallerRanges[i].image, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&peerCallerSequence, 1, memory_order_release);
}

static void rememberPeerCaller(const struct mach_header *image, intptr_t slide, const char *path) {
    if (!peerInspectionEnabled || !image || image->magic != MH_MAGIC_64 || !path ||
        (strcmp(path, servicesInstallPath) && strcmp(path, servicesPhysicalPath))) return;
    const struct mach_header_64 *header = (const struct mach_header_64 *)image;
    const unsigned char *command = (const unsigned char *)(header + 1), *end = command + header->sizeofcmds;
    forgetPeerCaller(image);
    atomic_fetch_add_explicit(&peerCallerSequence, 1, memory_order_acq_rel);
    for (uint32_t i = 0; i < header->ncmds && (size_t)(end - command) >= sizeof(struct load_command); ++i) {
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > (size_t)(end - command)) break;
        if (load->cmd == LC_SEGMENT_64 && load->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
            bool valid = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
            uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
            if (valid && segment->vmsize && segment->vmsize <= UINTPTR_MAX - begin && (segment->initprot & VM_PROT_EXECUTE)) {
                for (unsigned j = 0; j < 8; ++j) if (!atomic_load_explicit(&peerCallerRanges[j].image, memory_order_relaxed)) {
                    atomic_store_explicit(&peerCallerRanges[j].begin, begin, memory_order_relaxed);
                    atomic_store_explicit(&peerCallerRanges[j].end, begin + segment->vmsize, memory_order_relaxed);
                    atomic_store_explicit(&peerCallerRanges[j].image, (uintptr_t)image, memory_order_relaxed);
                    break;
                }
            }
        }
        command += load->cmdsize;
    }
    atomic_fetch_add_explicit(&peerCallerSequence, 1, memory_order_release);
}

static bool peerCallerAllowed(uintptr_t caller) {
    if (!peerInspectionEnabled) return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        unsigned sequence = atomic_load_explicit(&peerCallerSequence, memory_order_acquire);
        if (sequence & 1) continue;
        bool found = false;
        for (unsigned i = 0; i < 8; ++i) {
            if (!atomic_load_explicit(&peerCallerRanges[i].image, memory_order_relaxed)) continue;
            uintptr_t begin = atomic_load_explicit(&peerCallerRanges[i].begin, memory_order_relaxed);
            uintptr_t end = atomic_load_explicit(&peerCallerRanges[i].end, memory_order_relaxed);
            if (caller >= begin && caller < end) { found = true; break; }
        }
        if (sequence == atomic_load_explicit(&peerCallerSequence, memory_order_acquire)) return found;
    }
    return false;
}

bool LSBPeerProcessCallerEnabled(uintptr_t caller) { return peerCallerAllowed(caller); }

static bool peerQueryAllowed(uintptr_t caller, int pid) {
    return peerCallerAllowed(caller) && pid > 0 && pid != LSBNativePID() && pid != LSBGuestPID();
}

static int copyPeer(int pid, LSBPeerProcessIdentity *identity) {
    memset(identity, 0, sizeof(*identity));
    int error = LSBTransportCopyProcessIdentity(pid, identity);
    // The transport validates the genuine name right against the full audit token. Preserve that token and the snapshot verbatim.
    uint32_t snapshotPID = LSBPeerProcessIdentityPID(identity);
    if (!error && (identity->audit.val[5] != (uint32_t)pid || snapshotPID != (uint32_t)pid ||
                   !MACH_PORT_VALID(identity->taskName) || !memchr(identity->executablePath, '\0', sizeof(identity->executablePath)) ||
                   !canonicalPath(identity->executablePath))) error = EPROTO;
    if (error) LSBPeerProcessIdentityDispose(identity);
    return error;
}

static bool writePeerBytes(void *output, const void *bytes, mach_msg_type_number_t size) {
    return mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)output, (vm_offset_t)(uintptr_t)bytes, size) == KERN_SUCCESS;
}

int LSBPeerProcessPIDInfo(uintptr_t caller, int pid, int flavor, uint64_t argument, void *buffer, int size, int result, int originalErrno) {
    if (!LSBPeerProcessCallerEnabled(caller)) return result;
    int saved = errno;
    if (result != 0 || originalErrno != EPERM || flavor != LSB_PEER_SHORT_INFO_FLAVOR || argument || !buffer ||
        size < LSB_PEER_SHORT_INFO_BYTES || !peerQueryAllowed(caller, pid)) { errno = saved; return result; }
    LSBPeerProcessIdentity identity;
    if (!copyPeer(pid, &identity)) {
        if (writePeerBytes(buffer, identity.shortInfo, LSB_PEER_SHORT_INFO_BYTES)) result = LSB_PEER_SHORT_INFO_BYTES;
        else saved = EFAULT;
        LSBPeerProcessIdentityDispose(&identity);
    }
    errno = saved;
    return result;
}

kern_return_t LSBPeerProcessTaskName(uintptr_t caller, mach_port_name_t target, int pid, mach_port_name_t *task, kern_return_t result) {
    if (!LSBPeerProcessCallerEnabled(caller)) return result;
    int saved = errno;
    if ((result != KERN_FAILURE && result != KERN_PROTECTION_FAILURE) || target != mach_task_self() || !task ||
        !peerQueryAllowed(caller, pid)) { errno = saved; return result; }
    LSBPeerProcessIdentity identity;
    if (!copyPeer(pid, &identity)) {
        if (writePeerBytes(task, &identity.taskName, sizeof(identity.taskName))) {
            identity.taskName = MACH_PORT_NULL; // Transfer precisely one owned send reference only after successful copyout.
            result = KERN_SUCCESS;
        }
        LSBPeerProcessIdentityDispose(&identity);
    }
    errno = saved;
    return result;
}

static int peerPIDPath(uintptr_t caller, int pid, void *buffer, uint32_t size) {
    if (!LSBPeerProcessCallerEnabled(caller)) return proc_pidpath(pid, buffer, size);
    int result = proc_pidpath(pid, buffer, size), saved = errno;
    if ((!result && saved == EFAULT) || !buffer || !size || !peerQueryAllowed(caller, pid)) { errno = saved; return result; }
    LSBPeerProcessIdentity identity;
    if (!copyPeer(pid, &identity)) {
        // The broker keeps a virtual path for role checks. RBS passes proc_pidpath's result straight to xpc_bundle_create,
        // so this API must expose the physical executable, as it does on macOS Simulator, without changing broker identity.
        char physicalPath[PATH_MAX];
        size_t rootLength = strlen(peerRuntimeRoot), guestLength = strlen(identity.executablePath);
        size_t length = rootLength + guestLength;
        if (length >= sizeof(physicalPath)) { result = 0; saved = ENAMETOOLONG; }
        else if (length < size) {
            memcpy(physicalPath, peerRuntimeRoot, rootLength);
            memcpy(physicalPath + rootLength, identity.executablePath, guestLength + 1);
            if (writePeerBytes(buffer, physicalPath, (mach_msg_type_number_t)length + 1)) result = (int)length;
            else { result = 0; saved = EFAULT; }
        }
        else { result = 0; saved = ENOSPC; }
        LSBPeerProcessIdentityDispose(&identity);
    }
    errno = saved;
    return result;
}

__attribute__((noinline)) int LSBPeerProcessPIDPath(int pid, void *buffer, uint32_t size) {
    return peerPIDPath((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), pid, buffer, size);
}

__attribute__((used, noinline)) static int peerSandboxPolicy(pid_t pid, const char *operation, int type, uintptr_t caller) {
    if (!LSBPeerProcessCallerEnabled(caller)) return 0; // Interposition is active before constructors and simulator errno/TLS.
    int saved = errno;
    // These two stock RBS preflights have no variadic filter argument. The real queries still enforce live broker ownership.
    bool handled = peerCallerAllowed(caller) && pid == LSBNativePID() && type == SANDBOX_CHECK_NO_REPORT && operation &&
        (!strcmp(operation, "process-info-pidinfo") || !strcmp(operation, "mach-task-name"));
    errno = saved;
    return handled;
}

#if defined(__arm64__)
#ifndef LSB_PEER_SANDBOX_ORIGINAL
#define LSB_PEER_SANDBOX_ORIGINAL "_sandbox_check"
#endif
// Darwin arm64 puts variadic arguments on the caller's stack. Do not inspect or reconstruct any of them.
// Preserve every argument register (including indirect result x8), SIMD argument registers, LR and NZCV across the C policy call.
// On the unhandled path the original API sees the original SP and tail-call LR, including calls with no variadic argument at all.
__attribute__((naked)) int LSBPeerSandboxCheck(pid_t pid, const char *operation, int type, ...) {
    __asm__("sub sp, sp, #240\n"
            "stp x0, x1, [sp, #0]\n stp x2, x3, [sp, #16]\n stp x4, x5, [sp, #32]\n stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n stp x29, x30, [sp, #80]\n"
            "stp q0, q1, [sp, #96]\n stp q2, q3, [sp, #128]\n stp q4, q5, [sp, #160]\n stp q6, q7, [sp, #192]\n"
            "mrs x9, nzcv\n str x9, [sp, #224]\n mov x3, x30\n bl _peerSandboxPolicy\n mov w16, w0\n"
            "ldr x9, [sp, #224]\n msr nzcv, x9\n"
            "ldp q0, q1, [sp, #96]\n ldp q2, q3, [sp, #128]\n ldp q4, q5, [sp, #160]\n ldp q6, q7, [sp, #192]\n"
            "ldp x0, x1, [sp, #0]\n ldp x2, x3, [sp, #16]\n ldp x4, x5, [sp, #32]\n ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n ldp x29, x30, [sp, #80]\n add sp, sp, #240\n"
            "cbnz w16, 1f\n b " LSB_PEER_SANDBOX_ORIGINAL "\n1: mov w0, #0\n ret\n");
}
#else
#error Peer sandbox forwarding requires the Darwin arm64 ABI.
#endif

#ifndef LSB_PEER_PROCESS_COMPATIBILITY_TESTING
static void peerImageAdded(const struct mach_header *image, intptr_t slide) {
    Dl_info info;
    if (dladdr(image, &info)) rememberPeerCaller(image, slide, info.dli_fname);
}
static void peerImageRemoved(const struct mach_header *image, intptr_t slide) { (void)slide; forgetPeerCaller(image); }

__attribute__((constructor)) static void initializePeerProcessCompatibility(void) {
    int saved = errno;
    const char *enabled = getenv("LSB_ENABLE_PEER_PROCESS_INSPECTION"), *root = getenv("DYLD_ROOT_PATH");
    char executable[PATH_MAX], physicalExecutable[PATH_MAX], physicalRoot[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (enabled && !strcmp(enabled, "1") && root && realpath(root, physicalRoot) && !_NSGetExecutablePath(executable, &size) &&
        realpath(executable, physicalExecutable)) configurePeerScope(enabled, physicalRoot, physicalExecutable);
    if (peerInspectionEnabled) {
        _dyld_register_func_for_add_image(peerImageAdded);
        _dyld_register_func_for_remove_image(peerImageRemoved);
    }
    errno = saved;
}

// proc_pidinfo and task_name_for_pid are integrated into their existing wrappers, avoiding ambiguous duplicate interpose tuples.
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBPeerProcessPIDPath, (const void *)&proc_pidpath}, {(const void *)&LSBPeerSandboxCheck, (const void *)&sandbox_check}
};
#endif
