// Exercise the production audio RTP/FEC queue: in-order delivery, recovery of a
// lost data shard from a parity shard, and the bound on how far ahead a parity
// shard may open a block (an unauthenticated far-future base used to make the
// queue abandon the current block at once).
#include "../src/RtpAudioQueue.c"

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

#define BLOCK 64
static RTP_AUDIO_QUEUE queue;
static unsigned char storage[64][sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + BLOCK];
static int next;

static unsigned char* payloadOf(uint16_t seq, unsigned char* out) {
    for (int i = 0; i < BLOCK; i++) out[i] = (unsigned char)(seq * 7 + i);
    return out;
}

static int addData(uint16_t seq) {
    PRTP_PACKET rtp = (PRTP_PACKET)storage[next++ % 64];
    rtp->header = 0x80;
    rtp->packetType = RTP_PAYLOAD_TYPE_AUDIO;
    rtp->sequenceNumber = seq;
    rtp->timestamp = seq * AudioPacketDuration;
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
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
