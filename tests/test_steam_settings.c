// Exercise the control callback parser for Steam Controller settings writes (0x5505),
// including every wire truncation, every command/count combination, and queue ordering.
#include "../src/ControlStream.c"

#define WIRE (sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 4 + STEAM_SETTINGS_PAYLOAD_SIZE)

static void send_settings(unsigned controller, unsigned command, unsigned count, unsigned first_id) {
    unsigned char bytes[WIRE] = {0};
    PNVCTL_ENET_PACKET_HEADER_V1 hdr = (PNVCTL_ENET_PACKET_HEADER_V1)bytes;
    hdr->type = packetTypes[IDX_STEAM_SETTINGS];
    bytes[2] = (unsigned char)controller;
    bytes[4] = (unsigned char)command;
    bytes[5] = (unsigned char)count;
    for (unsigned i = 0; i < count && i < STEAM_SETTINGS_TRIPLES_MAX; ++i) {
        bytes[6 + i * 3] = (unsigned char)(first_id + i);
        bytes[7 + i * 3] = 0x34;
        bytes[8 + i * 3] = 0x12;
    }
    queueAsyncCallback(hdr, sizeof(bytes));
}

int main(void) {
    union {
        NVCTL_ENET_PACKET_HEADER_V1 header;
        unsigned char bytes[WIRE];
    } packet = {0};
    // A union's {0} initializer only covers its first member; the rest is unspecified.
    memset(&packet, 0, sizeof(packet));
    packetTypes = (short*)packetTypesGen7Enc;
    packet.header.type = packetTypes[IDX_STEAM_SETTINGS];
    packet.bytes[2] = 7;
    int failures = 0;
    if (LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 256) != 0) return 2;

    // Every command byte, every count, every length: only a full-size packet carrying 0x87
    // with 1..9 triples or 0x8E with none is admitted, and what arrives is what was sent.
    int admitted = 0;
    static const int counts[] = {0, 1, 5, 9, 10, 255};
    for (int command = 0; command <= 255; ++command) {
        for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
            const int count = counts[c];
            packet.bytes[4] = (unsigned char)command;
            packet.bytes[5] = (unsigned char)count;
            packet.bytes[6] = 48; packet.bytes[7] = 0x18; packet.bytes[8] = 0;
            // Every length for the two valid commands, the boundaries for the rest.
            const bool sweep = command == STEAM_SETTINGS_SET_VALUES || command == STEAM_SETTINGS_LOAD_DEFAULTS;
            for (size_t length = sizeof(packet.header); length <= sizeof(packet.bytes);
                 length = sweep ? length + 1 : (length == sizeof(packet.header) ? sizeof(packet.bytes) - 1 : length + 1)) {
                void* value = NULL;
                queueAsyncCallback(&packet.header, (int)length);
                bool received = LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS;
                bool expected = length == sizeof(packet.bytes) &&
                    ((command == STEAM_SETTINGS_SET_VALUES && count >= 1 && count <= STEAM_SETTINGS_TRIPLES_MAX) ||
                     (command == STEAM_SETTINGS_LOAD_DEFAULTS && count == 0));
                if (received != expected) failures++;
                if (received && expected) {
                    PQUEUED_ASYNC_CALLBACK cb = value;
                    ++admitted;
                    if (cb->typeIndex != IDX_STEAM_SETTINGS || cb->data.steamSettings.controllerNumber != 7 ||
                        cb->data.steamSettings.command != command || cb->data.steamSettings.count != count ||
                        cb->data.steamSettings.triples[0] != 48 || cb->data.steamSettings.triples[1] != 0x18) failures++;
                    if (isCallbackState(cb)) failures++;  // never coalesced
                }
                free(value);
            }
        }
    }
    printf("%s settings parser admits exactly the valid command/count pairs (%d admitted)\n",
           admitted == 4 ? "PASS" : "FAIL", admitted);
    failures += admitted != 4;  // 0x87 with 1, 5, 9 triples and 0x8E with none

    // A 20-triple write arrives as three chunks; they stay whole, in order, and are not
    // merged with each other or with a state callback for the same controller.
    {
        PQUEUED_ASYNC_CALLBACK rumble = calloc(1, sizeof(*rumble));
        if (rumble == NULL) return 2;
        rumble->typeIndex = IDX_RUMBLE_DATA; rumble->data.rumble.controllerNumber = 3;
        void* old = NULL;
        if (LbqOfferQueueItemPriority(&asyncCallbackQueue, rumble, &rumble->entry, sameCallbackState, isCallbackState, 32, &old) != LBQ_SUCCESS) { free(rumble); ++failures; }
        free(old);
        send_settings(3, STEAM_SETTINGS_SET_VALUES, 9, 0);
        send_settings(3, STEAM_SETTINGS_SET_VALUES, 9, 9);
        send_settings(3, STEAM_SETTINGS_SET_VALUES, 2, 18);
        send_settings(3, STEAM_SETTINGS_LOAD_DEFAULTS, 0, 0);
        int order[8] = {0}, n = 0;
        void* value = NULL;
        while (LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS) {
            PQUEUED_ASYNC_CALLBACK cb = value;
            if (n < 8) order[n] = cb->typeIndex == IDX_STEAM_SETTINGS ? (cb->data.steamSettings.command == STEAM_SETTINGS_LOAD_DEFAULTS ? 100 : cb->data.steamSettings.triples[0]) : -1;
            ++n; free(value);
        }
        bool ordered = n == 5 && order[0] == -1 && order[1] == 0 && order[2] == 9 && order[3] == 18 && order[4] == 100;
        printf("%s chunked settings write is delivered whole and in order after pending state (%d callbacks)\n", ordered ? "PASS" : "FAIL", n);
        failures += !ordered;
    }

    // Settings are transient events: a flood of them is bounded like other events, and a
    // pending haptic stop (state) survives it.
    {
        unsigned char bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 3 + STEAM_HAPTIC_REPORT_MAX] = {0};
        PNVCTL_ENET_PACKET_HEADER_V1 hdr = (PNVCTL_ENET_PACKET_HEADER_V1)bytes;
        hdr->type = packetTypes[IDX_STEAM_HAPTIC];
        bytes[2] = 1; bytes[4] = 8; bytes[5] = 0x81; bytes[6] = 0;
        queueAsyncCallback(hdr, sizeof(bytes));
        for (unsigned i = 0; i < 1000; ++i) send_settings(1, STEAM_SETTINGS_SET_VALUES, 1, i & 0x7F);
        int total = 0, states = 0;
        void* value = NULL;
        while (LbqPollQueueElement(&asyncCallbackQueue, &value) == LBQ_SUCCESS) {
            ++total; if (isCallbackState(value)) ++states; free(value);
        }
        bool bounded = states == 1 && total <= 33;
        printf("%s settings flood is bounded (%d delivered) and keeps the pending haptic stop\n", bounded ? "PASS" : "FAIL", total);
        failures += !bounded;
    }

    LbqSignalQueueShutdown(&asyncCallbackQueue);
    LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue);
    printf("Steam settings parser: %d failures\n", failures);
    return failures != 0;
}
