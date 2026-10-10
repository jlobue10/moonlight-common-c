// Exercise the production motion event entry point's argument validation.
// Type 0 used to pass a one-sided int comparison and index the sensor state at [-1].
#include "../src/InputStream.c"

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

int main(void) {
    memcpy(AppVersionQuad, (int[]){7, 1, 431, 0}, sizeof(AppVersionQuad));
    SunshineFeatureFlags = LI_FF_CONTROLLER_TOUCH_EVENTS;
    if (initializeInputStream() != 0) return 2;
    initialized = true;

    check(LiSendControllerMotionEvent(0, 0, 1.0f, 2.0f, 3.0f) == -3, "motion type 0 is rejected");
    check(LiSendControllerMotionEvent(0, MAX_MOTION_EVENTS + 1, 1.0f, 2.0f, 3.0f) == -3, "motion type past the last is rejected");
    check(LiSendControllerMotionEvent(0, 255, 1.0f, 2.0f, 3.0f) == -3, "motion type 255 is rejected");
    check(LiSendControllerMotionEvent(3, LI_MOTION_TYPE_ACCEL, 1.0f, 2.0f, 3.0f) == 0, "accelerometer event is queued");
    check(LiSendControllerMotionEvent(3, LI_MOTION_TYPE_GYRO, 4.0f, 5.0f, 6.0f) == 0, "gyroscope event is queued");
    check(currentGamepadSensorState[3][LI_MOTION_TYPE_GYRO - 1].x == 4.0f &&
          currentGamepadSensorState[3][LI_MOTION_TYPE_ACCEL - 1].z == 3.0f, "events land in their own slots");
    check(currentGamepadSensorState[2][MAX_MOTION_EVENTS - 1].x == 0.0f, "the neighbouring controller's state is untouched");

    initialized = false;
    destroyInputStream();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
