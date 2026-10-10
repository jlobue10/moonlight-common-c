// Exercise the actual control callback parser, including every wire truncation.
#include "../src/ControlStream.c"

static void send_haptic(unsigned controller, unsigned type, unsigned side, bool stop) {
    unsigned char bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 3 + STEAM_HAPTIC_REPORT_MAX] = {0};
    PNVCTL_ENET_PACKET_HEADER_V1 hdr = (PNVCTL_ENET_PACKET_HEADER_V1)bytes;
    hdr->type = packetTypes[IDX_STEAM_HAPTIC];
    bytes[2] = (unsigned char)controller; bytes[4] = 12; bytes[5] = (unsigned char)type; bytes[6] = (unsigned char)side;
    if (!stop) { bytes[7] = 3; bytes[9] = 1; bytes[11] = 1; bytes[12] = 1; }
    queueAsyncCallback(hdr, sizeof(bytes));
}

static void consume_test_callbacks(void* context) {
    unsigned* consumed = context;
    PQUEUED_ASYNC_CALLBACK current, next;
    while (LbqWaitForQueueElement(&asyncCallbackQueue, (void**)&current) == LBQ_SUCCESS) {
        while (LbqPollQueueElementMatching(&asyncCallbackQueue, (void**)&next,
                canBatchAsyncCallback, current) == LBQ_SUCCESS) {
            free(current); current = next;
        }
        ++*consumed;
        free(current);
    }
}

int main(void) {
    union {
        NVCTL_ENET_PACKET_HEADER_V1 header;
        unsigned char bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 3 + STEAM_HAPTIC_REPORT_MAX];
    } packet = {0};
    packetTypes = (short*)packetTypesGen7Enc;
    packet.header.type = packetTypes[IDX_STEAM_HAPTIC];
    packet.bytes[2] = 7;
    packet.bytes[5] = 0x81;
    int failures = 0;
    if (LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 128) != 0) return 2;
    for (int declared = 0; declared <= 255; ++declared) {
        packet.bytes[4] = (unsigned char)declared;
        for (int length = sizeof(packet.header); length <= sizeof(packet.bytes); ++length) {
            void* value = NULL;
            queueAsyncCallback(&packet.header, length);
            bool received = LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS;
            bool expected = length == sizeof(packet.bytes) && declared >= 2 && declared <= STEAM_HAPTIC_REPORT_MAX;
            if (received != expected) failures++;
            if (received && expected) {
                PQUEUED_ASYNC_CALLBACK cb = value;
                if (cb->data.steamHaptic.controllerNumber != 7 ||
                    cb->data.steamHaptic.length != declared || cb->data.steamHaptic.report[0] != 0x81) failures++;
            }
            free(value);
        }
    }
    for (int i = 0; i < 128; ++i) send_haptic(15, 0x81, 0, false);
    for (int id = 0; id < 16; ++id) {
        send_haptic(id, 0x80, 0, true);
        for (int type = 0x81; type <= 0x82; ++type) {
            for (int side = 0; side < 3; ++side) send_haptic(id, type, side, true);
        }
    }
    for (int i = 0; i < 1000; ++i) send_haptic(15, 0x81, 0, false);
    int stops = 0;
    void* value = NULL;
    while (LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS) {
        PQUEUED_ASYNC_CALLBACK cb = value;
        const unsigned char* report = cb->data.steamHaptic.report;
        if (report[2] == 0 && report[4] == 0 && report[6] == 0 && report[7] == 0) ++stops;
        free(value);
    }
    printf("%s full callback queue admits all 112 family/actuator stops (%d delivered)\n", stops == 112 ? "PASS" : "FAIL", stops);
    failures += stops != 112;
    send_haptic(0, 0x82, 1, true); send_haptic(0, 0x82, 1, false); send_haptic(0, 0x82, 1, true);
    int count = 0, commands[3] = {-1, -1, -1};
    while (LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS) {
        if (count < 3) commands[count] = ((PQUEUED_ASYNC_CALLBACK)value)->data.steamHaptic.report[2];
        ++count; free(value);
    }
    bool ordered = count == 2 && commands[0] == 3 && commands[1] == 0;
    printf("%s coalesced callback stop follows the intervening start\n", ordered ? "PASS" : "FAIL");
    failures += !ordered;
    unsigned consumed = 0;
    PLT_THREAD consumer;
    if (PltCreateThread("QueueTest", consume_test_callbacks, &consumed, &consumer) != 0) return 2;
    for (unsigned i = 0; i < 20000; ++i) {
        PQUEUED_ASYNC_CALLBACK cb = calloc(1, sizeof(*cb));
        if (cb == NULL) return 2;
        cb->typeIndex = IDX_RUMBLE_DATA;
        cb->data.rumble.controllerNumber = i % 16;
        if (LbqOfferQueueItem(&asyncCallbackQueue, cb, &cb->entry) != LBQ_SUCCESS) free(cb);
        send_haptic(i % 16, 0x82, i % 3, true);
    }
    LbqSignalQueueDrain(&asyncCallbackQueue);
    PltJoinThread(&consumer);
    printf("%s concurrent priority insertion and atomic batching (20000 iterations, %u consumed)\n", consumed ? "PASS" : "FAIL", consumed);
    failures += consumed == 0;
    LbqSignalQueueShutdown(&asyncCallbackQueue);
    LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue);
    printf("Steam haptic parser: 4096 length/truncation cases, %d failures\n", failures);
    return failures != 0;
}
