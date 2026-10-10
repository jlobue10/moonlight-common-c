#!/bin/sh
# Linux/Unix host tests of production parsing and negotiation code.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?Usage: tests/run-regressions.sh path/to/cmake-build}
build=$(CDPATH= cd -- "$build" && pwd)
cc=${CC:-cc}
# Match sanitizer flags used to build the library via CFLAGS, when applicable.
for test in steam_haptic hdr_state; do
    "$cc" ${CFLAGS:-} -ffunction-sections -fdata-sections -DHAS_SOCKLEN_T \
        -I"$repo/src" -I"$repo/enet/include" "$repo/tests/test_$test.c" \
        -L"$build" -lmoonlight-common-c -Wl,--gc-sections -Wl,-rpath,"$build" \
        -pthread -o "$build/test-$test"
done
"$cc" ${CFLAGS:-} "$repo/tests/test_video_format.c" -o "$build/test-video-format"
"$build/test-steam_haptic"
"$build/test-hdr_state"
"$build/test-video-format"
