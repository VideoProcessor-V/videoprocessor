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

	// Why camera(N) was not ready in time for the matcher's pair N (the d = 0 rule). Counted per cause, shown in one status line.
	//   NoAheadPair    no ahead copy was asked for N (the producer's pair was not there in time, or a seed-path record)
	//   NotPublished   the ahead fit of N was asked for but not published on time before the matcher ran
	//   Bridged        published through the lag bridge (N's seal had run), or there is no on-time target
	//   Stale          stale at use: the P2 epoch or N's stamp moved since the publish, or the seal rejected it
	//   NotThePair     the matcher's pair is not the camera's pair (a gap / hiccup fill pair)
	enum class OnTimeMiss : uint8_t { None, NoAheadPair, NotPublished, Bridged, Stale, NotThePair, Count };
	enum class OnTimeCheck : uint8_t { Full, PublishOnly };
	enum class SealVerdict : uint8_t { Pending, Accepted, Rejected };

	inline const char* Text(OnTimeMiss m)
	{
		switch (m)
		{
		case OnTimeMiss::None: return "on time";
		case OnTimeMiss::NoAheadPair: return "no ahead pair";
		case OnTimeMiss::NotPublished: return "fit not published before the matcher";
		case OnTimeMiss::Bridged: return "bridged / no on-time target";
		case OnTimeMiss::Stale: return "stale at use (epoch / stamp)";
		case OnTimeMiss::NotThePair: return "not the camera's pair (gap)";
		default: return "?";
		}
	}

	class ICameraLink
	{
	public:
		virtual ~ICameraLink() {}
		// The record P7 published AND the seal accepted for frame `seq` (null when late or absent).
		virtual std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t seq) const = 0;
		// The newest published record for `seq` regardless of the seal (diagnostics, the governor's skip confirmation, the context
		// provider's flow resample).
		virtual std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t seq) const = 0;
		// K4 at d = 0: camera(N) for the matcher's pair of N, READY IN TIME or null with the miss cause (refs 93-prep-cam-d0 section 1):
		// published on time into the on-time target (the mark), a committed on-time pool entry, an AHEAD field (a seed record is never
		// on time), and with check == Full also the P2 epoch, the stamp honoured at the publish and the seal verdict (render thread).
		// PublishOnly reads only the pool and the marks (the camera worker's lane job; the render thread re-checks in full before use).
		// Default (fakes): every published record counts as on time.
		virtual std::shared_ptr<const CameraModel::CameraMotion> OnTimeRecord(uint64_t seq, OnTimeCheck check, OnTimeMiss& why) const
		{
			(void)check;
			std::shared_ptr<const CameraModel::CameraMotion> r = CausalRecord(seq);
			why = r ? OnTimeMiss::None : OnTimeMiss::NotPublished;
			return r;
		}
		// The on-time seal's verdict on frame `seq` (Pending until OnTimeSeal(seq) ran; render thread). Default (fakes): Pending.
		virtual SealVerdict OnTimeSealVerdict(uint64_t seq) const { (void)seq; return SealVerdict::Pending; }
		virtual std::shared_ptr<const FieldCopy> FieldOf(uint64_t seq) const = 0;
		// P8 for consumer `c` at frame `seq` (K2: first consumer fills, later ones reuse).
		virtual bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out) = 0;
		// The guidance frame for the pair about to be searched (K4); null = refused or all switches off.
		virtual std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t seq) = 0;
	};
}
