// Drive the production audio receive thread through a recvUdpSocket stub with the RTP
// audio queue replaced by a counter. The 500 ms start-up drop budget must be spent on
// audio packets only (RTP v2 header with a payload), not on bare 12-byte datagrams whose
// second byte happens to be 97 (C27-03).
#include "../src/Limelight-internal.h"
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect);
static int testAddPacket(PRTP_AUDIO_QUEUE queue, PRTP_PACKET packet, uint16_t length);
#define recvUdpSocket testRecv
#define RtpaAddPacket testAddPacket
#include "../src/AudioStream.c"
#undef recvUdpSocket
#undef RtpaAddPacket
#include <stdio.h>

static struct { int size; unsigned char header, payloadType; } script[512];
static int scriptLen, scriptPos, adds, terminations, checks, failures;
static int testRecv(SOCKET s, char* buffer, int size, bool useSelect) {
    (void)s; (void)useSelect;
    if (scriptPos >= scriptLen) return -1;  // ends the thread through its error path
    int n = script[scriptPos].size;
    if (n > 0) {
        memset(buffer, 0, n < size ? n : size);
        buffer[0] = (char)script[scriptPos].header;
        buffer[1] = (char)script[scriptPos].payloadType;
    }
    scriptPos++;
    return n;
}
static int testAddPacket(PRTP_AUDIO_QUEUE queue, PRTP_PACKET packet, uint16_t length) {
    (void)queue; (void)packet; (void)length;
    ++adds;
    return 0;  // not consumed, nothing ready
}
static void stubTerminated(int code) { (void)code; ++terminations; }
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
static void add(int count, int size, unsigned char header, unsigned char payloadType) {
    while (count-- > 0) { script[scriptLen].size = size; script[scriptLen].header = header; script[scriptLen].payloadType = payloadType; scriptLen++; }
}

int main(void) {
    memset(&ListenerCallbacks, 0, sizeof(ListenerCallbacks));
    ListenerCallbacks.connectionTerminated = stubTerminated;
    AudioPacketDuration = 5;  // 100 packets of start-up budget
    check(initializeAudioStream() == 0, "audio stream initialized");

    // A hundred bare headers with byte 1 == 97, then 101 real audio packets.
    scriptLen = scriptPos = adds = 0;
    add(100, (int)sizeof(RTP_PACKET), 0x80, 97);
    add(101, (int)sizeof(RTP_PACKET) + 40, 0x80, 97);
    AudioReceiveThreadProc(NULL);
    check(adds == 1, "bare 12-byte datagrams do not spend the start-up drop budget (only the 101st real packet reaches the queue)");
    destroyAudioStream();

    // FEC shards and non-RTP datagrams never count either; real audio alone drains the budget.
    check(initializeAudioStream() == 0, "audio stream re-initialized");
    scriptLen = scriptPos = adds = 0;
    add(50, (int)sizeof(RTP_PACKET) + 40, 0x80, 127);
    add(50, (int)sizeof(RTP_PACKET) + 40, 0x00, 97);
    add(100, (int)sizeof(RTP_PACKET) + 40, 0x80, 97);
    add(3, (int)sizeof(RTP_PACKET) + 40, 0x80, 97);
    AudioReceiveThreadProc(NULL);
    check(adds == 3, "FEC shards and non-RTP datagrams do not spend the budget; the packets after it reach the queue");
    destroyAudioStream();

    check(terminations == 2, "each scripted run ends through the socket error path exactly once");
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
