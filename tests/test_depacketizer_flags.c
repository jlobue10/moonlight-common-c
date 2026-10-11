// Host-controlled SOF/EOF flags that break the frame-boundary invariant (a first packet
// without SOF, a SOF repeated mid-frame, a frame whose last packet lacks EOF followed by
// the next SOF) must be handled as corrupt frames. Built with -DLC_DEBUG: before the fix
// each case aborted a debug client at the LC_ASSERT(firstPacket ^ decodingFrame).
#include "../src/Limelight-internal.h"
static void stubIdr(void);
static void stubLoss(uint32_t startFrame, uint32_t endFrame);
static void stubComplete(uint32_t frameNumber, bool isLTR);
#define LiRequestIdrFrame stubIdr
#define connectionDetectedFrameLoss stubLoss
#define connectionReceivedCompleteFrame stubComplete
#include "../src/VideoDepacketizer.c"
#undef LiRequestIdrFrame
#undef connectionDetectedFrameLoss
#undef connectionReceivedCompleteFrame
#include <stdio.h>

static int idrRequests, lossReports, completeFrames, checks, failures;
static void stubIdr(void) { ++idrRequests; }
static void stubLoss(uint32_t startFrame, uint32_t endFrame) { (void)startFrame; (void)endFrame; ++lossReports; }
static void stubComplete(uint32_t frameNumber, bool isLTR) { (void)frameNumber; (void)isLTR; ++completeFrames; }
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); fflush(stdout); failures += !ok; }

static void deliver(uint32_t frame, uint32_t spi, int sof, int eof, const unsigned char* payload, int payloadLen) {
    const int headerLen = sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET);
    const int length = headerLen + payloadLen;
    const int entryOffset = (length + 7) & ~7;
    char* buffer = calloc(1, entryOffset + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    rtp->header = FLAG_EXTENSION;
    rtp->sequenceNumber = (uint16_t)spi;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + sizeof(RTP_PACKET) + 4);
    nv->streamPacketIndex = LE32(spi << 8);
    nv->frameIndex = LE32(frame);
    nv->flags = (sof ? FLAG_SOF : 0) | (eof ? FLAG_EOF : 0) | FLAG_CONTAINS_PIC_DATA;
    if (payloadLen) memcpy(buffer + headerLen, payload, payloadLen);
    PRTPV_QUEUE_ENTRY entry = (PRTPV_QUEUE_ENTRY)(buffer + entryOffset);
    entry->packet = rtp;
    entry->length = length;
    entry->receiveTimeUs = 1000000 + spi;
    entry->presentationTimeUs = 0;
    entry->rtpTimestamp = frame * 1000;
    entry->isParity = false;
    queueRtpPacket(entry);
}

static int popFrames(void) {
    int n = 0;
    PQUEUED_DECODE_UNIT qdu;
    while (LbqPollQueueElement(&decodeUnitQueue, (void**)&qdu) == LBQ_SUCCESS) {
        ++n;
        LiCompleteVideoFrame(qdu, DR_OK);
    }
    return n;
}

static void goodFrame(uint32_t frame, uint32_t firstSpi) {
    unsigned char first[8 + 16] = {0x01, 0, 0, 2};
    first[4] = 20;
    unsigned char last[20] = {0};
    deliver(frame, firstSpi, 1, 0, first, sizeof(first));
    deliver(frame, firstSpi + 1, 0, 1, last, sizeof(last));
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    VideoCallbacks.capabilities = 0;
    initializeVideoDepacketizer(1024);

    unsigned char first[8 + 16] = {0x01, 0, 0, 2};
    first[4] = 20;
    unsigned char last[20] = {0};

    // 1. The stream's first packet lacks SOF (SPI 0 is continuous against the initial index).
    deliver(1, 0, 0, 1, last, sizeof(last));
    check(popFrames() == 0 && !decodingFrame, "a first packet without SOF is dropped as a corrupt frame, not asserted");

    // 2. A SOF repeated mid-frame.
    goodFrame(2, 1);
    check(popFrames() == 1, "a well-formed frame still reassembles");
    deliver(3, 3, 1, 0, first, sizeof(first));
    deliver(3, 4, 1, 0, first, sizeof(first));
    check(!decodingFrame, "a SOF repeated mid-frame drops the frame");
    deliver(3, 5, 0, 1, last, sizeof(last));
    check(popFrames() == 0, "the tail of the dropped frame yields nothing");

    // 3. A frame whose last packet lacks EOF, followed by the next frame's SOF.
    goodFrame(4, 6);
    check(popFrames() == 1, "recovery: the next complete frame reassembles");
    deliver(5, 8, 1, 0, first, sizeof(first));
    deliver(5, 9, 0, 0, last, sizeof(last));
    deliver(6, 10, 1, 0, first, sizeof(first));
    deliver(6, 11, 0, 1, last, sizeof(last));
    const int popped = popFrames();
    goodFrame(7, 12);
    check(popped <= 1 && popFrames() == 1, "a missing EOF followed by a new SOF does not abort and the stream recovers");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
