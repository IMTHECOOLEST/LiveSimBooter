# Simulator Metal service

The isolated native Metal service now renders a shader-based triangle into a simulator IOSurface on the iPhone. The stock
guest compiler compiles the source, pipeline creation succeeds, and the guest verifies rendered pixels, writes a PNG and
exits 0. Stock backboardd subsequently rendered a spinner frame that reached the host image view. Apple-logo/progress
rendering and a complete simulator boot remain unverified. Partial composition reached 300 frames on the phone, then that
run aborted on an unexpected Indigo HID target, a separate input failure.

Metal commands from the iOS 26 simulator runtime are serialized by `MTLSimDriver` and sent to a separate native LiveProcess.
That helper retains the native shared cache and Metal framework. The native framebuffer and IOSurface services have their
own endpoints; neither substitutes for the Metal command service. The implementation and phone evidence below use the
original simulator **23A343** binaries on iPhone16,2 / iOS **26.1 (23B50)**, tested on 2026-10-25 and 2026-10-26.

## Latest phone results

| Trial | Confirmed result | Boundary |
| --- | --- | --- |
| Native probe, helper 81429 | Native `AGXG16Device`, Apple A17 Pro GPU, serializer version 9, deserializer creation/release, exit 0 | No listener or simulator peer in this trial |
| Service, host 81448 / helper 81449 / native guest 81452 | Five endpoint registrations succeeded; three exact-audit Metal peers accepted; guest device named `Apple iOS simulator GPU`; `newCommandQueue` succeeded | `newLibraryWithSource` failed; guest exited 1 |
| Precompiled library, host 81469 / helper 81470 / native guest 81473 | Loaded an 8,076-byte simulator metallib with `newLibraryWithData` | `newRenderPipelineState` failed with `CompilerError` code 1 and `XPC_ERROR_CONNECTION_INVALID`; guest exited 1 |
| Clear-only, host 81486 / helper 81487 / native guest 81490 | Created an IOSurface-backed texture, encoded a clear and reached `commit` and `waitUntilCompleted` | Command status 5 (`Error`), `MTLCommandBufferErrorDomain` code 7; no verified pixels or PNG |
| Complete compiler bundle, host 81518 / helper 81519 / native guest 81522 | Matching stock compiler bundle, dependencies and resources deployed; device and queue creation succeeded | `newLibraryWithSource` still returned `MTLLibraryErrorDomain` code 3 / `XPC_ERROR_CONNECTION_INVALID` |
| Clear-only with GPU assertion, host 81535 / helper 81536 / native guest 81539 | Helper acquired flags `0x27`; GPU command completed; center/corner BGRA `64,128,191,255`; PNG written; guest exit 0 | No shader compilation, triangle or framebuffer presentation in this trial |
| Explicit compiler registration, native guest 81561 / compiler 81564 | `xpc_add_bundle(path, 2)` led to xpcproxy and stock compiler launches | Compiler trapped while obtaining its XPCService dictionary: `Operation not permitted` |
| Six-site launchd identity adapter, host 81793 / helper 81794 / native guest 81797 / compiler 81799 | Stock source compilation, pipeline creation and GPU triangle rendering succeeded; verified center/corner pixels; PNG written; guest exit 0 | Offscreen probe only; no framebuffer presentation or stock backboardd rendering in this trial |
| Framework bundle redirection, host 81917 / helper 81919 / native guest 81923 | Normal CALayer bundle/resource lookup loaded the matching 175-function simulator library, including `fixed_frag_lpf_cpf`; explicit RuntimeRoot lookup matched; guest exit 0 | Resource/library selection only; stock backboardd rendering is a separate trial |
| Stock backboardd, host 82139 / helper 82140 / native backboardd 82143 | Operation 1 from surface 45 reached the host image view; recovered 1290 × 2796 PNG shows a spinner on black | Later dirty rectangles returned `ENOTSUP` in this trial; no full-boot proof |
| Partial composition, host 82181 / helper 82182 / native backboardd 82185 | Full surface 302 / operation 1, partial surface 303 / operation 5, and surface 304 / operation 300 completed; 300 frames | Backboardd aborted with SIGABRT after about 21 seconds when SimulatorHID rejected target 0 |

The stock run used simulator launchd **82141** and retained backboardd's logical PID **82142**. The resource/library and
compiler fixes removed the earlier shader abort, allowing a real guest frame through the framebuffer protocol. The
recovered `tmp/framebuffer-server/phone-first-frame.png` has SHA-256
`174efa36e14dd9edf5f7e765c2fd772adfbe2d89b0825c59eddd59f876f6b212`; its visible content is a spinner on black, with no
Apple logo or progress bar. Evidence is `tmp/metal-native/console-stock-framebuffer.log` and
`tmp/metal-native/guest-stock-framebuffer.log`. The scoped `tmp/metal-native/security-stock-framebuffer.log` contains no
amfid rejection or TXM event. See [the first stock frame](backboardd.md#first-stock-backboardd-frame) for the later
dirty-rectangle limitation and the separate LLDB-notification stall.

The subsequent partial-composition trial used simulator launchd **82183** and backboardd logical PID **82184**, native
**82185**. Matching dirty rectangles **`597,1350,96,96`** were accepted through 300 frames, and the first-frame PNG matched
the earlier hash. It then aborted in `-[SimHIDVirtualServiceManager serviceForIndigoHIDData:]` because an Indigo event used
unknown target 0. The broker recorded wait status `0x6`, revoked the owner and terminated helper **82182**. Rendering passed;
this run did not remain alive. Evidence is `tmp/metal-native/console-stock-dirty.log` and
`tmp/metal-native/guest-stock-dirty-exit.log`; the input codec fix is separate from the Metal/framebuffer result.

The latest source trial used simulator launchd **81795**, native guest **81797** with stable logical PID **81796**, and stock
compiler **81799** with stable logical PID **81798**. The complete shader-source and pipeline path succeeded after the sixth
launchd owner-domain fix. GPU completion returned no error; CPU readback found center BGRA **127,65,63,255** and corner
**0,0,0,255**. The recovered PNG visibly contains the triangle and has SHA-256
`9c4f59a004cf5563380110369466381e9b654292f9c46b00188730ab2956bcea`.
Evidence is `tmp/metal-native/console-compiler-sixth.log`, the **81797** section of
`tmp/metal-iosurface/guest-compiler-sixth.log`, and `tmp/metal-iosurface/phone-triangle.png`. Earlier lines in the guest log
belong to prior trials. This supersedes the compiler failures below; the probe does not use the framebuffer present protocol.

The service trial temporarily ran the offscreen Metal/IOSurface probe as the isolated launchd's backboardd job. Its stable
guest PID was **81451**, retained across xpcproxy's SETEXEC replacement; its actual native PID and service audit identity
were **81452**. Ordinary `IOSurfaceCreate` returned a 256 × 256 BGRA surface, ID 48, stride 1,024, allocation 262,144 bytes.
The source-library error was:

```text
Error Domain=MTLLibraryErrorDomain Code=3
Compiler encountered XPC_ERROR_CONNECTION_INVALID (is the OS shutting down?)
```

This records a failed compiler XPC connection, not a diagnosis of its underlying cause. No shader library, render pipeline,
GPU submission, verified triangle or PNG resulted. The broker observed wait status `0x100`, revoked the owner's shared
authorization and terminated native helper 81449. This confirms the intended failure cleanup as well as the successful
device/queue path. Evidence is in `tmp/metal-native/console-service.log` and the **81452** section of
`tmp/metal-iosurface/guest-service.log`; the latter contains appended output from an earlier unsuccessful run.
`tmp/metal-native/console.log` records the earlier construction probe.

The subsequent precompiled trial used Apple's Metal toolchain to build a simulator metallib. Loading it succeeded, but
creating the render pipeline still required compilation and returned the same XPC connection error, now in `CompilerError`
code 1. MTLSimDriver has a guest `MTLCompiler` that produces function variants before serializing a pipeline to the native
helper. A precompiled AIR library therefore does not remove all compiler-service calls. The error alone does not locate
the failing connection on the guest or native side. Evidence is in `tmp/metal-native/console-precompiled.log` and the
**81473** section of `tmp/metal-iosurface/guest-precompiled.log`.

Deploying the complete matching stock compiler set did not fix source compilation. That later trial used host **81518**,
helper **81519**, and native guest **81522**. The deployed set contains the MTLCompilerService XPC application bundle, Metal
framework metadata, nine signed binaries and the GPUCompiler resources: **98 files / 165,073,929 bytes** after signing.
`newLibraryWithSource` still returned `MTLLibraryErrorDomain` code 3 with the same invalid-connection message. Copying the
bundle alone did not establish successful XPC discovery or a compiler-service launch. Inventory
and evidence are `tmp/metal-iosurface/compiler-deployment.json`, `compiler-device-verification.log`,
`tmp/metal-native/console-compiler-bundle.log` and the **81522** section of `tmp/metal-iosurface/guest-compiler-bundle.log`.

A subsequent opt-in probe explicitly registered the canonical compiler `.xpc` bundle with `xpc_add_bundle(path, 2)` before
Metal initialization. Native guest **81561** then caused isolated launchd to spawn xpcproxy **81562** and replace it with
the stock compiler at native PID **81564**, retaining logical PID **81562**. The compiler reached `xpc_main` but trapped in
`_xpc_copy_xpcservice_dictionary.cold.2`: `Couldn't retrieve XPCService dictionary from service bundle. Cause: Operation not permitted`.
The failure comes from launchd's separate routine-718 owner comparison after self lookup; the original four-site PID adapter
did not cover it. The fifth owner-read patch fixed that failure: native compiler **81647** completed startup, but its listener
then canceled with `failed at listener activation with error 1 - Operation not permitted`, and source compilation waited
until the probe's 60-second alarm. Evidence for the earlier dictionary failure is in
`tmp/metal-native/console-register-compiler.log`, the **81561** section of `tmp/metal-iosurface/guest-register-compiler.log`,
and `tmp/metal-iosurface/compiler-crash.ips`. This establishes a stock guest compiler launch path without a native compiler relay.

A later LLDB capture proved why activation still failed: native compiler **81752** mapped to logical PID **81749**, but an
unpatched upstream lookup selected the user domain instead of that managed compiler's application PID domain. The endpoint
handler therefore saw an unmanaged placeholder and correctly denied registration. The [six-site adapter](launchd.md#native-identity-and-service-ownership)
also translates the authenticated owner read at launchd offset `0x2d130`, before the existing managed-job domain lookup.
It preserves the full audit token, native PID-domain handles, fallback, and endpoint ownership checks. The six-site output
passes twelve offline patch tests and the native arm64 register-preservation harness. Its signed phone trial with guest
**81797** and compiler **81799** then passed source compilation, pipeline creation and verified triangle rendering as recorded above.
The failure capture is `tmp/metal-iosurface/lldb-compiler-listener-failure.txt`; the prior compiler wait is recorded in
`tmp/metal-native/console-compiler-identity-atomic.log` and `tmp/metal-iosurface/lldb-compiler-wait.txt`.

The clear-only trial omits shader-library and pipeline creation. Its surface was ID 53, with the same 256 × 256 BGRA layout.
It reached command submission and completion waiting, then returned status 5 and `MTLCommandBufferErrorDomain` code 7
(`NotPermitted`). That error prevents the CPU pixel verification and PNG-writing stages; it is not successful GPU rendering.
This trial preceded the native-helper GPU assertion change described below. The broker observed exit status 1 and terminated
the authorized helper. Evidence is in `tmp/metal-native/console-clear.log` and the **81490** section of
`tmp/metal-iosurface/guest-clear.log`; output before that marker belongs to the earlier precompiled trial.

The repeat with the GPU assertion succeeded: native host **81535** launched helper **81536** with
`assertion flags=0x27 acquired=1`, simulator launchd **81537**, and xpcproxy **81538**, replaced by native guest **81539**.
The guest retained logical PID **81538**; authorization used the full native audit token for **81539**, version **202879**.
Ordinary guest launches still logged flags `0x7`. All five endpoint registrations and all three Metal peer admissions passed.
The 256 × 256 BGRA surface had ID **49**, stride **1,024**, and allocation **262,144** bytes. Its clear command reached
`MTLCommandBufferStatusCompleted` with no error. CPU verification after `IOSurfaceLock` observed both the center and corner
as **BGRA `64,128,191,255`**, matching the encoded clear color. The guest wrote
`SimulatorData/metal-iosurface.png`, and the broker published wait status **0** before revoking and terminating the helper.
This proves guest-to-native GPU submission, completion and shared pixel readback. It does not exercise a shader pipeline,
the framebuffer present protocol or visible stock backboardd output. Evidence is
`tmp/metal-native/console-clear-gpu-assertion.log` and the **81539** section of
`tmp/metal-iosurface/guest-clear-gpu-assertion.log`; earlier lines in that guest file belong to the failed compiler trial.

Build the unsigned arm64 simulator probes with:

```sh
sh Tools/build-metal-iosurface-probe.sh tmp/metal-iosurface/probe-source
sh Tools/build-metal-iosurface-probe.sh tmp/metal-iosurface/probe-precompiled tmp/metal-iosurface/probe.metallib
sh Tools/build-metal-iosurface-probe.sh --clear-only tmp/metal-iosurface/probe-clear
```

The existing positional output and optional metallib arguments remain supported. `--clear-only` selects the separate GPU
clear workload and rejects a simultaneous metallib input. All three variants compile with iOS Simulator minimum OS 26.0
and `-Wall -Wextra -Werror`; building alone does not sign, deploy or execute them.

Two compile-time diagnostic options are also available in `Guest/MetalIOSurfaceProbe.m`. `LSB_PROBE_REGISTER_COMPILER=1`
validates the compiler bundle under the canonical `DYLD_ROOT_PATH`, then calls declared `xpc_add_bundle(path, 2)` before
Metal initialization. Its request and return logs explicitly avoid claiming registration success because the API returns
void. `LSB_PROBE_COMPILER_DIAGNOSTICS=1` logs the invalidation string when Metal calls
`xpc_connection_copy_invalidation_reason` for `com.apple.MTLCompilerService`, preserving the original return value and
ownership. Both options are absent from normal builds and do not replace connection creation or event handlers.

## The initial failure

The reported stack at `MTLDeviceArrayInitialize() (.cold.4)` identifies a failed driver load, before device discovery or GPU
execution. The cold function at original Metal file offset `0x15f454` logs **“could not open MTLSimDriver!”**. Its caller at
`0xc4ab4` executes:

```c
dlopen("/System/Library/PrivateFrameworks/MTLSimDriver.framework/MTLSimDriver", 5);
```

Flags 5 are `RTLD_LAZY | RTLD_LOCAL`. On success the next step resolves `MTLCreateSimulatorDevice`; a missing export has a
different diagnostic in `.cold.3`. A failed dlopen alone does not identify whether the binary, one of its dependencies,
its signature or its platform metadata caused the error. Inspect `dlerror()` at that return to distinguish them.

The rebuilt 545-image cache contains Metal and the common dependencies but omits these two required standalone binaries:

| Install name | Original size |
| --- | ---: |
| `/System/Library/PrivateFrameworks/MTLSimDriver.framework/MTLSimDriver` | 559,744 bytes |
| `/System/Library/PrivateFrameworks/MetalSerializer.framework/MetalSerializer` | 518,608 bytes |

Their recursive dependency closure has no unavailable required or weak dependencies. The staged originals are under
`tmp/metal-iosurface/RuntimeRoot`, with paths, source hashes and sizes in `driver-manifest.json`. MTLSimDriver's `Info.plist`
and `default.metallib` were staged alongside it. The driver is IOSSIMULATOR, minimum OS 26.0, SDK 26.0. Loading these files
can resolve the initial missing-driver boundary; it does not provide the external Metal service.

## Phone probe after correcting IOSurface transport

The 2026-10-25 phone trial with native guest PID **81399** passed the IOSurface boundary and stopped at Metal device
creation. Its section in `tmp/metal-iosurface/guest-xpc-mode.log` follows older appended runs; those earlier failures are
not the corrected result. The matching simulator MTLSimDriver and MetalSerializer files were deployed for this trial.

The host listener now calls `xpc_connection_enable_sim2host_4sim` before activation, matching the simulator client's mode 2.
Raw `Method=0` returned `ErrorCode=0`, a 40-byte limits record and protocol version 1. Both raw allocations succeeded:
64 × 32 BGRA returned 16,384 bytes of shared memory and stride 256; 256 × 256 BGRA returned 262,144 bytes and stride 1,024.
Both returned 72-byte surface state and their `Method=3` releases succeeded. The subsequent ordinary `IOSurfaceCreate`
returned surface ID 33 with stride 1,024 and allocation 262,144 bytes. See [iosurface.md](iosurface.md) for wire-format
evidence and exact API ordering.

`MTLCreateSystemDefaultDevice` returned nil both before the raw IOSurface probe and after successful ordinary surface
creation, with `EnableSimApple5=YES`. No shader compilation, GPU command submission, rendered pixels or PNG resulted on
the phone. This separates the repaired surface service from the remaining Metal device/service problem. It does not
establish a detailed driver initialization failure beyond the nil device. This earlier trial preceded the native service;
the later **81452** result above supersedes its nil-device boundary.

## Preferences and selection

The referenced [TestMetalIOSurface](https://github.com/khanhduytran0/MacWSBootingGuide/tree/main/TestMetalIOSurface) calls
`CFPreferencesSetAppValue("EnableSimApple5", true, "com.apple.Metal")` before obtaining the default device. Neither
`EnableSimApple5` nor `SimApple5` occurs in the inspected iOS 26 simulator Metal or MTLSimDriver binaries. There is no
identified branch here that selects a direct physical GPU through this key.

The actual selection gate at Metal offsets `0xc4a50`–`0xc4a70` reads **`MetalSimulatorEnabled`** in **`com.apple.metal`**
(lowercase domain). The CFString references at `0x22af30` and `0x22af50` confirm these names. If the preference exists and is
false, initialization skips the simulator driver. If absent or true, it tries the driver. The false branch does not load a
native device driver; it simply returns without adding a simulator device. The later `MTLCreateSimulatorDevice` export at
driver offset `0x6dcc` always allocates an `MTLSimDevice`.

This observation is specific to these binaries. It does not establish what `EnableSimApple5` does in another macOS/iOS
Metal build used by the referenced experiment. Setting it in the guest is harmless as a probe, but is not evidence that
the external service can be omitted.

## Driver connections

`-[MTLSimDevice init]` at driver offset `0x2a2c` immediately creates its main connection. `createNewConnection` constructs
`com.apple.metal.simulator.<getprogname()>` and performs a bootstrap lookup. If that endpoint exists, it selects the
process-specific name; otherwise it selects **`com.apple.metal.simulator`**. Relevant offsets are:

| Offset | Operation |
| --- | --- |
| `0x2f18` | Probe the process-specific service with `bootstrap_look_up` |
| `0x2fb4` | `xpc_connection_create_mach_service(name, serialQueue, 0)` |
| `0x2fd0` | `xpc_connection_enable_sim2host_4sim(connection)` |
| `0x2ff4` | Activate the connection |
| `0x3060` | Send the initial request with synchronous reply |

The first dictionary contains uint64 values `messageType=0`, `connectionType=0`, `processRef=0`, `version=1`. The reply is
read for `processRef`, `version`, `registryID` and `hostFeatureProfile`. A failed initial connection returns no device.
Once a nonzero process reference is received, the driver opens two further connections with types 1 and 2, carrying the
same process reference. Subsequent traffic includes serialized resource creation, command-buffer submission, notifications
and completion/error timestamps. An invented successful initial reply would not implement those operations.

## Isolated native service route

The local Sim26Booter `SimBooter/main.m` starts three independently instanced XPC services and publishes their listener
ports as `com.apple.metal.simulator`, `com.apple.metal.simulator.backboardd` and
`com.apple.metal.simulator.SpringBoard`. This separates failure domains. Its `SimRenderServer/main.m` is the IOSurface
server, not the Metal implementation. Its raw XPC-object layout mutation is not required evidence for a compatible native
service and should not be copied into this implementation.

The stock `MTLSimDriverHost.xpc/MTLSimDriverHost` in this runtime is also **IOSSIMULATOR**, rather than a native iOS GPU host.
Its small entry point creates an XPC listener, calls `MTLCreateSystemDefaultDevice`, obtains the device's `registryID`, then
calls the exported `init_with_xpc_connection` from `MTLSimImplementation.framework` with listener, registry ID and zero in
the third argument register. The inspected implementation at offset `0x3198` consumes the first two arguments, installs
its event handler and activates the listener. It retains the listener for process lifetime and stores the registry ID in
global state, which also argues for one isolated service instance per native process.

The implementation's application-context constructor at `0x325c` calls `MTLCreateSystemDefaultDevice`, invokes
`allowLibrariesFromOtherPlatforms` on that device and constructs a deserializer with
`MTLCreateDeserializerForDevice(device, 1)`. The serializer/deserializer library and the native device must be compatible.
Keeping simulator Metal loaded in that host would recursively select a simulator client instead of reaching the physical
GPU. Running the bundled simulator host unchanged under the guest runtime is therefore not the intended native route.

The implemented route uses host-selected execution role **2**, `LSB_EXECUTION_NATIVE_METAL_SERVICE`. For
`--launchd-backboardd`, the broker starts this helper before simulator launchd and waits for its authenticated READY reply
carrying an owned endpoint send right. It registers that endpoint as both `com.apple.metal.simulator` and
`com.apple.metal.simulator.backboardd`, alongside Indigo, IOSurface and framebuffer. All five registrations complete before
the backboardd replacement is released. The initial xpcproxy can run first so launchd can service the registration RPCs.

The C LiveProcess entry obtains the role only from validated broker bootstrap metadata. Guest paths, environment variables
and spawn requests cannot request native mode. Role 2 retains the native cache and loads the backend only from
`LiveProcess.appex/Frameworks/NativeMetal/MetalNativeProbe.dylib`, resolved beneath its own executable directory. Missing
bundled files produce a normal preparation/READY error. The backend can use native Objective-C and dispatch; it is separate
from both the UIKit host and the simulator process. The construction-only host CLI `--native-metal-probe` selects role 1.

The broker creates an unlinked 4 KiB authorization file and retains its writable mapping. Only its read-only descriptor is
passed to the helper at fixed **fd 3** through the existing descriptor transport. The helper imports a read-only mapping;
the descriptor is then closed. There is no simulated parent/child wait relationship between this helper and launchd.
Before releasing backboardd's launch plan, the broker publishes its complete kernel-audited native token and generation.
The listener admits only exact-token matches. Guest PID translation does not alter audit tokens or authorize native peers.

The listener enables `xpc_connection_enable_sim2host_4sim` before activation. An export-trie alias exposes the original
implementation's peer handler to the authorization wrapper without changing its instructions. The matching stock server
handles all three connection types after admission. The successful clear and triangle trials establish mode-2 Metal framing,
resource creation, source/pipeline compilation, GPU execution and shared IOSurface readback. The later stock backboardd
trial also verifies the first real frame reaching the host image view; complete simulator boot remains unverified.

Each helper is limited to one owner/generation. A changed or revoked generation retires it permanently; the stock
implementation retains a process-global `processRef` map and provides no reusable shutdown API. On owner exit or
cancellation the broker revokes first, then terminates the helper. Helper failure fails pending dependent work and blocks
further backboardd launches in that session. Restart the host/session to obtain a fresh helper. Exec by a process already
owning a graphics service returns `ENOTSUP`; a second simultaneous Metal owner returns `EBUSY`.

See [NativeMetal/README.md](../NativeMetal/README.md) for the pinned preparation tool, signing/package procedure, peer alias,
authorization and lifetime limits. Keep simulator Metal out of the UIKit host and the native helper's system dependencies.

## Native iOS 26.1 ABI inspection

The matching local device symbol directory is:

```text
/Users/duy/Library/Developer/Xcode/iOS DeviceSupport/iPhone16,2 26.1 (23B50)/Symbols
```

Its image inventory and the installed iPhoneOS SDK contain neither MTLSimImplementation nor MetalSerializer. This is local
symbol/SDK evidence, not a complete probe of every path on the phone. The simulator copies therefore remain the concrete
candidate for adaptation. Their system dependency install names can resolve to native frameworks in a native process;
the implementation's MetalSerializer dependency must resolve to its companion adapted library, for example through a
local `@loader_path` install name. Both copies need iOS platform metadata and fresh signatures. Their other dependencies
must continue resolving to native system libraries.

Static import checks found all **113** MetalSerializer imports and all **122** MTLSimImplementation imports in the native
system libraries plus the companion serializer. This includes all **28** Metal/IOSurface-specific imports, including the
private Metal classes, class/instance-variable symbols, pixel-format and texture-layout functions, and
`IOSurfaceLookupFromXPCObject`. Libsystem's memcpy/memmove/string functions appear as indirect aliases to libsystem_platform
exports rather than ordinary address-bearing symbols. Reports are saved as `native-import-check.json` and
`native-all-import-check.json` in the ignored analysis directory. Presence of a symbol does not prove complete ABI or
behavioral compatibility.

The native device symbols also contain actual implementations of the required private methods:

| Native symbol | Unslid address | Observed behavior |
| --- | --- | --- |
| `-[_MTLDevice allowLibrariesFromOtherPlatforms]` | `0x185c628d0` | Clears a byte in the device's internal state, obtains its compiler, calls the compiler method |
| `-[MTLCompiler allowLibrariesFromOtherPlatforms]` | `0x185ccb638` | ORs flag `0x80` into compiler state |
| `-[_MTLLibrary setOverrideTriple:]` | `0x185cafcf8` | Method exists for the deserializer's library path |
| `-[AGXG16PFamilyDevice newLibraryWithData:error:]` | `0x29e0aebb8` | Native GPU implementation exists |
| `-[AGXG16PFamilyDevice featureProfile]` | `0x29d9a7a6c` | Native feature-profile implementation exists |
| `-[MTLIOAccelDevice registryID]` | `0x185c77f24` | Native registry-ID implementation exists |

The deserializer's serialized-library path at simulator offset `0xe65c` invokes `newLibraryWithData:error:`, optionally
`setOverrideTriple:`, then obtains a function from the resulting library. These calls have matching native method names;
the later triangle trial verifies the complete simulator shader/pipeline path on this native device. `MTLCreateDeserializerForDevice` and
`MTLDeserializerVersion` are exported by the companion MetalSerializer, not by native Metal.

The first native probe performed that construction test in process **81429**: it verified the actual native AGX class and
image, verified resolved symbol origins in both adapted libraries, called the declared `allowLibrariesFromOtherPlatforms`
method using normal Objective-C syntax, then created and released a deserializer with
`MTLCreateDeserializerForDevice(device, 1)`. It exited 0 without publishing a listener. The subsequent authenticated service
trial passed the initial simulator handshake and command-queue creation. The later clear-only trial additionally completed
GPU work and verified IOSurface pixels, as recorded above. The latest source trial also completed a shader pipeline and
verified the rendered triangle.

Two integration limits are visible in the stock server. `init_with_xpc_connection` installs its own listener handler, so
an authentication wrapper needs a deliberate integration point before forwarding newly accepted peers. Its internal
`MTLSimulatorHandleEvent` is a local symbol, not an exported API. Also, `reportLeaks` at offset `0x25f0` uses fork/exec to run
`/usr/bin/leaks`; that optional diagnostic is not a usable service operation on the jailed native process. The normal
device/context initialization path does not call it.

## Reproducing the inspection

Ignored analysis files are in `tmp/metal-iosurface`: `metal26-disassembly.txt`, `mtlsimdriver26-disassembly.txt`,
`mtlsimhost26-disassembly.txt`, `mtlsimimplementation26-disassembly.txt` and `driver-manifest.json`. They were produced with
`xcrun otool -tvV`, `xcrun otool -L`, `xcrun vtool -show-build` and a recursive comparison with
`tmp/backboardd/cache-ui/stage-manifest.json`. The originals are beneath:

```text
/Library/Developer/CoreSimulator/Volumes/iOS_23A343/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot
```

The copied upstream test source/Makefile are `tmp/metal-iosurface/upstream-main.m` and `upstream-Makefile`. The reference
build targets iPhone arm64 and also uses IOMobileFramebuffer plus an Apple tiled/compressed surface format. The host's
current BGRA framebuffer snapshot path is a separate constraint from Metal device discovery and shader execution.

## Native helper GPU assertion

The native Metal service and construction probe now request PlugInKit assertion flags **39 (`7 | 0x20`)** through the
existing declared `processAssertionWithPID:flags:reason:name:` API. Ordinary simulator jobs retain flags 7. Only the
broker's trusted native execution roles select this option; a guest path or environment variable cannot request it.
The launcher logs the requested flags and the actual `acquire` result, and reports `EACCES` if acquisition fails.

This choice follows the iOS 26.1 native binaries, rather than an entitlement change. AssertionServices'
`-[BKSProcessAssertion _legacyFlagsForFlags:]` at `0x22f59fabc` preserves the low six flag bits. RunningBoardServices calls
bit `0x20` `PreventThrottleDownUI`. Native RunningBoard's legacy process-state application at
`0x263321f4c`–`0x263321f80` raises `gpuRole` to 2 when that bit is set; the original flags 7 do not set it. The same branch
also permits the role for an explicit foreground jetsam band, which our launch-only extension does not request.

The existing reason 9 and extension-session name are retained. RunningBoard can translate reason 9 to extension reason 13
for an XPC-service target. Its legacy originator and target validation follow the existing host/extension relationship;
they contain no additional flag-specific entitlement check. Both reasons still suspend their assertion when the originating
host suspends. This is a foreground-host lifetime request, not background GPU permission or a call to privileged task-policy
APIs. A successful assertion acquisition alone does not prove a GPU command completed.

Focused launcher tests exercise flags 39, acquisition rejection, unknown-option rejection and a later default launch retaining
flags 7. Broker tests verify native service/probe selection and rejection of guest attempts to select native policy. The
clear-only device workload passed with helper **81536**, confirming GPU execution and pixel readback for this assertion
choice while the host was foreground. Background behavior remains untested. Native disassembly evidence is saved in
`tmp/metal-native/rb-native-legacy-apply.txt`, `rb-native-legacy-valid.txt`, `rb-native-originator-valid.txt`,
`rb-native-target-valid.txt`, `rb-native-gpu-device.txt` and `rbs-native-flag-names.txt`.

If a running native Metal role loses its assertion while its audited instance still exists, the broker treats that as a
helper failure: service authorization is revoked before termination and the dependent graphics owner is stopped. The probe
role has no authorization page and is terminated directly. If identity validation returns `ESRCH`, the existing exit monitor
remains responsible for the real exit status; no signal is submitted for that vanished identity. Failed identity validation
cannot fall back to signaling a bare PID. An invalidated GPU session retains only a `SIGKILL` cleanup route through its
original PlugInKit service-instance UUID until release. Ordinary simulator-session assertion behavior is unchanged.
Focused tests cover revoke-before-signal ordering, native signal denial and instance fallback, vanished or unverified
identities, duplicate callbacks, failed assertion acquisition, and unchanged default launch flags. Device assertion-loss
and background transitions remain untested.

## Guest compiler bundle registration

Deploying the complete matching compiler bundle did not make automatic framework discovery work. Explicitly calling
`xpc_add_bundle` with the physical RuntimeRoot `MTLCompilerService.xpc` path did make simulator launchd spawn the compiler.
This is guest PID-domain registration; it does not register a native system service or move the compiler into the isolated
native GPU helper.

The iOS 26 libxpc image callback at offset `0x1b68c` forwards each eligible dyld image path unchanged with import value 2.
The rebuilt cache names Metal `/System/Library/Frameworks/Metal.framework/Metal`, not its physical app-group path. Initial
system-framework paths are deferred until the PID domain is uncorked. Launchd's cache miss falls back to libxpc bundle
resolution, which uses `getattrlist`, a dyld shared-cache real-path fallback, and ordinary filesystem bundle/resource reads;
this path has no RuntimeRoot prefix substitution. Its framework cache is also keyed by the image path. These paths explain
why merely deploying resources under RuntimeRoot is insufficient; a device trace of the automatic import's final error
has not been captured. No environment variable selecting an additional compiler bundle was found in this libxpc build.
`IPHONE_SIMULATOR_ROOT` is used there for SystemVersion.plist, not this registration path.

The narrow C compatibility hook in `Guest/MetalCompilerRegistration.c` now runs from the inserted guest shim's constructor,
after its libSystem dependency has initialized and after the native-cache handoff. Only the generated backboardd job sets
`LSB_REGISTER_METAL_COMPILER=1`. The hook also requires the actual canonical `_NSGetExecutablePath` to equal canonical
`DYLD_ROOT_PATH` plus `/usr/libexec/backboardd`, so an inherited variable cannot register a domain from launchd, xpcproxy,
the compiler, or another executable. It resolves the one fixed compiler bundle inside that runtime root, rejects a bundle
symlink escaping the root, and requires nonempty regular readable `Info.plist` and `MTLCompilerService` files without following
their final symlinks. Missing or invalid files produce an errno log and skip registration. There is no general plugin scan.

The hook uses the normally declared C API `xpc_add_bundle(path, 2)`. In this libxpc build, flag 2 becomes import value `0x1001`;
the API uncorks the domain and sends routine 800 with `type=5`, the caller's PID as `handle`, and the physical bundle path.
It registers the service for demand launch rather than starting it directly. The API returns void and can abort on certain
domain errors; the log therefore says only that the call returned, with success unconfirmed. It has no added retry, timeout,
queue, or listener. A new backboardd replacement process runs its own constructor; service/domain lifetime remains managed
by simulator launchd. The constructor is deliberately not called from a dyld image callback.

Focused tests cover exact opt-in, canonical role/path checks, sibling-root and symlink escapes, missing/special/empty files,
and the number/path/flags of actual registration requests through a mocked XPC entry point. The full simulator shim links.
This validates the integration mechanics. The explicit-registration probe and six-site launchd adapter now pass stock guest
compiler launch and shader compilation on the phone. The later stock backboardd run also passes constructor registration
and reaches its first presented frame. Relevant disassemblies are `tmp/backboardd/libxpc-sim-disassembly.txt` (`_xpc_add_bundle` at `0x243a4`,
`__xpc_bootstrap_services` at `0x1aa84`, `__xpc_bundle_resolve_sync` at `0x29ac4`) and
`tmp/backboardd/launchd-disassembly.txt` (cache fallback at `0x10000b3f4`–`0x10000b42c`).

## Simulator framework resource lookup

Stock backboardd **81599** passed the earlier SystemWake assertion with the opt-in [system power adapter](system-power.md),
created its CAWindowServer and swapchain, then aborted in `CA::OGL::MetalContext::create_fragment_shader`:
`Metal failed to specialize fragment function`, with `Function fixed_frag_lpf_cpf was not found in the library`.
Deploying the matching simulator QuartzCore resources for a subsequent trial did not resolve this error: Foundation still
resolved the framework bundle from its absolute shared-cache image name. The failure was resource selection, before
specialization could use the compiler.

A separate phone diagnostic compared normal `[NSBundle bundleForClass:CALayer.class]` resource lookup with the explicit
RuntimeRoot URL through the same `newLibraryWithURL:error:` API:

| Selected QuartzCore `default.metallib` | Bytes | Function count | `fixed_frag_lpf_cpf` |
| --- | ---: | ---: | --- |
| Native `/System/Library/Frameworks/QuartzCore.framework` | 23,192,352 | 189 | Missing |
| Matching iOS 26 simulator framework under RuntimeRoot | 1,662,256 | 175 | Present |

Both libraries loaded successfully on the simulator Metal device. This explains why successful bundle, URL and library
creation did not catch the mismatch. The original comparison is `tmp/metal-iosurface/guest-quartzcore-library.log`.
The minimal matching resource payload is QuartzCore's `Info.plist` (783 bytes) and `default.metallib` (1,662,256 bytes),
preserving their framework-relative paths. Their hashes and source paths are in
`tmp/metal-iosurface/quartzcore-resources.json`; the executable remains in the rebuilt cache.

In the inspected iOS 26 Foundation, `+[NSBundle bundleForClass:]` obtains the class image name, derives the framework path,
then caches an NSBundle by path. Merely creating a physical CFBundle or NSBundle in advance does not override that choice;
all four native seed variants (`none`, `cf`, `ns`, `both`) still selected the original class-image bundle. The normal bundle's
lazy `-[NSBundle _cfBundle]` calls public `CFBundleCreate` at Foundation offset `0x7a0d48`. Its bundle URL, identifier and
modern resource methods then use that CFBundle. This provides the narrow C integration point.

`Guest/FrameworkBundleCompatibility.c` interposes only `CFBundleCreate`. With exact opt-in
`LSB_SIMULATE_FRAMEWORK_BUNDLES=1`, a file URL naming a `.framework` under `/System/Library/Frameworks/` or
`/System/Library/PrivateFrameworks/` can resolve to its existing counterpart under canonical `DYLD_ROOT_PATH`. Nested
frameworks are supported. The candidate must remain inside that root and one of those two namespaces after symlink
resolution, be a directory, and contain readable regular `Info.plist` metadata directly or through its versioned Resources
layout. Traversal components, escaping directory or metadata symlinks, invalid roots and incomplete bundles are rejected.
An ineligible or absent candidate forwards the original URL unchanged. Once a valid redirected URL is selected, a failed
CFBundle creation is returned without retrying against host resources.

The backboardd job enables this flag before the inserted shim initializes. The adapter uses C/CoreFoundation APIs, a
thread-local recursion guard, and ordinary dyld interposition; it does not replace Objective-C methods, class image names
or filesystem calls. It must be active before Foundation initializes the relevant cached CFBundle. Already-created bundle
objects are not invalidated. The private `_CFBundleCreateUnique` path is not interposed because the observed normal lookup
does not use it. This is scoped bundle selection, not general RuntimeRoot filesystem virtualization.

The signed phone trial used host **81917**, helper **81919**, simulator launchd **81920**, and native diagnostic guest
**81923**, retaining logical PID **81922**. Normal CALayer `bundleURL` and the `default.metallib` resource URL now both named
the physical RuntimeRoot framework; its identifier was `com.apple.QuartzCore`. Both normal and explicit library loads
returned the same **175** function names and included `fixed_frag_lpf_cpf`; the broker recorded exit status **0**. The bundle's
`executableURL` was nil because only resources exist at that location and framework code is cache-only. This did not prevent
the tested metadata/resource lookup. Evidence is `tmp/metal-iosurface/guest-quartzcore-redirect.log` and
`tmp/metal-native/console-quartzcore-redirect.log`. No shader specialization, GPU submission or stock backboardd frame was
attempted by this diagnostic.

Focused tests cover opt-in and namespace boundaries, missing/invalid metadata, nested and versioned frameworks, symlink
containment, exact fallback, recursion, concurrent creation and preservation of a failed redirected creation. Native
`DYLD_INSERT_LIBRARIES` testing verifies actual CALayer NSBundle URL, identifier and resource lookup through the production
interposer. The focused tests pass normally, under ASan/UBSan and under TSan; the C source also links for arm64 iOS Simulator.
Local Foundation disassemblies and seed-probe results are in `tmp/metal-iosurface/foundation-bundle-lookup.txt`,
`foundation-cfbundle.txt`, `foundation-bundle-resource.txt`, and `quartzcore-bundle-seed-native.log`.
