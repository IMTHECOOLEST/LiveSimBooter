#include <mach/mach.h>
static kern_return_t testTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task);
#define task_name_for_pid testTaskNameForPID
#define LSB_PROCESS_IDENTITY_TESTING
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
