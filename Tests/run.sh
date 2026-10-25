#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_directory=$(mktemp -d /tmp/LiveSimBooter-tests.XXXXXX)
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM
for test_name in DyldSimLoader SharedCacheHandoff MainThreadBootstrap DebuggerBridge LocalMach LocalRuntime; do
    xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
        "Tests/${test_name}Tests.cpp" -o "$test_directory/$test_name"
    "$test_directory/$test_name"
done
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/CompactImageInfoTests.cpp -framework CoreFoundation -o "$test_directory/CompactImageInfo"
"$test_directory/CompactImageInfo"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/LocalPOSIXTests.cpp LiveSimBooter/LocalPOSIX.cpp -o "$test_directory/LocalPOSIX"
"$test_directory/LocalPOSIX"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    -DLSB_LOCAL_RUNTIME_RAW_TESTING Tests/LocalRuntimeTests.cpp -o "$test_directory/LocalRuntimeRaw"
"$test_directory/LocalRuntimeRaw"
for family in Mach POSIX; do
    xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
        "Tests/Local${family}LiveTests.cpp" "LiveSimBooter/Local${family}.cpp" LiveSimBooter/LocalRuntime.cpp \
        -o "$test_directory/Local${family}Live"
    "$test_directory/Local${family}Live"
done
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/SimSyscallHelpersTests.cpp LiveSimBooter/SimSyscallHelpers.cpp LiveSimBooter/LocalRuntime.cpp \
    LiveSimBooter/LocalPOSIX.cpp LiveSimBooter/LocalMach.cpp LiveSimBooter/CompactImageInfo.cpp -o "$test_directory/SimSyscallHelpers"
"$test_directory/SimSyscallHelpers"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    -DLSB_DEBUGGER_NOTIFICATION_TESTING Tests/DebuggerNotificationTests.cpp LiveSimBooter/SimSyscallHelpers.cpp LiveSimBooter/LocalRuntime.cpp \
    LiveSimBooter/LocalPOSIX.cpp LiveSimBooter/LocalMach.cpp -o "$test_directory/DebuggerNotification"
"$test_directory/DebuggerNotification"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpawnPlanTests.c ProcessSupport/SpawnPlan.c -o "$test_directory/SpawnPlan"
"$test_directory/SpawnPlan"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ChildMonitorTests.c Host/ChildMonitor.c -o "$test_directory/ChildMonitor"
"$test_directory/ChildMonitor"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ChildStatusTests.c ProcessSupport/ChildStatus.c -o "$test_directory/ChildStatus"
"$test_directory/ChildStatus"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_CHILD_WAIT_TESTING \
    Tests/ChildWaitTests.c Guest/ChildWait.c ProcessSupport/ChildStatus.c -o "$test_directory/ChildWait"
"$test_directory/ChildWait"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_CHILD_KILL_TESTING \
    Tests/ChildKillTests.c Guest/ChildKill.c -o "$test_directory/ChildKill"
"$test_directory/ChildKill"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/CreationMaskTests.c -o "$test_directory/CreationMask"
"$test_directory/CreationMask"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/SpawnTransport"
"$test_directory/SpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_GUEST_RUNTIME \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/GuestSpawnTransport"
XPC_FLAGS=0x2 "$test_directory/GuestSpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_XPC_WIRE_TESTING -dynamiclib \
    Guest/XPCWireCompatibility.c -install_name "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/libXPCWireCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    -DLSB_GUEST_RUNTIME -DLSB_XPC_WIRE_COMPATIBILITY -DLSB_XPC_WIRE_TESTING \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c \
    "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/WireSpawnTransport"
DYLD_INSERT_LIBRARIES="$test_directory/libXPCWireCompatibility.dylib" XPC_FLAGS=0x2 "$test_directory/WireSpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_XPC_WIRE_TESTING \
    Tests/XPCWireCompatibilityTests.c "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/XPCWireCompatibility"
DYLD_INSERT_LIBRARIES="$test_directory/libXPCWireCompatibility.dylib" XPC_FLAGS=0x2 "$test_directory/XPCWireCompatibility"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -IProcessSupport -IHost \
    Tests/BrokerTests.m ProcessSupport/SpawnPlan.c ProcessSupport/ChildStatus.c -framework Foundation -o "$test_directory/Broker"
"$test_directory/Broker"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/ExtensionLauncherTests.m -framework Foundation -o "$test_directory/ExtensionLauncher"
"$test_directory/ExtensionLauncher"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/RunningBoardMonitorTests.m -framework Foundation -o "$test_directory/RunningBoardMonitor"
"$test_directory/RunningBoardMonitor"
