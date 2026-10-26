# Guest launchctl

`Guest/LaunchctlControl.c` runs stock simulator `/bin/launchctl` inside the existing simulator launchd session. It inherits
the guest bootstrap port, runtime environment, and spawn shim; each command uses the broker's ordinary spawn/wait path.
`Tools/guest-launchctl.py` submits literal arguments over native SSH and retrieves stdout, stderr, and exit status. Commands
do not use a shell, and the worker can execute only `/bin/launchctl`.

## Setup

Build with `Tools/build-launchctl-control.sh`. Sign its `tmp/launchctl-control/launchctl-control` output and the matching
runtime's stock `bin/launchctl` with `Tools/sign-sim-executable.py`, using the application's signing identity and identifier.
That signer retains MH_EXECUTE and the simulator platform while clearing CS_EXECSEG_MAIN_BINARY. Place the signed files
at `RuntimeRoot/usr/libexec/launchctl-control` and `RuntimeRoot/bin/launchctl`. The matching iOS 26 launchctl needs only
libSystem, libobjc, and CoreFoundation; the worker needs only libSystem.

Install this plist as `RuntimeRoot/System/Library/LaunchDaemons/com.kdt.LiveSimBooter.launchctl-control.plist`, and add its
label to the host's existing `--launchd-services` selection. Explicit labels replace the default additional service list,
so retain the other labels needed for the current boot.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
    <key>Label</key><string>com.kdt.LiveSimBooter.launchctl-control</string>
    <key>ProgramArguments</key><array><string>/usr/libexec/launchctl-control</string></array>
    <key>WorkingDirectory</key><string>/private/var/mobile/</string>
    <key>StandardOutPath</key><string>/private/var/mobile/Library/Logs/launchctl-control.log</string>
    <key>StandardErrorPath</key><string>/private/var/mobile/Library/Logs/launchctl-control.log</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><false/>
</dict></plist>
```

The worker requires simulator platform 7 and the host's sibling `RuntimeRoot` / `SimulatorData` directories. It creates
`SimulatorData/var/run/launchctl-control`. Directory components are opened without following symlinks, and a held worker
lock prevents two instances from consuming the same queue. Native SSH needs Python 3 and write access to that directory.

## Commands

Supply the actual SSH destination and physical SimulatorData path; the tool has no device or App Group default. Port 2222
is the default. SSH uses the user's normal host-key configuration; additional SSH options can be passed with `--ssh-option`.

```sh
python3 Tools/guest-launchctl.py --host root@device.local \
  --data-root /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/SimulatorData \
  --output-dir tmp/guest-launchctl -- print system
```

Keep those options and replace the arguments after `--` for subsequent commands:

```text
list
print system
kickstart -k user/501/com.apple.lsd
bootstrap user/501 /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/SimulatorData/Library/YourJobs/example.plist
bootout user/501/com.example.job
```

Use `print system` for diagnostics and `user/501` for the tested bootstrap/bootout domain. Do not infer the mutation target
from the diagnostic command's `system` spelling. The bootstrap **plist argument is a physical path** because launchctl
opens the file. Inside that plist, working directories and log paths use guest paths such as `/private/var/mobile/Library/...`;
physical App Group paths in those job fields can be translated a second time. The SSH client's `--data-root` is also a
physical path because native SSH accesses the App Group directly. See [guest sshdog](sshdog.md) for a complete example
that preserves a daemon's working directory by bootstrapping its original plist directly.

The initial device trial with the temporary worker completed `print system`, `list`, and `bootstrap`/`bootout user/501`
with exit 0. The promoted worker subsequently completed `list`, bootstrap of both the original sshdog and application-launch
plists, and bootout of the application-launch probe with exit 0, without restarting the host or simulator launchd.
Records are under `tmp/app-launch-fix/promoted-results`: `91db0a23abba4d2f9e5e03bd24069f55` (list),
`64cf219bf2504093a219582337e0b883` (sshdog bootstrap), `4375fd94d09f42a5b4dcc2c8949b9a3c` (application-launch bootstrap),
and `d9b31729d60b48249caa3e5553d99099` (probe bootout). Those completed requests have zero spawn/wait errors and no timeout.
This validates job control; it does not imply that every bootstrapped program completed its intended work.

## Results and interrupted requests

Each call prints a fresh request ID and saves `<ID>.request.json`, `<ID>.json`, and `<ID>.log` under `--output-dir`. The final
JSON contains spawn/wait errno, raw wait status, decoded exit/signal, and timeout state. The client returns the command's
exit status, 124 for a command timeout, 125 for unknown/transport failures, or 75 when submission is rejected before publication.
Logs remain on the phone; local downloads are capped at 16 MiB and explicitly report truncation.

The client holds a queue ownership record until completion. Complete requests are published atomically; a second client
cannot overwrite a pending request. Arguments are passed byte-for-byte as UTF-8, with 1–63 nonempty arguments and a 16 KiB
request limit. The worker kills a command after 60 seconds and allows five more seconds to observe its termination. Client
polling defaults to 90 seconds and can be changed with `--timeout`.

If SSH disconnects or polling expires, the client never submits that command again. Retrieve the same operation using the
same connection/output options plus `--request-id ID`, with no command arguments. An interrupted worker, unresolved wait,
or failed result publication leaves `active` and its evidence intact and stops the worker. Inspect the request, its logs,
the live child, and `client.lock/owner.json` before manually clearing the queue and restarting the control job. Do not delete
these files just to retry an operation whose side effect may already have occurred. Completed logs/status can be removed
after collection.

This controls guest launchd jobs; it does not provide a general command shell, a guest SSH server, or new launchd policy.
Restarting the control job through its own queue can interrupt the reply, so use another management path for that job.
