// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: direct photometric refinement of the chosen model (doc 103 s3 R3). CPU reference.
// Inverse compositional Gauss-Newton (Baker-Matthews): the template T = frame N-1 (its gradients and the warp
// Jacobian at the identity are computed once), the image I = frame N sampled at W(x; p). Per step the weighted
// normal equations of  gain * T(W(x; d)) + offset - I(W(x; p))  are accumulated in a fixed order in double, solved
// by Gaussian elimination, and the model is updated  W(p) <- W(p) o W(d)^-1.  See PhotometricRefine.h.
#include "PhotometricRefine.h"

#include <algorithm>
#include <cmath>

#include "GlobalModelGeometry.h"

namespace CameraModel
{
	namespace
	{
		template <typename Pix>
		struct Plane
		{
			const Pix* d = nullptr;
			int w = 0, h = 0, stride = 0;
			double k = 1;
			double At(int x, int y) const { return static_cast<double>(d[static_cast<size_t>(y) * stride + x]) * k; }
			uint32_t Code(int x, int y) const { return static_cast<uint32_t>(d[static_cast<size_t>(y) * stride + x]); }
		};

		// Keys cubic (a = -0.5) weights of the taps at -1, 0, 1, 2 for a fraction t in [0, 1).
		inline void KeysWeights(double t, double* wt)
		{
			wt[0] = ((-0.5 * t + 1.0) * t - 0.5) * t;
			wt[1] = (1.5 * t - 2.5) * t * t + 1.0;
			wt[2] = ((-1.5 * t + 2.0) * t + 0.5) * t;
			wt[3] = (0.5 * t - 0.5) * t * t;
		}

		// Sample at continuous pixel coordinates (u, v) (pixel centres at integers). False when a tap would leave the plane
		// (no clamping: a clamped sample biases the solution) or the position is not finite.
		template <typename Pix>
		inline bool SampleBicubic(const Plane<Pix>& p, double u, double v, double* out)
		{
			if (!(u >= 1.0 && v >= 1.0 && u < p.w - 2.0 && v < p.h - 2.0))
				return false;
			const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
			double wx[4], wy[4];
			KeysWeights(u - x0, wx);
			KeysWeights(v - y0, wy);
			const Pix* row = p.d + static_cast<size_t>(y0 - 1) * p.stride + (x0 - 1);
			double acc = 0;
			for (int r = 0; r < 4; ++r)
			{
				const Pix* q = row + static_cast<size_t>(r) * p.stride;
				acc += wy[r] * (wx[0] * q[0] + wx[1] * q[1] + wx[2] * q[2] + wx[3] * q[3]);
			}
			*out = acc * p.k;
			return true;
		}

		template <typename Pix>
		inline bool SampleBilinear(const Plane<Pix>& p, double u, double v, double* out)
		{
			if (!(u >= 0.0 && v >= 0.0 && u < p.w - 1.0 && v < p.h - 1.0))
				return false;
			const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
			const double fx = u - x0, fy = v - y0;
			const Pix* q = p.d + static_cast<size_t>(y0) * p.stride + x0;
			const double a = (1 - fx) * q[0] + fx * q[1];
			const double b = (1 - fx) * q[p.stride] + fx * q[p.stride + 1];
			*out = ((1 - fy) * a + fy * b) * p.k;
			return true;
		}

		// Gaussian elimination with partial pivoting on an n x n system (n <= 6), in place. False when singular relative
		// to the matrix's own diagonal scale.
		bool Solve(double a[6][6], double* b, int n, double* x)
		{
			double scale = 0;
			for (int i = 0; i < n; ++i)
				scale = std::max(scale, std::fabs(a[i][i]));
			if (!(scale > 0) || !IsFiniteBits(scale))
				return false;
			for (int c = 0; c < n; ++c)
			{
				int piv = c;
				for (int r = c + 1; r < n; ++r)
					if (std::fabs(a[r][c]) > std::fabs(a[piv][c]))
						piv = r;
				if (!(std::fabs(a[piv][c]) > 1e-12 * scale))
					return false;
				if (piv != c)
				{
					for (int k = 0; k < n; ++k)
						std::swap(a[c][k], a[piv][k]);
					std::swap(b[c], b[piv]);
				}
				for (int r = c + 1; r < n; ++r)
				{
					const double f = a[r][c] / a[c][c];
					for (int k = c; k < n; ++k)
						a[r][k] -= f * a[c][k];
					b[r] -= f * b[c];
				}
			}
			for (int r = n - 1; r >= 0; --r)
			{
				double v = b[r];
				for (int k = r + 1; k < n; ++k)
					v -= a[r][k] * x[k];
				x[r] = v / a[r][r];
				if (!IsFiniteBits(x[r]))
					return false;
			}
			return true;
		}

		inline double HuberCost(double r, double c)
		{
			const double a = std::fabs(r);
			return a <= c ? 0.5 * r * r : c * (a - 0.5 * c);
		}

		bool ParamsFinite(const Params& p)
		{
			for (int i = 0; i < 9; ++i)
				if (!IsFiniteBits(p.h[i]))
					return false;
			return true;
		}
	}

	const char* RefineStatusText(RefineStatus status)
	{
		switch (status)
		{
		case RefineStatus::Refined: return "refined";
		case RefineStatus::FellBackNoImprovement: return "fell back: photometric residual did not fall";
		case RefineStatus::FellBackTooFar: return "fell back: correction beyond the sub-pixel basin";
		case RefineStatus::FellBackPhotometric: return "fell back: photometric gain out of range";
		case RefineStatus::FellBackDegenerate: return "fell back: degenerate system";
		case RefineStatus::SkippedNoLuma: return "skipped: no luma pair";
		case RefineStatus::SkippedFewPixels: return "skipped: too few gradient pixels";
		case RefineStatus::SkippedModel: return "skipped: no usable model";
		}
		return "?";
	}

	InlierMask MaskOf(const BlockData& blocks)
	{
		InlierMask m;
		const size_t cells = static_cast<size_t>(blocks.gridW > 0 ? blocks.gridW : 0) * static_cast<size_t>(blocks.gridH > 0 ? blocks.gridH : 0);
		if (cells == 0 || blocks.label.size() != cells || !(blocks.blockSize > 0))
			return m;
		m.label = blocks.label.data();
		m.excluded = blocks.excluded.size() == cells ? blocks.excluded.data() : nullptr;
		m.gridW = blocks.gridW;
		m.gridH = blocks.gridH;
		m.blockSize = blocks.blockSize;
		return m;
	}

	struct PhotometricRefiner::Impl
	{
		template <typename Pix>
		static void Run(PhotometricRefiner& self, const Params& model, const Plane<Pix>& A, const Plane<Pix>& B, double s,
			const Rect& pic, const InlierMask& mask, RefineResult& res)
		{
			const RefineOptions& o = self.m_options;
			const int w = A.w, h = A.h;
			const double cx = pic.CentreX(), cy = pic.CentreY();
			const double margin = std::max(0.0, o.borderLumaPx) * s;
			const bool useMask = mask.label != nullptr && mask.gridW > 0 && mask.gridH > 0 && mask.blockSize > 0;
			res.maskUsed = useMask;

			// 1. Candidates: inlier-block pixels inside the picture with a gradient; counts per gradient-magnitude bin.
			static const int kBins = 4096;   // quarter-code bins of |dx| + |dy| (8-bit-equivalent codes)
			std::vector<uint32_t> hist(kBins, 0u);
			std::vector<uint16_t>& bin = self.m_bin;
			bin.assign(static_cast<size_t>(w) * h, 0xFFFFu);
			uint32_t candidates = 0;
			for (int j = 1; j < h - 1; ++j)
			{
				const double fy = s * (j + 0.5);
				if (fy < pic.top + margin || fy > pic.bottom - margin)
					continue;
				int by = 0;
				if (useMask)
				{
					by = static_cast<int>(fy / mask.blockSize);
					if (by >= mask.gridH)
						continue;
				}
				for (int i = 1; i < w - 1; ++i)
				{
					const double fx = s * (i + 0.5);
					if (fx < pic.left + margin || fx > pic.right - margin)
						continue;
					if (useMask)
					{
						const int bx = static_cast<int>(fx / mask.blockSize);
						if (bx >= mask.gridW)
							continue;
						const size_t cell = static_cast<size_t>(by) * mask.gridW + bx;
						if (mask.label[cell] != static_cast<uint8_t>(Label::Camera) || (mask.excluded && mask.excluded[cell]))
							continue;
					}
					const double mag = CandidateGradientCodes(A.Code(i - 1, j), A.Code(i + 1, j), A.Code(i, j - 1), A.Code(i, j + 1), A.k);
					if (!(mag >= o.minGradientCodes))
						continue;
					const int b = static_cast<int>(std::min(static_cast<double>(kBins - 1), mag * 4.0));
					bin[static_cast<size_t>(j) * w + i] = static_cast<uint16_t>(b);
					++hist[b];
					++candidates;
				}
			}
			res.candidates = candidates;
			if (static_cast<int>(candidates) < std::max(1, o.minPixels))
			{
				res.status = RefineStatus::SkippedFewPixels;
				return;
			}

			// 2. Selection: the strongest gradients, deterministic (threshold bin from the top; ties in raster order).
			const uint32_t target = static_cast<uint32_t>(std::max(o.minPixels, o.targetPixels));
			int thr = kBins - 1;
			uint32_t above = 0;   // count with bin > thr
			while (thr > 0 && above + hist[thr] < target)
			{
				above += hist[thr];
				--thr;
			}
			uint32_t tiesLeft = target > above ? target - above : 0u;
			const double L = 0.5 * std::sqrt(static_cast<double>(pic.Width()) * pic.Width() + static_cast<double>(pic.Height()) * pic.Height());
			std::vector<double>& X = self.m_x;
			std::vector<double>& Y = self.m_y;
			std::vector<double>& T = self.m_t;
			std::vector<double>& GX = self.m_gx;
			std::vector<double>& GY = self.m_gy;
			X.clear(); Y.clear(); T.clear(); GX.clear(); GY.clear();
			for (int j = 1; j < h - 1; ++j)
				for (int i = 1; i < w - 1; ++i)
				{
					const uint16_t b = bin[static_cast<size_t>(j) * w + i];
					if (b == 0xFFFFu || static_cast<int>(b) < thr)
						continue;
					if (static_cast<int>(b) == thr)
					{
						if (tiesLeft == 0)
							continue;
						--tiesLeft;
					}
					X.push_back(s * (i + 0.5) - cx);
					Y.push_back(s * (j + 0.5) - cy);
					T.push_back(A.At(i, j));
					GX.push_back((A.At(i + 1, j) - A.At(i - 1, j)) / (2.0 * s));   // codes per SOURCE px
					GY.push_back((A.At(i, j + 1) - A.At(i, j - 1)) / (2.0 * s));
				}
			const size_t n = X.size();
			res.pixels = static_cast<uint32_t>(n);

			const int nGeo = (o.translationOnly || model.type == ModelType::Translation) ? 2 : 4;
			const int nPar = nGeo + (o.fadeTerms ? 2 : 0);
			const double diag = 2.0 * L;
			std::vector<double>& R = self.m_r;
			std::vector<double>& R0 = self.m_r0;
			std::vector<uint8_t>& OK = self.m_ok;
			std::vector<uint8_t>& OK0 = self.m_ok0;
			std::vector<double>& ABS = self.m_abs;
			R.resize(n); OK.resize(n);
			const bool bicubic = o.interp == RefineInterp::Bicubic;

			Params p = model;
			double gain = 1, offset = 0, sigma = o.minScaleCodes;
			bool done = false, degenerate = false;
			int iterations = 0;
			double lastStep = 0;
			for (int pass = 0;; ++pass)
			{
				// Residuals r = gain * T + offset - I(W(x; p)) and the robust sigma (1.4826 x median |r|).
				ABS.clear();
				for (size_t k = 0; k < n; ++k)
				{
					double qx = 0, qy = 0, iv = 0;
					Apply(p, X[k], Y[k], &qx, &qy);
					const double u = (qx + cx) / s - 0.5, v = (qy + cy) / s - 0.5;
					const bool ok = bicubic ? SampleBicubic(B, u, v, &iv) : SampleBilinear(B, u, v, &iv);
					OK[k] = ok ? 1 : 0;
					R[k] = ok ? gain * T[k] + offset - iv : 0.0;
					if (ok)
						ABS.push_back(std::fabs(R[k]));
				}
				if (pass == 0)
				{
					R0 = R;
					OK0 = OK;
				}
				if (static_cast<int>(ABS.size()) < std::max(1, o.minPixels))
				{
					degenerate = true;
					break;
				}
				const size_t mid = ABS.size() / 2;
				std::nth_element(ABS.begin(), ABS.begin() + static_cast<std::ptrdiff_t>(mid), ABS.end());
				sigma = std::max(o.minScaleCodes, 1.4826 * ABS[mid]);
				if (done)
					break;

				// Weighted normal equations (Huber), fixed order, double.
				const double c = o.huberK * sigma;
				double H[6][6] = {}, g[6] = {};
				for (size_t k = 0; k < n; ++k)
				{
					if (!OK[k])
						continue;
					const double r = R[k];
					const double a = std::fabs(r);
					const double wt = a <= c ? 1.0 : c / a;
					double J[6];
					J[0] = gain * GX[k];
					J[1] = gain * GY[k];
					int q = 2;
					if (nGeo == 4)
					{
						J[2] = gain * (GX[k] * X[k] + GY[k] * Y[k]) / L;
						J[3] = gain * (GY[k] * X[k] - GX[k] * Y[k]) / L;
						q = 4;
					}
					if (o.fadeTerms)
					{
						J[q] = T[k];
						J[q + 1] = 1.0;
					}
					for (int ia = 0; ia < nPar; ++ia)
					{
						const double wa = wt * J[ia];
						g[ia] += wa * r;
						for (int ib = ia; ib < nPar; ++ib)
							H[ia][ib] += wa * J[ib];
					}
				}
				for (int ia = 0; ia < nPar; ++ia)
				{
					g[ia] = -g[ia];
					for (int ib = 0; ib < ia; ++ib)
						H[ia][ib] = H[ib][ia];
				}
				double d[6] = {};
				if (!Solve(H, g, nPar, d))
				{
					degenerate = true;
					break;
				}
				// W(d): translation (d0, d1) and, for 4 parameters, the similarity (1 + a, -b; b, 1 + a), a = d2 / L, b = d3 / L.
				Params S;
				S.type = nGeo == 4 ? ModelType::Similarity : ModelType::Translation;
				const double sa = nGeo == 4 ? d[2] / L : 0.0, sb = nGeo == 4 ? d[3] / L : 0.0;
				S.h[0] = 1 + sa; S.h[1] = -sb; S.h[2] = d[0];
				S.h[3] = sb; S.h[4] = 1 + sa; S.h[5] = d[1];
				bool inv = false;
				const Params Si = Invert(S, &inv);
				if (!inv)
				{
					degenerate = true;
					break;
				}
				Params next = Compose(p, Si);
				next.type = model.type;
				if (!ParamsFinite(next))
				{
					degenerate = true;
					break;
				}
				const double step = MaxCornerDifference(next, p, pic) * diag;
				lastStep = step;
				p = next;
				if (o.fadeTerms)
				{
					gain += d[nGeo];
					offset += d[nGeo + 1];
				}
				++iterations;
				done = !(step > o.convergeLumaPx * s) || iterations >= std::max(1, o.maxIterations);
			}
			res.iterations = iterations;
			res.lastStepPx = lastStep;
			res.robustSigma = sigma;
			if (degenerate || IsDegenerate(p, pic) || !IsFiniteBits(gain) || !IsFiniteBits(offset))
			{
				res.status = RefineStatus::FellBackDegenerate;
				return;
			}
			res.correctionPx = MaxCornerDifference(p, model, pic) * diag;

			// Verify against the record: the same pixels (valid under both models) and the same sigma.
			const double c = o.huberK * sigma;
			double before = 0, after = 0;
			size_t both = 0;
			for (size_t k = 0; k < n; ++k)
			{
				if (!OK0[k] || !OK[k])
					continue;
				before += HuberCost(R0[k], c);
				after += HuberCost(R[k], c);
				++both;
			}
			if (static_cast<int>(both) < std::max(1, o.minPixels))
			{
				res.status = RefineStatus::FellBackDegenerate;
				return;
			}
			res.residualBefore = before / static_cast<double>(both);
			res.residualAfter = after / static_cast<double>(both);
			if (o.fadeTerms && !(gain >= o.minGain && gain <= o.maxGain))
				res.status = RefineStatus::FellBackPhotometric;
			else if (res.correctionPx > o.maxCorrectionLumaPx * s)
				res.status = RefineStatus::FellBackTooFar;
			else if (!(res.residualAfter < res.residualBefore * (1.0 - o.minImprovement)))
				res.status = RefineStatus::FellBackNoImprovement;
			else
			{
				res.status = RefineStatus::Refined;
				res.model = p;
				res.gain = gain;
				res.offset = offset;
			}
		}
	};

	RefineResult PhotometricRefiner::Refine(const Params& model, const LumaPairView& luma, const Rect& picture, const InlierMask& mask)
	{
		RefineResult res;
		res.model = model;
		const LumaPlane& a = luma.from;
		const LumaPlane& b = luma.to;
		const double s = luma.sourcePxPerPixel;
		const bool lumaOk = a.Valid() && b.Valid() && (a.data8 != nullptr) == (b.data8 != nullptr) && a.width == b.width && a.height == b.height
			&& s > 0 && IsFiniteBits(s) && luma.codeScale > 0 && IsFiniteBits(luma.codeScale) && a.width >= 8 && a.height >= 8
			&& !picture.Empty() && picture.left >= 0 && picture.top >= 0
			&& picture.right <= a.width * s + s && picture.bottom <= a.height * s + s;
		if (!lumaOk)
			res.status = RefineStatus::SkippedNoLuma;
		else if (model.type == ModelType::None || !ParamsFinite(model) || IsDegenerate(model, picture))
			res.status = RefineStatus::SkippedModel;
		else if (a.data8)
		{
			Plane<uint8_t> pa, pb;
			pa.d = a.data8; pa.w = a.width; pa.h = a.height; pa.stride = a.stride; pa.k = luma.codeScale;
			pb.d = b.data8; pb.w = b.width; pb.h = b.height; pb.stride = b.stride; pb.k = luma.codeScale;
			Impl::Run(*this, model, pa, pb, s, picture, mask, res);
		}
		else
		{
			Plane<uint16_t> pa, pb;
			pa.d = a.data16; pa.w = a.width; pa.h = a.height; pa.stride = a.stride; pa.k = luma.codeScale;
			pb.d = b.data16; pb.w = b.width; pb.h = b.height; pb.stride = b.stride; pb.k = luma.codeScale;
			Impl::Run(*this, model, pa, pb, s, picture, mask, res);
		}
		if (res.status != RefineStatus::Refined)
		{
			res.model = model;
			res.gain = 1;
			res.offset = 0;
		}
		res.model.type = model.type;
		m_counters.Add(res.status);
		return res;
	}

	RefineResult PhotometricRefine(const Params& model, const LumaPairView& luma, const Rect& picture, const InlierMask& mask,
		const RefineOptions& options)
	{
		PhotometricRefiner refiner(options);
		return refiner.Refine(model, luma, picture, mask);
	}
}
