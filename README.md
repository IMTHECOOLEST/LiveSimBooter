# LiveSimBooter

Experimental arm64 simulator runtime on an iPhone. The native app has a minimal UIKit foreground window and acts as a spawn broker.
It discovers `LiveProcess.appex` through `NSExtension` and launches separate instances through PlugInKit's pkd launch operation.
Its C entry receives a launch plan and descriptors through synchronous XPC,
maps `dyld_sim` and the requested executable, detaches the native cache, then invokes `start_sim`.
The native view starts interactive `/bin/fish` with a socket-backed terminal once from `viewDidLoad`; app launch arguments can
select another RuntimeRoot executable. Pass `/bin/spawn-probe` explicitly for the process tests. UIKit is confined to the native host; the original delegates,
controller and storyboards remain in `unused/`, outside the app target.

`RuntimeRoot` lives inside App Group `group.com.kdt.LiveSimBooter`. On first launch, the broker moves an existing
`Documents/RuntimeRoot` there if the destination does not exist. Both app and extension need matching App Group provisioning.
The child contains no Objective-C imports; the native broker uses Foundation and UIKit. A simulator interposer
forwards `posix_spawn`/`posix_spawnp` to the broker, including argv, environment, supported attributes and ordered FD actions.
It also provides `wait`/`waitpid` over a shared child-status table and delivers `SIGCHLD` through the native host.
Waiting accepts `WUNTRACED` and `WCONTINUED`, but reports terminal exits only; stopped/continued events and job control remain unsupported.
The `kill` interposer sends signals to the caller's direct simulated children through the broker; positive self PIDs use native
signaling. Signal 0 checks a child's current identity, and nonpositive PID selectors return `ENOTSUP`. The cross-process path
uses synchronous XPC and is not async-signal-safe. The native app and extension target iOS 26.0 or later.
The replacement iPhone on iOS 26.1 passed the parent-issued SIGTERM test under LLDB: the child reported readiness, the parent
called `kill(child, SIGTERM)`, and `waitpid` returned raw status `0x000f` with SIGCHLD observed. Two df children exited 0, an
additional child exited 37, and the probe itself exited 0. The guest remained single threaded at the kill/transport/wait
breakpoints, with native-cache code inaccessible. A separate run without LLDB passed the same checks.
The rebuilt simulator cache supplied the runtime's dependencies.
The debugger bridge now selects the compact image archive format supported by native dyld before cache removal. A fresh late
LLDB attach on the iOS 26.1 phone discovered 48 modules automatically, resolved named simulator breakpoints, and completed
the spawn/signal/wait probe with exit 0. The bridge
retains the newer format on hosts that require it. Native preparation preserves a raw broker Mach send right for guest IPC,
and the transport adapter handles the native/simulator XPC wire-magic difference.
See [multiprocess setup, progress, and limits](docs/multiprocess.md).

The opt-in `--launchd` mode starts an isolated, empty simulator launchd bootstrap through the same C LiveProcess entry.
See [launchd startup, its holding-tank handshake, and workqueue compatibility](docs/launchd.md) for the current experiment.

Unmodified Fish 4.9.3 source, built for iOS Simulator, now runs noninteractive external commands through this runtime.
On the iOS 26.1 phone it spawned and waited for two df children, then separately verified a Bash child exiting 37. Fish exited 0
in both runs. Runtime interposers handle its executable-access checks and wait options. The new [fake terminal](docs/fake-pty.md)
adds native keyboard/display handling and pure-C guest termios/ioctl compatibility. With the worker-start bridge, interactive
Fish reached its prompt on the iPhone, accepted editing keys, spawned `df` with exit 0 and original-platform Bash with exit 37,
interrupted a looping child with Ctrl-C, and exited 0 on Ctrl-D. LLDB caught a new worker entering simulator pthread while
the host cache region remained inaccessible. Same-burst text/Ctrl-C flushing and other terminal limits are documented.
Fish starts with job control disabled. Native PTY allocation, full job control, and fork/exec emulation remain unsupported.
See [Fish setup and results](docs/fish.md) and [tracked creation masks for guest worker threads](docs/creation-mask.md).

Launch the app normally and keep its window in the foreground while programs run. Its active scene keeps the screen awake and
restores the normal idle timer when it becomes inactive. The earlier headless host was suspended once
its launch assertion ended, and attaching LLDB alone did not prevent that suspension. The retained child assertion also depends
on its host remaining active. The earlier wait/SIGCHLD device test passed under LLDB: two df children, exits 0 and 37, and self-issued SIGTERM all
produced the expected wait status, delivered SIGCHLD, and returned `ECHILD` after reaping. The guest remained single threaded
at the wait and signal-handler breakpoints, with native-cache code inaccessible. The original phone's test
runtime keeps 219 loose cached dylibs in the reversible App Group backup `RuntimeLooseDylibs-multiprocess`, beside `RuntimeRoot`;
the rebuilt cache supplies them. The detailed device results below describe the earlier direct-launch implementation.
The current `--verify-statuses` probe has the parent send SIGTERM after its child reports readiness, then checks the signal
status with `waitpid`. See the [signaling scope and device results](docs/multiprocess.md#signaling-a-simulated-child).

See [docs/](docs/README.md) for the handoff design, cache signing/rebuilding procedure, device debugging commands, and recorded test results.

Place your signed images at those paths, preserving their Mach-O layout. The loader accepts `dyld_sim` as `MH_DYLINKER` or
an experimental `MH_DYLIB` signing variant. A program may be `MH_EXECUTE` or `MH_DYLIB`, and must retain plain arm64
`MH_PIE` and `LC_MAIN`. Its single platform declaration may be iOS Simulator, iOS, or legacy `LC_VERSION_MIN_IPHONEOS`.
Device iOS images require the [version-pinned dyld_sim compatibility patch](docs/ios-platform.md); their platform commands
stay unchanged. The phone test ran an iOS executable with an iOS dylib and the existing iOS 7 Bash binary successfully after
signing, without platform rewriting. Signing and available runtime APIs still constrain compatibility.
Validation follows the image's launch role regardless of its file type.
Change the file type before signing; the loader preserves the signed header in memory. `MH_DYLIB` acceptance here does not
establish AMFI acceptance or compatibility with dyld_sim's own checks. In particular, a header-only `df` conversion lacks
`LC_ID_DYLIB`, which the pinned dyld source requires when validating an `MH_DYLIB` main image during preparation.
The loader accepts thin images or arm64 slices
in universal binaries. It registers each embedded signature with `F_ADDFILESIGS_RETURN`, reserves the image's address range,
maps its segments, and handles zero-fill. The executable's `__PAGEZERO` is excluded from that reservation. Signing remains your responsibility.

The entry receives df's mapped header, a persistent kernel-style argument block, and the version 18 syscall helper
table from the pinned Apple ABI. Tracing and external Mach monitor callbacks are disabled while preserving their ABI slots.
Image changes publish debugger metadata and call an app-resident notification function. `DYLD_ROOT_PATH` points to the App Group's `RuntimeRoot`, and
`DYLD_SHARED_CACHE_DIR` points to its `System/Library/Caches/com.apple.dyld` directory. Adjust that directory for a different runtime layout.
The caller's argv is preserved, and the apple vector's `executable_path` identifies the resolved runtime executable. The host's
`executable_file` metadata is removed so it cannot override df's path. dyld_sim handles df's fixups and dependencies.
The loader replaces inherited `ptr_munge` entries with a fresh nonzero random token before handoff. Its apple-vector storage
is writable because simulator libpthread consumes and erases that value; the original host apple strings remain untouched.

Immediately before entry, the loader follows Apple's shared-region query and `__shared_region_check_np(NULL)` detach sequence.
The destructive syscall runs from the app's own arm64 code, so it does not return into an unmapped libSystem stub. Arguments,
helpers, debugger metadata, main-thread bootstrap storage and the AMFI policy snapshot are prepared first. After successful detachment, the handoff does not return to
host cleanup. The returned `start_sim` wrapper receives the same argc, argv, environment and apple vectors; it calls df's main
and then simulator `exit()`. If the wrapper returns, a raw exit syscall preserves its status without host cleanup. A null
entry result exits with status 1; unsupported legacy startup glue exits with status 126. A pre-entry detach failure is reported
while host libraries remain available.

`MainThreadBootstrap.cpp` reserves a zeroed, aligned 16 KiB block in the app for the main pthread and its TSD. It validates the
host's ARM64 self/errno layout and initializes only self, errno and the kernel thread ID. The selected macOS support libpthread
uses a 224-byte prefix and 768 TSD slots, so copying the smaller iPhone host object would be insufficient. Immediately before
cache removal, the handoff switches `TPIDRRO_EL0` through Darwin's platform syscall and verifies readback. A switch or detach
failure restores the original TSD before returning to host error handling; a failed restoration exits directly with status 127.
Simulator pthread finishes initializing the fresh object. No host TLS keys, cleanup callbacks or thread-list pointers are copied.

All 67 helper callbacks now reside in the app. `LocalPOSIX.cpp` uses direct BSD syscalls; `LocalMach.cpp` uses Mach traps and
local message clients. `LocalRuntime.cpp` supplies memory/string operations, per-thread errno, recursive lock bridging and
stderr formatting without calling host libc, pthread, Objective-C, or C++ runtime functions. These files and the helper table
compile without builtins, stack protectors, exceptions or RTTI. LiveProcess startup uses C path handling without starting Foundation.

`HostPreparation.cpp` queries the actual host AMFI policy before detachment for the pinned simulator's input flags (1).
The callback returns that saved result for identical flags; other inputs return `ENOSYS`. No host function pointer is retained
and no policy permissions are fabricated. Signing and kernel restrictions still apply.

The simulator df now runs on the tested iPhone. This remains an initial runtime handoff rather than a complete simulator boot.
LLDB verified actual host-cache removal, inaccessible native dyld code, readable app-owned thread storage, and successful
dyld_sim preparation. Execution reached df's exact `LC_MAIN` entry through `start_sim`, printed the iPhone filesystem table,
and exited with status 0. Malloc reported that its nano zone could not reserve VM space; df still completed successfully.
This test used the matching iOS 26 dyld_sim/df/runtime set and the three locally signed macOS support dylibs marked as iOS Simulator.

`DebuggerBridge.cpp` transfers LLDB from the native dyld notifier to `LSBDebuggerImageNotification` and publishes simulator
metadata through `TASK_DYLD_INFO`. The kernel permits only one relocation of that pointer, which native startup may already
have consumed. In that case, after detaching the cache, the bridge maps fresh anonymous pages at the original metadata
address and places simulator state there. Only those metadata pages become readable; the old cache code stays inaccessible.
The image list retains the host app for source breakpoints and includes df before dyld_sim adds its dependencies.

Recent debugserver versions read dyld's compact image snapshot instead of the legacy image array. Without that snapshot,
their fallback finds executable/dyld images but leaves dynamically loaded libraries unnamed. `CompactImageInfo.cpp` writes
an AppleArchive containing `process.plist`, paths, UUIDs, load addresses, segment metadata and a CRC, without calling host
libraries, before each image notification. Startup callbacks also advance the published process state and timestamp.
This preserves the binaries' iOS Simulator platform and the debugger's iPhone connection.
The writer uses bounded app-owned storage for up to 1024 images. Its two snapshot buffers support inspection while LLDB
has stopped the process; external asynchronous process monitors remain unsupported.

On the tested iOS 27 device, LLDB automatically lists 49 named modules: the host app and 48 RuntimeRoot images, including
dyld_sim, df and its 46 dylibs. Module-scoped breakpoints hit `libsystem_pthread.dylib::__pthread_init` and
`libsystem_c.dylib::getmntinfo`; backtraces identify dyld_sim's `start_sim` and preserve host-app source frames.
The debugger reports `dyld_process_state_program_running`, native dyld code remains inaccessible, and df exits with status 0.
No manual module injection or platform changes in LLDB are needed. Symbols stripped from the supplied binaries remain unavailable.

The snapshot writer also accepts cache images packed on 4 KiB boundaries and uses each cached `__TEXT.vmaddr` as its preferred
address. It describes loaded cache images individually, including their actual segment coordinates. Cache-specific Atlas APIs
and the cache image bitmap are not implemented; dyld_sim still owns the legacy shared-cache address, slide and UUID fields.

The shared-cache experiment on 2026-10-24 rebuilt a matching iOS 26 cache containing 219 dylibs at `0x120000000`, using a patched
copy of the runtime's `update_dyld_sim_shared_cache`. The single signed file is 292,749,312 bytes. On the tested iPhone, the signed cache
with CodeDirectory v0x20400, no scatter table, 16 KiB SHA-256 pages, the app identifier/team, and no entitlements passes
`F_ADDFILESIGS_RETURN`. Its complete 187,645,952-byte TEXT mapping receives `r-x`, without adding execute permission afterward.
LLDB stopped inside cached `libSystem_initializer` at `0x129d4ff78`, then inside `getmntinfo` called by df through `start_sim`.
The native dyld code remained inaccessible. df printed the filesystem table and exited with status 0, both under LLDB and in
a fresh launch without a debugger. The initial run used two cached dylibs and loose RuntimeRoot overrides for the others.
A follow-up temporarily moved all 219 loose dylibs outside RuntimeRoot and verified that all 46 loaded dependencies carried
`MH_DYLIB_IN_CACHE` with headers inside the rebuilt cache. LLDB reached cached `__pthread_init` at `0x12b2688d8` and cached
`getmntinfo` at `0x12b09e984`, recognized all 49 modules, and df again exited 0 with and without the debugger.
AMFI reported no rejection. One TXM `selector: 38 | 23` event appeared during the LLDB run without preventing completion;
the separate normal-launch capture had no TXM errors. Three subsequent traced LLDB runs did not reproduce it. Packet logs
confirmed that internal pthread software breakpoints dirty two cache pages; requiring hardware breakpoints globally kept
cache TEXT clean. This establishes the source of the dirty pages, but not the earlier TXM event. All 219 loose dylibs were
restored after testing; the rebuilt cache remains enabled. See [the debugger comparison](docs/debugging.md#internal-breakpoint-investigation)
and [the cache notes](docs/shared-cache.md) for evidence and limits.

The rebuilt cache's full declared span is `[0x120000000, 0x135508000)`; LLDB confirmed the destination was free before mapping.
Comparison with the original build verified the exact `-0x60000000` relocation across all 219 images, 864 Mach-O segments,
4,564 sections, 765,301 section symbols and 63,525 pointer slots. The builder regenerates the addresses from the original dylibs.

The stock simulator mapping at `0x180000000` instead receives `r--` despite requesting `r-x`. Execution then faults at the
cached libSystem initializer. TXM logs `selector: 38 | 23`. The supplied TXM 182.0 binary identifies this as a registered code
region overlap: nesting inserts the full shared-region interval into the parent, and ordinary VM unmapping does not remove
that registration. Its selectors match the observed operations, although that supplied binary predates the device OS.
A later hardware-only retest reproduced the original-base failure: signature registration returned `0`, TEXT was `r--`
with zero dirty pages, and TXM logged `38 | 23`. Execution faulted in cached `LibSystemHelpers::version()` before cached-image
publication. Packet logs contained no software breakpoints or writes to cache TEXT. The restored `0x120000000` cache again
ran df to exit `0`; see [the retest evidence](docs/shared-cache.md#original-base-hardware-only-retest).

This accounts for the different results inside and outside the former host shared region. Changing the cache header alone
cannot relocate its payload: simulator caches have no slide information. The tested iOS 26 builder and dyld_sim mapper hardcode the base.
The rebuilt iOS 26 cache avoids that interval, and the patched dyld_sim reads its mapping base and failure-cleanup bounds from the cache header.

`Tools/patch-ios26-cache-base.py` creates the two patched tools from exact iOS 26 build 23A343 originals. It checks their SHA-256
hashes, verifies the six instruction changes, refuses overwrites and writes a patch manifest. Use its builder only for arm64
simulator inputs. Ad-hoc sign that builder for execution on the Mac, rebuild the cache, then sign the patched dyld_sim with the
app identity/identifier and sign the cache using the tool below. The supplied runtime binaries remain outside Git.

```sh
python3 Tools/patch-ios26-cache-base.py --builder ORIGINAL_BUILDER --dyld-sim ORIGINAL_DYLD_SIM --output-dir tmp/cache-base-tools
```

The old executable-overlap heuristic remains in dyld_sim; df does not intersect either range and needs no environment override.
Do not put `DYLD_SHARED_REGION=private` in the native app launch environment: native dyld processes it before main and its own
private-cache mapping hits the TXM overlap error. Any future simulator-only override must be added after native startup.

The installed iOS 27 runtime's arm64 cache consists of the main file and `.01`, with 11 mappings and 1,826 cached images.
Its declared region runs from `0x1c0000000` to `0x2abccc000`. All mapping slide-info offsets/sizes and both `maxSlide` fields are zero;
none of its images retain chained fixups, dyld rebase streams, split-segment information or section/local/external relocations.
Moving both files by the same 16 KiB-aligned delta preserves internal PC-relative instructions and relative metadata, but absolute
pointers and address-bearing headers still need adjustment. The surviving v4 symbol patch tables can help identify some pointer
sites; they are not a complete rebase inventory. Relocating this cache without the original dylibs is therefore a reconstruction
task, not just an instruction or header patch. The smaller cache's successful mapping at `0x120000000` does not establish a usable
address range for this much larger layout.
See [the concrete stale-pointer example and layout limits](docs/shared-cache.md#ios-27-relocation-limits).

Two later iOS 27 mapping-only trials used the matching 24A434 dyld_sim and df with the host app's Increased Memory Limit entitlement.
At `0x300000000`, both signatures passed and all 11 mappings completed, but TEXT received `r--` and TXM reported two overlap events.
This address is still inside the current native shared-region reservation, `[0x180000000, 0xfc0000000)`.
The first exception was a trie read at its stale address `0x26b010000`, before cached code execution.
A repeat at `0x8000000000`, beyond the native reservation and SDK GPU carveout, passed both signatures but its first fixed mapping
failed with `ENOMEM`; no TXM overlap was logged. This does not establish that another entitlement would make the mapping succeed.
The original working iOS 26 runtime was restored and df exited `0`. See [both trial results](docs/shared-cache.md#first-forced-base-trial-inside-the-native-reservation)
and [the provisioning cleanup repair](docs/provisioning-cleanup.md) that allowed the newly provisioned host build to install.

Ordinary embedded `codesign` signing adds a scatter table that this TXM rejects (`0x70 | 0x28 | 1`). Its DSC writer also leaves
`codeSignatureSize` stale when the signature grows. The header must contain `file size - codeSignatureOffset` before the final
page hashes and signature are generated, matching the purpose of this
[signature-size fixup](https://github.com/zhuowei/iOS-run-macOS-executables-tools/blob/main/resign_dyld_cache/fixup_signature_size.py).
Plain detached signing avoids scatter but emits v0x20200, rejected by this iPhone as an unsupported signature version.
`Tools/sign-dyld-cache.py` handles the header, contiguous 16 KiB hashes and v0x20400 signature with the selected keychain identity:

```sh
python3 Tools/sign-dyld-cache.py UNSIGNED_CACHE SIGNED_CACHE \
  --identity D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter --team UU7W52U79B
```

The tool uses `codesign` for the initial identity/requirements, then signs the adjusted CodeDirectory through Security.framework
without exporting a private key. It checks the page hashes and final signature. It does not relocate the cache or change device
policy. The relocated cache and patched dyld_sim are installed and active on the test iPhone. The earlier stock-base cache is
retained as `dyld_sim_shared_cache_arm64.test-disabled`, and the previous dyld_sim is backed up as `dyld_sim.before-relocated-cache`.
The loader keeps the existing handoff stack; UIKit now runs only in the separate native foreground host.

macOS's simulator launch [keeps bootstrap dyld outside the shared cache](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp#L1191-L1195).
Its handoff does not reset TLS. Simulator libpthread [adopts the existing main pthread](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/src/pthread.c#L2086-L2096),
whose bootstrap storage survives normal macOS cache removal. Here native app startup has already placed that storage in the host
cache, so the app supplies its own bootstrap block before detaching it. Only the current main thread's storage is replaced;
existing host threads and kernel pthread/workqueue entry-point registrations are not migrated.

The local bridge targets dyld's usage: caller-provided path buffers,
integer/string/pointer log formatting, up to 4096 lifetime errno thread slots and 512 recursive mutex identities. It does not
implement general host pthread synchronization or suspend existing host-library threads. Unsupported kernel operations return
the kernel's error. Keep launch minimal and test against the intended device/runtime combination. This phase has no app UI.

Build without signing for compilation checks:

```sh
xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Debug -sdk iphoneos \
  -destination 'generic/platform=iOS' -derivedDataPath /tmp/LiveSimBooter-build CODE_SIGNING_ALLOWED=NO build
```

The mapping sequence follows Apple's [prepareSim implementation](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp#L300-L469).
Cache detachment follows [deallocateExistingSharedCache](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/SharedCacheRuntime.cpp#L1230-L1243).

Run `sh Tests/run.sh` on an Apple Silicon Mac for loader and callback tests. Mapping/handoff tests use synthetic Mach-O images,
stub signature registration, a fake entry and mocked cache syscalls. Safe live callback tests exercise local syscalls, owned
VM/ports, directory reads, clocks, locks and errno, and check every helper pointer belongs to the executable.
Debugger tests cover metadata relocation and rollback, image notifications, startup state, and compact archives decoded
independently by CoreFoundation, including malformed input and publication failure cases.
They never detach the test process's cache or execute a simulator runtime, and do not validate iPhone code-signing acceptance.

Run `python3 Tests/audit-local-runtime.py` to compile the callback/handoff code for iPhone arm64 at `-O0` and `-O2` and reject
any external imports. Pass an Xcode `Objects-normal/arm64` directory to audit the actual build objects instead.

Simulator dyld diagnostics are disabled by default. Explicit `DYLD_PRINT_*` variables and their output destination are
preserved in the simulator's copied environment. See [the flags and capture workflow](docs/debugging.md#simulator-dyld-diagnostics).

Launch with `--probe-dlopen-dyld-sim` to call host `dlopen()` once on `RuntimeRoot/usr/lib/dyld_sim`, print its result and
`dlerror()`, close a successful handle, then exit. This diagnostic runs before image mapping or host-cache removal.

## License

LiveSimBooter's original code is available under the [MIT License](LICENSE), copyright 2026 Duy Tran.
The following Apple-derived files and their modifications remain under the [Apple Public Source License 2.0](LICENSES/APSL-2.0.txt):

- `LiveSimBooter/DyldSimLoader.cpp` — adapted simulator mapping and launch flow.
- `LiveSimBooter/SharedCacheHandoff.cpp` — adapted shared-region removal flow.
- `LiveSimBooter/SimSyscallHelpers.h` and `LiveSimBooter/SimSyscallHelpers.cpp` — adapted syscall-helper ABI and table.
- `LiveSimBooter/CompactImageInfo.cpp` — adapted compact image snapshot format.

Their Apple copyright/license notices and dated modification notices are retained. The source is in this repository at those
paths; upstream is [Apple dyld at fd8d0c4d52320ebf64db34f3cb280310d905c5ae](https://github.com/apple-oss-distributions/dyld/tree/fd8d0c4d52320ebf64db34f3cb280310d905c5ae).
APSL §4 permits combining covered code with separately licensed code in a larger work. MIT does not replace APSL for the
covered portions. Distributing or externally deploying modified covered code carries APSL's notice, license-copy and source
availability requirements; executable-only distributions also need a notice identifying how to obtain that covered source.
The licenses here do not grant rights to separately supplied Apple simulator binaries, runtime images, or SDKs.
