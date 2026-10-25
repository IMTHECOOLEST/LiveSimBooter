# Isolated native Metal backend

This backend runs only in a separate native LiveProcess that retains the host shared cache. Its Metal device and all system
frameworks come from native iOS. The UIKit host and simulator guest do not load these adapted backend libraries.

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

## Host-selected execution roles

`--native-metal-probe` selects role 1 for the construction-only test. `--launchd-backboardd` automatically starts role 2,
`LSB_EXECUTION_NATIVE_METAL_SERVICE`, before the simulator launchd process. Both roles are selected by the native host and
validated in the bootstrap reply; guest argv, environment and ordinary spawn RPCs cannot select them. LiveProcess startup
remains C. Native Objective-C is loaded only in the separate helper, which retains the host cache and native frameworks.

The role-2 plan passes an unlinked shared authorization page at fixed fd 3, using a read-only descriptor. The broker owns
the writable mapping and publishes the selected backboardd instance's full kernel audit token before releasing that guest.
The backend imports a read-only mapping, then LiveProcess closes fd 3. A successful service READY transfers an endpoint
send right; only then may launchd start. The broker publishes both `com.apple.metal.simulator` and
`com.apple.metal.simulator.backboardd` within that isolated namespace. Neither the native host nor other guests inherit
Metal authorization merely by discovering the service name.

The loader resolves only `Frameworks/NativeMetal/MetalNativeProbe.dylib` beneath the canonical extension executable directory,
checks the defining image for the required entry points, and rejects path escapes. A missing folder or image is a normal
preparation error, not a fallback to a guest-supplied path. The helper is independently monitored; it has no simulated
parent/child wait relationship with launchd. Other execution roles cannot submit a native endpoint in READY.

Assertion invalidation of a still-existing native role revokes the service session and stops its graphics owner. Termination
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
handler; replacing it requires an ordinary callable entry point for authenticated peers. The preparation tool adds the
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

The listener wrapper checks the full native audit token before forwarding an accepted connection through the alias.
The alias owns the peer's context, creates a separate serial connection queue, installs its protocol handler and activates
the peer. The wrapper must not overwrite that context or change its handler/queue afterward. A rejected incoming peer may
be canceled directly; XPC disposes of it. The stock invalidation handler releases its queue and application-context
reference, deletes the context, then sets the connection context to NULL.

Global application state is initialized lazily through `dispatch_once` when the first handshake arrives. Connection type 0
creates an application context and returns a `processRef`; types 1 and 2 retrieve that same context from the process-global
map. The stock driver normally opens all three connections. The reply includes serializer version 9, native registry ID
and the native feature profile; no invented reply is needed.

Bind each native helper to one owner for its lifetime. Revocation must cancel admitted connections and end that helper;
reuse for a different owner would overlap asynchronous cleanup with the old global `processRef` map. Authorization is
checked when a peer is admitted; later shared-control polling is a secondary revocation mechanism, not a hard real-time
deadline or a way to undo GPU operations already submitted. The broker must terminate the helper when its owner exits.
The backend retains global state and the listener without an exported shutdown API, making process exit the cleanup boundary.
The implementation permanently retires a helper after its admitted generation changes or becomes unreadable/revoked.
The broker refuses a second simultaneous owner, rejects exec by the active graphics owner, and marks dependent work failed
if the helper dies. Restart the host/session for a new owner; reauthorization is not a helper-reuse mechanism.

The driver first looks up `com.apple.metal.simulator.<getprogname()>`, then falls back to `com.apple.metal.simulator`.
The broker publishes the endpoint inside simulator launchd's namespace after helper READY, while authorization is still
revoked. It publishes the owner's native audit token only when that child's authenticated CHECKIN arrives, before releasing
its launch plan. Preserve kernel audit tokens; guest PID translation does not apply to this authentication step.

The optional stock `reportLeaks` operation invokes fork/exec for `/usr/bin/leaks` and is not supported in this jailed helper.
The normal device/resource/command path does not use it. The wrapper delegates the stock protocol after admission; it is
not a per-message protocol validator or an implementation of unavailable diagnostic operations.
