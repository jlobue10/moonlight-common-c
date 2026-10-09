#pragma once

#include "Limelight.h"

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
