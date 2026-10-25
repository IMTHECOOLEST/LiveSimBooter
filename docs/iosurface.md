# Native IOSurface remote service

The native host uses `IOSurfaceRemoteServer` from Apple's IOSurface.framework, as in Sim26Booter's SimRenderServer.
`Host/PrivateIOSurface.h` declares its Objective-C interface; `Host/IOSurfaceServer.m` calls it normally. LiveProcess and
the guest shim remain C. The native service keeps the host's libraries and shared cache.

An anonymous XPC listener supplies the send right registered as `com.apple.IOSurface.Remote` in the isolated simulator
launchd namespace. The host replaces only the listener's accept handler. It compares each connection's kernel audit token
with the broker-authorized backboardd identity before passing that connection to the framework's original
`_handleClientConnection:` method. Apple handles the surface allocation, lookup and state protocol. No libxpc fields,
IOSurface internals or system framebuffer are patched.

Revocation cancels accepted connections on the framework's serial queue. Existing queued replies may complete before XPC
delivers cancellation; later connection attempts require a new authorization. Weak connection tracking bounds the number
of live peers to eight without permanently retaining disconnected peers. Shutdown cancels the peers and native server and
releases the registration send right. The framebuffer service independently authenticates surface submissions and copies
accepted pixels into the app's own view.

`Tests/IOSurfaceServerTests.m` exercises the actual native class through an XPC connection. Besides the `Method=0` limits
exchange, full-token rejection, same-identity continuity, revocation and shutdown, it performs the guest's allocation path:

1. Serialize a 64 × 32 BGRA property dictionary with the framework's `_ioSurfaceSerializeToXPCDictionaryWithKey` helper and
   send `Method=2`. Validate the returned 72-byte `SurfaceState` and map its `MemoryObject` shared memory.
2. Lock with `Method=4`, write a distinct value to every pixel using the returned stride and base offset, and unlock with
   `Method=5`.
3. Export the surface with `Method=14`, import its Mach send right with native `IOSurfaceLookupFromMachPort`, and verify the
   ID, dimensions, format and every pixel through the native IOSurface API.
4. Release the native surface, exported right and memory mapping, then balance the remote allocation with `Method=3`.

This passed on the Mac using the stock server and real IOSurface allocation, including after enabling sim-to-host mode on
both the listener and test client. The tool sandbox prevents native IOSurface allocation, including a direct
`IOSurfaceCreate`; these native tests must run outside that tool sandbox. Passing on macOS
does not by itself establish simulator-to-iPhone allocation compatibility.

The first phone trial under LLDB accepted three IOSurface connections from the authorized backboardd process
(`tmp/framebuffer-server/console-initial-lldb.log`, native PID 81158, audit version 201865). It reported no IOSurface wire
error and produced no frame. That trial confirmed connection admission only. The same log records a separate unsupported
framebuffer swapchain request (`id=9`, `ENOTSUP`).

The later raw phone probe, native PID 81375, isolated a transport failure before allocation
(`tmp/metal-iosurface/guest-diagnostic.log`). `Method=0` and both `Method=2` requests immediately returned
`XPC_ERROR_CONNECTION_INTERRUPTED`; none returned an IOSurface `ErrorCode` or `MemoryObject`. The requests used the stock
simulator connection API and property serializer. The 64 × 32 test dictionary and the 256 × 256 probe dictionary failed in
the same way. The subsequent ordinary `IOSurfaceCreate` returned NULL. Connection admission alone therefore did not prove
that the framework could decode a request.

Disassembly of the matching native iOS 26.1 (23B50) and simulator iOS 26.0 (23A343) libxpc binaries explains this mismatch.
Their graph serializers and parsers use identical tables, indexed by the connection's platform mode:

| Mode | Graph magic | Use in the inspected binaries |
| --- | --- | --- |
| 0 | `0x40585043` | Native connection default |
| 1 | `0x7870633f` | Simulator connection default |
| 2 | `0x58504321` | Explicit sim-to-host connection |
| 3 | `0x42133742` | Additional table entry; unused by this service |

Both parsers require wire version 5 and compare the incoming magic strictly against their own connection mode. There is
no automatic conversion between modes 2 and 0. The simulator's `xpc_connection_enable_sim2host_4sim` sets mode 2; the same
exported native API also sets mode 2. A native anonymous listener left at its default mode 0 consequently rejects the
simulator's mode-2 graph before `IOSurfaceRemoteServer` can dispatch the method. Accepted native peers inherit their
listener's mode.

The host now calls `xpc_connection_enable_sim2host_4sim(_listener)` before `initWithListener:options:` activates the listener.
The test client calls the same API before activation. This uses the supplied connection API without editing private object
fields or rewriting message headers. The existing broker pipe adapter remains limited to its synchronous transport.
Sim26Booter's raw force-to-zero workaround does not match this pair of inspected libxpc binaries.

The native table is at original VM address `0x197cb5c50`, with the serializer lookup at `0x197c92718` and strict parser
comparison at `0x197c733a0`. The simulator equivalents are original offsets `0x33830`, `0x1c37c`, and `0x184c`.
The disassemblies are saved in `tmp/metal-iosurface/libxpc-native26-disassembly.txt` and
`tmp/backboardd/libxpc-sim-disassembly.txt`.

The corrected phone trial on 2026-10-25 confirms that the API change fixes the transport and allocation path. Read the
section beginning with native PID **81399** in `tmp/metal-iosurface/guest-xpc-mode.log`; the file also contains earlier
appended failures. With the native listener and simulator client both in mode 2:

| Request | Observed reply |
| --- | --- |
| `Method=0` | `ErrorCode=0`, 40-byte `Limits`, `ProtocolVersion=1` |
| `Method=2`, 64 × 32 BGRA | `ErrorCode=0`, 16,384-byte `MemoryObject`, 72-byte state, stride 256, base offset 0 |
| `Method=2`, 256 × 256 BGRA | `ErrorCode=0`, 262,144-byte `MemoryObject`, 72-byte state, stride 1,024, base offset 0 |
| `Method=3`, after each allocation | `ErrorCode=0` |

After those raw requests, ordinary simulator `IOSurfaceCreate` also succeeded: surface ID 33, 256 × 256 BGRA, stride 1,024,
allocation 262,144 bytes. Surface IDs were reused after release; the repeated ID does not indicate simultaneous ownership.
The subsequent `MTLCreateSystemDefaultDevice` returned nil, so this earlier trial proves guest allocation through the native
service, not GPU rendering or presentation. Later isolated native Metal service trials completed a GPU clear and then a
shader-based triangle using the stock guest compiler. The simulator guest verified the IOSurface pixels and wrote a PNG.
Stock backboardd subsequently presented a real spinner frame through the framebuffer service; see the current results in
[metal.md](metal.md) and [framebuffer.md](framebuffer.md#first-stock-phone-frame). A later run completed 300 frames with partial
updates before an Indigo HID exception aborted backboardd. Complete boot remains unverified.
