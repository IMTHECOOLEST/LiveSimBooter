# Simulator launchd

The opt-in `--launchd` host argument starts the matching iOS 26 (23A343) `sbin/launchd_sim` through a C LiveProcess.
The ordinary app launch still starts interactive Fish. The first bootstrap intentionally has no service search paths;
booting this empty instance is a separate checkpoint from booting the runtime's full daemon catalog.

The native host writes `SimulatorData/var/run/LiveSimBooter-launchd-bootstrap.plist` beside the app group's RuntimeRoot.
It supplies the runtime/data environment, a regular-file stdout/stderr at
`SimulatorData/Library/Logs/LiveSimBooter-launchd.log`, and `/dev/null` stdin. Stock launchd refuses terminal-backed stdout,
including the console inherited from a CoreDevice launch. The configuration is generated independently of any Mac simulator device.

## Replacing the trampoline and holding tank

The boot-chain reference is [Sim26Booter](https://github.com/khanhduytran0/Sim26Booter), especially its `SimBootProcess.md`
and trampoline hook. Here the native broker starts launchd directly, with no `execve` and no trampoline process.

For the pinned launchd binary, the tank transaction is Mach MIG message 708, not an XPC dictionary. The broker allocates
both a tank endpoint and launchd's future service port. An optional Mach send right in the existing synchronous XPC bootstrap
reply supplies the tank endpoint to LiveProcess. After its final native check-in, LiveProcess installs that endpoint as its
bootstrap port and enters simulator dyld.

Launchd sends a plain 24-byte request. `LaunchdHandshake.c` checks the complete kernel audit token against the extension's
check-in identity and replies with message 808, a 40-byte complex message carrying one `MOVE_RECEIVE` descriptor. The broker
retains a send right to the service port. A dispatch cancellation handler closes the tank only after its Mach receive source
is retired; job disposal releases the service right.

Stock launchd next calls `waitpid` using `XPC_SIMULATOR_HOLDING_TANK_HACK`. This implementation supplies `-1` while the initial
broker child ledger is empty. The shim returns `-1/ECHILD`, and this version of launchd compares that return value directly
with the parsed selector without checking success. It proceeds because they match. This is a pinned compatibility sentinel,
not a real tank PID or evidence that a tank process exited. A launchd update requires rechecking this sequence.

## Workqueues after native cache removal

An earlier libpthread registration survives in the kernel. The existing suspended-pthread adapter can replace each ordinary
thread's entry PC, but kernel workqueue threads start asynchronously at the registered native `start_wqthread` address.
Simulator libpthread also receives `EINVAL` from its second registration and consequently exposes an empty feature mask.

Before detachment, LiveProcess captures the native workqueue entry address and actual `_pthread_workqueue_supported()` mask.
The guest adapter uses these only after validating the known duplicate-registration ABI. On the first `__workq_open`, it
starts one simulator pthread and installs a Mach exception handler. It redirects only an instruction fault at the exact old
workqueue entry, with the expected workqueue argument shape, to simulator `start_wqthread`. Other register state is retained;
only the PC and its signing flag change. Other faults are forwarded to the previously installed exception handlers, including
LLDB. The handler uses Mach messages and no Objective-C or dispatch queue. Programs that never open workqueues do not start
this server thread.

Attach LLDB before the handoff for this experiment. Attaching a debugger later can replace the task exception handler. An
exception inside the exception-server thread cannot be handled by that same thread; a previously installed debugger handler
is retained for it when available.

## Initial findings

On the iOS 26.1 test phone, the first file-backed launch reached configuration parsing and reported `no bootargs string`.
The launchd-only `kern.bootargs` read adapter supplies an empty string; unrelated sysctls and writes remain unchanged.
During that fatal log path, LLDB showed the exception server inside simulator `mach_msg` and a workqueue thread inside
simulator `__workq_kernreturn`, after the host cache had been removed.

After the empty bootargs response, the next hardware breakpoint caught `could not create temporary state directory`.
The stock path is `/private/var/tmp/com.kdt.LiveSimBooter.launchd`; setting `TMPDIR` does not change it.
The first shim draft also exposed an early-initialization constraint: libSystem calls unrelated `sysctlbyname` queries before
`getenv` is usable. The adapter now checks the query name and write parameters before touching environment or errno state.

The scoped state adapter maps that exact directory and its descendants to `SimulatorData/tmp/launchd`. It covers mkdir,
open, fopen, stat, unlink and renamex_np; unrelated paths are unchanged. It rejects traversal components and overlong translated
paths. Legacy syslog still attempts its original Unix socket path and can fail nonfatally; no process-wide chdir or socket
namespace emulation is introduced.

On 2026-10-25, PID 79886 completed the tank handoff and reached the hardware breakpoint at launchd's `dispatch_main` call.
After continuing, the original main thread exited. LLDB showed the simulator exception server, a simulator workqueue thread,
and libdispatch's simulator signal thread. A direct legacy MIG 404 lookup sent to the retained simulator service port returned
`BOOTSTRAP_UNKNOWN_SERVICE` (1102), with the exact launchd audit token verified. The host reported `error=0 bootstrap=1102`.
This verifies an answering empty launchd instance, rather than just successful mapping or a breakpoint before the event loop.

The native-cache address inspected during the same stop lay in `[0x180000000, 0x300000000)` with `---` protection; the relocated
simulator cache at `0x120000000` was `r-x`, with zero dirty pages reported. LLDB discovered 48 modules. Evidence is in
`tmp/launchd-boot/console-state.log`, `lldb-live-service.txt` and `guest-state.log`.

## Main-thread exit and final validation

Native libpthread also consumes the apple-vector `main_stack` entry. The loader now captures `pthread_get_stackaddr_np` and
`pthread_get_stacksize_np` before detachment, verifies that the native main thread's current frame is inside those bounds,
and supplies a fresh writable `main_stack=0xTOP,0xSIZE,0x0,0x0`. Without this, simulator pthread can assume the macOS default
stack size when `dispatch_main` calls `pthread_exit`. No native pthread storage or allocation ownership is copied.

The final LLDB run (PID 79895) captured a real stack size of `0xfc000`, with bounds `[0x16ae5c000, 0x16af58000)`. Simulator
pthread's fields matched those exact bounds before `dispatch_main`, with zero allocation metadata. After the main thread
exited, `[0x16ae5c000, 0x16af50000)` was inaccessible and the two upper pages remained mapped. The authenticated lookup again
returned 1102. Evidence is in `tmp/launchd-boot/lldb-final.txt` and `console-final-lldb.log`.

The final full host test suite and signed device build pass. The same updated shim also passed the established Fish command
path: Fish launched `./bin/df`, waited for status 0, printed `LAUNCHD_SHIM_FISH_OK`, and exited 0. This uses RuntimeRoot-relative
paths because the runtime still has no general chroot. Evidence is in `tmp/launchd-boot/console-fish-final.log`.
A fresh run without LLDB (PID 79911) again returned `error=0 bootstrap=1102`; it was left running with the host foregrounded.
Its evidence is `tmp/launchd-boot/console-final-normal.log` and `guest-final.log`. The SSH capture in `security.log` contained
no amfid rejection or kernel TXM event during these runs. The capture used the configured test phone; these results do not
establish ordinary stock provisioning or full simulator-service support.

## Reproducing the empty bootstrap

Deploy the matching signed `sbin/launchd_sim`, the rebuilt guest shim, and the existing working iOS 26 runtime. Build the app
normally. To enable the bounded native health lookup, launch with:

```sh
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  --environment-variables '{"LSB_LAUNCHD_HEALTHCHECK":"1"}' -- com.kdt.LiveSimBooter --launchd
```

For LLDB, also set `LSB_DEBUG_STOP_BEFORE_HANDOFF=1`, attach to LiveProcess, and use hardware breakpoints. The probe starts five
seconds after the tank transfer and waits at most one second; a debugger pause can make it time out without implying a runtime
failure. It sends a deliberately missing lookup directly to the simulator service right. Native `bootstrap_look_up()` uses
libxpc's cached bootstrap pipe and would not establish that this simulator instance answered.

The bootstrap has an empty `Bootstrap.Paths`. This does not yet establish daemon loading, simulator XPC endpoint propagation
to spawned services, `xpcproxy_sim`, a full system boot, or generic execve. These remain separate work after the launchd baseline.
