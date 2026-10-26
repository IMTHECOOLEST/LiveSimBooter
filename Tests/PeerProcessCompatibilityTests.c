#include <assert.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int testPIDPath(int pid, void *buffer, uint32_t size);
int LSBTestSandboxOriginal(pid_t pid, const char *operation, int type, ...);
#define proc_pidpath testPIDPath
#define LSB_PEER_PROCESS_COMPATIBILITY_TESTING
#define LSB_PEER_SANDBOX_ORIGINAL "_LSBTestSandboxOriginal"
#include "../Guest/PeerProcessCompatibility.c"
#undef proc_pidpath

const int SANDBOX_CHECK_NO_REPORT = 0x100000;
pid_t LSBNativePID(void) { return 101; }
pid_t LSBGuestPID(void) { return 99; }
static unsigned queries, nativePathCalls, originalSandboxCalls;
static int queryError, pathResult = 9, pathErrno = EPERM;
static bool badAudit, badSnapshot, badPath;
static mach_port_t namePort;
static const char guestPath[] = "/System/Library/CoreServices/SpringBoard.app/SpringBoard";
static const char physicalGuestPath[] = "/private/Group/RuntimeRoot/System/Library/CoreServices/SpringBoard.app/SpringBoard";
static const char *replyPath = guestPath;
__attribute__((used, aligned(16))) static uint64_t sandboxEntry[30];
__attribute__((used)) static uintptr_t expectedStack, expectedLink;
__attribute__((used)) static const char captureOperation[] = "capture-registers";

int LSBTransportCopyProcessIdentity(pid_t pid, LSBPeerProcessIdentity *identity) {
    ++queries;
    memset(identity, 0, sizeof(*identity));
    errno = EIO; // Helpers must preserve the native call's errno rather than transport scratch state.
    if (queryError) return queryError;
    for (unsigned i = 0; i < 8; ++i) identity->audit.val[i] = 0x100 + i;
    identity->audit.val[5] = badAudit ? (uint32_t)pid + 1 : (uint32_t)pid;
    memset(identity->shortInfo, 0xa5, sizeof(identity->shortInfo));
    uint32_t snapshotPID = badSnapshot ? (uint32_t)pid + 1 : (uint32_t)pid;
    memcpy(identity->shortInfo, &snapshotPID, sizeof(snapshotPID));
    if (badPath) memset(identity->executablePath, 'x', sizeof(identity->executablePath));
    else {
        assert(strlen(replyPath) < sizeof(identity->executablePath));
        memcpy(identity->executablePath, replyPath, strlen(replyPath) + 1);
    }
    assert(mach_port_mod_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS);
    identity->taskName = namePort;
    return 0;
}

static mach_port_urefs_t sendReferences(void) {
    mach_port_urefs_t references;
    assert(mach_port_get_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_SEND, &references) == KERN_SUCCESS);
    return references;
}

static int testPIDPath(int pid, void *buffer, uint32_t size) {
    (void)pid;
    ++nativePathCalls;
    errno = pathErrno;
    if (buffer && size >= 10 && pathResult) memcpy(buffer, "/host/app", 10);
    return pathResult;
}

__attribute__((used)) static int sandboxOriginalBody(pid_t pid, const char *operation, int type, ...) {
    ++originalSandboxCalls;
    assert(pid == 101 || pid == 42);
    if (operation && !strcmp(operation, "forward-variadic")) {
        assert(type == 0x76543210);
        va_list args;
        va_start(args, type);
        for (uint64_t i = 0; i < 12; ++i) assert(va_arg(args, uint64_t) == UINT64_C(0xfedcba9800000000) + i);
        assert(va_arg(args, double) == 3.25);
        assert(!strcmp(va_arg(args, const char *), "tail-stack-argument"));
        va_end(args);
    }
    errno = EBUSY;
    return 117;
}

// Capture the actual native forwarding boundary before a C prologue can repurpose its caller-saved registers.
__attribute__((naked)) int LSBTestSandboxOriginal(pid_t pid, const char *operation, int type, ...) {
    __asm__("adrp x9, _sandboxEntry@PAGE\n add x9, x9, _sandboxEntry@PAGEOFF\n"
            "stp x0, x1, [x9, #0]\n stp x2, x3, [x9, #16]\n stp x4, x5, [x9, #32]\n stp x6, x7, [x9, #48]\n str x8, [x9, #64]\n"
            "stp q0, q1, [x9, #80]\n stp q2, q3, [x9, #112]\n stp q4, q5, [x9, #144]\n stp q6, q7, [x9, #176]\n"
            "mrs x10, nzcv\n str x10, [x9, #208]\n mov x10, sp\n str x10, [x9, #216]\n str x30, [x9, #224]\n"
            "b _sandboxOriginalBody\n");
}

__attribute__((naked, noinline)) static int callWithRegisters(void) {
    __asm__("stp x29, x30, [sp, #-16]!\n mov x29, sp\n"
            "adrp x9, _expectedStack@PAGE\n add x9, x9, _expectedStack@PAGEOFF\n mov x10, sp\n str x10, [x9]\n"
            "adrp x9, _expectedLink@PAGE\n add x9, x9, _expectedLink@PAGEOFF\n adr x10, 1f\n str x10, [x9]\n"
            "mov x0, #42\n adrp x1, _captureOperation@PAGE\n add x1, x1, _captureOperation@PAGEOFF\n"
            "mov x2, #0x1234\n mov x3, #0x3333\n mov x4, #0x4444\n mov x5, #0x5555\n mov x6, #0x6666\n"
            "mov x7, #0x7777\n mov x8, #0x8888\n"
            "movi v0.16b, #0x10\n movi v1.16b, #0x11\n movi v2.16b, #0x12\n movi v3.16b, #0x13\n"
            "movi v4.16b, #0x14\n movi v5.16b, #0x15\n movi v6.16b, #0x16\n movi v7.16b, #0x17\n"
            "cmp xzr, xzr\n bl _LSBPeerSandboxCheck\n1: ldp x29, x30, [sp], #16\n ret\n");
}

static void setCaller(uintptr_t begin, uintptr_t end) {
    for (unsigned i = 0; i < 8; ++i) atomic_store(&peerCallerRanges[i].image, 0);
    atomic_store(&peerCallerRanges[0].begin, begin);
    atomic_store(&peerCallerRanges[0].end, end);
    atomic_store(&peerCallerRanges[0].image, 1);
}

static void testScope(void) {
    const char *root = "/private/Group/RuntimeRoot", *exe = "/private/Group/RuntimeRoot/usr/libexec/runningboardd";
    const char *disabled[] = {NULL, "", "0", "true", "01"};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) {
        configurePeerScope(disabled[i], root, exe);
        assert(!peerInspectionEnabled);
    }
    const char *wrongExe[] = {"/usr/libexec/runningboardd", "/private/Group/RuntimeRoot-other/usr/libexec/runningboardd",
        "/private/Group/RuntimeRoot/usr/sbin/runningboardd", "/private/Group/RuntimeRoot/usr/libexec/runningboardd-helper",
        "/private/Group/RuntimeRoot/usr/libexec/../libexec/runningboardd", "/private/Group/RuntimeRoot/bin/df", "/x"};
    for (unsigned i = 0; i < sizeof(wrongExe) / sizeof(*wrongExe); ++i) {
        configurePeerScope("1", root, wrongExe[i]);
        assert(!peerInspectionEnabled);
    }
    const char *wrongRoot[] = {NULL, "", "/", "relative", "/private/Group/RuntimeRoot/", "/private//Group", "/A:/B", "/A/./B", "/A/../B"};
    for (unsigned i = 0; i < sizeof(wrongRoot) / sizeof(*wrongRoot); ++i) {
        configurePeerScope("1", wrongRoot[i], exe);
        assert(!peerInspectionEnabled);
    }
    configurePeerScope("1", root, exe);
    assert(!strcmp(peerRuntimeRoot, root));
    assert(peerInspectionEnabled && !strcmp(servicesPhysicalPath,
        "/private/Group/RuntimeRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices"));
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *paths[] = {servicesInstallPath, servicesPhysicalPath};
    for (unsigned i = 0; i < 2; ++i) {
        rememberPeerCaller(header, 0x4000, paths[i]);
        assert(!peerCallerAllowed(0x4fff) && peerCallerAllowed(0x5000) && peerCallerAllowed(0x50ff));
        assert(!peerCallerAllowed(0x5100) && !peerCallerAllowed(0x6000));
        forgetPeerCaller(header);
        assert(!peerCallerAllowed(0x5000));
    }
    const char *wrongPaths[] = {"/Plugins/RunningBoardServices", "/System/Library/Frameworks/RunningBoardServices.framework/RunningBoardServices",
        "/OtherRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices",
        "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServicesHelper"};
    for (unsigned i = 0; i < sizeof(wrongPaths) / sizeof(*wrongPaths); ++i) {
        rememberPeerCaller(header, 0, wrongPaths[i]);
        assert(!peerCallerAllowed(0x1000));
    }
    setCaller(0x1000, 0x1100);
}

static void testShortInfo(void) {
    unsigned char output[80], original[80];
    memset(output, 0xcc, sizeof(output));
    memcpy(original, output, sizeof(output));
    struct { uintptr_t caller; int pid, flavor; uint64_t argument; int size, result, error; } rejected[] = {
        {0x999, 202, 13, 0, 80, 0, EPERM}, {0x1100, 202, 13, 0, 80, 0, EPERM}, {0x1000, 101, 13, 0, 80, 0, EPERM},
        {0x1000, 99, 13, 0, 80, 0, EPERM}, {0x1000, -1, 13, 0, 80, 0, EPERM}, {0x1000, 202, 3, 0, 80, 0, EPERM},
        {0x1000, 202, 13, 1, 80, 0, EPERM}, {0x1000, 202, 13, 0, 63, 0, EPERM}, {0x1000, 202, 13, 0, -1, 0, EPERM},
        {0x1000, 202, 13, 0, 80, 64, EPERM}, {0x1000, 202, 13, 0, 80, 0, ESRCH}, {0x1000, 202, 13, 0, 80, -1, EPERM}
    };
    unsigned before = queries;
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        errno = ERANGE;
        assert(LSBPeerProcessPIDInfo(rejected[i].caller, rejected[i].pid, rejected[i].flavor, rejected[i].argument, output,
            rejected[i].size, rejected[i].result, rejected[i].error) == rejected[i].result && errno == ERANGE);
        assert(!memcmp(output, original, sizeof(output)) && queries == before);
    }
    assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, NULL, 80, 0, EPERM) && queries == before);
    errno = EPERM;
    assert(LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, output, sizeof(output), 0, EPERM) == 64 && errno == EPERM);
    uint32_t pid;
    memcpy(&pid, output, sizeof(pid));
    assert(pid == 202 && output[4] == 0xa5 && output[63] == 0xa5 && output[64] == 0xcc && sendReferences() == 1);
    for (unsigned i = 0; i < 4; ++i) {
        queryError = i == 0 ? ESRCH : 0;
        badAudit = i == 1; badSnapshot = i == 2; badPath = i == 3;
        memcpy(output, original, sizeof(output));
        errno = EPERM;
        assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, output, sizeof(output), 0, EPERM) && errno == EPERM);
        assert(!memcmp(output, original, sizeof(output)) && sendReferences() == 1);
    }
    queryError = 0; badAudit = badSnapshot = badPath = false;
}

static void testTaskNames(void) {
    mach_port_t result = 0x1234;
    unsigned before = queries;
    errno = EDOM;
    assert(LSBPeerProcessTaskName(0x999, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, MACH_PORT_NULL, 202, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 101, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, NULL, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_INVALID_ARGUMENT) == KERN_INVALID_ARGUMENT);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_SUCCESS) == KERN_SUCCESS);
    assert(result == 0x1234 && queries == before && errno == EDOM);
    queryError = ESRCH;
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_FAILURE && result == 0x1234);
    queryError = 0;
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_SUCCESS);
    assert(result == namePort && sendReferences() == 2 && errno == EDOM);
    assert(mach_port_deallocate(mach_task_self(), result) == KERN_SUCCESS && sendReferences() == 1);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_PROTECTION_FAILURE) == KERN_SUCCESS);
    assert(mach_port_deallocate(mach_task_self(), result) == KERN_SUCCESS && sendReferences() == 1);
}

static void testPaths(void) {
    char output[256];
    assert(peerPIDPath(0x999, 202, output, sizeof(output)) == 9 && !strcmp(output, "/host/app"));
    unsigned before = queries;
    assert(peerPIDPath(0x1000, 101, output, sizeof(output)) == 9 && queries == before);
    assert(peerPIDPath(0x1000, 202, output, sizeof(output)) == (int)strlen(physicalGuestPath) && !strcmp(output, physicalGuestPath));
    assert(errno == EPERM && sendReferences() == 1);
    queryError = ESRCH;
    assert(peerPIDPath(0x1000, 202, output, sizeof(output)) == 9 && !strcmp(output, "/host/app") && errno == EPERM);
    queryError = 0;
    memset(output, 0xcc, sizeof(output));
    assert(!peerPIDPath(0x1000, 202, output, 4) && errno == ENOSPC && (unsigned char)output[4] == 0xcc && sendReferences() == 1);
    assert(nativePathCalls == 5);
    memset(output, 0xcc, sizeof(output));
    pathResult = 0;
    assert(!peerPIDPath(0x1000, 202, output, (uint32_t)strlen(physicalGuestPath)) && errno == ENOSPC);
    assert((unsigned char)output[0] == 0xcc); // Space for the complete path including NUL is required.
    assert(peerPIDPath(0x1000, 202, output, sizeof(physicalGuestPath)) == (int)strlen(physicalGuestPath));
    assert(!memcmp(output, physicalGuestPath, sizeof(physicalGuestPath)) && (unsigned char)output[sizeof(physicalGuestPath)] == 0xcc);
    const char *malformed[] = {"relative", "/", "/System//SpringBoard", "/System/../SpringBoard", "/System/./SpringBoard", "/System/SpringBoard/"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        replyPath = malformed[i];
        memset(output, 0xcc, sizeof(output));
        assert(!peerPIDPath(0x1000, 202, output, sizeof(output)) && errno == EPERM);
        assert((unsigned char)output[0] == 0xcc && sendReferences() == 1);
    }
    char longPath[LSB_PATH_BYTES];
    memset(longPath, 'a', sizeof(longPath));
    longPath[0] = '/'; longPath[sizeof(longPath) - 1] = '\0';
    replyPath = longPath;
    assert(!peerPIDPath(0x1000, 202, output, sizeof(output)) && errno == ENAMETOOLONG && sendReferences() == 1);
    replyPath = guestPath; pathResult = 9;
}

static void testInvalidOutputs(void) {
    size_t page = (size_t)vm_page_size;
    unsigned char *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapping != MAP_FAILED);
    memset(mapping, 0xcc, page * 2);
    assert(!mprotect(mapping, page, PROT_READ) && !mprotect(mapping + page, page, PROT_NONE));
    void *invalid[] = {(void *)(uintptr_t)1, mapping, mapping + page - 2, mapping + page};
    pathResult = 0; // A native EPERM need not have inspected or touched the supplied output.
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        errno = EPERM;
        assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, invalid[i], 64, 0, EPERM) && errno == EFAULT);
        assert(sendReferences() == 1);
        errno = EDOM;
        assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, invalid[i], KERN_FAILURE) == KERN_FAILURE && errno == EDOM);
        assert(sendReferences() == 1); // The caller received no right, so the acquired send reference must be released.
        assert(!peerPIDPath(0x1000, 202, invalid[i], 256) && errno == EFAULT && sendReferences() == 1);
    }
    assert(mapping[0] == 0xcc && mapping[page - 1] == 0xcc);
    pathErrno = EFAULT;
    unsigned before = queries;
    assert(!peerPIDPath(0x1000, 202, (void *)(uintptr_t)1, 256) && errno == EFAULT && queries == before);
    pathResult = 9; pathErrno = EPERM;
    assert(!munmap(mapping, page * 2));
}

__attribute__((noinline)) static void testSandbox(void) {
    setCaller((uintptr_t)&testSandbox, (uintptr_t)&testSandbox + 4096);
    errno = EDOM;
    assert(!LSBPeerSandboxCheck(101, "process-info-pidinfo", SANDBOX_CHECK_NO_REPORT) && errno == EDOM);
    assert(!LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT) && errno == EDOM && !originalSandboxCalls);
    assert(LSBPeerSandboxCheck(42, "process-info-pidinfo", SANDBOX_CHECK_NO_REPORT) == 117 && errno == EBUSY);
    assert(LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT | 1, "filter") == 117);
    assert(LSBPeerSandboxCheck(101, "other-operation", 0) == 117); // No variadic arguments: no va_arg is permitted on forwarding.
    assert(LSBPeerSandboxCheck(42, "forward-variadic", 0x76543210,
        UINT64_C(0xfedcba9800000000), UINT64_C(0xfedcba9800000001), UINT64_C(0xfedcba9800000002), UINT64_C(0xfedcba9800000003),
        UINT64_C(0xfedcba9800000004), UINT64_C(0xfedcba9800000005), UINT64_C(0xfedcba9800000006), UINT64_C(0xfedcba9800000007),
        UINT64_C(0xfedcba9800000008), UINT64_C(0xfedcba9800000009), UINT64_C(0xfedcba980000000a), UINT64_C(0xfedcba980000000b),
        3.25, "tail-stack-argument") == 117 && errno == EBUSY);
    assert(originalSandboxCalls == 4);
    assert(callWithRegisters() == 117 && errno == EBUSY);
    assert(sandboxEntry[0] == 42 && sandboxEntry[1] == (uintptr_t)captureOperation && sandboxEntry[2] == 0x1234);
    for (unsigned i = 3; i <= 8; ++i) assert(sandboxEntry[i] == i * 0x1111);
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t pattern = UINT64_C(0x0101010101010101) * (0x10 + i);
        assert(sandboxEntry[10 + 2 * i] == pattern && sandboxEntry[11 + 2 * i] == pattern);
    }
    assert(sandboxEntry[26] == 0x60000000 && sandboxEntry[27] == expectedStack && sandboxEntry[28] == expectedLink);
    setCaller(0x1000, 0x1100);
    assert(LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT) == 117 && originalSandboxCalls == 6);
    peerInspectionEnabled = false;
    assert(!peerSandboxPolicy(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT, 0x1000));
}

int main(void) {
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &namePort) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), namePort, namePort, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    testScope(); testShortInfo(); testTaskNames(); testPaths(); testInvalidOutputs(); testSandbox();
    assert(sendReferences() == 1);
    assert(mach_port_deallocate(mach_task_self(), namePort) == KERN_SUCCESS);
    assert(mach_port_mod_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
    puts("PeerProcessCompatibilityTests passed");
    return 0;
}
