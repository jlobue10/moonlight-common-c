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

// Scripts: 0 = healthy reply then EOF, 1 = one byte per recv with 14 s between, 2 = one byte then interrupt.
static int mode, polls, recvs, closes, silentPolls, maxPollTimeoutMs;
static uint64_t clockMs = 1000;
static const char reply[] = "RTSP/1.0 200 OK" "\r\n" "CSeq: 1" "\r\n" "\r\n";
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
    if (mode == 0 || mode == 5) {
        if (recvs == 1) {
            int n = (int)sizeof(reply) - 1;
            memcpy(buffer, reply, n < size ? n : size);
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

int main(void) {
    int error = 0;
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

    printf("%d checks, %d failures" "\n", checks, failures);
    return failures != 0;
}
