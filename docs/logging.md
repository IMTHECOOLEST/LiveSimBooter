# Guest logging and retry loops

The guest shim disables libtrace's Mach logging ports while the simulator logging services are unavailable. It follows
[Sim26Booter's 2025-09-11 workaround](https://github.com/khanhduytran0/Sim26Booter/blob/main/PATCHING.md#2025-09-11),
implemented here at the entitlement query instead of patching libtrace instructions.

[LoggingCompatibility.c](../Guest/LoggingCompatibility.c) interposes the typed
`xpc_copy_entitlement_for_token(const char *, const audit_token_t *)` API. In guest builds, exactly
`com.apple.private.disable-log-mach-ports` with a **NULL token** returns an owned XPC true object. NULL is libtrace's
self-query convention. Every explicit token, including a token for the current process, and every other key or NULL-key
dictionary query tail-forwards unchanged. The override changes no signed entitlement, broker entitlement snapshot,
native host policy, or service authorization. It requires no environment variable.

The matching iOS 26 libtrace self-check calls this API at image offset `0x7330` and caches the result. A true result makes
`_os_trace_diagnosticd_pipe` return before trying `xpc_pipe_create("com.apple.diagnosticd", 0)`. Without it, an unavailable
service produces a NULL pipe that is retried by subsequent streaming log calls under an unfair lock. This workaround
suppresses that transport path; it does not provide a replacement diagnosticd, forward to the host, or establish log persistence.

The check can run before guest TLS and constructors. Its matching path reads no environment, errno, audit token, or path,
and uses no libc string function. The pinned libxpc Boolean factory is a leaf that returns an immutable singleton without
allocation or initialization; recheck this assumption when changing runtimes. See the
[early-init disassembly review](../tmp/cpu-investigation/logging-tests/early-init-review.md).

## Device results, 2026-10-26

The iOS 26 guest runtime was cold-started with the new shim on the iOS 26.1 phone: native host **11278**,
launchd **11282**, SpringBoard **11305**, and systemstatusd **11314**. Commands ran through launchd-owned Fish.
The [console transcript](../tmp/cpu-investigation/final-console.log) records these separate outcomes:

| Test | Result |
| --- | --- |
| Stock `/usr/bin/log show` and `log stream` | Exit 64: `Cannot run while sandboxed`. |
| Temporary `log-preflight-probe`, with only the CLI sandbox-check branch disabled | Help text renders; help invocation exits 64. |
| Diagnostic copy, `show --last 1m` with the marker predicate | Exit 65: `Could not open local log store: Connection to logd failed`. |
| Diagnostic copy, `stream --timeout 5` with the probe subsystem predicate | Prints the predicate/header and exits 0; no records delivered. |

The concurrent test started marker process **11391** with a two-second delay and stream process **11392**. The stream
exited before the marker emitted, despite the requested five-second timeout. The marker reported default/error/info/debug
enabled, emitted `LSB-LOG-LIVE-20261026`, and exited 0. That proves the probe reached the logging calls, not delivery or
retention. Guest log streaming and persistent retrieval remain unverified. The stock tool was retained; the branch patch
exists only in the temporary diagnostic copy under [log-stage](../tmp/cpu-investigation/log-stage/).

## CPU observations

The default graphical service list now includes stock `com.apple.systemstatusd`. Before it was loaded, SpringBoard's
SystemStatus publisher repeatedly reconnected, and the missing diagnosticd lookup amplified its logging work. Two
approximately 20.09-second windows around loading systemstatusd gave the following native CPU deltas, with one core = 100%:

| Process | Before systemstatusd | After systemstatusd |
| --- | ---: | ---: |
| launchd, native 11009 | 106.38% | 22.48% |
| SpringBoard, native 11156 | 102.16% | 2.72% |
| All stable app/extension processes | 242.24% (22 processes) | 27.35% (23 processes) |

Identities matched PID, executable UUID, and process start time across all five snapshots in each window. SpringBoard
and launchd retained their identities between windows. These are sequential observations with a ten-minute gap and
different display workloads, not a controlled benchmark. See the [comparison](../tmp/cpu-investigation/systemstatus-comparison.md).

After the subsequent cold boot with the logging override, another 20.09-second window measured launchd at 12.52%,
SpringBoard at 13.05%, and the 23 stable app/extension processes together at 43.99%. This changed workload cannot isolate
the override's CPU effect. The captured [launchd tail](../tmp/cpu-investigation/launchd-after-logging-tail.log) contains no
diagnosticd or SystemStatus publisher lookup failures; other missing services and Spotlight spawn retries remain.
Raw counters are in [after-logging-window.json](../tmp/cpu-investigation/after-logging-window.json).

Guest/non-guest unit tests and a real dynamic-interposition test cover exact matching, explicit tokens, passthrough
results, and errno. The full [test suite](../Tests/run.sh) passed with native framework access; its output is saved in
[final-suite.log](../tmp/cpu-investigation/final-suite.log). Evidence under `tmp/` is local and intentionally untracked.
