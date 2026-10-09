#include "../src/VideoFormat.h"
#include <stdio.h>

int main(void) {
    const int formats[] = {VIDEO_FORMAT_PYROWAVE, VIDEO_FORMAT_PYROWAVE_444,
        VIDEO_FORMAT_PYROWAVE_HDR10, VIDEO_FORMAT_PYROWAVE_HDR10_444};
    const int modes[] = {SCM_PYROWAVE, SCM_PYROWAVE_444, SCM_PYROWAVE_HDR10, SCM_PYROWAVE_HDR10_444};
    int failures = 0;
    for (int client = 0; client < 16; ++client) {
        for (int server = 0; server < 16; ++server) {
            int offered = VIDEO_FORMAT_H264, supported = SCM_AV1_MAIN8, expected = 0;
            for (int i = 0; i < 4; ++i) {
                if (client & (1 << i)) offered |= formats[i];
                if (server & (1 << i)) supported |= modes[i];
                if ((server & 1) && (client & server & (1 << i))) expected = formats[i];
            }
            if (selectPyroWaveFormat(offered, supported) != expected) failures++;
        }
    }
    printf("PyroWave negotiation: 256 capability combinations, %d failures\n", failures);
    return failures != 0;
}
