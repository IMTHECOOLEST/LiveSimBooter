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
table from the pinned Apple ABI. Tracing and monitor callbacks are disabled while preserving their ABI slots. The simulator gets
separate process-info storage. `DYLD_ROOT_PATH` points to `Documents/RuntimeRoot`, and
`DYLD_SHARED_CACHE_DIR` points to its `System/Library/Caches/com.apple.dyld` directory. Adjust that directory for a different runtime layout.
`argv[0]` and the apple vector's `executable_path` identify the actual df file, with no additional arguments. The host's
`executable_file` metadata is removed so it cannot override df's path. dyld_sim handles df's fixups and dependencies.
The loader replaces inherited `ptr_munge` entries with a fresh nonzero random token before handoff. Its apple-vector storage
is writable because simulator libpthread consumes and erases that value; the original host apple strings remain untouched.

Immediately before entry, the loader follows Apple's shared-region query and `__shared_region_check_np(NULL)` detach sequence.
The destructive syscall runs from the app's own arm64 code, so it does not return into an unmapped libSystem stub. Arguments,
helpers, main-thread bootstrap storage and the AMFI policy snapshot are prepared first. After successful detachment, the handoff does not return to
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
LLDB verified actual host-cache removal, inaccessible old TSD storage, readable app-owned thread storage, and successful
dyld_sim preparation. Execution reached df's exact `LC_MAIN` entry through `start_sim`, printed the iPhone filesystem table,
and exited with status 0. Malloc reported that its nano zone could not reserve VM space; df still completed successfully.
This test used the matching iOS 26 dyld_sim/df/runtime set and the three locally signed macOS support dylibs marked as iOS Simulator.

LLDB does not automatically discover the simulator images yet. In this run the app-owned process-info array contained 46
images, while LLDB retained 80 host modules and showed no RuntimeRoot modules. The private metadata is not published through
`TASK_DYLD_INFO`, and image-change notifications remain stubbed. Address breakpoints and direct memory inspection work;
symbolication currently uses exact local binaries and their load addresses. The iPhone target also rejects manually adding
iOS Simulator modules, a separate issue from publishing the image list.

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

Their Apple copyright/license notices and dated modification notices are retained. The source is in this repository at those
paths; upstream is [Apple dyld at fd8d0c4d52320ebf64db34f3cb280310d905c5ae](https://github.com/apple-oss-distributions/dyld/tree/fd8d0c4d52320ebf64db34f3cb280310d905c5ae).
APSL §4 permits combining covered code with separately licensed code in a larger work. MIT does not replace APSL for the
covered portions. Distributing or externally deploying modified covered code carries APSL's notice, license-copy and source
availability requirements; executable-only distributions also need a notice identifying how to obtain that covered source.
The licenses here do not grant rights to separately supplied Apple simulator binaries, runtime images, or SDKs.
