// Platform.c defines _GNU_SOURCE itself (pthread_setname_np); it has to be set before the
// first system header, which this test includes earlier.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
// Platform primitives: a failed mutex/condvar init must be reported by PltCreateEvent (the
// "< 0" test on a positive errno never fired), and a NULL crypto context must fail the
// operation instead of being dereferenced.
#include "../src/Limelight-internal.h"
#if !defined(LC_WINDOWS) && !defined(__WIIU__) && !defined(__3DS__)
static int failingMutexInit(pthread_mutex_t* m, const pthread_mutexattr_t* a);
static int failingCondInit(pthread_cond_t* c, const pthread_condattr_t* a);
#define pthread_mutex_init failingMutexInit
#define pthread_cond_init failingCondInit
#define POSIX_HOOKS 1
#endif
#include "../src/Platform.c"
#undef pthread_mutex_init
#undef pthread_cond_init
#include <stdio.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
#ifdef POSIX_HOOKS
static int mutexFailures, condFailures;
static int failingMutexInit(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    if (mutexFailures > 0) { --mutexFailures; return EAGAIN; }
    return (pthread_mutex_init)(m, a);
}
static int failingCondInit(pthread_cond_t* c, const pthread_condattr_t* a) {
    if (condFailures > 0) { --condFailures; return ENOMEM; }
    return (pthread_cond_init)(c, a);
}
#endif

int main(void) {
    PLT_EVENT event;
#ifdef POSIX_HOOKS
    int eventsBefore = activeEvents, mutexesBefore = activeMutexes;
    mutexFailures = 1;
    check(PltCreateEvent(&event) != 0 && activeEvents == eventsBefore && activeMutexes == mutexesBefore,
          "a failed mutex init fails PltCreateEvent and counts nothing");
    condFailures = 1;
    check(PltCreateEvent(&event) != 0 && activeEvents == eventsBefore && activeMutexes == mutexesBefore,
          "a failed condvar init fails PltCreateEvent and releases the mutex");
#endif
    check(PltCreateEvent(&event) == 0, "a healthy event is created");
    PltCloseEvent(&event);

    unsigned char key[16] = {0}, iv[12] = {0}, tag[16], in[16] = {0}, out[32];
    int outLen = sizeof(out);
    check(!PltEncryptMessage(NULL, ALGORITHM_AES_GCM, 0, key, sizeof(key), iv, sizeof(iv), tag, sizeof(tag), in, sizeof(in), out, &outLen),
          "encrypting with a NULL crypto context fails instead of crashing");
    check(!PltDecryptMessage(NULL, ALGORITHM_AES_GCM, 0, key, sizeof(key), iv, sizeof(iv), tag, sizeof(tag), in, sizeof(in), out, &outLen),
          "decrypting with a NULL crypto context fails instead of crashing");
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
