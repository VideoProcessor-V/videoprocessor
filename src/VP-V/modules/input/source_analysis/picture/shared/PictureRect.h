// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U8 "active-picture rect / black bars" (doc 62 section 1.2): NO new detector. The rect is Bill's active picture
// when given (authority: ActivePictureEvidence); else the matcher's own near-black rows/columns on the 1/8 luma; else the
// whole frame. The unit carries that one rule to every VP consumer: the matcher's PICTURE pass emits the kernel below, the
// P4 / TD / CfL readers ask InPicture / Coverage instead of keeping a private rect convention.
//
// Law (moved unchanged from motion/consumers/DejudderWarp.cpp PICTURE_BODY):
//   given   a non-empty rect (right > left and bottom > top, right/bottom exclusive, input pixels) is clamped to the frame
//   detect  otherwise, on the 1/8 luma: the first and last rows and columns whose maximum exceeds black + barCodes
//           (black = 16 limited / 0 full range; barCodes = 4), scaled back by the plane scale; none found = whole frame
//   frame   (0, 0, w, h)
// Determinism: one work group, atomic min/max of integers only (order independent); CPU Compute is the reference.
//
// Registered bug fix (not applied here, needs the repair packages and a GPU run): TextureRestore and the debander ignore the
// rect; they gain InPicture / Coverage reads in their own packages.
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace VpUnit
{
namespace PictureRect
{
// Bump on ANY change of the rule above, of the kernel text or of the GLSL entry names.
constexpr uint32_t kUnitVersion = 1;

struct Params {
    float blackCode = 16.0f;   // the plane's black in its own codes: 16 limited, 0 full range (ForRange)
    float barCodes = 4.0f;     // margin above black before a row/column counts as picture
    int planeScale = 8;        // source pixels per plane sample (the matcher's 1/8 luma)
};

inline Params ForRange(bool fullRange)
{
    Params p;
    p.blackCode = fullRange ? 0.0f : 16.0f;
    return p;
}

// A rectangle in input pixels, right and bottom exclusive.
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const { return !(x1 > x0 && y1 > y0); }
};

struct In {
    const float* plane = nullptr;   // the 1/8 luma, row-major, `planeW` x `planeH` (codes of Params::blackCode's domain)
    int planeW = 0, planeH = 0;
    Rect given;                     // Bill's rect; empty = not known
    float frameW = 0, frameH = 0;   // the input frame in pixels
};

// The CPU reference of the matcher's PICTURE pass.
inline Rect Compute(const In& in, const Params& p = Params())
{
    const float frameW = in.frameW, frameH = in.frameH;
    if (!in.given.Empty()) {
        Rect r;
        r.x0 = (std::min)((std::max)(in.given.x0, 0.0f), frameW);
        r.y0 = (std::min)((std::max)(in.given.y0, 0.0f), frameH);
        r.x1 = (std::min)((std::max)(in.given.x1, 0.0f), frameW);
        r.y1 = (std::min)((std::max)(in.given.y1, 0.0f), frameH);
        return r;
    }
    const float threshold = p.blackCode + p.barCodes;
    int rowFirst = 1 << 30, rowLast = -1, colFirst = 1 << 30, colLast = -1;
    if (in.plane) {
        for (int y = 0; y < in.planeH; ++y) {
            float m = 0.0f;
            for (int x = 0; x < in.planeW; ++x)
                m = (std::max)(m, in.plane[static_cast<size_t>(y) * in.planeW + x]);
            if (m > threshold) {
                rowFirst = (std::min)(rowFirst, y);
                rowLast = (std::max)(rowLast, y);
            }
        }
        for (int x = 0; x < in.planeW; ++x) {
            float m = 0.0f;
            for (int y = 0; y < in.planeH; ++y)
                m = (std::max)(m, in.plane[static_cast<size_t>(y) * in.planeW + x]);
            if (m > threshold) {
                colFirst = (std::min)(colFirst, x);
                colLast = (std::max)(colLast, x);
            }
        }
    }
    Rect r;
    r.x1 = frameW;
    r.y1 = frameH;
    if (rowLast >= rowFirst && colLast >= colFirst) {
        const float s = static_cast<float>(p.planeScale);
        r.x0 = static_cast<float>(colFirst) * s;
        r.y0 = static_cast<float>(rowFirst) * s;
        r.x1 = (std::min)(static_cast<float>(colLast + 1) * s, frameW);
        r.y1 = (std::min)(static_cast<float>(rowLast + 1) * s, frameH);
    }
    return r;
}

// A reader's view of a rect that may be absent (an empty rect means the whole frame).
inline Rect Effective(const Rect& r, float frameW, float frameH)
{
    if (r.Empty()) {
        Rect whole;
        whole.x1 = frameW;
        whole.y1 = frameH;
        return whole;
    }
    return r;
}

inline bool InPicture(const Rect& r, float px, float py) { return px >= r.x0 && px < r.x1 && py >= r.y0 && py < r.y1; }

// The share (0..1) of the block [bx0, bx1) x [by0, by1) that lies inside the rect.
inline float Coverage(const Rect& r, float bx0, float by0, float bx1, float by1)
{
    const float area = (bx1 - bx0) * (by1 - by0);
    if (!(area > 0.0f))
        return 0.0f;
    const float w = (std::min)(bx1, r.x1) - (std::max)(bx0, r.x0);
    const float h = (std::min)(by1, r.y1) - (std::max)(by0, r.y0);
    return (w > 0.0f && h > 0.0f) ? (std::min)(1.0f, (w * h) / area) : 0.0f;
}

// The rect of a half-size pyramid level from one in input pixels (left/top floor, right/bottom ceil): the inner matcher's grid.
inline void Halved(const int in[4], int out[4])
{
    out[0] = in[0] / 2;
    out[1] = in[1] / 2;
    out[2] = (in[2] + 1) / 2;
    out[3] = (in[3] + 1) / 2;
}

// ---- GLSL -----------------------------------------------------------------------------------------------------------------

// The matcher's PICTURE compute pass, byte-for-byte what the matcher used to carry (one work group of 256, `l1` the 1/8 luma
// sampler, `dst` the 1x1 rgba32f rect image, uniforms given, frame_size, black, bar_codes).
inline const char* KernelHeader()
{
    return R"GLSL(
shared int pa_[4];
)GLSL";
}

inline const char* KernelBody()
{
    return R"GLSL(
uint i = gl_LocalInvocationIndex;
if (i == 0u) { pa_[0] = 1 << 30; pa_[1] = -1; pa_[2] = 1 << 30; pa_[3] = -1; }
barrier();
ivec2 s = textureSize(l1, 0);
if (given.z <= given.x || given.w <= given.y) {
    for (int y = int(i); y < s.y; y += 256) {
        float m = 0.0;
        for (int x = 0; x < s.x; x++)
            m = max(m, texelFetch(l1, ivec2(x, y), 0).r);
        if (m > black + bar_codes) { atomicMin(pa_[0], y); atomicMax(pa_[1], y); }
    }
    for (int x = int(i); x < s.x; x += 256) {
        float m = 0.0;
        for (int y = 0; y < s.y; y++)
            m = max(m, texelFetch(l1, ivec2(x, y), 0).r);
        if (m > black + bar_codes) { atomicMin(pa_[2], x); atomicMax(pa_[3], x); }
    }
}
barrier();
if (i == 0u) {
    vec4 r = vec4(0.0, 0.0, frame_size);
    if (given.z > given.x && given.w > given.y)
        r = clamp(given, vec4(0.0), vec4(frame_size, frame_size));
    else if (pa_[1] >= pa_[0] && pa_[3] >= pa_[2])
        r = vec4(float(pa_[2]) * 8.0, float(pa_[0]) * 8.0,
                 min(float(pa_[3] + 1) * 8.0, frame_size.x), min(float(pa_[1] + 1) * 8.0, frame_size.y));
    imageStore(dst, ivec2(0), r);
}
)GLSL";
}

// Accessors for readers that hold the rect as a vec4 (x0, y0, x1, y1): vpu_in_picture / vpu_picture_coverage.
inline std::string Glsl()
{
    return std::string("#define VPU_PICTURE_RECT_VERSION ") + std::to_string(kUnitVersion) + "
"
        "bool vpu_in_picture(vec4 rect, vec2 px)
{
"
        "    return px.x >= rect.x && px.x < rect.z && px.y >= rect.y && px.y < rect.w;
}
"
        "float vpu_picture_coverage(vec4 rect, vec2 b0, vec2 b1)
{
"
        "    float area = (b1.x - b0.x) * (b1.y - b0.y);
"
        "    float w = min(b1.x, rect.z) - max(b0.x, rect.x);
"
        "    float h = min(b1.y, rect.w) - max(b0.y, rect.y);
"
        "    return (area > 0.0 && w > 0.0 && h > 0.0) ? min(1.0, (w * h) / area) : 0.0;
}
";
}

} // namespace PictureRect
} // namespace VpUnit
