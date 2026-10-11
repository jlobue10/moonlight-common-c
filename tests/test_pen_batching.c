// Execute production pen admission and batching. Button edges must retain reliable
// delivery when later motion replaces their coordinates or queue admission fails.
#include "../src/Limelight-internal.h"
static int hookSend(unsigned char*, int, uint8_t, uint32_t, bool);
static void hookFlush(void) {}
#define flushInputOnControlStream hookFlush
#define sendInputPacketOnControlStream hookSend
#include "../src/InputStream.c"
#undef sendInputPacketOnControlStream
#undef flushInputOnControlStream
#include <stdio.h>

static SS_PEN_PACKET sent[8];
static uint32_t sentFlags[8];
static int sends, checks, failures;
static int hookSend(unsigned char* data, int length, uint8_t channel, uint32_t flags, bool more) {
    (void)channel; (void)more;
    if (length == sizeof(sent[0]) && sends < 8) {
        memcpy(&sent[sends], data, length);
        sentFlags[sends++] = flags;
    }
    return 0;
}
static void check(bool ok, const char* text) {
    ++checks; failures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}
static int pen(uint8_t type, uint8_t buttons) {
    return LiSendPenEvent(type, LI_TOOL_TYPE_PEN, buttons, .5f, .5f, 0, 0, 0, 0, 0);
}
static void drain(void) {
    sends = 0;
    memset(sentFlags, 0, sizeof(sentFlags));
    LbqSignalQueueDrain(&packetQueue);
    inputSendThreadProc(NULL);
    LbqDestroyLinkedBlockingQueue(&packetQueue);
    LbqInitializeLinkedBlockingQueue(&packetQueue, MAX_QUEUED_INPUT_PACKETS);
}
int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    SunshineFeatureFlags = LI_FF_PEN_TOUCH_EVENTS;
    if (initializeInputStream() != 0) return 2;
    initialized = true;

    pen(LI_TOUCH_EVENT_MOVE, 1);pen(LI_TOUCH_EVENT_MOVE, 1);drain();
    check(sends == 1 && sent[0].penButtons == 1 && (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE),
          "coalesced pen button press retains reliable delivery");
    pen(LI_TOUCH_EVENT_MOVE, 0);pen(LI_TOUCH_EVENT_MOVE, 0);drain();
    check(sends == 1 && sent[0].penButtons == 0 && (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE),
          "coalesced pen button release retains reliable delivery");
    pen(LI_TOUCH_EVENT_MOVE, 0);pen(LI_TOUCH_EVENT_MOVE, 0);drain();
    check(sends == 1 && sentFlags[0] == 0, "ordinary motion remains batchable and unreliable");
    pen(LI_TOUCH_EVENT_HOVER, 1);pen(LI_TOUCH_EVENT_HOVER, 1);drain();
    check(sends == 1 && (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE), "hover button edges retain reliable delivery");
    pen(LI_TOUCH_EVENT_HOVER, 0);pen(LI_TOUCH_EVENT_HOVER, 1);drain();
    check(sends == 2 && sent[0].penButtons == 0 && sent[1].penButtons == 1 &&
          (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE) && (sentFlags[1] & ENET_PACKET_FLAG_RELIABLE),
          "opposite edges are delivered separately in order");

    packetQueue.sizeBound = 0;
    check(pen(LI_TOUCH_EVENT_MOVE, 0) == LBQ_BOUND_EXCEEDED, "full queue rejects release admission");
    check(currentPenButtonState == 1, "rejected release does not advance admitted button state");
    packetQueue.sizeBound = MAX_QUEUED_INPUT_PACKETS;
    pen(LI_TOUCH_EVENT_MOVE, 0);drain();
    check(sends == 1 && (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE), "release retry is still reliable after backpressure");
    pen(LI_TOUCH_EVENT_DOWN, 0);pen(LI_TOUCH_EVENT_UP, 0);drain();
    check(sends == 2 && (sentFlags[0] & ENET_PACKET_FLAG_RELIABLE) && (sentFlags[1] & ENET_PACKET_FLAG_RELIABLE),
          "contact transitions remain unbatched and reliable");

    initialized = false;
    LbqSignalQueueShutdown(&packetQueue);
    LbqSignalQueueShutdown(&packetHolderFreeList);
    destroyInputStream();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
