# Runtime handoff design

`main()` in [main.m](../LiveSimBooter/main.m) starts in C, without Foundation or UIKit startup. It locates the app's
`Documents/RuntimeRoot`, maps `usr/lib/dyld_sim` and `bin/df`, then enters the simulator runtime. The original delegates,
controller, and storyboards remain in `unused/`, outside the app target.

## Before host-cache removal

[DyldSimLoader.cpp](../LiveSimBooter/DyldSimLoader.cpp) validates the Mach-O role, registers the embedded signature through
`F_ADDFILESIGS_RETURN`, reserves the address range, and maps segments and zero-fill. It accepts thin arm64 images or an arm64
universal slice. The executable's `__PAGEZERO` is excluded from its reservation. The mapped signed headers are preserved.

The loader builds persistent kernel-style arguments for `df`, including its actual path in `argv[0]` and `executable_path`.
It sets `DYLD_ROOT_PATH` and `DYLD_SHARED_CACHE_DIR` to RuntimeRoot paths, removes stale host executable metadata, and replaces
inherited `ptr_munge` values with a fresh nonzero random token. The apple vector is writable because simulator libpthread
consumes and clears the token. It also enables [dyld diagnostics](debugging.md#simulator-dyld-diagnostics) in the copied
simulator environment, with stderr output and no inherited file redirection. Native launch settings are untouched.

[MainThreadBootstrap.cpp](../LiveSimBooter/MainThreadBootstrap.cpp) supplies an aligned, zeroed 16 KiB app-owned block for
the main pthread and TSD. It initializes only self, errno, and the kernel thread ID after validating the host layout.
It does not copy host TLS keys, cleanup callbacks, or thread-list pointers. The selected support libpthread needs a
224-byte prefix and 768 TSD slots, larger than the host object; simulator pthread completes initialization.

[HostPreparation.cpp](../LiveSimBooter/HostPreparation.cpp) snapshots the host AMFI policy for the pinned simulator's flags
before detachment. The callback returns the saved result only for the same flags and returns `ENOSYS` otherwise.
It fabricates no policy permissions and retains no host function pointer.

## The one-way transition

[SharedCacheHandoff.cpp](../LiveSimBooter/SharedCacheHandoff.cpp) queries the shared region, prepares the debugger transition,
switches the thread's TSD base with Darwin's platform syscall, and verifies it. It then performs the
`__shared_region_check_np(NULL)` detach from app-resident assembly, without returning into an unmapped libSystem syscall stub.
Before successful detachment, failures restore host TSD and debugger state for host-side error reporting.

After detachment, all 67 helper callbacks execute from the app. [LocalPOSIX.cpp](../LiveSimBooter/LocalPOSIX.cpp) uses BSD
syscalls, [LocalMach.cpp](../LiveSimBooter/LocalMach.cpp) uses Mach traps and local message clients, and
[LocalRuntime.cpp](../LiveSimBooter/LocalRuntime.cpp) supplies memory/string operations, errno, recursive lock bridging,
and stderr formatting. These paths are built without builtins, stack protectors, exceptions, or RTTI.

The version 18 [helper ABI](../LiveSimBooter/SimSyscallHelpers.h) and argument block are passed to dyld_sim's prepare entry.
For the supported `LC_MAIN` path, its returned `start_sim` wrapper receives the same argc/argv/environment/apple vectors,
calls `df`, and exits through the simulator runtime. A raw exit handles an unexpected wrapper return without host cleanup.
A null prepare result exits `1`; unsupported startup glue exits `126`; failed restoration of host TSD exits `127`.
Existing host frames remain below the simulator frames. The implementation does not reset the stack or remove those frames.

Only the current main thread's storage is replaced. Existing host threads and kernel pthread/workqueue entry registrations
are not migrated. The local callback bridge implements the pinned dyld's usage, not a complete replacement for host libc or pthread.
That is why startup remains minimal and UIKit remains deferred.

## Debugger publication

[DebuggerBridge.cpp](../LiveSimBooter/DebuggerBridge.cpp) hands LLDB from native dyld notification to an app-resident notifier
and publishes simulator state through `TASK_DYLD_INFO`. The kernel allows only one metadata-pointer relocation, which native
startup may already have used. In that case the bridge maps fresh anonymous metadata pages at the registered address after
detaching the cache. Native cache code remains inaccessible.

[CompactImageInfo.cpp](../LiveSimBooter/CompactImageInfo.cpp) publishes AppleArchive image snapshots before notifications,
including paths, UUIDs, addresses, segments, process state, and CRC. Recent debugserver versions need this compact snapshot
to name simulator-loaded libraries. Cached images use their actual segment coordinates and permit 4 KiB image-header alignment.
The app remains in the list for source breakpoints. Publication uses bounded app-owned storage for 1024 images and two buffers.
Asynchronous external process monitors, cache Atlas APIs, and the cache image bitmap are not implemented.

## Local verification

```sh
sh Tests/run.sh
python3 Tests/audit-local-runtime.py

xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Debug -sdk iphoneos \
  -destination 'generic/platform=iOS' -derivedDataPath /tmp/LiveSimBooter-build CODE_SIGNING_ALLOWED=NO build
```

The host tests use synthetic images and substitute dangerous handoff operations. They do not detach the Mac test process's
cache or establish device signing acceptance. The import audit checks the local callback/handoff code at `-O0` and `-O2`;
it can also inspect an actual Xcode arm64 object directory. Device behavior requires the separate [debugging workflow](debugging.md).

The implementation follows the pinned Apple dyld source described in the [root README](../README.md). Apple-derived files
retain their APSL notices; the project's separate original code uses MIT.
