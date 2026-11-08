// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1/K2/K4): the interface the camera extension exposes ("vpv.camera.v1"). Consumers (grain map, measurement,
// de-judder, ...) reach the camera only through it. Pure interface header; the implementation is CameraLinkCore.
#pragma once

#include "FieldCopy.h"

#include <VP-V/modules/motion/estimation/global_model/CameraGuidanceGpu.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>

#include <libplacebo/gpu.h>

#include <memory>

namespace CameraLink
{
	constexpr const char* kCameraQueryId = "vpv.camera.v1";

	// What a consumer gets for P8 of one frame: the label texture (GPU) when available. available == false means the
	// consumer runs with enabled = 0 (today's values), shown under the camera status.
	struct CameraLabelsView
	{
		bool available = false;
		pl_tex tex = nullptr;
		int gridW = 0, gridH = 0;
	};

	class ICameraLink
	{
	public:
		virtual ~ICameraLink() {}
		// The record P7 published AND the seal accepted for frame `seq` (null when late or absent).
		virtual std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t seq) const = 0;
		// The newest published record for `seq` regardless of the seal: the causal input of the guidance (lag d), read on
		// the render thread of a LATER frame, by which time the record is final.
		virtual std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t seq) const = 0;
		// The seq of the `back`-th PREVIOUS camera record before `seq` (back = 1 is the newest record older than seq). The guide of
		// pair N reads the d-th previous record, never N - d: the field numbering has gaps (cadence repeats, 24p in 30), so N - d
		// would answer "record late" for a record that exists. The chain is of REQUESTED fields (in order, known the moment the
		// field copy is asked for), so a record whose fit or readback is still running is "late", never replaced by an older one.
		// Default: the arithmetic N - back (fakes without a chain keep it). False: fewer than `back` records before seq.
		virtual bool PreviousRecordSeq(uint64_t seq, unsigned back, uint64_t& out) const
		{
			if (back == 0 || seq < back)
				return false;
			out = seq - back;
			return true;
		}
		// The newest requested record seq (0 = none). The lane job derives the next pair from it.
		virtual uint64_t LatestRecordSeq() const { return 0; }
		virtual std::shared_ptr<const FieldCopy> FieldOf(uint64_t seq) const = 0;
		// P8 for consumer `c` at frame `seq` (K2: first consumer fills, later ones reuse).
		virtual bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out) = 0;
		// The guidance frame for the pair about to be searched (K4); null = refused or all switches off.
		virtual std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t seq) = 0;
	};
}
