// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Source-ID consensus, part 1: the vote types and the voter adapters (input.source_analysis (identity/)).
//
// A "vote" is one voter's opinion about what the delivered source IS: how much of the native
// resolution it carries on each axis, the player's upscale kernel, the codec family and a quality
// tier. Voters abstain on fields they know nothing about (fieldMask). The consensus
// (SourceIdConsensus.h) combines the votes; nothing here decides anything.
//
// The adapters are pure functions over PLAIN structs that the call site fills from the source
// resolution monitor's snapshot, so this header does not include the monitor (or libplacebo).
// Header-only, standard library only, C++14, no clock: time is always SOURCE time passed in.
//
// AV1 note (evidence text only, never a reason to denoise): the ID net detects AV1 film-grain
// synthesis well. A future field may record it as evidence text. It must never be used to
// remove, average or interpolate grain (the grain rule).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SourceIdentity
{
	enum class Field { AxisH = 0, AxisV, Kernel, Codec, Quality, Count };
	enum class CodecClass { Unknown = 0, Avc, Hevc, Vp9, Av1, Vvc, Lcevc, Other };
	enum class VoterId { ClassicalResolution = 0, ClassicalKernel, BitrateQuality, IdNet, Count };

	constexpr int kFieldCount = static_cast<int>(Field::Count);
	constexpr int kVoterCount = static_cast<int>(VoterId::Count);

	inline uint32_t FieldBit(Field f) { return 1u << static_cast<unsigned>(f); }
	inline bool IsAxis(Field f) { return f == Field::AxisH || f == Field::AxisV; }

	// What the source is. ratioH / ratioV: delivered fraction of native on that axis (1 = native).
	struct Hypothesis
	{
		double ratioH = 1.0;
		double ratioV = 1.0;
		int kernel = -1;                      // -1 = unknown
		CodecClass codec = CodecClass::Unknown;
		int qualityTier = -1;                 // 0 = lowest; -1 = unknown
	};

	inline double FieldValue(const Hypothesis& h, Field f)
	{
		switch (f)
		{
		case Field::AxisH: return h.ratioH;
		case Field::AxisV: return h.ratioV;
		case Field::Kernel: return static_cast<double>(h.kernel);
		case Field::Codec: return static_cast<double>(static_cast<int>(h.codec));
		case Field::Quality: return static_cast<double>(h.qualityTier);
		default: return 0.0;
		}
	}

	inline void SetFieldValue(Hypothesis& h, Field f, double v)
	{
		switch (f)
		{
		case Field::AxisH: h.ratioH = v; break;
		case Field::AxisV: h.ratioV = v; break;
		case Field::Kernel: h.kernel = static_cast<int>(v); break;
		case Field::Codec: h.codec = static_cast<CodecClass>(static_cast<int>(v)); break;
		case Field::Quality: h.qualityTier = static_cast<int>(v); break;
		default: break;
		}
	}

	inline double Clamp01(double x) { return x > 0.0 ? (x < 1.0 ? x : 1.0) : 0.0; }

	struct Vote
	{
		VoterId voter = VoterId::ClassicalResolution;
		uint32_t fieldMask = 0;               // fields voted on; every other field is an abstention
		Hypothesis h;
		double confidence[kFieldCount] = {};  // 0..1 per field (the net's heads differ in confidence)
		uint64_t evidenceSeq = 0;
		int64_t evidenceTimeUs = 0;           // newest evidence behind this vote (age = now - this)
		// Only VoterId::ClassicalResolution may set it ("detail above the re-based size" => the source
		// is native on the voted axes at once). The consensus ignores it from any other voter.
		bool safetyVeto = false;
	};

	inline void SetField(Vote& v, Field f, double confidence)
	{
		v.fieldMask |= FieldBit(f);
		v.confidence[static_cast<int>(f)] = Clamp01(confidence);
	}

	// Adapter settings (the known trust profile of the voters is data, not code branches).
	struct VoteSettings
	{
		int kernelMinVotes = 3;                 // kernel confidence ramps up to this many fitted votes
		std::vector<double> qualityTierEdges = { 0.25, 0.5, 0.75 }; // tier = number of edges <= quality
		double bitrateQualityConfidence = 0.6;  // a bitrate reading is indirect evidence of the tier
		double netAxisTrust = 1.0;              // ID net: axes / factor
		double netKernelMinConfidence = 0.9;    // ID net kernel head: below this the net abstains
		double netCodecTrust = 0.25;            // ID net codec head: confidence capped here
		double netQualityTrust = 0.0;           // ID net bitrate-tier head is broken until re-validated: 0 = abstain
	};

	// The classical source-resolution monitor's reading (the call site copies its Snapshot).
	struct ClassicalReading
	{
		bool reading = false;                 // false = no reading yet (every classical voter abstains)
		double declaredRatio = 1.0, confidence = 0.0;
		double axisRatioX = 1.0, axisRatioY = 1.0, axisConfidence = 0.0;
		int kernel = -1, kernelVotes = 0, kernelAgree = 0;
		double bitrateQuality = 1.0;          // 0..1, 1 = best
		bool detailAbove = false;             // detail above the re-based size
		uint64_t seq = 0;
		int64_t timeUs = 0;
	};

	inline Vote ClassicalResolutionVote(const ClassicalReading& r)
	{
		Vote v;
		v.voter = VoterId::ClassicalResolution;
		v.evidenceSeq = r.seq;
		v.evidenceTimeUs = r.timeUs;
		if (!r.reading)
			return v;
		v.safetyVeto = r.detailAbove;
		const double delta = r.declaredRatio - 1.0;
		double confidence = 0.0;
		if (r.declaredRatio > 0.0 && (delta > 1e-9 || delta < -1e-9))
		{
			v.h.ratioH = r.declaredRatio;      // the uniform (both axes) reading
			v.h.ratioV = r.declaredRatio;
			confidence = r.confidence;
		}
		else
		{
			v.h.ratioH = r.axisRatioX > 0.0 ? r.axisRatioX : 1.0;   // per-axis trackers
			v.h.ratioV = r.axisRatioY > 0.0 ? r.axisRatioY : 1.0;
			confidence = r.axisConfidence;
		}
		SetField(v, Field::AxisH, confidence);
		SetField(v, Field::AxisV, confidence);
		return v;
	}

	inline Vote ClassicalKernelVote(const ClassicalReading& r, const VoteSettings& s)
	{
		Vote v;
		v.voter = VoterId::ClassicalKernel;
		v.evidenceSeq = r.seq;
		v.evidenceTimeUs = r.timeUs;
		if (!r.reading || r.kernel < 0 || r.kernelVotes <= 0)
			return v;
		const double agree = Clamp01(static_cast<double>(r.kernelAgree) / static_cast<double>(r.kernelVotes));
		const int minVotes = s.kernelMinVotes > 1 ? s.kernelMinVotes : 1;
		const double ramp = Clamp01(static_cast<double>(r.kernelVotes) / static_cast<double>(minVotes));
		v.h.kernel = r.kernel;
		SetField(v, Field::Kernel, agree * ramp);
		return v;
	}

	inline Vote BitrateQualityVote(const ClassicalReading& r, const VoteSettings& s)
	{
		Vote v;
		v.voter = VoterId::BitrateQuality;
		v.evidenceSeq = r.seq;
		v.evidenceTimeUs = r.timeUs;
		if (!r.reading || !(r.bitrateQuality >= 0.0))
			return v;
		int tier = 0;
		for (size_t i = 0; i < s.qualityTierEdges.size(); ++i)
			if (r.bitrateQuality >= s.qualityTierEdges[i])
				++tier;
		v.h.qualityTier = tier;
		SetField(v, Field::Quality, s.bitrateQualityConfidence);
		return v;
	}

	// The ID net's heads. The net is event-driven: its vote may be old (age is handled by the
	// consensus through the per-voter maximum evidence age).
	struct NetReading
	{
		bool valid = false;
		double ratioH = 1.0, ratioV = 1.0, ratioConf = 0.0;
		int kernel = -1;
		double kernelConf = 0.0;
		CodecClass codec = CodecClass::Unknown;
		double codecConf = 0.0;
		int qualityTier = -1;
		double qualityConf = 0.0;
		uint64_t seq = 0;
		int64_t timeUs = 0;
	};

	inline Vote IdNetVote(const NetReading& n, const VoteSettings& s)
	{
		Vote v;
		v.voter = VoterId::IdNet;
		v.evidenceSeq = n.seq;
		v.evidenceTimeUs = n.timeUs;
		if (!n.valid)
			return v;
		if (s.netAxisTrust > 0.0 && n.ratioConf > 0.0 && n.ratioH > 0.0 && n.ratioV > 0.0)
		{
			v.h.ratioH = n.ratioH;
			v.h.ratioV = n.ratioV;
			const double c = n.ratioConf < s.netAxisTrust ? n.ratioConf : s.netAxisTrust;
			SetField(v, Field::AxisH, c);
			SetField(v, Field::AxisV, c);
		}
		if (n.kernel >= 0 && n.kernelConf >= s.netKernelMinConfidence && n.kernelConf > 0.0)
		{
			v.h.kernel = n.kernel;
			SetField(v, Field::Kernel, n.kernelConf);
		}
		if (n.codec != CodecClass::Unknown && s.netCodecTrust > 0.0 && n.codecConf > 0.0)
		{
			v.h.codec = n.codec;
			SetField(v, Field::Codec, n.codecConf < s.netCodecTrust ? n.codecConf : s.netCodecTrust);
		}
		if (n.qualityTier >= 0 && s.netQualityTrust > 0.0 && n.qualityConf > 0.0)
		{
			v.h.qualityTier = n.qualityTier;
			SetField(v, Field::Quality, n.qualityConf < s.netQualityTrust ? n.qualityConf : s.netQualityTrust);
		}
		return v;
	}
}
