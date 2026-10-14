// The RTSP-over-TCP reply read loop must be bounded as a whole and interruptible: a host that
// trickles one byte per poll used to hold the connection thread for up to 1 MiB x 15 s and
// LiInterruptConnection() could not end it (C31-01). The production file is included so the
// static transact function is visible; sockets and the clock are scripted.
#define PltGetMillis testMillis
#include "../src/Limelight-internal.h"
static SOCKET testConnect(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec);
static int testPoll(struct pollfd* pollFds, int pollFdsCount, int timeoutMs);
static int testRecv(SOCKET s, char* buffer, int size, int flags);
static int testSend(SOCKET s, char* buffer, int size);
static void testNoDelay(SOCKET s);
static void testClose(SOCKET s);
#define connectTcpSocket testConnect
#define pollSockets testPoll
#define recv testRecv
#define sendMtuSafe testSend
#define enableNoDelay testNoDelay
#define closeSocket testClose
#include "../src/RtspConnection.c"
#undef connectTcpSocket
#undef pollSockets
#undef recv
#undef sendMtuSafe
#undef enableNoDelay
#undef closeSocket
#undef PltGetMillis
#include <stdio.h>

// Scripts: 0 = healthy reply then EOF, 1 = one byte per recv with 14 s between, 2 = one byte then interrupt,
// 6 = a DESCRIBE reply whose SDP carries a compatible PyroWave bitstream, 7 = the same SDP without the
// bitstream line (the host advertises SCM_PYROWAVE but speaks another revision).
static int mode, polls, recvs, closes, silentPolls, maxPollTimeoutMs;
static uint64_t clockMs = 1000;
static const char reply[] = "RTSP/1.0 200 OK" "\r\n" "CSeq: 1" "\r\n" "\r\n";
static const char describeCompatible[] =
    "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: application/sdp\r\n\r\n"
    "a=rtpmap:99 PYROWAVE/90000\r\na=x-ss-pyrowave.bitstream:" LI_PYROWAVE_BITSTREAM_ID "\r\n";
static const char describeIncompatible[] =
    "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: application/sdp\r\n\r\n"
    "a=rtpmap:99 PYROWAVE/90000\r\n";
uint64_t testMillis(void) { return clockMs; }
static SOCKET testConnect(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {
    (void)dstaddr; (void)addrlen; (void)port; (void)timeoutSec;
    return 7;
}
// Honours silent slices (modes 3-5) and records the largest timeout it was asked for, so the
// slice length and the err == 0 -> re-check path of the production loop are exercised.
static int testPoll(struct pollfd* pollFds, int pollFdsCount, int timeoutMs) {
    (void)pollFds; (void)pollFdsCount;
    ++polls;
    if (timeoutMs > maxPollTimeoutMs) {
        maxPollTimeoutMs = timeoutMs;
    }
    if (silentPolls > 0) {
        --silentPolls;
        clockMs += timeoutMs;  // the slice elapses with nothing to read
        if (mode == 4 && silentPolls == 0) {
            ConnectionInterrupted = true;
        }
        return 0;
    }
    return 1;
}
static int testRecv(SOCKET s, char* buffer, int size, int flags) {
    (void)s; (void)flags;
    ++recvs;
    if (recvs > 1000) {
        // The unbounded loop of the old code: stop it so the harness can report.
        SetLastSocketError(ECONNRESET);
        return -1;
    }
    if (mode == 0 || mode == 5 || mode == 6 || mode == 7) {
        if (recvs == 1) {
            const char* text = mode == 6 ? describeCompatible : mode == 7 ? describeIncompatible : reply;
            int n = (int)strlen(text);
            memcpy(buffer, text, n < size ? n : size);
            return n < size ? n : size;
        }
        return 0;
    }
    buffer[0] = 'R';
    if (mode == 1) {
        clockMs += 14000;
    }
    else {
        ConnectionInterrupted = true;
    }
    return 1;
}
static int testSend(SOCKET s, char* buffer, int size) { (void)s; (void)buffer; return size; }
static void testNoDelay(SOCKET s) { (void)s; }
static void testClose(SOCKET s) { (void)s; ++closes; }

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s" "\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

static bool transact(int scriptMode, int* error) {
    RTSP_MESSAGE request, response;
    bool ok;
    mode = scriptMode; polls = recvs = closes = 0; clockMs = 1000; ConnectionInterrupted = false;
    maxPollTimeoutMs = 0;
    silentPolls = scriptMode == 3 ? 1000 : scriptMode == 4 ? 3 : scriptMode == 5 ? 2 : 0;
    createRtspRequest(&request, NULL, 0, "OPTIONS", "rtsp://127.0.0.1", "RTSP/1.0", 1, NULL, NULL, 0);
    ok = transactRtspMessageTcp(&request, &response, error);
    freeMessage(&request);
    if (ok) {
        freeMessage(&response);
    }
    return ok;
}

// Drives the pre-OPTIONS prediction, a scripted DESCRIBE and the negotiation the way
// performRtspHandshake does, for a client that asked for PyroWave against a host that
// advertises SCM_PYROWAVE (C33-01, C33-03).
static bool describe(int scriptMode, int fallbackBitrate, int* format, int* error) {
    SERVER_INFORMATION serverInfo;
    RTSP_MESSAGE response;
    int expected;
    bool ok;
    memset(&serverInfo, 0, sizeof(serverInfo));
    serverInfo.rtspSessionUrl = "rtsp://10.0.0.5:48010";
    serverInfo.serverCodecModeSupport = SCM_H264 | SCM_PYROWAVE;
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_PYROWAVE;
    StreamConfig.bitrate = 300000;
    StreamConfig.pyroWaveFallbackBitrate = fallbackBitrate;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    memset(&AudioCallbacks, 0, sizeof(AudioCallbacks));
    useEnet = false;
    encryptedRtspEnabled = false;
    mode = scriptMode; polls = recvs = closes = 0; clockMs = 1000; ConnectionInterrupted = false;
    maxPollTimeoutMs = 0; silentPolls = 0;
    expected = conventionalBitrateAfterPyroWave(StreamConfig.supportedVideoFormats,
                                               selectPyroWaveFormat(StreamConfig.supportedVideoFormats, serverInfo.serverCodecModeSupport),
                                               StreamConfig.bitrate, StreamConfig.pyroWaveFallbackBitrate);
    selectRtspTargetUrl(&serverInfo, expected);
    if (strcmp(urlAddr, "10.0.0.5") != 0) {
        printf("prediction chose %s" "\n", urlAddr);
        return false;
    }
    ok = requestDescribe(&response, error);
    if (!ok) {
        return false;
    }
    *format = negotiateVideoFormat(&serverInfo, &response);
    freeMessage(&response);
    return true;
}

// Heap copy without the terminating NUL, as the ENet packet buffer is (ASan checks the bound).
static bool carriesPayload(const char* text) {
    size_t length = strlen(text);
    char* exact = malloc(length);
    bool result;
    memcpy(exact, text, length);
    result = enetReplyCarriesPayload(exact, (int)length);
    free(exact);
    return result;
}

int main(void) {
    int error = 0;
    int format = 0;
    AddrLen = sizeof(struct sockaddr_in);
    RtspPortNumber = 48010;

    check(transact(0, &error) && recvs == 2 && closes == 1, "a reply that arrives at once is parsed and the socket is closed");

    check(!transact(1, &error) && error == ETIMEDOUT && recvs <= 3,
          "a host trickling one byte every 14 s is cut off at the reply deadline, not per chunk");
    check(closes == 1, "the trickle socket is closed once");

    check(!transact(2, &error) && error == EINTR && recvs == 1,
          "LiInterruptConnection() ends the reply wait on the next iteration");
    check(closes == 1, "the interrupted socket is closed once");

    // C32-02: silent slices. A silent host times out at the whole-reply deadline after
    // ~150 slices of at most 100 ms; an interrupt during silence is seen within one slice;
    // a reply that arrives after two silent slices still succeeds.
    check(!transact(3, &error) && error == ETIMEDOUT && recvs == 0 && polls >= 150 && polls <= 152 && maxPollTimeoutMs == 100,
          "a silent host is cut off at the reply deadline through 100 ms slices");
    check(!transact(4, &error) && error == EINTR && recvs == 0 && polls == 3 && maxPollTimeoutMs <= 100,
          "an interrupt during silence ends the wait on the next slice");
    check(transact(5, &error) && recvs == 2 && polls == 4 && maxPollTimeoutMs <= 100,
          "a reply that arrives after silent slices is still parsed");

    // C33-03: the negotiation itself, with a scripted DESCRIBE. A compatible bitstream keeps
    // PyroWave and the budget; an incompatible one (advertised, wrong revision) falls back to
    // H.264 at the fallback bitrate and (C33-01) re-chooses normal-quality audio, which the
    // pre-DESCRIBE prediction could not know.
    check(describe(6, 8000, &format, &error) && format == VIDEO_FORMAT_PYROWAVE && StreamConfig.bitrate == 300000 &&
          strcmp(urlAddr, "10.0.0.5") == 0 && strcmp(rtspTargetUrl, "rtsp://10.0.0.5:48010") == 0,
          "a compatible PyroWave SDP keeps PyroWave, the budget and high-quality audio");
    check(describe(7, 8000, &format, &error) && format == VIDEO_FORMAT_H264 && StreamConfig.bitrate == 8000 &&
          strcmp(urlAddr, "0.0.0.0") == 0 && strcmp(rtspTargetUrl, "rtsp://0.0.0.0:48010") == 0,
          "an incompatible PyroWave SDP falls back to H.264 at the fallback bitrate and normal-quality audio");
    check(describe(7, 20000, &format, &error) && format == VIDEO_FORMAT_H264 && StreamConfig.bitrate == 20000 &&
          strcmp(urlAddr, "10.0.0.5") == 0,
          "a fallback bitrate above the threshold keeps high-quality audio");
    check(closes == 1, "the DESCRIBE socket is closed once");

    // Upstream 5ec8397 + C14-01: a repeated option on a request replaces the earlier node's
    // allocated fields and frees the duplicate node (LeakSanitizer bites on either leak).
    {
        RTSP_MESSAGE request;
        createRtspRequest(&request, NULL, 0, "OPTIONS", "rtsp://127.0.0.1", "RTSP/1.0", 1, NULL, NULL, 0);
        check(addOption(&request, "X-Test", "one") && addOption(&request, "X-Test", "two") &&
              request.options != NULL && request.options->next == NULL &&
              strcmp(request.options->option, "X-Test") == 0 && strcmp(request.options->content, "two") == 0,
              "a repeated request option keeps one node carrying the later content");
        freeMessage(&request);
    }

    // C33-02: over ENet the payload packet is awaited only behind a 200 reply.
    check(carriesPayload("RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n"), "a 200 reply header is followed by a payload packet");
    check(!carriesPayload("RTSP/1.0 400 Bad Request\r\nCSeq: 2\r\n\r\n"), "an error reply header is not waited on for a payload");
    check(carriesPayload("garbage"), "a header that does not parse alone takes the old wait-then-parse path");

    printf("%d checks, %d failures" "\n", checks, failures);
    return failures != 0;
}
