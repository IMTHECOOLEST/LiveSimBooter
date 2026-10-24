# LiveSimBooter

Initial arm64 `dyld_sim` handoff from an iPhone app. `main()` maps `Documents/RuntimeRoot/usr/lib/dyld_sim` and
`Documents/RuntimeRoot/bin/df`, then calls dyld_sim's entry point with df's mapped executable header.
After preparation, it invokes the returned `start_sim` wrapper to run df and terminate through the simulator runtime.
UIKit startup is commented out; the original delegates, controller and storyboards are in `unused/`, outside the app target.

Place your signed images at those paths, preserving their Mach-O layout. The loader accepts `dyld_sim` as `MH_DYLINKER` or
an experimental `MH_DYLIB` signing variant. df may be `MH_EXECUTE` or `MH_DYLIB`, and must retain arm64 iOS Simulator
`MH_PIE`, `LC_BUILD_VERSION`, and `LC_MAIN`. Validation follows the image's launch role regardless of its file type.
Change the file type before signing; the loader preserves the signed header in memory. `MH_DYLIB` acceptance here does not
establish AMFI acceptance or compatibility with dyld_sim's own checks. In particular, a header-only `df` conversion lacks
`LC_ID_DYLIB`, which the pinned dyld source requires when validating an `MH_DYLIB` main image during preparation.
The loader accepts thin images or arm64 slices
in universal binaries. It registers each embedded signature with `F_ADDFILESIGS_RETURN`, reserves the image's address range,
maps its segments, and handles zero-fill. The executable's `__PAGEZERO` is excluded from that reservation. Signing remains your responsibility.

The entry receives df's mapped header, a persistent kernel-style argument block, and the version 18 syscall helper
table from the pinned Apple ABI. Tracing and external Mach monitor callbacks are disabled while preserving their ABI slots.
Image changes publish debugger metadata and call an app-resident notification function. `DYLD_ROOT_PATH` points to `Documents/RuntimeRoot`, and
`DYLD_SHARED_CACHE_DIR` points to its `System/Library/Caches/com.apple.dyld` directory. Adjust that directory for a different runtime layout.
`argv[0]` and the apple vector's `executable_path` identify the actual df file, with no additional arguments. The host's
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
compile without builtins, stack protectors, exceptions or RTTI. Startup uses C path handling without starting Foundation.

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

The shared-cache experiment on 2026-10-24 built a matching iOS 26 cache containing 219 dylibs, using the runtime's
`update_dyld_sim_shared_cache`. The single signed file is 292,749,312 bytes. On the tested iPhone, a genuinely signed cache
with CodeDirectory v0x20400, no scatter table, 16 KiB SHA-256 pages, the app identifier/team, and no entitlements passes
`F_ADDFILESIGS_RETURN` and the executable first-page mapping. Its complete 187,645,952-byte TEXT mapping also receives `r-x`
at `0x120000000`, verified under LLDB without adding execute permission afterward. This establishes executable mapping of a
signed cache; it does not establish execution of the cache at a relocated address.

The stock simulator mapping at `0x180000000` instead receives `r--` despite requesting `r-x`. Execution then faults at the
cached libSystem initializer. TXM logs `selector: 38 | 23`. The supplied TXM 182.0 binary identifies this as a registered code
region overlap: nesting inserts the full shared-region interval into the parent, and ordinary VM unmapping does not remove
that registration. Its selectors match the observed operations, although that supplied binary predates the device OS.
This accounts for the different results inside and outside the former host shared region. Changing the cache header alone
cannot relocate it: simulator caches have no slide information, and both the builder and dyld_sim mapper hardcode the base.
A complete cache-backed launch therefore needs a rebuilt or fully relocated cache and matching mapper using an available address outside that interval.

The installed iOS 27 runtime's arm64 cache consists of the main file and `.01`, with 11 mappings and 1,826 cached images.
Its declared region runs from `0x1c0000000` to `0x2abccc000`. All mapping slide-info offsets/sizes and both `maxSlide` fields are zero;
none of its images retain chained fixups, dyld rebase streams, split-segment information or section/local/external relocations.
Moving both files by the same 16 KiB-aligned delta preserves internal PC-relative instructions and relative metadata, but absolute
pointers and address-bearing headers still need adjustment. The surviving v4 symbol patch tables can help identify some pointer
sites; they are not a complete rebase inventory. Relocating this cache without the original dylibs is therefore a reconstruction
task, not just an instruction or header patch. The smaller cache's successful mapping at `0x120000000` does not establish a usable
address range for this much larger layout.

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
policy. Runtime/cache binaries remain outside Git. The experimental device cache is retained as
`dyld_sim_shared_cache_arm64.test-disabled`; with it disabled, the existing loose-library df launch still exits with status 0.
The existing handoff stack and commented UIKit startup are unchanged.

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
