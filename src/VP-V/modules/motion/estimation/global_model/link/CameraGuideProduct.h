// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-1, decision D-K3): the guide product p7g_camera_guide. The measurement plan for pair N is a pure function of
// camera(N) = P7(N) (d = 0, on time) and P4(N) (scene flags), so it is KNOWN AHEAD and declared as a lane product: Cpu, T0, about
// 0.6 MB at 4K (RGBA32F guide + R32F code = 20 B per 16x16 block), inputs P7 at lag 0 and P4 at lag 0, consumer "camera guidance"
// (the Measurement path).
//
// Registration (doc 72 item 1): the product is REGISTERED in core/Products.h (id kP7gCameraGuide, right after P14, lag
// kGuideDefaultLag = 0) and in the catalog (row and the "camera guidance" consumer). GuideRow() is its catalog row;
// GuideConsumerDecl() the consumer.
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
