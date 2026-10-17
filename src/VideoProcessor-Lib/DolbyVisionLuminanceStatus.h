#pragma once

// Per-frame source metadata. Values are nits, independently of VP's output target.
struct DolbyVisionLuminanceStatus
{
    bool tunnelActive = false;
    bool available = false;
    bool sourceRangeFromMetadata = false;
    bool sceneAvailable = false;
    float sourceMin = 0;
    float sourceMax = 0;
    float sceneMax = 0;
    float sceneAvg = 0;
    // The L1 scene minimum (2026-10-17): status only; libplacebo has no scene
    // minimum input (see DolbyVisionLuminancePolicy::SceneMinNits).
    bool sceneMinAvailable = false;
    float sceneMin = 0;
};
