# Guest creation mask

Darwin 25 does not implement the `getumask` syscall exposed by newer Simulator libraries. The guest shim therefore captures the inherited process mask in its constructor, before Fish starts worker threads. It blocks catchable signals, requires a single thread, briefly calls the native `umask(0)` and restores the returned mask. A failed thread check leaves reads unavailable until an intercepted `umask` call supplies a known mask.

The shim interposes `umask` and maintains a process-local shadow. Updates and spawn reads share a small atomic lock. Each caller blocks catchable signals on its thread before taking the lock and restores its previous signal mask after releasing it. A signal handler can therefore call the interposer without interrupting a locked section on the same thread. The wrapper applies the requested mask through the original function, returns its previous value, and preserves the caller's `errno`.

An intercepted `umask` call made by an earlier dependency initializer also establishes the shadow; the constructor preserves that value. After initialization, reads do not temporarily change the kernel mask or require a single thread. If signal blocking or restoration fails during tracking, subsequent spawn reads fail instead of using a potentially stale shadow.

On Darwin 26 and later, the shim resolves the native `getumask` once and keeps using it. The actual kernel release controls this choice; the Simulator SDK or exported symbol alone cannot establish syscall availability.

Direct `umask` syscalls, inline `svc` instructions, and calls that bypass dyld interposition do not update the Darwin 25 shadow. Those paths remain unsupported. This also does not make a caller's own two-call `umask` read operation atomic: other threads can still observe the caller's intermediate mask. Generic `fork` remains unsupported and would need explicit handling of inherited lock state.

`Tests/CreationMaskTests.c` checks early initialization, an initializer that calls `umask` first, real multithreaded initialization rejection and recovery, eight concurrent readers/writers, delayed signal-handler reentry, tracking failures, native-query selection, and `errno` preservation.
