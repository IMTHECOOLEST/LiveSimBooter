# Simulator processes through LiveProcess

The native `LiveSimBooter` app now has a minimal UIKit foreground window and acts as a launch broker. Every simulator program
runs in a separate `LiveProcess.appex` instance. The implementation intercepts `posix_spawn`, `posix_spawnp`, `wait`, `waitpid`, and `kill`; it does
not implement a complete POSIX process tree or boot `launchd_sim`.

The matching iOS 26 runtime remains the target. The spawn attribute and file-action decoder is pinned to the arm64 Darwin ABI
audited against that runtime's host-support libkernel. Recheck those layouts before using another runtime.

## Launch path

1. [The native view](../Host/ViewController.m) starts the initial test once from `viewDidLoad`. [The broker](../Host/Broker.m)
   locates the shared runtime, registers `group.com.kdt.LiveSimBooter.spawn` with `bootstrap_check_in`, starts its receiver,
   and then submits the initial executable as a spawn plan. Arguments after the host executable name select the initial
   guest; without arguments it selects `/bin/spawn-probe`. The actual `posix_spawn` call occurs inside that simulator guest.
2. [The launch adapter](../Host/ExtensionLauncher.m) uses `NSExtension` to discover `com.kdt.LiveSimBooter.LiveProcess`. It obtains
   the underlying `PKHostPlugIn` and creates a fresh instance UUID. `PKHost readyPlugIns:...ready:` asks pkd to launch that instance
   and returns its PID. Its optional registered-plugin UUID array is checked when present; the device normally omits it.
   The adapter retains an extension process assertion using the same parameters as PlugInKit's startup path: flags 7, reason 9,
   name `com.apple.extension.session`.
3. [The C extension entry](../LiveProcess/main.c) calls `launch_extension_check_in_live_4UIKit()`, a synchronous libxpc SPI that
   notifies launchd the extension is alive. This is a C API call; the child does not create a UIKit application, NSXPC listener,
   or Objective-C extension context. It then checks that it is single threaded and contacts the broker through a synchronous XPC pipe.
4. The broker matches the kernel audit PID to the PID from pkd's launch reply. Check-in retries are bounded while that reply is
   pending. It binds the kernel audit PID generation to the job, preventing another extension instance or a reused PID from
   acquiring that job's plan. No data is delivered before the launch PID matches. This does not rely on a child-supplied PID or
   a cross-process code-signature query.
5. Before delivering the plan, the broker registers an exit monitor. The child receives owned strings, scalar attributes,
   ordered actions, XPC FD objects, a shared child-status file, and a wake-pipe reader. It restores descriptors and process
   state, resolves the executable within RuntimeRoot, and maps it alongside `usr/lib/dyld_sim`.
6. The native child retains one raw Mach send right to the broker and passes its numeric name in `LSB_BROKER_PORT`, replacing
   any inherited value. A READY exchange reports preparation success or its errno. After releasing native XPC objects, the child
   checks its thread count, performs the existing fresh-TSD/native-cache handoff, and enters dyld_sim. The loader checks again immediately before
   detachment. READY means preparation reached that point; it does not prove that dyld_sim or the guest program subsequently ran.
7. The child inserts [libLiveSimSpawn.dylib](../Guest/SpawnShim.c) into the simulator environment. Its interposition tuples replace
   public `posix_spawn` and `posix_spawnp`; a later guest spawn follows the same broker/extension path. Its wait interposer
   reads the shared status table directly, without guest XPC or a helper thread.

The host calls declared private methods using normal Objective-C syntax. The narrow declarations live in
[FoundationPrivate.h](../Host/Private/FoundationPrivate.h), [PlugInKitPrivate.h](../Host/Private/PlugInKitPrivate.h), and
[RunningBoardPrivate.h](../Host/Private/RunningBoardPrivate.h).
[HostApplication.m](../Host/HostApplication.m) enters `UIApplicationMain`, supplies app/scene delegates and a native window,
and retains the original command-line arguments for the view's one-time startup. Broker initialization returns without
blocking, so UIKit owns the original main thread's run loop. The native host retains its cache. Keep the app foregrounded
while programs run; it has no independent background execution assertion. The active host scene disables the display idle
timer and restores it when the scene resigns active. This prevents automatic screen sleep while testing in the foreground;
it does not prevent manual locking or keep the host running in the background.

The earlier headless FrontBoard workaround is preserved in [unused/HeadlessHost](../unused/HeadlessHost). It could complete a
short simulator df run, but a live process was not necessarily runnable: RunningBoard subsequently suspended it. Attaching
LLDB to that native host alone did not prevent suspension. Foreground activation then failed while decoding missing UIKit
scene classes. The native UIKit shell replaces that workaround and has now passed the foreground device test described below.

The child has no Objective-C source, extension principal class, `NSExtensionItem`, Foundation extension context, or call to
Foundation's `NSExtensionMain`. Its own C function has that entry-point name because the appex linker selects it. The built child
links only libSystem and libc++, with no Objective-C imports or `__objc` sections. This describes the child's own code and
imports; it is not a claim that Apple's implementation of each C library API never uses Objective-C internally. Existing C++
loader preparation is shared.

The extension uses LiveContainer's `com.apple.ar.viewer` extension point, `FALSEPREDICATE` activation rule, and application-type
XPC service with `_MultipleInstances = true`. The adapter deliberately stops at the pkd launch operation, before PlugInKit's
`beginUsing`/NSXPC Hub handshake. The broker's checked C handshake supplies readiness. Release invalidates the retained process
assertion and stops callbacks; the broker's process-exit watcher releases the corresponding job. The local iOS 26 RunningBoard
implementation rewrites ViewService reason 9 to Extension reason 13 for an XPC-service target and marks both reasons with
`suspendsOnOriginatorSuspension`. The retained child assertion therefore does not provide independent execution after the host
suspends. Long-running assertion and extension behavior remain device checks.

## Shared runtime and signing

Both targets require the app group `group.com.kdt.LiveSimBooter`. The broker resolves the group's actual container using
`containerURLForSecurityApplicationGroupIdentifier:`; it does not hard-code a container UUID.

The layout is `<app-group-container>/RuntimeRoot`. On startup, if that directory does not exist and the old
`Documents/RuntimeRoot` does, the host moves the old directory into the group. An existing group runtime wins; the migration
does not merge or overwrite it. Migration failure stops startup. The resolved directory is canonicalized before sending its
path to a child.

Both app and extension provisioning profiles must authorize their own application identifier and the same app group. The host
keeps Increased Memory Limit. The initial child profile lacked that entitlement, so the child currently omits it; it must not
be added to its signature without a matching profile. The signed build was verified with certificate SHA-1
`D7610EDC80D34A9B1A3D3C8563D66A5CD4716293`, team `UU7W52U79B`, matching embedded profiles, and `get-task-allow` for both targets.
No machine-specific provisioning UUID is stored in the project.

The runtime still needs appropriately signed dyld_sim, cache, executable, and shim files. Sharing the directory does not alter
their code signatures. Use the existing device-compatible `df` signing procedure for a new executable such as `spawn-probe`;
ordinary executable signing can reintroduce the executable-segment/profile issue documented in the earlier loader experiments.

## Synchronous XPC and native-cache removal

[SpawnTransport.c](../ProcessSupport/SpawnTransport.c) declares the private C pipe APIs in
[PrivateXPC.h](../ProcessSupport/PrivateXPC.h). It uses `bootstrap_look_up`, `xpc_pipe_create_from_port`, `xpc_pipe_routine`,
`xpc_pipe_receive`, and `xpc_pipe_routine_reply`. The receive ABI has **three** arguments:
`int xpc_pipe_receive(mach_port_t port, xpc_object_t *message, uint64_t flags)`. The implementation passes flags 0; it does not
pass a separate reply-port argument. The native child starts no dispatch queue or `xpc_connection` event handler.
The host can use dispatch and Foundation because it retains its native cache.

Eleven fresh macOS 27.0.1 ARM64 clients completed a separate pipe experiment with exactly one thread in all eight snapshots:
entry, port discovery, pipe creation, request creation, response, release, 500 ms later, and 1000 ms later. The experiment also
verified transferred descriptor offsets remain shared and that the returned audit PID identifies the actual client. A separate
bootstrap check-in/lookup experiment passed. Local evidence is under `tmp/multiprocess-reference/xpc-probe/`, including its
README, eleven run logs, and `macos-results.json`.

Those observations establish neither an API guarantee nor the absence of a transient worker between snapshots. The
[single-thread guard](../ProcessSupport/SingleThreadGuard.h) takes two snapshots and returns `EBUSY` if another thread is present.
It does not suspend workers or prevent a later one from appearing. Native XPC pipes, requests, replies, and their temporary
Mach rights are released before handoff. One independent broker send right intentionally survives: it is a kernel right in the
same task, with no native-cache object or function pointer attached. After detachment, the shim validates the numeric name from
`LSB_BROKER_PORT` as a send right, increments its reference count for the request, creates a new pipe with simulator libxpc,
and releases that temporary reference afterward. Each new extension obtains its own right during native preparation, so a
parent's task-local port number is never reused blindly in another task.

The first simulator spawn probe reached its interposed `posix_spawn` but returned `EIO` during service discovery. The pinned
simulator libxpc disables its launchd bootstrap lookup when `XPC_SIMULATOR_LAUNCHD_NAME` is missing. Supplying the native broker
right avoids that lookup without pretending a simulator launchd exists. Disassembly shows the direct pipe path does not take
the disabled-lookup branch; the guest-mode transport tests also pass with `XPC_FLAGS=0x2`.

The direct pipe exposed a second incompatibility: simulator and native libxpc use different graph markers despite both using
wire version 5. The simulator marker is `0x7870633f`; native iOS uses `0x40585043`. The native iOS 26 and 27 libraries both use
the native marker, so this is a platform distinction. A native receiver rejects the simulator marker with `EIO` and releases
the reply send-once right. The client's Mach receive succeeds with `MACH_NOTIFY_SEND_ONCE` (message ID `0x47`), which libxpc
then reports as `EIO`. Disassembly and a local reproduction confirmed this sequence; it does not require a failed Mach syscall.

[XPCWireCompatibility.c](../Guest/XPCWireCompatibility.c) now adapts that marker in the inserted guest shim. `callBroker`
brackets only its synchronous `xpc_pipe_routine` with `LSBXPCWireBegin` and `LSBXPCWireEnd`. The scoped `mach_msg` interposer
translates simulator-to-native requests to the broker and native-to-simulator replies on the matching reply port. It preserves
the version, message options, descriptors, and payload. Unrelated destinations and other threads remain unchanged; no cache
instructions or global libxpc state are patched.

The adapter validates message sizes, graph version, descriptor types/order, and inline or out-of-line graph bounds before
changing the marker. OOL header access uses checked VM reads/writes rather than dereferencing an arbitrary address. Its fixed
atomic context table uses no implicit thread-local allocation, and an inactive fast path avoids touching thread state during
early runtime startup. A reentry guard passes the adapter's own VM-copy MIG traffic through unchanged.

The inserted-dylib tests in [SpawnTransportTests.c](../Tests/SpawnTransportTests.c) and
[XPCWireCompatibilityTests.c](../Tests/XPCWireCompatibilityTests.c) use fresh client processes and a separate native XPC peer.
They cover inline and OOL graphs, a 70 KB payload with 128 transferred FDs, shared descriptor offsets, receive-buffer retries,
malformed/unmapped/read-only OOL addresses, and isolation from unrelated ports and threads. A test-only marker substitution
models the simulator wire format on macOS; the native peer's libxpc remains untouched. These tests run through `Tests/run.sh`.

The complete adapted path also passed on the iPhone: a simulator parent spawned two df children, each returning 2,268 bytes
through its stdout pipe followed by EOF. A separate LLDB run stopped at both `LSBXPCWireBegin` and `LSBXPCWireEnd` with exactly
one thread, completed another spawn, and exited 0. The captured logs showed no matching wire, Mach, AMFI, or TXM failure.
See the device-progress evidence below. This verifies the tested synchronous path; it does not establish safety for arbitrary
asynchronous XPC or dispatch work across native-cache removal.

The broker gives a started launch 30 seconds to reach READY. If a launch times out before being bound to a child, the broker
rejects subsequent launches until the host restarts, preventing a late child from receiving another job's arguments. At most
16 jobs may be queued, starting, or running. The private synchronous pipe routine has no documented per-call timeout: a stalled
broker or transport can still block a caller. The launch timer is not a general IPC deadline.

## Supported spawn state

The wire plan contains offsets into owned strings, never caller pointers or an opaque spawn-attribute blob. XPC FD objects
carry open file descriptions; their original descriptor numbers and `FD_CLOEXEC` flags are transported separately.

| State | Current behavior |
| --- | --- |
| Executable, argv, envp | Copied with bounds checks; `posix_spawnp` searches the caller's PATH, defaulting to `/usr/bin:/bin`. |
| Current directory and umask | Captured from the guest and applied before ordered file actions. |
| Descriptor inheritance | Captures open descriptors, including CLOEXEC descriptors needed by actions; transfers shared open descriptions. |
| File actions | OPEN, CLOSE, DUP2, INHERIT, CHDIR, and FCHDIR are replayed in order. |
| Close-on-exec | Applied after actions; `POSIX_SPAWN_CLOEXEC_DEFAULT` retains only explicit action destinations/inherited descriptors. |
| Signals | Carries the mask and ignored dispositions; resets caught handlers and honors SETSIGMASK/SETSIGDEF. |
| RESETIDS | Applies the extension's real UID/GID through the normal kernel APIs. |
| SETPGROUP | Attempts `setpgid`; propagates kernel errors when the extension's launch session prevents the request. |
| Unsupported attributes/actions | Returns `ENOTSUP`, including unknown file actions and nondefault private attribute fields. |

FD imports are first staged above every destination and action FD, so restoring one descriptor cannot clobber another imported
source. The broker closes its imported FD numbers after dispatching the plan and releases the original XPC request, which
also owns FD objects, when it replies at READY. Keeping either pipe writer would prevent the guest parent from seeing EOF.
A snapshot of a multithreaded caller's FD table is not atomic with concurrent close/open calls.

Current limits are 256 argv entries, 256 environment entries, 64 file actions, 128 transferred descriptors, and 49,152 bytes of
combined string storage. Executable/cwd/action paths must fit within 1,024 bytes including their terminator. The 128-FD limit
counts open CLOEXEC descriptors too. Count/string overflow and descriptor exhaustion return errors before launch.

Executable paths are resolved under RuntimeRoot and canonical paths escaping it are rejected. This is not a `chroot`: open/chdir
file actions operate on the actual sandbox filesystem, and general guest filesystem-path virtualization is not implemented.
The child replaces incoming `DYLD_INSERT_LIBRARIES`, `LSB_BROKER_PORT`, `LSB_CHILD_STATUS`, and `LSB_CHILD_WAKE_FD` values with
its own resources. Interposition still depends on dyld's captured AMFI policy; `DYLD_PRINT_INTERPOSING=1` must show actual interposition.

Returned PIDs identify real extension processes, but the simulator caller is not their kernel parent. Parent-PID virtualization,
fork, exec replacement, and full session/process-group inheritance remain unimplemented. A default child retains its system
launch session/group; guest waiting is provided by the separate mechanism below.

## Signaling a simulated child

[ChildKill.c](../Guest/ChildKill.c) interposes `kill`. The initial scope is exact positive PIDs belonging to the caller's direct
simulated children. An exact self PID calls the simulator's original `kill` implementation without broker IPC. Other positive
PIDs go through synchronous XPC to the authenticated broker; it rejects unknown, unrelated, and completed child jobs with
`ESRCH`. Nonpositive selectors, including process groups, `0`, and `-1`, return `ENOTSUP`. Signals outside `[0, NSIG)` return
`EINVAL`. Successful broker calls return 0 and preserve `errno`.

The broker validates the target's full audit identity against RunningBoard before signaling. This protects against a retained
job whose PID has already exited or been reused. `kill(child, 0)` performs only this identity check; it sends no signal, because
the private audited signal API does not accept signal 0. For real signals the host first uses `proc_signal_with_audittoken`.
An `EACCES` or `EPERM` response selects the same retained extension-instance UUID fallback used for SIGCHLD: launchd receives
the request through `xpc_connection_kill`. The fallback targets that instance, not a freshly looked-up process with the same PID.
An accepted launchd request is asynchronous; the child's exit monitor and `waitpid` establish the resulting terminal status.

The guest's cross-process `kill` implementation is **not async-signal-safe**: it allocates XPC objects and performs a synchronous
broker call. It starts no guest worker and needs the foreground host to remain runnable. It does not provide process-group
signaling, arbitrary-process signaling, or `sigqueue` payloads. Stop/continue signals do not add stopped/continued wait events;
the current waiting implementation still reports terminal statuses only.

## Child status, waiting, and SIGCHLD

The native broker does not reap these extension instances with a native `waitpid` syscall: they are launched through
PlugInKit/launchd, and their parent/child relationship is maintained by the broker. The guest `waitpid` reads the shared
status ledger described below. Direct audited signaling is sandbox denied on the tested phones, so the host delegates
delivery to launchd using the retained extension instance. Hosting an extension does not grant ordinary parent-process
`waitpid` or signal permissions.

Each guest receives a 4 KiB shared mapping with 16 child slots and a nonblocking wake-pipe reader. The host creates the backing
file inside the App Group, unlinks it immediately, and transfers its descriptor at check-in. Native preparation maps it, closes
the file descriptor, and preserves the reader outside file-action destinations and close sweeps. The mapping and FD survive
cache detachment; no native object or callback is retained. The spawn shim excludes this private reader from ordinary FD inheritance.

[ChildStatus.c](../ProcessSupport/ChildStatus.c) uses lock-free atomics and generation-tagged slots. A spawn reserves a slot in
its simulated parent's table; READY publishes the real PID and current group before releasing the child. A failed spawn
abandons the reservation. On exit the host publishes the raw wait status, writes a wake hint, and requests `SIGCHLD`. The guest
claims each status once with compare-and-swap; the host can then reuse the slot. Completed statuses survive removal of host job
records. Parent death cancels pending child launches and detaches running orphans, whose own exit monitors remain active.

[ChildMonitor.c](../Host/ChildMonitor.c) first tries `EVFILT_PROC` with `NOTE_EXIT | NOTE_EXITSTATUS`. macOS tests collect exact
statuses for non-child processes, but the iPhone sandbox rejects registration because XNU checks permission to send `SIGKILL`.
The host therefore falls back to [RunningBoardMonitor.m](../Host/RunningBoardMonitor.m), using an audited PID-generation handle
and hosted-process death subscription. Its `legacyCode` contains the raw wait4 status. A voluntary exit has a timestamp and
type 1 even though its modern status object is nil; abnormal exits use type 2 with a valid status. Empty synthesized contexts
and malformed values are rejected rather than converted to exit 0.

Direct audited signaling is also denied on the tested phone. Apple's
[libproc wrapper](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.c) returns a positive
errno on failure, which the host preserves. For `EACCES`/`EPERM`, the launch adapter asks launchd to signal the retained extension
instance UUID through `xpc_connection_kill`. It does not resume an extension NSXPC service connection. A successful submission
does not itself prove delivery; the device probe checks its handler. These notifications originate from the broker/launchd,
so `siginfo_t` does not describe the simulated child with normal `CLD_*` provenance.
The temporary native connection is canceled and activated in that canceled state before release, satisfying libxpc's lifecycle
requirements without starting a peer handshake.

[ChildWait.c](../Guest/ChildWait.c) supports exact PID, any child (`-1`), current group (`0`), and explicit negative-group selectors,
plus `WNOHANG` and `wait()`. It preserves the raw status for `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`, and `WTERMSIG`. A second wait
returns `ECHILD`; `WNOHANG` returns 0 without changing the status output when a matching child is still live. Blocking waits use
the pipe only as a hint and rescan every 100 ms, so another waiter draining it cannot lose an exit. Waiting performs no XPC and
starts no worker thread. Nonblocking reaping uses lock-free operations and is exercised from a signal handler.

The current limits are:

- `WUNTRACED` and `WCONTINUED` return `ENOTSUP`; other unknown option bits return `EINVAL`. Stop/continue events and `waitid` are unsupported.
- Process-group selectors use the group captured at READY. Later guest `setpgid` changes are not reflected in stored child records.
- A signal may interrupt a blocking wait with `EINTR` even with `SA_RESTART`; callers must retry. A status already available after interruption is returned first.
- Interposed `sigaction`/`signal` mirror `SIG_IGN` and `SA_NOCLDWAIT` into the table and discard unreaped/future statuses.
  Concurrent changes to the SIGCHLD disposition must be serialized by the caller.
- This models the broker's simulated children only. It does not merge native kernel-child waits, synthesize child `siginfo_t`, or virtualize sessions and parent PIDs.

## Build and exercise

From the repository root:

```sh
sh Tests/run.sh
sh Tools/build-spawn-shim.sh
xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Debug \
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath tmp/multiprocess-build \
  CODE_SIGNING_ALLOWED=NO build
```

The shim builder produces `tmp/multiprocess/libLiveSimSpawn.dylib` and `tmp/multiprocess/spawn-probe` for arm64 iOS Simulator.
After signing them using the runtime's established rules, place them at `RuntimeRoot/usr/lib/libLiveSimSpawn.dylib` and
`RuntimeRoot/bin/spawn-probe`. The view starts this probe by default; an explicit first argument selects another guest.
Launch the native host into the foreground:

```sh
xcrun devicectl device process launch --device iphone-0x11-cua-duy --terminate-existing --console \
  -- com.kdt.LiveSimBooter /bin/spawn-probe
```

The phone must be unlocked for the launch. `/bin/spawn-probe` is now the default; pass `/bin/df` explicitly for the earlier
single-program test. Keep the app window foregrounded during debugging pauses and longer runs. A debugger attachment alone
does not establish that the native broker remains runnable, and a headless launch has no guaranteed background grace period.

For the probe, [sign-sim-executable.py](../Tools/sign-sim-executable.py) preserves `MH_EXECUTE`, clears
`CS_EXECSEG_MAIN_BINARY`, and signs the updated CodeDirectory with the selected certificate. It verifies the code pages and
CMS and refuses to overwrite an existing output. For example:

```sh
python3 Tools/sign-sim-executable.py --source tmp/multiprocess/spawn-probe --output tmp/multiprocess/signed/spawn-probe \
  --identity D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter
codesign --force --sign D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --timestamp=none \
  --identifier com.kdt.LiveSimBooter tmp/multiprocess/libLiveSimSpawn.dylib
```

Those identifiers match the existing runtime signing set. The tested dyld_sim, cache, df, and shim were accepted from the
extension's App Group path. A newly signed executable still needs its own device validation.

[SpawnProbe.c](../Guest/SpawnProbe.c) spawns `/bin/df -h`, redirects stdout through a transferred pipe, prints the returned PID,
and reads until EOF with a 15-second deadline. It then checks `waitpid`, raw exit status 0, SIGCHLD delivery, and `ECHILD` after reaping.
`--verify-statuses` adds gated children that return 0, return 37, and wait for parent-issued SIGTERM. The signal child resets and
unblocks SIGTERM, writes a readiness byte, and remains blocked on its gate. After receiving readiness, the parent verifies
`WNOHANG=0`, calls `kill(child, 0)`, then `kill(child, SIGTERM)`. It keeps the gate open until `waitpid` observes termination,
so a delayed launchd signal cannot be confused with an EOF-induced child exit. It checks `WIFSIGNALED`, `WTERMSIG == SIGTERM`,
raw status `0x000f`, SIGCHLD, `ECHILD` on a second wait, and `ESRCH` from `kill(child, 0)` after reaping. The exit cases retain
their gate release and include an any-child wait selector.

`--repeat N` runs between 1 and 16 sequential spawn/read/EOF iterations, defaulting to one. `--debug-delay N` waits 1–60 seconds
after simulator startup before spawning, allowing LLDB to attach. Use `--debug-delay 60` for the verified device workflow:

```sh
xcrun devicectl device process launch --device iphone-0x11-cua-duy --terminate-existing --console \
  -- com.kdt.LiveSimBooter /bin/spawn-probe --debug-delay 60 --repeat 2 --verify-statuses
```

`--stop-before-spawn` prints the probe PID and raises `SIGSTOP` once before the first spawn. Native stop/continue testing passed,
but attaching to this already-stopped process failed in the device test; use the delay option there. Separate foreground device
runs completed two spawn/read/EOF iterations and a spawn under LLDB with hardware breakpoints before wait support was added.

`Tests/run.sh` includes the spawn-plan and transport tests. They cover plan validation, ordered FD actions, inheritance flags,
error propagation, pipe/FD ownership, malformed XPC payload cleanup, authenticated audit PID extraction, and detection of an
extra native thread. These Mac tests do not establish that extension launching or simulator-to-host XPC works on the phone.
The broker tests additionally exercise exact PID/generation binding, reply failures, descriptor cleanup, and failed-launch
queue behavior around the actual broker state machine. [ExtensionLauncherTests.m](../Tests/ExtensionLauncherTests.m) checks
launch reply validation, fresh instance UUIDs, deferred callbacks, release suppression, assertion failure/invalidation, and
errno propagation with local mocks of the declared private interfaces. A second transport build exercises the guest's direct
broker-port path with `XPC_FLAGS=0x2`, including malformed port names and rights validation.

The wait tests cover actual non-child kqueue exit statuses on macOS, shared-slot reuse, concurrent reaping, blocking and lost
wake hints, signal-handler reaping, discard policy, interrupted waits, RBS context validation, and canceled monitor callbacks.
Broker tests also cover status ownership, failed reservations, parent death, and direct-signal denial falling back to the
specific extension instance. These local tests do not establish iPhone sandbox permission or launchd delivery.

[ChildKillTests.c](../Tests/ChildKillTests.c) covers invalid signals, unsupported PID selectors, signal 0, native self-signaling,
and broker error/errno propagation. Transport tests cover kill request/reply encoding and malformed message rejection.
The broker's kill cases cover direct-child ownership, audit generation, completed jobs, and the retained-instance fallback.

The unsigned and signed app/extension builds passed, including the native UIKit shell used for the successful device run.
The child binary's dependencies and entry point were inspected, and the nested signatures and profile entitlements were
verified. Broker argument-preservation tests cover the default probe and explicit guest arguments/environment. The current
host links UIKit and Foundation; the child still links only libSystem and libc++, with no Objective-C imports or sections.

## Original-device progress, 2026-10-25

The following recorded tests predate the cross-process `kill` interposer. Their SIGTERM status case used a self-issued
signal; they validate waiting and notification, not parent-to-child signaling.

The complete wait/SIGCHLD test passed under LLDB. Foreground host PID 9918 launched simulator parent PID 9919 with
`--debug-delay 60 --repeat 2 --verify-statuses`:

| Child | Result |
| --- | --- |
| df PIDs 9921 and 9922 | Each returned 2,268 stdout bytes followed by EOF and raw wait status `0x0`. |
| Gated PID 9923 | `WNOHANG=0` while blocked, then exit 0 with raw status `0x0`. |
| Gated PID 9924 | `WNOHANG=0` while blocked, then exit 37 with raw status `0x2500`; reaped using the any-child selector. |
| Gated PID 9925 | `WNOHANG=0` while blocked, then SIGTERM with raw status `0xf`. |

Every child produced an observed SIGCHLD, and a second wait returned `ECHILD`. LLDB and RBS both reported parent exit 0.
Hardware breakpoints at `LSBWaitPID`, `childSignal`, and `finishWait` showed one guest thread. The signal handler received
argument 20 through simulator `libsystem_platform.dylib::_sigtramp`; wait completion exposed the expected 0, `0x2500`, and
`0xf` values. Native-cache code in `[0x180000000, 0x1ee31c000)` remained inaccessible. Evidence is
`tmp/multiprocess/console-wait-lldb.log`, `tmp/multiprocess/wait-sigchld-lldb.txt`, and `tmp/multiprocess/syslog-wait.log`.
The full local suite passed in `tmp/multiprocess/tests-wait-final.log`, and the installed signatures were audited.

Earlier wait iterations established the fallback requirements: kqueue status registration and direct SIGCHLD were sandbox
denied; RBS delivered real raw status; launchd delivered the notification. The first launchd run then trapped while releasing
an inactive connection. The canceled-connection activation fix resolved that cleanup failure in the complete run above.
The older `console-wait-rbs.log` and `console-wait-launchd.log` record those intermediate failures.

The following spawn-only baseline predates wait support. Native host PID 9858 launched simulator probe PID 9859, which called
`posix_spawn` twice in sequence and received child PIDs 9860 and 9861. Each child ran `/bin/df -h` from the rebuilt iOS 26 cache;
the probe read 2,268 bytes from each redirected stdout pipe and reached EOF. Each broker job then finished with error 0.
This verifies guest IPC, extension launch/check-in, argument and FD delivery, native-cache removal, simulator startup, repeated
launches, and pipe-writer cleanup. Its broker error 0 did not itself establish the guest's exit status.

The native host's launch assertion was dropped at 14:47:12.846327; SpringBoard still reported `running-active-Visible` at
14:47:12.960300 and 14:47:13.152221. Its real UIKit foreground scene therefore fixed the observed headless suspension condition.
This establishes the foreground test, not an entitlement to continue running in the background. Evidence is
`tmp/multiprocess/console-uikit-xpc.log` and `tmp/multiprocess/syslog-uikit-xpc.log`.

An LLDB run then attached to simulator parent PID 9869 during `--debug-delay 60`. Hardware breakpoints at `LSBXPCWireBegin`
and `LSBXPCWireEnd` each showed exactly one thread. The backtrace followed `spawn-probe` → `LSBPosixSpawn` →
`LSBTransportSpawn` → `callBroker`, establishing that the guest interposer and transport were used. Child PID 9872 returned
2,268 stdout bytes followed by EOF, and LLDB reported parent exit status 0.

LLDB recognized `dyld_sim` at `0x106020000`, the spawn shim at `0x10556c000`, and simulator `libsystem_kernel.dylib` at
`0x12b177000`. The native-cache code range `[0x180000000, 0x1ee31c000)` was inaccessible, while the simulator cache at
`0x120000000` was readable/executable. The old debugger metadata page beginning at `0x1ee31c000` is intentionally remapped by
the debugger bridge; its accessibility does not mean native-cache code remains mapped. The saved
`tmp/multiprocess/uikit-spawn-lldb.txt` contains the End breakpoint backtrace, thread list, image list, and memory-region
checks; the Begin breakpoint was inspected live before that transcript was saved. Output/EOF is recorded in
`tmp/multiprocess/console-uikit-lldb.log`.

The earlier cache-only test, child PID 9695, already established that the pure-C extension could run simulator df after the
App Group migration and native-cache handoff. The final test adds simulator-parent IPC and two independently launched children.

The current runtime has all 219 loose cached dylibs moved, with their relative paths preserved, to the reversible backup
`<app-group-container>/RuntimeLooseDylibs-multiprocess`. The active `RuntimeRoot` retains dyld_sim, the rebuilt cache, the spawn
shim, and the executables. This forces cached dependencies to come from the rebuilt cache; it does not delete their backups.
The successful run published 217 cached images, with delayed initialization and only the needed initializer calls. Local
console evidence is `tmp/multiprocess/console-cache-only.log`.

The device iterations established these implementation constraints:

- The original `SecTaskCreateWithAuditToken` signature inspection was denied by the sandbox's
  `process-info-codesignature` rule for another process, making the first check-in fail with `EACCES`. Authentication now uses
  pkd's exact returned PID and the kernel audit generation.
- The original `beginExtensionRequestWithInputItems:completion:` path waited for the absent NSXPC service/Hub handshake,
  reported error 4097 after about five seconds, and terminated the pure-C child. The launch-only pkd operation avoids that
  handshake, and the C startup notification satisfies launchd's extension-alive check without UIKit.
- A pkd launch reply returned a valid PID with `uuids = nil`. The adapter accepts that optional omission and still validates
  a UUID array when one is supplied. The PID array must contain exactly one valid PID for the one-plugin launch request.
- The first native host hit the approximately 20-second process-launch watchdog. The headless FrontBoard workspace completed
  the launch handshake with `--no-activate`, but foreground activation failed before the delegate received a scene callback
  because UIKit scene classes were missing. The new native UIKit shell replaces this workaround.
- RunningBoard subsequently suspended native host PID 9787: it was launched at 14:27:49.574660, its launch assertion was dropped
  at 14:27:50.626813, and its state became `running-suspended`, `taskState 3`, `debug:none` at 14:27:50.741700. There is no fixed
  10- or 30-second grace period. The capture started too late to determine when the preceding host PID 9764 stopped executing.
  Its child's unchanging PC during a debugging pause is consistent with the originator-dependent assertion policy, but that
  earlier suspension timing is not proven by this log. Evidence is `tmp/multiprocess/syslog-lifecycle.log`.
- Attaching LLDB to the headless native host alone did not prevent suspension. Actual foreground activation resumed host and
  child briefly, then failed scene setup and backgrounded them again. The 14:32:41 and 14:35:22 lifecycle captures motivated
  the minimal native UIKit shell. UIKit remains absent from the simulator child.
- An earlier successful handoff, host PID 9230 and child PID 9231, selected loose overrides and expanded the dependency closure
  to 217 libraries. AMFI accepted the loose RegexParser image, but the child remained in signature-registration `fcntl(97)`.
  Moving the 219 cached loose files aside produced the successful cache-only df run above. That earlier stall has not been
  independently explained.
- Spawn-probe PID 9697 reached the shim and returned `EIO` because simulator bootstrap discovery was disabled without
  `XPC_SIMULATOR_LAUNCHD_NAME`. Native preparation now preserves a raw broker send right for the simulator pipe path instead.
- Locked-device launch denials interrupted some tests, including the installed direct-port build's first retry. Those failures
  occurred before the app or extension test and do not establish a loader failure.

Remaining checks are:

- Device coverage of launch errors and supported attributes/actions beyond the tested stdout redirection and repeated launches.
- Longer-running and concurrent guest workloads, including resource cleanup at the configured job/FD limits.

Background execution and broader runtime compatibility remain outside this implementation.
