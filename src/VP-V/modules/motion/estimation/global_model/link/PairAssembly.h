// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-2, doc 56 section 1.4, F3): builds the Fitter's PairInput from a FieldCopy and the pair facts, and maps
// an Indicator Stamp onto the fitter's resync generation. The Fitter resets when its generation differs from the last one
// (GlobalModelFit.cpp), so the mapping is exact: it changes ONLY on a full change, a continuity slip or a motion-settings
// change; never on a re-base (re-base is Ignore: rebaseGeneration is always 0) and never on timing / graph counters.
// Pure, header-only, C++14, deterministic.
#pragma once

#include "FieldCopy.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>

#include <cstdint>

namespace CameraLink
{
	// A fixed mix of era and the three counters the camera honours. Deterministic (FNV-1a over fixed words).
	inline uint64_t GenerationsOf(const VpIndicator::Stamp& s)
	{
		uint64_t h = 1469598103934665603ull;
		const uint32_t words[4] = { s.era, s.v[VpIndicator::kFull], s.v[VpIndicator::kContinuity],
			s.v[VpIndicator::kSettingsMotion] };
		for (uint32_t w : words)
			for (int b = 0; b < 4; ++b)
			{
				h ^= (w >> (8 * b)) & 0xFFu;
				h *= 1099511628211ull;
			}
		return h;
	}

	// What the host knows about the pair beside the field.
	struct PairFacts
	{
		CameraModel::Rect picture;                  // P4 active picture (frame px); empty = full frame (status shows it)
		bool cut = false, fade = false, flash = false;
		double fadeGain = 1, fadeOffset = 0;
		bool haveEngineGlobal = false;              // only when IVectorEngine::GlobalFromEngine()
		double engineGx = 0, engineGy = 0;
		double pairSeconds = 0;                     // PairTiming::Seconds; 0 -> NoInput
		int64_t deadlineMicros = 0;                 // absolute, in the Fitter clock; 0 = none
		bool spansSlip = false;                     // IVectorEngine::PairSpansSlip(pairFrom, pairTo)
	};

	enum class AssembleStatus : uint8_t { Ready, NotFittedSlip, NotFittedAncient, NoField };

	inline const char* Text(AssembleStatus s)
	{
		switch (s)
		{
		case AssembleStatus::Ready: return "ready";
		case AssembleStatus::NotFittedSlip: return "pair spans a slip";
		case AssembleStatus::NotFittedAncient: return "stamp ancient";
		default: return "no field";
		}
	}

	struct AssembleInfo
	{
		bool pictureWasEmpty = false;   // the full frame was used
		bool noBackward = false;        // inverse signal neutral
	};

	// The returned PairInput points INTO `field` (and nothing else): the caller keeps the FieldCopy alive across Fit.
	// textGuard / texture / pixelResidual stay null (no producer yet: the status says "text guard: none").
	inline AssembleStatus AssemblePair(const FieldCopy& field, const PairFacts& facts, const VpIndicator::Stamp& stamp,
		CameraModel::PairInput& out, AssembleInfo* info = nullptr)
	{
		out = CameraModel::PairInput();
		if (!field.Valid())
			return AssembleStatus::NoField;
		if (stamp.ancient)
			return AssembleStatus::NotFittedAncient;
		if (facts.spansSlip)
			return AssembleStatus::NotFittedSlip;
		out.gridW = field.gridW;
		out.gridH = field.gridH;
		out.blockSize = field.blockSize;
		out.picture = facts.picture;
		const bool emptyPic = out.picture.Empty();
		if (emptyPic)
		{
			out.picture.left = 0;
			out.picture.top = 0;
			out.picture.right = static_cast<int>(field.gridW * field.blockSize + 0.5);
			out.picture.bottom = static_cast<int>(field.gridH * field.blockSize + 0.5);
		}
		out.fwdX = field.fwdX.data();
		out.fwdY = field.fwdY.data();
		out.fwdTrust = field.fwdTrust.size() == field.Blocks() ? field.fwdTrust.data() : nullptr;
		const bool bwd = field.HaveBackward() && field.bwdX.size() == field.Blocks() && field.bwdY.size() == field.Blocks();
		if (bwd)
		{
			out.bwdX = field.bwdX.data();
			out.bwdY = field.bwdY.data();
			out.bwdTrust = field.bwdTrust.size() == field.Blocks() ? field.bwdTrust.data() : nullptr;
		}
		out.haveEngineGlobal = facts.haveEngineGlobal;
		out.engineGx = facts.engineGx;
		out.engineGy = facts.engineGy;
		out.cut = facts.cut;
		out.fade = facts.fade;
		out.flash = facts.flash;
		out.fadeGain = facts.fadeGain;
		out.fadeOffset = facts.fadeOffset;
		out.seq = field.seq;
		out.pairFrom = field.pairFrom;
		out.pairTo = field.pairTo;
		out.pairSeconds = facts.pairSeconds;
		out.resyncGeneration = GenerationsOf(stamp);
		out.rebaseGeneration = 0;   // re-base is Ignore (F3)
		out.deadlineMicros = facts.deadlineMicros;
		if (info)
		{
			info->pictureWasEmpty = emptyPic;
			info->noBackward = !bwd;
		}
		return AssembleStatus::Ready;
	}

	// The record of a pair that is NOT fitted (slip / ancient stamp / no field): reason GenerationReset (not in the usable
	// set), invalid, so a consumer holds its measured values; the status names the cause.
	inline CameraModel::CameraMotion NotFittedRecord(uint64_t seq, uint64_t pairFrom, uint64_t pairTo)
	{
		CameraModel::CameraMotion m;
		m.seq = seq;
		m.pairFrom = pairFrom;
		m.pairTo = pairTo;
		m.valid = false;
		m.reason = CameraModel::Reason::GenerationReset;
		return m;
	}
}
