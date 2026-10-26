#include <sys/types.h>
#include <unistd.h>

// A real separate Mach-O caller proves that the return address is attributed to the loaded image, not the test executable.
__attribute__((visibility("default"), noinline)) pid_t LSBIdentityFixturePID(void) {
    volatile pid_t pid = getpid();
    return pid;
}

// The stock FrontBoard duplicate-source branch compares the RBS handle's native PID with its direct getpid() call.
__attribute__((visibility("default"), noinline)) int LSBIdentityFixtureIsSelf(pid_t handlePID) {
    return handlePID == getpid();
}
