#!/usr/bin/env python3
"""Reject imports outside the app-local callback/handoff code; never execute or detach anything."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ("LocalRuntime", "LocalPOSIX", "LocalMach", "SimSyscallHelpers", "MainThreadBootstrap", "CompactImageInfo", "DebuggerBridge", "SharedCacheHandoff")


def output(*args):
    return subprocess.check_output(args, text=True).splitlines()


def audit(objects):
    defined = set()
    undefined = set()
    for obj in objects:
        defined.update(output("xcrun", "nm", "-gUj", str(obj)))
        undefined.update(output("xcrun", "nm", "-uj", str(obj)))
    external = undefined - defined
    if external:
        raise SystemExit("Post-detachment code imports external symbols:\n" + "\n".join(sorted(external)))


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 2:
        # Check the actual Xcode build, including the flags selected by the project.
        objects = [pathlib.Path(sys.argv[1]) / (name + ".o") for name in SOURCES]
        audit(objects)
        print("Xcode callback/handoff objects: no external imports")
    else:
        sdk = output("xcrun", "--sdk", "iphoneos", "--show-sdk-path")[0]
        with tempfile.TemporaryDirectory(prefix="LiveSimBooter-audit-") as temporary:
            for optimization in ("-O0", "-O2"):
                objects = []
                for name in SOURCES:
                    obj = pathlib.Path(temporary) / (name + ".o")
                    subprocess.run([
                        "xcrun", "--sdk", "iphoneos", "clang++", "-target", "arm64-apple-ios15.0", "-isysroot", sdk,
                        "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fno-builtin", "-fno-stack-protector",
                        "-fno-exceptions", "-fno-rtti", optimization, "-c", str(ROOT / "LiveSimBooter" / (name + ".cpp")),
                        "-o", str(obj),
                    ], check=True)
                    objects.append(obj)
                audit(objects)
                print(f"iPhone ARM64 callback/handoff objects {optimization}: no external imports")
