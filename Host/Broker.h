#ifndef LSB_BROKER_H
#define LSB_BROKER_H
// Start once on the original main thread. The application's event loop continues processing broker callbacks.
// Returns zero after submitting the initial guest, or an errno value if startup fails.
int LSBStartHost(int argc, const char *argv[], const char *envp[]);
#ifdef __OBJC__
@class LSBIndigoHIDServer;
@class NSString;
extern NSString *const LSBHostIndigoDidChangeNotification;
int LSBHostUsesSimulatorDisplay(void);
// Main-thread access. The display view forwards only its own touches to this simulator-local endpoint.
LSBIndigoHIDServer *LSBHostIndigoHIDServer(void);
#endif
#endif
