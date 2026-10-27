# Native process names and argument storage

Every extension still executes the signed `LiveProcess` image. After its authenticated broker check-in, it writes a readable basename into
the original native `argv[0]` allocation. Native Metal helpers retain that stack and use `Metal.backboardd`, `Metal.SpringBoard`, and `Metal.public`;
replacement SpringBoard helpers retain the same label. Guest argument and environment vectors are separate and remain unchanged.

Readable native `ps` arguments for simulator guests remain unresolved in the default build: later cache mapping can overwrite the original native
argument storage. The experimental wide stack reservation described below is **disabled by default** because device cache mapping fails with it.

`kern.procname` also receives that basename as a best-effort kernel long name. Failure is logged without preventing launch. This field is separate
from both the signed executable path and native `p_comm`; setting it alone does not fix the argument text printed by Apple's `ps`.

## Why guest names became instructions

The kernel's `KERN_PROCARGS2` implementation reads the saved native argument window beneath `p->user_stack`. Moving execution to a new stack,
copying guest arguments, or updating libSystem's argv pointers does not change that kernel address. The full simulator cache overwrote the old
native stack after the handoff. Read-only samples from native launchd PID 12197 and Contacts PID 12247 returned ARM64 instructions as argv strings.
Those bytes match the signed cache at VM addresses `0x16b6f3b08` and `0x16bd13b08`, respectively.

The evidence is in `tmp/process-arguments/procargs-before.json`; it contains only a short prefix, not full process environments. Kernel source:
[sysctl_procargsx](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_sysctl.c) and
[create_unix_stack](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L6680).

## Disabled experiment: preserve the kernel's initial stack

`LiveProcess` retains ordinary `LC_MAIN` startup and an 8 MiB stack request. Its assembly source contributes a 16 KiB zero-fill placeholder
`__LSB_STACK` segment with no access permissions. Normal builds keep only that linked placeholder and treat it as an ordinary resident segment.
`LSB_ENABLE_NATIVE_STACK_RESERVATION` is `NO` in both Xcode configurations. Only an explicit `YES` passes `--enable` to the pre-signing
`Tools/prepare-native-stack.py` phase, expanding the segment to `0x10b000000–0x170000000` and updating its private symbol. Disabling it again restores
the linked placeholder, including on incremental builds. Segment count, chained fixups, entry point, file size and low `__LINKEDIT` remain intact.
The normal signing step covers either layout.

On the supported arm64 layout, XNU slides the executable upward by less than 80 MiB and the default stack downward from `0x16fe00000`.
The inaccessible segment covers that entire stack window, causing `create_unix_stack` to use its normal allocation fallback and record the new
address in the kernel. The segment ends below `0x175000000`, leaving the native shared region at `0x180000000` available during startup.

The guard leaves 176 MiB between the native image's slid base and its own beginning. The 8 MiB stack and roughly 36 MiB native image leave about 132 MiB in the current debug build;
this is sufficient for the current `dyld_sim` and Fish images. At most 80 MiB of unused guard can remain below the fixed simulator cache
base during native preparation. These are inaccessible virtual reservations, not initialized file bytes or a physical-memory allocation of that size.
After the final native worker check, the loader explicitly deallocates only this validated empty segment before cache handoff. The kernel-selected
argument stack is separately checked against both the simulator cache and the entire segment, and remains mapped. A deallocation error aborts
before TLS/cache detachment and reports the Mach error and exact extent.

Only the native loader image's exact prepared, file-empty, inaccessible segment is exempted from cache collision checks. A simulator image
cannot obtain that exemption by reusing its name. If the kernel fallback stack still overlaps the selected simulator cache, handoff fails before
overwriting arguments. This deliberately fails rather than relying on undocumented updates to kernel process structures or modifying cache text.

The kernel's native argc remains the original extension argc. The readable native name identifies the guest; it is not a complete mirror of the
guest's argument list. Guest programs continue to receive their actual full argv and environment from the broker.

## Validation

`Tests/NativeStackReservationTests.py` builds and signs a disposable arm64 executable with modern chained fixups, validates malformed layouts and
all possible ASLR pages, then launches five fresh processes. Each checks the kernel and pthread stack addresses, the native `KERN_PROCARGS2`
name, and unchanged adjacent arguments/environment. Loader tests cover the narrow exemption, malformed reservations, ordinary code collisions,
and refusal of a fallback stack inside the cache. Broker tests check all three native helper labels and the replacement-helper label.

Local tests cover default-off layout, on/off incremental transitions, malformed layouts, and releasing/reusing the exact experimental guard
while preserving kernel argv in five signed native probe launches.

The iPhone 17 test preserved the native argument stack at top `0x105150000`, size 8 MiB, but the subsequent anonymous cache reservation failed.
Explicitly deallocating the exact guard succeeded; cache `mmap` still returned `EACCES` (13) at `0x110000000`, size `0x7e194000`.
The previous package without the wide reservation successfully ran `df` with the required runtime environment. These results establish a regression
associated with the wide native layout, but do not establish the kernel/TXM mechanism. The experiment remains off; no TXM state manipulation is used.
Evidence: `tmp/migration-iphone17/df-console.log`, `df-stack-release-console.log`, and the fallback test logs in that directory.

Reservation failures log numeric errno, address and size through app-local code even after cache detachment. No native libc formatting is used
at that point. The current default package retains the helper-name changes and legacy simulator stack relocation; guest `ps` repair is deferred.
