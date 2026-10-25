#pragma once

int LSBRunHostApplication(int argc, const char *argv[], const char *envp[]);
// The first native view starts the configured guest once, after broker initialization.
int LSBStartInitialGuest(void);
