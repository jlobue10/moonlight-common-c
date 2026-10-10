// Exercise the actual decoder/depacketizer with a receive-side flush at the
// queue read boundary. The decoder must own any frame it reads or discards.
#include "../src/Limelight-internal.h"
static int peekWithFlush(PLINKED_BLOCKING_QUEUE queue, void** value);
static int pollWithFlush(PLINKED_BLOCKING_QUEUE queue, void** value);
#define LbqPeekQueueElement peekWithFlush
#define LbqPollQueueElement pollWithFlush
#include "../src/VideoDepacketizer.c"
#undef LbqPeekQueueElement
#undef LbqPollQueueElement
#include "../src/VideoStream.c"

static bool injectFlush;
static int submitted, lastSubmitted;

static int peekWithFlush(PLINKED_BLOCKING_QUEUE queue, void** value) {
    int status = LbqPeekQueueElement(queue, value);
    if (injectFlush && status == LBQ_SUCCESS) {
        injectFlush = false;
        // The receiver is allowed to flush the queue after its bound is hit.
        freeDecodeUnitList(LbqFlushQueueItems(queue));
    }
    return status;
}

static int pollWithFlush(PLINKED_BLOCKING_QUEUE queue, void** value) {
    int status = LbqPollQueueElement(queue, value);
    if (injectFlush && status == LBQ_SUCCESS) {
        injectFlush = false;
        // A removed frame must survive the same receive-side overflow flush.
        freeDecodeUnitList(LbqFlushQueueItems(queue));
    }
    return status;
}

static void enqueueFrame(int number) {
    PQUEUED_DECODE_UNIT qdu = calloc(1, sizeof(*qdu));
    PLENTRY_INTERNAL buffer = calloc(1, sizeof(*buffer) + 1);
    buffer->allocPtr = buffer;
    buffer->entry.data = (char*)(buffer + 1);
    buffer->entry.length = 1;
    buffer->entry.bufferType = BUFFER_TYPE_PICDATA;
    qdu->decodeUnit.bufferList = &buffer->entry;
    qdu->decodeUnit.fullLength = 1;
    qdu->decodeUnit.frameType = FRAME_TYPE_IDR;
    qdu->decodeUnit.frameNumber = number;
    if (LbqOfferQueueItem(&decodeUnitQueue, qdu, &qdu->entry) != LBQ_SUCCESS) abort();
}

static int submitFrame(PDECODE_UNIT unit) {
    ++submitted;
    lastSubmitted = unit->frameNumber;
    stopVideoDepacketizer();
    return DR_OK;
}

int main(int argc, char** argv) {
    bool interFrame = argc > 1 && strcmp(argv[1], "av1") == 0;
    NegotiatedVideoFormat = interFrame ? VIDEO_FORMAT_AV1_MAIN8 : VIDEO_FORMAT_PYROWAVE;
    VideoCallbacks.capabilities = 0;
    VideoCallbacks.submitDecodeUnit = submitFrame;
    initializeVideoDepacketizer(1024);
    enqueueFrame(1);
    enqueueFrame(2);
    enqueueFrame(3);
    bool flush = argc > 1 && !interFrame;
    injectFlush = flush;
    VideoDecoderThreadProc(NULL);
    bool ok = submitted == 1 && lastSubmitted == (interFrame ? 1 : flush ? 2 : 3) &&
              LiGetSkippedVideoFrames() == (interFrame ? 0u : flush ? 1u : 2u);
    printf("%s %s decoder submits frame %d; skip counter %u\n",
           ok ? "PASS" : "FAIL", interFrame ? "AV1 FIFO" : "PyroWave newest-frame",
           lastSubmitted, LiGetSkippedVideoFrames());
    destroyVideoDepacketizer();
    return !ok;
}
