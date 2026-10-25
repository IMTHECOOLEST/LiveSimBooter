#ifndef LSB_CHILD_WAIT_H
#define LSB_CHILD_WAIT_H

#include "../ProcessSupport/ChildStatus.h"
#include <signal.h>

int LSBChildWaitConfigure(LSBChildStatus *ledger, int wakeFD);
int LSBGuestChildWakeFD(void);
pid_t LSBWaitPID(pid_t pid, int *status, int options);
pid_t LSBWait(int *status);
int LSBSigaction(int number, const struct sigaction *action, struct sigaction *oldAction);
sig_t LSBSignal(int number, sig_t handler);

#endif
