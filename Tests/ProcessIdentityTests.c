#include <mach/mach.h>
static kern_return_t testTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task);
#define task_name_for_pid testTaskNameForPID
#define LSB_PROCESS_IDENTITY_TESTING
#define LSB_GUEST_RUNTIME
#include "../Guest/ProcessIdentity.c"
#undef task_name_for_pid
#include "../Guest/ChildKill.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>

static unsigned calls;
int LSBTransportKill(pid_t pid, int number) { (void)pid; (void)number; ++calls; return ESRCH; }

static unsigned taskNameCalls;
static mach_port_name_t observedTaskTarget, *observedTaskOutput;
static int observedTaskPID;
static kern_return_t taskNameResult;
static bool launchdOverride;
static kern_return_t launchdResult;
bool LSBLaunchdProcessTaskName(uintptr_t caller, mach_port_name_t target, int pid, mach_port_name_t *task, kern_return_t *result) {
    (void)caller; (void)target; (void)pid;
    if (!launchdOverride) return false;
    *result = launchdResult;
    if (task && launchdResult == KERN_SUCCESS) *task = 0x4321;
    return true;
}
kern_return_t LSBPeerProcessTaskName(uintptr_t caller, mach_port_name_t target, int pid, mach_port_name_t *task, kern_return_t result) {
    (void)caller; (void)target; (void)pid; (void)task;
    return result;
}
static kern_return_t testTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task) {
    ++taskNameCalls;
    observedTaskTarget = target;
    observedTaskPID = pid;
    observedTaskOutput = task;
    if (task && taskNameResult == KERN_SUCCESS) *task = 0x1234;
    return taskNameResult;
}

static void testTaskNameSelfTranslation(void) {
    mach_port_name_t task = MACH_PORT_NULL, self = mach_task_self();
    taskNameResult = KERN_SUCCESS;
    assert(LSBTaskNameForPID(self, 123, &task) == KERN_SUCCESS && task == 0x1234);
    assert(taskNameCalls == 1 && observedTaskTarget == self && observedTaskPID == LSBNativePID() && observedTaskOutput == &task);
    assert(LSBTaskNameForPID(self, LSBNativePID(), &task) == KERN_SUCCESS && observedTaskPID == LSBNativePID());
    assert(LSBTaskNameForPID(self, 789, &task) == KERN_SUCCESS && observedTaskPID == 789);
    assert(LSBTaskNameForPID(MACH_PORT_NULL, 123, &task) == KERN_SUCCESS && observedTaskPID == 123 && observedTaskTarget == MACH_PORT_NULL);
    assert(LSBTaskNameForPID(self, 0, &task) == KERN_SUCCESS && observedTaskPID == 0);
    assert(LSBTaskNameForPID(self, -1, &task) == KERN_SUCCESS && observedTaskPID == -1);
    taskNameResult = KERN_FAILURE;
    task = 0x5678;
    assert(LSBTaskNameForPID(self, 123, &task) == KERN_FAILURE && task == 0x5678 && observedTaskPID == LSBNativePID());
    taskNameResult = KERN_INVALID_ARGUMENT;
    assert(LSBTaskNameForPID(self, 123, NULL) == KERN_INVALID_ARGUMENT && !observedTaskOutput && taskNameCalls == 8);
    unsigned before = taskNameCalls;
    launchdOverride = true;
    launchdResult = KERN_SUCCESS;
    taskNameResult = KERN_SUCCESS; // A reused logical PID could resolve natively, but the launchd override must run first.
    assert(LSBTaskNameForPID(self, 789, &task) == KERN_SUCCESS && task == 0x4321 && taskNameCalls == before);
    launchdResult = KERN_FAILURE;
    task = 0x5678;
    assert(LSBTaskNameForPID(self, 789, &task) == KERN_FAILURE && task == 0x5678 && taskNameCalls == before);
    launchdOverride = false;
}

static void testNativeCallerRanges(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text, data, text2; } image = {0};
    image.header.magic = MH_MAGIC_64;
    image.header.ncmds = 3;
    image.header.sizeofcmds = 3 * sizeof(struct segment_command_64);
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    image.text2 = image.text;
    image.text2.vmaddr = 0x3000;
    const struct mach_header *header = (const struct mach_header *)&image;
    rememberNativeIdentityImage(header, 0x4000, "/usr/lib/system/libxpc.dylib");
    assert(!callerNeedsNativeIdentity(0x4fff) && callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff));
    assert(!callerNeedsNativeIdentity(0x5100) && !callerNeedsNativeIdentity(0x6000));
    assert(callerNeedsNativeIdentity(0x7000) && !callerNeedsNativeIdentity(0x7100));
    rememberNativeIdentityImage(header, 0, "/usr/lib/system/libUnknown.dylib");
    assert(callerNeedsNativeIdentity(0x5000) && !callerNeedsNativeIdentity(0x1000));
    forgetNativeIdentityImage(header);
    assert(!callerNeedsNativeIdentity(0x5000) && !callerNeedsNativeIdentity(0x7000));
    rememberNativeIdentityImage(header, -0x800, "/RuntimeRoot/usr/lib/system/libxpc.dylib");
    assert(callerNeedsNativeIdentity(0x800) && !callerNeedsNativeIdentity(0x1800));
    forgetNativeIdentityImage(header);
    image.header.magic = 0;
    rememberNativeIdentityImage(header, 0, "/usr/lib/system/libxpc.dylib");
    assert(!callerNeedsNativeIdentity(0x1000));
    assert(LSBGetPID() == LSBGuestPID());
}

static void testNotifydCallerIdentity(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 1, .sizeofcmds = sizeof(image.text)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *root = "/private/var/mobile/Group/RuntimeRoot", *notifyd = "/private/var/mobile/Group/RuntimeRoot/usr/sbin/notifyd";
    assert(!setenv("DYLD_ROOT_PATH", root, 1));
    const char *disabled[] = {NULL, "", "0", "01", "true", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        if (disabled[i]) assert(!setenv("LSB_SIMULATE_NOTIFICATION_SHM", disabled[i], 1));
        else assert(!unsetenv("LSB_SIMULATE_NOTIFICATION_SHM"));
        configureNativeNotifydIdentity();
        rememberNativeIdentityImage(header, 0, notifyd);
        assert(!callerNeedsNativeIdentity(0x1000));
        rememberNativeIdentityImage(header, 0, "/usr/lib/system/libxpc.dylib");
        assert(callerNeedsNativeIdentity(0x1000)); // Existing libxpc handling does not depend on the notification mode.
        forgetNativeIdentityImage(header);
    }
    assert(!setenv("LSB_SIMULATE_NOTIFICATION_SHM", "1", 1));
    configureNativeNotifydIdentity();
    const char *unrelated[] = {"/usr/sbin/notifyd", "/private/var/mobile/Group/RuntimeRoot-other/usr/sbin/notifyd",
        "/private/var/mobile/Group/RuntimeRoot/usr/bin/notifyd", "/private/var/mobile/Group/RuntimeRoot/usr/sbin/notifyd-helper",
        "/private/var/mobile/Group/RuntimeRoot/usr/sbin/../sbin/notifyd", "/usr/lib/system/libsystem_notify.dylib",
        "/private/var/mobile/Group/RuntimeRoot/usr/lib/system/libsystem_notify.dylib"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        rememberNativeIdentityImage(header, 0, unrelated[i]);
        assert(!callerNeedsNativeIdentity(0x1000));
    }
    rememberNativeIdentityImage(header, 0x4000, notifyd);
    assert(callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff) && !callerNeedsNativeIdentity(0x5100));
    assert(LSBGetPID() == 123); // Ordinary guest callers still observe their logical identity.
    forgetNativeIdentityImage(header);
    const char *invalidRoots[] = {NULL, "", "/", "relative/RuntimeRoot", "/private/var/mobile/Group/RuntimeRoot/",
        "/private//RuntimeRoot", "/private/./RuntimeRoot", "/private/../RuntimeRoot", "/RuntimeRoot:/OtherRoot"};
    for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(invalidRoots[0]); ++i) {
        if (invalidRoots[i]) assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
        else assert(!unsetenv("DYLD_ROOT_PATH"));
        configureNativeNotifydIdentity();
        assert(!nativeNotifydPath[0]);
        rememberNativeIdentityImage(header, 0, notifyd);
        assert(!callerNeedsNativeIdentity(0x1000));
    }
    assert(!unsetenv("DYLD_ROOT_PATH") && !unsetenv("LSB_SIMULATE_NOTIFICATION_SHM"));
    configureNativeNotifydIdentity();
}

static void testBaseBoardCallerIdentity(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *physical = "/private/var/mobile/Group/RuntimeRoot/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard";
    assert(!setenv("DYLD_ROOT_PATH", "/private/var/mobile/Group/RuntimeRoot", 1));
    configureNativeImagePath(nativeBaseBoardPath, baseBoardInstallPath);
    assert(!strcmp(nativeBaseBoardPath, physical));
    const char *accepted[] = {baseBoardInstallPath, physical};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        rememberNativeIdentityImage(header, 0x4000, accepted[i]);
        assert(!callerNeedsNativeIdentity(0x4fff) && callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff));
        assert(!callerNeedsNativeIdentity(0x5100) && !callerNeedsNativeIdentity(0x6000));
        assert(LSBGetPID() == 123 && LSBGuestPID() == 123); // Ordinary callers retain the logical process identity.
        forgetNativeIdentityImage(header);
        assert(!callerNeedsNativeIdentity(0x5000));
    }
    const char *unrelated[] = {"/Plugins/BaseBoard", "/System/Library/Frameworks/BaseBoard.framework/BaseBoard",
        "/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoardHelper",
        "/System/Library/PrivateFrameworks/Other.framework/BaseBoard", "/System/Library/PrivateFrameworks/BaseBoard.framework/../BaseBoard",
        "/private/var/mobile/Group/RuntimeRoot-other/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard",
        "/OtherRoot/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        rememberNativeIdentityImage(header, 0, unrelated[i]);
        assert(!callerNeedsNativeIdentity(0x1000));
    }
    const char *invalidRoots[] = {"/", "relative", "/RuntimeRoot/", "/RuntimeRoot:/OtherRoot", "/RuntimeRoot/../Other", "/Runtime//Root"};
    for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(invalidRoots[0]); ++i) {
        assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
        configureNativeImagePath(nativeBaseBoardPath, baseBoardInstallPath);
        assert(!nativeBaseBoardPath[0]);
        rememberNativeIdentityImage(header, 0, physical);
        assert(!callerNeedsNativeIdentity(0x1000));
        rememberNativeIdentityImage(header, 0, baseBoardInstallPath); // A cached install path needs no physical-root alias.
        assert(callerNeedsNativeIdentity(0x1000));
        forgetNativeIdentityImage(header);
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    configureNativeImagePath(nativeBaseBoardPath, baseBoardInstallPath);
    assert(!nativeBaseBoardPath[0]);
}

static void testRunningBoardServicesCallerIdentity(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *physical =
        "/private/var/mobile/Group/RuntimeRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices";
    assert(!setenv("DYLD_ROOT_PATH", "/private/var/mobile/Group/RuntimeRoot", 1));
    initializeIdentity();
    assert(!strcmp(nativeRunningBoardServicesPath, physical));
    const char *accepted[] = {runningBoardServicesInstallPath, physical};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        rememberNativeIdentityImage(header, 0x4000, accepted[i]);
        assert(!callerNeedsNativeIdentity(0x4fff) && callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff));
        assert(!callerNeedsNativeIdentity(0x5100) && !callerNeedsNativeIdentity(0x6000));
        assert(LSBGetPID() == 123 && LSBGuestPID() == 123 && LSBNativePID() == getpid());
        forgetNativeIdentityImage(header);
        assert(!callerNeedsNativeIdentity(0x5000));
    }
    const char *unrelated[] = {"/Plugins/RunningBoardServices",
        "/System/Library/Frameworks/RunningBoardServices.framework/RunningBoardServices",
        "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServicesHelper",
        "/System/Library/PrivateFrameworks/Other.framework/RunningBoardServices",
        "/System/Library/PrivateFrameworks/RunningBoardServices.framework/../RunningBoardServices",
        "/private/var/mobile/Group/RuntimeRoot-other/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices",
        "/OtherRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        rememberNativeIdentityImage(header, 0, unrelated[i]);
        assert(!callerNeedsNativeIdentity(0x1000));
    }
    const char *invalidRoots[] = {"/", "relative", "/RuntimeRoot/", "/RuntimeRoot:/OtherRoot", "/RuntimeRoot/../Other", "/Runtime//Root"};
    for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(invalidRoots[0]); ++i) {
        assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
        configureNativeImagePath(nativeRunningBoardServicesPath, runningBoardServicesInstallPath);
        assert(!nativeRunningBoardServicesPath[0]);
        rememberNativeIdentityImage(header, 0, physical);
        assert(!callerNeedsNativeIdentity(0x1000));
        rememberNativeIdentityImage(header, 0, runningBoardServicesInstallPath);
        assert(callerNeedsNativeIdentity(0x1000));
        forgetNativeIdentityImage(header);
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    configureNativeImagePath(nativeRunningBoardServicesPath, runningBoardServicesInstallPath);
    assert(!nativeRunningBoardServicesPath[0]);
}

static void testFrontBoardCallerIdentity(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *physical = "/private/Group/RuntimeRoot/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard";
    assert(!setenv("DYLD_ROOT_PATH", "/private/Group/RuntimeRoot", 1));
    initializeIdentity();
    assert(!strcmp(nativeFrontBoardPath, physical));
    const char *accepted[] = {frontBoardInstallPath, physical};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
        rememberNativeIdentityImage(header, 0x4000, accepted[i]);
        assert(!callerNeedsNativeIdentity(0x4fff) && callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff));
        assert(!callerNeedsNativeIdentity(0x5100) && !callerNeedsNativeIdentity(0x6000));
        assert(LSBGetPID() == 123 && LSBGuestPID() == 123 && LSBNativePID() == getpid());
        forgetNativeIdentityImage(header);
        assert(!callerNeedsNativeIdentity(0x5000));
    }
    const char *unrelated[] = {"/Plugins/FrontBoard", "/System/Library/Frameworks/FrontBoard.framework/FrontBoard",
        "/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoardHelper", "/System/Library/PrivateFrameworks/Other.framework/FrontBoard",
        "/private/Group/RuntimeRoot-other/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard",
        "/OtherRoot/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(*unrelated); ++i) {
        rememberNativeIdentityImage(header, 0, unrelated[i]);
        assert(!callerNeedsNativeIdentity(0x1000));
    }
    const char *invalidRoots[] = {"/", "relative", "/RuntimeRoot/", "/RuntimeRoot:/OtherRoot", "/RuntimeRoot/../Other", "/Runtime//Root"};
    for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(*invalidRoots); ++i) {
        assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
        configureNativeImagePath(nativeFrontBoardPath, frontBoardInstallPath);
        assert(!nativeFrontBoardPath[0]);
        rememberNativeIdentityImage(header, 0, physical);
        assert(!callerNeedsNativeIdentity(0x1000));
        rememberNativeIdentityImage(header, 0, frontBoardInstallPath);
        assert(callerNeedsNativeIdentity(0x1000));
        forgetNativeIdentityImage(header);
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    configureNativeImagePath(nativeFrontBoardPath, frontBoardInstallPath);
}

static void testUIFrameworkCallerIdentity(void) {
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const struct { const char *name, *install; char *configured; } frameworks[] = {
        {"UIKitCore", uiKitCoreInstallPath, nativeUIKitCorePath},
        {"FrontBoardServices", frontBoardServicesInstallPath, nativeFrontBoardServicesPath},
        {"BackBoardServices", backBoardServicesInstallPath, nativeBackBoardServicesPath},
        {"ExtensionFoundation", extensionFoundationInstallPath, nativeExtensionFoundationPath}
    };
    const char *root = "/private/var/mobile/Group/RuntimeRoot";
    for (unsigned framework = 0; framework < sizeof(frameworks) / sizeof(*frameworks); ++framework) {
        const char *name = frameworks[framework].name, *install = frameworks[framework].install;
        const char *directory = strstr(install, "/PrivateFrameworks/") ? "PrivateFrameworks" : "Frameworks";
        const char *otherDirectory = !strcmp(directory, "Frameworks") ? "PrivateFrameworks" : "Frameworks";
        char physical[PATH_MAX], unrelated[PATH_MAX];
        assert(snprintf(physical, sizeof(physical), "%s%s", root, install) < (int)sizeof(physical));
        assert(!setenv("DYLD_ROOT_PATH", root, 1));
        initializeIdentity();
        assert(!strcmp(frameworks[framework].configured, physical));
        const char *accepted[] = {install, physical};
        for (unsigned i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
            rememberNativeIdentityImage(header, 0x4000, accepted[i]);
            assert(!callerNeedsNativeIdentity(0x4fff) && callerNeedsNativeIdentity(0x5000) && callerNeedsNativeIdentity(0x50ff));
            assert(!callerNeedsNativeIdentity(0x5100) && !callerNeedsNativeIdentity(0x6000));
            assert(LSBGetPID() == 123 && LSBGuestPID() == 123 && LSBNativePID() == getpid());
            forgetNativeIdentityImage(header);
            assert(!callerNeedsNativeIdentity(0x5000));
        }
        const char *invalidPaths[] = {"/Plugins/%1$s", "/System/Library/%3$s/%1$s.framework/%1$s",
            "/System/Library/%2$s/%1$s.framework/%1$sHelper", "/System/Library/%2$s/Other.framework/%1$s",
            "/System/Library/%2$s/%1$s.framework/../%1$s", "/System//Library/%2$s/%1$s.framework/%1$s",
            "/System/Library/%2$s/%1$s.framework/./%1$s", "/System/Library/%2$s/%1$s.framework/%1$s/",
            "/private/var/mobile/Group/RuntimeRoot-other/System/Library/%2$s/%1$s.framework/%1$s",
            "/OtherRoot/System/Library/%2$s/%1$s.framework/%1$s"};
        for (unsigned i = 0; i < sizeof(invalidPaths) / sizeof(*invalidPaths); ++i) {
            assert(snprintf(unrelated, sizeof(unrelated), invalidPaths[i], name, directory, otherDirectory) < (int)sizeof(unrelated));
            rememberNativeIdentityImage(header, 0, unrelated);
            assert(!callerNeedsNativeIdentity(0x1000) && LSBGetPID() == 123);
        }
        const char *invalidRoots[] = {NULL, "", "/", "relative", "/RuntimeRoot/", "/RuntimeRoot:/OtherRoot",
            "/RuntimeRoot/../Other", "/Runtime/./Root", "/Runtime//Root"};
        for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(*invalidRoots); ++i) {
            if (invalidRoots[i]) assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
            else assert(!unsetenv("DYLD_ROOT_PATH"));
            initializeIdentity();
            assert(!frameworks[framework].configured[0]);
            rememberNativeIdentityImage(header, 0, physical);
            assert(!callerNeedsNativeIdentity(0x1000) && LSBGetPID() == 123);
            rememberNativeIdentityImage(header, 0, install); // Cached install paths do not depend on a physical-root alias.
            assert(callerNeedsNativeIdentity(0x1000));
            forgetNativeIdentityImage(header);
        }
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    initializeIdentity();
    assert(!nativeUIKitCorePath[0] && !nativeFrontBoardServicesPath[0] && !nativeBackBoardServicesPath[0] && !nativeExtensionFoundationPath[0] && LSBGetPID() == 123);
}

int main(void) {
    unsetenv("LSB_GUEST_PID");
    unsetenv("LSB_GUEST_PPID");
    initializeIdentity();
    assert(LSBGuestPID() == getpid() && LSBGuestParentPID() == getppid());
    setenv("LSB_GUEST_PID", "123", 1);
    setenv("LSB_GUEST_PPID", "456", 1);
    initializeIdentity();
    assert(LSBGuestPID() == 123 && LSBGuestParentPID() == 456 && LSBNativePID() == getpid());
    testNativeCallerRanges();
    testNotifydCallerIdentity();
    testBaseBoardCallerIdentity();
    testRunningBoardServicesCallerIdentity();
    testFrontBoardCallerIdentity();
    testUIFrameworkCallerIdentity();
    testTaskNameSelfTranslation();
    setenv("LSB_GUEST_PPID", "0", 1);
    initializeIdentity();
    assert(LSBGuestParentPID() == 0);
    assert(!LSBKill(123, 0) && !calls);
    assert(LSBKill(789, 0) == -1 && errno == ESRCH && calls == 1);
    const char *invalid[] = {"", "-1", " 123", "123x", "2147483648", "999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        setenv("LSB_GUEST_PID", invalid[i], 1);
        initializeIdentity();
        assert(LSBGuestPID() == getpid());
    }
    LSBChildStatus ledger;
    LSBChildStatusInitialize(&ledger);
    int wake[2];
    assert(!pipe(wake) && !fcntl(wake[0], F_SETFL, O_NONBLOCK));
    assert(!LSBChildWaitConfigure(&ledger, wake[0]));
    setenv("LSB_GUEST_PPID", "456", 1);
    initializeIdentity();
    assert(LSBGuestParentPID() == 456); // Unknown shared metadata falls back to trusted startup metadata.
    assert(!LSBChildStatusSetParentPID(&ledger, 789) && LSBGuestParentPID() == 789);
    assert(!LSBChildStatusSetParentPID(&ledger, 890) && LSBGuestParentPID() == 890); // No constructor rerun after adoption.
    assert(!LSBChildStatusSetParentPID(&ledger, 0) && LSBGuestParentPID() == 0);
    close(wake[0]);
    close(wake[1]);
    puts("ProcessIdentity: stable PID/PPID, native self task lookup/signal, fallback and range validation passed");
    return 0;
}
