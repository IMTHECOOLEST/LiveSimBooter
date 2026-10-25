#ifndef LSB_BROKER_H
#define LSB_BROKER_H
// Start once on the original main thread. The application's event loop continues processing broker callbacks.
// Returns zero after submitting the initial guest, or an errno value if startup fails.
int LSBStartHost(int argc, const char *argv[], const char *envp[]);
#ifdef __OBJC__
@class LSBIndigoHIDServer;
@class LSBFramebufferServer;
@class NSString;
extern NSString *const LSBHostIndigoDidChangeNotification;
extern NSString *const LSBHostFramebufferDidChangeNotification;
int LSBHostUsesSimulatorDisplay(void);
// Main-thread access. The display view forwards only its own touches to this simulator-local endpoint.
LSBIndigoHIDServer *LSBHostIndigoHIDServer(void);
// Main-thread access. Bind the presentation callback before the next backboardd authorization.
LSBFramebufferServer *LSBHostFramebufferServer(void);
#endif
#endif
