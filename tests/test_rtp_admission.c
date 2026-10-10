// Exercise the actual RTP video queue admission logic: duplicates and reordered
// packets on a block the size PyroWave sends, with the depacketizer and control
// stream replaced by counters. No sockets, decoder or FEC parity involved.
#define queueRtpPacket testQueueRtpPacket
#define notifyFrameLost testNotifyFrameLost
#define connectionSawFrame testSawFrame
#define connectionSendFrameFecStatus testFecStatus
#define LiRequestIdrFrame testRequestIdr
#define isReferenceFrameInvalidationEnabled testRfiEnabled
#include "../src/Limelight-internal.h"
#include "../src/RtpVideoQueue.c"
#undef queueRtpPacket
#undef notifyFrameLost
#undef connectionSawFrame
#undef connectionSendFrameFecStatus
#undef LiRequestIdrFrame
#undef isReferenceFrameInvalidationEnabled

static int delivered, lostFrames, lastDeliveredSeq, deliveredInOrder = 1;
void testQueueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    if (delivered && U16(entry->packet->sequenceNumber) != U16(lastDeliveredSeq + 1)) deliveredInOrder = 0;
    lastDeliveredSeq = entry->packet->sequenceNumber;
    ++delivered;
    free(entry->packet);
}
void testNotifyFrameLost(unsigned int frameNumber, bool speculative) { (void)frameNumber; (void)speculative; ++lostFrames; }
void testSawFrame(uint32_t frameIndex) { (void)frameIndex; }
void testFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void testRequestIdr(void) {}
bool testRfiEnabled(void) { return false; }

static RTP_VIDEO_QUEUE queue;
static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

#define PAYLOAD 1392
// Builds one data shard of a block with no parity (fecPercentage 0), as the host sends.
static int addPacket(uint32_t frame, uint16_t seq, uint32_t fecIndex, uint32_t dataPackets, int sof, int eof) {
    const int headerLen = sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET);
    const int length = headerLen + PAYLOAD;
    char* buffer = calloc(1, length + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    rtp->header = FLAG_EXTENSION;
    rtp->sequenceNumber = seq;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + sizeof(RTP_PACKET) + 4);
    nv->streamPacketIndex = LE32(seq);
    nv->frameIndex = LE32(frame);
    nv->flags = (sof ? FLAG_SOF : 0) | (eof ? FLAG_EOF : 0);
    nv->multiFecFlags = 0x10;
    nv->multiFecBlocks = 0x00;  // block 0 of 1
    nv->fecInfo = LE32((dataPackets << 22) | (fecIndex << 12) | (0u << 4));
    int status = RtpvAddPacket(&queue, rtp, length, (PRTPV_QUEUE_ENTRY)(buffer + length));
    if (status != RTPF_RET_QUEUED) free(buffer);
    return status;
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, 0}, sizeof(AppVersionQuad));  // multi-FEC capable host
    StreamConfig.packetSize = PAYLOAD;
    RtpvInitializeQueue(&queue);

    // Frame 1: four shards, the third arrives twice and after the fourth.
    check(addPacket(1, 100, 0, 4, 1, 0) == RTPF_RET_QUEUED, "first shard queued");
    check(addPacket(1, 101, 1, 4, 0, 0) == RTPF_RET_QUEUED, "second shard queued");
    check(addPacket(1, 103, 3, 4, 0, 1) == RTPF_RET_QUEUED, "fourth shard queued ahead of the third");
    check(addPacket(1, 103, 3, 4, 0, 1) == RTPF_RET_REJECTED, "duplicate of a queued shard is rejected");
    check(addPacket(1, 101, 1, 4, 0, 0) == RTPF_RET_REJECTED, "duplicate of an in-order shard is rejected");
    check(delivered == 0, "nothing is delivered while a shard is missing");
    check(addPacket(1, 102, 2, 4, 0, 0) == RTPF_RET_QUEUED, "reordered third shard queued");
    check(delivered == 4 && deliveredInOrder, "complete frame delivered in sequence order");
    check(lostFrames == 0, "no frame loss reported");

    // Frame 2: a PyroWave-sized unprotected block loses one shard early; every later
    // shard must still be admitted, duplicates still rejected, and the frame is lost
    // only when frame 3 begins.
    const uint32_t big = 1000;
    uint16_t base = 200;
    int queued = 0, rejected = 0;
    for (uint32_t i = 0; i < big; i++) {
        if (i == 5) continue;  // lost
        int status = addPacket(2, (uint16_t)(base + i), i, big, i == 0, i == big - 1);
        if (status == RTPF_RET_QUEUED) ++queued; else ++rejected;
        if (i == 500 && addPacket(2, (uint16_t)(base + 400), 400, big, 0, 0) == RTPF_RET_REJECTED) ++rejected;
    }
    check(queued == (int)big - 1 && rejected == 1, "after an early loss every later shard is admitted and a duplicate is rejected");
    check(delivered == 4 && lostFrames == 0, "an incomplete block delivers nothing and is not yet reported");
    check(addPacket(3, (uint16_t)(base + big), 0, 2, 1, 0) == RTPF_RET_QUEUED, "next frame's first shard starts a new block");
    check(lostFrames == 1, "the incomplete frame is reported lost when the next frame starts");
    check(addPacket(3, (uint16_t)(base + big + 1), 1, 2, 0, 1) == RTPF_RET_QUEUED && delivered == 6, "the next frame completes normally");

    // Sequence wrap: a block straddling 65535 -> 0 still indexes the bitmap correctly.
    // Sequence numbers only ever climb (a jump backwards is rejected as behind the
    // window), so walk a fresh queue up to the wrap in steps under 32768.
    RtpvCleanupQueue(&queue);
    RtpvInitializeQueue(&queue);
    check(addPacket(1, 30000, 0, 2, 1, 0) == RTPF_RET_QUEUED && addPacket(1, 30001, 1, 2, 0, 1) == RTPF_RET_QUEUED &&
          delivered == 8, "wrap: a frame at 30000 is delivered");
    check(addPacket(2, 60000, 0, 2, 1, 0) == RTPF_RET_QUEUED && addPacket(2, 60001, 1, 2, 0, 1) == RTPF_RET_QUEUED &&
          delivered == 10, "wrap: a frame at 60000 is delivered");
    deliveredInOrder = 1; lastDeliveredSeq = 65533;  // the order check is per frame; earlier frames were not contiguous
    check(addPacket(3, 65534, 0, 3, 1, 0) == RTPF_RET_QUEUED, "wrap: first shard");
    check(addPacket(3, 0, 2, 3, 0, 1) == RTPF_RET_QUEUED, "wrap: last shard past the wrap");
    check(addPacket(3, 0, 2, 3, 0, 1) == RTPF_RET_REJECTED, "wrap: duplicate past the wrap rejected");
    check(addPacket(3, 65535, 1, 3, 0, 0) == RTPF_RET_QUEUED && delivered == 13 && deliveredInOrder,
          "wrap: middle shard completes the frame in order");

    RtpvCleanupQueue(&queue);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
