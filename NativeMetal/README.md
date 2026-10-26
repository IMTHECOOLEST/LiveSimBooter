# Isolated native Metal backend

This backend runs only in separate native LiveProcess instances that retain the host shared cache. Each instance's Metal
device and all system frameworks come from native iOS. The UIKit host and simulator guest do not load these adapted
backend libraries. Graphics sessions now use a public helper for `com.apple.metal.simulator` and a dedicated backboardd
helper. Selecting stock SpringBoard adds a third, dedicated SpringBoard helper. The public helper accepts any client
holding its endpoint; dedicated helpers still require their owner's exact full audit token. Each helper has its own native
process, listener and stock process-reference map. Public IOSurface access remains in the UIKit host.

This public-service change has local integration coverage described below. The phone results in the following paragraphs
predate it and preserve the evidence for the earlier one- and two-helper implementations.

Host **97774** started native helpers **97775** for backboardd and **97776** for SpringBoard. All six registrations returned
error/status **0**, and guest backboardd **97796** and SpringBoard **97795** each opened three accepted Metal connections.
Backboardd registered a **1290 × 2796** CAWindowServer framebuffer and connected to IOSurface as owner **22**. SpringBoard
exited **0** about a second later; its helper retired while backboardd and the other helper survived. SpringBoard drawing,
boot completion and SpringBoard IOSurface IPC were not observed in that first trial. The then-current two-owner IOSurface
contract was verified locally; this historical exit preceded the Home Screen result below. Evidence:
`tmp/lsd-services/console-springboard-metal.log`.

The native construction probe passed on iPhone16,2 / iOS 26.1 in process **81429**: it obtained an `AGXG16Device` for the
A17 Pro, loaded the two adapted libraries, read deserializer version **9**, and created/released a deserializer. This proves
basic native-device compatibility. A later service trial used host **81448**, helper **81449** and native guest **81452**.
All five host endpoints registered, three Metal connections passed exact-audit authorization, and the guest obtained
`Apple iOS simulator GPU` plus a command queue. `newLibraryWithSource` then failed with `MTLLibraryErrorDomain` code 3,
`Compiler encountered XPC_ERROR_CONNECTION_INVALID`. The guest exited 1 and the broker revoked/terminated the helper.
Those initial logs are `tmp/metal-native/console-service.log` and the PID 81452 section of
`tmp/metal-iosurface/guest-service.log`. A later clear-only trial with host **81535**, native helper **81536** and native guest
**81539** completed GPU work, read back center/corner BGRA **64,128,191,255**, wrote a PNG and exited 0. The helper acquired
PlugInKit assertion flags **39 (`7 | 0x20`, `PreventThrottleDownUI`)**; ordinary guests retained flags 7. Evidence is
`tmp/metal-native/console-clear-gpu-assertion.log` and the PID 81539 section of
`tmp/metal-iosurface/guest-clear-gpu-assertion.log`. The latest trial with host **81793**, helper **81794**, native guest
**81797** and stock guest compiler **81799** passed source compilation and pipeline creation after explicit compiler bundle
registration and the six-site launchd identity adapter. The GPU rendered a triangle; CPU readback found center BGRA
**127,65,63,255** and corner **0,0,0,255**. The guest wrote a verified PNG and exited 0. Evidence is
`tmp/metal-native/console-compiler-sixth.log`, the **81797** section of `tmp/metal-iosurface/guest-compiler-sixth.log`, and
`tmp/metal-iosurface/phone-triangle.png`. Stock backboardd **82143** subsequently presented its first real frame to the host
image view: a spinner on black. The next stock run reached 300 frames with dirty-rectangle composition before aborting on
an Indigo HID target mismatch. Apple-logo/progress rendering and a complete simulator boot remain unverified.
See [the stock frame results](../docs/backboardd.md#first-stock-backboardd-frame).
See [the Metal investigation](../docs/metal.md) for the discovery path and earlier IOSurface results.

The later Solarium-enabled SpringBoard trial found a separate guest compiler-discovery gap. Native SpringBoard **3722**
loaded the correct SiriUICore `default.metallib`, then `siriChipVert` specialization returned nil with
`MTLLibraryErrorDomain` code **3**: `MTLCompilerService` lookup failed with error 3, `No such process`. The temporary
diagnostic preserved the original API results; the library and its function names were present. The guest shim now imports
the fixed compiler bundle for the exact host-selected SpringBoard executable as well as backboardd, using the existing
`xpc_add_bundle(path, 2)` API. This compiler remains a simulator service managed by simulator launchd; it is independent
of the isolated native GPU helper and ExtensionKit registration. Focused registration/configuration tests and registration
ASan/UBSan pass. Production SpringBoard **3998** then passed the Siri path and, after the matching AutoFillUI binary was
staged, displayed the Home Screen without the temporary diagnostic. The screenshot has Calendar artwork and changed
Solarium dock/search styling, but most icons remain placeholders and widget tiles are blank; complete visual fidelity is
not established. Evidence: `tmp/autofillui-stage/guest-wifi.log`, `console-wifi.log`, and `springboard-wifi.png`. See
[the captured failure and registration fix](../docs/metal.md#springboard-compiler-registration).

A subsequent stock global IconServices job reached the generic Metal endpoint in native agents **4063**, **4083**,
**4087**, and **4089** and was rejected by the then-current backboardd-only authorization. Its guest log also identified
missing simulator CoreImage plugin binaries, including `CIVisionFilters`, `CIBarcode`, `PortraitFilters`, `CIPassThrough`,
and `CIInpainting`; copied plugin resources alone cannot load these filters. That experiment did not repair the placeholders
and left the job out of its final selection. The public helper now removes that admission restriction; it does not supply
missing guest code or itself prove successful icon rendering. Evidence from the earlier experiment:
`tmp/iconservices-stage/console-wifi.log` and `guest-wifi.log`.

## Host-selected execution roles

`--native-metal-probe` selects role 1 for the construction-only test. Graphics launch modes automatically start two
role-2 helpers, `LSB_EXECUTION_NATIVE_METAL_SERVICE`, before simulator launchd: public and backboardd. The selected-services
mode adds a third only when the host selects `com.apple.SpringBoard` and its generated job retains the exact label and
stock executable path. Roles and policies are selected by the native host, not guest argv, environment or ordinary spawn
RPCs. LiveProcess startup remains C; native Objective-C runs in the separate helpers with native frameworks and cache.

Each role-2 plan passes a separate unlinked 4 KiB control page at fixed fd 3 using a read-only descriptor. The broker owns
the writable mapping. Control ABI **2** has revoked, dedicated-owner and public-session modes: dedicated publication carries
the full kernel audit token, while public publication carries a zero audit token. The public mode is published before its
helper starts. `LSBNativeMetalControlPublish(control, NULL)` revokes either mode. Invalid modes, nonzero public/revoked audit
fields and older ABI versions fail validation. Rebuild and package `MetalNativeProbe.dylib` with the matching host ABI.

The backend imports its read-only mapping, then LiveProcess closes fd 3. Authenticated READY transfers the helper's endpoint
send right. Public and backboardd helpers must both be ready before launchd starts; a known SpringBoard-helper failure
allows the baseline session to continue with only that dedicated route disabled.

| Simulator service | Process and admission policy |
| --- | --- |
| `com.apple.metal.simulator` | Separate public native helper; any client holding the endpoint, up to 256 live peers |
| `com.apple.metal.simulator.backboardd` | Dedicated native helper; exact backboardd audit token, up to 64 live peers |
| `com.apple.metal.simulator.SpringBoard` | Dedicated native helper when selected; exact SpringBoard audit token, up to 64 live peers |
| `com.apple.IOSurface.Remote` | Native UIKit host's `IOSurfaceRemoteServer`; any client holding the endpoint, up to 64 live peers |

Each Metal helper has a separate PlugInKit instance UUID, GPU assertion, control file, listener and `processRef` map. These
endpoints are published into the simulator launchd session. Public admission requires no per-client broker registration;
it does not install a globally discoverable service in the phone's host bootstrap namespace. Backboardd proceeds after
five baseline registrations; SpringBoard's sixth registration has independent pending/error state within the same
30-second startup deadline. Indigo and framebuffer submission remain backboardd-owned.

IOSurface no longer has an owner allowlist. Its native server manages individual client connections, and session shutdown
cancels all tracked peers and shuts down the server. A client exiting or executing a replacement process does not revoke
other public clients. Weak tracking uses `allObjects.count`: `NSHashTable.count` can include zeroed weak entries and would
otherwise exhaust the limit after enough disconnected clients. Metal uses the same live-peer counting rule.

The endpoint transport is reconstructed from the 23A343 host `liblaunch_sim.dylib` original
`launch_sim_register_endpoint` → `launch_mig_bridge_register` path, MIG **701/801**. Our differently shaped
`LSB_launch_sim_register_endpoint` accepts a resolved service port and adds reply-audit validation and a bounded deadline;
see [the service-route details](../docs/metal.md#isolated-native-service-route).

The loader resolves only `Frameworks/NativeMetal/MetalNativeProbe.dylib` beneath the canonical extension executable directory,
checks the defining image for the required entry points, and rejects path escapes. A missing folder or image is a normal
preparation error, not a fallback to a guest-supplied path. The helper is independently monitored; it has no simulated
parent/child wait relationship with launchd. Other execution roles cannot submit a native endpoint in READY.

Assertion invalidation of a still-existing native role revokes that helper. A dedicated helper also stops its graphics
owner; public-helper failure in a running session marks its service unavailable without terminating unrelated clients. Termination
validates the full audit identity and can fall back to the original PlugInKit instance UUID; only `SIGKILL` remains available
on an invalidated GPU session. A vanished identity waits for its normal exit monitor instead. Local tests cover these paths;
device background/assertion-loss behavior remains untested.

## Reproduce the unsigned artifacts

From the repository root, use an original **iOS 26.0 build 23A343** simulator RuntimeRoot and a new output folder:

```sh
python3 Tools/prepare-native-metal.py \
  --runtime-root '/Library/Developer/CoreSimulator/Volumes/iOS_23A343/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot' \
  --output-dir tmp/native-metal-build \
  --self-test
```

The tool requires Xcode's `vtool`, `install_name_tool`, `dyld_info` and iPhoneOS clang/SDK. It produces:

| Artifact | Purpose |
| --- | --- |
| `MetalSerializer.dylib` | Original simulator serializer/deserializer with native iOS metadata |
| `MTLSimImplementation.dylib` | Original server with native iOS metadata and a peer-handler export alias |
| `MetalNativeProbe.dylib` | Repository probe, service wrapper and authorization-reader implementation |
| `manifest.json` | Source/output hashes, section checks, export tables, source hashes and build commands |

All three dylibs require signing before loading. The tool performs no signing, app staging, installation or device actions.
It refuses an existing output folder, including one containing previously signed artifacts. Signed copies belong at
`LiveProcess.appex/Frameworks/NativeMetal`; the native-role loader obtains this fixed location from its executable path.
The adapted implementation's companion dependency is `@loader_path/MetalSerializer.dylib`. Other dependencies remain the
original system install names and resolve against the native cache in this process.

Both original files are pinned by SHA-256. The tool verifies their UUIDs and all file-backed section bytes after metadata
adaptation, requires the expected iOS 26 platform/minimum/SDK fields and exact install-name/dependency transformations,
then removes stale signatures. It does not pin an intermediate signature, which may change independently of the code.
The self-test exercises export-trie roundtrips, malformed input rejection, source/code/platform mismatch rejection, and
acceptance of an otherwise identical input with different signature bytes.

## Sign and package

Build and sign the app normally first, preserving its App Group provisioning. Stage a separate copy of that signed app,
copy the three prepared dylibs into its extension, sign those dylibs, then re-sign the extension and enclosing app in that
order. The native backend is not an automatic Xcode build product or a repository binary asset. For example:

```sh
LSB_BUILT_APP="$PWD/tmp/multiprocess-device-build/Build/Products/Debug-iphoneos/LiveSimBooter.app"
LSB_STAGED_APP="$PWD/tmp/native-metal-package/LiveSimBooter.app"
LSB_SIGNING_IDENTITY='D7610EDC80D34A9B1A3D3C8563D66A5CD4716293'
ditto "$LSB_BUILT_APP" "$LSB_STAGED_APP"
LSB_NATIVE_BUNDLE="$LSB_STAGED_APP/PlugIns/LiveProcess.appex/Frameworks/NativeMetal"
mkdir -p "$LSB_NATIVE_BUNDLE"
for library in MetalSerializer MTLSimImplementation MetalNativeProbe; do
  cp "tmp/native-metal-build/$library.dylib" "$LSB_NATIVE_BUNDLE/$library.dylib"
  codesign --force --sign "$LSB_SIGNING_IDENTITY" --identifier com.kdt.LiveSimBooter \
    --timestamp=none "$LSB_NATIVE_BUNDLE/$library.dylib"
done
codesign --force --sign "$LSB_SIGNING_IDENTITY" --preserve-metadata=identifier,entitlements \
  --timestamp=none "$LSB_STAGED_APP/PlugIns/LiveProcess.appex"
codesign --force --sign "$LSB_SIGNING_IDENTITY" --preserve-metadata=identifier,entitlements \
  --timestamp=none "$LSB_STAGED_APP"
codesign --verify --strict --deep "$LSB_STAGED_APP"
```

Use a fresh staging directory and a currently provisioned signing identity. The dylibs receive no application entitlements;
the extension and app retain their own existing identifiers/entitlements. Install the staged app and launch
`--native-metal-probe` for construction, or `--launchd-backboardd` for the native service plus simulator session. The guest
RuntimeRoot separately needs the matching simulator `MTLSimDriver` and `MetalSerializer` files described in
[metal.md](../docs/metal.md). Preparing, signing and packaging the native helper does not install those guest dependencies.

## The exported peer adapter

The original implementation exports only `init_with_xpc_connection`. That initializer installs an unconditional listener
handler; replacing it requires an ordinary callable entry point for admitted peers. The preparation tool adds the
export name **`LSBMTLSimulatorAcceptPeer`** to the existing local function without changing executable instructions:

```c
void LSBMTLSimulatorAcceptPeer(xpc_connection_t peer);
void init_with_xpc_connection(xpc_connection_t listener, uint64_t registryID, uint64_t unused);
```

The third initializer argument is zero in the stock host and unused by this implementation. The peer adapter's original
Mach-O symbol is `__Z23MTLSimulatorHandleEventPU24objcproto13OS_xpc_object8NSObject`, at VM/file offset **0x2ae4**, length
**196 bytes**, in the implementation UUID **04F76BD9-B7BB-3B5E-A562-C260F8D3D1CF**. Its body SHA-256 is:

```text
2d7059957d1f7aec23f5ab627f840e3b0b3d5dedb13ae778b6ad75c6fc5d6fa3
```

The tool checks this body, the initializer at `0x3198`, and its two-instruction original listener block at `0x31d4`.
It preserves every existing export terminal, writes a replacement export trie in `__LINKEDIT`, updates its metadata,
and removes the old final signature blob and `LC_CODE_SIGNATURE` command. An empty signature command is rejected by
`codesign`, so the remaining load commands are compacted and the freed header padding is zeroed. All **25** implementation
sections and **24** serializer sections remain byte-identical to their originals. `dyld_info -exports` independently
confirms the new alias and original initializer address.
The alias does not require a new symbol-table entry; `dlsym` resolves its export-trie entry.
Ad-hoc signing and `codesign --verify --strict` passed on disposable copies of all three outputs; the preparation tool
itself performs neither operation.

## Listener and lifetime requirements

Create an anonymous listener with its serial control queue already selected. Call
`xpc_connection_enable_sim2host_4sim(listener)` **before** the stock initializer, because the initializer activates it.
This native/simulator libxpc pair requires mode **2**; native default mode 0 does not decode the simulator's messages.
The clear-only and triangle phone trials verify this framing through peer handshakes, GPU command completion and IOSurface
pixel readback. The triangle trial also verifies stock guest shader compilation and pipeline creation. A later stock
backboardd trial verifies the first real framebuffer presentation, and the next run completed 300 frames with partial updates
before a separate input exception ended that process.

Call the initializer once with the native device's registry ID. It stores that value globally, retains the listener and
activates it. Replace its listener handler with the authorization wrapper while the anonymous endpoint is still private,
then publish the listener port. Do not change the target queue after activation. The SDK documents event-handler replacement
as asynchronous and non-preemptive, so this sequence depends on withholding the endpoint until the wrapper is installed.

The listener wrapper checks its control page before forwarding a connection through the alias. Dedicated mode compares
the complete native audit token; public mode accepts any peer while the session authorization remains active.
The alias owns the peer's context, creates a separate serial connection queue, installs its protocol handler and activates
the peer. The wrapper must not overwrite that context or change its handler/queue afterward. A rejected incoming peer may
be canceled directly; XPC disposes of it. The stock invalidation handler releases its queue and application-context
reference, deletes the context, then sets the connection context to NULL.

Global application state is initialized lazily through `dispatch_once` when the first handshake arrives. Connection type 0
creates an application context and returns a `processRef`; types 1 and 2 retrieve that same context from the process-global
map. Each type-0 handshake creates a separate application context, so one stock server can support multiple client
contexts; this is the public helper's model. The stock driver normally opens all three connections for its own context.
The reply includes serializer version 9, native registry ID and the native feature profile; no invented reply is needed.
The public helper deliberately shares one server and failure domain across its clients. Dedicated backboardd and SpringBoard
maps remain in separate processes.

Keep a helper's original policy and control generation for its lifetime. A changed or revoked generation permanently
retires it; do not repurpose an existing map while old peers are being cleaned up asynchronously. Dedicated helpers retire
when their owner exits; the public helper lives until session shutdown and does not bind an individual client to its old
process during exec replacement. Authorization is checked on admission and polled every 100 ms for cancellation. Polling is
not a hard real-time deadline and cannot undo submitted GPU work; the broker also terminates a revoked helper.

Local tests cover public/dedicated control publication, malformed ABI rejection, cross-process read-only imports and
revocation; broker startup failures, endpoint ownership, delayed registration after helper death, client exit/exec and
session teardown; and the real native IOSurface server with three unregistered processes, shared pixels, the live-peer
limit, recovered capacity after disconnect and shutdown. These tests do not establish successful public Metal rendering
or icon artwork on the phone.

The broker still refuses a second owner within a dedicated helper and rejects exec by either dedicated graphics owner.
SpringBoard helper/owner failure retires that pair while backboardd and public services continue; the failed dedicated
route stays unavailable until a new session. IOSurface now has no per-owner authorization to revoke. The historical phone
trial with SpringBoard **97795** verified normal-exit cleanup while backboardd survived; it preceded public-service access.

The driver first looks up `com.apple.metal.simulator.<getprogname()>`, then falls back to `com.apple.metal.simulator`.
The broker publishes all configured endpoints inside simulator launchd's namespace after helper READY. Public authorization
is already active; dedicated authorization remains revoked until the owner's authenticated CHECKIN, before its launch
plan is released. Preserve kernel audit tokens; guest PID translation does not apply to dedicated admission checks.

The optional stock `reportLeaks` operation invokes fork/exec for `/usr/bin/leaks` and is not supported in this jailed helper.
The normal device/resource/command path does not use it. The wrapper delegates the stock protocol after admission; it is
not a per-message protocol validator or an implementation of unavailable diagnostic operations.
