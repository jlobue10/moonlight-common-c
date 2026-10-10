// Run the actual feedback parser against every truncated payload prefix.
#include "../src/ControlStream.c"

int main(void) {
    union {
        NVCTL_ENET_PACKET_HEADER_V1 header;
        unsigned char bytes[128];
    } packet = {0};
    packetTypes = (short*)packetTypesGen7Enc;
    const struct { int kind, size; } cases[] = {
        {IDX_RUMBLE_DATA, 10}, {IDX_RUMBLE_TRIGGER_DATA, 6},
        {IDX_SET_MOTION_EVENT, 5}, {IDX_SET_RGB_LED, 5},
        {IDX_DS_ADAPTIVE_TRIGGERS, 5 + 2 * DS_EFFECT_PAYLOAD_SIZE},
        {IDX_HDR_INFO, 1}
    };
    int failures = 0, tested = 0;
    if (LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 256) != 0) return 2;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int errors = 0;
        for (int payload = 0; payload <= cases[i].size + 4; ++payload) {
            memset(packet.bytes, 0x11, sizeof(packet.bytes));
            packet.header.type = packetTypes[cases[i].kind];
            queueAsyncCallback(&packet.header, sizeof(packet.header) + payload);
            void* value = NULL;
            bool received = LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS;
            errors += received != (payload >= cases[i].size);
            if (received) errors += ((PQUEUED_ASYNC_CALLBACK)value)->typeIndex != cases[i].kind;
            free(value);
            ++tested;
        }
        failures += errors;
        printf("%s callback type %d rejects every truncated prefix and accepts full/padded payloads (%d errors)\n",
               errors ? "FAIL" : "PASS", cases[i].kind, errors);
    }
    // An absent HDR enable byte must not disable an already active display.
    SS_HDR_METADATA before = {0};
    before.maxDisplayLuminance = 1000;
    publishHdrState(true, &before);
    updateHdrState(&packet.header, sizeof(packet.header));
    bool unchanged = LiGetCurrentHostDisplayHdrMode();
    printf("%s empty HDR payload does not overwrite published state\n", unchanged ? "PASS" : "FAIL");
    failures += !unchanged;
    LbqSignalQueueShutdown(&asyncCallbackQueue);
    LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue);
    printf("Control payloads: %d cases, %d failures\n", tested, failures);
    return failures != 0;
}
