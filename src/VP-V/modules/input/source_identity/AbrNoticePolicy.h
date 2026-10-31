// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Source-ID consensus, part 4: the one-time "stream quality dropped" notice (input.source_identity).
//
// When the streaming player switches to a lower-quality rendition mid-title, the picture gets worse
// for reasons nothing in VP can repair. Restarting playback on the player usually brings full
// quality back, so the user is told ONCE per title. The notice is deliberately conservative:
//   - it fires only for a Drop event of enough confidence (AbrStepDetector);
//   - only AFTER the consensus has latched the lower state (the quality tier and/or an axis ratio
//     went down in the consensus Result, i.e. the drop persisted through the dwell): a flicker
//     never latches, so it never fires;
//   - at most once per title key (the call site passes the Indicator's FULL/era counter).
// The call site keeps the last Drop event and calls Consider() with each new consensus Result until
// it fires or a new title starts.
// The text never claims a cause it did not measure; resolutions are named only when the call site
// knows them (NoticeContext), otherwise the wording carries no numbers.
// Header-only, standard library only, C++14, no clock.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_identity/AbrStepDetector.h>
#include <VP-V/modules/input/source_identity/SourceIdConsensus.h>

#include <cstdint>
#include <string>

namespace SourceIdentity
{
	// Stable id of the notice (the GUI's one-time notice keys on it).
	inline const char* AbrNoticeId() { return "abr_quality_drop"; }

	struct NoticeContext
	{
		int fromHeight = 0;   // 0 = unknown; the call site fills them only when it measured them
		int toHeight = 0;
	};

	inline std::string AbrNoticeText(const NoticeContext& c)
	{
		std::string t = "Stream quality dropped: the player switched to a lower-quality stream";
		if (c.fromHeight > 0 && c.toHeight > 0)
			t += " (about " + std::to_string(c.fromHeight) + "p to " + std::to_string(c.toHeight) + "p)";
		t += ". Restarting playback on the player usually brings full quality back.";
		return t;
	}

	class NoticePolicy
	{
	public:
		struct Settings
		{
			bool enabled = true;          // source_id_abr_notice
			double minConfidence = 0.5;   // of the Drop event
			int minTierDrop = 1;          // quality tiers the consensus must have dropped by
			double minRatioDrop = 0.1;    // delivered-ratio drop the consensus must have latched
		};

		NoticePolicy() {}
		explicit NoticePolicy(const Settings& s) : s_(s) {}

		// A new title (the call site passes the Indicator's FULL/era counter). A new key re-arms.
		void NewTitle(uint64_t titleKey)
		{
			if (!haveTitle_ || titleKey != titleKey_)
				fired_ = false;
			haveTitle_ = true;
			titleKey_ = titleKey;
		}

		bool Fired() const { return fired_; }

		bool Consider(const AbrEvent& event, const Result& after, std::string& text, const NoticeContext& context = NoticeContext())
		{
			if (!s_.enabled || fired_)
				return false;
			if (event.direction != AbrDirection::Drop || event.confidence < s_.minConfidence)
				return false;
			bool latchedLower = false;
			if (after.Changed(Field::Quality) && after.before.qualityTier >= 0 &&
				after.before.qualityTier - after.value.qualityTier >= s_.minTierDrop)
				latchedLower = true;
			if (after.Changed(Field::AxisH) && after.before.ratioH - after.value.ratioH >= s_.minRatioDrop)
				latchedLower = true;
			if (after.Changed(Field::AxisV) && after.before.ratioV - after.value.ratioV >= s_.minRatioDrop)
				latchedLower = true;
			if (!latchedLower)
				return false;
			fired_ = true;
			text = AbrNoticeText(context);
			return true;
		}

	private:
		Settings s_;
		bool haveTitle_ = false;
		uint64_t titleKey_ = 0;
		bool fired_ = false;
	};

	// The registry entry for a fired notice (engaged) or none (released).
	inline FallbackCatalog::Decision NoticeDecision(bool fired, const std::string& text)
	{
		if (!fired)
			return FallbackCatalog::Released(AbrNoticeId());
		return FallbackCatalog::Engaged(AbrNoticeId(), text);
	}
}
