// Exercise the production mouse producers, queue and sender. A mouse batch must
// not consume movement that was submitted after a click, key or scroll event.
#include "../src/Limelight-internal.h"
static int hookSend(unsigned char*, int, uint8_t, uint32_t, bool);
static int flushes;
static void hookFlush(void) { ++flushes; }
static int hookWait(PLINKED_BLOCKING_QUEUE queue, void** data);
#define sendInputPacketOnControlStream hookSend
#define flushInputOnControlStream hookFlush
#define LbqWaitForQueueElement hookWait
#include "../src/InputStream.c"
#undef sendInputPacketOnControlStream
#undef flushInputOnControlStream
#undef LbqWaitForQueueElement
#include <stdio.h>

static struct { uint32_t magic; int x, y; } sent[32];
static int sends, checks, failures;
static bool injectDuringSend, injectAfterPop;
static void check(bool ok, const char* message) {
    ++checks; failures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
}
// The window between the sender popping a mouse holder and latching it under
// batchedInputMutex: a producer running here sees currentQueuedMousePacket still
// naming the popped holder and must not coalesce into it.
static int hookWait(PLINKED_BLOCKING_QUEUE queue, void** data) {
    int err = LbqWaitForQueueElement(queue, data);
    if (err == LBQ_SUCCESS && injectAfterPop) {
        injectAfterPop = false;
        check(LiSendMouseMoveEvent(17, 19) == 0, "movement queued after the pop");
        LbqSignalQueueDrain(&packetQueue);
    }
    return err;
}
static int hookSend(unsigned char* data, int length, uint8_t channel, uint32_t flags, bool more) {
    (void)length; (void)channel; (void)flags; (void)more;
    NV_INPUT_HEADER* header = (NV_INPUT_HEADER*)data;
    if (sends >= 32) return -1;
    sent[sends].magic = LE32(header->magic);
    if (sent[sends].magic == MOUSE_MOVE_REL_MAGIC_GEN5) {
        sent[sends].x = (short)BE16(((PNV_REL_MOUSE_MOVE_PACKET)data)->deltaX);
        sent[sends].y = (short)BE16(((PNV_REL_MOUSE_MOVE_PACKET)data)->deltaY);
    } else if (sent[sends].magic == MOUSE_MOVE_ABS_MAGIC) {
        sent[sends].x = (short)BE16(((PNV_ABS_MOUSE_MOVE_PACKET)data)->x);
        sent[sends].y = (short)BE16(((PNV_ABS_MOUSE_MOVE_PACKET)data)->y);
    }
    ++sends;
    if (injectDuringSend) {
        injectDuringSend = false;
        check(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0, "concurrent click queued");
        check(LiSendMouseMoveEvent(17, 19) == 0, "concurrent movement queued");
        LbqSignalQueueDrain(&packetQueue);
    }
    return 0;
}
static void drain(void) {
    sends = 0;
    flushes = 0;
    memset(sent, 0, sizeof(sent));
    if (!injectDuringSend && !injectAfterPop) LbqSignalQueueDrain(&packetQueue);
    inputSendThreadProc(NULL);
    check(LbqGetItemCount(&packetQueue) == 0, "sender drained all queued input");
    LbqDestroyLinkedBlockingQueue(&packetQueue);
    LbqInitializeLinkedBlockingQueue(&packetQueue, MAX_QUEUED_INPUT_PACKETS);
}
int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    if (initializeInputStream() != 0) return 2;
    initialized = true;

    LiSendMouseMoveEvent(10, 20);
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    LiSendMouseMoveEvent(30, 40);
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    drain();
    check(sends == 4 && sent[0].x == 10 && sent[0].y == 20 &&
          sent[1].magic == MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 && sent[2].x == 30 &&
          sent[2].y == 40 && sent[3].magic == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5,
          "relative drag retains movement on each side of press and release");

    LiSendMousePositionEvent(10, 20, 1920, 1080);
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    LiSendMousePositionEvent(300, 400, 1920, 1080);
    drain();
    check(sends == 3 && sent[0].x == 10 && sent[0].y == 20 &&
          sent[1].magic == MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 && sent[2].x == 300,
          "absolute click stays at its original position before subsequent movement");

    LiSendMouseMoveEvent(10, 20);
    LiSendKeyboardEvent(0xA0, KEY_ACTION_DOWN, 0);
    LiSendMouseMoveEvent(30, 40);
    drain();
    check(sends == 3 && sent[0].x == 10 && sent[1].magic == KEY_ACTION_DOWN && sent[2].x == 30,
          "keyboard modifiers also end a mouse batch");

    LiSendMousePositionEvent(10, 20, 1920, 1080);
    LiSendHighResScrollEvent(120);
    LiSendMousePositionEvent(300, 400, 1920, 1080);
    drain();
    check(sends == 3 && sent[0].x == 10 && sent[1].magic == SCROLL_MAGIC_GEN5 && sent[2].x == 300,
          "scroll stays at its original mouse position");

    LiSendMouseMoveEvent(10, 20);
    LiSendMousePositionEvent(100, 200, 1920, 1080);
    LiSendMouseMoveEvent(30, 40);
    LiSendMousePositionEvent(300, 400, 1920, 1080);
    drain();
    check(sends == 4 && sent[0].x == 10 && sent[1].x == 100 && sent[2].x == 30 && sent[3].x == 300,
          "relative and absolute input retain their interleaving");

    LiSendMouseMoveEvent(10, -20);
    LiSendMouseMoveEvent(30, -40);
    drain();
    check(sends == 1 && sent[0].x == 40 && sent[0].y == -60, "adjacent relative movement still coalesces");
    LiSendMousePositionEvent(10, 20, 1920, 1080);
    LiSendMousePositionEvent(300, 400, 1920, 1080);
    drain();
    check(sends == 1 && sent[0].x == 300 && sent[0].y == 400, "adjacent absolute movement keeps the newest position");

    LiSendMouseMoveEvent(30000, -30000);
    LiSendMouseMoveEvent(30000, -30000);
    drain();
    check(sends == 2 && sent[0].x + sent[1].x == 60000 && sent[0].y + sent[1].y == -60000,
          "large relative batches split without losing movement");

    LiSendMouseMoveEvent(5, 7);
    injectDuringSend = true;
    drain();
    check(sends == 3 && sent[0].x == 5 && sent[1].magic == MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 && sent[2].x == 17,
          "input arriving during network I/O cannot extend the latched mouse batch");

    LiSendMouseMoveEvent(5, 7);
    injectAfterPop = true;
    drain();
    check(sends == 2 && sent[0].x == 5 && sent[0].y == 7 && sent[1].x == 17 && sent[1].y == 19,
          "movement arriving between the pop and the latch goes into a new holder");

    // A batch that coalesces to nothing sends no packet; the click queued before it
    // went out with moreData set and must still be flushed.
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    LiSendMouseMoveEvent(5, 0);
    LiSendMouseMoveEvent(-5, 0);
    drain();
    check(sends == 1 && sent[0].magic == MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 && flushes >= 1,
          "a zero-delta batch flushes the input that preceded it");

    packetQueue.sizeBound = 1;
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    check(LiSendMouseMoveEvent(9, 11) == LBQ_BOUND_EXCEEDED, "full queue rejects a new mouse holder");
    drain();
    check(LiSendMouseMoveEvent(13, 15) == 0, "a rejected holder does not poison the next batch");
    drain();
    check(sends == 1 && sent[0].x == 13 && sent[0].y == 15, "retry contains only accepted movement");

    // Relative-as-absolute motion must invert the ref - 1 normalization. A
    // one-pixel negative move must escape the right/bottom edges immediately.
    LiSendMousePositionEvent(1919, 1079, 1920, 1080);
    LiSendMouseMoveAsMousePositionEvent(-1, -1, 1920, 1080);
    drain();
    check(sends == 1 && sent[0].x == 1918 && sent[0].y == 1078,
          "single-pixel motion leaves the right and bottom edges");
    LiSendMousePositionEvent(1919, 1079, 1920, 1080);
    for (int i = 0; i < 20; ++i) LiSendMouseMoveAsMousePositionEvent(-1, -1, 1920, 1080);
    drain();
    check(sends == 1 && sent[0].x == 1899 && sent[0].y == 1059,
          "repeated fine motion does not stick at the far edges");
    bool stable = true;
    for (int x = 0; x < 32767; ++x) {
        LiSendMousePositionEvent((short)x, (short)x, 32767, 32767);
        LiSendMouseMoveAsMousePositionEvent(0, 0, 32767, 32767);
        stable &= BE16(currentQueuedMousePacket->packet.mouseMoveAbs.x) == x &&
                  BE16(currentQueuedMousePacket->packet.mouseMoveAbs.y) == x;
    }
    drain();
    check(stable, "zero motion preserves every position on the largest reference plane");
    LiSendMousePositionEvent(1919, 1079, 1920, 1080);
    LiSendMouseMoveAsMousePositionEvent(-1, -1, 3840, 2160);
    drain();
    check(sends == 1 && sent[0].x == 3838 && sent[0].y == 2158,
          "dimension changes retain the normalized far edge before applying motion");
    LiSendMousePositionEvent(0, 0, 1920, 1080);
    LiSendMouseMoveAsMousePositionEvent(-32768, -32768, 1920, 1080);
    drain();
    check(sends == 1 && sent[0].x == 0 && sent[0].y == 0, "motion still clamps to the near edges");
    check(LiSendMouseMoveAsMousePositionEvent(1, 1, 1, 1080) == -3 &&
          LiSendMouseMoveAsMousePositionEvent(1, 1, 1920, 0) == -3 &&
          LbqGetItemCount(&packetQueue) == 0, "degenerate relative reference planes queue no input");
    check(LiSendMousePositionEvent(1, 1, 1, 1) == -3 &&
          LiSendMousePositionEvent(0, 0, 0, 0) == -3 &&
          LiSendMousePositionEvent(5, 5, 1920, -1) == -3 &&
          LiSendMousePositionEvent(5, 5, -1920, 1080) == -3 &&
          LbqGetItemCount(&packetQueue) == 0, "degenerate absolute reference planes queue no input");
    check(LiSendMousePositionEvent(1, 1, 2, 2) == 0, "the smallest valid plane is accepted");
    drain();
    check(sends == 1 && sent[0].x == 1 && sent[0].y == 1, "the far corner of a 2x2 plane is sent as-is");
    // The rejections above must not have moved the virtual cursor off the plane.
    LiSendMouseMoveAsMousePositionEvent(0, 0, 1920, 1080);
    drain();
    check(sends == 1 && sent[0].x >= 0 && sent[0].x <= 1919 && sent[0].y >= 0 && sent[0].y <= 1079,
          "the virtual cursor stays finite and on the plane after rejected calls");

    initialized = false;
    LbqSignalQueueShutdown(&packetHolderFreeList);
    LbqSignalQueueShutdown(&packetQueue);
    destroyInputStream();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
