#pragma once

#include "Limelight.h"
#include <stddef.h>
#include <string.h>

// Parse complete SDP lines within the RTSP payload, rather than finding the
// codec name or revision inside an unrelated attribute. Require exactly one
// revision: conflicting/duplicate declarations are not a compatibility promise.
static inline int hasCompatiblePyroWaveSdp(const char* sdp, size_t length) {
    static const char revisionPrefix[] = "a=x-ss-pyrowave.bitstream:";
    static const char mediaPrefix[] = "a=rtpmap:";
    static const char mediaName[] = " PYROWAVE/90000";
    int revisionFound = 0, mediaFound = 0;
    size_t offset = 0;

    if (sdp == NULL) return 0;
    while (offset < length) {
        const char* line = sdp + offset;
        size_t lineLength = 0;
        while (lineLength < length - offset && line[lineLength] != '\n') lineLength++;
        offset += lineLength;
        if (offset < length) offset++;
        if (lineLength && line[lineLength - 1] == '\r') lineLength--;

        if (lineLength >= sizeof(revisionPrefix) - 1 &&
            memcmp(line, revisionPrefix, sizeof(revisionPrefix) - 1) == 0) {
            if (revisionFound ||
                lineLength != sizeof(revisionPrefix) - 1 + sizeof(LI_PYROWAVE_BITSTREAM_ID) - 1 ||
                memcmp(line + sizeof(revisionPrefix) - 1, LI_PYROWAVE_BITSTREAM_ID,
                       sizeof(LI_PYROWAVE_BITSTREAM_ID) - 1) != 0) return 0;
            revisionFound = 1;
        }
        else if (lineLength > sizeof(mediaPrefix) - 1 &&
                 memcmp(line, mediaPrefix, sizeof(mediaPrefix) - 1) == 0) {
            size_t pos = sizeof(mediaPrefix) - 1, firstDigit = pos;
            unsigned int payloadType = 0;
            while (pos < lineLength && pos - firstDigit < 3 &&
                   line[pos] >= '0' && line[pos] <= '9') {
                payloadType = payloadType * 10 + (unsigned int)(line[pos++] - '0');
            }
            if (pos > firstDigit && payloadType <= 127 &&
                lineLength - pos == sizeof(mediaName) - 1 &&
                memcmp(line + pos, mediaName, sizeof(mediaName) - 1) == 0) mediaFound = 1;
        }
    }
    return revisionFound && mediaFound;
}

// Zero means there is no mutually supported PyroWave profile. Keep the server's
// base capability requirement, but never infer support for an unoffered profile.
static inline int selectPyroWaveFormat(int clientFormats, int serverModes) {
    if (!(serverModes & SCM_PYROWAVE)) return 0;
    if ((clientFormats & VIDEO_FORMAT_PYROWAVE_HDR10_444) && (serverModes & SCM_PYROWAVE_HDR10_444))
        return VIDEO_FORMAT_PYROWAVE_HDR10_444;
    if ((clientFormats & VIDEO_FORMAT_PYROWAVE_HDR10) && (serverModes & SCM_PYROWAVE_HDR10))
        return VIDEO_FORMAT_PYROWAVE_HDR10;
    if ((clientFormats & VIDEO_FORMAT_PYROWAVE_444) && (serverModes & SCM_PYROWAVE_444))
        return VIDEO_FORMAT_PYROWAVE_444;
    if (clientFormats & VIDEO_FORMAT_PYROWAVE) return VIDEO_FORMAT_PYROWAVE;
    return 0;
}
