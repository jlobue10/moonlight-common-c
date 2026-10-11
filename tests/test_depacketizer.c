// Drive the production depacketizer (queueRtpPacket -> processRtpPayload -> reassembleFrame)
// with hand-built PyroWave packets: an empty middle packet queues no zero-length buffer, a
// lost frame neither requests an IDR nor counts twice, and a normal frame reassembles intact.
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
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

// One RTP packet as the video queue hands it over: RTP header + 4-byte extension + NV header + payload,
// with the queue entry living past the packet (freed through allocPtr when the decode unit is freed).
static void deliver(uint32_t frame, uint32_t spi, int sof, int eof, const unsigned char* payload, int payloadLen) {
    const int headerLen = sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET);
    const int length = headerLen + payloadLen;
    const int entryOffset = (length + 7) & ~7;  // keep the queue entry aligned behind an odd-sized payload
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

// PyroWave frame header for a >= 7.1.415 host: 0x01 = 8-byte header, byte 3 = frame type (2 = IDR),
// bytes 4-5 = payload length of the frame's last packet (a later packet carries no header).
static void sendFrame(uint32_t frame, uint32_t firstSpi, int emptyMiddle, int lastPayload) {
    unsigned char first[8 + 16] = {0x01, 0, 0, 2};
    first[4] = (unsigned char)(lastPayload & 0xFF);
    first[5] = (unsigned char)(lastPayload >> 8);
    for (int i = 0; i < 16; i++) first[8 + i] = (unsigned char)(0xA0 + i);
    unsigned char last[64];
    for (int i = 0; i < lastPayload; i++) last[i] = (unsigned char)(0xB0 + i);
    uint32_t spi = firstSpi;
    deliver(frame, spi++, 1, 0, first, sizeof(first));
    if (emptyMiddle) deliver(frame, spi++, 0, 0, NULL, 0);
    deliver(frame, spi++, 0, 1, last, lastPayload);
}

static int popFrame(int* buffers, int* zeroLength, int* total) {
    PQUEUED_DECODE_UNIT qdu;
    *buffers = *zeroLength = *total = 0;
    if (LbqPollQueueElement(&decodeUnitQueue, (void**)&qdu) != LBQ_SUCCESS) return 0;
    for (PLENTRY e = qdu->decodeUnit.bufferList; e != NULL; e = e->next) {
        ++*buffers;
        if (e->length <= 0) ++*zeroLength;
        *total += e->length;
    }
    int ok = qdu->decodeUnit.fullLength == *total;
    LiCompleteVideoFrame(qdu, DR_OK);  // the production return path (also marks the IDR as processed)
    return ok ? 1 : -1;
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    VideoCallbacks.capabilities = 0;
    initializeVideoDepacketizer(1024);

    int buffers, zero, total;
    sendFrame(1, 0, 0, 20);
    check(popFrame(&buffers, &zero, &total) == 1 && buffers == 2 && zero == 0 && total == 36,
          "a two-packet PyroWave frame reassembles into two non-empty buffers");

    sendFrame(2, 2, 1, 20);
    check(popFrame(&buffers, &zero, &total) == 1 && buffers == 2 && zero == 0 && total == 36 && completeFrames == 2,
          "an empty middle packet queues no zero-length buffer");

    // A short first packet of frame 3 (header only, 8 bytes) carries a last-payload
    // length of 0; the frame is dropped, and frame 2's value must not survive into it.
    {
        unsigned char headerOnly[8] = {0x01, 0, 0, 2, 0, 0, 0, 0};
        deliver(3, 5, 1, 0, headerOnly, sizeof(headerOnly));
        check(lastPacketPayloadLength == 0, "a new frame's first packet resets the last-payload length");
        unsigned char tail[20] = {0};
        deliver(3, 6, 0, 1, tail, sizeof(tail));
        int rc = popFrame(&buffers, &zero, &total);
        check(rc != 1 || zero == 0, "a header-only last payload length never yields a zero-length buffer");
    }

    // Lose frame 4; frame 5 then arrives. Intra-only: no IDR request, counted once, no RFI.
    nextFrameNumber = 4;  // frame 3 is not sent in this run
    idrRequests = lossReports = 0;
    const int dropsBefore = consecutiveFrameDrops;
    notifyFrameLost(4, false);
    check(idrRequests == 0 && lossReports == 0 && nextFrameNumber == 5 && consecutiveFrameDrops == dropsBefore + 1,
          "a lost PyroWave frame requests nothing and advances to its successor");
    sendFrame(5, 7, 0, 20);
    check(popFrame(&buffers, &zero, &total) == 1 && idrRequests == 0 && consecutiveFrameDrops == 0,
          "the frame after a loss decodes and clears the drop count");

    stopVideoDepacketizer();  // destroy asserts a stopped or empty queue
    destroyVideoDepacketizer();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
