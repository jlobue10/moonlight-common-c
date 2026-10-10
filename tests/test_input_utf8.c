// Exercise the production UTF-8 text chunker on the input send thread: one packet per
// code point, the last one flushing the batch, and a truncated trailing sequence
// never copied past the text.
#include "../src/Limelight-internal.h"
static int hookSend(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData);
static void hookFlush(void);
static bool hookInTransit(void);
#define sendInputPacketOnControlStream hookSend
#define flushInputOnControlStream hookFlush
#define isControlDataInTransit hookInTransit
#include "../src/InputStream.c"
#undef sendInputPacketOnControlStream
#undef flushInputOnControlStream
#undef isControlDataInTransit
#include <stdio.h>

static int sends, lengths[16], more[16], flushes;
static int hookSend(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData) {
    (void)data; (void)channelId; (void)flags;
    if (sends < 16) { lengths[sends] = length; more[sends] = moreData; }
    ++sends;
    return 0;
}
static void hookFlush(void) { ++flushes; }
static bool hookInTransit(void) { return false; }

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

static void runText(const char* text, int length) {
    sends = flushes = 0;
    PPACKET_HOLDER holder = allocatePacketHolder(length);
    holder->packet.unicode.header.size = BE32(sizeof(uint32_t) + length);
    holder->packet.unicode.header.magic = LE32(UTF8_TEXT_EVENT_MAGIC);
    memcpy(holder->packet.unicode.text, text, length);
    LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    LbqSignalQueueDrain(&packetQueue);
    inputSendThreadProc(NULL);
    // re-arm the queue for the next case
    LbqDestroyLinkedBlockingQueue(&packetQueue);
    LbqInitializeLinkedBlockingQueue(&packetQueue, MAX_QUEUED_INPUT_PACKETS);
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, 0}, sizeof(AppVersionQuad));  // encrypted control stream: packets go through the hook
    if (initializeInputStream() != 0) return 2;
    initialized = true;

    runText("a\xC3\xA9", 3);  // "aé"
    check(sends == 2 && flushes == 1, "two code points produce two packets");
    check(lengths[1] == lengths[0] + 1, "a two-byte code point adds one byte to the packet");
    check(more[0] == 1 && more[1] == 0, "the last code point flushes the batch");

    runText("\xE2\x82\xAC", 3);  // euro sign alone
    check(sends == 1 && more[0] == 0, "a single multi-byte code point is sent once and flushed");

    runText("a\xE2\x82", 3);  // truncated 3-byte sequence
    check(sends == 1 && more[0] == 1, "a truncated trailing code point is not sent");

    runText("\xC3", 1);
    check(sends == 0, "a lone lead byte sends nothing");

    runText("\xFF" "b", 2);
    check(sends == 0, "an invalid lead byte stops the chunker");

    initialized = false;
    LbqSignalQueueShutdown(&packetHolderFreeList);
    LbqSignalQueueShutdown(&packetQueue);
    destroyInputStream();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
