#include "../src/VideoFormat.h"
#include <stdio.h>
#include <stdlib.h>

static int testSdp(void) {
    static const char media[] = "a=rtpmap:99 PYROWAVE/90000\r\n";
    static const char revision[] = "a=x-ss-pyrowave.bitstream:" LI_PYROWAVE_BITSTREAM_ID;
    const struct { const char* text; int expected; } cases[] = {
        {"a=rtpmap:99 PYROWAVE/90000\r\na=x-ss-pyrowave.bitstream:186f0393\r\n", 1},
        {"a=x-ss-pyrowave.bitstream:186f0393\na=rtpmap:99 PYROWAVE/90000", 1},
        {"a=x-ss-pyrowave.bitstream:186f0393\na=rtpmap:100 PYROWAVE/90000\n", 1},
        {"a=rtpmap:99 PYROWAVE/90000\r\n", 0},
        {"a=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:deadbeef", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f039", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f03930", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393 garbage", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393\na=x-ss-pyrowave.bitstream:deadbeef", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:deadbeef\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:99 PYROWAVE/90000\na=note:x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=note:PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:99 PYROWAVE/90000suffix\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:128 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap:9999999999999 PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"a=rtpmap: PYROWAVE/90000\na=x-ss-pyrowave.bitstream:186f0393", 0},
        {"", 0},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = strlen(cases[i].text);
        // Deliberately omit the terminating NUL. ASan checks the length boundary.
        char* exact = malloc(length ? length : 1);
        memcpy(exact, cases[i].text, length);
        if (hasCompatiblePyroWaveSdp(exact, length) != cases[i].expected) {
            fprintf(stderr, "SDP case %zu failed\n", i);
            failures++;
        }
        free(exact);
    }
    if (hasCompatiblePyroWaveSdp(NULL, 0) || hasCompatiblePyroWaveSdp(NULL, 20)) failures++;

    char packet[sizeof(media) + sizeof(revision)];
    snprintf(packet, sizeof(packet), "%s%s", media, revision);
    size_t length = strlen(packet);
    for (size_t size = 0; size <= length; ++size) {
        char* exact = malloc(size ? size : 1);
        memcpy(exact, packet, size);
        if (hasCompatiblePyroWaveSdp(exact, size) != (size == length)) failures++;
        free(exact);
    }
    // Test embedded NULs and all single-byte corruptions of the required ID.
    for (size_t pos = length - 8; pos < length; ++pos) {
        char original = packet[pos];
        for (int value = 0; value < 256; ++value) {
            packet[pos] = (char)value;
            if (hasCompatiblePyroWaveSdp(packet, length) != (value == (unsigned char)original)) failures++;
        }
        packet[pos] = original;
    }
    printf("PyroWave SDP: 20 cases, all truncations, 2048 revision mutations; %d failures\n", failures);
    return failures;
}

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
    failures += testSdp();
    return failures != 0;
}
