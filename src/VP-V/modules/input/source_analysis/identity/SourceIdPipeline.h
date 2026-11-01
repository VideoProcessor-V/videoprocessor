// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The source-resolution worker's side of P14 (doc 61 section 2.2, SID-2): for each analysed frame, build the frame-level quality
// sample, run the ABR step detector, submit the classical votes, evaluate the consensus and publish the held-state record.
//
// Owned and called by ONE thread (the monitor's analysis worker, under its evidence mutex); the HeldState it publishes into is
// the only thing other threads touch. Rebase requests the consensus makes go through a DeferredIndicatorPort: they are recorded
// in the record as per-axis generations and the RENDER side applies each exactly once (RebaseApplier), so this thread never
// writes the Indicator. The one-time notice text is kept here for the render thread to take (TakeNotice).
//
// Quality field: the classical monitor's smoothed bitrate quality is combined with the event-anchored stream LEVEL (1 until an
// ABR event; a confirmed Drop lowers it by the event's magnitude, a Rise raises it), so a rendition switch reaches the quality
// tier within one dwell and does not drift back while the stream stays low. Header-only, C++14, no clock.
#include <VP-V/modules/input/source_analysis/identity/AbrNoticePolicy.h>
#include <VP-V/modules/input/source_analysis/identity/AbrStepDetector.h>
#include <VP-V/modules/input/source_analysis/identity/FrameQuality.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdConsensus.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdProduct.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace SourceIdentity
{
	struct PipelineSettings
	{
		Settings consensus;
		AbrStepDetector::Settings detector;
		NoticePolicy::Settings notice;
		VoteSettings vote;
		FrameQuality::Settings quality;
		// A record older than this is stale. 0 = derived: twice the longest classical vote expiry (never a fixed number).
		int64_t maxAgeUs = 0;

		int64_t MaxAgeUs() const
		{
			if (maxAgeUs > 0)
				return maxAgeUs;
			int64_t longest = 0;
			for (int v = 0; v <= static_cast<int>(VoterId::BitrateQuality); ++v)
				longest = (std::max)(longest, consensus.maxEvidenceAgeUs[v]);
			return 2 * longest;
		}
	};

	// Records the consensus's rebase requests instead of acting on them (the render side applies them, once per generation).
	class DeferredIndicatorPort : public IndicatorPort
	{
	public:
		struct Request { int axis; uint64_t fromSeq; uint8_t cause; };
		void AdvanceRebase(int axis, uint64_t fromSeq, const char* cause) override
		{
			Request r;
			r.axis = axis;
			r.fromSeq = fromSeq;
			r.cause = RebaseCauseCode(cause ? cause : "");
			requests_.push_back(r);
		}
		std::vector<Request> TakeRequests()
		{
			std::vector<Request> out;
			out.swap(requests_);
			return out;
		}

	private:
		std::vector<Request> requests_;
	};

	struct FrameInput
	{
		uint64_t seq = 0;
		int64_t timeUs = 0;                         // source (frame clock) time, non-decreasing
		uint64_t streamKey = 0;                     // the Indicator's FULL/era counter (0 = not driven)
		ClassicalReading classical;                 // the monitor snapshot, copied by the call site
		double deliveredRatio = 0.0;                // THIS frame's reading (<= 0 = none)
		std::vector<FrameQuality::TileNoise> noise; // the frame's informative tiles, measured by FrameQuality::MeasureNoise
		double apl = 0.5;                           // mean luma 0..1
		bool blank = false, cut = false, fade = false;
		int nativeHeight = 0;                       // for the notice wording (0 = unknown)
	};

	class Pipeline
	{
	public:
		Pipeline() : Pipeline(PipelineSettings()) {}
		explicit Pipeline(const PipelineSettings& s)
			: s_(s), consensus_(s.consensus), detector_(s.detector), notice_(s.notice), signal_(s.quality) {}

		void SetConsensusOn(bool on) { consensusOn_ = on; }
		void SetNoticeEnabled(bool on)
		{
			s_.notice.enabled = on;
			notice_ = NoticePolicy(s_.notice);
			if (haveTitle_)
				notice_.NewTitle(titleKey_);
		}
		const PipelineSettings& Settings_() const { return s_; }

		// FULL / new title or source: everything back to native/unknown, the held records dropped. `streamKey` is the Indicator's
		// FULL/era counter (it re-arms the one-time notice).
		void Discontinuity(uint64_t seq, uint64_t streamKey)
		{
			consensus_.Discontinuity(seq);
			detector_.Reset();
			signal_.Reset();
			level_ = 1.0;
			haveEvent_ = false;
			latchedMask_ = 0;
			rebaseGen_[0] = rebaseGen_[1] = 0;
			rebaseCause_[0] = rebaseCause_[1] = 0;
			noticeFired_ = false;
			++discontinuities_;
			haveTitle_ = true;
			titleKey_ = streamKey;
			notice_.NewTitle(streamKey);
			held_.Reset();
			lastQuality_ = 1.0;
		}

		// One analysed frame. Publishes the record and returns it.
		Record Push(const FrameInput& in)
		{
			if (!haveTitle_)
			{
				haveTitle_ = true;
				titleKey_ = in.streamKey;
				notice_.NewTitle(in.streamKey);
			}
			lastQuality_ = signal_.Push(in.deliveredRatio, in.noise);
			QualitySample sample;
			sample.seq = in.seq;
			sample.timeUs = in.timeUs;
			sample.quality = lastQuality_;
			sample.deliveredRatio = in.deliveredRatio;
			sample.apl = in.apl;
			sample.blank = in.blank;
			sample.cut = in.cut;
			sample.fade = in.fade;
			AbrEvent event;
			if (detector_.Push(sample, event))
			{
				lastEvent_ = event;
				haveEvent_ = true;
				eventCount_++;
				consensus_.NoteAbrBoundary(event);
				level_ = event.direction == AbrDirection::Drop ? (std::max)(0.0, level_ - event.magnitude)
					: (std::min)(1.0, level_ + event.magnitude);
			}

			ClassicalReading c = in.classical;
			c.seq = in.seq;
			c.timeUs = in.timeUs;
			if (c.reading)
				c.bitrateQuality = (std::min)(c.bitrateQuality, level_);
			consensus_.Submit(ClassicalResolutionVote(c));
			consensus_.Submit(ClassicalKernelVote(c, s_.vote));
			consensus_.Submit(BitrateQualityVote(c, s_.vote));

			DeferredIndicatorPort port;
			const Result result = consensus_.Evaluate(in.seq, in.timeUs, &port);
			latchedMask_ = static_cast<uint8_t>(latchedMask_ | result.changedMask);
			if (result.changedMask != 0)
				++consensusGen_;
			for (const DeferredIndicatorPort::Request& r : port.TakeRequests())
			{
				++rebaseGen_[r.axis];
				rebaseCause_[r.axis] = r.cause;
			}

			if (haveEvent_ && lastEvent_.direction == AbrDirection::Drop)
			{
				NoticeContext ctx;
				if (in.nativeHeight > 0 && result.Changed(Field::AxisV) && result.before.ratioV > result.value.ratioV)
				{
					ctx.fromHeight = static_cast<int>(result.before.ratioV * in.nativeHeight + 0.5);
					ctx.toHeight = static_cast<int>(result.value.ratioV * in.nativeHeight + 0.5);
				}
				std::string text;
				if (notice_.Consider(lastEvent_, result, text, ctx))
				{
					std::lock_guard<std::mutex> g(noticeMutex_);
					noticeText_ = text;
					++noticeSerial_;
					noticeFired_ = true;
				}
			}

			Record r;
			r.flags = static_cast<uint16_t>((consensusOn_ ? kFlagConsensusOn : 0) | (in.classical.reading ? kFlagReading : 0) |
				(haveEvent_ && lastEvent_.direction == AbrDirection::Drop ? kFlagDropEventSeen : 0) |
				(noticeFired_ ? kFlagNoticeFired : 0));
			r.seq = in.seq;
			r.timeUs = in.timeUs;
			r.consensusGen = consensusGen_;
			r.streamKey = in.streamKey;
			r.maxAgeUs = s_.MaxAgeUs();
			r.ratioH = static_cast<float>(result.value.ratioH);
			r.ratioV = static_cast<float>(result.value.ratioV);
			r.kernel = result.value.kernel;
			r.codec = static_cast<uint8_t>(result.value.codec);
			r.tier = static_cast<int8_t>(result.value.qualityTier);
			r.quality = static_cast<float>(c.reading ? c.bitrateQuality : 1.0);
			r.level = static_cast<float>(level_);
			r.latched = latchedMask_;
			for (int i = 0; i < kFieldCount; ++i)
			{
				r.reason[i] = static_cast<uint8_t>(ReasonOf(result.field[i].reason));
				r.confidence[i] = static_cast<float>(result.field[i].support);
			}
			if (haveEvent_)
			{
				r.abrDirection = lastEvent_.direction == AbrDirection::Drop ? 1 : 2;
				r.abrTimeUs = lastEvent_.timeUs;
				r.abrMagnitude = static_cast<float>(lastEvent_.magnitude);
				r.abrRatioStep = static_cast<float>(lastEvent_.ratioStep);
				r.abrConfidence = static_cast<float>(lastEvent_.confidence);
			}
			r.noticeSerial = noticeSerial_;
			r.rebaseGen[0] = rebaseGen_[0];
			r.rebaseGen[1] = rebaseGen_[1];
			r.rebaseCause[0] = rebaseCause_[0];
			r.rebaseCause[1] = rebaseCause_[1];
			r.discontinuities = discontinuities_;
			held_.Publish(r);
			lastResult_ = result;
			return r;
		}

		HeldState& Held() { return held_; }
		const HeldState& Held() const { return held_; }
		double LastFrameQuality() const { return lastQuality_; }
		double LastNoise() const { return signal_.LastNoise(); }
		uint64_t Events() const { return eventCount_; }
		const AbrEvent& LastEvent() const { return lastEvent_; }
		bool HasEvent() const { return haveEvent_; }
		const Result& LastResult() const { return lastResult_; }
		const Stats& ConsensusStats() const { return consensus_.GetStats(); }
		const AbrStepDetector& Detector() const { return detector_; }

		// Render thread: the one-time notice. True once per firing (`seen` is the caller's last serial).
		bool TakeNotice(uint64_t& seen, std::string& text) const
		{
			std::lock_guard<std::mutex> g(noticeMutex_);
			if (noticeSerial_ == seen)
				return false;
			seen = noticeSerial_;
			text = noticeText_;
			return true;
		}

	private:
		PipelineSettings s_;
		Consensus consensus_;
		AbrStepDetector detector_;
		NoticePolicy notice_;
		FrameQuality::Signal signal_;
		HeldState held_;
		bool consensusOn_ = false;
		double level_ = 1.0;
		double lastQuality_ = 1.0;
		bool haveEvent_ = false;
		AbrEvent lastEvent_;
		uint64_t eventCount_ = 0;
		uint8_t latchedMask_ = 0;
		uint64_t consensusGen_ = 0;
		uint32_t rebaseGen_[2] = {};
		uint8_t rebaseCause_[2] = {};
		uint32_t discontinuities_ = 0;
		bool haveTitle_ = false;
		uint64_t titleKey_ = 0;
		bool noticeFired_ = false;
		mutable std::mutex noticeMutex_;
		std::string noticeText_;
		uint64_t noticeSerial_ = 0;
		Result lastResult_;
	};
}
