#pragma once

// active_picture_lookahead_frames follows the queue (owner decision 2026-10-17,
// the same "queue = look-ahead" rule as Dynamic Drive). Pure, CPU-tested.
//
// - An explicit value (the active queue profile, the default queue section or the
//   command line) always wins, an explicit 0 included.
// - Absent everywhere: the normal queue looks ahead over what it holds,
//   min(target_frames - 1, queue_size - 1, max); the low-latency path (nothing is
//   held ahead) looks ahead 0.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace QueueLookahead
{
    // The renderer's own bound on the active-picture look-ahead.
    constexpr size_t kMaxActivePictureLookahead = 8;

    inline size_t ResolveActivePictureLookahead(bool configured, size_t value, bool lowLatencyPath,
        size_t targetFrames, size_t queueSize, size_t maxFrames = kMaxActivePictureLookahead)
    {
        if (configured)
            return (std::min)(value, maxFrames);
        if (lowLatencyPath)
            return 0;
        const size_t fromTarget = targetFrames > 0 ? targetFrames - 1 : 0;
        const size_t fromQueue = queueSize > 0 ? queueSize - 1 : 0;
        return (std::min)((std::min)(fromTarget, fromQueue), maxFrames);
    }

    // Bug hunt 3 F2: the frames the active-picture look-ahead needs the locked-film queue
    // to keep (LockedFilmQueueTarget's floor; 0 = no floor of its own). An explicit value
    // looks that far ahead whatever the queue holds: lookahead + 1 (A shown). The automatic
    // look-ahead follows the queue it is given (it looks over what is queued), so it holds
    // no floor: target - 1 resolved for the signal would otherwise keep a locked queue at
    // the signal's target and cancel the film scaling.
    inline size_t LockedFilmLookaheadFloor(size_t lookahead, bool automatic)
    {
        if (automatic || lookahead == 0)
            return 0;
        return lookahead + 1;
    }

    // Bug hunt 3 F8: a queue section that holds an explicit active_picture_lookahead_frames: 0
    // (older Config queue presets wrote 0 on every preset) never gets the automatic look-ahead.
    // Not migrated (user data): Config shows this notice and the owner decides.
    inline bool ExplicitZeroLookahead(const std::string& value)
    {
        size_t begin = 0, end = value.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
        if (begin == end)
            return false;   // absent: automatic
        for (size_t i = begin; i < end; ++i)
            if (value[i] != '0')
                return false;
        return true;
    }
    constexpr const char* kExplicitZeroLookaheadNotice =
        "This profile sets Active-picture lookahead to 0 explicitly (older Config queue presets saved 0), so "
        "the automatic look-ahead (which follows the queue) does not apply. Choose Auto to use it, or keep 0 to "
        "leave the look-ahead off.";
}
