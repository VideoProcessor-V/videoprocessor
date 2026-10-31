// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale on the GPU: the CfL upsampler of ChromaCfl.h as libplacebo compute passes that mirror the CPU model
// step by step (grain guard, luma at the chroma sites, regression, guided-filter averaging, Catmull-Rom upsample with the
// clamp and the grain add-back; bars by the plain bar-side Catmull-Rom).
//
// Source layouts: plane 0 = luma (one component, mapped to Y); chroma either plane 1 with two components (CB and CR in any
// order) or planes 1 and 2 with one component each. The chroma siting is read from the chroma plane's shift_x / shift_y. The
// output frame is the source with 2 planes: the luma plane unchanged and a full-size two-component CbCr plane with zero
// shifts, so libplacebo must not (and will not) upsample again.
//
// Passes (all compute, nothing read back, no atomics, no barriers):
//   CROP    luma rect, chroma rect(s) into own textures, bit-exact (only when the active picture is not the whole plane)
//   SPLIT   repair.grain_split x3 (luma, Cb, Cr)           (GrainSplitGpu)
//   LO      chroma grid: tent luma at the sites; clean chroma; chroma grain          -> rgba32f (Ylo, CcB, CcR), rg32f (GcB, GcR)
//   STATS   chroma grid: regression, rho2, neighbour range                            -> rgba32f (aB, bB, aR, bR), rg32f, rgba32f
//   AVG     chroma grid: box average of a and b                                      -> rgba32f
//   UP      every luma pixel: Catmull-Rom + CfL (picture) / bar-side Catmull-Rom (bars) -> the output texture
//
// Off: the output is the source frame itself and no pass runs. On any failure Apply returns false with LastError and the
// output untouched: the caller renders the original frame and shows "Chroma upscale: libplacebo (<reason>)".
#pragma once

#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>

namespace ChromaUpscale
{
	namespace Gpu
	{
		struct Params
		{
			Settings settings;
			int colorDepth = 10;           // codes of the signal (the model's units)
			double lumaLines = 0.0;        // the picture's luma lines (sets the grain split's Scale); 0 = the luma plane's height
			ActiveRect active;             // the active picture in luma pixels (empty = whole plane); bars get the plain upsample
			CflParams cfl;
		};

		class CflModule
		{
		public:
			CflModule() = default;
			~CflModule();
			CflModule(const CflModule&) = delete;
			CflModule& operator=(const CflModule&) = delete;

			// `source` with its chroma upsampled to luma size (2 planes: luma, CbCr; shifts 0). Off, or a 4:4:4 frame:
			// `out` = `source`, no pass. False (and `out` untouched) on any failure.
			// The output frame's chroma texture belongs to the module and stays valid until the next Apply (a host that keeps
			// frames, e.g. render-ahead or frame mixing, copies it).
			bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }
			// off, ok, "4:4:4: nothing to upsample", or the failure reason.
			const std::string& Status() const { return m_status; }
			unsigned long long Failures() const { return m_failures; }

			// VRAM of the module's textures for a W x H luma plane and a cw x ch chroma plane (bytes; with the three splits').
			static uint64_t TextureBytes(int W, int H, int cw, int ch);

		private:
			struct Impl;
			Impl* m_impl = nullptr;
			std::string m_error, m_status = "off";
			unsigned long long m_failures = 0;
		};
	}
}
