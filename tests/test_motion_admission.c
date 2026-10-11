// Exercise production sensor admission with concurrent producers and backpressure.
#include "../src/Limelight-internal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

static void* hookMalloc(size_t size);
#define malloc hookMalloc
#include "../src/InputStream.c"
#undef malloc

static _Thread_local bool failAllocation;
static atomic_bool allocating, finishAllocation, followerStarted, followerDone;
static int ownerResult, followerResult;
static int checks, failures;

static void check(bool ok, const char* message) {
    ++checks;
    failures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
}

static void* hookMalloc(size_t size) {
    if (failAllocation) {
        atomic_store(&allocating, true);
        while (!atomic_load(&finishAllocation)) sched_yield();
        return NULL;
    }
    return malloc(size);
}

static void* owner(void* unused) {
    (void)unused;
    failAllocation = true;
    ownerResult = LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, 1, 2, 3);
    return NULL;
}

static void* follower(void* unused) {
    (void)unused;
    atomic_store(&followerStarted, true);
    // The final zero gyro sample must have a real holder if this reports success.
    followerResult = LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, 0, 0, 0);
    atomic_store(&followerDone, true);
    return NULL;
}

static void* flood(void* unused) {
    (void)unused;
    for (int i = 0; i < 10000; ++i) {
        LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, 1, 2, 3);
    }
    return NULL;
}

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, -1}, sizeof(AppVersionQuad));
    SunshineFeatureFlags = LI_FF_CONTROLLER_TOUCH_EVENTS;
    if (initializeInputStream() != 0) return 2;
    initialized = true;

    pthread_t first, second;
    pthread_create(&first, NULL, owner, NULL);
    while (!atomic_load(&allocating)) sched_yield();
    pthread_create(&second, NULL, follower, NULL);
    while (!atomic_load(&followerStarted)) sched_yield();
    // The old implementation lets this producer return success while the only
    // prospective holder is still being allocated. The fixed one waits for that
    // transaction, then admits its own holder after the allocation fails.
    for (int i = 0; i < 100 && !atomic_load(&followerDone); ++i) {
        struct timespec interval = {0, 1000000};
        nanosleep(&interval, NULL);
    }
    atomic_store(&finishAllocation, true);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    check(ownerResult == -1, "the failed allocation is reported to its caller");
    check(followerResult == 0 && LbqGetItemCount(&packetQueue) == 1 &&
          currentGamepadSensorState[0][LI_MOTION_TYPE_GYRO - 1].dirty &&
          currentGamepadSensorState[0][LI_MOTION_TYPE_GYRO - 1].x == 0,
          "an accepted final gyro stop retains a holder after a concurrent allocation failure");

    PPACKET_HOLDER holder;
    while (LbqPollQueueElement(&packetQueue, (void**)&holder) == LBQ_SUCCESS) freePacketHolder(holder);
    memset(currentGamepadSensorState, 0, sizeof(currentGamepadSensorState));
    // A zero-capacity queue deterministically refuses every new packet.
    packetQueue.sizeBound = 0;
    pthread_t producers[4];
    for (int i = 0; i < 4; ++i) pthread_create(&producers[i], NULL, flood, NULL);
    for (int i = 0; i < 4; ++i) pthread_join(producers[i], NULL);
    check(!currentGamepadSensorState[0][LI_MOTION_TYPE_GYRO - 1].dirty &&
          LbqGetItemCount(&packetQueue) == 0, "rejected concurrent updates leave no phantom pending holder");
    packetQueue.sizeBound = MAX_QUEUED_INPUT_PACKETS;
    check(LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, 0, 0, 0) == 0 &&
          LbqGetItemCount(&packetQueue) == 1, "admission recovers when queue capacity returns");

    initialized = false;
    LbqSignalQueueShutdown(&packetQueue);
    LbqSignalQueueShutdown(&packetHolderFreeList);
    destroyInputStream();
    printf("%d checks; %d failures\n", checks, failures);
    return failures != 0;
}
