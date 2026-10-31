// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split on the GPU: the shared grain-free picture of one plane, as compute passes that mirror the CPU
// reference model GrainSplit::Compute (GrainSplit.h) pass by pass (same separable Gaussians and edge replication,
// same sub-block grain level, neighbourhood minimum and Wiener share). Plane-generic: luma for temporal_detail,
// luma and each chroma plane for the chroma upscaler's grain guard.
//
// Passes (all compute, nothing read back):
//   1. GH     rows:    Y -> (G1 Y, Gb Y, G6 Y, Y)                       (rgba32f, full size)
//   2. GV     columns: -> bands (d1, d2, d3, base)                       (rgba32f, full size)
//   3. QUIET  one thread per block: the 16 sub-block energies of D = d1 + d2, mean of the quietest x cal; the
//             block mean of d3^2 (the focus band's energy, read by temporal_detail)
//   4. NMIN   one thread per block: the minimum over (2 n + 1)^2 blocks -> blocks (N, e3)
//   5. F      per pixel: e3x3 = mean3x3(D^2), f = clamp(1 - kappa N / max(e, floor), 0, 1) -> split (f d1, f d2, f, base)
// The split texture reuses the rows texture of pass 1. Values are codes of the signal (the plane's value / CodeUnit).
// clean = base + s1 + s2 and G = Y - clean are formed by the reader (Y = clean + G exactly by construction).
//
// Cost (estimate): about 85 texture reads per pixel; two rgba32f planes (32 B/px: ~265 MB at 3840x2160, ~66 MB at
// 1080p; a chroma plane of 4:2:0 a quarter of that); well under 1.5 ms at 4K on an RTX 3090, ~4-6 ms on the RTX 3050
// Ti laptop (estimates; GrainSplitGpuTiming measures). Deterministic: fixed loop orders, no atomics.
#pragma once

#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <string>

namespace GrainSplit
{
	namespace Gpu
	{
		// The product of one frame of one plane (valid until the next Compute of the same Splitter).
		// Owner rule (2026-10-31): ONE producer, many readers. The host computes it once, early, on the decoded source
		// (its active picture), publishes it as the shared `grain.split` product (GrainSplit::kProductDecl), and every repair
		// reads the same Frame; the exact original grain is added back once, at the end of the source-domain block. A reader
		// never re-estimates grain from a processed frame; its own Splitter is only a visible fallback.
		struct Frame
		{
			pl_tex split = nullptr;    // w x h rgba32f: s1 = f d1, s2 = f d2, f, base (codes)
			pl_tex blocks = nullptr;   // bw x bh rgba32f: N (grain level, codes^2 of D), e3 (block mean of d3^2), 0, 0
			int w = 0, h = 0, bw = 0, bh = 0, B = 16;
			double scale = 1.0;
		};

		// Whether a published product fits a reader's plane (same size, block and scale): else the reader falls back visibly.
		inline bool Fits(const Frame* f, int w, int h, int B, double scale)
		{
			return f && f->split && f->blocks && f->w == w && f->h == h && f->B == B && f->scale == scale;
		}

		class Splitter
		{
		public:
			Splitter() = default;
			~Splitter() = default;   // textures belong to the gpu: Release
			Splitter(const Splitter&) = delete;
			Splitter& operator=(const Splitter&) = delete;

			// The split of `plane` (component `component`, 0..3) whose values are codes x codeUnit. scale multiplies
			// the sigmas, B is the block edge in plane pixels. False (with LastError) on any failure.
			bool Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B,
				const Params& params = Params());
			const Frame& Out() const { return m_out; }
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }

		private:
			pl_tex m_rows = nullptr, m_bands = nullptr, m_quiet = nullptr, m_blocks = nullptr;
			Frame m_out;
			std::string m_error;
		};

		// The Gaussian weights of GrainSplit::Gauss (radius max(1, ceil(3 sigma)), normalised over the taps).
		int GaussRadius(double sigma);
		double GaussWeight(int k, double sigma);
	}
}
