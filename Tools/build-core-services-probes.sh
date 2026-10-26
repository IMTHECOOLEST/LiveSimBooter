#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/core-services-probes}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Guest/CoreServicesProbe.c -o "$output/core-services-probe"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Guest/DistributedNotificationProbe.c -framework CoreFoundation -o "$output/distributed-notification-probe"
printf '%s\n' "$output/core-services-probe" "$output/distributed-notification-probe"
