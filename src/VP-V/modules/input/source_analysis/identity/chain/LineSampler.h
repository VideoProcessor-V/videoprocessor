// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// LineSampler (SID-C1 unit 1): the deterministic plan of which picture lines the chain estimator reads.
//
// 32 rows and 32 columns (settings) are chosen INSIDE the active rectangle, 48 px away from its edges (the
// bars), stratified: the inner extent is cut into `count` equal strata and one line is picked per stratum, the
// position inside the stratum coming from an integer hash of (source sequence number, axis, stratum). The plan
// is a pure function of (active rectangle, sequence, settings). It skips nothing itself: the caller decides
// not to ask for a plan on cadence repeats and blank frames (Indicator).
//
// Row lines run along x over the inner width, column lines along y over the inner height. `Fill` copies one
// axis out of a plane (8/10/16-bit integer or float samples) into a LineSet in 10-bit code units.
#include "ChainCommon.h"

namespace SourceIdentity
{
	namespace Chain
	{
		struct LineRect
		{
			int x = 0;
			int y = 0;
			int w = 0;
			int h = 0;
		};

		struct SamplerSettings
		{
			int rows = 32;      // row lines (axis 0)
			int cols = 32;      // column lines (axis 1)
			int margin = 48;    // px between the active rectangle edge and the first/last sample
			int minRun = 64;    // a line shorter than this (inner extent) is not worth analysing
		};

		struct LinePlan
		{
			bool valid = false;
			LineRect active;                 // as given
			LineRect inner;                  // active rectangle shrunk by the margin
			int rowCount = 0;
			int colCount = 0;
			int rowY[kMaxLines] = {};    // frame y of every row line
			int colX[kMaxLines] = {};    // frame x of every column line
		};

		class LineSampler
		{
		public:
			static LinePlan Plan(const LineRect& active, uint64_t seq, const SamplerSettings& s = SamplerSettings())
			{
				LinePlan p;
				p.active = active;
				const int rows = s.rows < 0 ? 0 : (s.rows > kMaxLines ? kMaxLines : s.rows);
				const int cols = s.cols < 0 ? 0 : (s.cols > kMaxLines ? kMaxLines : s.cols);
				p.inner.x = active.x + s.margin;
				p.inner.y = active.y + s.margin;
				p.inner.w = active.w - 2 * s.margin;
				p.inner.h = active.h - 2 * s.margin;
				if (p.inner.w < s.minRun || p.inner.h < s.minRun)
					return p;   // invalid: bars/margins leave too little
				if (p.inner.h < rows || p.inner.w < cols)
					return p;
				for (int i = 0; i < rows; ++i)
					p.rowY[i] = p.inner.y + Pick(p.inner.h, rows, i, seq, 0x51u);
				for (int i = 0; i < cols; ++i)
					p.colX[i] = p.inner.x + Pick(p.inner.w, cols, i, seq, 0xa7u);
				p.rowCount = rows;
				p.colCount = cols;
				p.valid = rows > 0 || cols > 0;
				return p;
			}

			// Copies axis `axis` of the plan out of a plane. `scale` converts a sample to 10-bit code units
			// (4 for an 8-bit plane, 1 for 10-bit, 0.25 for 12-bit). `stride` is in samples.
			// `scalerRect` is the rectangle the player's scaler axis spans: the active rectangle (default) when it
			// scaled the picture alone, the whole frame when the bars were part of the raster it scaled. The
			// LineSet's axisLen and origin (the geometry of the centre-aligned mapping) follow from it.
			template <class Pix>
			static bool Fill(const LinePlan& plan, int axis, const Pix* plane, std::ptrdiff_t stride, double scale, LineSet& out,
				const LineRect* scalerRect = nullptr)
			{
				if (!plan.valid || plane == nullptr)
					return false;
				const LineRect& sr = scalerRect != nullptr ? *scalerRect : plan.active;
				if (axis == 0)
				{
					if (plan.rowCount <= 0)
						return false;
					out.axis = 0;
					out.axisLen = sr.w;
					out.origin = plan.inner.x - sr.x;
					out.Resize(plan.rowCount, plan.inner.w);
					for (int j = 0; j < plan.rowCount; ++j)
					{
						const Pix* src = plane + static_cast<std::ptrdiff_t>(plan.rowY[j]) * stride + plan.inner.x;
						double* dst = out.Line(j);
						for (int i = 0; i < plan.inner.w; ++i)
							dst[i] = static_cast<double>(src[i]) * scale;
						out.cross[static_cast<size_t>(j)] = plan.rowY[j] - sr.y;
					}
					return true;
				}
				if (plan.colCount <= 0)
					return false;
				out.axis = 1;
				out.axisLen = sr.h;
				out.origin = plan.inner.y - sr.y;
				out.Resize(plan.colCount, plan.inner.h);
				for (int j = 0; j < plan.colCount; ++j)
				{
					const Pix* src = plane + static_cast<std::ptrdiff_t>(plan.inner.y) * stride + plan.colX[j];
					double* dst = out.Line(j);
					for (int i = 0; i < plan.inner.h; ++i)
						dst[i] = static_cast<double>(src[static_cast<std::ptrdiff_t>(i) * stride]) * scale;
					out.cross[static_cast<size_t>(j)] = plan.colX[j] - sr.x;
				}
				return true;
			}

		private:
			// Offset (0..extent-1) of the line of stratum `i` out of `count` strata over `extent` samples.
			static int Pick(int extent, int count, int i, uint64_t seq, uint32_t salt)
			{
				const int lo = static_cast<int>((static_cast<int64_t>(extent) * i) / count);
				const int hi = static_cast<int>((static_cast<int64_t>(extent) * (i + 1)) / count);   // exclusive
				const int span = hi - lo > 0 ? hi - lo : 1;
				const uint32_t h = HashMix(seq, salt * 1000u + static_cast<uint32_t>(i));
				return lo + static_cast<int>(h % static_cast<uint32_t>(span));
			}
		};
	}
}
