#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/multiprocess/libLiveSimSpawn.dylib}
probe=${2:-tmp/multiprocess/spawn-probe}
mkdir -p "$(dirname "$output")" "$(dirname "$probe")"
trace_flags=
if [ "${LSB_TRANSPORT_TRACE:-0}" = 1 ]; then trace_flags=-DLSB_TRANSPORT_TRACE; fi
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    -dynamiclib -fvisibility=hidden -DLSB_GUEST_RUNTIME -DLSB_XPC_WIRE_COMPATIBILITY $trace_flags \
    -install_name /usr/lib/libLiveSimSpawn.dylib -Wl,-not_for_dyld_shared_cache \
    Guest/SpawnShim.c Guest/CreationMask.c Guest/ExecutableAccess.c Guest/ChildWait.c Guest/ChildKill.c Guest/FakePTY.c Guest/ThreadBootstrap.c \
    Guest/XPCWireCompatibility.c \
    ProcessSupport/ChildStatus.c ProcessSupport/FakePTY.c \
    ProcessSupport/SpawnPlan.c ProcessSupport/SpawnTransport.c -o "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Guest/SpawnProbe.c -o "$probe"
printf '%s\n' "$output" "$probe"
