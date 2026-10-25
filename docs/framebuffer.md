# Simulator framebuffer provider and service

These findings describe the installed CoreSimulator 1171.7 framework inspected on 2026-10-25. The stock simulator client
is being used; a host framebuffer server is **not implemented**. The wire layout below comes from local disassembly and
stock macOS serialization, not an end-to-end display test. IOSurface remote allocation and Metal execution remain separate,
unverified services. See [backboardd startup](backboardd.md#framebuffer-provider-selection) for the phone evidence.

The provider phone trial with backboardd 81070 successfully created an SFB connection object. `SFBConnectionConnect`
returned false with a nonnull error object, but backboardd continued. The bootstrap lookup's exact error was not captured.
It then stopped on the missing BackBoard event-processor configuration file. Thus the provider selection is verified and
the connection failure is nonfatal on this path; no successful framebuffer server exchange or GPU request was observed.

## Provider selection

The iOS 26 `SimFramebufferClient.framework` in the rebuilt cache is a trampoline. Its initializer at stock file offset
`0x4768` calls `dlopen(getenv("SIMULATOR_FRAMEBUFFER_FRAMEWORK"), RTLD_LAZY)` and resolves its public functions with dlsym.
When the variable is absent, dlopen receives NULL and the lookup finds the trampoline's own exports. Backboardd 81009
therefore repeatedly tail-called `SFBConnectionCreate` at cache address `0x13abcbc90`, reached from
`-[CAWindowServer _detectSimDisplays]`. It was not waiting for a service at that point.

The actual arm64 simulator provider is installed on the Mac at:

```text
/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer
```

Its UUID is `43D9592A-0F1F-3ACA-8173-D497AB5F16BB`; the original file's SHA-256 is
`f1f396e7a973e1a6953157ed66e8eb6151dac5a2df66a844fe94a9f659359d00`. It has iOS Simulator platform metadata, minimum OS 15.0,
and SDK 27.0. Its dependencies are CoreFoundation, Foundation, IOSurface, libobjc and libSystem. The host configuration
selects the signed copy at `<RuntimeRoot>/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer` for backboardd.
Static import availability does not guarantee every cross-version behavior.

The installed macOS **SimFramebufferServer** is different. It depends on Swift, ROCKit, CoreSimDeviceIO,
SimStreamProcessorObjects, CoreSimulatorUtilities, Metal and macOS framework install paths. Copying that server into the
native iOS host is not established as workable. Its display/session handling is useful protocol evidence.

The local Sim26Booter `SimFramebuffer/SimFramebuffer.m` supplies a replacement client, bypassing this Mach service. It
reports one local `PurpleMain` display, returns successful connect calls, and uses an IOSurface `SwapCmd` value for its
presentation path. Many other exports abort, and swap callbacks return failure. Reusing those stubs would not preserve the
stock client's complete behavior; the current direction is the stock provider with a host service.

## Initial service exchange

`_SFBGetServerPort` at provider offset `0x9974` performs:

```c
bootstrap_look_up(bootstrap_port, "com.apple.CoreSimulator.SimFramebufferServer", &port);
```

This is synchronous Mach messaging, not XPC. The startup sequence visible in the provider is:

1. `SFBConnectionCreate` creates a local CF object. `SFBConnectionConnect` checks that the framebuffer environment is enabled.
2. Connect allocates a receive right for asynchronous server events, obtains the service send right, and creates a Mach-send
   dispatch source for server death. It sends message ID **1** with a check-in item, the client name, and an event-port
   descriptor. The observed caller's name is `CAWindowServer`.
3. Connect waits for a synchronous reply. A type **18** item carries a 16-byte session identifier and marks the connection
   successful. A type **17** item supplies a structured error. Success installs the event receive source.
4. `SFBConnectionCopyDisplays` sends message ID **2**, with that session ID. The reply supplies display property and mode
   items, which become CF display objects. The server writes each display's properties followed by its modes and optional
   extended properties/mask information.

The server's reply constructor copies the request's Mach message ID; it does not add a MIG reply offset. The provider's
`simFramebufferMessageSendWithReply` requests a MAKE_SEND_ONCE reply right and waits without an explicit timeout. A future
host service must answer a valid supported request or send a valid error, rather than leave the client waiting indefinitely.

## Observed wire ABI

The structures are little endian. They use explicit packed widths; normal compiler alignment would change several fields.
The serializer always marks sent messages complex, even when the descriptor count is zero. Requests use COPY_SEND for the
destination and MAKE_SEND_ONCE for the reply port; replies use MOVE_SEND_ONCE. The check-in's port descriptor uses MAKE_SEND
on the client's event receive right, so the server receives a send right.

For descriptor count `n`, let `d = 12*n + 4*(n & 1)`. The observed envelope is:

| Offset | Field |
| --- | --- |
| `0x00` | Standard 24-byte `mach_msg_header_t` |
| `0x18` | `uint32_t` descriptor count |
| `0x1c` | `n` standard 12-byte port descriptors, then 4 bytes of padding when `n` is odd |
| `0x1c + d` | `uint64_t` magic `0x00ace0ace0face01` |
| `0x24 + d` | 16-byte session ID |
| `0x34 + d` | `uint32_t` item count |
| `0x38 + d` | First data item |

Each item begins with `uint64_t magic = 0xfadecafe00007777`, `uint32_t type`, `uint32_t stride`, then its payload. The
inspected serializer appends fixed `0x108`-byte items and writes that stride, leaving unused payload bytes zero. Its initial
message size is 76 bytes, so the emitted size is `76 + d + 0x108*itemCount`: the buffers contain 20 additional zero bytes
after the enumerated item area. Those bytes are inside `msgh_size`; they are not a received Mach audit trailer. Do not
interpret them as another item. The stock enumerator follows the declared count and each stride.

The native serialization-only probe produced these sizes:

| Buffer | Message ID | Descriptors | Items | Size |
| --- | ---: | ---: | ---: | ---: |
| Check-in | 1 | 1 | 1, type 14 | 356 |
| Display-list request | 2 | 0 | 0 | 76 |
| Check-in reply | 1 | 0 | 1, type 18 | 340 |
| POSIX error reply | 1 | 0 | 1, type 17 | 340 |

The check-in type-14 payload begins with a `uint32_t` descriptor index, another zero-initialized `uint32_t`, and a 64-byte
NUL-terminated name. The descriptor index identifies the event port; it is not a port name embedded in the payload.
The type-18 payload contains the 16-byte session identifier.

Type **4** contains a packed 244-byte display-properties payload. Getter and constructor disassembly establish these fields:

| Payload offset | Type | Meaning |
| --- | --- | --- |
| `0x00`, `0x40`, `0x80` | Three `char[64]` | Display UID, display name, device name |
| `0xc0` | `uint64_t` | Supported surface flags |
| `0xc8` | `uint64_t` | Display flags; bit 0 supplies power state |
| `0xd0` | `uint32_t` | Supported presentation modes |
| `0xd4`, `0xd8` | Two `uint32_t` | Device width and height |
| `0xdc`, `0xe0` | Two `uint32_t` | Current canvas width and height |
| `0xe4`, `0xe8`, `0xec` | Three `uint32_t` | Display ID, type, dot pitch |
| `0xf0`, `0xf2` | Two `uint16_t` | Maximum layer count and swapchain count |

Type **5** contains a packed **35-byte** mode, with eight consecutive `uint32_t` fields followed by three `uint8_t` flags.
The first three fields are the mode ID, width, and height. `_SFBDisplayAddMode` uses the first final byte at `0x20` to select
the preferred mode and the last at `0x22` to select the current mode. The remaining enum meanings and supported combinations
need validation before advertising them. In particular, do not treat the 35-byte mode as a CGSize or substitute a pointer
return for the real output argument: `SFBDisplayGetCurrentMode(display, modeOut)` fills a caller-provided structure.

Type **17** contains a packed 156-byte error payload: `char description[140]`, a `uint64_t` error/domain discriminator at
`0x8c`, and an eight-byte union at `0x94`. `_SFBErrorCreateFromSimReplyError` interprets discriminator **1000** as Mach with a
signed 32-bit result, **1001** as POSIX with a signed 64-bit result, and other nonzero values as SFB errors. Zero produces no
CFError. The builder's POSIX input is an `int *` which it sign-extends into that 64-bit wire field. A POSIX ENOTSUP response
can therefore use discriminator 1001, value 45 on Darwin, and a bounded NUL-terminated description. A failed check-in without
an actual error item can trigger the client's `!error || *error` assertion; an empty success-shaped reply is not a substitute.

## Host service requirements, not implemented behavior

The existing `LSBLaunchdRegisterEndpoint` helper can register a host-owned send right under the framebuffer service name in
the isolated simulator launchd namespace. It already uses a bounded registration transaction authenticated against
launchd's native audit token. Registration should complete before releasing the backboardd job, as for Indigo. It must not
replace or check in to a system-owned framebuffer service.

A server should authenticate every received native audit trailer against the broker-authorized backboardd instance. The
logical PID and a client-supplied name are insufficient. Generate a fresh unpredictable session ID on check-in, bind it to
that exact audit identity and event send right, and reject stale sessions or cross-client reuse. Define duplicate check-in
semantics explicitly instead of silently replacing an active event port. Reject unknown/malformed descriptor layouts before
retaining rights, and bound descriptor counts, item counts, strides, strings, message size and queue draining. Apple's helper
functions contain assertions and do not constitute a defensive host-side parser.

On cancellation, job exit, authorization revocation or dead event port, invalidate the session, pending replies, display
state, swapchains, surface rights and callbacks together. Asynchronous callbacks must carry a connection generation so old
work cannot target a later client. Reuse the existing restriction on exec by an authorized graphics/input owner until a
complete ownership handoff exists. Always consume or destroy transferred Mach rights, including failed reply send-once
rights. Valid unsupported requests should receive structured errors without pretending to allocate resources.

Check-in and a coherent single-display list are only the first stage. Subsequent calls create swapchains, change display
state, transfer IOSurface Mach ports and fence descriptors, and expect completion callbacks on the event port. The client
contains `IOSurfaceCreateMachPort` and asynchronous callback handling; a successful display enumeration alone does not make
guest frames available to `LSBSimulatorDisplayView`. Surface ownership, format/stride validation, fence completion and
bounded buffering must be implemented before connecting those submissions to the native view. No Metal or IOSurface
remote-service request is established by the initial self-tailcall failure.

## Reproducing the inspection

The experimental scripts and generated binary analysis remain in ignored `tmp/backboardd/`. They are inspection tools, not
runtime components. From the repository root:

```sh
SFB_CORE=/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A
SFB_CLIENT="$SFB_CORE/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer"
xcrun otool -tvV "$SFB_CLIENT" > tmp/backboardd/simframebuffer-provider-disassembly.txt
xcrun nm -nm "$SFB_CLIENT"
xcrun otool -L "$SFB_CORE/Frameworks/SimFramebufferServer.framework/Versions/A/SimFramebufferServer"
python3 tmp/backboardd/inspect-framebuffer-wire.py > tmp/backboardd/framebuffer-wire.json
```

The Python probe loads the **macOS** common SimFramebuffer library, constructs check-in, display-request, check-in-reply and
POSIX-error buffers, then reads their Mach headers. It never sends a message or opens a service; its placeholder descriptor
number is serialized only. The corresponding simulator builder/enumerator disassembly agrees with these widths and offsets.
Relevant provider offsets are `0x67c0` (create), `0x6c98` (enumerate), `0x6e50` (send/wait), `0x7444` (reply), `0x7580`
(check-in), `0x76a0` (check-in reply), `0x7750` (error), `0x8354` (append items/descriptors), `0x979c` (decode error), `0x9974`
(service lookup), `0xb5c8` (connect), and `0xde18` (display-list request). These offsets are version-specific.
