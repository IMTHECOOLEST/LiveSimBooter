# Keyboard haptics on the iOS 27 host

The iOS Simulator 26.0/23A343 CoreHaptics image (`2AFD3C06-3A36-35CE-8940-B381C1AFFC0B`) crashed when Contacts
opened its keyboard on iPhone18,2 running iOS 27.0/24A435. The guest reads the native system keyboard AHAP files under
`/Library/Audio/Tunings/Generic/Haptics/Patterns`. Their relative `AudioResources/Key*.caf` references are valid, but stock
`+[CHHapticPattern patternForKey:error:]` parses the AHAP into a dictionary and loses the source directory. The dictionary
initializer returns error -4830; the stock caller then stores the pattern identifier through nil + 0x18.

Controls isolated path resolution from AHAP version or missing resources: all eight original keyboard dictionaries failed,
while both absolute waveform paths and `initWithContentsOfURL:error:` succeeded for the same files. No audio events need
to be removed and no global working-directory change is needed.

[patch-ios26-keyboard-haptics.py](../Tools/patch-ios26-keyboard-haptics.py) changes the pinned system-pattern loader to pass
its existing path through `NSURL` and the public URL initializer. Existing ownership, cleanup, NSError handling and pattern
identifier assignment are preserved; a nil guard handles other initializer failures. App dictionary initializers are untouched.
The tool validates the original image UUID, section extents, instructions, selector/class references, branch targets and
mapping protections before producing a separate output. It refuses unexpected, partial or already patched inputs. Its current
layout profile is the tracked 1,353-image cache at `0x110000000`, reservation size `0x7e194000`.

Apply this after the container-metadata patch and before signing:

```sh
python3 Tools/patch-ios26-keyboard-haptics.py --source "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64" \
  --output "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" > "$LSB_WORK/cache-haptics-patch.json"
python3 Tools/sign-dyld-cache.py "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" \
  "$LSB_WORK/signed-cache/dyld_sim_shared_cache_arm64" \
  --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --team "$LSB_TEAM"
```

The device test changed 29 bytes across seven instruction spans, signed the new cache using the same app identity and
identifier, and published it with affected guests stopped and a rollback retained. Post-copy checksums were skipped as
requested. Cache UUID remained `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; the new CodeDirectory hash is
`ed8d1b7d245abdacdd12a81e97a5ca7b8a299352`. Host11690 / launchd11694 retained the native cache and used hardware
workqueue redirection without LLDB. All eight stock `patternForKey:error:` initializations succeeded with nil NSError.
The user then confirmed the simulator keyboard works. This verifies pattern construction and UI input, not native haptic
actuator playback.

Sixteen focused patcher tests cover valid patching, immutable inputs, malformed layouts, unexpected instructions and
references, branch safety, duplicate patches and output collisions. Local evidence is under ignored
`tmp/lsd-metadata-default/`: `haptic-probe/path-analysis.md`, `results/haptic-probe-v2.log`, `results/haptic-probe-v3.log`,
and the patch/signing manifests. These local files are evidence, not setup dependencies.
