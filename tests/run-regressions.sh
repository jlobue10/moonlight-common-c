#!/bin/sh
# Linux/Unix host tests of production parsing and negotiation code.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?Usage: tests/run-regressions.sh path/to/cmake-build}
build=$(CDPATH= cd -- "$build" && pwd)
cc=${CC:-cc}
# Match sanitizer flags used to build the library via CFLAGS, when applicable.
for test in steam_haptic steam_settings hdr_state control_truncation video_drain rtp_admission rtp_staging socket_subnet input_motion motion_admission audio_queue rtsp_parser input_utf8 controller_batching pen_batching mouse_batching video_start video_start_unwind depacketizer depacketizer_flags platform_init sdp_bitrate rtsp_opus_params stun_padding audio_start rtsp_transact connection_start port_string; do
    extra_flags=
    # Exercise ownership and oversized-FEC admission assertions in debug builds.
    # (test_rtp_staging.c builds blocks by hand and #undefs LC_DEBUG itself before
    # including the queue, so it is deliberately not on this list.)
    if [ "$test" = video_drain ] || [ "$test" = rtp_admission ] || [ "$test" = mouse_batching ] || [ "$test" = controller_batching ] || [ "$test" = pen_batching ] || [ "$test" = motion_admission ] || [ "$test" = video_start_unwind ] || [ "$test" = sdp_bitrate ] || [ "$test" = depacketizer_flags ] || [ "$test" = rtsp_opus_params ]; then extra_flags=-DLC_DEBUG; fi
    # The audio queue's block invariants (validateFecBlockState, the placeholder accounting) only
    # exist in debug builds; LC_FUZZING keeps LC_ASSERT but leaves FEC_VALIDATION_MODE off so the
    # 4-shard completion checks still work.
    if [ "$test" = audio_queue ]; then extra_flags="-DLC_DEBUG -DLC_FUZZING"; fi
    "$cc" ${CFLAGS:-} $extra_flags -ffunction-sections -fdata-sections -DHAS_SOCKLEN_T \
        -I"$repo/src" -I"$repo/enet/include" -I"$repo/nanors" -I"$repo/nanors/deps" -I"$repo/nanors/deps/obl" \
        "$repo/tests/test_$test.c" \
        -L"$build" -lmoonlight-common-c -Wl,--gc-sections -Wl,-rpath,"$build" \
        -pthread -o "$build/test-$test"
done
"$cc" ${CFLAGS:-} "$repo/tests/test_video_format.c" -o "$build/test-video-format"
for kind in 1 2 3 4; do
    # LC_DEBUG: destroying an initialised-but-never-started stage used to assert (C19-02).
    "$cc" ${CFLAGS:-} -DLC_DEBUG -DINIT_KIND="$kind" -ffunction-sections -fdata-sections -DHAS_SOCKLEN_T \
        -I"$repo/src" -I"$repo/enet/include" -I"$repo/nanors" -I"$repo/nanors/deps" -I"$repo/nanors/deps/obl" \
        "$repo/tests/test_stream_init.c" \
        -L"$build" -lmoonlight-common-c -Wl,--gc-sections -Wl,-rpath,"$build" \
        -pthread -o "$build/test-stream-init-$kind"
    "$build/test-stream-init-$kind"
done
"$build/test-steam_haptic"
"$build/test-steam_settings"
"$build/test-hdr_state"
"$build/test-control_truncation"
"$build/test-video_drain"
"$build/test-video_drain" flush
"$build/test-video_drain" av1
"$build/test-rtp_admission"
"$build/test-rtp_staging"
"$build/test-socket_subnet"
"$build/test-port_string" null-buffer
"$build/test-input_motion"
"$build/test-motion_admission"
"$build/test-audio_queue"
"$build/test-rtsp_parser"
"$build/test-input_utf8"
"$build/test-controller_batching"
"$build/test-pen_batching"
"$build/test-mouse_batching"
"$build/test-video_start"
"$build/test-video_start_unwind"
"$build/test-sdp_bitrate"
"$build/test-depacketizer_flags"
"$build/test-rtsp_opus_params"
"$build/test-stun_padding"
"$build/test-audio_start"
# These two include RtspConnection.c / Connection.c, whose globals the shared library also
# defines; ASan reports the deliberate shadowing as an ODR violation, so that check is off here.
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_odr_violation=0" "$build/test-rtsp_transact"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_odr_violation=0" "$build/test-connection_start"
"$build/test-depacketizer"
"$build/test-platform_init"
"$build/test-video-format"
python3 -I "$repo/tests/test_server_commands.py"
