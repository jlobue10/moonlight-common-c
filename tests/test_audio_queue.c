// Exercise the production audio RTP/FEC queue: in-order delivery, recovery of a
// lost data shard from a parity shard, and the bound on how far ahead a parity
// shard may open a block (an unauthenticated far-future base used to make the
// queue abandon the current block at once).
#include "../src/Limelight-internal.h"
static uint64_t clockUs = 1000000;
static uint64_t testMicroseconds(void) { return clockUs; }
#define PltGetMicroseconds testMicroseconds
#include "../src/RtpAudioQueue.c"
#undef PltGetMicroseconds

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

#define BLOCK 64
static RTP_AUDIO_QUEUE queue;
static unsigned char storage[64][sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + BLOCK];
static int next;
static bool wrapTimestamps;

static unsigned char* payloadOf(uint16_t seq, unsigned char* out) {
    for (int i = 0; i < BLOCK; i++) out[i] = (unsigned char)(seq * 7 + i);
    return out;
}

static int addData(uint16_t seq) {
    PRTP_PACKET rtp = (PRTP_PACKET)storage[next++ % 64];
    rtp->header = 0x80;
    rtp->packetType = RTP_PAYLOAD_TYPE_AUDIO;
    rtp->sequenceNumber = seq;
    // RTP timestamps continue increasing when the 16-bit sequence number wraps.
    rtp->timestamp = (seq + (wrapTimestamps && seq < 0x8000 ? 0x10000 : 0)) * AudioPacketDuration;
    rtp->ssrc = 0x1234;
    payloadOf(seq, (unsigned char*)(rtp + 1));
    return RtpaAddPacket(&queue, rtp, sizeof(RTP_PACKET) + BLOCK);
}

static int addFec(uint16_t base, uint8_t shard, const unsigned char* parity) {
    PRTP_PACKET rtp = (PRTP_PACKET)storage[next++ % 64];
    rtp->header = 0x80;
    rtp->packetType = RTP_PAYLOAD_TYPE_FEC;
    rtp->sequenceNumber = (uint16_t)(base + RTPA_DATA_SHARDS + shard);
    rtp->timestamp = 0;
    rtp->ssrc = 0x1234;
    PAUDIO_FEC_HEADER fec = (PAUDIO_FEC_HEADER)(rtp + 1);
    fec->fecShardIndex = shard;
    fec->payloadType = RTP_PAYLOAD_TYPE_AUDIO;
    fec->baseSequenceNumber = BE16(base);
    fec->baseTimestamp = BE32((uint32_t)(base * AudioPacketDuration));
    fec->ssrc = BE32(0x1234);
    memcpy(fec + 1, parity, BLOCK);
    return RtpaAddPacket(&queue, rtp, sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + BLOCK);
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, 0}, sizeof(AppVersionQuad));
    AudioPacketDuration = 5;
    RtpaInitializeQueue(&queue);

    // Synchronisation consumes the first partial block; delivery starts at the next boundary
    // and the queue leaves its synchronising state once that block has been consumed.
    addData(100);
    // The next expected packet is fixed by the first packet, so a far-future parity shard is
    // bounded during synchronisation too (it used to pin a tail block for minutes).
    unsigned char zerosEarly[BLOCK] = {0};
    check(addFec((uint16_t)(104 + 4000), 0, zerosEarly) == 0 && queue.blockTail == NULL &&
          queue.stats.packetCountFecInvalid == 1, "a far-future parity shard during synchronisation opens no block");
    check(addData(104) == RTPQ_RET_HANDLE_NOW && addData(105) == RTPQ_RET_HANDLE_NOW &&
          addData(106) == RTPQ_RET_HANDLE_NOW && addData(107) == RTPQ_RET_HANDLE_NOW, "in-order data is handed over at once");
    check(!queue.synchronizing, "the queue is synchronised after its first full block");

    // A parity shard with a base thousands of packets ahead opens nothing and changes nothing.
    unsigned char zeros[BLOCK] = {0};
    PRTPA_FEC_BLOCK tailBefore = queue.blockTail;
    uint32_t invalidBefore = queue.stats.packetCountFecInvalid;
    check(addFec((uint16_t)(108 + 4000), 0, zeros) == 0, "a far-future parity shard is not reported as ready data");
    check(queue.blockTail == tailBefore && queue.stats.packetCountFecInvalid == invalidBefore + 1,
          "a far-future parity shard opens no block and is counted invalid");
    check(queue.stats.packetCountOOS == 0 && !queue.receivedOosData, "no out-of-sequence data was recorded");

    // A lost data shard is recovered from a parity shard of its own block.
    unsigned char shards[RTPA_TOTAL_SHARDS][BLOCK];
    unsigned char* shardPtrs[RTPA_TOTAL_SHARDS];
    for (int i = 0; i < RTPA_DATA_SHARDS; i++) { payloadOf((uint16_t)(108 + i), shards[i]); shardPtrs[i] = shards[i]; }
    for (int i = 0; i < RTPA_FEC_SHARDS; i++) { memset(shards[RTPA_DATA_SHARDS + i], 0, BLOCK); shardPtrs[RTPA_DATA_SHARDS + i] = shards[RTPA_DATA_SHARDS + i]; }
    check(reed_solomon_encode(queue.rs, shardPtrs, RTPA_TOTAL_SHARDS, BLOCK) == 0, "parity computed for the test block");
    check(addData(108) == RTPQ_RET_HANDLE_NOW && addData(109) == RTPQ_RET_HANDLE_NOW, "block 108: first two shards delivered");
    check(addData(111) == 0, "block 108: a shard after a gap waits");
    check(addFec(108, 0, shards[RTPA_DATA_SHARDS]) == RTPQ_RET_PACKET_READY, "block 108: one parity shard completes the block");
    uint16_t length = 0;
    PRTP_PACKET recovered = RtpaGetQueuedPacket(&queue, 0, &length);
    unsigned char expected[BLOCK];
    payloadOf(110, expected);
    check(recovered != NULL && length == sizeof(RTP_PACKET) + BLOCK && recovered->sequenceNumber == 110 &&
          memcmp(recovered + 1, expected, BLOCK) == 0, "the lost packet 110 is recovered with its payload");
    free(recovered);
    PRTP_PACKET following = RtpaGetQueuedPacket(&queue, 0, &length);
    check(following != NULL && following->sequenceNumber == 111 && length == sizeof(RTP_PACKET) + BLOCK, "packet 111 follows");
    free(following);
    check(RtpaGetQueuedPacket(&queue, 0, &length) == NULL, "nothing else is queued");

    // A parity shard one block ahead of its data is still accepted and opens its block.
    tailBefore = queue.blockTail;
    check(addFec(116, 1, zeros) == 0 && queue.blockTail != tailBefore && queue.blockTail->fecHeader.baseSequenceNumber == 116,
          "a parity shard one block ahead opens its block");

    RtpaCleanupQueue(&queue);
    // Reordered packets get one block duration plus the reorder grace in ms.
    // Feed production admission/dequeue with a controlled microsecond clock.
    for (int duration = 5; duration <= 20; duration *= 2) {
        AudioPacketDuration = duration;
        clockUs = 1000000;
        RtpaInitializeQueue(&queue);addData(100);
        for (int seq = 104; seq <= 107; ++seq) addData((uint16_t)seq);
        addData(107); // A late duplicate exits fast recovery mode.
        check(queue.receivedOosData, "late data enables the reordering grace");
        addData(108);
        clockUs += (uint64_t)duration * 2000;
        addData(110);addData(111); // 109 is still in flight.
        clockUs = 1000000 + (uint64_t)duration * RTPA_DATA_SHARDS * 1000;
        int status = addData(112);
        check(status == 0 && !queue.blockHead->allowDiscontinuity && queue.stats.packetCountFecFailed == 0,
              "a second block does not prematurely discard reordered audio");
        if (status & RTPQ_RET_PACKET_READY) {
            PRTP_PACKET packet;
            while ((packet = RtpaGetQueuedPacket(&queue, 1, &length)) != NULL) free(packet);
        }
        clockUs += 5000;
        // 110..112 are already buffered behind 109: it is reported ready with them, not
        // handed over alone (which left the successors waiting for the next arrival).
        check(addData(109) == RTPQ_RET_PACKET_READY, "late audio inside the full grace is delivered with its buffered successors");
        int delivered = 0;
        PRTP_PACKET packet;
        while ((packet = RtpaGetQueuedPacket(&queue, 0, &length)) != NULL) {
            if (length != 0) ++delivered;
            free(packet);
        }
        check(delivered == 4 && queue.stats.packetCountFecFailed == 0,
              "reordered audio drains in order without loss concealment");
        RtpaCleanupQueue(&queue);

        clockUs = 2000000;
        RtpaInitializeQueue(&queue);addData(100);
        for (int seq = 104; seq <= 107; ++seq) addData((uint16_t)seq);
        addData(107);addData(108);addData(110);addData(111);
        clockUs += ((uint64_t)duration * RTPA_DATA_SHARDS + RTPQ_OOS_WAIT_TIME_MS) * 1000 + 1;
        check((addData(112) & RTPQ_RET_PACKET_READY) && queue.blockHead->allowDiscontinuity &&
              queue.stats.packetCountFecFailed == 1, "a genuinely lost packet expires after the full grace");
        RtpaCleanupQueue(&queue);
    }

    // A gap filled behind buffered successors drains them at once (C23-03), including
    // across a block boundary; an in-order packet with nothing behind it is still fast.
    AudioPacketDuration = 5;
    clockUs = 3000000;
    RtpaInitializeQueue(&queue);
    addData(100);
    for (int seq = 104; seq <= 107; ++seq) addData((uint16_t)seq);
    check(addData(108) == RTPQ_RET_HANDLE_NOW, "an in-order packet with nothing buffered behind it is handed over at once");
    check(addData(110) == 0 && addData(111) == 0, "packets behind a gap wait");
    check(addData(109) == RTPQ_RET_PACKET_READY, "the packet that fills the gap is reported ready with its successors");
    int seqs[4] = {0}; int drained = 0;
    PRTP_PACKET drainedPacket;
    while ((drainedPacket = RtpaGetQueuedPacket(&queue, 0, &length)) != NULL) {
        if (drained < 4) seqs[drained] = drainedPacket->sequenceNumber;
        ++drained;
        free(drainedPacket);
    }
    check(drained == 3 && seqs[0] == 109 && seqs[1] == 110 && seqs[2] == 111, "the gap filler and both successors drain in order");
    for (int seq = 112; seq <= 114; ++seq) addData((uint16_t)seq);
    // No reorder grace yet: 116 makes the queue give 115 up at once (a placeholder is ready);
    // when 115 then arrives late it is still reported ready together with the buffered 116.
    check((addData(116) & RTPQ_RET_PACKET_READY) && addData(115) == RTPQ_RET_PACKET_READY, "a successor buffered in the next block is drained with the block's last packet");
    drained = 0;
    while ((drainedPacket = RtpaGetQueuedPacket(&queue, 0, &length)) != NULL) { ++drained; free(drainedPacket); }
    check(drained == 2, "the block boundary is crossed in one drain");
    RtpaCleanupQueue(&queue);

    // One data packet every other block (a whole block lost in between) with the consumer
    // draining between adds: after the head block is given up, the next packet precedes the
    // new head, and only an add used to resynchronise, so every second block stayed live and
    // the backlog grew with the stream (half of 300 adds; bounded only by the 16-bit ring).
    // Draining must move on to the oldest block itself (C23-01).
    clockUs = 4000000;
    RtpaInitializeQueue(&queue);
    addData(100);
    for (int seq = 104; seq <= 107; ++seq) addData((uint16_t)seq);
    int maxLive = 0, totalDrained = 0;
    for (int block = 0; block < 64; ++block) {
        int status = addData((uint16_t)(108 + block * 2 * RTPA_DATA_SHARDS));
        if (status & RTPQ_RET_PACKET_READY) {
            while ((drainedPacket = RtpaGetQueuedPacket(&queue, 0, &length)) != NULL) { free(drainedPacket); ++totalDrained; }
        }
        int live = 0;
        for (PRTPA_FEC_BLOCK b = queue.blockHead; b != NULL; b = b->next) ++live;
        if (live > maxLive) maxLive = live;
    }
    check(maxLive <= 3, "a stream that loses every other block keeps at most a few live blocks");
    check(totalDrained >= 63 * RTPA_DATA_SHARDS, "every retired block is drained (data or loss placeholders) as it is given up");
    RtpaCleanupQueue(&queue);

    // A late block tail must also unblock buffered data beyond a wholly lost
    // block. Exercise both ordinary sequence numbers and the 16-bit wrap.
    for (int wrap = 0; wrap < 2; ++wrap) {
        uint16_t base = wrap ? 65528 : 108;
        wrapTimestamps = wrap != 0;
        clockUs = 5000000;
        RtpaInitializeQueue(&queue);
        addData((uint16_t)(base - 8));
        for (int i = -4; i < 0; ++i) addData((uint16_t)(base + i));
        addData((uint16_t)(base - 1)); // Real late duplicate enables reordering grace.
        for (int i = 0; i < 3; ++i) addData((uint16_t)(base + i));
        check(addData((uint16_t)(base + 8)) == 0,
              "a later block waits for the current block's reordered tail");
        int status = addData((uint16_t)(base + 3));
        check(status == RTPQ_RET_PACKET_READY,
              "late tail unblocks buffered audio across a wholly lost block");
        drained = 0;
        // Match AudioStream's caller: HANDLE_NOW does not enter the queue drain.
        if (status & RTPQ_RET_PACKET_READY) {
            while ((drainedPacket = RtpaGetQueuedPacket(&queue, 0, &length)) != NULL) {
                if (drained < 4) seqs[drained] = drainedPacket->sequenceNumber;
                ++drained;
                free(drainedPacket);
            }
        }
        check(drained == 2 && seqs[0] == (uint16_t)(base + 3) && seqs[1] == (uint16_t)(base + 8),
              "late tail and buffered successor reach the decoder without another arrival");
        check(queue.nextRtpSequenceNumber == (uint16_t)(base + 9) && queue.stats.packetCountFecFailed == 0,
              "resynchronization across the lost block preserves ordering without extra concealment");
        RtpaCleanupQueue(&queue);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
