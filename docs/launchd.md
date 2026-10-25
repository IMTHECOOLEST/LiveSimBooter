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

This mode has an empty `Bootstrap.Paths`. The empty-bootstrap result alone does not establish daemon loading or a full system
boot. The following job probes separately exercise endpoint propagation, `xpcproxy_sim`, and supported exec replacement.

## A df job and exec replacement

The opt-in `--launchd-df` bootstrap writes one RunAtLoad job, `com.kdt.LiveSimBooter.df`, into the dedicated
`SimulatorData/Library/LiveSimBooter-LaunchDaemons` directory. It refuses unexpected entries in that directory. The job uses
guest path `/bin/df` and writes stdout/stderr to `SimulatorData/Library/Logs/df.log`. The plist uses the guest path
`/private/var/mobile/Library/Logs/df.log`; launchd translates it through the configured simulator data directory.
WorkingDirectory uses `/private/var/mobile/` with a trailing slash to select that same prefix rule.
`LSB_DEBUG_STOP_CHILDREN=1` propagates the pre-handoff debugger stop to the job processes as well as launchd.

Stock launchd first spawns `usr/libexec/xpcproxy_sim`. A plain job has no optional service-controller endpoint, so its spawn
attributes do not override the registered Mach ports. Native spawn would inherit launchd's registered service send right;
the shim snapshots those ports explicitly and carries owned copies through the broker. The ordered array preserves null
slots and repeated rights. TASK_BOOTSTRAP_PORT is separate, including the distinction between inheritance and explicitly
clearing the port. LiveProcess installs this state before entering simulator libSystem.

xpcproxy asks launchd for its job configuration, then calls posix_spawn with POSIX_SPAWN_SETEXEC. The broker prepares a new
LiveProcess for the final image. The parent keeps the original **guest PID**, while the broker records a separate current
native PID and audit generation. The new instance maps the same child-status ledger and wake endpoint as the old instance.
Only after preparation succeeds does the broker commit the replacement, reparent its existing children, and retire the old
native process. A preparation failure returns an error to the original caller. The old native exit is not a guest exit;
waitpid returns the replacement's final status under the original guest PID, and kill targets its current native identity.

The ledger carries a nonconsuming observation of the current native PID, exec generation and exit status. A staged exec
publication keeps readers from observing an accepted replacement until the native READY reply succeeds. Guest getpid/getppid
expose the logical identity. Kernel audit tokens retain their actual native identity.

Launchd also watches process dispatch sources and queries proc_pidinfo. The compatibility source for a managed child uses
a real DATA_OR dispatch source and polls the ledger for exec/exit transitions; it does not watch the retired proxy's native
PID. Process-info queries translate to the current native PID. These adapters are scoped to broker-managed children.
They do not create a kernel PID namespace, and raw syscalls or arbitrary audit-based service authentication are not virtualized.

The process source polls every 10 ms and coalesces events. It supports C dispatch retain/release ownership and does not emit
fork events. Version 3 of the child ledger pins each subscribed ticket until polling completes: waitpid and SA_NOCLDWAIT may
consume the status without losing an existing source's EXIT event. A pinned slot cannot be reused; abandoning a live child
does not manufacture EXIT. The proc_pidinfo flavor-18 reply rewrites PID/PPID only; unique IDs and audit
identity remain native. wait4 supports a null rusage pointer; nonnull usage fails without consuming a status.

Stock xpcproxy unconditionally calls setsid for a valid program. Native extensions reject that call with EPERM on this test
device. The launchd bootstrap therefore explicitly enables `LSB_SIMULATE_SESSIONS=1`. This gives the current guest a logical
SID/PGID, preserves them across spawn/exec even when proxy supplies a new environment, and permits setsid once unless it is
already a guest group leader. It does not change kernel groups, detach a controlling terminal, or implement group signals.
Cross-process session queries, setpgid and POSIX_SPAWN_SETPGROUP reject with ENOTSUP in this mode.

On 2026-10-25, the standalone SETEXEC probe preserved guest PID 79999 while replacing native PID 79999 with 80000; the parent
reaped exit 23 under 79999. Its second case preserved guest PID 80001 across native PID 80002, delivered SIGTERM using 80001,
and reaped signal status 15. The real launchd job subsequently ran proxy PID 80117, replaced it with df PID 80118, and reported
df status 0 while retaining guest PID 80117. Launchd remained responsive: the native health probe returned 1102.
Evidence: `tmp/launchd-df/console-exec-probe.log`, `console-final-normal.log`, and `df-normal.log`.

## Final LLDB validation and startup storage

The final hardware-breakpoint run used launchd native PID 80141. Its stock xpcproxy PID 80145 called posix_spawn with flags
`0x4040`, including SETEXEC, for RuntimeRoot/bin/df. The replacement ran as native PID 80150 while retaining guest PID 80145.
LLDB caught launchd's EXEC event handler, df's LC_MAIN entry, and df's exit 0. Launchd's wait4 returned 80145 (`0x13911`),
and the caller consumed status 0. The inspected native-cache region `[0x180000000, 0x300000000)` remained inaccessible;
the simulator TEXT region at `0x120000000` was executable with zero dirty pages. LLDB discovered all 50 df images.
Evidence: `tmp/launchd-df/lldb-final.txt`, `console-final-lldb.log`, and `df-final-lldb.log`.

This testing exposed a separate startup lifetime bug: native malloc placed long apple-vector strings inside addresses later
replaced by the simulator cache's fixed __LINKEDIT mapping. A missing ptr_munge token was actually an overwritten string.
The loader now serializes the kernel argument words and every argv/env/apple byte into a bounded 512 KiB writable allocation
in the app image. Temporary C++ staging allocations are destroyed before detachment. Oversized startup data fails with E2BIG;
main_stack and ptr_munge remain writable for simulator pthread consumption. Tests cover complete pointer containment,
exact-fit and overflow boundaries, destroyed staging allocations, and consumption of the writable startup entries.

Debugger detachment has a separate limitation. Immediately after detaching from PID 80141, a new workqueue thread faulted at
`0x1e4a538b8` inside the unloaded native cache, with the workqueue cleanup entry register shape. Debugserver's detach path
[restores its saved exception ports](https://github.com/llvm/llvm-project/blob/main/lldb/tools/debugserver/source/MacOSX/MachException.cpp#L437-L458),
overwriting the runtime adapter installed after attachment. The df exec/wait sequence had completed before
detach. Do not rely on a detached instance remaining healthy; start a fresh instance without LLDB after these tests.
Evidence: `tmp/launchd-df/post-detach.ips` and the final lines of `console-final-lldb.log`.

The final normal run used launchd PID 80165, proxy/guest PID 80166, and replacement native PID 80167. df exited 0 and launchd
again answered the authenticated lookup with `error=0 bootstrap=1102`. This instance was left running without a debugger.
The SSH amfid/TXM capture contained no rejection or TXM event during these runs. Evidence: `console-final-verified.log`,
`df-final-verified.log`, and `security-final.log` under `tmp/launchd-df`.

`sh Tests/run.sh` and the signed iPhone build pass. Focused loader and process-event lifecycle tests also pass ASan/UBSan.
Process-event metadata is retained through running event callbacks and queued cancellation callbacks, including after the
caller's final release; cancellation wrappers preserve the user's block/function handler and context.

## Process lifecycle follow-up

When a simulated parent exits, the broker transfers its running children and unconsumed exit statuses to a live root simulator
launchd. Each child's own shared ledger carries its current parent PID, so getppid updates without an exec or constructor rerun.
Replacing a parent through SETEXEC preserves its existing children and does not trigger adoption. With no live simulator
launchd, or after launchd exits, the explicit fallback is guest parent PID 0 with no simulated wait owner.

If launchd's 16 child slots are full, copied adoption records wait for space and new spawns receive EAGAIN. Parent metadata
updates immediately, but a specific waitpid for a deferred orphan can return ECHILD until its record is imported. The retry
queue does not retain whole old ledgers. Unknown exit-monitor failures do not invent an exit status.

`--launchd-lifecycle` runs the signed `/bin/lifecycle-probe` as an isolated job. On 2026-10-25, all three device cases reaped
exit 23 before delivering their process-source EXIT callbacks; metadata remained valid. Child 80212 observed parent PID
80211 change to simulator launchd 80205, then exited 37. The broker published its status to launchd; the probe exited 0 and
launchd answered the health lookup. Evidence is in `tmp/backboardd/console-lifecycle.log` and `lifecycle.log`.
Native tests additionally cover multiple watchers, discard/reuse races, bounded observer counts, abandonment, deferred
adoption, zombie transfer and exec during adoption. Focused ledger, event and broker tests pass ASan/UBSan.

## Native identity and service ownership

The backboardd experiment revealed that the logical PID adapter must leave some native runtime identity checks intact.
After xpcproxy replacement, libdispatch's firehose setup queried the old guest PID and received ESRCH. A self
`proc_pidinfo(guestPID, ...)` query now resolves to the current native PID and returns the native payload unchanged. Direct
`getpid()` calls from libxpc's executable segments also retain the native PID, because libxpc compares it against kernel
audit tokens. Other guest callers still receive the stable logical PID. Image add/remove notifications maintain the libxpc
address ranges; an indirect call through another image or a tail call cannot be classified by this direct-caller adapter.

These changes passed the phone run with guest PID 80376 and native replacement PID 80379. They deliberately leave kernel
audit tokens, process unique IDs and PID generations unchanged. Managed-child `proc_pidinfo` translation and logical
wait/kill behavior do not provide a general kernel PID namespace.

The next observed failure was ownership of `com.apple.iohideventsystem`: simulator launchd resolves the native caller PID from the
kernel audit token against jobs still indexed by the proxy's stable guest PID. Backboardd's check-in returned 1100
(`BOOTSTRAP_NOT_PRIVILEGED`), then `-[BKHIDSystem startHIDSystem]` failed. Details, the backtrace and the expanded cache's
verified df run are in [backboardd and the simulator UI cache](backboardd.md).

The implemented adapter changes four inline PID loads in the pinned 23A343 launchd_sim, UUID
`C3EC5C45-8C85-3D11-B59C-62C4248F5EF9`. The current bootstrap check-in (routine 206) and XPC listener activation (routine 805)
both call the endpoint handler at image offset `0x23670`. Its PID load at `0x236b0` now resolves the caller through the broker
before the existing job lookup and endpoint-owner comparison. The two legacy check-in loads at `0x2e658` and `0x2e6b8`, plus
XPC self lookup at `0x1bc08`, use the same adapter. Interposing `audit_token_to_pid` alone cannot cover these inline reads.

[The offline patcher](../Tools/patch-ios26-launchd-identity.py) replaces each load with a `B` into verified empty __TEXT
padding. Each veneer preserves all general and SIMD registers except the original load's destination, restores NZCV,
FPCR/FPSR, SP and LR, then branches to the next original instruction. It obtains the token pointer from the original register
or stack location without changing any token byte. A new libSystem `_dlsym` chained import resolves the injected shim's
`LSBLaunchdResolveAuditPID` export. The symbol lookup uses RTLD_DEFAULT; a missing symbol returns PID -1. Existing chained
imports and pointer fixups remain intact, and the signature allocation is moved after the new fixup metadata. The resulting
file must be signed again before deployment.

The helper sends the complete audit token through the existing synchronous XPC broker channel and preserves errno. Only an
authenticated, running simulator launchd may make this request. An exact full-token match to a running, committed managed
process returns its stable guest PID. Pending replacements, retired processes, stale credentials or PID generations, and
native PIDs aliasing an active or retired managed PID are rejected. Unmanaged identities otherwise retain their native PID;
the broker trusts the authorized launchd to supply the token from its kernel-audited request. The original token remains
available to launchd for sandbox and entitlement checks. The child ledger's `execGeneration` is not the kernel PID version.

Replacement commit and these resolver requests run on the same broker queue, so a lookup cannot run between the READY reply
and completion of the exec commit. Exited and replaced identities are retained in a bounded history of 256 records. Once
that capacity is exceeded, unmanaged lookups fail closed until the host restarts; exact active managed identities still
resolve. Old records are not evicted into native-PID fallback. Launchd's job PID and hash tables remain stable, as do guest
wait/kill semantics. This translation does not provide missing entitlements or a general kernel PID namespace.

The owner RPC uses `xpc_pipe_routine` without an explicit timeout. A returned transport or authentication error yields PID -1,
but a host that stops answering can stall launchd inside its service handler. This is separate from the one-second native
Indigo registration deadline below. Avoid pausing the host broker while expecting a guest owner lookup to complete.

## Reproducing the signed launchd adapter

Use a fresh output directory: the patcher and executable signer refuse to overwrite their source or an existing output.
The patcher accepts the original stock payload or that same payload with a different signature allocation, validates its
UUID and all nonsigning bytes, rejects partial or unrelated patches, and recognizes an already patched input idempotently.
It never signs or deploys a file. Rebuild and deploy the matching host app and shim with the patched launchd; an older shim
without the exported resolver cannot complete the patched owner lookups.

```sh
runtime='/Library/Developer/CoreSimulator/Volumes/iOS_23A343/Library/Developer/CoreSimulator/Profiles/Runtimes'
runtime="$runtime/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot"
work=tmp/launchd-identity-repro
identity=D7610EDC80D34A9B1A3D3C8563D66A5CD4716293

python3 Tools/patch-ios26-launchd-identity.py \
  --source "$runtime/sbin/launchd_sim" --output "$work/launchd_sim-patched"
python3 Tools/sign-sim-executable.py \
  --source "$work/launchd_sim-patched" --output "$work/launchd_sim-signed" \
  --identity "$identity" --identifier com.kdt.LiveSimBooter

sh Tools/build-spawn-shim.sh "$work/libLiveSimSpawn.dylib" "$work/spawn-probe"
codesign --force --sign "$identity" --identifier com.kdt.LiveSimBooter \
  --timestamp=none "$work/libLiveSimSpawn.dylib"
codesign --verify --strict "$work/libLiveSimSpawn.dylib"

python3 Tests/LaunchdIdentityPatchTests.py --source "$runtime/sbin/launchd_sim" -v
python3 Tests/LaunchdIdentityVeneerTests.py
xcrun dyld_info -validate_only "$work/launchd_sim-signed"
xcrun dyld_info -imports "$work/launchd_sim-signed"
```

Install the signed executable as `RuntimeRoot/sbin/launchd_sim` and the signed shim as
`RuntimeRoot/usr/lib/libLiveSimSpawn.dylib`, retaining the previous files for reversal. The executable signer preserves
MH_EXECUTE while clearing CS_EXECSEG_MAIN_BINARY, as required by this runtime's earlier signing tests.

All nine offline patch tests passed against both stock and the device's unpatched signed binary. An independent chain walk
verified all 1,708 existing fixups and exactly one new `_dlsym` bind. The native arm64 harness assembles the same veneer source,
compares its instructions with the emitted patch bytes, and exercises all four sites with positive, zero, negative and
missing-helper results. It verifies original token bytes, destination W-register zero extension, and GPR/SIMD/flags/SP/LR
preservation. Broker, transport and resolver tests cover authorization, stale generations, aliases, replacement ordering,
and history saturation. The full host suite also passes. Device check-in success is not established by these local tests.

`dyld_info -validate_only` and `-imports` accept the new metadata. The installed `dyld_info -fixups` printer traps when it
tries to name the added GOT slot's section: the slot is valid segment/page padding outside a named section. Use the independent
chain-walk test to inspect this layout. The veneers likewise live outside the named __text section, so inspect them by address
in LLDB rather than relying on section-based disassembly. Local artifacts and test/signing logs are under `tmp/launchd-identity/`.

## Native Indigo endpoint

The `--launchd-backboardd` mode creates a native Indigo input receiver before the tank handoff. After the handoff, the host
registers `IndigoHIDRegistrationPort` with the isolated launchd using MIG request 701 and reply 801. The helper accepts only
the expected full native audit token, validates the reply shape, and applies a one-second deadline. It runs off the main
queue so the UIKit host and broker can continue servicing startup. A timeout is an unknown registration outcome and blocks
the dependent replacement; it is not retried blindly.

The initial xpcproxy is allowed to become ready while registration is pending, so launchd can finish its synchronous spawn
and receive the registration request. Its queued SETEXEC replacement waits for endpoint registration to complete. A
registration failure is returned to the replacement request. Once the selected backboardd instance has authenticated to
the broker, its exact native audit token authorizes the Indigo event-port registration; job exit revokes that authorization.

A process already owning Indigo authorization cannot currently perform another exec: the broker returns ENOTSUP before
creating a replacement or changing the live event port, audit authorization or gesture state. This applies even before
that owner connects. The initial xpcproxy-to-backboardd replacement remains supported. A future ownership transfer must
preserve those capabilities transactionally; restoring only the old token after failure cannot restore a dropped event port.

Host tests cover the MIG endpoint protocol, audit mismatch, malformed replies, send/receive timeout cleanup and retained
rights. They also verify Indigo touch/cancel/backpressure behavior. The health-lookup helper now destroys any copied send
right returned by a timed-out send, while retaining the caller's original right. The phone's native host 80851 successfully
registered the endpoint with simulator launchd 80852 (`error=0 bootstrap=0`) before the backboardd replacement was prepared.
The guest event-port handshake and view-to-guest touch delivery remain blocked by IOHID job ownership. No IOSurface XPC or
Metal server has been implemented.
