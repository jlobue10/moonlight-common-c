// Inject resource failures into the actual stream initializers, including the
// audio FEC queue and video depacketizer. Track ownership so a failed initializer
// must release exactly what it acquired, then permit a healthy retry.
#include "../src/Limelight-internal.h"
#include <rs.h>
#include <sys/wait.h>
#include <unistd.h>

static int createQueue(PLINKED_BLOCKING_QUEUE queue, int bound);
static PLINKED_BLOCKING_QUEUE_ENTRY destroyQueue(PLINKED_BLOCKING_QUEUE queue);
static int createMutex(PLT_MUTEX* mutex);
static void destroyMutex(PLT_MUTEX* mutex);
static int createEvent(PLT_EVENT* event);
static void destroyEvent(PLT_EVENT* event);
static PPLT_CRYPTO_CONTEXT createCrypto(void);
static void destroyCrypto(PPLT_CRYPTO_CONTEXT ctx);
static reed_solomon* createFec(int data, int parity);
static void destroyFec(reed_solomon* rs);

#define LbqInitializeLinkedBlockingQueue createQueue
#define LbqDestroyLinkedBlockingQueue destroyQueue
#define PltCreateMutex createMutex
#define PltDeleteMutex destroyMutex
#define PltCreateEvent createEvent
#define PltCloseEvent destroyEvent
#define PltCreateCryptoContext createCrypto
#define PltDestroyCryptoContext destroyCrypto
#define reed_solomon_new createFec
#define reed_solomon_release destroyFec
#if INIT_KIND == 1
#include "../src/InputStream.c"
#define INITIALIZE initializeInputStream
#define DESTROY destroyInputStream
#define NAME "input"
#elif INIT_KIND == 2
#include "../src/ControlStream.c"
#define INITIALIZE initializeControlStream
#define DESTROY destroyControlStream
#define NAME "control"
#elif INIT_KIND == 3
#include "../src/RtpAudioQueue.c"
#include "../src/AudioStream.c"
#define INITIALIZE initializeAudioStream
#define DESTROY destroyAudioStream
#define NAME "audio"
#elif INIT_KIND == 4
#include "../src/VideoDepacketizer.c"
#include "../src/VideoStream.c"
#define INITIALIZE initializeVideoStream
#define DESTROY destroyVideoStream
#define NAME "video"
#else
#error INIT_KIND must select a stream
#endif
#undef LbqInitializeLinkedBlockingQueue
#undef LbqDestroyLinkedBlockingQueue
#undef PltCreateMutex
#undef PltDeleteMutex
#undef PltCreateEvent
#undef PltCloseEvent
#undef PltCreateCryptoContext
#undef PltDestroyCryptoContext
#undef reed_solomon_new
#undef reed_solomon_release

static void* live[16];
static int creations, failAt, liveCount, invalidDeletes;
static bool shouldFail(const char* kind) {
    if (++creations != failAt) return false;
    printf("inject %s resource %d: %s\n", NAME, creations, kind);
    fflush(stdout);
    return true;
}
static void add(void* resource) {
    if (resource != NULL) live[liveCount++] = resource;
}
static bool removeLive(void* resource) {
    if (resource == NULL) return false;
    for (int i = 0; i < liveCount; ++i) {
        if (live[i] == resource) {
            live[i] = live[--liveCount];
            return true;
        }
    }
    ++invalidDeletes;
    return false;
}
static int createQueue(PLINKED_BLOCKING_QUEUE queue, int bound) {
    if (shouldFail("queue")) return ENOMEM;
    int err = LbqInitializeLinkedBlockingQueue(queue, bound);
    if (err == 0) add(queue);
    return err;
}
static PLINKED_BLOCKING_QUEUE_ENTRY destroyQueue(PLINKED_BLOCKING_QUEUE queue) {
    return removeLive(queue) ? LbqDestroyLinkedBlockingQueue(queue) : NULL;
}
static int createMutex(PLT_MUTEX* mutex) {
    if (shouldFail("mutex")) return EAGAIN;
    int err = PltCreateMutex(mutex);
    if (err == 0) add(mutex);
    return err;
}
static void destroyMutex(PLT_MUTEX* mutex) {
    if (removeLive(mutex)) PltDeleteMutex(mutex);
}
static int createEvent(PLT_EVENT* event) {
    if (shouldFail("event")) return EAGAIN;
    int err = PltCreateEvent(event);
    if (err == 0) add(event);
    return err;
}
static void destroyEvent(PLT_EVENT* event) {
    if (removeLive(event)) PltCloseEvent(event);
}
static PPLT_CRYPTO_CONTEXT createCrypto(void) {
    if (shouldFail("crypto")) return NULL;
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext();
    add(ctx);
    return ctx;
}
static void destroyCrypto(PPLT_CRYPTO_CONTEXT ctx) {
    if (removeLive(ctx)) PltDestroyCryptoContext(ctx);
}
static reed_solomon* createFec(int data, int parity) {
    if (shouldFail("FEC")) return NULL;
    reed_solomon* rs = reed_solomon_new(data, parity);
    add(rs);
    return rs;
}
static void destroyFec(reed_solomon* rs) {
    if (removeLive(rs)) reed_solomon_release(rs);
}
static int initialize(void) {
#if INIT_KIND == 4 && defined(LEGACY_VOID_VIDEO_INIT)
    INITIALIZE();
    return 0;
#else
    return INITIALIZE();
#endif
}
static void destroy(void) {
#if INIT_KIND == 2
    stopping = true;
#endif
    DESTROY();
}
static int runFailure(int index) {
    creations = 0;
    failAt = index;
    int err = initialize();
    int ok = err != 0 && liveCount == 0 && invalidDeletes == 0;
    printf("%s %s failure %d rejects initialization and releases owned resources\n",
           ok ? "PASS" : "FAIL", NAME, index);
    if (err == 0) destroy();
    if (liveCount != 0 || invalidDeletes != 0) return 1;

    failAt = creations = 0;
    err = initialize();
    if (err == 0) destroy();
    int retryOk = err == 0 && liveCount == 0 && invalidDeletes == 0;
    printf("%s %s healthy retry after failure %d\n", retryOk ? "PASS" : "FAIL", NAME, index);
    return !ok || !retryOk;
}
int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    StreamConfig.packetSize = 1024;
    if (initialize() != 0) return 1;
    const int resources = creations;
    destroy();
    if (liveCount != 0 || invalidDeletes != 0) return 1;
    printf("PASS %s healthy initialization and cleanup (%d resources)\n", NAME, resources);
    fflush(stdout);

    int failures = 0;
    for (int i = 1; i <= resources; ++i) {
        pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) {
            int result = runFailure(i);
            fflush(stdout);
            _exit(result);
        }
        int status;
        if (waitpid(child, &status, 0) != child) return 1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ++failures;
    }
    printf("%s: %d fault cases, %d failures\n", NAME, resources, failures);
    return failures != 0;
}
