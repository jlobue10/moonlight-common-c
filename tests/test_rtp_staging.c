// Production FEC block staging: ordering, parity disposal, wrap and large blocks.
// Optional --benchmark measures only staging, excluding packet allocation.
#include "Limelight-internal.h"
#undef LC_DEBUG
#include "RtpVideoQueue.c"
#include <time.h>

static uint32_t randomState = 0x12c0ffee;
static uint32_t nextRandom(void) {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

static void build(RTP_VIDEO_QUEUE* queue, unsigned int count, unsigned int parity,
                  unsigned int base, int order) {
    unsigned int indices[1100];
    memset(queue, 0, sizeof(*queue));
    queue->bufferLowestSequenceNumber = base;
    queue->bufferDataPackets = count;
    queue->bufferFirstRecvTimeUs = 123456;
    for (unsigned int i = 0; i < count + parity; i++) indices[i] = i;
    if (order == 1) {
        for (unsigned int i = 0; i < (count + parity) / 2; i++) {
            unsigned int t = indices[i];
            indices[i] = indices[count + parity - 1 - i];
            indices[count + parity - 1 - i] = t;
        }
    }
    else if (order == 2) {
        for (unsigned int i = count + parity - 1; i > 0; i--) {
            unsigned int j = nextRandom() % (i + 1), t = indices[i];
            indices[i] = indices[j]; indices[j] = t;
        }
    }
    else if (order == 3 && count > 2 && !parity) {
        // Early data shard reconstructed by FEC is appended after the others.
        for (unsigned int i = 1; i + 1 < count; i++) indices[i] = i + 1;
        indices[count - 1] = 1;
    }
    for (unsigned int i = 0; i < count + parity; i++) {
        struct test_packet { RTP_PACKET packet; RTPV_QUEUE_ENTRY entry; };
        struct test_packet* allocation = calloc(1, sizeof(*allocation));
        assert(allocation);
        PRTP_PACKET packet = &allocation->packet;
        PRTPV_QUEUE_ENTRY entry = &allocation->entry;
        packet->sequenceNumber = U16(base + indices[i]);
        entry->packet = packet;
        entry->isParity = indices[i] >= count;
        insertEntryIntoList(&queue->pendingFecBlockList, entry);
    }
}

static void verify(RTP_VIDEO_QUEUE* queue, unsigned int count, unsigned int base) {
    assert(queue->pendingFecBlockList.count == 0);
    assert(queue->pendingFecBlockList.head == NULL && queue->pendingFecBlockList.tail == NULL);
    assert(queue->completedFecBlockList.count == count);
    PRTPV_QUEUE_ENTRY entry = queue->completedFecBlockList.head, prev = NULL;
    for (unsigned int i = 0; i < count; i++) {
        assert(entry && !entry->isParity && entry->packet->sequenceNumber == U16(base + i));
        assert(entry->prev == prev && entry->receiveTimeUs == 123456);
        prev = entry; entry = entry->next;
    }
    assert(entry == NULL && prev == queue->completedFecBlockList.tail);
}

static uint64_t nowNs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

int main(int argc, char** argv) {
    RTP_VIDEO_QUEUE queue;
    unsigned int cases = 0;
    const unsigned int sizes[] = {1, 2, 31, 255, 1000, 1023};
    const unsigned int bases[] = {100, 65530};
    for (unsigned int size = 0; size < 6; size++) {
        for (unsigned int base = 0; base < 2; base++) {
            for (int order = 0; order < 4; order++) {
                for (unsigned int parity = 0; parity <= 12; parity += 12) {
                    build(&queue, sizes[size], parity, bases[base], order);
                    stageCompleteFecBlock(&queue);verify(&queue, sizes[size], bases[base]);
                    purgeListEntries(&queue.completedFecBlockList);cases++;
                }
            }
        }
    }
    printf("%u staging cases passed\n", cases);
    if (argc > 1 && strcmp(argv[1], "--benchmark") == 0) {
        const char* names[] = {"ordered", "reversed", "shuffled", "one-recovered"};
        for (int order = 0; order < 4; order++) {
            uint64_t total = 0;
            for (int i = 0; i < 1000; i++) {
                build(&queue, 1000, 0, 65530, order);
                uint64_t start = nowNs();stageCompleteFecBlock(&queue);total += nowNs() - start;
                verify(&queue, 1000, 65530);purgeListEntries(&queue.completedFecBlockList);
            }
            printf("%s: %.1f ns/block (1000 data shards, 1000 trials)\n", names[order], total / 1000.0);
        }
    }
    return 0;
}
