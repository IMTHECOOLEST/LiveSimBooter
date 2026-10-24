#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_directory=$(mktemp -d /tmp/LiveSimBooter-tests.XXXXXX)
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM
for test_name in DyldSimLoader SharedCacheHandoff MainThreadBootstrap LocalMach LocalRuntime; do
    xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
        "Tests/${test_name}Tests.cpp" -o "$test_directory/$test_name"
    "$test_directory/$test_name"
done
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
    LiveSimBooter/LocalPOSIX.cpp LiveSimBooter/LocalMach.cpp -o "$test_directory/SimSyscallHelpers"
"$test_directory/SimSyscallHelpers"
