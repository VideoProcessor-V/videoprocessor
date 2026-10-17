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
// Items 2-5 are reserved fields (not built; always Off).
struct IntentDescriptor
{
    uint64_t shotSequence = 0;
    IntentField<ContentClass> content;              // 1: content recognition (built)
    IntentField<float> motionScale;                 // 2: motion intent (reserved)
    IntentField<float> ambientBlackNits;            // 3: ambient adaptation (reserved)
    IntentField<float> shadowStepsBelowBlack;       // 4: black protection report (reserved)
    IntentField<float> displayCompressionStops;     // 5: display mapping readout (reserved)
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
