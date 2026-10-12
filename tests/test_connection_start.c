// LiStartConnection against faked stages: a termination reported while the control stream was
// starting must not let the remaining stages start or connectionStarted() follow
// connectionTerminated() (C31-02), and a negative packet size is refused before any stage
// (C31-03). The production file is included; the stage functions are renamed to fakes.
// Pause immediately after the termination claim, before the callback's separate
// error field is written. This models a real preemption of the reporting thread.
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
static pthread_mutex_t claimMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t claimChanged = PTHREAD_COND_INITIALIZER;
static bool pauseClaim, claimPaused, releaseClaim;
static pthread_t reportingThread;
static bool asyncTermination;
static void waitForFlag(bool* flag) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    while (!*flag) {
        if (pthread_cond_timedwait(&claimChanged, &claimMutex, &deadline) != 0) abort();
    }
}
static void pauseAfterClaim(void) {
    pthread_mutex_lock(&claimMutex);
    if (pauseClaim) {
        pauseClaim = false;
        claimPaused = true;
        pthread_cond_broadcast(&claimChanged);
        waitForFlag(&releaseClaim);
    }
    pthread_mutex_unlock(&claimMutex);
}
static int observedExchange(volatile int* ptr, int value, int order) {
    int previous = __atomic_exchange_n(ptr, value, order);
    if (value != 0 && previous == 0) pauseAfterClaim();
    return previous;
}
static bool observedCompareExchange(volatile int* ptr, int* expected, int value,
                                    bool weak, int successOrder, int failureOrder) {
    bool changed = __atomic_compare_exchange_n(ptr, expected, value, weak, successOrder, failureOrder);
    if (changed && value != 0) pauseAfterClaim();
    return changed;
}
#define __atomic_exchange_n observedExchange
#define __atomic_compare_exchange_n observedCompareExchange

#define resolveHostName testResolve
#define getLocalAddressByUdpConnect testLocalAddr
#define initializeAudioStream testInitAudio
#define performRtspHandshake testHandshake
#define initializeControlStream testInitControl
#define initializeVideoStream testInitVideo
#define initializeInputStream testInitInput
#define startControlStream testStartControl
#define startVideoStream testStartVideo
#define startAudioStream testStartAudio
#define startInputStream testStartInput
#define stopControlStream testStopControl
#define stopVideoStream testStopVideo
#define stopAudioStream testStopAudio
#define stopInputStream testStopInput
#define destroyControlStream testDestroyControl
#define destroyVideoStream testDestroyVideo
#define destroyAudioStream testDestroyAudio
#define destroyInputStream testDestroyInput
#define LiSendMouseMoveEvent testMouseMove
#include "../src/Connection.c"
#undef __atomic_exchange_n
#undef __atomic_compare_exchange_n
#include <stdio.h>

static int terminateDuringControlStart, controlTerminationCode = -7, startsVideo, startsAudio, startsInput, stops, destroys;
static int stagesStarting, stagesFailed, started, terminations, lastTermination;
static bool callbackReceived;

int testResolve(const char* host, int family, int tcpTestPort, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen) {
    (void)host; (void)family; (void)tcpTestPort;
    memset(addr, 0, sizeof(*addr));
    addr->ss_family = AF_INET;
    *addrLen = sizeof(struct sockaddr_in);
    return 0;
}
int testLocalAddr(const struct sockaddr_storage* targetAddr, SOCKADDR_LEN targetAddrLen, unsigned short targetPort,
                  struct sockaddr_storage* localAddr, SOCKADDR_LEN* localAddrLen) {
    (void)targetAddr; (void)targetPort;
    memset(localAddr, 0, sizeof(*localAddr));
    localAddr->ss_family = AF_INET;
    *localAddrLen = targetAddrLen;
    return 0;
}
int testInitAudio(void) { return 0; }
int testHandshake(PSERVER_INFORMATION serverInfo) { (void)serverInfo; return 0; }
int testInitControl(void) { return 0; }
int testInitVideo(void) { return 0; }
int testInitInput(void) { return 0; }
static void* reportAsynchronously(void* unused) {
    (void)unused;
    ListenerCallbacks.connectionTerminated(-19);
    return NULL;
}
int testStartControl(void) {
    if (asyncTermination) {
        pthread_mutex_lock(&claimMutex);
        pauseClaim = true; claimPaused = releaseClaim = false;
        pthread_mutex_unlock(&claimMutex);
        if (pthread_create(&reportingThread, NULL, reportAsynchronously, NULL) != 0) abort();
        pthread_mutex_lock(&claimMutex);
        waitForFlag(&claimPaused);
        pthread_mutex_unlock(&claimMutex);
        // A second stream must not replace the first claimed error or issue another callback.
        ListenerCallbacks.connectionTerminated(-33);
    }
    if (terminateDuringControlStart) {
        // The control receive thread reporting a host termination before the stage returns.
        ListenerCallbacks.connectionTerminated(controlTerminationCode);
    }
    return 0;
}
int testStartVideo(void* rendererContext, int drFlags) { (void)rendererContext; (void)drFlags; ++startsVideo; return 0; }
int testStartAudio(void* audioContext, int arFlags) { (void)audioContext; (void)arFlags; ++startsAudio; return 0; }
int testStartInput(void) { ++startsInput; return 0; }
int testStopControl(void) {
    ++stops;
    if (asyncTermination) {
        pthread_mutex_lock(&claimMutex);
        releaseClaim = true;
        pthread_cond_broadcast(&claimChanged);
        pthread_mutex_unlock(&claimMutex);
        pthread_join(reportingThread, NULL);
    }
    return 0;
}
void testStopVideo(void) { ++stops; }
void testStopAudio(void) { ++stops; }
int testStopInput(void) { ++stops; return 0; }
void testDestroyControl(void) { ++destroys; }
void testDestroyVideo(void) { ++destroys; }
void testDestroyAudio(void) { ++destroys; }
void testDestroyInput(void) { ++destroys; }
int testMouseMove(short deltaX, short deltaY) { (void)deltaX; (void)deltaY; return 0; }

static void stubStageStarting(int s) { (void)s; ++stagesStarting; }
static void stubStageComplete(int s) { (void)s; }
static void stubStageFailed(int s, int code) { (void)s; (void)code; ++stagesFailed; }
static void stubStarted(void) { ++started; }
static void stubTerminated(int code) {
    pthread_mutex_lock(&claimMutex);
    ++terminations; lastTermination = code; callbackReceived = true;
    pthread_cond_broadcast(&claimChanged);
    pthread_mutex_unlock(&claimMutex);
}
static void waitForCallback(void) {
    pthread_mutex_lock(&claimMutex);
    waitForFlag(&callbackReceived);
    pthread_mutex_unlock(&claimMutex);
}

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s" "\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

static int start(int packetSize) {
    SERVER_INFORMATION server;
    STREAM_CONFIGURATION config;
    CONNECTION_LISTENER_CALLBACKS callbacks;
    LiInitializeServerInformation(&server);
    server.address = "127.0.0.1";
    server.serverInfoAppVersion = "7.1.431.0";
    server.serverCodecModeSupport = SCM_H264;
    LiInitializeStreamConfiguration(&config);
    config.width = 1280;
    config.height = 720;
    config.fps = 60;
    config.bitrate = 10000;
    config.packetSize = packetSize;
    config.streamingRemotely = STREAM_CFG_LOCAL;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;
    LiInitializeConnectionCallbacks(&callbacks);
    callbacks.stageStarting = stubStageStarting;
    callbacks.stageComplete = stubStageComplete;
    callbacks.stageFailed = stubStageFailed;
    callbacks.connectionStarted = stubStarted;
    callbacks.connectionTerminated = stubTerminated;
    startsVideo = startsAudio = startsInput = stops = destroys = stagesStarting = stagesFailed = started = terminations = 0;
    callbackReceived = false;
    return LiStartConnection(&server, &config, &callbacks, NULL, NULL, NULL, 0, NULL, 0);
}

int main(void) {
    int ret;

    ret = start(1024);
    check(ret == 0 && started == 1 && startsVideo == 1 && startsAudio == 1 && startsInput == 1 && terminations == 0,
          "an undisturbed start reaches connectionStarted() with every stage started");
    LiStopConnection();
    check(stops == 4 && destroys == 4, "LiStopConnection() unwinds every started stage");

    terminateDuringControlStart = 1;
    ret = start(1024);
    waitForCallback();  // the app's connectionTerminated runs on the detached AsyncTerm thread
    terminateDuringControlStart = 0;
    check(ret == -7, "a termination during the control stream start fails LiStartConnection() with the reported code");
    check(started == 0, "connectionStarted() never follows connectionTerminated()");
    check(startsVideo == 0 && startsAudio == 0 && startsInput == 0, "no later stage starts for a session the host already ended");
    check(stagesFailed == 0 && terminations == 1 && lastTermination == -7, "the app receives exactly one terminated callback and no stageFailed");
    check(stops == 1 && destroys == 4, "the started control stage and the initialised stages are unwound");

    // The previous session reported -7. A reporting thread in this one has claimed
    // termination but is preempted before writing terminationCallbackErrorCode.
    asyncTermination = true;
    ret = start(1024);
    asyncTermination = false;
    waitForCallback();
    check(ret == -19, "startup returns this session's termination code even if the reporting thread pauses after claiming it");
    check(started == 0 && startsVideo == 0 && startsAudio == 0 && startsInput == 0,
          "a claimed asynchronous termination prevents every later start stage");
    check(terminations == 1 && lastTermination == -19 && stagesFailed == 0,
          "the asynchronous app callback agrees with the startup error and runs once");
    check(stops == 1 && destroys == 4, "asynchronous termination still unwinds all initialized stages");

    terminateDuringControlStart = 1;
    controlTerminationCode = 0;
    ret = start(1024);
    waitForCallback();
    terminateDuringControlStart = 0;
    check(ret == -1 && started == 0, "graceful termination during startup is still a failed start");
    check(terminations == 1 && lastTermination == 0, "the app callback retains the original graceful termination code");

    ret = start(-16);
    check(ret == -1 && stagesStarting == 0, "a negative packet size is refused before any stage starts");
    ret = start(0);
    check(ret == -1 && stagesStarting == 0, "a zero packet size is still refused");

    printf("%d checks, %d failures" "\n", checks, failures);
    return failures != 0;
}
