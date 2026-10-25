# Simulator framebuffer provider and service

These findings describe the installed CoreSimulator 1171.7 framework inspected on 2026-10-25. The stock simulator client
now connects to the native `LSBFramebufferServer`. The server implements authenticated check-in, one display and swapchain,
bounded immediate/delayed presentation, IOSurface import, producer fences, completion callbacks and cancellation. A real
stock macOS client has presented and verified surface pixels through this server in native tests. On the phone, stock
backboardd has now submitted a real frame that reached the host image view; its recovered PNG shows a spinner on black.
The first frame was followed by unsupported dirty-rectangle requests. Bounded partial composition subsequently passed on
the phone through 300 completed frames, before a separate Indigo HID target mismatch aborted that earlier backboardd.
The subsequent corrected input path now produces readable stock touch markers alongside the spinner on the phone. IOSurface remote
allocation is a [separate service](iosurface.md), and the
[native Metal service](metal.md) passes source compilation, pipeline creation and GPU rendering. A complete simulator boot,
Apple-logo display and progress-bar behavior remain unverified.

The provider phone trial with backboardd 81070 successfully created an SFB connection object. `SFBConnectionConnect`
returned false with a nonnull error object, but backboardd continued. The bootstrap lookup's exact error was not captured.
It then stopped on the missing BackBoard event-processor configuration file. This earlier trial established that a missing
server is nonfatal on this startup path. After the server was added, the initial phone trial reached a real check-in and
then rejected the client's mode-2 swapchain request. QuartzCore unconditionally requests mode 2, so the implementation was
extended with delayed presentation before the next device trial. These results do not establish a GPU submission.

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
`simFramebufferMessageSendWithReply` requests a MAKE_SEND_ONCE reply right and waits without an explicit timeout. The host
answers supported requests or sends valid errors; a presentation reply follows the actual copy or cancellation result.

## Observed wire ABI

The structures are little endian. They use explicit packed widths; normal compiler alignment would change several fields.
`Host/FramebufferProtocol.h` declares the fixed-width packed wire structures and asserts their sizes and important offsets.
The parser first validates message bounds, then copies wire payloads with `memcpy`; it does not dereference potentially
unaligned typed pointers into received Mach messages.
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

Type **5** contains a packed **35-byte** mode: eight `uint32_t` fields for mode ID, width, height, scale, HDR value, pixel
format, color-space value and refresh rate in hundredths of Hz, followed by preferred, native and current byte flags.
The host uses the native server's BGRA fallback values: HDR 1, pixel format `0x42475241`, color space 1. Do not treat the
35-byte mode as a CGSize or substitute a pointer return for the real output argument:
`SFBDisplayGetCurrentMode(display, modeOut)` fills a caller-provided structure.

Swapchain creation uses message ID **9**, item type **7**, with a 32-byte payload. Its eight `uint32_t` values are width,
height, display ID, swapchain ID, pixel format, color-space value, HDR value and presentation mode. The request initializes
width, height, display ID and presentation mode; the reply fills the remaining values. Mode 1 is immediate and mode 2 is
delayed. Request mode 0 selects the host's default immediate mode. These are option-set values, not bit indexes.
Destroying the client swapchain sends ID **13**, type 7, with its display/chain IDs. Cancellation is ID **18**, type **22**,
whose payload contains two `uint32_t` values, display and swapchain ID.

Presentation is message ID **11**, item type **10**, with this packed 80-byte payload:

| Offset | Field |
| --- | --- |
| 0, 8, 16 | `uint64_t` operation ID, requested absolute time in nanoseconds, flags |
| 24, 40 | Source and destination rectangles, each four `uint32_t` values |
| 56, 60, 64 | `uint32_t` display ID, swapchain ID, IOSurface ID |
| 68, 72, 76 | `uint32_t` surface-descriptor index, fence-descriptor index, layer |

The surface port descriptor transfers a fresh IOSurface send right using MOVE_SEND. An optional fence transfers a receive
right using MOVE_RECEIVE; the absent fence index is `UINT32_MAX`. The producer retains a send right and watches for its
death. Destroying the received fence right after the host finishes reading the surface therefore releases the producer;
there is no fence-completion message. Flags include bit 3 and optionally bit 4 for a requested callback.

The synchronous successful presentation reply keeps ID 11 and contains no items. A requested asynchronous completion uses
ID **12**, type **11**, with `uint64_t` operation ID, requested presentation timestamp and actual copy-completion timestamp,
followed by a `uint32_t` swapchain ID. Both timestamps are absolute nanoseconds derived from `mach_absolute_time`; the
second timestamp is **not a duration**. The provider converts both back to Mach ticks before invoking its callback.

Type **17** contains a packed 156-byte error payload: `char description[140]`, a `uint64_t` error/domain discriminator at
`0x8c`, and an eight-byte union at `0x94`. `_SFBErrorCreateFromSimReplyError` interprets discriminator **1000** as Mach with a
signed 32-bit result, **1001** as POSIX with a signed 64-bit result, and other nonzero values as SFB errors. Zero produces no
CFError. The builder's POSIX input is an `int *` which it sign-extends into that 64-bit wire field. A POSIX ENOTSUP response
can therefore use discriminator 1001, value 45 on Darwin, and a bounded NUL-terminated description. A failed check-in without
an actual error item can trigger the client's `!error || *error` assertion; an empty success-shaped reply is not a substitute.

## Host implementation and limits

The broker registers the host-owned service with `LSBLaunchdRegisterEndpoint` in the isolated simulator launchd namespace.
Indigo and framebuffer registration both complete before the broker releases the backboardd job. It does not replace or
check in to a system-owned service. The authorized backboardd instance may not replace itself with exec while it owns these
graphics/input endpoints; the existing restriction remains until complete ownership handoff is implemented.

`Host/FramebufferServer.m` verifies the entire native kernel audit token on every message against the broker's authorized
identity. It generates a fresh random session ID for check-in, retains the event send right and rejects duplicate check-in
with EALREADY, stale sessions with ENOTCONN and unauthorized senders with EPERM. The parser limits messages to 16 KiB,
64 descriptors and 32 fixed-size items, verifies every descriptor/stride/count and drains at most 16 messages per dispatch.
Unknown or unsupported operations receive a structured POSIX error; their side effects are not simulated as success.

The single integrated display is `PurpleMain` / `LCD` / `primary`, 1290×2796 at scale 3 and 60 Hz, with dot pitch 460 and
ID 1, matching the configured iPhone 15 Pro Max profile. It advertises one layer and one swapchain. The current backend
accepts nonplanar 32BGRA, full-size surfaces with identical source/destination rectangles and no blending, translation or
scaling. Rectangles must have positive size and fit inside the display. A successful full-frame copy establishes the
swapchain baseline; a partial update before that returns `ENODATA`. A failed full copy does not establish the baseline,
which is reset when the client/session or swapchain is reset. IOSurface IDs, format, element size, row size and allocation
bounds are verified after genuine Mach-port import. Multiple layers, HDR output and other display operations remain unsupported.

The native display view itself uses the guest's 1290:2796 aspect ratio. Auto Layout centers it in the host safe area and
limits both dimensions to 94% of the available space, leaving a small margin below the native status bar and around the
guest screen. The image viewport and touch normalization remain in this view's local coordinates; resizing still cancels
active contacts. This layout passed the existing coordinate/image tests and iOS compilation. The final phone screenshot
`tmp/framebuffer-server/phone-display-inset.png` confirms the unobstructed native status bar and readable guest touch markers.

Immediate mode presents when accepted, as the native scheduler does. Delayed mode holds a single presentation until its
requested monotonic deadline; a deadline more than five seconds ahead returns ENOTSUP. Already-due deadlines run immediately.
A real dispatch timer implements waiting. Additional submissions while a timer, GPU copy or main-queue callback is pending
receive EAGAIN. Even a canceled main-queue block occupies the bounded work slot until it drains, preventing repeated
present/cancel requests from accumulating retained blocks while the UI is stalled.

The native view's handler runs asynchronously on main and uses `Host/MetalDisplayRenderer.m` to copy the accepted pixels
with native Metal. It imports the guest IOSurface as a texture, blits the dirty rectangle into a persistent private full-frame
texture, and blits that complete baseline into a `CAMetalLayer` drawable. Pixels outside the rectangle retain their previous
values, not unrelated contents of the incoming surface. A serial worker waits for command-buffer completion before delivering
an errno result through the main-thread callback. The view and server keep the existing single-work-slot bound; neither queues unbounded rectangles or retains
a growing history of frames. The server replies and sends any completion callback only after that result, retaining the
fence until all GPU reads have ended. The layer manages three drawables and their compositor reuse independently of the guest.
The asynchronous server handler accepts one completion, ignores duplicates through an atomic state transition, and preserves
the active surface/fence through revoke or close until that completion retires the GPU reads.
Canceling a waiting/queued presentation releases its surface and fence and returns ECANCELED immediately. Canceling during
copy retains the source until the asynchronous completion reports that GPU reads have finished. Authorization changes atomically invalidate the old generation before returning to the
broker; queued state mutation never waits for main. Work retains the generation which admitted the request, so revocation
cannot stamp old work with a replacement's identity. Event-port death and callback-send failure conditionally revoke only
their own generation. All transferred rights are consumed or destroyed, including failed reply send-once rights.

`Tests/FramebufferServerTests.m` exercises malformed/authenticated requests, stale sessions, duplicate check-in, structured
errors, right cleanup, delayed cancellation, revoked queued frames and bounded main-queue work. It also loads the installed
stock macOS client and performs real check-in, display enumeration, swapchain creation, IOSurface pixel submission, fence
waiting, callback decoding and swapchain destruction. A delayed target 100 ms ahead must not copy early; its callback must
preserve the requested time and report a later completion. These tests pass with ASan/UBSan. IOSurface allocation requires
running the test with normal native kernel access; it returns NULL in the restricted command sandbox.

The dirty-rectangle server tests cover the observed `597,1350,96,96` update, edge pixels, missing/failed full baselines, session and
swapchain resets, invalid bounds, and rejection of scaled or translated rectangles. The real stock macOS client also submits
a partial update. `Tests/SimulatorDisplayTests.m` covers layout bounds, coordinate mapping, and immutable diagnostic image
publication. The earlier CPU backend passed focused tests, the full regression suite and the device build, then
reached 300 completed phone frames with partial updates, as recorded below. Those historical results do not validate the new
Metal backend; its separate GPU tests and phone evidence are recorded with that implementation.

## Native Metal presentation

The production display renderer uses public Metal and QuartzCore APIs in the native host. It creates no shaders or rendering
pipeline. `newTextureWithDescriptor:iosurface:plane:` imports the complete guest BGRA IOSurface, and a blit encoder performs
two ordered copies: the accepted dirty rectangle into a persistent private texture, then the entire texture into the next
`CAMetalLayer` drawable. The layer uses `framebufferOnly = NO`, the API's default `maximumDrawableCount = 3`, and guest-sized `drawableSize`.
Its view-space frame provides scaling and the existing inset layout; touch normalization uses the same viewport.

The baseline is necessary even with multiple display buffers: stock backboardd alternates IOSurfaces while supplying small dirty
rectangles, so unrelated pixels from the incoming surface cannot be treated as the complete current frame. At 1290 × 2796,
one packed BGRA frame contains 14,427,360 bytes. The previous implementation allocated and CPU-copied a full image on every
update. The Metal path keeps one private baseline and lets Core Animation own the bounded drawable pool. Three display
buffers do not mean three pending guest copies: the server and renderer still admit only one source copy at a time. The ordinary frame
path performs no IOSurface lock, CPU pixel copy, image allocation, PNG encode, or readback.

The renderer retains the imported source texture, IOSurface, and drawable through worker-side `waitUntilCompleted`. Only a completed GPU
command establishes a new baseline or reports success. Invalid rectangles, unsupported surface layouts, allocation failures,
and unavailable drawables preserve the previous baseline. A submitted GPU failure may have partially modified it, so the
renderer discards the baseline and hides the layer; subsequent partial updates return `ENODATA` until a full frame arrives.
Reset immediately revokes an atomic generation and hides existing presentation, then retires the baseline on the worker.
Queued work skips GPU submission; in-flight work finishes its source reads and reports `ECANCELED`. The view detaches the old layer and creates a fresh
renderer/layer for a replacement session, so a retired generation's queued drawable cannot reappear when the new display
is unhidden. A drawable is not reused by the app while Core Animation consumes it; that lifetime belongs to the layer's pool,
rather than a guessed frame delay or a transaction-completion block.

Drawable acquisition and GPU waits run on a serial worker, leaving the native main thread free to process touches and Core
Animation commits. Main owns layer configuration, layout, hidden state and published display metadata; only the worker
touches the private baseline. The server keeps its single pending-work slot, imported surface and producer fence until the
asynchronous completion arrives. Even a request without a transferred fence receives no early presentation reply while its
source is still being read. Concurrent renderer submissions return `EAGAIN` rather than building an unbounded queue.

`allowsNextDrawableTimeout = YES` bounds drawable-pool exhaustion to the API's timeout; there is no independent application
timeout for a submitted GPU command. Both waits are off the main thread. The renderer reports average/max drawable and GPU
wait durations and observed throughput every 300 successful frames, with at most eight individual slow-wait logs per generation.
The blocking renderer methods remain test/diagnostic conveniences; the production view uses completion-based presentation.

The optional `LSB_FRAMEBUFFER_CAPTURE_FIRST=1` diagnostic explicitly copies the private baseline into a shared Metal buffer,
waits on the worker for that readback, then delivers an immutable `CGImage` for the existing atomic PNG writer. This is the only renderer path
that reads pixels on the CPU; it is not enabled for normal frame presentation.

This native display copy runs in the host app, using its ordinary native Metal device. The simulator's Metal implementation,
shader compilation, and guest GPU commands still use the isolated native helper described in [metal.md](metal.md). Adding a
host presentation copy does not move that guest backend into the host process.

`Tests/MetalDisplayRendererTests.m` passed on the Mac's native Apple M4 GPU using a real standalone `CAMetalLayer` pool. It
verifies full frames, alternating source surfaces with poisoned pixels outside the dirty rectangle, immediate producer
overwrite after return, immutable diagnostic readback, invalid-input preservation, first-partial rejection, reset, resize,
and drawable-exhaustion recovery. A layer subclass only injects the unavailable-drawable case; successful copies use actual
Metal commands and drawable textures. The ASan/UBSan run also passed on the same native GPU, without skipping the GPU tests.
Evidence is `tmp/metal-display/tests.log` and `tmp/metal-display/tests-asan.log`.

The first Metal phone build used a synchronous main-thread wait and reached 6,000 completed stock backboardd presentations
on the native Apple A17 GPU. Its diagnostic PNG was byte-identical to the earlier CPU first frame, with SHA-256
`174efa36e14dd9edf5f7e765c2fd772adfbe2d89b0825c59eddd59f876f6b212`. Evidence is `tmp/metal-display/console.log`,
`tmp/metal-display/first-frame.png`, and `tmp/metal-display/phone-first.png`; `tmp/metal-display/security.log` contained no
amfid/TXM event. The user observed lag during that run, motivating the worker/completion contract above. The synchronous
image-correctness result alone did not establish responsiveness.

The async phone trial with two drawables still reached only **30.1 frames/s** over its first 300 frames: average drawable
acquisition was **26.835 ms**, while the GPU wait was only **2.233 ms**. Moving the wait off main preserved UI availability,
but the drawable pool remained the dominant presentation delay. Changing only the pool limit from two to the public API's
default three improved the first 300 frames to **43.9 frames/s**, with average drawable acquisition **8.476 ms** and GPU wait
**2.656 ms**. The user reported improved responsiveness. These are preliminary whole-run averages, not measured touch-to-pixel
latency or a guarantee of 60 Hz; sustained measurements are retained in the phone logs.

The same three-buffer run used host **82532**, native GPU helper **82533**, launchd **82534**, backboardd logical **82535** /
native **82537**, and cfprefsd logical **82536** / native **82538**. It reached 3,000 successful frames at **57.9 frames/s**
cumulative, with average drawable/GPU waits of **10.259 / 2.796 ms**. The rate derived from the rounded cumulative counters
between frames 1,200 and 3,000 is approximately **60 frames/s**, so the later interval recovered the guest display's intended
cadence. In contrast, the two-buffer async trial remained at 30.0 frames/s through 2,100 frames. The direct comparison is
`tmp/metal-display/console-async.log` versus `tmp/metal-display/console-triple.log`; cumulative throughput includes startup.

The three-buffer async path is the current implementation. A display-link refresh hint remains a possible later experiment
if the remaining rate is a problem. Apple documents [`CADisplayLink` refresh hints and system policy](https://developer.apple.com/documentation/quartzcore/optimizing-iphone-and-ipad-apps-to-support-promotion-displays),
and iOS 17's [`CAMetalDisplayLink`](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink) can supply drawables
with explicit preferred frame latency. Neither refresh mechanism is added merely to force a higher reported rate; the current
change is the measured pool adjustment while preserving bounded guest work.

## Direct presentation and SimRun comparison

The earlier display path performed CPU copies: even a 96 × 96 dirty update created a new full image, preserving unchanged
pixels from the previous immutable image. The Metal implementation above replaces that CPU work while retaining the same
dirty-region and producer-lifetime rules.

Direct IOSurface layer contents are supported by the inspected native stack. The installed iPhoneOS SDK's
`QuartzCore/CALayer.h` documents `contents` as accepting an `IOSurfaceRef`; normal syntax on an owned sublayer is
`layer.contents = (__bridge id)surface`. No manually invoked private selector or object-field mutation is needed. The
matching iOS 26.1 (23B50) QuartzCore binary also has a distinct IOSurface branch in `CA::Render::copy_render_value` at
`0x183b42a38`, constructing a `CA::Render::Surface` from the original surface. The saved disassembly is
`tmp/framebuffer-server/quartzcore-native26-disassembly.txt`. This establishes the rendering path, not end-to-end zero-copy
scanout or a safe producer-reuse signal.

The user-referenced [SimRun repository](https://github.com/unixzii/SimRun) was inspected at commit
`16139a384413421acffe10b3f2fce25fb2337802`. Its implementation separates guest rendering from host display:

- The macOS viewer assigns an IOSurface to an owned CALayer with implicit contents animations disabled. It clears and
  reassigns contents to notify Core Animation when the same surface is updated.
  [Layer setup](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/host/simrund/session/session.m#L247-L252),
  [frame assignment](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/host/simrund/session/session.m#L714-L721).
- Those are separate display buffers, not the original guest swapchain buffers. Its replacement SimFramebuffer maintains
  four buffers and normally Metal-blits the entire guest surface into a selected buffer before notifying the viewer. It
  has a CPU-copy fallback. This avoids CPU copying on its normal path, but still copies pixels on the GPU.
  [Buffer selection and copy](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/sim/simfb/simfb.m#L100-L179).
- Its surface-fence API returns success unconditionally, and the swap callback can run before the asynchronous GPU copy
  completes. The viewer acknowledges a frame after two refresh intervals; the producer can also reuse an older buffer
  after 250 ms without an acknowledgment. These are timing heuristics, not a compositor release fence.
  [Fence stub](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/sim/simfb/simfb.m#L320),
  [swap completion](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/sim/simfb/simfb.m#L372-L389),
  [viewer acknowledgment](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/host/simrund/session/session.m#L430-L443).

SimRun reports that delaying swap callbacks did not prevent Core Animation from recycling its three guest buffers, and
that directly displaying those buffers tore under viewer stalls. Its separate display-buffer copy addressed the observed
tearing. It also reports fully valid pixels outside dirty rectangles in its tested workloads; that is not a guarantee for
our stock iOS 26 backboardd. Our accumulator therefore continues to preserve the previous image outside the dirty region.
[SimRun's measurements](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/docs/render.md#L297-L313).

The relevant service overlap is IOSurfaceRemoteServer and MTLSimImplementation, both hosted behind sim2host XPC listeners.
SimRun runs these on macOS and replaces SimFramebuffer in the guest. LiveSimBooter runs native iOS services and implements
the stock provider's Mach protocol, so its display acknowledgment protocol is not interchangeable with our receive-right
fences. [SimRun's GPU services](https://github.com/unixzii/SimRun/blob/16139a384413421acffe10b3f2fce25fb2337802/src/host/simrund/sim2host/gpuhost.m#L26-L53).

Simply assigning the incoming IOSurface to CALayer and returning from our handler would be unsafe: the compositor can
still be reading after our server replies and destroys a transferred fence. `CATransaction` completion is not a solution;
its documented completion concerns animations and can run immediately when none were added. Retaining an IOSurface keeps
the object alive but does not prevent the guest from overwriting its pixels. The observed stock dirty presentations had
one port descriptor, for the surface, so the native tests' explicit `UseFences` option does not establish that backboardd
uses producer fences. Completion and reuse must be verified on this actual path.

The implemented path uses a native Metal texture view of the guest IOSurface, a GPU copy of the accepted dirty rectangle
into a persistent host texture, and presentation through CAMetalLayer. Metal's public
[`newTextureWithDescriptor:iosurface:plane:`](https://developer.apple.com/documentation/metal/mtldevice/maketexture(descriptor:iosurface:plane:))
uses the IOSurface as texture storage. The renderer keeps the source and any transferred fence alive until GPU reads have
completed and checks command-buffer status before reporting success. It preserves the current submission boundary until the no-fence
producer's reuse behavior is established; an early reply or guessed frame delay is not sufficient. Cancellation and
authorization revocation must retire outstanding GPU reads before releasing their resources. This removes CPU readback
and full-image CPU composition while retaining dirty-region semantics; it is a GPU-copy design, not an end-to-end zero-copy
claim. The synchronous and asynchronous phone results above validate image copying and the measured scheduling improvements.

### Private APIs considered

The inspected iOS 26.1 native QuartzCore and IOSurfaceAccelerator binaries contain several relevant APIs. None supplies a
verified drop-in, CPU-free snapshot and double-buffered display contract for the existing stock provider:

| API family | What the inspected implementation establishes | Remaining issue |
| --- | --- | --- |
| `CAIOSurfaceCreate` | Wraps the input IOSurface as a render surface. | No independent pixel copy; guest reuse can still race presentation. |
| `CAImageQueue` | Registers IOSurfaces and exposes collection/released-image callbacks. | Can manage queued references, but does not snapshot the guest surface or preserve dirty-region composition. |
| `CABackingStoreBeginUpdate` / `EndUpdate` | Provides a backing-store buffer pool through a `CGContext`. | No verified CPU-free IOSurface import/copy path. |
| `CAContext` fence APIs | Adds transaction dependencies. | Not a verified signal that the compositor has finished reading guest pixels. |
| `IOSurfaceAcceleratorTransferSurface` / `BlitSurface` | Opens `AppleM2ScalerCSCDriver` through `IOServiceOpen` type 0. | Sandbox availability is untested and a separate output-buffer pool is still needed. |

`IOSurfaceAcceleratorTransferSurfaceWithSwap` in that binary is a success-returning stub, not an implemented swap path.
The saved evidence is `tmp/framebuffer-server/quartzcore-native26-disassembly.txt` and
`tmp/framebuffer-server/iosurface-accelerator-native26-disassembly.txt`. These are static findings, not phone validation of
those private APIs. The public `CAMetalLayer` pool plus explicit Metal copy completion supplies the needed ownership without
depending on an unverified private snapshot or compositor-release contract.

## First stock phone frame

These are historical trials of the CPU-copy display backend, before the native Metal presentation implementation above.

The normal run used native host **82139**, Metal helper **82140**, simulator launchd **82141**, and stock backboardd native
PID **82143**, retaining logical PID **82142**. After the matching QuartzCore resources and compiler fixes, the server
completed presentation operation **1** from IOSurface **45**, and the native view assigned the copied image to its UIImageView.
The optional first-frame capture encoded that same immutable image after assignment.

`tmp/framebuffer-server/phone-first-frame.png` is **1290 × 2796** and visibly shows a spinner on black. Its SHA-256 is
`174efa36e14dd9edf5f7e765c2fd772adfbe2d89b0825c59eddd59f876f6b212`. This proves a real stock guest frame through the
presentation path. It does not show an Apple logo, a progress bar or a completed simulator boot.

Subsequent stock requests supplied identical source/destination rectangles **`597,1350,96,96`**, layer 0, flags **`0x18`**.
The then-full-frame-only implementation returned POSIX `ENOTSUP` (45), so the captured first frame alone does not prove
continued spinner animation. The bounded composition implementation above addresses that observed request shape; the next
phone trial verified those updates. First-frame-run evidence is `tmp/metal-native/console-stock-framebuffer.log` and
`tmp/metal-native/guest-stock-framebuffer.log`. The scoped `tmp/metal-native/security-stock-framebuffer.log` contains no
amfid rejection or TXM event.

The partial-composition run used host **82181**, helper **82182**, launchd **82183**, and backboardd native **82185** / logical
**82184**. It completed full-frame operation **1**, surface **302**, then partial operation **5**, surface **303**, and operation
**300**, surface **304**, reaching **300** completed frames. The logged partial rectangle was **`597,1350,96,96`**; the
first-frame PNG had the same SHA-256 as above. Evidence is `tmp/metal-native/console-stock-dirty.log`.

About 21 seconds after startup, this run ended with `SIGABRT` when SimulatorHID raised an uncaught
`NSInternalInconsistencyException` for `Encountered HID event with unexpected target 0 not in known targets`.
`-[SimHIDVirtualServiceManager serviceForIndigoHIDData:]` is the relevant frame in
`tmp/metal-native/guest-stock-dirty-exit.log`. The broker recorded wait status `0x6` and terminated the revoked native Metal
helper. This input failure is separate from the verified frame-composition path; the run did not remain alive or complete boot.

The later normal run used host **82455**, helper **82456**, launchd **82457**, backboardd logical **82458** / native **82460**,
and cfprefsd logical **82459** / native **82461**. With the Indigo and missing-runtime-asset fixes, physical touches produced
stock marker text and dirty updates. The 1290 × 2796 phone screenshot `tmp/framebuffer-server/phone-display-inset.png` shows
readable `t3 p1`, `in range`, `touch down`, and `ctx:C488CF8E` labels, the spinner and a clear native status bar. Its SHA-256 is
`11177bacf9c2d2914ca1b4f00b7e63574f4d7cabdbffb491fbf7c3b397e14476`.
The normal production spawn shim was restored before this run. See `tmp/metal-native/console-display-inset.log`,
`tmp/cfprefsd/final-phone-state.log`, and [preferences/font validation](cfprefsd.md#touch-visualization-runtime-assets).
This confirms the displayed marker and font correction; it does not establish a complete simulator boot.

A separate debugger run with host **82160** and backboardd **82168** stalled around image notification. Its transcript is
`tmp/framebuffer-server/lldb-stock-framebuffer-notifier.txt`, with console output in
`tmp/metal-native/console-stock-framebuffer-lldb.log`. It is separate from the normal-run frame evidence above.

## Reproducing the inspection

### Earlier iPhone integration result

The earlier signed build registered all three endpoints with isolated simulator launchd (`bootstrap=0`). Native backboardd
PID 81249 connected as `CAWindowServer`, created swapchain 1 at 1290 × 2796 in delayed mode 2, and opened four authorized
IOSurface connections. The device was iPhone16,2 on iOS 26.1 (23B50), using the rebuilt 23A343 simulator cache at
`0x120000000`. Logs are `tmp/framebuffer-server/console-final-lldb.log` and `lldb-final.txt`.

This earlier test produced no guest frame or verified Apple-logo display. LLDB temporarily continued past the SystemWake
registration assertion and returned from `DMProgressHostIsReady` and `_DMPerformMigration` at entry. The guest then remained
in its main and render run loops without issuing a presentation request. These debugger-only changes are not runtime fixes;
the power failure and invalidation hazard are described in [system power](system-power.md). At that point, the normal boot
path stopped at that assertion; the later virtual-awake adapter and Metal fixes enabled the first frame recorded above.

The missing stock ProgressUI bundle metadata and 22 Apple-logo PNG variants were copied into RuntimeRoot, with SHA-256
inventory in `tmp/framebuffer-server/progressui-resources/manifest.json`. Backboard's initial overlay requests the logo
without a progress bar; skipping migration does not establish progress-bar behavior. No synthetic image substitutes for
guest output. The amfid/TXM capture reported no event during these trials (`tmp/framebuffer-server/security.log`).

The full host regression suite, focused framebuffer ASan/UBSan tests and signed iOS build passed after the final timer
lifetime fix. The debugged process was terminated after capture; no debugger-only state is retained for normal launches.

### Local inspection commands

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
