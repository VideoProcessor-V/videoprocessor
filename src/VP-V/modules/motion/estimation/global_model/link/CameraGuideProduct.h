// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-1, decision D-K3): the guide product p7g_camera_guide. The measurement plan for pair N is a pure function of
// P7(N-d), the CPU field copy of that pair (prevFwd) and P4(N) (scene flags), so it is KNOWN AHEAD and declared as a lane
// product: Cpu, T0, about 0.6 MB at 4K (RGBA32F guide + R32F code = 20 B per 16x16 block), inputs P7 at lag d and P4 at lag 0,
// consumer "camera guidance" (the Measurement path). d is fixed per graph (GuideLagFor); it is recomputed only on a graph rebuild.
//
// Registration (doc 72 item 1): the product is REGISTERED in core/Products.h (id kP7gCameraGuide, right after P14, with the floor
// lag kGuideDefaultLag) and in the catalog (row and the "camera guidance" consumer). SkeletonProductsWithGuide(d) is the registry with
// the MEASURED lag d set on its P7 input (a graph rebuild); GuideRow() is its catalog row; GuideConsumerDecl() the consumer.
// Pure, header-only, C++14.
#pragma once

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace CameraLink
{
	constexpr VpSkeleton::ProductId kP7gCameraGuide = VpSkeleton::kP7gCameraGuide;       // registered in core/Products.h
	constexpr uint16_t kGuideConsumerId = kCameraConsumerIdBase + 0x40;                   // beside the 14 camera consumers
	constexpr uint32_t kGuideBytesPerBlock = 20;                                          // RGBA32F guide + R32F code
	constexpr uint8_t kMinGuideLag = 2;
	constexpr uint8_t kMaxGuideLag = 16;   // was kMaxInputs * 4 (= 16 when kMaxInputs was 4): pinned, not tied to the input limit

	// d = max(2, 1 + ceil((readback + p95 fit) / source period)). All three in seconds, measured; unknown (<= 0) period -> 2.
	inline uint8_t GuideLagFor(double readbackSeconds, double p95FitSeconds, double sourcePeriodSeconds)
	{
		if (!(sourcePeriodSeconds > 0))
			return kMinGuideLag;
		const double need = std::max(0.0, readbackSeconds) + std::max(0.0, p95FitSeconds);
		const double d = 1.0 + std::ceil(need / sourcePeriodSeconds - 1e-9);
		const double clamped = std::min(static_cast<double>(kMaxGuideLag), std::max(static_cast<double>(kMinGuideLag), d));
		return static_cast<uint8_t>(clamped);
	}

	// Sets the measured lag d on the guide's P7 input (the registered declaration carries the floor lag).
	inline void SetGuideLag(std::vector<VpSkeleton::ProductDecl>& decls, uint8_t lagD)
	{
		VpSkeleton::ProductDecl& p = decls[kP7gCameraGuide];
		p.inputs[0].lag = lagD < kMinGuideLag ? kMinGuideLag : lagD;
	}

	// The skeleton registry with the guide's measured lag d.
	inline VpSkeleton::Registry SkeletonProductsWithGuide(uint8_t lagD)
	{
		std::vector<VpSkeleton::ProductDecl> d = VpSkeleton::SkeletonProducts().All();
		SetGuideLag(d, lagD);
		return VpSkeleton::Registry(std::move(d));
	}

	// The consumer "camera guidance": reads the guide product at the pair about to be searched.
	inline VpSkeleton::ConsumerDecl GuideConsumerDecl()
	{
		VpSkeleton::ConsumerDecl c;
		c.id = kGuideConsumerId;
		c.name = "camera guidance";
		VpSkeleton::AddConsume(c, kP7gCameraGuide);
		c.hasReducedMode = true;   // refusal: the matcher runs today's full search (visible)
		return c;
	}

	inline VpSkeleton::Catalog::ProductRow GuideRow()
	{
		return { kP7gCameraGuide, "motion/estimation/global_model", VpSkeleton::Catalog::kExpectedCameraUnit, "camera guide (camera worker)" };
	}
}
