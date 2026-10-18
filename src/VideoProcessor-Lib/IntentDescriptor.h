#pragma once

// Intent interpreter shared core (docs/VP-intent-interpreter-design.md,
// "Shared core: IntentDescriptor"; owner decision 2026-10-17). One descriptor
// per shot; each field carries its value, where it came from and whether it
// is valid, following the SceneDescriptor.h source-tag pattern.
//
// Rules (owner): rule-based only (no AI or neural methods), no hard-coded
// rates, taste settings are never fitted, every function default off. The
// interpreter only SELECTS owner-written profiles (the `content` variable of
// when: rules) or, later, scales controls the owner already sets within the
// owner's limits; it never invents values.
//
// Pure: no renderer, GPU or configuration state. ContentIntentStatus is plain
// data so it can cross the renderer plugin boundary.

#include <cstdint>
#include <cstring>
#include <string>

// Where an intent value came from. Precedence (design section 6):
// Dv2 > DvL11 / DvL1 > Inferred > Off (the owner's own setting).
enum class IntentSource : uint8_t
{
    Off,        // the function is off, or nothing is known: the owner's setting applies
    Inferred,   // VP's own rule-based measurement
    DvL1,       // DV tunnel L1 (scene statistics)
    DvL11,      // DV tunnel L11 (content type, reference mode, hints)
    Dv2,        // reserved: real DV2 metadata
};

inline const char* ToString(IntentSource source)
{
    switch (source)
    {
    case IntentSource::Inferred: return "inferred";
    case IntentSource::DvL1: return "dv_l1";
    case IntentSource::DvL11: return "dv_l11";
    case IntentSource::Dv2: return "dv2";
    default: return "off";
    }
}

// The wording the DV card / status line shows: "inferred" or "from metadata".
inline const char* DisplayName(IntentSource source)
{
    switch (source)
    {
    case IntentSource::Inferred: return "inferred";
    case IntentSource::DvL1:
    case IntentSource::DvL11:
    case IntentSource::Dv2: return "from metadata";
    default: return "off";
    }
}

inline int Precedence(IntentSource source)
{
    switch (source)
    {
    case IntentSource::Dv2: return 3;
    case IntentSource::DvL11:
    case IntentSource::DvL1: return 2;
    case IntentSource::Inferred: return 1;
    default: return 0;
    }
}

template <typename T>
struct IntentField
{
    T value{};
    IntentSource source = IntentSource::Off;
    bool valid = false;

    static IntentField Make(const T& v, IntentSource s)
    {
        IntentField field;
        field.value = v;
        field.source = s;
        field.valid = s != IntentSource::Off;
        return field;
    }
};

// The valid field with the higher precedence; a tie keeps `a`.
template <typename T>
inline IntentField<T> Prefer(const IntentField<T>& a, const IntentField<T>& b)
{
    if (!b.valid) return a;
    if (!a.valid) return b;
    return Precedence(b.source) > Precedence(a.source) ? b : a;
}

// Item 1: the content class (design section 5).
enum class ContentClass : uint8_t
{
    Unknown,     // not classified yet (or the function is off)
    Film,
    Video,
    Sports,
    Animation,
    StaticUi,
};

// The `content` variable's values (when: content == "sports").
inline const char* ToString(ContentClass c)
{
    switch (c)
    {
    case ContentClass::Film: return "film";
    case ContentClass::Video: return "video";
    case ContentClass::Sports: return "sports";
    case ContentClass::Animation: return "animation";
    case ContentClass::StaticUi: return "static_ui";
    default: return "";
    }
}

inline bool ParseContentClass(const std::string& text, ContentClass& out)
{
    for (const ContentClass c : { ContentClass::Film, ContentClass::Video, ContentClass::Sports,
        ContentClass::Animation, ContentClass::StaticUi })
        if (text == ToString(c)) { out = c; return true; }
    return false;
}

// DV L11 content type -> class (libdovi level11.rs: 0 default, 1 movies,
// 2 game, 3 sport, 4 user-generated content, others reserved). Default and
// reserved say nothing (false: the measurement decides). A game is a moving,
// rendered video picture: video. User-generated content: video.
inline bool ContentClassFromL11(uint8_t contentType, ContentClass& out)
{
    switch (contentType)
    {
    case 1: out = ContentClass::Film; return true;
    case 2: out = ContentClass::Video; return true;
    case 3: out = ContentClass::Sports; return true;
    case 4: out = ContentClass::Video; return true;
    default: return false;
    }
}

// One descriptor per shot, keyed by the source sequence of its first frame.
struct IntentDescriptor
{
    uint64_t shotSequence = 0;
    IntentField<ContentClass> content;              // 1: content recognition
    IntentField<float> motionScale;                 // 2: motion intent (multiplies de-judder strength)
    IntentField<float> ambientBlackNits;            // 3: ambient adaptation (effective black)
    IntentField<float> shadowStepsBelowBlack;       // 4: black protection report
    IntentField<float> displayCompressionStops;     // 5: display mapping readout
};

// ---- Items 2-5 (intent remainder, 2026-10-17): plain-data status structs ----
// They cross the renderer plugin boundary (IRenderer::GetIntentStatus), so
// they hold no std::string or pointers; fixed char buffers only.

// Motion intent policies (owner decision 2026-10-17, key intent_motion_policy).
enum class MotionIntentPolicy : uint8_t
{
    Display,     // analysis on the OSD / log only; strength unchanged (default)
    EaseFast,    // reduce strength as pan speed rises
    EaseSlow,    // full strength on fast pans; ease on slow, steady shots
    Visibility,  // scale by the judder-visibility estimate J
    Metadata,    // act only on L11 frame-rate-conversion hints / real DV2
};

inline const char* ToString(MotionIntentPolicy p)
{
    switch (p)
    {
    case MotionIntentPolicy::EaseFast: return "ease_fast";
    case MotionIntentPolicy::EaseSlow: return "ease_slow";
    case MotionIntentPolicy::Visibility: return "visibility";
    case MotionIntentPolicy::Metadata: return "metadata";
    default: return "display";
    }
}

inline bool ParseMotionIntentPolicy(const std::string& text, MotionIntentPolicy& out)
{
    for (const MotionIntentPolicy p : { MotionIntentPolicy::Display, MotionIntentPolicy::EaseFast,
        MotionIntentPolicy::EaseSlow, MotionIntentPolicy::Visibility, MotionIntentPolicy::Metadata })
        if (text == ToString(p)) { out = p; return true; }
    return false;
}

template <size_t N>
inline void CopyIntentText(char (&dst)[N], const char* src)
{
    size_t i = 0;
    for (; src && src[i] != '\0' && i + 1 < N; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

struct MotionIntentStatus
{
    bool enabled = false;                           // intent_motion on
    MotionIntentPolicy policy = MotionIntentPolicy::Display;
    bool shotValid = false;                         // a measured shot is behind the values
    uint64_t shotSequence = 0;                      // first source frame of the shot the scale belongs to
    float appliedScale = 1.0f;                      // the multiplier on the de-judder strength now (1 = unchanged)
    float candidateScale = 1.0f;                    // what the policy computes for the shot being measured
    float inferredScale = 1.0f;                     // the inferred value, always logged next to metadata
    IntentSource source = IntentSource::Off;        // where appliedScale came from
    float panPictureHeightsPerSecond = 0.0f;        // shot mean pan speed
    float panSpread = 0.0f;                         // steadiness: spread of the pan speed within the shot
    float edgeContrast = 0.0f;                      // mean edge term (0..1)
    float visibility = 0.0f;                        // J = band(v) x edge term
    float shotSeconds = 0.0f;
    bool vectorsUsed = false;                       // the de-judder's valid vectors fed the pan speed
    char reason[64] = {};                           // why the scale is what it is
};

struct AmbientIntentStatus
{
    bool enabled = false;                           // ambient_adapt on
    bool sensorValid = false;                       // a recent reading (or a manual room) is behind the values
    float smoothedLux = 0.0f;
    float veilNits = 0.0f;                          // lux x ambient_screen_reflect
    float displayBlackNits = 0.0f;                  // the owner's black (DisplayProfile)
    float targetBlackNits = 0.0f;                   // quantised B_eff the filter wants
    float appliedBlackNits = 0.0f;                  // B_eff the renderer renders to now
    char reason[64] = {};
};

enum class BlackIntentMode : uint8_t { Off, Report, LedProtect };

inline const char* ToString(BlackIntentMode m)
{
    return m == BlackIntentMode::Report ? "report" : m == BlackIntentMode::LedProtect ? "led_protect" : "off";
}

inline bool ParseBlackIntentMode(const std::string& text, BlackIntentMode& out)
{
    if (text == "off") { out = BlackIntentMode::Off; return true; }
    if (text == "report") { out = BlackIntentMode::Report; return true; }
    if (text == "led_protect") { out = BlackIntentMode::LedProtect; return true; }
    return false;
}

struct BlackIntentStatus
{
    BlackIntentMode mode = BlackIntentMode::Off;
    bool valid = false;
    float l1MinNits = 0.0f;                         // DV L1 scene minimum (when present)
    IntentSource l1Source = IntentSource::Off;
    float p1Nits = 0.0f, p5Nits = 0.0f, p10Nits = 0.0f;   // measured shadow percentiles
    float displayBlackNits = 0.0f;
    float thresholdNits = 0.0f;                     // display black + one 8-bit step
    int stepsBelow = 0;                             // shadow steps (L1 min .. p10) under the threshold
    int stepsTotal = 0;
    float ledMinimumShare = 0.0f;                   // led_protect: the lowest light share that keeps p5 visible (0 = no cap)
    char reason[64] = {};
};

struct DisplayIntentStatus
{
    bool enabled = false;                           // intent_display on
    bool valid = false;
    float sceneMaxNits = 0.0f;
    IntentSource sceneMaxSource = IntentSource::Off;  // DvL1 or Inferred (measured)
    float displayPeakNits = 0.0f;
    float compressionPq = 0.0f;                     // PQ(scene max) - PQ(display peak), 0..1 PQ units; >= 0
    float compressionStops = 0.0f;                  // log2(scene max / display peak); never negative (no expansion)
    float trimNits = 0.0f;                          // the L2/L8 trim target chosen for the display peak (0 = none)
    char line[96] = {};                             // the DV-card line
};

// Everything items 2-5 publish, read by the GUI once a second.
struct IntentStatus
{
    MotionIntentStatus motion;
    AmbientIntentStatus ambient;
    BlackIntentStatus black;
    DisplayIntentStatus display;
};

// The content class as the GUI reads it from the renderer (plain data).
struct ContentIntentStatus
{
    bool enabled = false;                       // intent_content on
    ContentClass content = ContentClass::Unknown;   // what the rules see (after hysteresis)
    IntentSource source = IntentSource::Off;
    ContentClass candidate = ContentClass::Unknown; // measured now, before hysteresis
    IntentSource candidateSource = IntentSource::Off;
    bool motionValid = false;                   // look-ahead motion statistics available
    float panPictureHeightsPerSecond = 0.0f;    // mean pan speed over the window
    float widePanShare = 0.0f;                  // share of moving frames with a wide pan
    float windowSeconds = 0.0f;                 // seconds of statistics behind the candidate
    char reason[64] = {};                       // the rule that gave the candidate
};

inline void SetReason(ContentIntentStatus& status, const char* reason)
{
    size_t i = 0;
    for (; reason && reason[i] != '\0' && i + 1 < sizeof(status.reason); ++i)
        status.reason[i] = reason[i];
    status.reason[i] = '\0';
}

// The rules' `content` value: empty while the switch is off or nothing is
// classified yet (the same convention as `ambient`).
inline std::string ContentRuleValue(const ContentIntentStatus& status)
{
    if (!status.enabled || status.content == ContentClass::Unknown)
        return std::string();
    return ToString(status.content);
}
