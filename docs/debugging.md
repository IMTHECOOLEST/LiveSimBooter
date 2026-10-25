# Device debugging

The commands below match the working command-line procedure on 2026-10-24. Run them from the repository root with Xcode's
command-line tools selected. Unlock the iPhone and keep it unlocked for launch/attach. Use the current device identifier
from `xcrun devicectl list devices`; the tested device UDID was `00008150-0006505C1428401C`.

```sh
DEVICE=00008150-0006505C1428401C
APP=tmp/device-build/Build/Products/Debug-iphoneos/LiveSimBooter.app
mkdir -p tmp/device-log
```

## Install and transfer

The host target requests `com.apple.developer.kernel.increased-memory-limit` through `LiveSimBooter.entitlements` in both
Debug and Release. Use a provisioning profile that grants it and inspect the final signed entitlements before installation.
It is separate from Extended Virtual Addressing; verify the intended VM range under LLDB instead of assuming that range is available.

Install a normally signed development build of the host app. Compilation without signing is useful for checks but does
not produce an installable device build. Keep the local executable and dSYM matching the installed app for source debugging.

```sh
xcrun devicectl device install app --device "$DEVICE" "$APP" --json-output tmp/device-log/install.json

xcrun devicectl device copy to --device "$DEVICE" --source tmp/cache-base-tools/dyld_sim \
  --destination Documents/RuntimeRoot/usr/lib/dyld_sim \
  --domain-type appDataContainer --domain-identifier com.kdt.LiveSimBooter --json-output tmp/device-log/dyld-copy.json

xcrun devicectl device copy to --device "$DEVICE" --source tmp/cache-base-tools/signed/dyld_sim_shared_cache_arm64 \
  --destination Documents/RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64 \
  --domain-type appDataContainer --domain-identifier com.kdt.LiveSimBooter --timeout 120 --json-output tmp/device-log/cache-copy.json
```

Transfer by app data-container domain instead of hardcoding a container UUID; reinstalling can change it.
Stop the app and preserve the current files before replacing them. The [shared-cache notes](shared-cache.md) list the existing backups.
File transfer does not establish signature acceptance; capture the next launch.

## Capture AMFI and TXM

The test device exposes SSH on port 2222. Start capture before launch in a separate terminal and stop it after the process exits.
The SSH options below match this test setup and do not record its host key in the Mac's known-hosts file.

```sh
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@iphone-0x11-cua-duy.local \
  'log stream -p amfid' > tmp/device-log/amfid.log 2>&1
```

For kernel TXM failures as well, use this broader capture instead:

```sh
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@iphone-0x11-cua-duy.local \
  > tmp/device-log/security.log 2>&1 <<'REMOTE'
log stream --style compact --level debug --predicate \
  'process == "amfid" OR (process == "kernel" AND (composedMessage CONTAINS[c] "TXM" OR composedMessage CONTAINS[c] "AMFI"))'
REMOTE
```

An AMFI profile success is only one stage. Also check `F_ADDFILESIGS_RETURN`, actual mapping permissions, and the eventual
execution result. A successful `mmap` return can still be `r--` when TXM refused executable association.

## Simulator dyld diagnostics

The loader enables these variables in the simulator's copied environment, after native dyld has completed startup:

```text
DYLD_PRINT_TO_STDERR=1
DYLD_PRINT_LIBRARIES=1
DYLD_PRINT_SEGMENTS=1
DYLD_PRINT_INITIALIZERS=1
DYLD_PRINT_LOADERS=1
DYLD_PRINT_SEARCHING=1
DYLD_PRINT_APIS=1
DYLD_PRINT_NOTIFICATIONS=1
```

They show cache and segment mapping, dependency searches, loader selection, initializers, API calls, and image
notifications. Output goes to stderr, visible in Xcode's debug console or the `devicectl --console` captures below.
The loader removes an inherited `DYLD_PRINT_TO_FILE` from that copied environment so it cannot redirect the capture.
Native environment strings are untouched. Dyld still applies the real AMFI policy's `allowEnvVarsPrint` gate.

These flags are presence switches: assigning `0` still enables them. To disable a category, remove its name from the
diagnostic list in [DyldSimLoader.cpp](../LiveSimBooter/DyldSimLoader.cpp). `DYLD_PRINT_BINDINGS` is optional; add it to that list
to log every fixup. The first loose-library launch with that flag emitted 867,477 console lines (49 MB) and ended with
SIGKILL before `df` ran. The capture does not establish the kill's cause. It is omitted from the default list to limit output.

The final device test on 2026-10-24 (PID 5630) printed 2,983 console lines and ran `df` to exit `0` after host-cache removal.
The log shows the working cache at `0x120000000` and loose libraries overriding cached images; this was not a cache-only test.
The concurrent SSH capture contained no AMFI signature rejection or TXM failure. Runtime files were preserved during the
in-place app update. Evidence is in `tmp/dyld-diagnostics/console.log`, `launch.json`, and `security.log`; the earlier verbose
attempt is retained as `bindings-console.log` and `bindings-launch.json` in the same ignored directory.

## Launch stopped and attach LLDB

Do not provide `DYLD_SHARED_REGION=private` through `--environment-variables`, `DEVICECTL_CHILD_DYLD_SHARED_REGION`, or the
app's native launch scheme. It affects native dyld before `main()` and caused a separate pre-handoff failure.

In terminal one, launch stopped with console capture. This command remains running until the app exits:

```sh
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --start-stopped --console --timeout 600 \
  --json-output tmp/device-log/launch.json -- com.kdt.LiveSimBooter > tmp/device-log/console.log 2>&1
```

In terminal two, find the live `LiveSimBooter` PID, then open LLDB:

```sh
DEVICE=00008150-0006505C1428401C
xcrun devicectl device info processes --device "$DEVICE" --json-output tmp/device-log/processes.json
xcrun lldb --no-lldbinit
```

Use the current CoreDevice identifier and PID in the following LLDB commands. The UUID shown is from the recorded test,
not a required constant. `target create` must point at the executable matching the installed app.

```text
settings set -g target.require-hardware-breakpoint true
target create --arch arm64 tmp/device-build/Build/Products/Debug-iphoneos/LiveSimBooter.app/LiveSimBooter
device select 25FDAD7E-026E-51E2-9C23-60AD659ABFCE
device process attach -p PID
breakpoint set -n 'lsb::mmap' -c 'location == (void*)0x120000000'
breakpoint set -H -a 0x129d4ff78
continue
```

At the first cache mapping, inspect the **whole** proposed range `[0x120000000, 0x135508000)` before continuing into `MAP_FIXED`.
`memory region 0x120000000` must report an unmapped region extending beyond the end; otherwise walk each returned region
until the end and stop the test if any is mapped. Do not infer the full range is free just from its first page.

```text
memory region 0x120000000
bt 8
breakpoint disable 1
continue
memory region 0x120000000
memory region 0x12b2f3fff
```

At cached `libSystem_initializer`, both TEXT endpoints were in the same `r-x` region starting at `0x120000000`.
The initializer's absolute address is specific to the recorded cache; recompute it after changing inputs.
Set the global hardware requirement before target creation and attach, not just `-H` on individual user breakpoints:
LLDB also creates internal breakpoints. Hardware resources are limited; check insertion errors and reduce active breakpoints
if necessary rather than assuming every requested breakpoint was installed.

```text
image list -o -f
breakpoint set -H -s libsystem_c.dylib -n getmntinfo
continue
bt
continue
```

LLDB should recognize simulator libraries automatically through the app's debugger bridge. Do not manually inject modules
when evaluating whether that bridge works. Both the mixed cache/loose run and the later cache-only run listed 49 named modules.
In the cache-only run, all 46 dependencies had cache-backed image headers and addresses. The legacy image array had 48 entries;
dyld_sim is published separately. Module count alone therefore does not distinguish cached dependencies from loose overrides.

The cache-only run also hit `__pthread_init` at `0x12b2688d8` and `getmntinfo` at `0x12b09e984`. These addresses belong to
the recorded cache UUID. All 219 loose dylibs had been moved outside RuntimeRoot for that test and were restored afterward.
Reproduce that distinction with a recorded move manifest and restoration step, rather than assuming the currently installed
runtime has no loose files.

To read published metadata without executing functions in the target, use LLDB's Python value API:

```text
script target = lldb.debugger.GetSelectedTarget()
script info = target.FindFirstGlobalVariable('sActiveProcessInfo').Dereference()
script print(hex(info.GetChildMemberWithName('sharedCacheBaseAddress').GetValueAsUnsigned()))
script print(info.GetChildMemberWithName('processDetachedFromSharedRegion').GetValueAsUnsigned())
script host = target.FindFirstGlobalVariable('sBridge').GetChildMemberWithName('hostSnapshot')
script print(hex(host.GetChildMemberWithName('dyldImageLoadAddress').GetValueAsUnsigned()))
```

Use `memory region` on that saved native dyld address after handoff; the successful test showed `---`.
The debugger metadata pages can remain readable independently, so readable metadata does not imply the host cache is still loaded.
For image-source checks, cached Mach-O headers carry `MH_DYLIB_IN_CACHE` (`0x80000000`); count those separately from total LLDB modules.
Avoid calling host libc, pthread, or Objective-C expressions after cache detachment.

## Internal breakpoint investigation

LLDB temporarily enables pthread thread-creation breakpoints while stepping over its dyld notification breakpoint.
`breakpoint list -i` can show those locations disabled at a later stop even though LLDB already inserted and removed them.
The default debugger therefore writes cache code even when all explicit user breakpoints use `-H`.

Enable protocol and breakpoint logging before target creation/attach when investigating this behavior:

```text
log enable -T -p -f tmp/device-log/packets.log gdb-remote packets
log enable -T -p -f tmp/device-log/breakpoints.log lldb break dyld process
settings set -g target.require-hardware-breakpoint true
```

Packet timestamps are Unix epoch seconds from the Mac; convert them to the device log's timezone before comparing events.
Mac/device clock skew was not measured in these runs, so converted timestamps alone cannot establish precise cross-device ordering.
Look inside `jMultiBreakpoint` batches as well as individual packets. `Z0`/`z0` insert/remove software breakpoints;
`Z1`/`z1` insert/remove hardware breakpoints. Verify the corresponding replies, rather than inferring installation from a breakpoint list.

The controlled 2026-10-24 comparison observed these cached pthread locations:
`0x12b269a74`, `0x12b26ab5c`, and `0x12b26e618`.

| Run | Actual internal breakpoint requests | Cache TEXT dirty pages | TXM errors |
| --- | --- | --- | --- |
| Default internal breakpoints, PID 4631 | Three `Z0` requests followed by `z0`; all `OK` | Two pthread pages | None |
| Global hardware requirement, PID 4638 | `Z1`/`z1` at all three hooks, two cycles; all `OK` | Zero | None |
| Full default replay, PID 4645 | Internal `Z0` insertions succeeded | The same two pthread pages at all three stops | None |

The software insert/remove cycle occurred at `12:28:37.279–.286 +0700`; the first hardware cycle at `12:32:38.911–.918 +0700`.
Both the hardware run and full default replay reached cached `libSystem_initializer`, `__pthread_init`, and `getmntinfo`,
with all 46 dependencies cached and 49 recognized modules. The hardware run kept TEXT clean at all three stops; the default
replay retained the same two dirty pthread pages. Both exited `0`, with no AMFI rejection. The global hardware requirement
therefore avoids the observed debugger writes into cache TEXT.

None of the three runs reproduced the earlier `TXM selector: 38 | 23` event. The dirty-page difference does not prove those
writes caused that event. The first traced default run omitted the original run's later pthread and `getmntinfo` stops;
the full default replay included those stops and still produced no TXM error.
These tests used fresh processes with the same installed cache file; they did not compare fresh file objects or cold boots.
The original kernel event does not identify the affected PID, and its LLDB session had no packet capture.
All 219 loose dylibs were restored and independently checked against the move manifest after testing.

The comparison is summarized in `tmp/txm-breakpoints/result.json`. Evidence is in `tmp/txm-breakpoints/baseline/`, `tmp/txm-breakpoints/hardware-only/`, and `tmp/txm-breakpoints/baseline-full/`,
including packet, breakpoint, LLDB, and security logs. These experiment artifacts remain outside Git.

The subsequent [original-base retest](shared-cache.md#original-base-hardware-only-retest) reproduced `TXM 38 | 23` with
zero software-breakpoint requests and zero dirty TEXT pages. At `0x180000000`, `mmap` still returned `r--`; execution faulted
in cached `LibSystemHelpers::version()` before the cached images or pthread hooks were published. The restored cache at
`0x120000000` then ran df successfully. This separates the reproducible original-base mapping failure from the earlier,
unreproduced relocated-cache event.

## Confirm a normal launch

After the LLDB run, repeat without `--start-stopped` or an attached debugger:

```sh
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --console --timeout 60 \
  --json-output tmp/device-log/normal-launch.json -- com.kdt.LiveSimBooter > tmp/device-log/normal-console.log 2>&1
```

Check the filesystem-table output and `result.terminationResult.exitCode` in both launch JSON files. A successful launch request
alone does not mean `df` completed. Record any AMFI/TXM rejection and crash alongside the exit status.

The completed cache-only test exited `0` in both modes. Its LLDB capture contained one `TXM selector: 38 | 23` event at
`12:20:51.144`; cached pthread pages were subsequently executable/dirty and internal LLDB thread-creation breakpoints were
present. The cause of that event was not established. The fresh normal-launch capture contained no TXM errors or AMFI
rejections. Keep those two observations separate when comparing future runs.

For a host `dlopen` diagnostic only, add `--probe-dlopen-dyld-sim` after the bundle identifier. It prints the handle and
`dlerror()`, then exits before custom mapping or cache removal. Its result is not a substitute for testing the custom loader.
