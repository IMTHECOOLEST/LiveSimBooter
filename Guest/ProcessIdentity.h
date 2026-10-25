#ifndef LSB_PROCESS_IDENTITY_H
#define LSB_PROCESS_IDENTITY_H

#include <sys/types.h>
#include <mach/mach.h>

// Native process identity for kernel-facing APIs; the guest PID may survive a replacement process.
pid_t LSBNativePID(void);
pid_t LSBGuestPID(void);
// getpid interposer: preserves native identity for direct calls from libxpc, whose audit tokens stay kernel-authentic.
pid_t LSBGetPID(void);
// Translate only a guest self lookup in the current task; the kernel still supplies the actual task-name right and result.
kern_return_t LSBTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task);
// Parent metadata is refreshed from the shared ledger so adoption does not require changing the child environment.
pid_t LSBGuestParentPID(void);

#endif
