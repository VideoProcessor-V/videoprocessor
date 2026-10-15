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
// Pure logic: no GPU, no capture types.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>

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
			m_history.clear();
			Unlock();
			m_signalHz = 0.0;
		}

		// Feed every captured frame in order: `repeat` = same content as the
		// previous frame; `gap` = frames were lost just before it; signalHz =
		// the capture's frame rate. A gap or the signal rate moving more than
		// 0.5% (a new mode without a resync) drops the lock and the history; a
		// resync resets the detector from the renderer.
		Decision Next(bool repeat, bool gap, double signalHz)
		{
			if (gap || (m_signalHz > 0.0 && signalHz > 0.0 && std::fabs(signalHz - m_signalHz) > m_signalHz * 0.005))
				Reset();
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
				const char expected = m_pattern[m_phase];
				const bool contradiction = expected == 'R' && !repeat;
				const uint64_t window = static_cast<uint64_t>(std::ceil(
					(signalHz > 0.0 ? signalHz : 60.0) * kContradictionSeconds));
				while (!m_contradictions.empty() && m_frames - m_contradictions.front() >= window)
					m_contradictions.pop_front();
				if (contradiction)
					m_contradictions.push_back(m_frames);
				if (static_cast<int>(m_contradictions.size()) >= kContradictionsToUnlock)
					Unlock();
				else
					return Locked(decision, signalHz, expected == 'R' && repeat, false, contradiction);
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
				return Locked(decision, signalHz, repeat && pattern[m_phase] == 'R', true, false);
			}
			return decision;
		}

		bool Locked() const { return m_locked; }
		int Cycle() const { return m_locked ? static_cast<int>(m_pattern.size()) : 0; }
		int Unique() const { return m_locked ? m_unique : 0; }

	private:
		Decision Locked(Decision& decision, double signalHz, bool drop, bool relocked, bool update)
		{
			decision.locked = true;
			decision.relocked = relocked;
			decision.update = update;
			decision.cycle = static_cast<int>(m_pattern.size());
			decision.unique = m_unique;
			decision.filmHz = signalHz * m_unique / decision.cycle;
			decision.drop = drop;
			if (!drop && !update)
				++m_filmIndex;
			decision.filmIndex = m_filmIndex;
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
		}

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
		double Time(uint64_t filmIndex, double arrival, double filmHz)
		{
			if (!(filmHz > 0.0))
				return arrival;
			const double ideal = static_cast<double>(filmIndex) / filmHz;
			m_offsets.push_back({ ideal, arrival - ideal });
			while (m_offsets.size() > 1 && ideal - m_offsets.front().first > kWindowSeconds)
				m_offsets.pop_front();
			double anchor = m_offsets.front().second;
			for (const auto& offset : m_offsets)
				anchor = offset.second > anchor ? offset.second : anchor;
			return anchor + ideal;
		}

	private:
		std::deque<std::pair<double, double>> m_offsets;   // (ideal time, arrival - ideal)
	};
}
