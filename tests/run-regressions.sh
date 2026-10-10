#!/bin/sh
# Linux/Unix host tests of production parsing and negotiation code.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?Usage: tests/run-regressions.sh path/to/cmake-build}
build=$(CDPATH= cd -- "$build" && pwd)
cc=${CC:-cc}
# Match sanitizer flags used to build the library via CFLAGS, when applicable.
for test in steam_haptic hdr_state control_truncation video_drain rtp_admission rtp_staging socket_subnet input_motion audio_queue; do
    extra_flags=
    # Exercise ownership and oversized-FEC admission assertions in debug builds.
    if [ "$test" = video_drain ] || [ "$test" = rtp_admission ] || [ "$test" = rtp_staging ]; then extra_flags=-DLC_DEBUG; fi
    "$cc" ${CFLAGS:-} $extra_flags -ffunction-sections -fdata-sections -DHAS_SOCKLEN_T \
        -I"$repo/src" -I"$repo/enet/include" -I"$repo/nanors" -I"$repo/nanors/deps" -I"$repo/nanors/deps/obl" \
        "$repo/tests/test_$test.c" \
        -L"$build" -lmoonlight-common-c -Wl,--gc-sections -Wl,-rpath,"$build" \
        -pthread -o "$build/test-$test"
done
"$cc" ${CFLAGS:-} "$repo/tests/test_video_format.c" -o "$build/test-video-format"
"$build/test-steam_haptic"
"$build/test-hdr_state"
"$build/test-control_truncation"
"$build/test-video_drain"
"$build/test-video_drain" flush
"$build/test-video_drain" av1
"$build/test-rtp_admission"
"$build/test-rtp_staging"
"$build/test-socket_subnet"
"$build/test-input_motion"
"$build/test-audio_queue"
"$build/test-video-format"
python3 "$repo/tests/test_server_commands.py"
