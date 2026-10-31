// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Source-ID consensus, part 3: ABR boundary timing and quality-step evidence (input.source_identity).
//
// Adaptive-bitrate players switch renditions at segment boundaries. A rendition switch shows up as
// a STEP in the frame-level quality reading (and sometimes in the delivered resolution). This
// detector finds such steps in a stream of per-frame samples and reports them as AbrEvents, so the
// consensus can shorten its dwell (a switch is sudden) and the notice policy can tell the user
// about a persisted quality drop.
//
// Method (deterministic, no clock, no randomness): samples go into a bounded buffer in SOURCE time.
// At every Push the valid samples of the last (pre + post) window are split at "now - post":
//   post = (now - postWindowUs, now], pre = (now - postWindowUs - preWindowUs, now - postWindowUs].
// A step needs |median(post) - median(pre)| >= minStep with BOTH windows stable (median absolute
// deviation <= stableMad), at least minSamples valid samples in each, and the post window's valid
// samples spanning >= confirmUs (the new level has persisted). The boundary is the first sample of
// the newest run that sits on the post side of the midpoint, and it must reach back over the whole
// post window (a half-new window is not yet a clean step). One boundary yields one event.
//
// Excluded (evidence neither way): blank and fade samples, and samples whose mean luma is below
// darkApl (quality is unreliable in dark scenes).
// Cut confound: a step landing on (or next to) a cut sample may be the content changing rather
// than the rendition. It is only emitted when the delivered ratio steps with it (>= minRatioStep)
// or the boundary lies on the measured segment grid; otherwise it is counted as confounded.
// Segment period: MEASURED from the intervals between accepted steps (two consecutive intervals
// that agree within gridToleranceUs and fall inside the plausibility range). segmentMinUs /
// segmentMaxUs only say which measured periods are plausible; they are not assumed rates.
//
// Future codecs (AV1, VVC, LCEVC): their switch behaviour (e.g. an LCEVC enhancement layer can
// drop independently of its base, a quality step without a resolution step) must be MEASURED on
// real material; this detector carries no codec-specific constants.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace SourceIdentity
{
	struct QualitySample
	{
		uint64_t seq = 0;
		int64_t timeUs = 0;           // source time, non-decreasing
		double quality = 1.0;         // frame-level quality, 0..1
		double deliveredRatio = 0.0;  // live resolution reading (<= 0 = none)
		double apl = 0.5;             // mean luma 0..1
		bool blank = false;
		bool cut = false;
		bool fade = false;
	};

	enum class AbrDirection { Drop, Rise };

	struct AbrEvent
	{
		uint64_t seq = 0;             // sequence of the boundary sample
		int64_t timeUs = 0;           // source time of the boundary (first sample on the new level)
		AbrDirection direction = AbrDirection::Drop;
		double magnitude = 0.0;       // |median(post) - median(pre)| of quality
		double ratioStep = 0.0;       // median(post) - median(pre) of the delivered ratio (0 = none)
		double confidence = 0.0;      // 0..1
		bool onGrid = false;          // the boundary lies on the measured segment grid
		bool cutConfound = false;     // it sat on a cut and was emitted anyway (ratio step or grid)
	};

	inline std::string SerializeEvent(const AbrEvent& e)
	{
		std::ostringstream o;
		o.setf(std::ios::fixed);
		o.precision(4);
		o << e.seq << ',' << e.timeUs << ',' << (e.direction == AbrDirection::Drop ? "drop" : "rise") << ','
			<< e.magnitude << ',' << e.ratioStep << ',' << e.confidence << ',' << (e.onGrid ? 1 : 0) << ','
			<< (e.cutConfound ? 1 : 0);
		return o.str();
	}

	class AbrStepDetector
	{
	public:
		struct Settings
		{
			int64_t preWindowUs = 1500000;     // median window before the step
			int64_t postWindowUs = 1500000;    // median window after the step
			double minStep = 0.20;             // smallest quality step (0..1 scale)
			double minRatioStep = 0.10;        // smallest delivered-ratio step that counts as concurrent
			double darkApl = 0.04;             // samples below this mean luma are excluded
			double stableMad = 0.04;           // both windows must have a MAD no larger than this
			int64_t confirmUs = 750000;        // the post window's valid samples must span this long
			int64_t segmentMinUs = 1000000;    // plausibility of a MEASURED segment period (not an assumption)
			int64_t segmentMaxUs = 10000000;
			int64_t gridToleranceUs = 400000;  // agreement of intervals / distance from the grid
			int64_t minEventGapUs = 1000000;   // boundaries closer than this are one boundary
			int minSamples = 4;                // valid samples needed in each window
			size_t maxHistory = 16;            // accepted boundaries kept for the period estimate
			size_t maxSamples = 4096;          // hard cap on the buffer
		};

		AbrStepDetector() {}
		explicit AbrStepDetector(const Settings& s) : s_(s) {}

		// Returns true and fills `out` when a step is confirmed by this sample.
		bool Push(const QualitySample& sample, AbrEvent& out)
		{
			if (!samples_.empty() && sample.timeUs < samples_.back().timeUs)
				Reset();                                // time went backwards: a new source
			samples_.push_back(sample);
			Trim(sample.timeUs);

			const int64_t now = sample.timeUs;
			const int64_t split = now - s_.postWindowUs;
			const int64_t lo = split - s_.preWindowUs;

			std::vector<size_t> pre, post;              // ring indices of the valid samples
			std::vector<size_t> all;
			for (size_t i = 0; i < samples_.size(); ++i)
			{
				const QualitySample& q = samples_[i];
				if (q.timeUs <= lo || !Valid(q))
					continue;
				all.push_back(i);
				if (q.timeUs > split)
					post.push_back(i);
				else
					pre.push_back(i);
			}
			const size_t minN = static_cast<size_t>(s_.minSamples > 1 ? s_.minSamples : 1);
			if (pre.size() < minN || post.size() < minN)
				return false;
			if (samples_[post.back()].timeUs - samples_[post.front()].timeUs < s_.confirmUs)
				return false;

			std::vector<double> qPre, qPost;
			for (size_t i = 0; i < pre.size(); ++i) qPre.push_back(samples_[pre[i]].quality);
			for (size_t i = 0; i < post.size(); ++i) qPost.push_back(samples_[post[i]].quality);
			const double medPre = Median(qPre);
			const double medPost = Median(qPost);
			const double delta = medPost - medPre;
			const double magnitude = std::fabs(delta);
			if (magnitude < s_.minStep)
				return false;
			const double madPre = Mad(qPre, medPre);
			const double madPost = Mad(qPost, medPost);
			if (madPre > s_.stableMad || madPost > s_.stableMad)
				return false;

			// Boundary: the newest run of samples on the post side of the midpoint.
			const bool drop = delta < 0.0;
			const double mid = 0.5 * (medPre + medPost);
			size_t run = 0;
			for (size_t k = all.size(); k > 0; --k)
			{
				const double q = samples_[all[k - 1]].quality;
				if (drop ? (q <= mid) : (q >= mid))
					++run;
				else
					break;
			}
			if (run < post.size() || run >= all.size())
				return false;                            // not a clean step yet
			const size_t boundaryRing = all[all.size() - run];
			const QualitySample& boundary = samples_[boundaryRing];
			const int64_t tb = boundary.timeUs;

			if (haveCandidate_ && tb - lastCandidateUs_ < s_.minEventGapUs && lastCandidateUs_ - tb < s_.minEventGapUs)
				return false;                            // the same boundary seen again

			// Concurrent delivered-ratio step.
			double ratioStep = 0.0;
			{
				std::vector<double> rPre, rPost;
				for (size_t i = 0; i < pre.size(); ++i)
					if (samples_[pre[i]].deliveredRatio > 0.0) rPre.push_back(samples_[pre[i]].deliveredRatio);
				for (size_t i = 0; i < post.size(); ++i)
					if (samples_[post[i]].deliveredRatio > 0.0) rPost.push_back(samples_[post[i]].deliveredRatio);
				if (rPre.size() >= minN && rPost.size() >= minN)
					ratioStep = Median(rPost) - Median(rPre);
			}
			const bool ratioMoved = std::fabs(ratioStep) >= s_.minRatioStep;

			// Segment grid (period measured from earlier accepted boundaries).
			bool onGrid = false;
			if (periodUs_ > 0 && !history_.empty())
			{
				const double dt = static_cast<double>(tb - history_.back());
				const double p = static_cast<double>(periodUs_);
				const double k = std::floor(dt / p + 0.5);
				if (k >= 1.0)
					onGrid = std::fabs(dt - k * p) <= static_cast<double>(s_.gridToleranceUs);
			}

			// Cut confound: the boundary sample or a neighbour carries a cut flag.
			bool cut = boundary.cut;
			if (boundaryRing > 0 && samples_[boundaryRing - 1].cut) cut = true;
			if (boundaryRing + 1 < samples_.size() && samples_[boundaryRing + 1].cut) cut = true;

			haveCandidate_ = true;
			lastCandidateUs_ = tb;
			if (cut && !ratioMoved && !onGrid)
			{
				++confounded_;
				return false;
			}

			double confidence = 0.7 * Clamp01Local(magnitude / (1.5 * s_.minStep));
			if (onGrid) confidence += 0.2;
			if (ratioMoved) confidence += 0.1;
			out = AbrEvent();
			out.seq = boundary.seq;
			out.timeUs = tb;
			out.direction = drop ? AbrDirection::Drop : AbrDirection::Rise;
			out.magnitude = magnitude;
			out.ratioStep = ratioStep;
			out.confidence = Clamp01Local(confidence);
			out.onGrid = onGrid;
			out.cutConfound = cut;

			history_.push_back(tb);
			while (history_.size() > s_.maxHistory)
				history_.erase(history_.begin());
			UpdatePeriod();
			return true;
		}

		// A new source: forget the buffer, the boundaries and the measured period.
		void Reset()
		{
			samples_.clear();
			history_.clear();
			periodUs_ = 0;
			haveCandidate_ = false;
			lastCandidateUs_ = 0;
			confounded_ = 0;
		}

		int64_t MeasuredPeriodUs() const { return periodUs_; }      // 0 = not measured yet
		uint64_t Confounded() const { return confounded_; }         // cut-confounded steps, not emitted
		const Settings& GetSettings() const { return s_; }

	private:
		bool Valid(const QualitySample& q) const { return !q.blank && !q.fade && q.apl >= s_.darkApl; }

		static double Clamp01Local(double x) { return x > 0.0 ? (x < 1.0 ? x : 1.0) : 0.0; }

		static double Median(std::vector<double> v)
		{
			if (v.empty())
				return 0.0;
			std::sort(v.begin(), v.end());
			const size_t n = v.size();
			return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
		}

		static double Mad(const std::vector<double>& v, double median)
		{
			std::vector<double> d;
			for (size_t i = 0; i < v.size(); ++i)
				d.push_back(std::fabs(v[i] - median));
			return Median(d);
		}

		void Trim(int64_t now)
		{
			const int64_t keepFrom = now - (s_.preWindowUs + 2 * s_.postWindowUs);
			size_t drop = 0;
			while (drop < samples_.size() && samples_[drop].timeUs < keepFrom)
				++drop;
			if (samples_.size() - drop > s_.maxSamples)
				drop = samples_.size() - s_.maxSamples;
			if (drop > 0)
				samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(drop));
		}

		// Two consecutive intervals between accepted boundaries that agree and are plausible.
		void UpdatePeriod()
		{
			const size_t n = history_.size();
			if (n < 3)
				return;
			const int64_t a = history_[n - 2] - history_[n - 3];
			const int64_t b = history_[n - 1] - history_[n - 2];
			const int64_t diff = a > b ? a - b : b - a;
			if (diff > s_.gridToleranceUs)
				return;
			if (a < s_.segmentMinUs || a > s_.segmentMaxUs || b < s_.segmentMinUs || b > s_.segmentMaxUs)
				return;
			periodUs_ = (a + b) / 2;
		}

		Settings s_;
		std::vector<QualitySample> samples_;
		std::vector<int64_t> history_;       // accepted boundaries
		int64_t periodUs_ = 0;
		bool haveCandidate_ = false;
		int64_t lastCandidateUs_ = 0;
		uint64_t confounded_ = 0;
	};
}
