// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The P15 / P15c frame-pattern record (unit QualityPattern, version 1): the layout of the ONE 64-byte record per unique source
// frame that the live frame-role detector (quality_pattern, and its causal short-queue substitute quality_pattern.causal) will
// publish. DECLARATION ONLY (P15-DECL): nothing here computes a role; no producer is registered, so every reader finds the record
// absent and says so (QualityPatternView.h, "frame pattern: not produced"). The detector (owner 2026-11-12: a CENTRAL, mandatory part
// of temporal detail; picture-only, no encoder side data, since the live input is an HDMI stream from a streaming app) drops into
// this layout later without a skeleton, catalog or hook change. Fields per refs/99c A3 items 3 and 8.
//
// Layout: fixed, natural alignment, no padding (offsets pinned by static_assert and by QualityPatternRecordTests). Little-endian host
// (x64). The record is a CPU POD kept in system RAM (Products.h kQualityPatternBytes, residency SystemRam); zero-initialised =
// role Unknown, nothing valid, not provisional, not causal. Spare words are RESERVED and must stay zero (a later detector may name
// them; the unit version then bumps only if an existing field changes meaning).
//
// Header-only, C++14.
#include <cstddef>
#include <cstdint>

namespace QualityPattern
{
	constexpr uint32_t kUnitVersion = 1;          // the unit QualityPattern, version 1 (ProductCatalog.h kExpectedQualityPatternUnit)
	constexpr uint32_t kRecordBytes = 64;         // == VpSkeleton::kQualityPatternBytes (asserted in QualityPatternRecordTests)

	// The inferred role of the frame in the source's coding structure (from the picture only; doc 91 frame-role line).
	enum class FrameRole : uint8_t
	{
		Unknown = 0,       // default: no inference yet (chain start, gap, or not produced)
		Peak = 1,          // quality peak / anchor (the I-like frame the canvas is built on)
		Reference = 2,     // a reference frame that is not the peak (first P / reference B)
		NonReference = 3,  // a non-reference frame (saw-tooth valley side)
		ExactCopy = 4      // an exact copy of an earlier frame (see exactCopyFlags)
	};

	// Peak / valley marker of the saw-tooth position inside the window.
	enum class PeakValley : uint8_t { None = 0, Peak = 1, Valley = 2 };

	// validityBits
	constexpr uint32_t kValidRole = 1u << 0;        // role is an inference (else Unknown by default)
	constexpr uint32_t kValidScore = 1u << 1;       // score is measured
	constexpr uint32_t kValidRank = 1u << 2;        // rank / peakValley / windowFrames are filled
	constexpr uint32_t kValidSteps = 1u << 3;       // the per-block step summary is filled
	constexpr uint32_t kValidGrid = 1u << 4;        // block-grid period / phase are filled
	constexpr uint32_t kValidCopy = 1u << 5;        // exactCopyFlags is evaluated
	constexpr uint32_t kProvisional = 1u << 6;      // the role may still change (short queue / not enough history)

	// exactCopyFlags
	constexpr uint8_t kCopyOfPrev1 = 1u << 0;       // exact copy of frame N-1
	constexpr uint8_t kCopyOfPrev2 = 1u << 1;       // exact copy of frame N-2

	struct Record
	{
		uint32_t unit;            //  0 unit version (kUnitVersion); 0 = never written
		uint32_t validityBits;    //  4 kValid* | kProvisional
		uint64_t seq;             //  8 source seq key of the frame this record describes
		uint8_t role;             // 16 FrameRole
		uint8_t leadUsed;         // 17 timing: lead (frames ahead) the detector used: 0 (P15c), 1 or 2 (P15)
		uint8_t causal;           // 18 timing: 1 = causal record (P15c: lead 0, role may be provisional), 0 = P15
		uint8_t peakValley;       // 19 PeakValley
		float score;              // 20 quality score (identity/FrameQuality.h scale: 0..1, 1 = clean)
		uint16_t rank;            // 24 rank of the frame inside the window (0 = best)
		uint16_t windowFrames;    // 26 frames in the window the rank is over
		uint16_t stepMean;        // 28 per-block step summary, Q8.8: mean
		uint16_t stepP90;         // 30 per-block step summary, Q8.8: 90th percentile
		uint16_t stepMax;         // 32 per-block step summary, Q8.8: maximum
		uint16_t gridPeriodX;     // 34 block-grid period in coded pixels along x (0 = none)
		uint16_t gridPeriodY;     // 36 block-grid period along y (0 = none)
		uint8_t gridPhaseX;       // 38 block-grid phase along x
		uint8_t gridPhaseY;       // 39 block-grid phase along y
		uint8_t exactCopyFlags;   // 40 kCopyOf*
		uint8_t spareBytes[3];    // 41 reserved, zero
		uint32_t spare[5];        // 44 reserved words (A3 item 8 spare channels), zero
	};

	static_assert(sizeof(Record) == kRecordBytes, "the frame-pattern record is exactly 64 bytes (Products.h kQualityPatternBytes)");
	static_assert(offsetof(Record, unit) == 0 && offsetof(Record, validityBits) == 4 && offsetof(Record, seq) == 8, "header offsets");
	static_assert(offsetof(Record, role) == 16 && offsetof(Record, score) == 20 && offsetof(Record, rank) == 24, "role / score / rank offsets");
	static_assert(offsetof(Record, stepMean) == 28 && offsetof(Record, gridPeriodX) == 34 && offsetof(Record, exactCopyFlags) == 40, "step / grid / copy offsets");
	static_assert(offsetof(Record, spareBytes) == 41 && offsetof(Record, spare) == 44, "spare channels fill the record to 64 bytes");

	// ---- P16 block-grid texture layout RESERVE (constants only; NOTHING is allocated here) -------------------------------------------
	// The per-block coding-evidence maps (coding_evidence, P16) are 4 channels x 2 textures over a cell grid derived from the source
	// dimensions (never a fixed resolution): cells = ceil(width / kBlockGridCellEdge) x ceil(height / kBlockGridCellEdge) (240 x 135 at 4K).
	constexpr uint32_t kBlockGridChannels = 4;
	constexpr uint32_t kBlockGridTextures = 2;
	constexpr uint32_t kBlockGridCellEdge = 16;
	inline uint32_t BlockGridCellsX(uint32_t width) { return (width + kBlockGridCellEdge - 1) / kBlockGridCellEdge; }
	inline uint32_t BlockGridCellsY(uint32_t height) { return (height + kBlockGridCellEdge - 1) / kBlockGridCellEdge; }
}
