// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The libplacebo glue of GpuAttribution.h (iface-L-DET section 2).
//   GpuAttribution::Watch(dispatch, "owner");   one line after a VP pl_dispatch_create
//   GpuAttribution::NoteRenderPass(info);         the PassTimerHook info_callback (passes inside pl_render_image)
// Watch installs the pl_dispatch callback only while ComputeOnce::Enabled() (otherwise nothing is installed and
// libplacebo keeps its timers off for that dispatch): the off cost is the one flag load at creation.

#include <VP-V/core/GpuAttribution.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

namespace GpuAttribution
{
	inline void NoteDispatchInfo(const struct pl_dispatch_info* info, const char* defaultOwner)
	{
		if (!info || !ComputeOnce::Enabled())
			return;
		const pl_shader_info s = info->shader;
		Global().Note(s ? s->steps : nullptr, s ? s->num_steps : 0, s ? s->description : nullptr, info->last, defaultOwner);
	}

	inline void DispatchCallback(void* priv, const struct pl_dispatch_info* info)
	{
		NoteDispatchInfo(info, static_cast<const char*>(priv));
	}

	// `owner` must outlive the dispatch (a string literal).
	inline void Watch(pl_dispatch dispatch, const char* owner)
	{
		if (dispatch && ComputeOnce::Enabled())
			pl_dispatch_callback(dispatch, const_cast<char*>(owner), &DispatchCallback);
	}

	inline void NoteRenderPass(const struct pl_render_info* info)
	{
		if (info)
			NoteDispatchInfo(info->pass, nullptr);
	}
}
