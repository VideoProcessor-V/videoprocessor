#pragma once

// Pulldown detection (owner 2026-10-14; off by default): film carried inside a
// faster signal by repeating whole frames, in ANY combination of rates - no
// rate, cadence or resolution is fixed (owner: everything general). Examples:
// 24p in 60 Hz (2:3), 24p in 30 Hz (one repeat in five, as Crave and the Apple
// TV at 30 Hz sometimes send), 30p in 60, 25p in 50, 24p in 50 (2:2...2:3),
// 25p in 60, 24p or 30p in 120. The capture marks each frame that repeats the
// previous one bit for bit; this finds the shortest cycle the repeats follow,
// locks onto it, says which frames are only repeats (not queued) and gives the
// unique frames an even film clock: film rate = signal rate x new / cycle.
//
// Guarding against a wrong lock (owner: "very much guard against incorrect
// activation"), none of it tied to a rate. A lock needs ALL of:
//  - the cycle repeating exactly, with no exception, for at least two seconds
//    and at least three whole cycles;
//  - the repeats spread evenly, as real pulldown does: every picture held for
//    the same number of frames, or one more;
//  - at least one new and one repeated frame in the cycle;
//  - a film rate of at least 15 fps (so animation drawn on twos or threes and
//    held stills are never taken for film).
// While locked, a new picture where the cycle has a repeat (a subtitle or
// menu drawn by the source device, which changes on any signal frame) is
// shown at once as an update of the same film frame: it does not advance the
// film clock and is not paired for motion (owner 2026-10-14, option b). Only
// sustained contradictions - three within one second, as video after film
// gives - a gap or the signal rate moving more than 0.5% drop the lock; the
// next lock needs a fresh two seconds. Even a wrong lock never drops a new
// picture: only bit-exact repeats are ever skipped.
//
// Audit 2026-10-16 (docs/audit/pulldown-review-2026-10-16.md), each behind a
// Settings switch, on by default, the old behaviour available:
//  - D4: a counted capture miss of at most one cycle advances the phase and
//    keeps the lock, verified over the next cycle (a contradiction in it
//    drops the lock); an uncounted or longer gap still resets.
//  - D3: a phase shift (an edited telecine, pause and restart, a 23.976-in-60
//    slip) re-phases in place: from a contradiction on, the rotations of the
//    locked pattern that match every frame since are followed; while the
//    locked phase is contradicted the frames are decided by those rotations
//    (only exact repeats dropped), and after one full cycle with exactly one
//    rotation left it is adopted - the lock and film clock continue, no 2 s
//    relock. A contradiction in the next cycle drops the lock.
//  - D1: every lock change (gained, lost, restarted, re-phased) and every
//    counted miss leaves a pending discontinuity until ConsumeDiscontinuity()
//    - the renderer consumes it on the next KEPT frame, so a change on a
//    dropped repeat still breaks the motion pair.
//  - D2: FilmToSignalRatio() for resolving a time-based queue target at the
//    film rate while locked.
// Pure logic: no GPU, no capture types.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace Pulldown
{
	// A cheap content signature of a frame: a hash of evenly spaced rows. HDMI
	// delivers a repeated frame bit for bit, so equal signatures are a repeat;
	// a new picture (grain, noise, motion) changes the sampled rows.
	inline uint64_t Signature(const void* data, size_t rowBytes, size_t height, size_t sampleRows = 64)
	{
		if (!data || rowBytes == 0 || height == 0)
			return 0;
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint64_t h = 0x9E3779B97F4A7C15ull ^ (rowBytes * 31 + height);
		const size_t rows = sampleRows < height ? sampleRows : height;
		for (size_t r = 0; r < rows; ++r)
		{
			const uint8_t* row = bytes + ((r * 2 + 1) * height / (rows * 2)) * rowBytes;
			const size_t words = rowBytes / 8;
			for (size_t i = 0; i < words; ++i)
			{
				uint64_t k;
				std::memcpy(&k, row + i * 8, 8);
				h = (h ^ (k * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
				h ^= h >> 29;
			}
		}
		return h == 0 ? 1 : h;
	}

	// The lowest film rate a lock may imply: below it, repeats are artistic
	// (animation on twos, held frames), not a carrier's pulldown.
	constexpr double kMinFilmHz = 15.0;

	// Real pulldown holds every picture for floor(cycle / new) or
	// ceil(cycle / new) frames. `pattern` is one cycle, 'N' new, 'R' repeat.
	inline bool IsEvenPattern(const std::string& pattern)
	{
		int unique = 0;
		for (char c : pattern)
			unique += c == 'N' ? 1 : 0;
		const int length = static_cast<int>(pattern.size());
		if (unique < 1 || unique >= length)
			return false;
		const int shortHold = length / unique, longHold = (length + unique - 1) / unique;
		// Hold lengths around the cycle (it wraps): from each 'N' to the next.
		for (int i = 0; i < length; ++i)
		{
			if (pattern[i] != 'N')
				continue;
			int hold = 1;
			while (pattern[(i + hold) % length] != 'N')
				++hold;
			if (hold < shortHold || hold > longHold)
				return false;
		}
		return true;
	}

	struct Decision
	{
		bool drop = false;        // an expected repeat that is a repeat: not queued
		bool locked = false;
		bool relocked = false;    // the lock (and its clock) started at this frame
		bool update = false;      // a new picture on a repeat slot: the same film frame, updated
		double filmHz = 0.0;      // the content rate when locked
		int cycle = 0;            // frames in the cycle
		int unique = 0;           // new pictures in the cycle
		uint64_t filmIndex = 0;   // counts kept frames while locked (the even clock)
		// Audit 2026-10-16 (D1/D3).
		bool lockChanged = false;   // the lock was gained, lost, restarted or re-phased at this frame
		bool rephased = false;      // the phase moved at this frame; lock and film clock continue (D3)
		bool discontinuity = false; // a lock change or counted miss is pending (ConsumeDiscontinuity)
		// Audit 2026-10-16: how late this kept frame's slot shows its film
		// frame against the earliest new slot of the cycle, and the most of
		// any new slot, in seconds (0 with cadenceAwareClock off): FilmClock.
		double cadenceLateness = 0.0;
		double cadenceLatenessMax = 0.0;
	};

	// Behaviour fixes of the audit 2026-10-16, switchable (owner rule); off
	// gives the 2026-10-14 behaviour.
	struct Settings
	{
		bool keepLockAcrossMisses = true;   // D4: a counted miss <= one cycle keeps the lock
		bool rephaseInPlace = true;         // D3: a phase shift re-phases instead of unlocking
		bool cadenceAwareClock = true;      // Decision::cadenceLateness for an even FilmClock at long cycles
	};

	// "23.976p film in 59.940 Hz (2 new of 5)".
	inline std::string Describe(double filmHz, double signalHz, int unique, int cycle)
	{
		char text[96];
		std::snprintf(text, sizeof(text), "%.3fp film in %.3f Hz (%d new of %d)", filmHz, signalHz, unique, cycle);
		return text;
	}

	class Detector
	{
	public:
		static constexpr int kCyclesToLock = 3;
		static constexpr double kSecondsToLock = 2.0;
		// Sustained contradictions: this many within kContradictionSeconds.
		static constexpr int kContradictionsToUnlock = 3;
		static constexpr double kContradictionSeconds = 1.0;
		static constexpr int kHistory = 2048;

		// The longest cycle looked for: one second of the signal (any real
		// pulldown repeats well within that), within the history.
		static int MaxCycle(double signalHz)
		{
			const int second = signalHz > 0.0 ? static_cast<int>(std::ceil(signalHz)) : 2;
			const int limit = kHistory / kCyclesToLock;
			return second < limit ? second : limit;
		}

		static int FramesToLock(int cycle, double signalHz)
		{
			int need = cycle * kCyclesToLock;
			const int seconds = signalHz > 0.0 ? static_cast<int>(std::ceil(signalHz * kSecondsToLock)) : 0;
			need = seconds > need ? seconds : need;
			return need < kHistory ? need : kHistory;
		}

		void Reset()
		{
			ResetState();
			m_pendingDiscontinuity = false;
		}

		void SetSettings(const Settings& settings) { m_settings = settings; }
		const Settings& GetSettings() const { return m_settings; }

		// Feed every captured frame in order: `repeat` = same content as the
		// previous frame; `gap` = frames were lost just before it; signalHz =
		// the capture's frame rate. A gap or the signal rate moving more than
		// 0.5% (a new mode without a resync) drops the lock and the history; a
		// resync resets the detector from the renderer.
		// Audit 2026-10-16 (D4): `missedFrames` = how many signal frames the
		// gap lost (-1 unknown). With keepLockAcrossMisses, a locked detector
		// told of 1..cycle missed frames advances its phase over them and keeps
		// the lock (verified over the next cycle); `repeat` for this frame is
		// still against the last frame captured. Unknown or longer: reset.
		Decision Next(bool repeat, bool gap, double signalHz, int missedFrames = -1)
		{
			const bool wasLocked = m_locked;
			Decision decision = Decide(repeat, gap, signalHz, missedFrames);
			// Audit 2026-10-16 (D1): a lock change is pending until a kept frame
			// consumes it, so one landing on a dropped repeat is not lost.
			decision.lockChanged = decision.locked != wasLocked || decision.relocked || decision.rephased;
			if (decision.lockChanged)
				m_pendingDiscontinuity = true;
			decision.discontinuity = m_pendingDiscontinuity;
			return decision;
		}

		// Audit 2026-10-16 (D1). The renderer, on every frame it keeps (after
		// `drop` was false - not on dropped repeats):
		//   if (m_pulldown.ConsumeDiscontinuity()) gapBefore = true;   // no motion pair across it
		// and resets its FilmClock when the decision was `relocked` or the lock
		// was lost (not on `rephased`: the clock continues). A frame the
		// renderer drops with its own gap (missed frames, a resync) calls
		// MarkDiscontinuity() so the next kept frame carries the gap.
		bool ConsumeDiscontinuity()
		{
			const bool pending = m_pendingDiscontinuity;
			m_pendingDiscontinuity = false;
			return pending;
		}
		void MarkDiscontinuity() { m_pendingDiscontinuity = true; }
		bool PendingDiscontinuity() const { return m_pendingDiscontinuity; }

		// Audit 2026-10-16 (D2): film frames per signal frame while locked
		// (new / cycle, e.g. 0.4 at 3:2), 1 when not. A locked queue holds film
		// frames, so a time-based queue target resolves at the film rate:
		//   frames = round(targetSeconds * signalHz * FilmToSignalRatio())
		// (200 ms at 24p in 60 = 5 frames, not 12). Re-resolve whenever
		// Decision::lockChanged is set (lock gained / lost changes the ratio).
		double FilmToSignalRatio() const
		{
			return m_locked && !m_pattern.empty() ? static_cast<double>(m_unique) / static_cast<double>(m_pattern.size()) : 1.0;
		}

	private:
		Decision Decide(bool repeat, bool gap, double signalHz, int missedFrames)
		{
			if (m_signalHz > 0.0 && signalHz > 0.0 && std::fabs(signalHz - m_signalHz) > m_signalHz * 0.005)
				ResetState();
			else if (gap && !SkipMissed(missedFrames))
				ResetState();
			if (signalHz > 0.0 && m_signalHz <= 0.0)
				m_signalHz = signalHz;
			++m_frames;
			m_history.push_back(repeat ? 'R' : 'N');
			if (static_cast<int>(m_history.size()) > kHistory)
				m_history.pop_front();

			Decision decision;
			if (m_locked)
			{
				m_phase = (m_phase + 1) % static_cast<int>(m_pattern.size());
				const uint64_t window = static_cast<uint64_t>(std::ceil(
					(signalHz > 0.0 ? signalHz : 60.0) * kContradictionSeconds));
				while (!m_contradictions.empty() && m_frames - m_contradictions.front() >= window)
					m_contradictions.pop_front();
				// Audit 2026-10-16 (D4): the first frame after a counted miss
				// whose missed slots held a new picture, on a repeat slot: new
				// against the frame before the miss = that missed film frame
				// (already counted), shown at once; a repeat = a still.
				const bool carry = m_missCarry;
				m_missCarry = false;
				if (carry && m_pattern[m_phase] == 'R')
					return Locked(decision, signalHz, repeat, false, false, false);
				const char observed = repeat ? 'R' : 'N';
				char expected = m_pattern[m_phase];
				bool contradiction = expected == 'R' && !repeat;
				// Audit 2026-10-16 (D3/D4): the cycle after a re-phase or a
				// counted miss verifies it; a contradiction there drops the lock,
				// except that with re-phasing on one more re-phase may resolve it
				// (a 23.976-in-60 slip inside the window, a miscounted miss) - if
				// that finds no rotation, or its own verification is contradicted,
				// the lock drops.
				if (m_probationUntil > 0 && m_frames > m_probationUntil)
				{
					m_probationUntil = 0;
					m_retrying = false;
				}
				bool unlock = false;
				if (contradiction && m_probationUntil > 0)
				{
					unlock = !m_settings.rephaseInPlace || m_retrying;
					m_retrying = true;
					m_probationUntil = 0;
				}
				if (unlock)
					Unlock();
				else
				{
					bool rephased = false;
					if (m_settings.rephaseInPlace)
						expected = Track(observed, expected, contradiction, rephased);
					if (m_retrying && !rephased && m_trackFrames == 0 && m_probationUntil == 0)
						unlock = true;   // the retry found no rotation
					if (contradiction)
						m_contradictions.push_back(m_frames);
					if (unlock || static_cast<int>(m_contradictions.size()) >= kContradictionsToUnlock)
						Unlock();
					else
					{
						decision.rephased = rephased;
						return Locked(decision, signalHz, expected == 'R' && repeat, false, expected == 'R' && !repeat);
					}
				}
			}

			// Not locked: the shortest cycle the latest frames follow exactly.
			const int size = static_cast<int>(m_history.size());
			const int maxCycle = MaxCycle(signalHz);
			for (int cycle = 2; cycle <= maxCycle; ++cycle)
			{
				const int need = FramesToLock(cycle, signalHz);
				if (size < need)
					break;   // longer cycles need at least as much history
				bool periodic = true;
				for (int i = size - need + cycle; i < size && periodic; ++i)
					periodic = m_history[i] == m_history[i - cycle];
				if (!periodic)
					continue;
				// The shortest period: is it pulldown? (A longer period of the
				// same frames is only this one repeated, so nothing else is.)
				const std::string pattern(m_history.end() - cycle, m_history.end());
				int unique = 0;
				for (char c : pattern)
					unique += c == 'N' ? 1 : 0;
				const double filmHz = signalHz * unique / cycle;
				if (!IsEvenPattern(pattern) || !(filmHz >= kMinFilmHz))
					break;
				m_locked = true;
				m_pattern = pattern;
				m_phase = cycle - 1;   // the latest frame is the pattern's last slot
				m_unique = unique;
				m_filmIndex = 0;
				m_contradictions.clear();
				EndTracking();
				m_probationUntil = 0;
				m_retrying = false;
				m_missCarry = false;
				BuildLateness();
				return Locked(decision, signalHz, repeat && pattern[m_phase] == 'R', true, false);
			}
			return decision;
		}

	public:
		bool Locked() const { return m_locked; }
		int Cycle() const { return m_locked ? static_cast<int>(m_pattern.size()) : 0; }
		int Unique() const { return m_locked ? m_unique : 0; }

	private:
		Decision Locked(Decision& decision, double signalHz, bool drop, bool relocked, bool update)
		{
			return Locked(decision, signalHz, drop, relocked, update, !drop && !update);
		}

		// `advance`: a new film frame (the clock moves on).
		Decision Locked(Decision& decision, double signalHz, bool drop, bool relocked, bool update, bool advance)
		{
			decision.locked = true;
			decision.relocked = relocked;
			decision.update = update;
			decision.cycle = static_cast<int>(m_pattern.size());
			decision.unique = m_unique;
			decision.filmHz = signalHz * m_unique / decision.cycle;
			decision.drop = drop;
			if (advance)
				++m_filmIndex;
			decision.filmIndex = m_filmIndex;
			if (m_settings.cadenceAwareClock && signalHz > 0.0 && m_phase < static_cast<int>(m_lateness.size()))
			{
				decision.cadenceLateness = m_lateness[m_phase] / signalHz;
				decision.cadenceLatenessMax = m_latenessMax / signalHz;
			}
			return decision;
		}

		void Unlock()
		{
			m_locked = false;
			m_pattern.clear();
			m_phase = 0;
			m_unique = 0;
			m_filmIndex = 0;
			m_contradictions.clear();
			EndTracking();
			m_probationUntil = 0;
			m_retrying = false;
			m_missCarry = false;
		}

		// Audit 2026-10-16: per slot, how many signal frames after its film
		// frame's even time the slot shows it (film frame j of the cycle is
		// due at j x cycle / new), less the earliest new slot's; the most over
		// the new slots. A repeat slot shows the latest film frame before it.
		void BuildLateness()
		{
			const int cycle = static_cast<int>(m_pattern.size());
			m_lateness.assign(static_cast<size_t>(cycle), 0.0);
			const double hold = static_cast<double>(cycle) / static_cast<double>(m_unique);
			double earliest = 1e9, latest = -1e9;
			int film = -1;   // a leading repeat shows the previous cycle's last film frame
			for (int k = 0; k < cycle; ++k)
			{
				film += m_pattern[k] == 'N' ? 1 : 0;
				m_lateness[k] = k - film * hold;
				if (m_pattern[k] == 'N')
				{
					earliest = m_lateness[k] < earliest ? m_lateness[k] : earliest;
					latest = m_lateness[k] > latest ? m_lateness[k] : latest;
				}
			}
			for (double& lateness : m_lateness)
				lateness -= earliest;
			m_latenessMax = latest - earliest;
		}

		void ResetState()
		{
			m_history.clear();
			Unlock();
			m_signalHz = 0.0;
		}

		// Audit 2026-10-16 (D4): advance over `missed` lost signal frames and
		// keep the lock. False (the caller resets) when the switch is off, not
		// locked, or the count is unknown or longer than one cycle.
		bool SkipMissed(int missed)
		{
			const int cycle = static_cast<int>(m_pattern.size());
			if (!m_settings.keepLockAcrossMisses || !m_locked || missed <= 0 || missed > cycle)
				return false;
			for (int i = 0; i < missed; ++i)
			{
				m_phase = (m_phase + 1) % cycle;
				if (m_pattern[m_phase] == 'N')
				{
					++m_filmIndex;   // a film frame went by unseen: the clock stays on time
					m_missCarry = true;
				}
			}
			// The missed slots are unknown: a later relock needs fresh frames.
			m_history.clear();
			EndTracking();
			m_probationUntil = m_frames + static_cast<uint64_t>(cycle);   // verify over the next cycle
			m_pendingDiscontinuity = true;
			return true;
		}

		void EndTracking()
		{
			m_trackFrames = 0;
			m_candidates.clear();
			m_currentFailed = false;
			m_absorbed.clear();
		}

		// Tracking found no single rotation: the contradictions it decided by
		// the rotations count after all, so content that only looks like a
		// phase shift (video, another cadence) still unlocks on the usual rule.
		void AbandonTracking()
		{
			for (uint64_t frame : m_absorbed)
				m_contradictions.insert(std::upper_bound(m_contradictions.begin(), m_contradictions.end(), frame), frame);
			EndTracking();
		}

		// Audit 2026-10-16 (D3): re-phase in place. Starts at a contradiction
		// (a new picture on a repeat slot: a subtitle, or the phase moved) with
		// every other phase - the starting frame constrains none, as a restart
		// or seek can begin part-way through a hold; each later frame keeps the
		// phases (the rotations of the pattern) that match it. Once the locked
		// phase has
		// failed again (a single subtitle leaves it matching), the frames are
		// decided by the matching rotations - they all agree with the frame,
		// so only exact repeats are dropped - and their contradictions are held
		// back; after one whole cycle matched exactly with one rotation left
		// (unique: two rotations of the shortest pattern cannot agree over a
		// cycle) it is adopted - the bound on the wrong-time window. No
		// rotation left, or the locked
		// phase never failed again: tracking ends, the held-back contradictions
		// count and the locked phase stands. Returns the slot value to decide this frame with;
		// clears `contradiction` when it is held back or resolved.
		char Track(char observed, char expected, bool& contradiction, bool& rephased)
		{
			const int cycle = static_cast<int>(m_pattern.size());
			bool restart = false, carry = false;
			if (m_trackFrames > 0)
			{
				++m_trackFrames;
				size_t kept = 0;
				for (int candidate : m_candidates)
				{
					const int next = (candidate + 1) % cycle;
					if (m_pattern[next] == observed)
						m_candidates[kept++] = next;
				}
				m_candidates.resize(kept);
				if (observed != expected)
					m_currentFailed = true;
				if (m_candidates.empty())
				{
					// Every rotation died: start afresh from this frame. The first
					// such restart keeps the held-back contradictions (a restart
					// then a slip inside one cycle); after that they count, and only
					// a new picture starts again.
					if (m_restartsLeft > 0)
					{
						--m_restartsLeft;
						carry = true;
						m_trackFrames = 0;
						restart = true;
					}
					else
					{
						AbandonTracking();
						restart = observed == 'N';
					}
				}
				else if (m_trackFrames > cycle)
				{
					if (m_currentFailed && m_candidates.size() == 1)
					{
						// The contradiction that started it stays counted: content
						// that keeps moving phase (24p through a 30p lock) still
						// unlocks on the usual rule instead of re-phasing forever.
						m_phase = m_candidates.front();
						rephased = true;
						contradiction = false;
						m_probationUntil = m_frames + static_cast<uint64_t>(cycle);
						EndTracking();
						return m_pattern[m_phase];
					}
					AbandonTracking();
				}
				else if (m_currentFailed)
				{
					if (contradiction)
					{
						m_absorbed.push_back(m_frames);
						contradiction = false;
					}
					return observed;
				}
				else
					return expected;
			}
			if (contradiction || restart)
			{
				for (int phase = 0; phase < cycle; ++phase)
					if (phase != m_phase)
						m_candidates.push_back(phase);
				m_trackFrames = m_candidates.empty() ? 0 : 1;
				m_currentFailed = false;
				if (!carry)
					m_restartsLeft = 1;
			}
			return expected;
		}

		Settings m_settings;
		bool m_pendingDiscontinuity = false;   // D1: until ConsumeDiscontinuity()
		bool m_missCarry = false;              // D4: the missed slots held a new picture
		uint64_t m_probationUntil = 0;         // D3/D4: verify up to this frame (0 none)
		int m_trackFrames = 0;                 // D3: frames since the contradiction (0 not tracking)
		std::vector<int> m_candidates;         // D3: phases of the latest frame that match since
		bool m_currentFailed = false;          // D3: the locked phase failed after the contradiction
		std::vector<uint64_t> m_absorbed;      // D3: contradictions held back while tracking
		bool m_retrying = false;               // D3/D4: a verification was contradicted once
		int m_restartsLeft = 0;                // D3: restarts that keep the held-back contradictions
		std::vector<double> m_lateness;        // per slot, in signal frames (BuildLateness)
		double m_latenessMax = 0.0;
		std::deque<char> m_history;
		std::deque<uint64_t> m_contradictions;   // frame numbers of recent contradictions
		uint64_t m_frames = 0;
		std::string m_pattern;
		bool m_locked = false;
		int m_phase = 0;
		int m_unique = 0;
		uint64_t m_filmIndex = 0;
		double m_signalHz = 0.0;   // the rate the history was built at
	};

	// The even film clock for kept frames: time(n) = anchor + n / filmHz, the
	// anchor the latest (arrival - n / filmHz) over the recent frames, so no
	// frame is timed before it arrived and the clock follows slow drift.
	class FilmClock
	{
	public:
		static constexpr double kWindowSeconds = 2.0;

		void Reset() { m_offsets.clear(); }

		// arrival and the result in seconds on any steady clock; the anchor
		// over the last kWindowSeconds of film.
		// Audit 2026-10-16: lateness / latenessMax (Decision::cadenceLateness /
		// cadenceLatenessMax; 0 = the 2026-10-14 behaviour) take the cadence's
		// own deterministic lateness out of each offset, so the anchor is the
		// largest arrival jitter, not the latest slot of the few cycles in the
		// window - at long cycles (24p in 50, 24p in 30) that stepped the clock
		// by milliseconds whenever such a slot left the window. latenessMax is
		// added back, and the result is never before the frame arrived.
		double Time(uint64_t filmIndex, double arrival, double filmHz, double lateness = 0.0, double latenessMax = 0.0)
		{
			if (!(filmHz > 0.0))
				return arrival;
			const double ideal = static_cast<double>(filmIndex) / filmHz;
			m_offsets.push_back({ ideal, arrival - lateness - ideal });
			while (m_offsets.size() > 1 && ideal - m_offsets.front().first > kWindowSeconds)
				m_offsets.pop_front();
			double anchor = m_offsets.front().second;
			for (const auto& offset : m_offsets)
				anchor = offset.second > anchor ? offset.second : anchor;
			const double time = anchor + ideal + latenessMax;
			return time > arrival ? time : arrival;
		}

	private:
		std::deque<std::pair<double, double>> m_offsets;   // (ideal time, arrival - ideal)
	};
}
