#pragma once

#include "AlphaSourceCropPolicy.h"

// Output columns and rows the display cannot show cleanly (for example a
// colour-cast light-engine edge). The picture is laid out inside what remains;
// the blanked strips stay part of the cleared black matte. Insets are output
// pixels before the lens, like screen_edge_padding.
namespace EdgeBlanking
{
    constexpr int MAX_INSET_PIXELS = 4000;

    struct Insets
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    inline bool Any(const Insets& insets)
    {
        return insets.left > 0 || insets.top > 0 ||
            insets.right > 0 || insets.bottom > 0;
    }

    inline bool operator==(const Insets& a, const Insets& b)
    {
        return a.left == b.left && a.top == b.top &&
            a.right == b.right && a.bottom == b.bottom;
    }

    inline bool operator!=(const Insets& a, const Insets& b) { return !(a == b); }

    struct Result
    {
        AlphaSourceCrop::PresentationRect rect;
        bool applied = false;
        // Insets that would leave less than half of either axis are ignored so
        // a mistyped value can never shrink the picture to nothing.
        bool rejected = false;
    };

    inline Result Apply(const AlphaSourceCrop::PresentationRect& available,
        const Insets& insets)
    {
        Result result;
        result.rect = available;
        if (!Any(insets) || insets.left < 0 || insets.top < 0 ||
            insets.right < 0 || insets.bottom < 0)
            return result;
        const double width = available.right - available.left;
        const double height = available.bottom - available.top;
        const double remainingWidth = width - insets.left - insets.right;
        const double remainingHeight = height - insets.top - insets.bottom;
        if (width <= 0.0 || height <= 0.0 ||
            remainingWidth < width * 0.5 || remainingHeight < height * 0.5)
        {
            result.rejected = true;
            return result;
        }
        result.rect.left += insets.left;
        result.rect.top += insets.top;
        result.rect.right -= insets.right;
        result.rect.bottom -= insets.bottom;
        result.applied = true;
        return result;
    }

    inline double BlankedAspect(double width, double height, const Insets& insets)
    {
        const Result blanked = Apply({ 0.0, 0.0, width, height }, insets);
        const double h = blanked.rect.bottom - blanked.rect.top;
        return h > 0.0 ? (blanked.rect.right - blanked.rect.left) / h : 0.0;
    }
}
