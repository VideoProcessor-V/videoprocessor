#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The LED measurement's readback schedule (module findings 2026-10-24, part 3 item 1).
//
// Why: on D3D11, libplacebo's pl_buf_poll always reports "ready" and pl_buf_read is a
// CopyResource followed by a blocking Map, so collecting the measurement right after the render
// made the render thread wait for the whole GPU frame, every frame. The hook now copies each
// frame's results into its own staging slot right after the measurement pass and reads a slot
// only once the GPU says that copy is done (a non-blocking query). It never waits.
//
// This class is the schedule alone: pure, no GPU, CPU-tested. N slots in submission order:
//  - Claim(frame): the next slot for this frame's results, or -1 when every slot still holds an
//    unread result (that frame is dropped, the controllers keep the last statistics);
//  - Collect(frame, ready): the slots whose results can be read now, oldest first. It stops at
//    the first slot that is not ready, so results always arrive in frame order;
//  - Release(slot) after reading it;
//  - a pending result older than `lateFrames` counts as late (once per result), and Note() says
//    so while it is happening, for the "Fallbacks engaged" line (visible, never log-only).
namespace LedDrive
{
	class ReadbackRing
	{
	public:
		ReadbackRing(int slots, int lateFrames)
			: claimedAt(slots > 0 ? static_cast<size_t>(slots) : 1u, 0), pending(claimedAt.size(), false),
			  lateCounted(claimedAt.size(), false), late(lateFrames > 0 ? lateFrames : 1) {}

		int Slots() const { return static_cast<int>(claimedAt.size()); }

		int Claim(uint64_t frame)
		{
			const size_t slot = next;
			if (pending[slot])
			{
				++dropped;
				++droppedRecent;
				return -1;
			}
			pending[slot] = true;
			lateCounted[slot] = false;
			claimedAt[slot] = frame;
			order.push_back(static_cast<int>(slot));
			next = (next + 1) % claimedAt.size();
			return static_cast<int>(slot);
		}

		// `ready(slot)` must not block. Returns the slots to read now, oldest first.
		template <class Ready>
		std::vector<int> Collect(uint64_t frame, Ready ready)
		{
			std::vector<int> out;
			runningLate = false;
			for (size_t i = 0; i < order.size(); ++i)
			{
				const int slot = order[i];
				if (ready(slot))
				{
					out.push_back(slot);
					continue;
				}
				if (frame >= claimedAt[slot] + static_cast<uint64_t>(late))
				{
					runningLate = true;
					if (!lateCounted[slot])
					{
						lateCounted[slot] = true;
						++lateResults;
					}
				}
				break;   // keep frame order: nothing after an unready slot is handed out
			}
			return out;
		}

		void Release(int slot)
		{
			if (slot < 0 || static_cast<size_t>(slot) >= pending.size() || !pending[slot])
				return;
			pending[slot] = false;
			for (size_t i = 0; i < order.size(); ++i)
				if (order[i] == slot)
				{
					order.erase(order.begin() + static_cast<std::ptrdiff_t>(i));
					break;
				}
		}

		void Reset()
		{
			for (size_t i = 0; i < pending.size(); ++i)
				pending[i] = lateCounted[i] = false;
			order.clear();
			next = 0;
			runningLate = false;
			droppedRecent = 0;
		}

		int Pending() const { return static_cast<int>(order.size()); }
		uint64_t Dropped() const { return dropped; }
		uint64_t LateResults() const { return lateResults; }
		bool RunningLate() const { return runningLate; }

		// "" while results arrive in time; otherwise what the controllers see. Clears the recent
		// drop count it reports.
		std::string Note()
		{
			std::string note;
			if (runningLate)
				note = "readbacks late: the LED follows statistics more than " + std::to_string(late) + " frames old";
			else if (droppedRecent > 0)
				note = "readbacks behind: " + std::to_string(droppedRecent) + " frames not measured";
			droppedRecent = 0;
			return note;
		}

	private:
		std::vector<uint64_t> claimedAt;
		std::vector<bool> pending;
		std::vector<bool> lateCounted;
		std::vector<int> order;
		size_t next = 0;
		int late;
		bool runningLate = false;
		uint64_t dropped = 0;
		uint64_t droppedRecent = 0;
		uint64_t lateResults = 0;
	};
}
