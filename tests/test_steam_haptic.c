// Exercise the actual control callback parser, including every wire truncation.
#include "../src/ControlStream.c"

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
    if (LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 4) != 0) return 2;
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
    LbqSignalQueueShutdown(&asyncCallbackQueue);
    LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue);
    printf("Steam haptic parser: 4096 length/truncation cases, %d failures\n", failures);
    return failures != 0;
}
