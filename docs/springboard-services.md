# Services toward SpringBoard

This note records the service-selection evidence and the staged experiment after the working backboardd display/input
path. It does not establish a sufficient minimum service set or a successful SpringBoard boot. The additional services
below must be evaluated through their actual startup, service replies and failure traces; a generated job plist is not
evidence that its daemon works.

## Pinned simslim evidence

The reference is [MobAI-App/simslim at `2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc`](https://github.com/MobAI-App/simslim/tree/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc),
committed on 2026-10-25. The read-only local clone is `tmp/simslim-reference`. Its
[profile implementation](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L378-L448)
constructs a set of services to **disable** and removes explicitly retained categories or labels from that set.
Everything outside its managed catalog is left alone. It starts with a stock CoreSimulator installation and boot process;
it does not construct a minimal replacement launchd environment.

The pinned catalog contains 168 unique disable labels. Comparing them with the matching iOS 26 **23A343** runtime's
`System/Library/LaunchDaemons` and `System/Library/LaunchAgents` gives:

| Static inventory | Count |
| --- | ---: |
| Stock job definitions inspected | 323 |
| Definitions matching a disable label | 158 |
| Definitions outside the disable list | 165 |
| Unique labels among those remaining definitions | 164 |

The remaining definitions include stock-disabled and feature-conditional jobs, so these are neither live process counts
nor a list of jobs that must be started. The full comparison, stock executable paths and service names are recorded in
`tmp/simslim-reference-inventory.json`. simslim likewise
[distinguishes managed-label counts from processes](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/README.md#L22-L27).
Its current category measurements use
[iOS 26.5](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/docs/category-memory.md#L8-L18),
while this project uses 26.0 / 23A343 on a jailed iPhone. Its retained services are useful evidence, not a directly portable
boot recipe or memory budget.

## Services simslim deliberately preserves

Three services have explicit `AlwaysEnabled` entries:

| Label | Reason recorded by simslim |
| --- | --- |
| `com.apple.assistantd` | Dictation service availability prevents a system Spotlight memory problem. |
| `com.apple.gamed` | Required for timely XCUITest app launches through the development tooling/GameKit path. |
| `com.apple.sharingd` | Required for system share sheets. |

These are workflow requirements, not proof that all three are necessary to reach SpringBoard's initial screen. Their
definitions are in the pinned [Siri](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L85-L90),
[apps](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L275-L280), and
[connectivity](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L317-L322) categories.

The separate [forbidden-label regression test](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles_test.go#L9-L38)
protects these fifteen exact names from disabling:

```text
com.apple.nanoregistryd
com.apple.nanoregistrylaunchd
com.apple.nanoprefsyncd.2
com.apple.nanotimekitcompaniond
com.apple.nanobackupd
com.apple.sleepd
com.apple.appprotectiond
com.apple.ManagedSettingsAgent
com.apple.managedconfiguration.profiled
com.apple.mobiletimerd
com.apple.routined
com.apple.biomed
com.apple.biomesyncd
com.apple.dmd
com.apple.donotdisturbd
```

The test's comments specifically associate disabling `sleepd` with SpringBoard retry storms and disabling
`nanoregistryd` with stalled CoreSimulator operations. The other entries were observed to break boot in that environment.
This is evidence against indiscriminately removing them from a working stock simulator. It is not proof that this
project must launch the entire set before its first SpringBoard experiment, especially where an observation concerns
CoreSimulator's Mac-side management rather than a guest UI dependency.

## Current selected-job mode

`--launchd-services` retains the existing backboardd and simulator cfprefsd jobs and selects these additional defaults in
`Host/Broker.m`:

| Job label | Matching 23A343 program | Main service declarations |
| --- | --- | --- |
| `com.apple.notifyd` | `/usr/sbin/notifyd` | `com.apple.system.notification_center` |
| `com.apple.lsd` | `/usr/libexec/lsd` | Stock `com.apple.lsd.*` endpoints for application, icon, plugin and database operations |
| `com.apple.runningboardd` | `/usr/libexec/runningboardd` | `com.apple.runningboard`, `.watchdog`, `.resource_notify` |
| `com.apple.containermanagerd` | `/usr/libexec/containermanagerd` with stock agent-mode arguments | `com.apple.containermanagerd` |
| `com.apple.fontservicesd` | `/System/Library/PrivateFrameworks/FontServices.framework/Support/fontservicesd` | `com.apple.fontservicesd` |
| `com.apple.containermanagerd.system` | `/usr/libexec/containermanagerd_system` with stock privileged-runmode arguments | `com.apple.containermanagerd.system` |
| `com.apple.distnoted.xpc.daemon` | `/usr/sbin/distnoted daemon` | `com.apple.distributed_notifications@0v3`, `com.apple.distributed_notifications@1v3` |
| `com.apple.configd_sim` | `/usr/libexec/configd_sim -d` | `com.apple.SystemConfiguration.{NetworkInformation_sim,DNSConfiguration_sim,configd_sim}` |

Explicit native-host command-line labels replace those eight defaults; backboardd and cfprefsd remain included. For example,
`--launchd-services com.apple.notifyd` selects only notifyd as the additional job. Labels do not come from guest environment
variables. The mode writes its generated jobs under the owned `SimulatorData/Library/LiveSimBooter-CoreDaemons-<selection-hash>` directory, with
per-label output in `SimulatorData/Library/Logs/<label>.log`. Sorted selections have stable directory names; narrowing or
expanding a selection leaves earlier job files intact and only bootstraps the selected directory.

`LSBPrepareLaunchdServicesPlan` validates the selected matching-runtime plist, label, program and argument fields before
creating a bounded one-shot job. It preserves `Program`, `ProgramArguments`, `MachServices`, accepted `PublishesEvents`
and permitted stock environment entries. It sets `RunAtLoad` true, `KeepAlive` false and `EnableTransactions` false for
the experiment. Stock credentials, scheduling/resource policy, session restrictions and `LaunchEvents` are not reproduced.
Thus this mode tests selected daemon startup and ordinary service handling; it does not claim full stock launch policy.

Disabling `KeepAlive` avoids unconditional restarts, but requests to a declared Mach service can still relaunch a failed
daemon. The stock `containermanagerd` agent uses proxy modes for bundle and system containers; its separate system partner
is now included, but their presence alone does not establish successful container operations. Each parent retains 16
logical-child slots, so this mode accepts at most 14 selected jobs in addition to backboardd and cfprefsd. The broker permits 34 physical jobs: the 16 children and their queued exec replacements, plus simulator launchd
and the native Metal helper. Demand-launched children still consume the same parent ledger.

Host graphics endpoint registration now shares one **30-second monotonic deadline** across the Indigo, IOSurface,
framebuffer and two Metal registrations. The larger initial xpcproxy batch can occupy launchd_sim long enough that the
previous one-second-per-RPC limit expired before it serviced a registration. Each original request and reply remains
alive within the shared budget; requests are not retried, because a timeout leaves their outcome unknown. This bounds
startup registration without duplicating endpoints or treating a delayed reply as permission to register again.

## Staged plan and concrete boundaries

The following order is an implementation plan inferred from the stock jobs and current project boundaries, not a minimal
set established by simslim:

1. Test the current eight selected jobs with the working backboardd/cfprefsd path. Confirm actual service replies and inspect
   each first failure before adding consumers. Notification delivery, LaunchServices registration, RunningBoard identity
   and container operations are distinct checkpoints.
2. Add further supporting jobs when a trace identifies their use, such as `com.apple.mobile.installd`
   (`/usr/libexec/installd`). The newly included configd_sim and system container manager still require service-level
   verification. distnoted has passed the serialized notification round trip below. The two container-manager jobs have
   different service names and argv; they are not substitutes.
3. Stage the matching SpringBoard executable, framework closure and resources, and provide `com.apple.sleepd` before
   judging SpringBoard's retry behavior. Its stock program is `/System/Library/PrivateFrameworks/SleepDaemon.framework/sleepd`,
   exporting `com.apple.sleepd.sleepserver` and `com.apple.private.alloy.sleep.classd-idswake`.
4. Follow observed startup requests for `mobiletimerd`, `donotdisturbd`, application protection and managed-settings jobs.
   Preserve simslim's negative evidence while avoiding a claim that every untouched job is needed immediately.

The stock SpringBoard job is **`com.apple.SpringBoard`**, while the app's bundle identifier is `com.apple.springboard`.
Its executable is `/System/Library/CoreServices/SpringBoard.app/SpringBoard` (134,544 bytes in the inspected runtime).
That executable directly links `/System/Library/PrivateFrameworks/SpringBoard.framework/SpringBoard`, whose standalone
binary is 35,179,968 bytes and is absent from the current [545-image cache](backboardd.md). `RunningBoard.framework` and
`SleepDaemon.framework` are absent too. Adding executables alone cannot satisfy those dependencies. Resources and dynamic
loads also need verification; dependency closure is separate from the daemon-selection list.

The host graphics services currently authorize the selected backboardd instance's full native audit token. The IOSurface
server has a single authorized owner, and the isolated native Metal helper retires when its owner's authorization
generation changes. SpringBoard must receive deliberate additional graphics routing if it requests those services; a
new job must not be accepted merely because its PID belongs to a simulator. Replacing backboardd's token with SpringBoard's
would revoke the working owner. Indigo and framebuffer presentation remain backboardd-owned. See
[IOSurface](iosurface.md) and [native Metal](metal.md) for the existing ownership boundaries.

One stock-plist collision also needs care: SpringBoard declares `com.apple.systemstatus`,
`com.apple.systemstatus.publisher` and `com.apple.systemstatus.activityattribution` when `SystemStatus/EmbeddedDaemon`
is disabled. `com.apple.systemstatusd` declares the same names when that flag is enabled. Preserve the mutually exclusive
conditions; flattening both dictionaries to unconditional services would assign the same endpoints to two jobs.

Device results for this selected-service stage should be recorded separately from these source findings. Existing
[backboardd display/input results](backboardd.md) do not establish that the additional daemons or SpringBoard have booted.

## Notification service on the phone

The 2026-10-26 trial uses the matching 23A343 runtime on iPhone16,2 / iOS 26.1. The first notifyd startup aborted because
`shm_open("apple.shm.notification_center", ...)` returned `EPERM`. `Guest/NotificationSharedMemory.c` adapts only that exact
object (with or without its leading slash), and only with `LSB_SIMULATE_NOTIFICATION_SHM=1`, to an owned regular file at
`SimulatorData/tmp/notification-center.shm`. It preserves creation/exclusion/truncation, normal file permissions, close-on-exec,
and descriptor/mapping lifetime across unlink. It does not change unrelated POSIX shared-memory objects.

notifyd publishes its PID to libnotify clients, which create native process-exit watches. The identity adapter therefore
returns the native PID for direct calls from the exact configured runtime notifyd executable. Other guest code retains
logical identity; `libsystem_notify` is not broadly switched to native identity.

The next failure appeared as Foundation namespace 19 / code 1 / SIGKILL in native process logs. LLDB instead caught the
actual instruction: notifyd offset `0x5ec4`, `brk #0xb001`, with `BUG IN LIBNOTIFY: Event publisher error` and error 1.
The initial publisher fetch and poll used two additional native-audit-PID job lookups in launchd_sim. The
[eight-site identity adapter](launchd.md) now resolves those through the existing authenticated broker; audit tokens and
event-stream authorization checks remain intact.

With these fixes, native host **83483**, simulator launchd **83485**, notifyd **83505**, and probe **83502** / logical PID
**83491** completed a real notification round trip. The probe registered two tokens, set/read the 64-bit state
`0x4c534200a6fa2007`, posted, observed the change on its first poll, read the same state, cancelled both tokens, and exited 0.
The six bootstrap lookups also succeeded. A lookup alone does not establish the functionality of the other five services.
Evidence: `tmp/springboard-services/probe-events-fixed.log`, `console-events-fixed.log`, and `lldb-notifyd-first-trap.txt`.
The older timeout at the beginning of the probe log belongs to the deliberately stopped debugger run.

That run also kept runningboardd, fontservicesd, distnoted and configd_sim alive and restored backboardd framebuffer updates.
configd_sim logged a denied `setiopolicy_np` request. LaunchServices then aborted while obtaining its system-container URL;
the container-manager agent had crashed first. LLDB located its null dereference inside CoreFoundation during
`NSISO8601DateFormatter` initialization from `MCMMigrationStatus`. The matching runtime's `/usr/share/icu` data was missing.
The next trial installed the original `usr/share/icu/icudt76l.dat` and `usr/share/icu/icutzformat.txt`, together with the
separate stock `containermanagerd_system` executable/job. Its libraries and 29 configuration plists were already staged.
Both container-manager processes then stayed alive: native agent **83660** and system partner **83657** in the observed
run. This removes the observed missing-ICU formatter crash; it does not yet establish successful container creation.
Evidence: `lldb-container-first-crash.txt`, `containermanagerd-system-inventory.json`, `icu-small-state.log` and
`lsd-after-icu.log` in the same temporary directory. The ICU hashes recorded on the phone match the staged runtime data.

LaunchServices native **83659** now gets farther and aborts with
`Error getting container url (NOT_CODESIGNED)! Cannot continue.` while obtaining `systemContainerURL`. A later temporary trace identified the failed operation: system container manager **83810** called
`csops_audittoken(83808, CS_OPS_IDENTITY, ..., 1032, &token)` for native lsd **83808**, with the matching native PID and
pidversion in the audit token. The original syscall returned **-1 / EPERM (1)** at
`libsystem_containermanager.dylib+0x3668`. The same query for backboardd **83809** was also denied. The container manager
then translates the missing identifier into `NOT_CODESIGNED`; this result does not mean the mapped executable lacked a
signature. Its own libxpc entitlement query (`CS_OPS_ENTITLEMENTS_BLOB`, operation 7) succeeded.

This establishes a denied native cross-process identity query, not a logical-PID lookup failure. A future guest identity
adapter must account for this native query restriction and the distinction between the LiveProcess executable identity
and the mapped guest executable. The temporary shim only logged original results, preserved return/errno/output, and was
removed after capture. Production signing checks are unchanged. Evidence: `tmp/springboard-services/csops-phone-trace.log`
and `console-csops-trace.log`. Both native Mac interposition comparison and focused preservation tests passed for the
temporary tracer before the phone run.

The subsequent full-batch run used native host **83683** and simulator launchd **83685**, with the shared registration
deadline build. All five host endpoint registrations reported transport error 0 and bootstrap status 0: Indigo,
IOSurface, framebuffer, generic Metal and backboardd Metal. Both guest probes completed actual service operations:

| Probe | Native / logical PID | Verified result |
| --- | --- | --- |
| Distributed notifications | **83699 / 83689** | One main-thread callback; `delivered=1`, `invalid=0`; exact serialized payload returned in **0.381 seconds**; exit 0. |
| libnotify | **83704 / 83693** | Set/read state `0x4c5342000a6895ce`, posted, observed change on the first poll, read the same state, cancelled both tokens; exit 0. |

The distributed probe also resolved both distnoted service names, and the libnotify probe's six bootstrap lookups all
succeeded. Those lookup results do not establish the other services' request handling: LaunchServices **83703** still
aborted with `NOT_CODESIGNED` in the same run. Evidence is `tmp/springboard-services/probes-registration-budget.log`
and `console-registration-budget.log`; the latter confirms wait status `0x0` for both probes and `0x6` for that lsd
instance. The initial libnotify lines in the probe log are retained output from the earlier run, before the new
`pid=83704` entry.

The final installed build also enforces the 14-selected-job boundary and allows the 34 physical jobs needed during a
full set of exec replacements. Local boundary tests cover rejection before any configuration files change and queuing
all 16 replacements. The phone was left using the production shim and the seven surviving additional daemons, with
LaunchServices omitted to avoid demand-driven abort/restart cycles:

```text
--launchd-services com.apple.notifyd com.apple.runningboardd com.apple.containermanagerd
  com.apple.containermanagerd.system com.apple.fontservicesd com.apple.distnoted.xpc.daemon com.apple.configd_sim
```

This is an explicit experimental selection, not a claim that SpringBoard has booted. The default selection still includes
lsd to reproduce its current boundary. The final verification added the distributed-notification probe as a transient job: native **83841** / logical **83831**
again delivered one valid main-thread callback and exited 0, in **0.210 seconds**. Host **83825**, launchd **83827** and
backboardd **83842** remained running; all seven selected daemon PIDs were present in the final process snapshot. The
production shim SHA-256 matched `3206b627ef9a4023c81f919c50c39fa3d459c66fed945ba8abe71a0ade4576d2`, and the installed
extension still carried the increased-memory-limit entitlement. Evidence: `final-phone-state.json`,
`phone-liveprocess-final-entitlements.plist`, and `console-final-working-services.log` under `tmp/springboard-services`.
The final console also records one auxiliary replacement failing with `ENOENT`; its target has not yet been identified.
This did not terminate the seven selected daemons or framebuffer updates.

## Reproducing the service probes

Build both standalone arm64 simulator probes from the repository root:

```sh
Tools/build-core-services-probes.sh tmp/core-services-probes
```

This produces `core-services-probe` and `distributed-notification-probe`; signing and installation use the existing
RuntimeRoot executable workflow. Run them as guest jobs under the simulator launchd bootstrap, so their lookups and IPC
reach the selected simulator services. Merely running them under the native host bootstrap would test different daemons.

`Guest/CoreServicesProbe.c` performs bootstrap lookups and the libnotify state/post round trip described above.
`Guest/DistributedNotificationProbe.c` is pure C and separately registers with
`CFNotificationCenterGetDistributedCenter`, posts a unique name/object with a property-list payload, and validates the
returned nonce, 64-bit number and binary bytes. It pumps the main CFRunLoop for at most five seconds, with a 20-second
hard process timeout. The matching simulator CoreFoundation sends this registration and post over its distnoted XPC
connection and delivers the callback from an inbound `post_token`; the distributed post does not dispatch directly to
its local observer table.

The distributed probe has compiled for the simulator and passed against the Mac's actual distnoted, with one valid
main-thread callback and exit 0. Its sandbox-denied Mac run produced no callback and failed as expected. Those are local
probe checks, separate from the successful iPhone service run above. Local evidence is
`tmp/distributed-notification-probe/macos-native.log` and `macos.log`.

## Cache capacity and provisioning

The current 545-image cache reserves **1067.27 MiB**, from `0x120000000` through `0x162b44000`, leaving **468.73 MiB** below
`0x180000000`. Adding SpringBoard's full strong dependency closure while retaining the current set gives 1347 images:
their original TEXT segments alone total **1482.61 MiB**, with roughly 249 MiB of data before LINKEDIT. These are input
segment totals, not a completed rebuilt-cache measurement. The full unpruned set does not fit the current 1536 MiB window;
the next cache/layout experiment needs a different budget or a smaller dependency set. Inventory is in
`tmp/springboard-services/inventory.json`.

The refreshed LiveProcess provisioning profile permits `com.apple.developer.kernel.increased-memory-limit`; adding the key
to `LiveProcess/LiveProcess.entitlements`, rebuilding, and installing produced an on-device extension signature containing
`true`. This verifies provisioning/signing and successful launch, not a measured memory ceiling. It does not enlarge the
executable virtual-address window. See [installation notes](debugging.md#install-and-transfer).
