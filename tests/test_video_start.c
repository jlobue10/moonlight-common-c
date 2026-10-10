// startVideoStream against a failing UDP bind: the failure must be reported as a
// failure even when the platform error code reads zero (LastSocketFail), with the
// decoder cleaned up exactly once, so LiStartConnection never records the stage as
// started and tears down threads that were never created.
#include "../src/Limelight-internal.h"
static SOCKET testBind(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int qosType);
#define bindUdpSocket testBind
#include "../src/VideoStream.c"
#undef bindUdpSocket
#include <stdio.h>

static int binds, setups, starts, cleanups, checks, failures;
static SOCKET testBind(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int qosType) {
    (void)addressFamily; (void)localAddr; (void)addrLen; (void)bufferSize; (void)qosType;
    ++binds;
    SetLastSocketError(0);
    return INVALID_SOCKET;
}
static int stubSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    (void)videoFormat; (void)width; (void)height; (void)redrawRate; (void)context; (void)drFlags;
    ++setups;
    return 0;
}
static void stubStart(void) { ++starts; }
static void stubCleanup(void) { ++cleanups; }
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

int main(void) {
    memset(&VideoCallbacks, 0, sizeof(VideoCallbacks));
    VideoCallbacks.setup = stubSetup;
    VideoCallbacks.start = stubStart;
    VideoCallbacks.cleanup = stubCleanup;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 60;
    StreamConfig.packetSize = 1024;
    RemoteAddr.ss_family = AF_INET;

    int err = startVideoStream(NULL, 0);
    check(binds == 1 && setups == 1, "the decoder is set up and the RTP socket bound");
    check(err != 0, "a failed bind with a zero platform error is still reported as a failure");
    check(cleanups == 1 && starts == 0, "the decoder is cleaned up once and never started");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
