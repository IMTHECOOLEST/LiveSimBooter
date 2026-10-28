# Building without a Mac

[`.github/workflows/build.yml`](../.github/workflows/build.yml) compiles the project on GitHub-hosted Apple Silicon runners.
The native host uses `xcode-27` because the Xcode project format requires Xcode 27; guest tools and the informational
local suite use `macos-26`. It runs on pushes to `main`, on pull requests, and on demand from **Actions → Build → Run workflow**.

This is a compile and packaging check with downloadable binaries. It is not a replacement for the full
[setup guide](agent-setup.md) on an Apple Silicon Mac, and it does not produce a runnable phone app.

## What it builds

| Job | Output | Notes |
| --- | --- | --- |
| `Native host (iPhoneOS arm64)` | `LiveSimBooter-unsigned.ipa`, `LiveSimBooter.app.zip`, `BUILD-INFO.txt` | `xcodebuild -scheme LiveSimBooter -configuration Release -destination 'generic/platform=iOS'` with `CODE_SIGNING_ALLOWED=NO`. The embedded `LiveProcess.appex` is checked for presence, architecture and bundle identifier. |
| `Guest tools (arm64 iOS Simulator)` | `libLiveSimSpawn.dylib`, `spawn-probe`, `terminal-fish`, `display-settings`, `launchctl-control`, `app-registration`, the Core Services probes and `metal-iosurface-probe` | Built from the tracked `Tools/build-*.sh` scripts, unmodified. |
| `Local suite (informational)` | `tests-log` artifact | `sh Tests/run.sh`. Marked `continue-on-error` because some tests need a normal local process session; read the log instead of treating a red job as a build failure. |

`LiveSimBooter.xcodeproj/xcshareddata/xcschemes/LiveSimBooter.xcscheme` is tracked so `xcodebuild -scheme` works on a
fresh checkout; Xcode previously created that scheme only in its own user state.

## What it cannot build

These stages need inputs that are not in Git and are supplied per setup, so they stay on a Mac with the original
Apple runtime:

- Shared-cache build, patch and signing (`Tools/setup-ios26-cache.py`, `patch-ios26-*.py`, `sign-dyld-cache.py`) —
  needs the pinned iOS Simulator 26.0 build 23A343 runtime and a keychain signing identity.
- Runtime staging (`Tools/setup-ios26-runtime.py`) and the device-type profiles — need the same original runtime,
  about 13 GB of payload, and identities.
- The native Metal backend (`Tools/prepare-native-metal.py`) — needs the original `MTLSimImplementation` and
  `MetalSerializer` images from that runtime, and its pinned UUID/`__cstring` checks reject anything else.
- Fish — needs the external Fish 4.9.3 source and the pinned Rust toolchain.
- Code signing, provisioning profiles, installation, and every device verification step.

## Using the unsigned IPA

An unsigned `.ipa` can only be re-signed with your own identity and provisioning profile. A self-signed or free-account
build does not include `com.apple.developer.kernel.increased-memory-limit` or the `group.com.kdt.LiveSimBooter` App
Group, both of which the host and extension need to be provisioned for; installing on a phone and expecting the
simulator to boot is not a supported outcome. Treat the artifact as a build result and, if you need a device install,
follow [the setup guide](agent-setup.md) with your own team, identifiers and profiles.

## Reproducing the two build commands locally

On an Apple Silicon Mac with Xcode, these are the two commands CI runs for the compiled artifacts:

```sh
xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath tmp/ci/native-build \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO ARCHS=arm64 ONLY_ACTIVE_ARCH=NO build

sh Tools/build-spawn-shim.sh tmp/ci/guest/libLiveSimSpawn.dylib tmp/ci/guest/spawn-probe
sh Tools/build-terminal-fish.sh tmp/ci/guest
sh Tools/build-display-settings.sh tmp/ci/guest
sh Tools/build-launchctl-control.sh tmp/ci/guest
sh Tools/build-app-registration.sh tmp/ci/guest
```
