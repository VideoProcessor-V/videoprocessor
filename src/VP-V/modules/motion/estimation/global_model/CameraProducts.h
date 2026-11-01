// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link layer (K0): the camera products' CONSUMERS. Each component that reads the global
// camera model states its demand here as a registry ConsumerDecl (plan 45 section 2), so the product
// graph (core/ProductGraph.h) holds P7 camera fit / P8 camera labels / P9 camera smoothed exactly
// when an active consumer needs them. Pure, header-only, C++14, no Windows, no GPU.
//
// P9 is always declared by the consumers that want the smoothed camera; the GRAPH substitutes the
// causal P7 when the queue is short (Graph::ReducedNodes lists it: a visible reduced mode).
#pragma once

#include <VP-V/core/Products.h>

#include "CameraAccessor.h"

#include <cstdint>
#include <vector>

namespace CameraLink
{
	// The unit version of the camera products P7-P9 (fit, labels, smoothing): bump on ANY change of their law or layout.
	// core/ProductCatalog.h holds the expected number; a test compares.
	constexpr uint32_t kCameraUnitVersion = 1;

	enum class CameraConsumer : int
	{
		Diagnostics, GrainMap, ResidualStabilise, TemporalDetail, DetailPulse, TextureRestore,
		DitherVectors, Debander, OcclusionFiller, Measurement, DejudderWarp, HiccupRepair,
		RollOff, ObjectSplit
	};
	constexpr int kCameraConsumerCount = 14;
	constexpr uint16_t kCameraConsumerIdBase = 0x0200;   // registry consumer id = base + CameraConsumer index

	inline CameraConsumer ConsumerAt(int i) { return static_cast<CameraConsumer>(i); }

	inline const char* Name(CameraConsumer c)
	{
		switch (c)
		{
		case CameraConsumer::Diagnostics: return "camera diagnostics";
		case CameraConsumer::GrainMap: return "camera grain map";
		case CameraConsumer::ResidualStabilise: return "camera residual stabilise";
		case CameraConsumer::TemporalDetail: return "camera temporal detail";
		case CameraConsumer::DetailPulse: return "camera detail pulse";
		case CameraConsumer::TextureRestore: return "camera texture restore";
		case CameraConsumer::DitherVectors: return "camera dither vectors";
		case CameraConsumer::Debander: return "camera debander";
		case CameraConsumer::OcclusionFiller: return "camera occlusion filler";
		case CameraConsumer::Measurement: return "camera measurement";
		case CameraConsumer::DejudderWarp: return "camera de-judder warp";
		case CameraConsumer::HiccupRepair: return "camera hiccup repair";
		case CameraConsumer::RollOff: return "camera roll-off";
		case CameraConsumer::ObjectSplit: return "camera object split";
		}
		return "?";
	}

	// The accessor's own consumer enum (CameraAccessor.h) where one exists.
	inline bool ToModelConsumer(CameraConsumer c, CameraModel::Consumer* out)
	{
		CameraModel::Consumer m = CameraModel::Consumer::Debander;
		switch (c)
		{
		case CameraConsumer::GrainMap: m = CameraModel::Consumer::GrainMap; break;
		case CameraConsumer::ResidualStabilise: m = CameraModel::Consumer::ResidualStabilise; break;
		case CameraConsumer::TextureRestore: m = CameraModel::Consumer::TextureRestore; break;
		case CameraConsumer::DitherVectors: m = CameraModel::Consumer::DitherVectors; break;
		case CameraConsumer::Debander: m = CameraModel::Consumer::Debander; break;
		case CameraConsumer::OcclusionFiller: m = CameraModel::Consumer::OcclusionMask; break;
		case CameraConsumer::DejudderWarp: m = CameraModel::Consumer::DejudderWarp; break;
		case CameraConsumer::RollOff: m = CameraModel::Consumer::RollOff; break;
		case CameraConsumer::ObjectSplit: m = CameraModel::Consumer::CameraObjectSplit; break;
		default: return false;
		}
		if (out)
			*out = m;
		return true;
	}

	struct CameraDemandInput
	{
		bool active[kCameraConsumerCount] = {};
	};

	// The declaration of one camera consumer (plan 45 section 2).
	inline VpSkeleton::ConsumerDecl DeclarationOf(CameraConsumer c)
	{
		using namespace VpSkeleton;
		ConsumerDecl d;
		d.id = uint16_t(kCameraConsumerIdBase + static_cast<int>(c));
		d.name = Name(c);
		switch (c)
		{
		case CameraConsumer::Diagnostics:
			AddConsume(d, kP7CameraFit);
			AddConsume(d, kP8CameraLabels);
			break;
		case CameraConsumer::GrainMap:
			AddConsume(d, kP8CameraLabels);
			AddConsume(d, kP3AnalysisField);
			break;
		case CameraConsumer::ResidualStabilise:
		case CameraConsumer::DitherVectors:
		case CameraConsumer::Debander:
		case CameraConsumer::OcclusionFiller:
			AddConsume(d, kP8CameraLabels);
			break;
		case CameraConsumer::TemporalDetail:
		case CameraConsumer::DetailPulse:
		case CameraConsumer::TextureRestore:
			AddConsume(d, kP7CameraFit);
			AddConsume(d, kP8CameraLabels);
			AddConsume(d, kP9CameraSmoothed);
			break;
		case CameraConsumer::Measurement:
			AddConsume(d, kP7CameraFit, 1);   // causal: the previous frame's fit, never P9
			break;
		case CameraConsumer::DejudderWarp:
		case CameraConsumer::HiccupRepair:
		case CameraConsumer::RollOff:
		case CameraConsumer::ObjectSplit:
			AddConsume(d, kP9CameraSmoothed);
			break;
		}
		d.holdsSpan = c == CameraConsumer::DitherVectors || c == CameraConsumer::DejudderWarp;
		d.hasReducedMode = true;
		return d;
	}

	// Appends one declaration per active consumer, in CameraConsumer order (deterministic).
	inline void DeclareCameraConsumers(const CameraDemandInput& in, std::vector<VpSkeleton::ConsumerDecl>& out)
	{
		for (int i = 0; i < kCameraConsumerCount; ++i)
			if (in.active[i])
				out.push_back(DeclarationOf(ConsumerAt(i)));
	}
}
