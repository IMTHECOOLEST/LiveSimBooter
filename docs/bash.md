# Bash baseline and remaining process support

A noninteractive Bash baseline runs in LiveProcess on `iphone-0xf-26-cua-duy.local` (iOS 26.1, build `23B50`).
This establishes shell startup and builtins after native-cache removal. Full `fork`, `exec`, terminal creation, and interactive
job control are not implemented or established by this test.

## Runtime file

The locally supplied source is plain arm64 Bash 3.2.57 from
`/Users/duy/Developer/spironolactone/work/sshtar/bin/bash`, originally tagged for iOS 7. A copy was changed with `vtool` to
`LC_BUILD_VERSION` platform `IOSSIMULATOR`, minimum OS 26.0, SDK 26.0, then signed with the existing runtime procedure:

```sh
mkdir -p tmp/bash-probe
xcrun vtool -set-build-version iossim 26.0 26.0 -replace \
  -output tmp/bash-probe/bash-simulator /Users/duy/Developer/spironolactone/work/sshtar/bin/bash
python3 Tools/sign-sim-executable.py --source tmp/bash-probe/bash-simulator --output tmp/bash-probe/bash-signed \
  --identity D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter
```

The signed output retains `MH_EXECUTE`, has no entitlements, and clears `CS_EXECSEG_MAIN_BINARY`. It was copied to the App
Group's `RuntimeRoot/bin/bash`. Its only linked dependencies, `/usr/lib/libiconv.2.dylib` and `/usr/lib/libSystem.B.dylib`, are
already available in the rebuilt iOS 26 cache at `0x120000000`; no extra loose libraries were copied. The supplied source
binary remains unchanged. Build copies, signatures, and logs remain in ignored `tmp/bash-probe/`.

## Builtin-only device result, 2026-10-25

Guest PID 79320 ran Bash without startup files. It printed `bash=3.2.57(1)-release` and `answer=42`, completed a shell loop,
and deliberately exited 37. The native broker reported raw wait status `0x2500`. No external program, pipeline, command
substitution, or fork/exec path was exercised. This run was launched normally, without LLDB.

Evidence is in `tmp/bash-probe/console-builtins.log` and `tmp/bash-probe/sign-bash.json`. The latter records the output hash,
verified signed pages, cleared executable-segment flag, absent entitlements, and unchanged signing input. The later
`App terminated due to signal 2` line is the stopped native console session; the broker had already recorded Bash's exit 37.

## Terminal diagnostic

A separate simulator probe, PID 79331, inspected existing standard descriptors and attempted terminal opens without changing
terminal settings, session IDs, or process groups:

| Operation | Observed result |
| --- | --- |
| `isatty(0)` | False; stdin was not a TTY. |
| `isatty(1)` / `isatty(2)` | True for inherited console output descriptors. |
| `tcgetattr(1)` / `tcgetattr(2)` | Succeeded. |
| `tcgetpgrp(1)` / `tcgetpgrp(2)` | Failed with `EPERM`. |
| Open `/dev/tty` or `/dev/ptmx` | Failed with `EPERM`. |
| `posix_openpt(O_RDWR \| O_NOCTTY)` | Failed with `EPERM`. |

The diagnostic completed with exit 0. Evidence is in `tmp/bash-probe/console-pty.log`; the source and its behavior notes are
`pty-probe.c` and `PTY-README.txt` in the same directory. Inherited console TTY descriptors do not establish that the guest can
allocate a PTY or obtain a controlling terminal.

A subsequent LLDB probe in native host PID 79330 also found `posix_openpt(O_RDWR | O_NOCTTY) == -1` with `EPERM`.
A direct `/dev/ptmx` open returned -1, but that expression's errno capture was inconclusive and does not establish its error
code. Neither call opened a descriptor; the debugger was detached afterward. Evidence is in
`tmp/bash-probe/host-pty-lldb.txt`. Thus moving this allocation into the current native host did not make the tested PTY path
available.

## Process-model questions

These are constraints for future fork/exec emulation, not a selected implementation plan. Unmodified Fish now has a verified
noninteractive baseline through the existing `posix_spawn` path; whether every required execution path avoids fork remains
unverified. See the separate [Fish results](fish.md).

- A fork-like operation needs a consistent snapshot at identical guest virtual addresses, with signed executable mappings
  recreated and mutable pages copied privately or inherited through actual copy-on-write. Shared writable mappings alone
  would allow parent and child to modify each other's memory and would not provide fork semantics. Native fork also creates
  kernel process/task/thread state, as shown by [XNU's fork implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_fork.c).
- Resuming a cloned guest also requires its register state and stack, pthread/TSD storage, libc fork hooks, pointer-munging
  state, descriptors, and Mach rights to remain coherent. Copying their numeric values is insufficient across native tasks.
  A single-threaded restriction can reduce the initial scope but does not remove these requirements. [libSystem's initialization
  code](https://github.com/apple-oss-distributions/Libsystem/blob/main/init.c) coordinates prepare/parent/child hooks across runtime components.
- Replacing a guest image through a fresh LiveProcess instance needs a stable guest-visible PID and a broker transaction that
  rebinds its parent ledger, suppresses the retiring native process's exit notification, and transfers its existing simulated
  children. Launching another instance alone would expose a new process rather than implement `exec` semantics. Apple's
  [execve manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/execve.2.html)
  specifies PID preservation, descriptor inheritance, and signal-state changes during image replacement.
- Interactive Bash additionally needs usable terminal transport and session/process-group behavior. The terminal diagnostic
  establishes the current restrictions, not an implemented PTY or job-control solution.

The existing brokered `posix_spawn`, child `kill`, `waitpid`, and SIGCHLD paths are described in [multiprocess.md](multiprocess.md).
They are useful primitives for this work, but do not substitute for the missing fork and exec behavior of an unmodified shell.
