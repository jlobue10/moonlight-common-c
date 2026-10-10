// Exercise the actual RTP video queue admission logic: duplicates and reordered
// packets on a block the size PyroWave sends, with the depacketizer and control
// stream replaced by counters. No sockets, decoder or FEC parity involved.
#define PltGetMicroseconds testGetMicroseconds
#define queueRtpPacket testQueueRtpPacket
#define notifyFrameLost testNotifyFrameLost
#define connectionSawFrame testSawFrame
#define connectionSendFrameFecStatus testFecStatus
#define LiRequestIdrFrame testRequestIdr
#define isReferenceFrameInvalidationEnabled testRfiEnabled
#include "../src/Limelight-internal.h"
#include "../src/RtpVideoQueue.c"
#undef PltGetMicroseconds
#undef queueRtpPacket
#undef notifyFrameLost
#undef connectionSawFrame
#undef connectionSendFrameFecStatus
#undef LiRequestIdrFrame
#undef isReferenceFrameInvalidationEnabled

static int delivered, lostFrames, lastDeliveredSeq, deliveredInOrder = 1, verifyPayload, payloadMismatches;
#define PAYLOAD 1392
// Deterministic payload per sequence number, so recovered shards can be checked byte for byte.
static void fillPayload(unsigned char* data, uint16_t seq) {
    for (int i = 0; i < PAYLOAD; i++) data[i] = (unsigned char)(seq * 31 + i * 7);
}
// The transport clock is initialized by connection startup in production.
uint64_t testGetMicroseconds(void) { static uint64_t now = 1000000; return ++now; }
void testQueueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    if (delivered && U16(entry->packet->sequenceNumber) != U16(lastDeliveredSeq + 1)) deliveredInOrder = 0;
    lastDeliveredSeq = entry->packet->sequenceNumber;
    ++delivered;
    if (verifyPayload) {
        unsigned char expected[PAYLOAD];
        fillPayload(expected, entry->packet->sequenceNumber);
        const unsigned char* data = (const unsigned char*)entry->packet + sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET);
        if (memcmp(data, expected, PAYLOAD) != 0) ++payloadMismatches;
    }
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

// Builds a shard buffer without submitting it: RTP header, 4-byte extension, NV header, payload.
static char* buildFecPacket(uint32_t frame, uint16_t seq, uint32_t fecIndex, uint32_t dataPackets, uint32_t fecPercentage, int sof, int eof, int* lengthOut) {
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
    nv->multiFecBlocks = 0x00;
    nv->fecInfo = LE32((dataPackets << 22) | (fecIndex << 12) | (fecPercentage << 4));
    *lengthOut = length;
    return buffer;
}

static int submitBuffer(char* buffer, int length) {
    ((PRTPV_QUEUE_ENTRY)(buffer + length))->receiveTimeUs = 1000;
    int status = RtpvAddPacket(&queue, (PRTP_PACKET)buffer, length, (PRTPV_QUEUE_ENTRY)(buffer + length));
    if (status != RTPF_RET_QUEUED) free(buffer);
    return status;
}

// A recoverable block: `data` data shards with real Reed-Solomon parity computed the way the
// host does (over the whole packet, header included; the recovered header fields are patched by
// the queue). Shards are submitted in `order` (indices into data+parity), skipping `skip`.
static void deliverRecoverableBlock(uint32_t frame, uint16_t base, unsigned int data, unsigned int percentage,
                                    const unsigned int* order, unsigned int count, int skip) {
    const unsigned int parity = (data * percentage + 99) / 100;
    unsigned char** shards = calloc(data + parity, sizeof(unsigned char*));
    char** buffers = calloc(data + parity, sizeof(char*));
    int length = 0;
    for (unsigned int i = 0; i < data; i++) {
        buffers[i] = buildFecPacket(frame, (uint16_t)(base + i), i, data, percentage, i == 0, i == data - 1, &length);
        fillPayload((unsigned char*)buffers[i] + sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET), (uint16_t)(base + i));
        shards[i] = (unsigned char*)buffers[i];
    }
    for (unsigned int i = data; i < data + parity; i++) {
        buffers[i] = buildFecPacket(frame, (uint16_t)(base + i), i, data, percentage, 0, 0, &length);
        shards[i] = (unsigned char*)buffers[i];
    }
    reed_solomon* rs = reed_solomon_new(data, parity);
    assert(rs != NULL);
    // Parity is computed over the receive size (packetSize + the RTP header) like reconstructFrame's decode.
    int encodeResult = reed_solomon_encode(rs, shards, data + parity, StreamConfig.packetSize + MAX_RTP_HEADER_SIZE);
    assert(encodeResult == 0);
    reed_solomon_release(rs);
    // The host stamps the RTP header and the FEC bookkeeping fields of each parity shard after
    // encoding; the queue patches exactly those fields in a recovered packet, so the payload and
    // the remaining NV header fields (flags, extraFlags, streamPacketIndex) come out of the
    // Reed-Solomon output unchanged and must be left as encoded here.
    for (unsigned int i = data; i < data + parity; i++) {
        int unused;
        char* header = buildFecPacket(frame, (uint16_t)(base + i), i, data, percentage, 0, 0, &unused);
        memcpy(buffers[i], header, sizeof(RTP_PACKET) + 4);
        PNV_VIDEO_PACKET stamped = (PNV_VIDEO_PACKET)(header + sizeof(RTP_PACKET) + 4);
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffers[i] + sizeof(RTP_PACKET) + 4);
        nv->frameIndex = stamped->frameIndex;
        nv->multiFecFlags = stamped->multiFecFlags;
        nv->multiFecBlocks = stamped->multiFecBlocks;
        nv->fecInfo = stamped->fecInfo;
        free(header);
    }
    for (unsigned int n = 0; n < count; n++) {
        unsigned int i = order[n];
        if ((int)i == skip) { free(buffers[i]); continue; }
        submitBuffer(buffers[i], length);
    }
    free(shards);
    free(buffers);
}

// A shard of a block that declares parity: fecIndex counts data then parity shards.
static int addFecPacket(uint32_t frame, uint16_t seq, uint32_t fecIndex, uint32_t dataPackets, uint32_t fecPercentage, int sof, int eof) {
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
    nv->multiFecBlocks = 0x00;
    nv->fecInfo = LE32((dataPackets << 22) | (fecIndex << 12) | (fecPercentage << 4));
    int status = RtpvAddPacket(&queue, rtp, length, (PRTPV_QUEUE_ENTRY)(buffer + length));
    if (status != RTPF_RET_QUEUED) free(buffer);
    return status;
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, 0}, sizeof(AppVersionQuad));  // multi-FEC capable host
    StreamConfig.packetSize = PAYLOAD + sizeof(NV_VIDEO_PACKET);  // counted from the end of the RTP header, as the host sets it
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

    // A block whose data + parity exceed nanors' 255-shard limit can never be recovered:
    // its parity must be ignored up front (no reed_solomon_new() per packet, no assert)
    // and the frame reported lost like any other incomplete block.
    RtpvCleanupQueue(&queue);
    RtpvInitializeQueue(&queue);
    delivered = 0; lostFrames = 0;
    const uint32_t wide = 300, parity = 30;  // 10 % of 300
    int wideQueued = 0, parityRejected = 0;
    for (uint32_t i = 0; i < wide; i++) {
        if (i == 7) continue;  // lost
        if (addFecPacket(1, (uint16_t)(1000 + i), i, wide, 10, i == 0, i == wide - 1) == RTPF_RET_QUEUED) ++wideQueued;
    }
    for (uint32_t i = 0; i < parity; i++) {
        if (addFecPacket(1, (uint16_t)(1000 + wide + i), wide + i, wide, 10, 0, 0) == RTPF_RET_REJECTED) ++parityRejected;
    }
    check(wideQueued == (int)wide - 1, "oversized FEC block: every data shard is admitted");
    check(parityRejected == (int)parity, "oversized FEC block: parity shards are rejected instead of fed to nanors");
    check(delivered == 0 && lostFrames == 0, "oversized FEC block: nothing delivered, not yet reported");
    check(addFecPacket(2, (uint16_t)(1000 + wide + parity), 0, 2, 0, 1, 0) == RTPF_RET_QUEUED && lostFrames == 1,
          "oversized FEC block: reported lost once when the next frame starts");
    // A block within the limit still uses its parity window.
    check(addFecPacket(2, (uint16_t)(1000 + wide + parity + 1), 1, 2, 0, 0, 1) == RTPF_RET_QUEUED && delivered == 2,
          "the next frame completes normally");
    RtpvCleanupQueue(&queue);
    RtpvInitializeQueue(&queue);
    delivered = 0;
    // This is an admission test: keep the block incomplete because these synthetic
    // parity bytes are not a valid Reed-Solomon encoding. Recovery is tested separately.
    addFecPacket(1, 2000, 0, 200, 10, 1, 0);
    int smallParityQueued = 0;
    for (uint32_t i = 0; i < 20; i++) {
        if (addFecPacket(1, (uint16_t)(2200 + i), 200 + i, 200, 10, 0, 0) == RTPF_RET_QUEUED) ++smallParityQueued;
    }
    check(smallParityQueued == 20, "a block within the FEC limit still admits its parity shards");

    // Ignoring unusable parity must not make debug FEC validation wait for an
    // extra shard or attempt recovery when every data shard has arrived.
    for (int reversed = 0; reversed < 2; reversed++) {
        RtpvCleanupQueue(&queue);
        RtpvInitializeQueue(&queue);
        delivered = lostFrames = 0;
        deliveredInOrder = 1;
        lastDeliveredSeq = 999;
        int accepted = 0;
        for (uint32_t j = 0; j < wide; j++) {
            uint32_t i = reversed ? wide - j - 1 : j;
            if (addFecPacket(1, (uint16_t)(1000 + i), i, wide, 10,
                             i == 0, i == wide - 1) == RTPF_RET_QUEUED) ++accepted;
        }
        check(accepted == (int)wide, "complete oversized FEC block: every data shard is admitted");
        check(delivered == (int)wide && deliveredInOrder && lostFrames == 0,
              "complete oversized FEC block: delivered in order without waiting for unusable parity");
        check(queue.pendingFecBlockList.count == 0 && queue.completedFecBlockList.count == 0,
              "complete oversized FEC block: queue ownership is drained");
    }

    // Real parity recovery through the queue: a lost data shard is rebuilt from a parity shard,
    // delivered in order with its exact payload, in both arrival orders (the reordered path
    // stages recovered shards through stageReorderedFecBlock).
    {
        const unsigned int ordered[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        const unsigned int reversed[12] = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
        for (int rev = 0; rev < 2; rev++) {
            RtpvCleanupQueue(&queue);
            RtpvInitializeQueue(&queue);
            delivered = lostFrames = payloadMismatches = 0;
            deliveredInOrder = 1;
            verifyPayload = 1;
            deliverRecoverableBlock(1, 3000, 8, 50, rev ? reversed : ordered, 12, 3);
            check(delivered == 8 && deliveredInOrder, rev ? "FEC recovery (reversed arrival): all eight data shards delivered in order"
                                                          : "FEC recovery (ordered arrival): all eight data shards delivered in order");
            check(payloadMismatches == 0, rev ? "FEC recovery (reversed arrival): the recovered shard carries its exact payload"
                                              : "FEC recovery (ordered arrival): the recovered shard carries its exact payload");
            check(lostFrames == 0 && queue.pendingFecBlockList.count == 0 && queue.completedFecBlockList.count == 0,
                  rev ? "FEC recovery (reversed arrival): nothing lost, queues drained" : "FEC recovery (ordered arrival): nothing lost, queues drained");
            verifyPayload = 0;
        }
        // Two lost shards with four parity shards also recover; five lost do not.
        RtpvCleanupQueue(&queue);
        RtpvInitializeQueue(&queue);
        delivered = lostFrames = payloadMismatches = 0;
        deliveredInOrder = 1;
        verifyPayload = 1;
        const unsigned int twoLost[10] = {0, 1, 2, 4, 6, 7, 8, 9, 10, 11};  // shards 3 and 5 never arrive
        deliverRecoverableBlock(1, 4000, 8, 50, twoLost, 10, -1);
        check(delivered == 8 && deliveredInOrder && payloadMismatches == 0, "FEC recovery: two lost shards are rebuilt from four parity shards");
        verifyPayload = 0;
    }

    RtpvCleanupQueue(&queue);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
