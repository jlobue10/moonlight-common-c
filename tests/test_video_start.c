// startVideoStream against a failing UDP bind: the failure must be reported as a
// failure even when the platform error code reads zero (LastSocketFail), with the
// decoder cleaned up exactly once, so LiStartConnection never records the stage as
// started and tears down threads that were never created.
#include "../src/Limelight-internal.h"
static SOCKET testBind(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int qosType);
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect);
#define bindUdpSocket testBind
#define recvUdpSocket testRecv
#include "../src/VideoStream.c"
#undef bindUdpSocket
#undef recvUdpSocket
#include <stdio.h>

// The receive thread is driven by a script of datagram sizes: 0 = poll timeout, -1 ends it.
static int script[256], scriptLen, scriptPos, terminations, lastTermination;
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect) {
    (void)s; (void)useSelect;
    int n = scriptPos < scriptLen ? script[scriptPos++] : -1;
    if (n > 0) memset(buffer, 0, n < size ? n : size);
    return n;
}
static void stubTerminated(int code) { ++terminations; lastTermination = code; }

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

    // C27-02: a runt before the host's first packet must not disarm the no-traffic timeout.
    // The timeout counts 100 ms polls, so a hundred zero returns model ten seconds of silence.
    ListenerCallbacks.connectionTerminated = stubTerminated;
    EncryptionFeaturesEnabled = 0;
    check(initializeVideoStream() == 0, "video stream initialized for the receive-thread cases");
    scriptLen = 0;
    script[scriptLen++] = 3;
    for (int i = 0; i < 100; i++) script[scriptLen++] = 0;
    scriptPos = terminations = 0;
    VideoReceiveThreadProc(NULL);
    check(terminations == 1 && lastTermination == ML_ERROR_NO_VIDEO_TRAFFIC,
          "a runt datagram before the first packet does not disarm the no-traffic timeout");
    destroyVideoStream();

    check(initializeVideoStream() == 0, "video stream re-initialized");
    scriptLen = 0;
    script[scriptLen++] = (int)sizeof(RTP_PACKET);
    for (int i = 0; i < 100; i++) script[scriptLen++] = 0;
    scriptPos = terminations = 0;
    VideoReceiveThreadProc(NULL);
    check(terminations == 1 && lastTermination != ML_ERROR_NO_VIDEO_TRAFFIC,
          "a full RTP header still counts as the host's first packet");
    destroyVideoStream();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
