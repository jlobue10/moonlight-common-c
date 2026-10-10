// Actual packet parser/getters, with concurrent complete metadata snapshots.
#include "../src/ControlStream.c"
#include <stdatomic.h>

static atomic_bool writerStarted;
static void hdr_writer(void* unused) {
    (void)unused;
    unsigned char bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 1 + sizeof(SS_HDR_METADATA)] = {0};
    bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1)] = 1;
    for (unsigned i = 0; i < 200000; ++i) {
        unsigned char v = (i & 1) ? 0x42 : 0x51;
        memset(bytes + sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 1, v, sizeof(SS_HDR_METADATA));
        updateHdrState((PNVCTL_ENET_PACKET_HEADER_V1)bytes, sizeof(bytes));
        if (i == 0) atomic_store(&writerStarted, true);
    }
}

int main(void) {
    AppVersionQuad[3] = -1;
    // Queries before any connection must remain safe; no initialized/destroyed mutex.
    if (LiGetCurrentHostDisplayHdrMode()) return 2;
    PLT_THREAD writer;
    if (PltCreateThread("HdrWriter", hdr_writer, NULL, &writer) != 0) return 2;
    while (!atomic_load(&writerStarted)) PltSleepMs(0);
    unsigned reads = 0, mixed = 0;
    for (unsigned i = 0; i < 200000; ++i) {
        SS_HDR_METADATA metadata;
        if (!LiGetHdrMetadata(&metadata)) continue;
        unsigned char bytes[sizeof(metadata)];
        memcpy(bytes, &metadata, sizeof(bytes));
        bool consistent = true;
        for (unsigned j = 0; j < sizeof(bytes); ++j) consistent &= bytes[j] == bytes[0];
        mixed += !consistent;
        ++reads;
    }
    PltJoinThread(&writer);
    printf("%s HDR publication: %u snapshots, %u mixed snapshots (200000 writes)\n", reads && !mixed ? "PASS" : "FAIL", reads, mixed);
    return !reads || mixed != 0;
}
