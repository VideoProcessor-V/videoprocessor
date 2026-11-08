// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// WindowClearPolicy (SID revert window, owner-approved design): WHEN the monitor asks the chain estimator to clear its evidence window
// (EstimatorEvent::ClearWindow). One call per analysed monitor frame, from the analysis worker, with what the monitor already has:
//   mixed    the classical mixed-screen read of this frame (a UI / menu / overlay around or over the video: SourceResolution::FrameReading::mixed)
//   cut      a cut was detected on this frame (the same flag that feeds EstimatorEvent::Cut)
//   seconds  the frame's source time
//   minSec   the shortest mixed episode that counts: one ACQUIRING interval of source time (the estimator's own, measured plan; never a frame count)
// A mixed episode is debounced (audit M2): mixed frames at most minSec apart belong to ONE episode (a flickering menu is one episode), and
// the episode COUNTS only once it has spanned minSec from its first to its last mixed frame. A one-frame or sub-interval mixed blip mid-scene
// (a sharp tile, a grain frame) is nothing: it clears nothing and arms nothing, so N2's hold of native-looking frames is not weakened by it.
// The window is cleared at
//   - the END of a counted episode (the first frame that is not mixed after its mixed frames), at most ONCE per episode, and
//   - the first CUT on a frame that is not mixed after a counted episode (once per episode).
// It is NOT cleared by a black / dark frame, by an ordinary cut (no counted episode since the last clear-on-cut), by a blip, or by a cut
// inside the episode (the end clears). Resyncs, format changes and discontinuities clear through the existing NoteDiscontinuity path.
// Frame-rate and cadence changes are not observable here (chain/README.md).
//
// Pure, no clock of its own, no frame counts; header-only, C++14.
namespace SourceIdentity
{
	namespace Chain
	{
		class WindowClearPolicy
		{
		public:
			struct Result
			{
				bool clear = false;            // ask the estimator to clear the window now
				bool episodeEnded = false;     // cause: the first non-mixed frame after a counted episode
				bool cutAfterEpisode = false;  // cause: the first cut after a counted episode
			};

			Result Frame(bool mixed, bool cut, double seconds, double minSec)
			{
				Result r;
				if (mixed)
				{
					if (!active_ || seconds - lastMixed_ > minSec)
					{
						active_ = true;          // a new episode (more than minSec since the last mixed frame)
						start_ = seconds;
						counted_ = false;
						cleared_ = false;
					}
					lastMixed_ = seconds;
					if (lastMixed_ - start_ >= minSec)
					{
						counted_ = true;         // it has lasted one acquiring interval: a menu / overlay, not a blip
						cutArmed_ = true;
					}
					wasMixed_ = true;
					return r;
				}
				if (wasMixed_ && active_ && counted_ && !cleared_)
				{
					r.episodeEnded = true;
					r.clear = true;
					cleared_ = true;             // a flicker clears once
				}
				if (cut && cutArmed_)
				{
					r.cutAfterEpisode = true;
					r.clear = true;
					cutArmed_ = false;           // the first cut after the episode is consumed
				}
				wasMixed_ = false;
				return r;
			}

			void Reset()
			{
				wasMixed_ = false;
				active_ = false;
				counted_ = false;
				cleared_ = false;
				cutArmed_ = false;
				start_ = 0.0;
				lastMixed_ = 0.0;
			}

			bool InEpisode() const { return wasMixed_; }
			bool CutArmed() const { return cutArmed_; }

		private:
			bool wasMixed_ = false;     // the previous analysed frame was mixed
			bool active_ = false;       // an episode has started (its mixed frames are at most minSec apart)
			bool counted_ = false;      // it has spanned minSec
			bool cleared_ = false;      // its end already asked for a clear
			bool cutArmed_ = false;     // a counted episode happened: the next cut on a non-mixed frame clears
			double start_ = 0.0, lastMixed_ = 0.0;
		};
	}
}
