// startVideoStream against a failing UDP bind: the failure must be reported as a
// failure even when the platform error code reads zero (LastSocketFail), with the
// decoder cleaned up exactly once, so LiStartConnection never records the stage as
// started and tears down threads that were never created.
#define PltGetMillis testGetMillis
#include "../src/Limelight-internal.h"
static SOCKET testBind(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int qosType);
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect);
#define bindUdpSocket testBind
#define recvUdpSocket testRecv
#include "../src/VideoStream.c"
#undef bindUdpSocket
#undef recvUdpSocket
#undef PltGetMillis
#include <stdio.h>

// The receive thread is driven by a script of datagram sizes: 0 = poll timeout, -1 ends it.
static int script[256], scriptLen, scriptPos, terminations, lastTermination;
static uint64_t clockMs = 1000;
uint64_t testGetMillis(void) { return clockMs; }
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect) {
    (void)s; (void)useSelect;
    // Each scripted receive happens 100 ms later, including successful garbage
    // reads. Wall time must advance even if recv() never reports a timeout.
    clockMs += UDP_RECV_POLL_TIMEOUT_MS;
    int n = scriptPos < scriptLen ? script[scriptPos++] : -1;
    if (n > 0) {
        // A zeroed RTP packet with the extension flag (asserted on in debug builds)
        // and frame index 0, which the queue rejects as behind frame 1.
        memset(buffer, 0, n < size ? n : size);
        buffer[0] = FLAG_EXTENSION;
    }
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
    // A hundred receives model ten seconds on the monotonic clock.
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
    script[scriptLen++] = (int)(sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET));
    for (int i = 0; i < 100; i++) script[scriptLen++] = 0;
    scriptPos = terminations = 0;
    VideoReceiveThreadProc(NULL);
    check(terminations == 1 && lastTermination == ML_ERROR_NO_VIDEO_FRAME,
          "an incomplete first frame followed by silence expires the first-frame deadline");
    destroyVideoStream();

    for (int first = 0; first <= 1; first++) {
        check(initializeVideoStream() == 0, "video initialized for continuous-runt deadline case");
        scriptLen = 0;
        if (first) script[scriptLen++] = (int)(sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET));
        for (int i = 0; i < 110; i++) script[scriptLen++] = 3;
        scriptPos = terminations = 0;
        VideoReceiveThreadProc(NULL);
        check(terminations == 1 && lastTermination == (first ? ML_ERROR_NO_VIDEO_FRAME : ML_ERROR_NO_VIDEO_TRAFFIC),
              "continuous runt datagrams cannot extend either video startup deadline");
        destroyVideoStream();
    }
    EncryptionFeaturesEnabled = SS_ENC_VIDEO;
    check(initializeVideoStream() == 0, "video initialized for failed-authentication deadline case");
    scriptLen = 0;
    for (int i = 0; i < 110; i++) script[scriptLen++] = (int)(sizeof(ENC_VIDEO_HEADER) + sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET));
    scriptPos = terminations = 0;
    VideoReceiveThreadProc(NULL);
    check(terminations == 1 && lastTermination == ML_ERROR_NO_VIDEO_TRAFFIC,
          "continuous unauthenticated encrypted packets cannot extend the no-traffic deadline");
    destroyVideoStream();

    EncryptionFeaturesEnabled = 0;
    check(initializeVideoStream() == 0, "video initialized for completed-frame control case");
    receivedDataFromPeer = true;
    firstDataTimeMs = clockMs;
    notifyKeyFrameReceived();
    scriptLen = 0;
    for (int i = 0; i < 110; i++) script[scriptLen++] = 0;
    scriptPos = terminations = 0;
    VideoReceiveThreadProc(NULL);
    check(scriptPos == scriptLen && lastTermination != ML_ERROR_NO_VIDEO_FRAME && lastTermination != ML_ERROR_NO_VIDEO_TRAFFIC,
          "a successfully received frame retires the startup deadline");
    destroyVideoStream();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
