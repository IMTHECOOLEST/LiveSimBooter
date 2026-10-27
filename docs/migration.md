# Move a simulator runtime to another iPhone

This runbook complements [agent setup](agent-setup.md). Keep the guest pinned to iOS Simulator 26.0 / 23A343 even when the physical
phone changes. As of 2026-10-26, the full local stage and finalization passed: 494,754 entries, 163 app-registration candidates,
and all applicable signing/section-preservation checks, with four recorded dependency gaps. USB rsync of the full runtime and
prepared extras completed, and the new runtime was activated on the destination iPhone 17. The runtime transfer covered
15,099,025,144 bytes; `RuntimeRoot.before-full-20261026` retains the previous root. No post-copy checksum pass was performed,
following the user's request. There is no in-app installer or automatic device-to-device migration.

The current host/shim default retains the native cache and redirects its workqueue entry with a hardware breakpoint.
This passed concurrent dispatch work, timers, and launchd spawning df without LLDB. After applying and signing the pinned
cache/helper metadata-name patch, the iPhone18,2 running iOS 27.0 / 24A435 reached a home screen with app icons, confirmed by
the user. lsd started and 647 guest metadata files were created. The migration initially omitted SimulatorHID; the plugin has
been restored for a separate touch-input retest. Input and broad application compatibility are not yet established by this boot.

Both the previous working host and the latest host with the wide native stack guard disabled ran `df` with exit status 0 after
receiving the two simulator root variables. The wide guard still produced cache-reservation `mmap` error `EACCES`, including after
explicitly releasing its reservation, and is now opt-in with `LSB_ENABLE_NATIVE_STACK_RESERVATION=NO` by default.

Normal simulator launchd attempts on the new phone were terminated with `CODESIGNING / InvalidPage` at native `start_wqthread`
after native-cache detachment. Native Metal helpers initialize on A19 Pro. An empty launchd test attached before handoff with
hardware-only LLDB breakpoints reached `dispatch_main`; a later interrupt found the simulator exception server, signal thread,
and workqueue wait. This debugger-assisted result does not verify a normal graphical boot or establish why normal launch differs.

A signed, directly linked native dylib probe loaded successfully, but its `__bsdthread_register` interposer was never called.
Read-only disassembly showed the native pthread call at `0x190118910` branching to an `ADRP`/`ADD`/`BR` island at `0x197d14b00`,
which computes the original registration function's address directly. There is no GOT lookup for ordinary dyld interposition to
redirect. This explains the negative probe; it does not establish that all native interposition is disabled. The later successful
retained-cache handoff leaves that kernel registration intact and redirects workqueue entry instead.

The 2026-10-27 hardware-breakpoint experiment also ran without LLDB. A mapped-function control inherited a task-default
breakpoint on a new ordinary pthread, returned 84 after redirection, then returned its original 42 after the owned thread slot
was removed. Exact task-state restoration left the next thread unaffected. Code-signing flags stayed `0x22003305`, with
`CS_DEBUGGED` clear throughout. The actual launchd trial confirmed successful arming and readback at native `start_wqthread`
before workqueue creation, but recorded no handled exception and exited with wait status 9. Earlier attempts reported
`CODESIGNING / InvalidPage` at the same address. Breakpoint-versus-fetch priority and actual workqueue inheritance on this
device remain unseparated; the control alone does not establish a working handoff.

The failed experiment removed the native cache. The current default enables both `LSB_RETAIN_HOST_CACHE` and
`LSB_WORKQUEUE_HARDWARE_BREAKPOINT`; unset values mean enabled. Set both explicitly to `0` only for a legacy diagnostic, using
the `DEVICECTL_CHILD_` prefix when launching through CoreDevice. Arming can follow native registration, but must precede worker
creation; the owned slot must persist for reused workers. See [the launchd trial](launchd.md#ios-27-hardware-breakpoint-trial)
and [retained-cache results](launchd.md#retained-native-cache-with-hardware-redirection).

Temporary probe processes and paused guests were stopped. The normal package with the wide guard disabled was restored and its
installation confirmed; independent process scans found no owned probe processes or debugserver sessions. The full transfer and
direct `df` baseline are complete. The subsequent patched-cache boot reached the home screen; remaining input validation is
recorded separately below.

## Record the source and destination

Record the runtime build, cache base and signing receipt, patched loader/launchd/shim versions, Fish and helper versions, native
app build, and simulator device type. The default simulated iPhone 15 Pro Max profile need not match the physical destination.
Keep one working source runtime and its mutable state until the new device passes verification.

Identify the destination with `xcrun devicectl list devices`. Install the correctly signed native app first. Both app and extension
profiles must include this device, their distinct identifiers, the same App Group, and Increased Memory Limit. Preserve the native
Metal backend under `LiveProcess.appex/Frameworks/NativeMetal`; these three libraries do not belong in `RuntimeRoot`.

Set task-local values; do not commit actual device identifiers, container UUIDs, keys, or credentials:

```sh
LSB_DEVICE='CURRENT_COREDEVICE_IDENTIFIER'
LSB_USB_UDID='CURRENT_USBMUX_DEVICE_UDID'
LSB_APP_GROUP='YOUR_REGISTERED_APP_GROUP'
LSB_SSH_USER='YOUR_NATIVE_DEVICE_SSH_USER'
LSB_SSH_PORT='YOUR_LOCAL_FORWARD_PORT'
LSB_DEVICE_SSH_PORT='YOUR_NATIVE_DEVICE_SSH_PORT'
LSB_SSH_DEST="$LSB_SSH_USER@127.0.0.1"
LSB_RUNTIME_STAGE='/absolute/path/to/completed/stage/RuntimeRoot'
LSB_DATA_STAGE='/absolute/path/to/reviewed/SimulatorData'
```

The native-device SSH service is externally provided. It is separate from an optional sshdog server running inside the simulator.
Do not assume that a previous local port still forwards to the intended phone. With a compatible libusbmuxd `iproxy`, use a
dedicated foreground terminal and bind only loopback:

```sh
iproxy --local --udid "$LSB_USB_UDID" --source 127.0.0.1 "$LSB_SSH_PORT:$LSB_DEVICE_SSH_PORT"
```

Inspect `iproxy --help` if the installed version uses different syntax. Verify the native SSH host key/device identity over this
connection before transferring. Do not create a second forward if a verified one already owns the port.

## Discover the actual App Group

The App Group identifier is stable; its filesystem UUID is not. CoreDevice can inspect the installed group without assuming a path:

```sh
xcrun devicectl device info files --device "$LSB_DEVICE" --domain-type appGroupDataContainer \
  --domain-identifier "$LSB_APP_GROUP" --no-recurse
```

Obtain the canonical physical group path from the host's `[broker] RuntimeRoot=...` startup record, or inspect container metadata
through the already authorized native SSH connection. The latter must match `MCMMetadataIdentifier` exactly to the configured
group; require one result. Do not guess from directory timestamps or copy a UUID from an older run. If metadata access is unavailable,
use the host/CoreDevice route rather than changing device permissions.

Set `LSB_DEVICE_GROUP` to the discovered directory containing `RuntimeRoot` and `SimulatorData`. List its existing contents and
available storage. A fresh App Group may initially contain only `Library`. The host migrates `Documents/RuntimeRoot` only when
the group has no `RuntimeRoot`; it does not merge a later Documents copy into an existing runtime.

## Finish a portable local payload

Use the explicit full scopes in [agent setup](agent-setup.md#stage-resources-apps-and-signed-loose-code), including `Developer` and
`RestoreVersion.plist`. Wait for a successful tool exit and its final `stage-manifest.json`. A directory with thousands of signed
files is still incomplete while the stage runs: final symlinks and app registration JSON are published last.

The finished payload must include the current patched `dyld_sim`, patched `launchd_sim`, guest shim, Fish, terminal helper,
control worker, app-registration helper, capabilities, and the separately signed cache. Include both external CoreSimulator
clients with their `Info.plist` files: `Library/PrivateFrameworks/SimFramebuffer.framework` and
`Library/Frameworks/SimulatorHID.framework`. They are outside the stock runtime root and require explicit `--add-binary`
inputs plus resource copies in [the staging recipe](agent-setup.md#stage-resources-apps-and-signed-loose-code). The host's
`SIMULATOR_HID_SYSTEM_MANAGER` points to the latter bundle; missing it can leave graphics working without touch delivery.
Keep original resources and signed uncached code together. Never overlay the original runtime wholesale onto this tree,
which would restore stock loaders and unsigned code.

Include the container-metadata filename patch from [agent setup](agent-setup.md#build-and-inspect-the-cache): run
`Tools/patch-ios26-container-metadata.py --source INPUT_CACHE --output NEW_CACHE` before signing the cache, and use its
`--mach-o` mode on the original MobileInstallationHelperService before passing that copy to `setup-ios26-runtime.py --add-binary`.
The tool validates five original 23A343 cache `__cstring` sections and the one matching loose helper, replacing the stock name
with `.com.kdt.LiveSimBooter.container-metadata.v1.plist` without changing its length. Both outputs require new signatures.
The device probe demonstrated exact-basename `EPERM`, and the kernel logged a sandbox `System Policy` denial for creating that
stock basename. The exact policy definition remains unidentified. The patched cache/helper pair then passed guest metadata
creation and lsd startup, reaching the home screen without LLDB. Cache UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8` and base
`0x110000000` were preserved; the replacement changes data bytes and therefore requires re-signing. Keep an older cache/helper
with its corresponding data: a data root whose metadata uses the stock basename needs a separately reviewed migration rather
than an incidental filename rewrite during transfer.

A local hard link can attach an already signed immutable cache without consuming another 2 GB, provided both paths are on the
same filesystem. After linking, do not rewrite, re-sign, chmod, or chown either path: they share an inode. Normal rsync copies the
file contents to the device. Retain the original signature/layout receipt; this is not a new signing operation.

`RuntimeRoot` and `SimulatorData` are separate transfer roots:

- `RuntimeRoot` holds code, cache, bundles, resources, and generated job definitions.
- `SimulatorData` holds mutable databases, preferences, logs, guest control state, and optional SSH configuration. Choose deliberately
  between a fresh data root, a stopped snapshot from the old setup, and retaining an existing destination data root. Do not merge live
  databases or overwrite destination state as an incidental consequence of copying code.

Regenerate configuration containing physical paths for the new App Group. In particular, [sshdog staging](sshdog.md) uses
`Tools/stage-sshdog.py --device-group ...` to create its launch job, Fish hook, and key configuration. Supply the intended public
authorization keys and host key explicitly; preserve their restrictive modes. Review generated plists/JSON/Fish hooks for the old
container UUID. Do not blanket-replace text inside binaries or databases. Job executable/log paths should remain guest paths except
where a documented adapter requires a physical host path.

## Transfer to inactive directories

Stop the destination host and its simulator/native helper processes before publication. Detach any debugger and verify the target
resumed before stopping it normally. Keep the old runtime and data in place during transfer; choose new inactive directory names.

With the verified USB connection still running, a typical pair of transfers is:

```sh
LSB_DEVICE_GROUP='/private/var/mobile/Containers/Shared/AppGroup/DISCOVERED_UUID'
LSB_RSYNC_SSH="ssh -p $LSB_SSH_PORT"
rsync -rlpt --partial -e "$LSB_RSYNC_SSH" "$LSB_RUNTIME_STAGE/" \
  "$LSB_SSH_DEST:$LSB_DEVICE_GROUP/RuntimeRoot.incoming/"
# Only when a reviewed data stage is intended for this destination:
rsync -rlpt --partial -e "$LSB_RSYNC_SSH" "$LSB_DATA_STAGE/" \
  "$LSB_SSH_DEST:$LSB_DEVICE_GROUP/SimulatorData.incoming/"
```

The trailing slash copies each root's contents. Use `--dry-run --itemize-changes` first when destination contents are uncertain.
Do not add `--delete`, `--checksum`, or `--inplace`; a partial transfer must not delete rollback files or modify active mapped code.
Honor the user's transfer-verification preference. The current workflow skips a post-copy checksum pass; local signing and layout
validation still apply. A dropped connection can resume into the same inactive directory using the same source artifacts.

After both intended transfers exit successfully, inspect their exit codes, required paths, filesystem ownership, and permissions.
Files uploaded by a privileged SSH account must be readable by the native app, and mutable data must be writable by that app's
device account. Preserve private-key permissions. Do not apply a blanket world-writable mode or alter the host app bundle.

Publish with reviewed same-filesystem renames while every affected process is stopped: preserve an existing `RuntimeRoot` under
a unique rollback name, then rename `RuntimeRoot.incoming` to `RuntimeRoot`. If replacing data, preserve its existing root separately
and publish the matching inactive data root. There is no atomic transaction spanning both renames; do not launch between them.
If keeping destination data, leave it in place. On a fresh destination with no staged data, let the host create its required data
directories and copy only explicitly prepared configuration afterward.

## Boot, register, and retain rollback

Launch `/bin/df` first using [the setup smoke-test command](agent-setup.md#transfer-and-boot). It requires the newly discovered
`LSB_DEVICE_GROUP`: pass `DEVICECTL_CHILD_SIMULATOR_ROOT="$LSB_DEVICE_GROUP/RuntimeRoot"` and
`DEVICECTL_CHILD_SIMULATOR_SHARED_RESOURCES_DIRECTORY="$LSB_DEVICE_GROUP/SimulatorData"` to CoreDevice. Direct mode does not inject
those variables, and omitting them can trigger stock libsystem_featureflags' missing-root trap. Graphical mode already supplies them.
The destination test reproduced that trap without the variables, then printed the filesystem table and exited 0 with them using
both the previous working host and the latest default-off guard build. The migrated runtime therefore has a passing `df` baseline;
the later metadata-name patch also reached the home screen, but the 163 registration candidates are not a claim that every app works.
Check the broker's actual exit record, then boot graphical launchd with the native host foregrounded.
Check the cache mapping, helper readiness, framebuffer, Fish tab, and one known app. Use the generated app-registration manifest
inside guest launchd after lsd starts; rsync completion alone does not register icons. See [guest control](guest-control.md).

Record actual results separately from copy success. Full-runtime staging is not proof that every included application or optional
daemon works. Keep deferred application failures deferred instead of adding unrelated compatibility changes during migration.
If the baseline fails, stop the new session and restore the matching runtime/data rollback pair. Delete old copies only after the
replacement is verified and the user no longer needs them. Stop only the USB forward and debugger sessions created for this task.

The iPhone 17/iOS 27 migration subsequently passed launchd spawning df without a debugger by retaining the native cache and
redirecting its workqueue entry with a hardware breakpoint. This is now the default and requires the matching rebuilt host/shim;
see [the setup note](agent-setup.md#transfer-and-boot) and [measured results](launchd.md#retained-native-cache-with-hardware-redirection).
The full cache and native mapping fit together on the tested boot. After the filename-specific sandbox denial was isolated,
the signed cache/helper patch allowed lsd startup and produced 647 guest metadata files. The user confirmed the home screen
with app icons on iPhone18,2 / iOS 27.0 / 24A435 without LLDB. The initial migration lacked the external SimulatorHID plugin;
restoring the signed plugin and its Info.plist then restored touch markers, home-page swipes and opening Contacts, confirmed by the user. Evidence is in
`tmp/lsd-metadata-default/results/`, including `sandbox-policy-kernel.log`, `patched-lsd-status.log`, and the guest logs.
