// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The ONE read API of P14 for its consumers (doc 61 section 2.1, 2.4): every consumer that wants "how good is this source" calls
// View(), never `Snapshot.bitrateQuality` directly. Registered as a shared unit (tools/check_shared_units.py).
//
//   consensus off   -> exactly the snapshot's bitrateQuality and today's tier mapping (byte-identical to before; not a fallback).
//   consensus on    -> the sealed record's latched tier and event-anchored level, combined with the live classical value so a
//                      quiet stream reads exactly as before:  quality = min(snapshot.bitrateQuality, record.level).
//   record absent / stale / unlatched / another unit version -> the classical value, VISIBLY: the returned Decision is
//                      engaged (FallbackCatalog "product.source_quality") with the reason, and recovers by itself when a fresh
//                      record returns (the next call).
//
// Pure functions over a plain struct (no monitor, no libplacebo, no clock: `nowUs` is passed in). Header-only, C++14.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdProduct.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace SourceIdentity
{
	// The classical readings the call site copies from the monitor snapshot.
	struct ClassicalQuality
	{
		bool reading = false;
		double bitrateQuality = 1.0;   // spec 1b, smoothed (what every consumer read before)
		double effectiveBits = 0.0;
	};

	enum class QualityOrigin { Classical, Consensus };

	// The consumers of the view (the fallback status wording per consumer; doc 61 table 2.4).
	enum class QualityConsumer { TemporalDetailAuto, DitherNeed, Deband, ContrastRecovery, AdaptiveScaling, DetailPulse, TextureRestore, Status };

	struct QualityView
	{
		double quality = 1.0;                 // 0..1, 1 = clean
		int tier = -1;                        // 0 = lowest ... edges.size() = best; -1 = unknown
		QualityOrigin origin = QualityOrigin::Classical;
		bool consensusRequested = false;
		std::string reason;                   // why the classical value is used while consensus is on ("" otherwise)
		FallbackCatalog::Decision fallback;   // engaged exactly when consensusRequested && origin == Classical
		bool abrDropSeen = false;             // an ABR Drop event is the newest event of a fresh record
	};

	inline int TierOf(double quality, const VoteSettings& vs = VoteSettings())
	{
		int tier = 0;
		for (size_t i = 0; i < vs.qualityTierEdges.size(); ++i)
			if (quality >= vs.qualityTierEdges[i])
				++tier;
		return tier;
	}

	inline const char* ConsumerStatusText(QualityConsumer c)
	{
		switch (c)
		{
		case QualityConsumer::TemporalDetailAuto: return "TD auto: source quality unknown";
		case QualityConsumer::DitherNeed: return "dither need: classical quality";
		case QualityConsumer::Deband: return "deband: classical quality";
		case QualityConsumer::ContrastRecovery: return "contrast recovery: classical quality";
		case QualityConsumer::AdaptiveScaling: return "adaptive scaling: classical quality";
		case QualityConsumer::DetailPulse: return "detail pulse: classical quality";
		case QualityConsumer::TextureRestore: return "texture restore: classical quality";
		case QualityConsumer::Status: return "Source identity: classical only";
		}
		return "classical quality";
	}

	// `record` may be null (nothing published, or the unit version differs). `nowUs`: source time of the reader.
	inline QualityView View(const Record* record, const ClassicalQuality& snap, bool consensusOn, int64_t nowUs,
		const VoteSettings& vs = VoteSettings())
	{
		QualityView v;
		v.quality = snap.bitrateQuality;
		v.tier = snap.reading ? TierOf(snap.bitrateQuality, vs) : -1;
		v.consensusRequested = consensusOn;
		if (!consensusOn)
			return v;                          // exactly today's value; consensus off is not a fallback
		const char* why = nullptr;
		if (!record)
			why = "no record";
		else if (record->unitVersion != kUnitVersion)
			why = "unit version";
		else if (record->maxAgeUs > 0 && (nowUs - record->timeUs > record->maxAgeUs || nowUs < record->timeUs - record->maxAgeUs))
			why = "stale";
		else if (record->tier < 0 || !record->Latched(Field::Quality))
			why = "quality not latched yet";
		if (why)
		{
			v.reason = why;
			v.fallback = FallbackCatalog::ProductFallback("product.source_quality", why);
			return v;
		}
		v.origin = QualityOrigin::Consensus;
		v.quality = (std::min)(snap.bitrateQuality, static_cast<double>(record->level));
		v.tier = record->tier;
		v.abrDropSeen = (record->flags & kFlagDropEventSeen) != 0;
		v.fallback = FallbackCatalog::Released("product.source_quality");
		return v;
	}

	// The status text of a consumer under a view: its fallback wording while the classical value stands in, "" otherwise.
	inline std::string ConsumerStatus(const QualityView& v, QualityConsumer c)
	{
		return v.fallback.engaged ? std::string(ConsumerStatusText(c)) + " (" + v.reason + ")" : std::string();
	}

	// ---- the consumer helpers (pure; the classical view feeds the same functions, so a fallback changes the INPUT only) ----

	// Dither need in 0..1: the lower the quality, the more a step is visible. Smooth, monotone.
	inline double DitherNeed(const QualityView& v) { return (std::min)(1.0, (std::max)(0.0, 1.0 - v.quality)); }

	// Contrast recovery gates OFF on a clean high-bitrate source (get out of the way): the best tier and a high quality.
	inline bool ContrastRecoveryActive(const QualityView& v, const VoteSettings& vs = VoteSettings(), double cleanQuality = 0.9)
	{
		const int top = static_cast<int>(vs.qualityTierEdges.size());
		return !(v.tier >= top && v.quality >= cleanQuality);
	}

	// The Ctrl+I / Config status line (doc 61 2.2).
	inline std::string StatusLine(const QualityView& v, const Record* record)
	{
		if (!v.consensusRequested)
			return "Source identity: off";
		if (v.origin == QualityOrigin::Classical)
			return std::string("Source identity: classical only (") + v.reason + ")";
		char b[160];
		std::snprintf(b, sizeof(b), "Source identity: consensus (tier %d, level %.2f%s)", v.tier, record ? record->level : 1.0f,
			v.abrDropSeen ? ", stream quality dropped" : "");
		return b;
	}
}
