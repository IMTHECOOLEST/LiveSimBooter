# Socket-backed terminal

The native app now starts `/bin/fish -i -N -P` with an inherited terminal endpoint when launched without guest arguments.
An initialization command disables job control, enables `fish_use_posix_spawn`, and selects a simple `fish> ` prompt.
This keeps the upstream Fish executable unchanged. The integrated terminal and pthread bridge reached an interactive prompt
on the iPhone, accepted editing keys, ran `df` and Bash, interrupted a running child, and exited cleanly on Ctrl-D.

The terminal runs in the native UIKit host. LiveProcess remains C, releases native XPC state before its single-thread check,
and detaches the host cache as before. The guest additions are C interposers compiled into `libLiveSimSpawn.dylib` and use
the Simulator's libc after handoff. The terminal does not create a native kernel PTY.

## Launch and files

The runtime needs the existing cache, dyld_sim, updated spawn shim, and the [pristine Fish build](fish.md) at `bin/fish`.
Keep the app foregrounded. A normal app launch opens its terminal; explicit commands keep their previous console behavior
unless prefixed with `--terminal`:

```sh
# Default interactive Fish terminal.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter

# Wrap an explicitly configured shell in the terminal.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter --terminal /bin/fish -i -N -P -C 'status job-control none; set -g fish_use_posix_spawn 1'

# Run the existing process test without the fake terminal.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter /bin/spawn-probe --repeat 2 --verify-statuses
```

Terminal mode sets `TERM=xterm-256color`, `HOME` to `RuntimeRoot/home/mobile`, and `PATH` to the actual RuntimeRoot `bin` and
`usr/bin` directories. It creates the home directory when needed. Programs can then find `df` through PATH without a general
`chroot` implementation. Explicit `--terminal` commands choose their own job-control policy; only the default Fish arguments
disable it automatically. The loader continues to leave `DYLD_PRINT_*` diagnostics off unless supplied explicitly.

## Transport and guest controls

`Host/TerminalSession.m` creates an `AF_UNIX/SOCK_STREAM` socketpair. The host keeps the nonblocking master endpoint and
transfers duplicates of the other endpoint as guest descriptors 0, 1, and 2 through the existing spawn plan. Real sockets
provide stream reads/writes, readiness, backpressure, EOF, and descriptor inheritance. stdout/stderr share the same endpoint.
Partial host writes queue up to 1 MiB and resume through a dispatch write source. Cancellation handlers retire the dispatch
registrations before their descriptors close. Read/write callbacks process bounded amounts before yielding to the main queue,
so a continuously writing guest cannot monopolize keyboard and signal handling.

A 16 KiB file beside RuntimeRoot holds the shared termios, window size, foreground identifier, and slave socket identity.
Its path is passed in `LSB_FAKE_PTY_STATE`. The guest constructor maps it through the ordinary App Group filesystem; spawned
descendants inherit the environment entry and socket open description. Matching the socket's device/inode identifies aliases
after `dup`, fileport transfer, and descriptor renumbering without globally treating stdin or every socket as a terminal.
Unrelated descriptors retain native behavior. The host removes the state file when the session closes; already mapped state
remains available until the corresponding guest exits.

`ProcessSupport/FakePTY.c` implements the shared records and host line discipline. `Guest/FakePTY.c` interposes `isatty`,
termios access, foreground-group access, window ioctls, and tty naming for matching descriptors. `TCSANOW` updates supported
attributes; canonical input implements line editing, echo, CR conversion, and selected signal characters. Noncanonical input
supports `VMIN=1, VTIME=0`. Output applies `OPOST/ONLCR` when selected. `ttyname` reports `/dev/lsbtty` as a label; that path
is not an openable device node.

The host forwards VINTR/VQUIT and resize notifications through the broker's existing audited signal path. With job control
disabled, it targets all live jobs belonging to that terminal, not a kernel foreground process group. The stored foreground
identifier satisfies the shell's terminal queries but does not virtualize `setpgid`, sessions, or stopped/continued jobs.
The [creation-mask tracker](creation-mask.md) lets the existing spawn path capture a known mask after Fish starts workers.

Interactive Fish also starts pthread workers. XNU retains the native pthread startup PC after cache detachment, and rejects
the simulator's repeat registration. The shim's [thread-start bridge](runtime-handoff.md#new-guest-pthreads) creates each
supported new pthread suspended through the real syscall, redirects its initial PC to simulator libpthread, and resumes it.
This preserves kernel thread/TSD initialization while avoiding entry into the unmapped native cache. It supports the pinned
custom-stack pthread ABI; kernel workqueue startup remains unsupported.

## Native display and input

`Host/TerminalView.m` provides software-keyboard input, hardware arrow/control keys, and an accessory row with Escape, Tab,
Ctrl-C, Ctrl-D, arrows, and Return. The display parses UTF-8, cursor movement, erasure, basic insert/delete operations, and
device/status queries, with bounded scrollback. Size changes update the shared window and ask the broker to send SIGWINCH.
Raw output is also mirrored to a connected `devicectl --console` stream. A usable pipe/TTY on the host app's stdin can feed
the same session, but `devicectl` did not forward stdin on the tested iPhone; keyboard input and the opt-in test FIFO work independently.
Console mirroring currently writes synchronously to stdout. A stalled attached console can therefore stall the host's main
queue even though socket I/O is bounded; ordinary on-screen use without an attached console is unaffected.

The renderer is a basic ANSI screen. It is not a complete VT/xterm implementation: colors and unsupported modes have no
visual effect, Unicode cell widths are incomplete, and full-screen terminal applications need separate work.

## Limits and validation

- `openpty`, `posix_openpt`, `/dev/ptmx`, and `/dev/tty` are not virtualized. Only the inherited socket endpoint is a fake terminal.
- Guest flush/drain controls, `TCSADRAIN`, `TCSAFLUSH`, flow control, breaks, and unsupported terminal ioctls return `ENOTSUP`.
- Canonical EOF on an empty line, software flow control, suspend characters, timed reads, and nondefault minimum-length reads
  are unsupported. In Fish's noncanonical mode Ctrl-D is an ordinary input byte that Fish can interpret itself.
- Full job control, stopped/continued wait events, generic `fork`, and process-image replacement remain unsupported.
  Pipelines, external command substitutions, background jobs, and arbitrary shell applications need separate execution tests.
- Input and signal delivery are separate operations. Text followed immediately by Ctrl-C in the same input burst can leave
  text already queued in the slave socket when SIGINT reaches Fish; Fish may display that text afterward. A separate Ctrl-C
  after the line appears clears it. Host queue flushing cannot remove bytes already queued in the socket; an acknowledged
  flush/control protocol or guest read interposition is not implemented.

`Tests/FakePTYTests.c` covers shared state, alias matching, native-descriptor forwarding, termios/window controls, line editing,
echo, output translation, signal characters, and explicit unsupported operations. Native session tests exercise inherited
endpoints, dispatch I/O, raw input, resize/signals, and EOF cleanup. Screen tests exercise split UTF-8/escape sequences,
cursor movement, editing, bounded state, and parser callbacks that reenter input handling. These are host tests and do not
replace the separate device checks below.

## Device checks and diagnostics

On 2026-10-25, the integrated terminal and worker shim ran on `iphone-0xf-26-cua-duy.local` (iOS 26.1, build `23B50`):

- Fish PID `79627` reached the prompt and accepted typed commands. `ls` was absent from the test runtime; typing `df` spawned
  PID `79629`, printed its output, and produced exit status 0.
- A fresh Fish launch, PID `79649`, accepted raw test input through the opt-in FIFO and printed `interactive-ok`.
- `df -h` spawned PID `79695`, printed its filesystem table, and returned `df-status=0` through Fish's wait path.
- Original-platform `bash-ios` spawned PID `79699`, exited 37, and returned `bash-status=37`. Its iOS platform command
  remained unchanged; see the [dyld compatibility results](ios-platform.md).
- Backspace and arrow editing produced `edit-ok`. A separate Ctrl-C cleared a rendered unfinished line. The same-burst
  text/Ctrl-C ordering limitation above was also reproduced.
- A Bash builtin loop, PID `79711`, terminated on Ctrl-C with raw wait status `0x2`; Fish reported `interrupt-status=130`
  and returned to its prompt. `echo substitution=(math 20 + 22)` then printed `substitution=42` using Fish builtins.
- Resizing the native view updated the terminal and submitted SIGWINCH. Ctrl-D at the final prompt exited Fish `79649`
  with raw wait status `0x0`. SSH inspection then confirmed removal of both the terminal state file and its input FIFO.

Hardware-only LLDB breakpoints caught `LSBThreadCreate` and then a new worker at simulator libpthread's `_pthread_start`,
address `0x12b26e618`. The host region `0x180000000–0x300000000` remained inaccessible (`---`); the simulator cache range
starting at `0x120000000` was `r-x` with zero dirty pages reported. LLDB named Fish, dyld_sim, the shim, and simulator pthread.
The simultaneous amfid/TXM capture, saved as `tmp/fake-pty/security-interactive.log`, completed with no matching events.

Evidence is in `tmp/fake-pty/console-key-tests.log` and `tmp/fake-pty/interactive-lldb.txt`. The final `Tests/run.sh` suite
and signed iPhone Xcode build passed. After the I/O fairness and parser reentrancy fixes, the final installed build also
passed a fresh launch: host PID `79724`, Fish PID `79725`, a new prompt, and `final-terminal-ok`. Original-platform Bash
PID `79728` exited with raw status `0x2500`, and Fish printed `final-status=37`. This final run is recorded in
`tmp/fake-pty/console-final.log`.

The first attempts exposed two startup failures before a prompt: one libpthread zero-token trap, and a later worker starting
at the old host pthread PC. The short and large startup-vector tests did not reproduce token corruption. Redirecting a
suspended worker under LLDB isolated the stale entry point; the integrated thread bridge then enabled the successful runs above.
The isolated zero-token report remains a separate unresolved observation.

Two diagnostic environment switches are available and normally off:

- `LSB_DEBUG_STOP_BEFORE_HANDOFF=1` makes LiveProcess send itself `SIGSTOP` after reporting READY and building its launch
  arguments, before entering the simulator loader. This permits attaching LLDB while native code is still available. The
  setting is removed from the guest environment, so guest-spawned children do not inherit the stop request.
- `LSB_TERMINAL_INPUT_FIFO=1` makes the native host create a mode-`0600` FIFO at `<terminal-state-path>.input` and print its
  exact path as `[terminal] input FIFO=...`. Writing raw bytes to it feeds the same line-discipline/input path as the keyboard.
  It avoids relying on console stdin forwarding during device tests. The host removes the FIFO when the terminal closes.

The FIFO and stop switch are test controls; neither is required for ordinary on-screen use. Runtime `DYLD_PRINT_*`
diagnostics remain off unless explicitly supplied.
