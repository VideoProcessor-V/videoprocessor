// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Direct photometric refinement of the camera model (doc 103 s3 R3), CPU reference: synthetic pans with sub-pixel
// truth (1.25 engine px, alternating phase) at 1080- and 4K-equivalent engine scale, a fade pan, a mover-masked pan,
// similarity / affine models, the fall-back paths, determinism and a cost line.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/PhotometricRefine.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "CppUnitTest.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;

		const double kPi = 3.14159265358979323846;

		struct Wave { double kx, ky, ph, amp; };

		// Band-limited texture in frame source px: 128 + sum of plane waves, wavelengths 6..64 engine px, amplitude ~ sqrt(lambda).
		std::vector<Wave> MakeWaves(uint64_t seed, double s, double totalAmp)
		{
			SplitMix64 rng(seed);
			std::vector<Wave> waves;
			const int count = 12;
			double sum = 0;
			for (int k = 0; k < count; ++k)
			{
				const double lambdaLuma = 6.0 * std::pow(64.0 / 6.0, k / static_cast<double>(count - 1));
				const double lambda = lambdaLuma * s;
				const double ang = 2.0 * kPi * rng.Unit();
				Wave w;
				w.kx = 2.0 * kPi / lambda * std::cos(ang);
				w.ky = 2.0 * kPi / lambda * std::sin(ang);
				w.ph = 2.0 * kPi * rng.Unit();
				w.amp = std::sqrt(lambdaLuma);
				sum += w.amp;
				waves.push_back(w);
			}
			for (size_t k = 0; k < waves.size(); ++k)
				waves[k].amp *= totalAmp / sum;
			return waves;
		}

		// Source position X = M * Y + c (frame px) for an output position Y: the layer shows tex(X).
		struct Layer
		{
			const std::vector<Wave>* waves = nullptr;
			double m[4] = { 1, 0, 0, 1 };
			double c[2] = { 0, 0 };
			bool clip = false;                      // only inside [l, r) x [t, b) (frame px, by pixel centre)
			double l = 0, t = 0, r = 0, b = 0;
		};

		uint32_t Hash(uint64_t a)
		{
			SplitMix64 g(a);
			return static_cast<uint32_t>(g.Next() >> 32);
		}

		// Renders one engine-scale luma plane (pixel centre s*(i + 0.5)); layers later in the list paint over earlier ones.
		std::vector<double> RenderD(int w, int h, double s, const std::vector<Layer>& layers, double gain, double offset, uint64_t noiseSeed, double noiseAmp)
		{
			std::vector<double> out(static_cast<size_t>(w) * h, 0.0);
			std::vector<double> sx(w), cxv(w), sy(h), cy(h);
			for (size_t li = 0; li < layers.size(); ++li)
			{
				const Layer& L = layers[li];
				std::vector<double> acc(static_cast<size_t>(w) * h, 128.0);
				for (size_t k = 0; k < L.waves->size(); ++k)
				{
					const Wave& wv = (*L.waves)[k];
					const double kx = L.m[0] * wv.kx + L.m[2] * wv.ky;   // M^T k
					const double ky = L.m[1] * wv.kx + L.m[3] * wv.ky;
					const double ph = wv.ph + wv.kx * L.c[0] + wv.ky * L.c[1];
					for (int i = 0; i < w; ++i)
					{
						const double X = s * (i + 0.5);
						sx[i] = std::sin(kx * X);
						cxv[i] = std::cos(kx * X);
					}
					for (int j = 0; j < h; ++j)
					{
						const double Y = s * (j + 0.5);
						sy[j] = std::sin(ky * Y + ph);
						cy[j] = std::cos(ky * Y + ph);
					}
					for (int j = 0; j < h; ++j)
					{
						double* row = &acc[static_cast<size_t>(j) * w];
						const double a = wv.amp * cy[j], bb = wv.amp * sy[j];
						for (int i = 0; i < w; ++i)
							row[i] += sx[i] * a + cxv[i] * bb;
					}
				}
				for (int j = 0; j < h; ++j)
					for (int i = 0; i < w; ++i)
					{
						if (L.clip)
						{
							const double X = s * (i + 0.5), Y = s * (j + 0.5);
							if (X < L.l || X >= L.r || Y < L.t || Y >= L.b)
								continue;
						}
						out[static_cast<size_t>(j) * w + i] = acc[static_cast<size_t>(j) * w + i];
					}
			}
			for (int j = 0; j < h; ++j)
				for (int i = 0; i < w; ++i)
				{
					const size_t idx = static_cast<size_t>(j) * w + i;
					const uint32_t hv = Hash(noiseSeed * 0x100000001B3ull + idx);
					const double noise = noiseAmp * ((hv & 0xFFFFu) / 32767.5 - 1.0);
					out[idx] = gain * out[idx] + offset + noise;
				}
			return out;
		}

		std::vector<uint8_t> To8(const std::vector<double>& v)
		{
			std::vector<uint8_t> o(v.size());
			for (size_t k = 0; k < v.size(); ++k)
				o[k] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::floor(v[k] + 0.5))));
			return o;
		}

		std::vector<uint16_t> To16(const std::vector<double>& v)
		{
			std::vector<uint16_t> o(v.size());
			for (size_t k = 0; k < v.size(); ++k)
				o[k] = static_cast<uint16_t>(std::min(65535.0, std::max(0.0, std::floor(v[k] * 257.0 + 0.5))));
			return o;
		}

		Rect Pic(int w, int h)
		{
			Rect r;
			r.left = 0; r.top = 0; r.right = w; r.bottom = h;
			return r;
		}

		Params Translation(double tx, double ty)
		{
			Params p;
			p.type = ModelType::Translation;
			p.h[2] = tx;
			p.h[5] = ty;
			return p;
		}

		LumaPairView View8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h, double s)
		{
			LumaPairView v;
			v.from.data8 = a.data(); v.from.width = w; v.from.height = h; v.from.stride = w;
			v.to.data8 = b.data(); v.to.width = w; v.to.height = h; v.to.stride = w;
			v.sourcePxPerPixel = s;
			v.codeScale = 1.0;
			return v;
		}

		// Layer showing the texture moved by `shift` (frame px): X = Y - shift.
		Layer Shifted(const std::vector<Wave>& waves, double shiftX, double shiftY)
		{
			Layer L;
			L.waves = &waves;
			L.c[0] = -shiftX;
			L.c[1] = -shiftY;
			return L;
		}

		struct PanStats { double meanX = 0, meanY = 0, maxAbs = 0, maxLastStep = 0; int refined = 0, pairs = 0, maxIter = 0; };

		// A pan of `pairs` pairs: frame k is the texture shifted by k * t; pair k = (frame k, frame k + 1); the model starts
		// at t + err. Errors are measured in source px.
		PanStats RunPan(int frameW, int frameH, double s, double tx, double ty, double errX, double errY, int pairs, double gain, double offset,
			uint64_t seed, const RefineOptions& opt)
		{
			const int w = (frameW + static_cast<int>(s) - 1) / static_cast<int>(s), h = (frameH + static_cast<int>(s) - 1) / static_cast<int>(s);
			const std::vector<Wave> waves = MakeWaves(seed, s, 90.0);
			PhotometricRefiner refiner(opt);
			PanStats st;
			const Rect pic = Pic(frameW, frameH);
			for (int k = 0; k < pairs; ++k)
			{
				std::vector<Layer> la(1, Shifted(waves, k * tx, k * ty));
				std::vector<Layer> lb(1, Shifted(waves, (k + 1) * tx, (k + 1) * ty));
				const std::vector<uint8_t> A = To8(RenderD(w, h, s, la, 1.0, 0.0, seed * 31 + 2 * k, 1.0));
				const std::vector<uint8_t> B = To8(RenderD(w, h, s, lb, gain, offset, seed * 31 + 2 * k + 1, 1.0));
				const RefineResult r = refiner.Refine(Translation(tx + errX, ty + errY), View8(A, B, w, h, s), pic, InlierMask());
				const double ex = r.model.h[2] - tx, ey = r.model.h[5] - ty;
				st.meanX += ex;
				st.meanY += ey;
				st.maxAbs = std::max(st.maxAbs, std::max(std::fabs(ex), std::fabs(ey)));
				st.maxIter = std::max(st.maxIter, r.iterations);
				st.maxLastStep = std::max(st.maxLastStep, r.lastStepPx);
				if (r.status == RefineStatus::Refined)
					++st.refined;
				++st.pairs;
			}
			st.meanX /= pairs;
			st.meanY /= pairs;
			return st;
		}

		void Log(const char* text)
		{
			Logger::WriteMessage(text);
		}

		// Convergence (audit SHOULD 3): never more than maxIterations steps, and the last step moves no corner by more than
		// `limit` source px (the bias target), so the iteration cap does not cut off a solution still moving at that scale.
		void CheckConvergence(const char* name, int maxIter, double maxLastStep, double limit)
		{
			char msg[200];
			std::snprintf(msg, sizeof(msg), "%s convergence: max iterations %d (cap %d), max last step %.5f source px (limit %.3f)\n", name, maxIter,
				RefineOptions().maxIterations, maxLastStep, limit);
			Log(msg);
			Assert::IsTrue(maxIter >= 1 && maxIter <= RefineOptions().maxIterations, L"iterations outside 1..maxIterations");
			Assert::IsTrue(maxLastStep <= limit, L"last Gauss-Newton step above the bias target");
		}

		// Box-downscale (integer factor s) of an 8-bit full-resolution plane from crop origin (ox, oy), cw x ch source px,
		// plus deterministic uniform noise of +-noiseAmp codes; the producer's box convention (pixel centre s*(i + 0.5)).
		std::vector<uint8_t> BoxCrop(const std::vector<uint8_t>& full, int fullW, int ox, int oy, int cw, int ch, int s, uint64_t noiseSeed, double noiseAmp)
		{
			const int w = cw / s, h = ch / s;
			std::vector<uint8_t> o(static_cast<size_t>(w) * h);
			for (int j = 0; j < h; ++j)
				for (int i = 0; i < w; ++i)
				{
					double sum = 0;
					for (int b = 0; b < s; ++b)
						for (int a = 0; a < s; ++a)
							sum += full[static_cast<size_t>(oy + j * s + b) * fullW + (ox + i * s + a)];
					const size_t idx = static_cast<size_t>(j) * w + i;
					const uint32_t hv = Hash(noiseSeed * 0x100000001B3ull + idx);
					const double v = sum / (s * s) + noiseAmp * ((hv & 0xFFFFu) / 32767.5 - 1.0);
					o[idx] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::floor(v + 0.5))));
				}
			return o;
		}

		std::string EnvText(const char* n)
		{
			char* v = nullptr;
			size_t sz = 0;
			std::string out;
			if (_dupenv_s(&v, &sz, n) == 0 && v)
			{
				out = v;
				free(v);
			}
			return out;
		}
	}

	TEST_CLASS(PhotometricRefineTests)
	{
	public:
		TEST_METHOD(Pan1080HalfAlternatingPhaseBias)
		{
			// 1080 frame, half-size engine (s = 2): 2.5 source px = 1.25 engine px per frame, alternating phase; start 0.4 engine px off.
			const PanStats st = RunPan(1920, 1080, 2.0, 2.5, 0.0, 0.8, -0.5, 8, 1.0, 0.0, 11, RefineOptions());
			char msg[256];
			std::snprintf(msg, sizeof(msg), "1080 pan: refined %d/%d, bias x %.4f y %.4f, max |err| %.4f source px\n", st.refined, st.pairs, st.meanX, st.meanY, st.maxAbs);
			Log(msg);
			Assert::AreEqual(st.pairs, st.refined);
			Assert::IsTrue(std::fabs(st.meanX) <= 0.02 && std::fabs(st.meanY) <= 0.02, L"1080 bias above 0.02 source px");
			Assert::IsTrue(st.maxAbs <= 0.05, L"1080 per-pair error above 0.05 source px");
			CheckConvergence("1080 pan", st.maxIter, st.maxLastStep, 0.02);
		}

		TEST_METHOD(Pan4KQuarterAlternatingPhaseBias)
		{
			// 4K frame, quarter-size engine (s = 4): 5 source px = 1.25 engine px per frame; start 1.7 source px (0.42 engine px) off.
			const PanStats st = RunPan(3840, 2160, 4.0, 5.0, 0.0, 1.7, 0.6, 8, 1.0, 0.0, 12, RefineOptions());
			char msg[256];
			std::snprintf(msg, sizeof(msg), "4K pan: refined %d/%d, bias x %.4f y %.4f, max |err| %.4f source px\n", st.refined, st.pairs, st.meanX, st.meanY, st.maxAbs);
			Log(msg);
			Assert::AreEqual(st.pairs, st.refined);
			Assert::IsTrue(std::fabs(st.meanX) <= 0.04 && std::fabs(st.meanY) <= 0.04, L"4K bias above 0.04 source px");
			Assert::IsTrue(st.maxAbs <= 0.1, L"4K per-pair error above 0.1 source px");
			CheckConvergence("4K pan", st.maxIter, st.maxLastStep, 0.04);
		}

		TEST_METHOD(DiagonalPanBothAxes)
		{
			// 1.25 engine px in x and 0.75 in y (different phase sequences per axis).
			const PanStats st = RunPan(1920, 1080, 2.0, 2.5, 1.5, -0.7, 0.6, 6, 1.0, 0.0, 13, RefineOptions());
			char msg[256];
			std::snprintf(msg, sizeof(msg), "diagonal pan: refined %d/%d, bias x %.4f y %.4f, max |err| %.4f\n", st.refined, st.pairs, st.meanX, st.meanY, st.maxAbs);
			Log(msg);
			Assert::AreEqual(st.pairs, st.refined);
			Assert::IsTrue(std::fabs(st.meanX) <= 0.02 && std::fabs(st.meanY) <= 0.02);
			CheckConvergence("diagonal pan", st.maxIter, st.maxLastStep, 0.02);
		}

		TEST_METHOD(FadePanGainOffset)
		{
			// Frame N = 0.8 x frame N-1 + 10 codes, panning 1.25 engine px.
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(21, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 1, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 0.8, 10.0, 2, 1.0));
			const RefineResult r = PhotometricRefine(Translation(tx + 0.8, -0.4), View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			char msg[256];
			std::snprintf(msg, sizeof(msg), "fade: %s, err x %.4f y %.4f, gain %.4f offset %.3f, cost %.3f -> %.3f\n", RefineStatusText(r.status),
				r.model.h[2] - tx, r.model.h[5], r.gain, r.offset, r.residualBefore, r.residualAfter);
			Log(msg);
			Assert::IsTrue(r.status == RefineStatus::Refined);
			Assert::IsTrue(std::fabs(r.model.h[2] - tx) <= 0.02 && std::fabs(r.model.h[5]) <= 0.02);
			Assert::IsTrue(std::fabs(r.gain - 0.8) < 0.01);
			Assert::IsTrue(std::fabs(r.offset - 10.0) < 1.5);
			Assert::IsTrue(r.residualAfter < r.residualBefore);
		}

		TEST_METHOD(MoverMaskedByInlierLabels)
		{
			// A separately textured mover (about 15% of the picture) moves (+14, -9) source px while the camera pans 2.5.
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> bg = MakeWaves(31, s, 90.0);
			const std::vector<Wave> fg = MakeWaves(32, s, 100.0);
			const double l = 700, t = 300, rw = 640, rh = 380, mx = 14, my = -9;
			std::vector<Layer> la(1, Shifted(bg, 0, 0)), lb(1, Shifted(bg, tx, 0));
			Layer ma = Shifted(fg, 0, 0);
			ma.clip = true; ma.l = l; ma.t = t; ma.r = l + rw; ma.b = t + rh;
			Layer mb = Shifted(fg, mx, my);
			mb.clip = true; mb.l = l + mx; mb.t = t + my; mb.r = l + rw + mx; mb.b = t + rh + my;
			la.push_back(ma);
			lb.push_back(mb);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, la, 1.0, 0.0, 3, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, lb, 1.0, 0.0, 4, 1.0));

			BlockData blocks;
			blocks.blockSize = 32;
			blocks.gridW = (frameW + 31) / 32;
			blocks.gridH = (frameH + 31) / 32;
			blocks.label.assign(static_cast<size_t>(blocks.gridW) * blocks.gridH, static_cast<uint8_t>(Label::Camera));
			blocks.excluded.assign(blocks.label.size(), 0);
			const double ul = std::min(l, l + mx) - 4, ut = std::min(t, t + my) - 4, ur = std::max(l + rw, l + rw + mx) + 4, ub = std::max(t + rh, t + rh + my) + 4;
			for (int by = 0; by < blocks.gridH; ++by)
				for (int bx = 0; bx < blocks.gridW; ++bx)
				{
					const double x0 = bx * 32.0, y0 = by * 32.0;
					if (x0 + 32 > ul && x0 < ur && y0 + 32 > ut && y0 < ub)
						blocks.label[static_cast<size_t>(by) * blocks.gridW + bx] = static_cast<uint8_t>(Label::Object);
				}
			const RefineResult masked = PhotometricRefine(Translation(tx + 0.8, 0.5), View8(A, B, w, h, s), Pic(frameW, frameH), MaskOf(blocks));
			const RefineResult unmasked = PhotometricRefine(Translation(tx + 0.8, 0.5), View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			char msg[320];
			std::snprintf(msg, sizeof(msg), "mover: masked %s err x %.4f y %.4f (mask %d, %u px); unmasked %s err x %.4f y %.4f\n",
				RefineStatusText(masked.status), masked.model.h[2] - tx, masked.model.h[5], masked.maskUsed ? 1 : 0, masked.pixels,
				RefineStatusText(unmasked.status), unmasked.model.h[2] - tx, unmasked.model.h[5]);
			Log(msg);
			Assert::IsTrue(masked.maskUsed);
			Assert::IsFalse(unmasked.maskUsed);
			Assert::IsTrue(masked.status == RefineStatus::Refined);
			Assert::IsTrue(std::fabs(masked.model.h[2] - tx) <= 0.02 && std::fabs(masked.model.h[5]) <= 0.02);
			// The labels matter (audit SHOULD 6): without the mask the mover pulls the fit further from the camera truth.
			const double errMasked = std::max(std::fabs(masked.model.h[2] - tx), std::fabs(masked.model.h[5]));
			const double errUnmasked = std::max(std::fabs(unmasked.model.h[2] - tx), std::fabs(unmasked.model.h[5]));
			Assert::IsTrue(errUnmasked > errMasked, L"unmasked error not above masked error");
		}

		TEST_METHOD(SimilarityAndAffineKeepType)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0;
			const Rect pic = Pic(frameW, frameH);
			const double diag = std::sqrt(1920.0 * 1920.0 + 1080.0 * 1080.0);
			// Truth: zoom 1.002, rotation 0.001 rad, translation (2.5, -1.0), picture-centred.
			Params truth;
			truth.type = ModelType::Similarity;
			const double zc = 1.002 * std::cos(0.001), zs = 1.002 * std::sin(0.001);
			truth.h[0] = zc; truth.h[1] = -zs; truth.h[2] = 2.5;
			truth.h[3] = zs; truth.h[4] = zc; truth.h[5] = -1.0;
			bool ok = false;
			const Params inv = Invert(truth, &ok);
			Assert::IsTrue(ok);
			const std::vector<Wave> waves = MakeWaves(41, s, 90.0);
			// Frame N shows tex(Winv(y)) in centred coordinates: X = Mi (Y - C) + ci + C.
			const double C[2] = { pic.CentreX(), pic.CentreY() };
			Layer lb;
			lb.waves = &waves;
			lb.m[0] = inv.h[0]; lb.m[1] = inv.h[1]; lb.m[2] = inv.h[3]; lb.m[3] = inv.h[4];
			lb.c[0] = inv.h[2] + C[0] - (inv.h[0] * C[0] + inv.h[1] * C[1]);
			lb.c[1] = inv.h[5] + C[1] - (inv.h[3] * C[0] + inv.h[4] * C[1]);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 5, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, lb), 1.0, 0.0, 6, 1.0));

			Params start = truth;
			start.h[2] += 0.7;
			start.h[5] -= 0.4;
			start.h[0] -= 0.0002;   // 0.2 px at the corners
			start.h[4] -= 0.0002;
			const double before = MaxCornerDifference(start, truth, pic) * diag;
			const RefineResult rs = PhotometricRefine(start, View8(A, B, w, h, s), pic, InlierMask());
			const double afterSim = MaxCornerDifference(rs.model, truth, pic) * diag;

			Params startAff = start;
			startAff.type = ModelType::Affine;
			const RefineResult ra = PhotometricRefine(startAff, View8(A, B, w, h, s), pic, InlierMask());
			const double afterAff = MaxCornerDifference(ra.model, truth, pic) * diag;
			char msg[256];
			std::snprintf(msg, sizeof(msg), "similarity: %s corner err %.4f -> %.4f; affine: %s -> %.4f source px\n", RefineStatusText(rs.status), before, afterSim,
				RefineStatusText(ra.status), afterAff);
			Log(msg);
			Assert::IsTrue(rs.status == RefineStatus::Refined && ra.status == RefineStatus::Refined);
			Assert::IsTrue(rs.model.type == ModelType::Similarity);
			Assert::IsTrue(ra.model.type == ModelType::Affine);
			Assert::IsTrue(afterSim <= 0.05 && afterAff <= 0.05, L"corner error above 0.05 source px");
		}

		TEST_METHOD(FallBackFlatFrameReturnsInputModel)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0;
			const std::vector<Wave> waves = MakeWaves(51, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 7, 1.0));
			const std::vector<uint8_t> flat(static_cast<size_t>(w) * h, 128);
			const Params start = Translation(2.9, -0.3);

			const RefineResult r1 = PhotometricRefine(start, View8(A, flat, w, h, s), Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(r1.status != RefineStatus::Refined);
			Assert::IsTrue(RefineFellBack(r1.status));
			Assert::IsTrue(std::memcmp(r1.model.h, start.h, sizeof(start.h)) == 0);
			Assert::IsTrue(r1.model.type == start.type);
			Assert::AreEqual(1.0, r1.gain);
			Assert::AreEqual(0.0, r1.offset);

			RefineOptions o;
			o.fadeTerms = false;
			o.translationOnly = true;
			o.maxCorrectionLumaPx = 1000.0;
			const RefineResult r2 = PhotometricRefine(start, View8(A, flat, w, h, s), Pic(frameW, frameH), InlierMask(), o);
			char msg[200];
			std::snprintf(msg, sizeof(msg), "flat: %s / %s (cost %.4f -> %.4f)\n", RefineStatusText(r1.status), RefineStatusText(r2.status), r2.residualBefore, r2.residualAfter);
			Log(msg);
			Assert::IsTrue(r2.status == RefineStatus::FellBackNoImprovement);
			Assert::IsTrue(std::memcmp(r2.model.h, start.h, sizeof(start.h)) == 0);
		}

		TEST_METHOD(FallBackTooFarAndCounters)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(61, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 8, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 1.0, 0.0, 9, 1.0));
			const Params start = Translation(tx + 0.8, 0.0);   // 0.4 engine px off

			RefineOptions tight;
			tight.maxCorrectionLumaPx = 0.05;
			PhotometricRefiner refTight(tight);
			const RefineResult tooFar = refTight.Refine(start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(tooFar.status == RefineStatus::FellBackTooFar);
			Assert::IsTrue(std::memcmp(tooFar.model.h, start.h, sizeof(start.h)) == 0);
			Assert::IsTrue(tooFar.correctionPx > 0.05 * s);

			PhotometricRefiner ref;
			const RefineResult good = ref.Refine(start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(good.status == RefineStatus::Refined);
			LumaPairView none;
			const RefineResult noLuma = ref.Refine(start, none, Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(noLuma.status == RefineStatus::SkippedNoLuma);
			Assert::IsTrue(std::memcmp(noLuma.model.h, start.h, sizeof(start.h)) == 0);
			LumaPairView mismatched = View8(A, B, w, h, s);
			mismatched.to.width = w - 1;
			Assert::IsTrue(ref.Refine(start, mismatched, Pic(frameW, frameH), InlierMask()).status == RefineStatus::SkippedNoLuma);
			Params none2;
			Assert::IsTrue(ref.Refine(none2, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask()).status == RefineStatus::SkippedModel);
			const std::vector<uint8_t> flat(static_cast<size_t>(w) * h, 128);
			const RefineResult fb = ref.Refine(start, View8(A, flat, w, h, s), Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(RefineFellBack(fb.status));
			// Picture larger than the planes cover: no luma for it.
			Assert::IsTrue(ref.Refine(start, View8(A, B, w, h, s), Pic(3840, 2160), InlierMask()).status == RefineStatus::SkippedNoLuma);

			const RefineCounters& c = ref.Counters();
			Assert::AreEqual(static_cast<uint64_t>(1), c.refined);
			Assert::AreEqual(static_cast<uint64_t>(1), c.fellBack);
			Assert::AreEqual(static_cast<uint64_t>(3), c.skippedNoLuma);
			Assert::AreEqual(static_cast<uint64_t>(1), c.skippedOther);
			Assert::AreEqual(static_cast<uint64_t>(1), refTight.Counters().fellBack);
		}

		TEST_METHOD(DeterministicByteEqual)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(71, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 10, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0.5)), 0.9, 5.0, 11, 1.0));
			Params start;
			start.type = ModelType::Similarity;
			start.h[2] = tx + 0.6;
			start.h[5] = 0.1;
			PhotometricRefiner reused;
			const RefineResult r1 = reused.Refine(start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			const RefineResult r2 = reused.Refine(start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			const RefineResult r3 = PhotometricRefine(start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask());
			Assert::IsTrue(r1.status == RefineStatus::Refined);
			const RefineResult* rs[2] = { &r2, &r3 };
			for (int k = 0; k < 2; ++k)
			{
				Assert::IsTrue(std::memcmp(r1.model.h, rs[k]->model.h, sizeof(r1.model.h)) == 0);
				Assert::IsTrue(std::memcmp(&r1.gain, &rs[k]->gain, sizeof(double)) == 0);
				Assert::IsTrue(std::memcmp(&r1.offset, &rs[k]->offset, sizeof(double)) == 0);
				Assert::IsTrue(std::memcmp(&r1.residualBefore, &rs[k]->residualBefore, sizeof(double)) == 0);
				Assert::IsTrue(std::memcmp(&r1.residualAfter, &rs[k]->residualAfter, sizeof(double)) == 0);
				Assert::AreEqual(r1.iterations, rs[k]->iterations);
				Assert::AreEqual(r1.pixels, rs[k]->pixels);
			}
		}

		TEST_METHOD(CostLine)
		{
			// ms per call (median of 5) at 1080 half luma (960x540, s 2), 4K quarter luma (960x540, s 4) and 4K R16 half
			// luma (1920x1080, s 2). QUALITY 2 plausibility: ns per unit of work (one selection pixel, or one sampled
			// pixel-iteration counted as 16 taps), must lie in a sane CPU band.
			struct Case { const char* name; int frameW, frameH; double s; bool r16; double stepLimit; };
			const Case cases[3] = { { "1080 half 8-bit", 1920, 1080, 2.0, false, 0.02 }, { "4K quarter 8-bit", 3840, 2160, 4.0, false, 0.04 },
				{ "4K half R16", 3840, 2160, 2.0, true, 0.04 } };
			for (int ci = 0; ci < 3; ++ci)
			{
				const Case& cs = cases[ci];
				const int si = static_cast<int>(cs.s);
				const int w = (cs.frameW + si - 1) / si, h = (cs.frameH + si - 1) / si;
				const double tx = 1.25 * cs.s;
				const std::vector<Wave> waves = MakeWaves(81 + ci, cs.s, 90.0);
				const std::vector<double> Ad = RenderD(w, h, cs.s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 12, 1.0);
				const std::vector<double> Bd = RenderD(w, h, cs.s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 1.0, 0.0, 13, 1.0);
				const std::vector<uint8_t> A8 = To8(Ad), B8 = To8(Bd);
				const std::vector<uint16_t> A16 = To16(Ad), B16 = To16(Bd);
				LumaPairView v;
				if (cs.r16)
				{
					v.from.data16 = A16.data(); v.to.data16 = B16.data();
					v.codeScale = 255.0 / 65535.0;
				}
				else
				{
					v.from.data8 = A8.data(); v.to.data8 = B8.data();
				}
				v.from.width = v.to.width = w;
				v.from.height = v.to.height = h;
				v.from.stride = v.to.stride = w;
				v.sourcePxPerPixel = cs.s;
				PhotometricRefiner ref;
				std::vector<double> ms;
				RefineResult r;
				for (int k = 0; k < 5; ++k)
				{
					const auto t0 = std::chrono::steady_clock::now();
					r = ref.Refine(Translation(tx + 0.4 * cs.s, 0.0), v, Pic(cs.frameW, cs.frameH), InlierMask());
					const auto t1 = std::chrono::steady_clock::now();
					ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				const double med = ms[2];
				const double work = static_cast<double>(w) * h + static_cast<double>(r.pixels) * (r.iterations + 1) * 16.0;
				const double nsPerUnit = med * 1e6 / work;
				char msg[256];
				std::snprintf(msg, sizeof(msg), "cost %s: %.2f ms/call (median of 5; min %.2f max %.2f), %u px, %d it (last step %.5f source px), %s, %.2f ns/unit\n",
					cs.name, med, ms[0], ms[4], r.pixels, r.iterations, r.lastStepPx, RefineStatusText(r.status), nsPerUnit);
				Log(msg);
				Assert::IsTrue(r.status == RefineStatus::Refined);
				Assert::IsTrue(nsPerUnit > 0.05 && nsPerUnit < 100.0, L"cost outside the plausible CPU band");
				CheckConvergence(cs.name, r.iterations, r.lastStepPx, cs.stepLimit);
			}
		}

		TEST_METHOD(RealTextureKnownShift)
		{
			// Audit SHOULD 5: real picture texture with exact motion. One real full-resolution luma frame (8-bit raw, path in
			// VP_R3_REAL_GRAY, size in VP_R3_REAL_W / VP_R3_REAL_H) is cropped at two origins that differ by an integer number of
			// source px and box-downscaled to the engine scale s (2 and, for frames >= 3000 wide, 4), so the engine-scale motion
			// has sub-pixel phase (shift / s) and exact truth; +-1 code independent noise per frame. Real clips have no camera
			// truth finer than about 0.5 px in tools/camera_eval (DIS / NVOF agreement), so this is the finest real-content check.
			// Without the variables the test logs that it did not run and passes (no repository test data).
			const std::string path = EnvText("VP_R3_REAL_GRAY");
			const int fullW = std::atoi(EnvText("VP_R3_REAL_W").c_str()), fullH = std::atoi(EnvText("VP_R3_REAL_H").c_str());
			if (path.empty() || fullW <= 0 || fullH <= 0)
			{
				Log("real texture: VP_R3_REAL_GRAY / _W / _H not set, not run\n");
				return;
			}
			std::vector<uint8_t> full(static_cast<size_t>(fullW) * fullH);
			{
				std::ifstream in(path, std::ios::binary);
				Assert::IsTrue(static_cast<bool>(in), L"real texture file not readable");
				in.read(reinterpret_cast<char*>(full.data()), static_cast<std::streamsize>(full.size()));
				Assert::IsTrue(in.gcount() == static_cast<std::streamsize>(full.size()), L"real texture file shorter than W x H");
			}
			const double limit = fullW >= 3000 ? 0.04 : 0.02;
			const int margin = 16;
			const int shifts[6][2] = { { 3, 1 }, { 5, -2 }, { 7, 3 }, { -2, 5 }, { 1, -3 }, { 6, 2 } };
			for (int si = 0; si < 2; ++si)
			{
				const int s = si == 0 ? 2 : 4;
				if (s == 4 && fullW < 3000)
					continue;
				const int cw = (fullW - 2 * margin) / s * s, ch = (fullH - 2 * margin) / s * s;
				const Rect pic = Pic(cw, ch);
				double meanX = 0, meanY = 0, maxAbs = 0, maxLast = 0, before = 0, after = 0;
				int refined = 0, maxIt = 0;
				for (int k = 0; k < 6; ++k)
				{
					const int sx = shifts[k][0], sy = shifts[k][1];
					// B(x) = A(x - shift): B's crop origin is A's minus the shift.
					const std::vector<uint8_t> A = BoxCrop(full, fullW, margin, margin, cw, ch, s, 900 + 2 * k, 1.0);
					const std::vector<uint8_t> B = BoxCrop(full, fullW, margin - sx, margin - sy, cw, ch, s, 901 + 2 * k, 1.0);
					const RefineResult r = PhotometricRefine(Translation(sx + 0.4 * s, sy - 0.3 * s), View8(A, B, cw / s, ch / s, s), pic, InlierMask());
					const double ex = r.model.h[2] - sx, ey = r.model.h[5] - sy;
					meanX += ex;
					meanY += ey;
					maxAbs = std::max(maxAbs, std::max(std::fabs(ex), std::fabs(ey)));
					maxLast = std::max(maxLast, r.lastStepPx);
					maxIt = std::max(maxIt, r.iterations);
					before += r.residualBefore;
					after += r.residualAfter;
					if (r.status == RefineStatus::Refined)
						++refined;
				}
				meanX /= 6;
				meanY /= 6;
				char msg[320];
				std::snprintf(msg, sizeof(msg), "real texture %dx%d s %d: refined %d/6, bias x %.4f y %.4f, max |err| %.4f source px, mean cost %.2f -> %.2f\n",
					fullW, fullH, s, refined, meanX, meanY, maxAbs, before / 6, after / 6);
				Log(msg);
				Assert::AreEqual(6, refined);
				Assert::IsTrue(std::fabs(meanX) <= limit && std::fabs(meanY) <= limit, L"real-texture bias above the target");
				CheckConvergence("real texture", maxIt, maxLast, limit);
			}
		}
	};
}
