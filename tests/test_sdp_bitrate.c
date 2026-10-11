// The advertised x-ml-video.configuredBitrateKbps must stay within the bound Vibepollo
// enforces (800000 kbps; above it the ANNOUNCE is rejected with no fallback), while the
// local StreamConfig keeps the real budget (C19-01).
#include "../src/Limelight-internal.h"
#include "../src/SdpGenerator.c"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char* name) {
    ++checks;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static const char* attribute(const char* sdp, const char* name) {
    const char* at = strstr(sdp, name);
    return at ? at + strlen(name) : NULL;
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));  // Sunshine-class host
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    RemoteAddr.ss_family = AF_INET;
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.width = 3840;
    StreamConfig.height = 2160;
    StreamConfig.fps = 120;
    StreamConfig.packetSize = 1024;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    StreamConfig.colorSpace = COLORSPACE_REC_709;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    AudioPacketDuration = 5;
    VideoPortNumber = 47998;  // fillSdpTail asserts a bound video port in debug builds

    int length = 0;
    StreamConfig.bitrate = 2000000;
    char* sdp = getSdpPayloadForStreamConfig(0, &length);
    check(sdp != NULL && length > 0, "the SDP payload is generated");
    const char* advertised = sdp ? attribute(sdp, "x-ml-video.configuredBitrateKbps:") : NULL;
    check(advertised != NULL && strncmp(advertised, "800000", 6) == 0 && advertised[6] < '0',
          "a 2 Gbps PyroWave budget advertises the 800000 kbps host bound");
    check(sdp != NULL && strstr(sdp, "2000000") == NULL, "the unclamped value is not advertised anywhere");
    check(StreamConfig.bitrate == 2000000, "the local stream budget is untouched");
    free(sdp);

    StreamConfig.bitrate = 150000;
    sdp = getSdpPayloadForStreamConfig(0, &length);
    advertised = sdp ? attribute(sdp, "x-ml-video.configuredBitrateKbps:") : NULL;
    check(advertised != NULL && strncmp(advertised, "150000", 6) == 0 && advertised[6] < '0',
          "a budget below the bound is advertised unchanged");
    free(sdp);

    AppVersionQuad[3] = 0;  // GFE-class host: the attribute is not emitted at all
    sdp = getSdpPayloadForStreamConfig(0, &length);
    check(sdp != NULL && strstr(sdp, "configuredBitrateKbps") == NULL, "a non-Sunshine host gets no configured-bitrate attribute");
    free(sdp);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
