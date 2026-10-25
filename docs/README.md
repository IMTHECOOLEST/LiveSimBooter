# Development notes

These notes describe the runtime experiments and the multiprocess implementation added on 2026-10-25.
The current target is a minimal arm64 runtime handoff with brokered `posix_spawn`; UIKit and a complete simulator boot remain later work.

- [Runtime handoff](runtime-handoff.md): startup, local callbacks, thread storage, cache detachment, and debugger metadata.
- [Multiprocess](multiprocess.md): App Group migration, native extension launches, synchronous XPC, FD replay, and current validation limits.
- [Bash](bash.md): noninteractive builtin baseline, terminal-access probes, and remaining fork/exec design requirements.
- [Fish](fish.md): interim spawn-based shell experiments, executable-access findings, and source-port progress.
- [Shared cache](shared-cache.md): signing format, TXM findings, the relocated iOS 26 cache, and reproduction commands.
- [iOS 27 relocation](ios27-relocation.md): low-address capacity, missing relocation records, and pointer/instruction fixups.
- [iOS 26 base comparison](ios26-base-comparison.md): minimum stock inputs, two rebuilt caches, and exact assembly/data differences.
- [Pointer classification](cache-pointer-classification.md): structural filtering of all false positives in both caches, preserving measured relocations.
- [Device debugging](debugging.md): installation, file transfer, SSH logs, LLDB attachment, and verification.
- [Provisioning cleanup](provisioning-cleanup.md): the custom trigger conflict, targeted repair, backup, and installation evidence.

The matching iOS 26 build **23A343** runtime runs on the tested iOS 27 iPhone. With all 219 loose dylibs temporarily moved
outside RuntimeRoot, all 46 dependencies loaded from the relocated cache at `0x120000000`. `df` exited zero under LLDB
and in a normal launch; LLDB recognized 49 modules. One TXM overlap event occurred during the LLDB run, with no such event
in the normal capture; its cause is not established. The loose dylibs were restored afterward, and the relocated cache remains active.

The first iOS 27 build **24A434** mapping-only trial registered both signed cache files and mapped all 11 regions, then
faulted on an unchanged trie address. Its requested base `0x300000000` was still inside the native shared-region reservation
`[0x180000000, 0xfc0000000)`; both TEXT mappings were `r--`, with two TXM overlap errors. The iOS 26 files were restored
and `df` again exited zero. A repeat above both the reservation and GPU carveout at `0x8000000000` accepted both signatures
but failed its first TEXT mapping with `ENOMEM`; no high cache mapping was installed. The captured repeat had no TXM error
or AMFI rejection. The iOS 26 files were restored again and `df` exited zero. iOS 27 startup remains unestablished.

[Mapping-only dyld_sim comparisons](shared-cache.md#signed-dyld_sim-mapping-above-0x180000000) duplicated the signed iOS 26
loader's TEXT at `0x300000000` after cache detachment and `0x400000000` while the native cache remained loaded. In both,
signature registration and mmap succeeded, but the duplicate was `r--` while the identical low-address TEXT remained
`r-x`; TXM logged `38 | 23`, with zero dirty pages and no software breakpoints. Detachment was therefore unnecessary to
reproduce the failure. Both high copies were removed without execution, and the normal handoff ran `df` to exit zero.
The second test used a different verified-free address because the first was occupied by native cache data.
The original host app was restored afterward; a fresh normal launch also ran `df` to exit zero.
A signed Mach-O wrapper for the cache remains untested.

A [manual native dylib test](shared-cache.md#manually-mapped-native-dylib) mapped the same signed `MH_DYLIB` at
`0x140000000` and `0x400000000` with the host cache still loaded. Signature registration and all segment mappings succeeded.
The low TEXT was `r-x` and its leaf function returned `42`; the identical high TEXT was `r--` and faulted on its first
instruction, with TXM `38 | 23`. Neither copy had dirty TEXT pages or software breakpoints. This control needed no rebasing
or binding and does not implement a general dylib loader. The test process was terminated, the temporary device dylib
removed, and the restored original app ran `df` normally to exit zero.

[Requesting execute permission afterward](shared-cache.md#requesting-execute-permission-with-mprotect) failed with
`EACCES`. Actual VM queries showed low TEXT current/maximum protections `r-x`/`rwx`, but high TEXT `r--`/`rw-` both before
and after mprotect. High execution still faulted on its first instruction; the restored original app again ran `df` to exit zero.

[Shared-region lifetime research](shared-cache.md#unregistering-the-retained-host-shared-region) found no app-callable
selective TXM unregister path. Detachment removes VM mappings but retains the nested pmap association; the examined XNU
and TXM only remove it during whole-address-space destruction. Fork inherits the association and exec installs it again.
This conclusion is bounded to public XNU 12377.1.9 and the supplied TXM 182.0 binary, which predates the live iOS 27 monitor.

Keep runtime binaries, signed outputs, logs, and experiment scripts under ignored `tmp/`. They are not repository assets.
The repository's [license explanation](../README.md#license) covers the project sources, not separately supplied Apple runtimes or SDKs.

When recording a new result, include the runtime build, signing identity/identifier, cache base and UUID, loaded-image source,
actual VM protections, exit status, and whether a debugger was attached. Keep unsuccessful experiments separate from confirmed behavior.
