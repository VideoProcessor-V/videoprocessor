// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/modules/control/skeleton/PlTexBackend.h>

#include <libplacebo/gpu.h>

#include <string>

// 70b: the texture backend of the lane GPU products' ProductTextures (the product store; the allocator core is
// VP-V/core/GpuAllocator.h): PlTexBackend, optionally host-readable (tests only). It lives here, beside PlTexBackend,
// because the product store is one of the two places that may create textures (the other is the scratch arena): the
// private-texture scan does not look into this folder.
namespace VpLaneProducts
{
	struct GpuTexBackend : VpSkeleton::PlTexBackend
	{
		explicit GpuTexBackend(pl_gpu g = nullptr, bool readable = false) : VpSkeleton::PlTexBackend(g), hostReadable(readable) {}
		// Host readback (tests only) is requested only when the format supports it (PL_FMT_CAP_HOST_READABLE): asking for a
		// capability the format lacks makes the create call refuse. No blit capability is requested (nothing here blits). A
		// failed create records WHY (lastError, shown by the dispatcher's status entry), never a bare "could not allocate".
		// lastError describes the most recent call only (cleared at its start).
		Handle Create(const VpSkeleton::TexShape& s)
		{
			lastError.clear();
			const pl_fmt fmt = FindFormat(gpu, s.fmt);
			if (!fmt)
			{
				lastError = "no sampleable and storable texture format for the product texture (format " +
					std::to_string(int(s.fmt)) + ", " + std::to_string(s.w) + "x" + std::to_string(s.h) + ")";
				return nullptr;
			}
			pl_tex_params tp{};
			tp.w = int(s.w);
			tp.h = int(s.h);
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.host_readable = hostReadable && (fmt->caps & PL_FMT_CAP_HOST_READABLE) != 0;
			const Handle h = pl_tex_create(gpu, &tp);
			if (!h)
				lastError = std::string("the GPU refused a product texture ") + std::to_string(s.w) + "x" + std::to_string(s.h) + " " +
					(fmt->name ? fmt->name : "?") + (tp.host_readable ? " (host readable)" : "");
			else if (hostReadable && !tp.host_readable)
				lastError = std::string("note: ") + (fmt->name ? fmt->name : "?") + " has no host readback; products in it cannot be read back";
			return h;
		}
		bool hostReadable;
		std::string lastError;
	};
}
