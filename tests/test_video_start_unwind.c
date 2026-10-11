// A receiver can queue a frame before decoder-thread creation fails. Exercise
// the real start/destroy paths with that ordering, including queue assertions.
#include "../src/Limelight-internal.h"
static SOCKET testBind(int family, struct sockaddr_storage* local, SOCKADDR_LEN len, int size, int qos);
static int testCreateThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread);
static void testInterruptThread(PLT_THREAD* thread);
static void testJoinThread(PLT_THREAD* thread);
#include "../src/VideoDepacketizer.c"
#define bindUdpSocket testBind
#define PltCreateThread testCreateThread
#define PltInterruptThread testInterruptThread
#define PltJoinThread testJoinThread
#include "../src/VideoStream.c"
#undef bindUdpSocket
#undef PltCreateThread
#undef PltInterruptThread
#undef PltJoinThread

static int starts, stops, cleanups, interrupted, joined;
static SOCKET testBind(int family, struct sockaddr_storage* local, SOCKADDR_LEN len, int size, int qos) {
    (void)family; (void)local; (void)len; (void)size; (void)qos;
    return socket(AF_INET, SOCK_DGRAM, 0);
}
static int testCreateThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread) {
    (void)entry; (void)context; (void)thread;
    if (strcmp(name, "VideoRecv") == 0) {
        PQUEUED_DECODE_UNIT unit = calloc(1, sizeof(*unit));
        if (!unit || LbqOfferQueueItem(&decodeUnitQueue, unit, &unit->entry) != LBQ_SUCCESS) abort();
        return 0;
    }
    if (strcmp(name, "VideoDec") != 0) abort();
    return EAGAIN;
}
static void testInterruptThread(PLT_THREAD* thread) { (void)thread; ++interrupted; }
static void testJoinThread(PLT_THREAD* thread) { (void)thread; ++joined; }
static int setup(int format, int width, int height, int fps, void* context, int flags) {
    (void)format; (void)width; (void)height; (void)fps; (void)context; (void)flags;
    return 0;
}
static void start(void) { ++starts; }
static void stop(void) { ++stops; }
static void cleanup(void) { ++cleanups; }

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    StreamConfig.packetSize = 1024;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    VideoCallbacks.setup = setup;
    VideoCallbacks.start = start;
    VideoCallbacks.stop = stop;
    VideoCallbacks.cleanup = cleanup;
    VideoCallbacks.capabilities = 0;
    if (initializeVideoStream() != 0) return 1;
    int err = startVideoStream(NULL, 0);
    int ok = err == EAGAIN && decodeUnitQueue.shutdown &&
             starts == 1 && stops == 1 && cleanups == 1 && interrupted == 1 && joined == 1;
    printf("%s failed decoder creation shuts down the populated queue and joins the receiver\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    // The connection never marked video as started: destroy is the only later
    // cleanup. The baseline asserts here on the populated, unretired queue.
    destroyVideoStream();
    printf("PASS failed-start destruction frees the queued frame without an assertion\n");
    if (initializeVideoStream() != 0) return 1;
    destroyVideoStream();
    printf("PASS a fresh video initializer still succeeds after failure cleanup\n");
    return !ok;
}
