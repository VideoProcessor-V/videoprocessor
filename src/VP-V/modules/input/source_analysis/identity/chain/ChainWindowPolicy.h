// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// WindowClearPolicy (SID revert window, owner-approved design): WHEN the monitor asks the chain estimator to clear its evidence window
// (EstimatorEvent::ClearWindow). One call per analysed monitor frame, from the analysis worker, with two facts the monitor already has:
//   mixed  the classical mixed-screen read of this frame (a UI / menu / overlay around or over the video: SourceResolution::FrameReading::mixed)
//   cut    a cut was detected on this frame (the same flag that feeds EstimatorEvent::Cut)
// The window is cleared at
//   - the END of a mixed episode (the first frame that is not mixed after one or more mixed frames), and
//   - the first CUT after a mixed episode (a cut on a frame that is not mixed, once an episode has happened since the previous such cut).
// It is NOT cleared by a black / dark frame, by an ordinary cut (no episode since the last clear-on-cut), or while the episode lasts (a cut
// inside a menu is not "after" it; the end of the episode clears). Resyncs, format changes and discontinuities clear through the existing
// NoteDiscontinuity path and are not this policy's business. Frame-rate and cadence changes are not observable here (see chain/README.md).
//
// Pure, no clock, no frame counts; header-only, C++14.
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
				bool episodeEnded = false;     // cause: the first non-mixed frame after a mixed episode
				bool cutAfterEpisode = false;  // cause: the first cut after a mixed episode
			};

			Result Frame(bool mixed, bool cut)
			{
				Result r;
				if (mixed)
				{
					episodeSinceCut_ = true;   // an episode is going on (or just happened): the next cut after it clears
				}
				else
				{
					if (wasMixed_)
					{
						r.episodeEnded = true;
						r.clear = true;
					}
					if (cut && episodeSinceCut_)
					{
						r.cutAfterEpisode = true;
						r.clear = true;
						episodeSinceCut_ = false;   // the first cut after the episode is consumed
					}
				}
				wasMixed_ = mixed;
				return r;
			}

			void Reset()
			{
				wasMixed_ = false;
				episodeSinceCut_ = false;
			}

			bool InEpisode() const { return wasMixed_; }
			bool EpisodePending() const { return episodeSinceCut_; }

		private:
			bool wasMixed_ = false;           // the previous analysed frame was mixed
			bool episodeSinceCut_ = false;    // a mixed episode happened since the last cut that consumed it
		};
	}
}
