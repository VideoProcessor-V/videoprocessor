// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-1, decision D-K3): the guide product p7g_camera_guide. The measurement plan for pair N is a pure function of
// P7(N-d), the CPU field copy of that pair (prevFwd) and P4(N) (scene flags), so it is KNOWN AHEAD and declared as a lane
// product: Cpu, T0, about 0.6 MB at 4K (RGBA32F guide + R32F code = 20 B per 16x16 block), inputs P7 at lag d and P4 at lag 0,
// consumer "camera guidance" (the Measurement path). d is fixed per graph (GuideLagFor); it is recomputed only on a graph rebuild.
//
// Registration: the id follows the last SA0 id (kSkeletonProductEnd). It is NOT added to SkeletonProducts() / ProductRows() here:
// those tables are pinned by the catalog tests and the wiring step owns the integration. SkeletonProductsWithGuide(d) is the
// assembly with it; GuideRow() is its catalog row; GuideConsumerDecl(d) the consumer. Pure, header-only, C++14.
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
	constexpr VpSkeleton::ProductId kP7gCameraGuide = VpSkeleton::kSkeletonProductEnd;   // "after the last SA0 id"
	constexpr uint16_t kGuideConsumerId = kCameraConsumerIdBase + 0x40;                   // beside the 14 camera consumers
	constexpr uint32_t kGuideBytesPerBlock = 20;                                          // RGBA32F guide + R32F code
	constexpr uint8_t kMinGuideLag = 2;

	// d = max(2, 1 + ceil((readback + p95 fit) / source period)). All three in seconds, measured; unknown (<= 0) period -> 2.
	inline uint8_t GuideLagFor(double readbackSeconds, double p95FitSeconds, double sourcePeriodSeconds)
	{
		if (!(sourcePeriodSeconds > 0))
			return kMinGuideLag;
		const double need = std::max(0.0, readbackSeconds) + std::max(0.0, p95FitSeconds);
		const double d = 1.0 + std::ceil(need / sourcePeriodSeconds - 1e-9);
		const double clamped = std::min(static_cast<double>(VpSkeleton::kMaxInputs * 4), std::max(static_cast<double>(kMinGuideLag), d));
		return static_cast<uint8_t>(clamped);
	}

	inline void AppendCameraGuideProduct(std::vector<VpSkeleton::ProductDecl>& decls, uint8_t lagD)
	{
		using VpSkeleton::detail::AddInput;
		using VpSkeleton::detail::AddTerm;
		using VpSkeleton::detail::Make;
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		VpSkeleton::ProductDecl p = Make(kP7gCameraGuide, "p7g_camera_guide", "camera guide", VpSkeleton::Backend::Cpu, 0,
			VpSkeleton::Tier::T0, content, Bit(kSettingsMotion));
		AddInput(p, VpSkeleton::kP7CameraFit, lagD < kMinGuideLag ? kMinGuideLag : lagD);
		AddInput(p, VpSkeleton::kP4SourceStats, 0);
		p.residency = VpSkeleton::Residency::SystemRam;
		p.spanMask = content;   // a pair spanning a slip / full change has no guidance
		AddTerm(p, 4, kGuideBytesPerBlock);
		p.size.fixedBytes = 256;
		p.refRenderMs = 0.05f;
		decls.push_back(p);
	}

	// The skeleton registry with the guide product appended (id kP7gCameraGuide).
	inline VpSkeleton::Registry SkeletonProductsWithGuide(uint8_t lagD)
	{
		std::vector<VpSkeleton::ProductDecl> d = VpSkeleton::SkeletonProducts().All();
		AppendCameraGuideProduct(d, lagD);
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
