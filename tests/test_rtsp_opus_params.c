// Host-supplied RTSP state that used to reach debug asserts: the surround-params Opus
// layout (streams == 0 aborted startAudioStream; a non-digit aborted the DESCRIBE parser)
// and the ENet transport selected together with encrypted RTSP (the ENet path asserted).
// Built with -DLC_DEBUG: the production file is included so its static helpers are visible.
#include "../src/Limelight-internal.h"
#include "../src/RtspConnection.c"
#include <stdio.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

int main(void) {
    OPUS_MULTISTREAM_CONFIGURATION config;

    memset(&config, 0, sizeof(config));
    check(parseOpusConfigFromParamString("42012345", 6, &config) == 0 && config.streams == 4 && config.coupledStreams == 2 &&
          config.mapping[5] == 5, "a well-formed 5.1 layout parses");
    check(parseOpusConfigFromParamString("00012345", 6, &config) != 0, "zero streams is refused instead of asserting later");
    check(parseOpusConfigFromParamString("45012345", 6, &config) != 0, "more coupled streams than streams is refused");
    check(parseOpusConfigFromParamString("x2012345", 6, &config) != 0, "a non-digit stream count is refused without asserting");
    check(parseOpusConfigFromParamString("4201234", 8, &config) != 0, "a short mapping is refused");
    check(parseOpusConfigFromParamString("42012345", 9, &config) != 0, "a channel count above the maximum is refused");

    check(rtspOverEnet((int[]){7, 1, 350, 0}, false), "a GFE 7.1.350 host without encryption uses ENet RTSP");
    check(!rtspOverEnet((int[]){7, 1, 350, 0}, true), "encrypted RTSP never rides on ENet, whatever the version says");
    check(!rtspOverEnet((int[]){7, 1, 431, 0}, false), "a modern host uses TCP");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
