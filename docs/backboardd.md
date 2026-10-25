# Backboardd and the simulator UI cache

## Cache coverage

The earlier iOS 26 simulator cache contains **219 images**. It includes CoreGraphics and QuartzCore but does not include
UIKit or UIKitCore. The initial backboardd staging added 137 required loose libraries and two more libraries for an AVFAudio
weak re-export. The currently deployed expanded cache replaces those loose dependencies. These are simulator runtime
contents; the native foreground host separately uses the device's UIKit.

On 2026-10-25, a new cache was built from original iOS 26 simulator **23A343** libraries. Its inputs combine the existing
cache, UIKit's dependencies, and backboardd's dependencies, recursively including every weak dependency available in that
runtime. Install-name aliases, including the Cryptex WebKit paths, are resolved before traversal. The result is **545
images**: 542 original runtime libraries plus the three host support libraries supplied by Apple's simulator cache builder.
All expected images are present, including UIKit, UIKitCore, AudioSession, AVFAudio, and every required backboardd dependency.

The only unresolved names in this traversal are weak dependencies absent from the original runtime:

- `/usr/lib/libobjc-env.dylib`
- `/System/Library/PrivateFrameworks/RemoteProcessingBlock.framework/RemoteProcessingBlock`
- `/System/Library/PrivateFrameworks/MLCompilerRuntime.framework/Libraries/libmlc_rt.dylib`
- `/usr/appleinternal/lib/liblinkguard.dylib`

This is a dependency subset for UIKit and backboardd, not a complete simulator installation. Dynamic library loads,
framework resources, service configuration, and the services needed to bring up a user interface still require separate
validation. In particular, including UIKit in a cache does not establish that its initialization succeeds on the device.

## Measured address-space use

These measurements come from the cache headers and every mapping in every subcache, rather than filesystem sizes. The
reserved span includes internal gaps and the final 16 KiB reservation beyond the last mapping.

| Cache | Images | Reserved span | Base | Reserved end |
| --- | ---: | ---: | --- | --- |
| Earlier iOS 26 subset | 219 | 341.03125 MiB (`0x15508000`) | `0x120000000` | `0x135508000` |
| Expanded UIKit/backboardd subset | 545 | 1,067.265625 MiB (`0x42b44000`) | `0x120000000` | `0x162b44000` |
| Full stock iOS 26 cache | 3,221 | 4,076.03125 MiB (`0xfec08000`) | `0x180000000` | `0x27ec08000` |
| Full stock iOS 27 cache | 1,826 | 3,772.796875 MiB (`0xebccc000`) | `0x1c0000000` | `0x2abccc000` |

The expanded cache fits below the native shared-region base and leaves **468.734375 MiB** between its reserved end and
`0x180000000`. Its six mappings occupy 1,003.25 MiB; two internal gaps total 64 MiB. Its last mapping ends at `0x162b40000`.
The unsigned builder output occupies 1,054,048,256 bytes on disk. Signing expands the signature allocation, producing a
1,060,225,024-byte file without changing any mapping or the reserved VM span.

The full iOS 26 cache has 12 mappings across its main file and `.01` subcache. They span
`[0x180000000, 0x27ec04000)`: 4,076.015625 MiB, including 128 MiB of internal gaps. At `0x120000000`, its complete reservation
would end at `0x21ec08000`, well past the native shared-region boundary. Its executable mappings alone occupy
2,380.171875 MiB, so moving only data and LINKEDIT elsewhere would still not fit the unchanged executable mappings in the
entire theoretical 2 GiB low window. The stock builder also omitted SpringBoard.framework because its full cache overflowed;
even that full cache does not contain every runtime image.

The full iOS 27 cache similarly does not fit: at `0x120000000`, its reserved end would be `0x20bccc000`. Its relocation
metadata limitations are documented in [iOS 27 relocation](ios27-relocation.md).

## Why deallocating page zero does not provide another 4 GiB

Placing the full iOS 26 cache at `0x01000000` would mathematically end at `0xffc08000`, below the app's preferred text address.
However, the native LiveProcess executable has `__PAGEZERO = [0, 0x100000000)`. XNU implements this segment by raising the
task VM map's minimum address, rather than inserting an ordinary removable mapping. The arm64 executable loader requires
that minimum to cover at least 4 GiB, and PIE can extend it further. Removing mappings therefore does not open this range;
the kernel's minimum-address update also rejects lowering the bound. These are source findings, not a new device mapping
experiment. See Apple's [Mach-O loader](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/mach_loader.c)
(`load_machfile`, `load_segment`) and [VM map implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c)
(`vm_map_raise_min_offset`).

## Build and signature validation

The existing patched 23A343 builder was reused from `tmp/minimal-cache-compare/tools/update_dyld_sim_shared_cache`. Its base
selection remains `0x120000000`; the existing dyld_sim cache-base and cleanup-size patches do not depend on the image count.
The source libraries were selected individually from the original runtime, with no extracted-cache libraries or patched
loose host-support copies used as builder inputs. Absolute symlinks to external inputs were not enumerated by the builder,
so the staging script copies only the 542 selected originals and dyld_sim.

```sh
WORK="$PWD/tmp/backboardd/cache-ui"
./tmp/minimal-cache-compare/tools/update_dyld_sim_shared_cache \
  -root "$WORK/root" -cache_dir "$WORK/unsigned" -iOS -arch arm64 -force -verbose
```

The completed cache was signed with identity `D7610EDC80D34A9B1A3D3C8563D66A5CD4716293`, identifier
`com.kdt.LiveSimBooter`, and team `UU7W52U79B`, using [the cache signer](../Tools/sign-dyld-cache.py). Independent inspection
confirmed:

- CodeDirectory version `0x20400`, SHA-256, 16 KiB pages, 64,208 valid code-page hashes, no scatter table, and no entitlements.
- Executable-segment flags are zero, and its extent matches the executable cache mapping.
- Signature offset 1,051,983,872 plus capacity 8,241,152 equals the signed file size. The 2,060,041-byte signature superblob
  fits, with unused signature capacity zero-filled.
- The cache payload matches the unsigned output byte for byte except for the header's signature-size field. Mapping tables
  and the reserved range are unchanged.
- `codesign --verify --strict --verbose=4` passes with host trust-service access. A sandboxed verification first reported
  `CSSMERR_TP_NOT_TRUSTED`; verification outside the sandbox succeeded.

Signed cache SHA-256: `493ad467f2032d283608678002b888edb618f4736f174458f64c3be38384ea24`.
CDHash: `2f91969b23198c4c8d31b0d876aea7f3441a8704`.

Local evidence is under ignored `tmp/backboardd/cache-ui/`: `stage.py`, `stage-manifest.json`, `build.log`, `sign.log`,
`validation.json`, and the `unsigned/` and `signed/` outputs. The full iOS 26 measurements use the original cache under
`/Library/Developer/CoreSimulator/Caches/dyld/26A434/com.apple.CoreSimulator.SimRuntime.iOS-26-0.23A343/`; the iOS 27 measurements
use the 24A434 runtime's `RuntimeRoot/System/Library/Caches/com.apple.dyld/`.

## Device validation of the expanded cache

The signed 545-image cache is now installed on the iOS 26.1 test phone, with its SHA-256 verified after transfer. The 139
loose backboardd dependencies were moved out of RuntimeRoot into the reversible App Group backup
`Library/CacheTrials/UIKit545`. This makes the cache responsible for those dependencies during the test.

LLDB attached to simulator launchd PID 80819 and observed executable cache TEXT at `[0x120000000, 0x146148000)`, with `r-x`
protection and zero dirty pages. The native shared region `[0x180000000, 0x300000000)` remained inaccessible. Detaching that
debugger caused the already documented native-workqueue SIGBUS after exception-port restoration; this was not a failure to
execute the expanded cache. See [the detach limitation](launchd.md#final-lldb-validation-and-startup-storage).

A fresh run used launchd 80841, xpcproxy/guest PID 80844, and replacement df native PID 80847. LLDB caught df calling the
cache's `getmntinfo` at `0x145ef2984` and exiting 0. The broker published wait status 0 and submitted SIGCHLD to launchd;
this trial did not stop on the parent's wait. This establishes mapping and execution of the expanded signed cache with the
loose dependencies absent. It does not establish successful UIKit initialization or a complete backboardd boot.
Evidence: `activate.log`, `lldb-launchd.txt`, `console-df-fresh.log`, and `lldb-df.txt` under `tmp/backboardd/cache-ui/`.

## Backboardd startup and the first service failure

`--launchd-backboardd` creates one isolated RunAtLoad job from the stock 23A343 `/usr/libexec/backboardd`. The generated plist
imports the stock job's 21 MachServices entries, uses `KeepAlive = false` and `EnableTransactions = false`, and omits scheduler
and identity attributes that the broker does not implement. It writes to `SimulatorData/Library/Logs/backboardd.log`.
The stock executable's `LC_MAIN` offset is `0xb828`; its UUID is `385892A3-EF4B-362A-BAFF-02FE702C41DD`.

The initial loose-library run first failed to resolve `_OBJC_CLASS_$_AVAudioSession`. AVFAudio re-exports that symbol through
its weak AudioSession dependency; importing only required load commands therefore missed a real dependency. Adding
AudioSession and libSessionUtility let backboardd reach main. The expanded cache now includes available weak dependencies
recursively. The original staging manifests are `tmp/backboardd/dependencies.json` and `reexport-dependencies.json`.

Backboardd then exposed two native-identity assumptions after SETEXEC. Firehose queried `proc_pidinfo` using the retired
guest PID, and libxpc compared `getpid()` with a native audit token. The adapters now translate self process-info queries to
the current native PID and give direct libxpc `getpid()` calls that native identity. Ordinary guest code retains its stable
guest PID. Kernel audit tokens and unique process identities remain unchanged; see [the identity boundary](launchd.md#native-identity-and-service-ownership).

With those fixes on the phone, native PID 80379 retained guest PID 80376 and progressed to the following failure:

```text
IOHIDEventSystemCreate:
  bootstrap_check_in("com.apple.iohideventsystem") -> 1100 (BOOTSTRAP_NOT_PRIVILEGED)

-[BKHIDSystem startHIDSystem] + 476
backboardd + 0xb530
backboardd + 0xc1e8
backboardd + 0xb530
backboardd + 0xbb08
```

The fatal message was `IOHIDEventSystem creation failed`. Stock launchd's check-in handler reads the caller PID directly
from the audit token and compares it with the job PID it stored before xpcproxy's replacement. The broker's logical PID
preservation alone did not update that launchd ownership check. The signed owner-translation adapter documented below now
addresses this mismatch without changing kernel audit tokens.
Evidence is in `tmp/backboardd/lldb-identity-fixed.txt`, `lldb-hid-checkin.txt`, and `lldb-hid-failure.txt`.
No IOSurface or Metal service request was observed before this failure.

## Host Indigo input and presentation preparation

The native host now owns an `IndigoHIDRegistrationPort` receiver for the backboardd mode. It registers that endpoint with
the isolated simulator launchd through a bounded MIG 701 transaction and authenticates the reply against launchd's full
native audit token. The broker authorizes only the selected backboardd instance's exact native audit token before guest
entry. Registration accepts the simulator's header-only event-port handoff, retains that send right, and bounds message
parsing and queue draining. It does not open or capture the phone's native IOHID event system.

`SIMULATOR_HID_SYSTEM_MANAGER` points to `RuntimeRoot/Library/Frameworks/SimulatorHID.framework`. The staged plugin comes
from the installed CoreSimulator framework, version 1171.7, built for iOS Simulator with the iOS 27 SDK. Its arm64 imports
were checked against the iOS 26 libraries and it was signed with the same identity and identifier as the runtime. This
static check does not establish full cross-version runtime compatibility. No matching older plugin was found in the
installed Xcode locations. Staging evidence is in `tmp/backboardd/hid-plugin/manifest.json`.

`LSBSimulatorDisplayView` can snapshot a completed nonplanar 32BGRA IOSurface into a native UIImage. It checks the format,
stride and allocation size, copies under an IOSurface read lock, and retains no surface after the call. The view aspect-fits
that image. Only touches beginning inside the displayed image on that view are forwarded as normalized Indigo digitizer
events, with stable touch identities; letterbox and unrelated view touches are ignored. Clearing or detaching the view,
changing its geometry, and app inactivity cancel active contacts. If the Mach queue is full, the server retains the pending
cancellation, retries nonblocking on its serial queue, and rejects new touches until cancellation succeeds or the client
disconnects.

Native tests cover endpoint wire layout, audit authentication, malformed requests/replies, deadlines and Mach-right cleanup;
Indigo tests cover touch sequences, backpressure, cancellation retries and reauthorization. Display geometry and surface
validation tests also pass. The complete host suite passed in `tmp/backboardd/tests-final.log`; focused Indigo tests passed
ASan/UBSan and the server/helper compile for arm64 iPhoneOS.

On the phone, native host 80851 successfully registered the endpoint with simulator launchd 80852:
`[Indigo] registration error=0 bootstrap=0`. Its proxy 80856 then prepared backboardd replacement 80860. This confirms the
host-to-launchd endpoint registration. In this run, the earlier `com.apple.iohideventsystem` ownership failure still blocked
the guest event-port handshake and view-to-guest touch delivery. Evidence is in `tmp/backboardd/cache-ui/console-indigo.log`.

The cache-only backboardd 80860 run then reproduced that failure under hardware-only LLDB: `bootstrap_check_in` returned
1100 at `IOKit` address `0x12708fa64`, and the fatal stop was
`0x1304df830 BackBoardHIDEventFoundation\`-[BKHIDSystem startHIDSystem] + 476`. The next frames were backboardd offsets
`0xb530`, `0xc1e8`, `0xb530`, `0xbb08`, then `dyld_sim\`start_sim + 20`. The crash message remained
`IOHIDEventSystem creation failed`. Cache TEXT was executable with zero dirty pages, and the native shared region remained
unmapped. The SSH amfid/TXM capture contained no rejection or TXM event. Evidence is in
`tmp/backboardd/cache-ui/lldb-backboardd.txt` and `tmp/backboardd/security.log`.

After installing the final host build, a fresh run without a debugger again registered Indigo successfully. Host 80877 and
simulator launchd 80878 remained running after replacement backboardd 80880 exited with signal status 5. The broker
published its exit under guest PID 80879, and launchd still answered the authenticated health lookup
(`error=0 bootstrap=1102`). This confirms the remaining failure also occurs without LLDB. The final installed build includes
the explicit ENOTSUP guard for exec by an Indigo-authorized process. Evidence: `tmp/backboardd/console-final-normal.log`.

## Signed owner translation and HID client identity

The first phone trial of the [signed launchd adapter](launchd.md#reproducing-the-signed-launchd-adapter) used host 80924,
simulator launchd 80925, proxy/guest PID 80928, and replacement backboardd native PID 80931. The broker authenticated the
native audit identity and resolved it to guest PID 80928. Both HID `bootstrap_check_in` calls returned 0 under hardware-only
LLDB, and `IOHIDEventSystemCreate` returned the nonnull object `0x104707010`. This confirms that owner translation passed the
previous service check-in failure while preserving the original audit tokens.

Backboardd then stopped at a new assertion in `IOHIDEventSystemClient.c:589`:

```text
__IOHIDEventSystemClientRefresh: kern_result == KERN_SUCCESS

__assert_rtn
__IOHIDEventSystemClientRefresh.cold.5 + 40
__IOHIDEventSystemClientRefresh + 1528
IOHIDEventSystemClientCreateWithType + 800
-[BKHIDSystem startHIDSystem] + 84
backboardd + 0xb530
backboardd + 0xc1e8
backboardd + 0xb530
backboardd + 0xbb08
dyld_sim`start_sim + 20
```

The failing native operation was `task_name_for_pid(mach_task_self(), getpid(), &port)`: ordinary guest `getpid()` still
returned the stable proxy PID, which is not the running replacement's kernel PID. The new `LSBTaskNameForPID` wrapper in
`Guest/ProcessIdentity.c` translates only a positive guest-self PID when the target is the current task. It calls the real
kernel API with the native PID and preserves that API's returned task-name right and result; other targets and PIDs are
unchanged. This does not grant access to another process or replace a kernel-generated identity.

The wrapper passed native unit tests, ASan/UBSan, and a live macOS self-task lookup. In the second phone run, backboardd 80959
passed both HID check-ins and the self task-name lookup with result 0. The host then logged
`[indigo] guest event port registered native=80959`. This establishes the guest event-port handshake, but not view-to-guest
touch delivery. The first run's evidence is in `tmp/launchd-identity/lldb-first-assert.txt` and `console-lldb.log`; the second
run's host log is `console-self-task.log`. The earlier HID initialization failures preceded SimulatorHID startup and did not
demonstrate plugin incompatibility.

## Initial simulator device profile

After the Indigo handshake, backboardd advanced through `AWStartStandaloneServer` into CMCapture.
`FigCaptureGetModelSpecificName` passed the result of `getenv("SIMULATOR_PRODUCT_CLASS")` to NSString, raising an exception
because the environment value was absent. The host now supplies `SIMULATOR_PRODUCT_CLASS=D84` and
`SIMULATOR_MODEL_IDENTIFIER=iPhone16,2` for the initial iPhone 15 Pro Max guest profile. These exact values come from
`iPhone 15 Pro Max.simdevicetype/Contents/Resources/profile.plist` under
`/Library/Developer/CoreSimulator/Profiles/DeviceTypes/`; CoreSimulator's `-[SimDeviceType environment]` copies the profile's
productClass directly into that environment key. The value is `D84`, not a guessed board identifier with an `AP` suffix.

Explicit inherited values for either key override its default in both launchd and child environments. The first occurrence
wins, matching getenv; empty or invalid UTF-8 values fail configuration with EINVAL before any configuration is published.
Focused tests cover defaults, overrides, duplicate handling and these failures. This supplies only the two identity keys;
display dimensions and capability resources are not added.

The next normal phone launch used host 81006, simulator launchd 81007, guest/proxy PID 81008, and replacement backboardd
native PID 81009. It passed the model-name failure and HID initialization, and the host logged
`[indigo] guest event port registered native=81009`. Backboardd then reached QuartzCore's simulator display discovery.
This confirms the initial profile and the Indigo handshake together in a run started without LLDB. It does not establish
display output or touch delivery. Evidence: `tmp/launchd-identity/console-profile-normal.log` and `backboardd-profile-normal.log`.

## Framebuffer provider selection

Backboardd 81009 consumed a CPU while entering `CAWindowServer`. Attaching LLDB found this repeating tail branch:

```text
0x13abcbcc8  SimFramebufferClient`SFBConnectionCreate + 56
0x13abcbccc  br x2
x2 = 0x13abcbc90  SimFramebufferClient`SFBConnectionCreate
```

The cached framework is a dispatch trampoline. Its initializer calls
`dlopen(getenv("SIMULATOR_FRAMEBUFFER_FRAMEWORK"), RTLD_LAZY)` without checking for a missing environment value.
The unset value became `dlopen(NULL)`, so `dlsym` found the trampoline's own exports and every call branched back to itself.
This failure did not require incorrect loader interposition. The stack continued through `-[CAWindowServer _detectSimDisplays]`
and `+[CAWindowServer serverWithOptions:]`. Evidence: `tmp/launchd-identity/lldb-framebuffer.txt`.

Backboardd configuration now supplies an absolute path beneath the canonical RuntimeRoot in both the launchd `Self` and
`Child` environments:

```text
SIMULATOR_FRAMEBUFFER_FRAMEWORK=<RuntimeRoot>/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer
```

The value is controlled by the host; inherited duplicates cannot redirect it to the cached SimFramebufferClient or clear it.
Other launchd modes do not add this setting. Focused configuration tests cover canonicalization, the two scopes, duplicate
removal and the mode boundary.

The provider to deploy at that path is the real arm64 simulator framework from
`/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework`.
Its version is 1171.7, minimum OS 15.0, built with the iOS 27 SDK. It implements the trampoline's exports and connects to the
bootstrap service `com.apple.CoreSimulator.SimFramebufferServer`. In the phone trial with backboardd 81070, the dispatch table
resolved to the real provider, and `SFBConnectionCreate` returned object `0x106b228c0`. `SFBConnectionConnect` returned false
with error object `0x1075041b0`; the exact bootstrap lookup result was not captured. Backboardd continued past this failure,
so an unavailable framebuffer connection is nonfatal on this startup path. Evidence: `tmp/launchd-identity/lldb-framebuffer-provider.txt`.

The stock client's initial Mach exchange, packed structures, error replies and proposed host lifetime rules are recorded in
[framebuffer protocol research](framebuffer.md); that server is not implemented yet.

## Runtime resources after framebuffer discovery

Backboardd 81070 next raised `NSInternalInconsistencyException` while reading the absent
`/System/Library/BackBoard/EventProcessorConfiguration.plist`, at `BKHIDCreatePrimaryEventProcessor + 496`. The matching stock
23A343 resource was copied into RuntimeRoot. A subsequent normal launch used host 81093, simulator launchd 81094, guest PID
81095 and backboardd native PID 81096. It registered the Indigo event port and remained alive after approximately 22 seconds
with reported CPU use 0.0%. This is a process-liveness observation, not proof of a working display or a complete simulator.
Evidence: `tmp/launchd-identity/backboardd-framebuffer-provider.log`, `console-resources-normal.log`, and `resources-normal-state.log`.

Attaching LLDB to 81096 confirmed that its main thread reached `CFRunLoopRun`, through backboardd offset `0xbf90` and
`dyld_sim`'s `start_sim`. The render-server thread was waiting in `CA::Render::Server::server_thread`. A separate worker was
waiting for a synchronous DataMigration XPC reply in `_DMPerformMigration + 292`; startup is therefore not fully complete.
The transcript is `tmp/launchd-identity/lldb-backboardd-idle.txt`. The debugged process was terminated after capture.
The matching stock `System/Library/CoreServices/SpringBoard.app/US.uchr` resource was also copied after its missing-file
diagnostic. Both resource files come from the iOS 26.0 23A343 RuntimeRoot; they are data and require no executable signature.

The complete `sh Tests/run.sh` regression suite and signed device build passed after these fixes. The SSH amfid/TXM capture
contained no rejection or TXM event (`tmp/launchd-identity/security-test.log`). LLDB's internal image-notification breakpoint
can loop during handoff: the test used hardware-only breakpoints, paused the guest, disabled the internal image/thread
breakpoints, then stepped past the notifier before continuing. This workaround does not fix LLDB's notification handling.

The view is not yet connected to a guest surface producer. An IOSurface XPC service and a Metal service are not implemented;
their wire protocols and runtime needs remain work after an actual graphics-service request is reached. The 545-image cache
remains deployed; the earlier 219-image cache and the 139 loose dependencies are retained for reversal.
