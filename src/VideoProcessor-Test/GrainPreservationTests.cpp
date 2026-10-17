#include "pch.h"
#include "CppUnitTest.h"

#include "GrainReferenceModel.h"

#include <vprenderer/GrainPreserve.h>
#include <vprenderer/InterpolationPolicy.h>
#include <vprenderer/LibplaceboRenderParameters.h>
#include <ConfigFile.h>
#include <RendererProfileConfig.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Film grain (owner 2026-10-17: never averaged or interpolated away; the goal is a
// faithful image with detail as if it were max-bitrate RGB). CPU reference-model
// tests of each fixed stage (GrainReferenceModel.h mirrors the shaders): a panning
// texture at fractional motion with fresh grain on every frame, sigma 0.5 / 1 / 2 / 4
// codes, white and soft. Pass: grain RMS within +-5 % of the source at every output
// frame, and no more frame-to-frame grain sharing than a hold (docs/audit/
// grain-audit-2026-10-17.md). The legacy switches reproduce the old maths.
namespace GrainPreservationTest
{
	using namespace GrainModel;

	constexpr int kW = 160, kH = 128, kMargin = 24;
	constexpr double kSigmas[] = { 0.5, 1.0, 2.0, 4.0 };
	const Motion kPan = { 3.3, 1.7 };   // fractional source pixels per source frame

	// A gentle picture (10-bit codes) whose own high band is small next to the grain:
	// the block gain matches the high band's total energy, so a strongly textured
	// block keeps its texture and the grain in it nearer Catmull-Rom's level (the
	// texture is not over-sharpened to make up for it).
	inline double Texture(double x, double y)
	{
		return 512.0 + 32.0 * std::sin(6.283185307 * x / 211.0) + 32.0 * std::sin(6.283185307 * y / 241.0 + 1.0);
	}

	inline Image TextureAt(double shift)
	{
		Image t(kW, kH);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x)
				t.at(x, y) = Texture(x + 0.5 - shift * kPan.vx, y + 0.5 - shift * kPan.vy);
		return t;
	}

	struct Sequence
	{
		std::vector<Image> frames, grains;
	};

	inline Sequence MakeSequence(int count, double sigma, bool soft, unsigned seed)
	{
		std::mt19937 rng(seed);
		Sequence s;
		for (int k = 0; k < count; ++k)
		{
			s.grains.push_back(Grain(kW, kH, sigma, soft, rng));
			s.frames.push_back(TextureAt(k) + s.grains.back());
		}
		return s;
	}

	// Output n of a source at sourceHz on a display at displayHz: position n * ratio.
	struct Output
	{
		Image grain;      // the output minus the true picture at its position
		double position;  // in source frames
	};

	inline std::vector<Output> Run(const Sequence& s, double ratio, int outputs, bool preserve)
	{
		std::vector<Output> out;
		for (int n = 0; n < outputs; ++n)
		{
			const double x = n * ratio;
			const int m = static_cast<int>(std::floor(x + 1e-9));
			const double t = x - m;
			Image o = t < 1e-6 ? s.frames[m] :
				(preserve ? InterpolatePreserve(s.frames[m], s.frames[m + 1], t, kPan) : InterpolateBlend(s.frames[m], s.frames[m + 1], t, kPan));
			out.push_back({ o - TextureAt(x), x });
		}
		return out;
	}

	// Lag-1 correlation of consecutive outputs' grain, the earlier one carried to the
	// later one's position (Catmull-Rom along the pan).
	inline std::vector<double> Lag1(const std::vector<Output>& outputs)
	{
		std::vector<double> c;
		for (size_t n = 0; n + 1 < outputs.size(); ++n)
		{
			const double d = outputs[n + 1].position - outputs[n].position;
			c.push_back(Corr(Warp(outputs[n].grain, d * kPan.vx, d * kPan.vy, true), outputs[n + 1].grain, kMargin));
		}
		return c;
	}

	// A hold at the same cadence: consecutive outputs showing the same source frame.
	inline int HoldRepeats(double ratio, int outputs)
	{
		int repeats = 0;
		for (int n = 0; n + 1 < outputs; ++n)
			repeats += static_cast<int>(std::floor(n * ratio + 1e-9)) == static_cast<int>(std::floor((n + 1) * ratio + 1e-9));
		return repeats;
	}

	inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	inline Image Blur(const Image& img, double sigma)
	{
		const int r = static_cast<int>(std::ceil(3.0 * sigma));
		std::vector<double> k(2 * r + 1);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
		Image t(img.w, img.h), o(img.w, img.h);
		for (int y = 0; y < img.h; ++y)
			for (int x = 0; x < img.w; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += k[i + r] / sum * img.get(x + i, y);
				t.at(x, y) = acc;
			}
		for (int y = 0; y < img.h; ++y)
			for (int x = 0; x < img.w; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += k[i + r] / sum * t.get(x, y + i);
				o.at(x, y) = acc;
			}
		return o;
	}

	TEST_CLASS(GrainPreservationTests)
	{
	public:
		// interpolation_grain preserve: every synthesised frame of a 50p-on-60 Hz and a
		// 24p-on-60 Hz cadence carries the source's grain RMS within +-5 %; blend (the
		// legacy path) carries about half at mid phases (the audit's finding).
		TEST_METHOD(InterpolatorPreserveKeepsGrainRmsAtEveryPhase)
		{
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
				{
					const Sequence s = MakeSequence(8, sigma, soft, 101u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
					for (double ratio : { 50.0 / 60.0, 24.0 / 60.0 })
					{
						const int outputs = ratio > 0.5 ? 7 : 11;
						const std::vector<Output> preserve = Run(s, ratio, outputs, true);
						const std::vector<Output> blend = Run(s, ratio, outputs, false);
						double worstBlend = 1.0;
						for (int n = 0; n < outputs; ++n)
						{
							const int m = static_cast<int>(std::floor(preserve[n].position + 1e-9));
							const double source = Rms(s.grains[m], kMargin);
							const double r = Rms(preserve[n].grain, kMargin) / source;
							char text[200];
							snprintf(text, sizeof(text), "%s sigma %.1f ratio %.3f output %d (t %.3f): preserve %.3f blend %.3f",
								soft ? "soft" : "white", sigma, ratio, n, preserve[n].position - m, r,
								Rms(blend[n].grain, kMargin) / source);
							Logger::WriteMessage(text);
							Assert::IsTrue(r > 0.95 && r < 1.05, W(text).c_str());
							worstBlend = (std::min)(worstBlend, Rms(blend[n].grain, kMargin) / source);
						}
						Assert::IsTrue(worstBlend < 0.75, L"the legacy blend averages grain away (audit: about 0.5x)");
					}
				}
		}

		// No more frame-to-frame grain sharing than a hold: per 50p-on-60 Hz cycle at most as
		// many near-duplicate pairs (correlation > 0.5) as the hold has repeats, and every
		// other pair below 0.10 (the legacy blend smears grain over consecutive outputs: up
		// to 0.55 on several pairs).
		TEST_METHOD(InterpolatorPreserveGrainNotSmearedAcrossFrames)
		{
			const double ratio = 50.0 / 60.0;
			const int outputs = 7;
			const int holdRepeats = HoldRepeats(ratio, outputs);
			Assert::AreEqual(1, holdRepeats, L"one repeat per 5:6 cycle");
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
				{
					const Sequence s = MakeSequence(8, sigma, soft, 202u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
					const std::vector<double> preserve = Lag1(Run(s, ratio, outputs, true));
					const std::vector<double> blend = Lag1(Run(s, ratio, outputs, false));
					int shared = 0, blendSmeared = 0;
					std::string text = std::string(soft ? "soft" : "white") + " sigma " + std::to_string(sigma) + " lag-1:";
					for (size_t i = 0; i < preserve.size(); ++i)
					{
						char v[64];
						snprintf(v, sizeof(v), " %.2f/%.2f", preserve[i], blend[i]);
						text += v;
						if (preserve[i] > 0.5) ++shared;
						else Assert::IsTrue(preserve[i] < 0.10, W(text).c_str());
						blendSmeared += blend[i] > 0.3 ? 1 : 0;
					}
					Logger::WriteMessage(text.c_str());
					Assert::IsTrue(shared <= holdRepeats, W(text).c_str());
					Assert::IsTrue(blendSmeared >= 2, L"the legacy blend shares grain over several pairs");
				}
		}

		// Where the occlusion forces the A sample alone (wb 0) the grain keeps the same
		// strength as next to it (the legacy blend: one frame's bilinear sample beside a
		// blend of two - patchy).
		TEST_METHOD(InterpolatorPreserveOcclusionKeepsGrainUniform)
		{
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
				{
					const Sequence s = MakeSequence(2, sigma, soft, 303u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
					const std::function<bool(double, double)> leftOccluded = [](double lbx, double) { return lbx < kW / 2.0; };
					for (double t : { 0.3, 0.5, 0.75 })
					{
						const Image g = InterpolatePreserve(s.frames[0], s.frames[1], t, kPan, leftOccluded) - TextureAt(t);
						// Each half less 16 px at the frame edges and 8 px at the occlusion edge.
						const int half = kW / 2 - 24;
						Image left(half, kH - 32), right(half, kH - 32);
						for (int y = 0; y < kH - 32; ++y)
							for (int x = 0; x < half; ++x)
							{
								left.at(x, y) = g.at(x + 16, y + 16);
								right.at(x, y) = g.at(x + kW / 2 + 8, y + 16);
							}
						const double source = Rms(s.grains[0], kMargin);
						const double l = Rms(left, 0) / source, r = Rms(right, 0) / source;
						char text[160];
						snprintf(text, sizeof(text), "%s sigma %.1f t %.2f: occluded half %.3f, other half %.3f", soft ? "soft" : "white", sigma, t, l, r);
						Logger::WriteMessage(text);
						Assert::IsTrue(l > 0.95 && l < 1.05 && r > 0.95 && r < 1.05, W(text).c_str());
					}
				}
		}

		// A whole-pixel position with the same content in A and B (B = A moved by the
		// vector) gives A back exactly: the energy target and the high band are the same
		// numbers, the gain exactly 1.
		TEST_METHOD(InterpolatorPreserveIsExactAtWholePixelPositions)
		{
			std::mt19937 rng(404u);
			const Image grain = Grain(kW + 64, kH + 64, 2.0, false, rng);
			const Motion v = { 16.0, 8.0 };
			Image a(kW, kH), b(kW, kH);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x)
				{
					a.at(x, y) = Texture(x + 0.5, y + 0.5) + grain.at(x + 32, y + 32);
					b.at(x, y) = Texture(x + 0.5 - v.vx, y + 0.5 - v.vy) + grain.at(x + 32 - 16, y + 32 - 8);
				}
			for (double t : { 0.25, 0.5, 0.75 })
			{
				Image gain;
				const Image o = InterpolatePreserve(a, b, t, v, nullptr, &gain);
				const int sx = static_cast<int>(v.vx * t), sy = static_cast<int>(v.vy * t);
				// Blocks clear of the frame edges by the low band's radius (there A's and B's
				// clamped low bands differ, and the block's gain with them).
				double worst = 0.0;
				for (int y = 32; y < kH - 32; ++y)
					for (int x = 32; x < kW - 48; ++x)
						worst = (std::max)(worst, std::fabs(o.at(x, y) - a.get(x - sx, y - sy)));
				Assert::IsTrue(worst < 1e-6, (L"exact at t = " + std::to_wstring(t) + L": " + std::to_wstring(worst)).c_str());
			}
		}

		// interpolation_grain blend is the established maths: (1 - t) A(p - t v) + t B(p + (1 - t) v),
		// bilinear; on fresh white grain at a whole-pixel shift sqrt((1 - t)^2 + t^2) of the grain.
		TEST_METHOD(InterpolatorBlendIsTheLegacyMaths)
		{
			std::mt19937 rng(505u);
			const Image a = Grain(kW, kH, 1.0, false, rng), b = Grain(kW, kH, 1.0, false, rng);
			for (double t : { 0.25, 0.5 })
			{
				const Motion v = { 4.0 / t, 0.0 };   // whole-pixel shifts at this t
				const Image o = InterpolateBlend(a, b, t, v);
				for (int y = 30; y < 40; ++y)
					for (int x = 40; x < 60; ++x)
					{
						const double expected = (1.0 - t) * a.get(x - 4, y) + t * b.get(x + static_cast<int>(std::lround((1.0 - t) * v.vx)), y);
						Assert::AreEqual(expected, o.at(x, y), 1e-9);
					}
				const double r = Rms(o, kMargin);
				Assert::AreEqual(std::sqrt((1.0 - t) * (1.0 - t) + t * t), r, 0.03);
			}
			Assert::IsTrue(InterpolationPolicy::ParseGrain("blend") == InterpolationPolicy::Grain::Blend);
			Assert::IsTrue(InterpolationPolicy::ParseGrain("preserve") == InterpolationPolicy::Grain::Preserve);
			Assert::IsTrue(InterpolationPolicy::ParseGrain("") == InterpolationPolicy::Grain::Preserve, L"the faithful default");
			Assert::AreEqual("preserve", InterpolationPolicy::Name(InterpolationPolicy::Grain::Preserve));
		}

		// dejudder_source_warp_filter catmull: more of the grain than the hardware's bilinear at
		// every fractional shift; soft (film-like) grain >= 0.92x, white >= 0.68x (Catmull-Rom's
		// own response at Nyquist; the +-5 % target needs the interpolator's energy-matched high
		// band - not part of this switch). bilinear is the established maths. A warp of one
		// frame shares that frame's grain with its neighbours exactly as a hold does.
		TEST_METHOD(SourceWarpCatmullKeepsMoreGrainThanBilinear)
		{
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
				{
					std::mt19937 rng(606u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
					const Image g = Grain(kW, kH, sigma, soft, rng);
					const double source = Rms(g, kMargin);
					for (double shift : { 0.25, 0.5, 0.75, 1.3 })
					{
						const double c = Rms(Warp(g, shift, 0.6 * shift, true), kMargin) / source;
						const double b = Rms(Warp(g, shift, 0.6 * shift, false), kMargin) / source;
						char text[160];
						snprintf(text, sizeof(text), "%s sigma %.1f shift %.2f: catmull %.3f bilinear %.3f", soft ? "soft" : "white", sigma, shift, c, b);
						Logger::WriteMessage(text);
						Assert::IsTrue(c > b + 0.05, W(text).c_str());
						Assert::IsTrue(c >= (soft ? 0.92 : 0.68) && c < 1.05, W(text).c_str());
					}
					// Whole pixels: both filters give the source back.
					const Image whole = Warp(g, 2.0, 1.0, true), wholeB = Warp(g, 2.0, 1.0, false);
					for (int y = 10; y < 20; ++y)
						for (int x = 10; x < 20; ++x)
						{
							Assert::AreEqual(g.at(x - 2, y - 1), whole.at(x, y), 1e-9);
							Assert::AreEqual(g.at(x - 2, y - 1), wholeB.at(x, y), 1e-9);
						}
				}
		}

		// residual_stabilise_grain_guard: blocks the grain term marks as grain pass this
		// frame's residual through - the output's grain is the source's plus its own CNN /
		// SSIM boost, every frame, no frame-to-frame sharing; the legacy one-pole (alpha
		// 1/3, bilinear previous residual) keeps 0.83-0.90x and correlates frames.
		TEST_METHOD(ResidualStabiliseGuardLeavesGrainBlocks)
		{
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
					for (double share : { 0.2, 0.4 })
					{
						std::mt19937 rng(707u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
						Image prevLegacy, prevGuard, lastLegacyOut, lastGuardOut, lastOff;
						double offSum = 0.0, legacySum = 0.0, guardSum = 0.0, legacyCorr = 0.0, guardCorr = 0.0;
						int counted = 0;
						for (int f = 0; f < 16; ++f)
						{
							const Image g = Grain(kW, kH, sigma, soft, rng);
							Image r = g - Blur(g, 1.0);
							for (double& x : r.v) x *= share;   // the residual: this frame's own high-band boost
							const Motion v = kPan;
							const Image legacy = f == 0 ? r : StabiliseLegacy(r, prevLegacy, v);
							const Image guard = f == 0 ? r : StabiliseGuard(r, prevGuard, v, 0.4, 0.4, 0.0);   // g at the grain floor
							prevLegacy = legacy;
							prevGuard = guard;
							const Image off = g + r, outLegacy = g + legacy, outGuard = g + guard;
							if (f >= 6)
							{
								offSum += Rms(off, kMargin);
								legacySum += Rms(outLegacy, kMargin);
								guardSum += Rms(outGuard, kMargin);
								legacyCorr += Corr(Warp(lastLegacyOut, v.vx, v.vy, true), outLegacy, kMargin);
								guardCorr += Corr(Warp(lastGuardOut, v.vx, v.vy, true), outGuard, kMargin);
								++counted;
							}
							lastLegacyOut = outLegacy;
							lastGuardOut = outGuard;
						}
						const double legacy = legacySum / offSum, guard = guardSum / offSum;
						char text[200];
						snprintf(text, sizeof(text), "%s sigma %.1f share %.1f: legacy %.3f (lag-1 %.2f), guard %.3f (lag-1 %.2f)",
							soft ? "soft" : "white", sigma, share, legacy, legacyCorr / counted, guard, guardCorr / counted);
						Logger::WriteMessage(text);
						Assert::IsTrue(guard > 0.995 && guard < 1.005, W(text).c_str());
						Assert::IsTrue(guardCorr / counted < 0.05, W(text).c_str());
						if (!soft)
							Assert::IsTrue(legacy < 0.97, L"the legacy hold averages part of the grain (audit: 0.83-0.90x)");
					}
		}

		// The guard's other two parts: a clean block (grain term 1) is still held, with the
		// held change at most the coding-noise limit; the previous residual is read by
		// texel, not bilinear; and the legacy formula is mix(bilinear prev, r, 1/3).
		TEST_METHOD(ResidualStabiliseGuardClampsToTheCodingNoise)
		{
			std::mt19937 rng(808u);
			const Image prev = Grain(kW, kH, 3.0, false, rng), r = Grain(kW, kH, 3.0, false, rng);
			const Motion v = { 2.4, 1.6 };
			const double limit = GrainPreserve::CodingNoiseCodes(0.5);
			const Image guard = StabiliseGuard(r, prev, v, 1.0, 0.4, limit);
			const Image unclamped = StabiliseGuard(r, prev, v, 1.0, 0.4, 0.0);
			const Image legacy = StabiliseLegacy(r, prev, v);
			int clamped = 0;
			for (int y = 8; y < kH - 8; ++y)
				for (int x = 8; x < kW - 8; ++x)
				{
					Assert::IsTrue(std::fabs(guard.at(x, y) - r.at(x, y)) <= limit + 1e-12);
					clamped += std::fabs(unclamped.at(x, y) - r.at(x, y)) > limit;
					const double rp = prev.get(static_cast<int>(std::floor(x + 0.5 - v.vx)), static_cast<int>(std::floor(y + 0.5 - v.vy)));
					Assert::AreEqual(rp + (r.at(x, y) - rp) / 3.0, unclamped.at(x, y), 1e-9, L"texel fetch, alpha 1/3");
					const double rb = Bilinear(prev, x + 0.5 - v.vx, y + 0.5 - v.vy);
					Assert::AreEqual(rb + (r.at(x, y) - rb) / 3.0, legacy.at(x, y), 1e-9, L"the legacy one-pole");
				}
			Assert::IsTrue(clamped > 100, L"the limit binds on large held changes");
			Assert::AreEqual(0.5, GrainPreserve::CodingNoiseCodes(1.0), 1e-12);
			Assert::AreEqual(3.0, GrainPreserve::CodingNoiseCodes(0.0), 1e-12);
			Assert::AreEqual(3.0, GrainPreserve::CodingNoiseCodes(-1.0), 1e-12);
		}

		// deband_vp_grain_guard: VpDebander without vectors keeps grain of 0.5 code and more
		// at the default threshold (1.5 codes) and at the low-bitrate maximum (3 codes), while
		// a banded gradient is debanded exactly as before; off is the established debander
		// (0.5-0.6x of 0.5-code grain).
		TEST_METHOD(VpDebanderGuardKeepsGrainWithoutVectors)
		{
			for (bool soft : { false, true })
				for (double sigma : kSigmas)
					for (double thr : { 1.5, 3.0 })
					{
						std::mt19937 rng(909u + static_cast<unsigned>(sigma * 8) + (soft ? 1000u : 0u));
						const Image g = Grain(kW, kH, sigma, soft, rng);
						Image img(kW, kH);
						for (int y = 0; y < kH; ++y)
							for (int x = 0; x < kW; ++x)
								img.at(x, y) = std::round(100.0 + 0.07 * x + 0.03 * y + g.at(x, y));   // 8-bit content
						Image ramp(kW, kH);
						for (int y = 0; y < kH; ++y)
							for (int x = 0; x < kW; ++x)
								ramp.at(x, y) = 100.0 + 0.07 * x + 0.03 * y;
						const double source = Rms(img - ramp, kMargin);
						const double lapMin = GrainPreserve::GuardLaplacianCodes(thr, 8);
						const double guard = Rms(Deband(img, thr, 16.0, lapMin) - ramp, kMargin) / source;
						const double legacy = Rms(Deband(img, thr, 16.0, 0.0) - ramp, kMargin) / source;
						char text[160];
						snprintf(text, sizeof(text), "%s sigma %.1f threshold %.1f: guard %.3f legacy %.3f", soft ? "soft" : "white", sigma, thr, guard, legacy);
						Logger::WriteMessage(text);
						Assert::IsTrue(guard > 0.95 && guard < 1.05, W(text).c_str());
						if (sigma == 0.5) Assert::IsTrue(legacy < 0.75, W(text).c_str());
					}
			// Bands: a quantised gentle ramp loses the same error with and without the guard.
			for (double slope : { 1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0 })
			{
				Image ramp(kW, kH), banded(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
					{
						ramp.at(x, y) = 100.0 + slope * x;
						banded.at(x, y) = std::round(ramp.at(x, y));
					}
				const double before = Rms(banded - ramp, kMargin);
				const double legacy = Rms(Deband(banded, 1.5, 16.0, 0.0) - ramp, kMargin);
				const double guard = Rms(Deband(banded, 1.5, 16.0, GrainPreserve::GuardLaplacianCodes(1.5, 8)) - ramp, kMargin);
				char text[160];
				snprintf(text, sizeof(text), "band slope %.4f: error %.3f -> legacy %.3f, guard %.3f", slope, before, legacy, guard);
				Logger::WriteMessage(text);
				Assert::IsTrue(legacy < 0.75 * before, W(text).c_str());
				Assert::AreEqual(legacy, guard, 0.01 * before, W(text).c_str());
			}
			Assert::AreEqual(0.5, GrainPreserve::GuardLaplacianCodes(1.5, 8), 1e-12);
			Assert::AreEqual(0.5, GrainPreserve::GuardLaplacianCodes(3.0, 8), 1e-12, L"the bitrate allowance does not raise it");
			Assert::AreEqual(2.0, GrainPreserve::GuardLaplacianCodes(6.0, 10), 1e-12, L"10-bit codes");
		}

		// deband_grain off: libplacebo's deband adds no grain; threshold, radius and
		// iterations stay as selected for every strength; on restores its parameters.
		TEST_METHOD(LibplaceboDebandGrainOffZeroesOnlyTheGrain)
		{
			using namespace LibplaceboRenderParameters;
			for (const char* strength : { "auto", "default", "light" })
				for (Toggle toggle : { Toggle::Auto, Toggle::On })
				{
					Settings on;
					on.debandStrength = strength;
					on.deband = toggle;
					on.debandGrain = true;
					Settings off = on;
					off.debandGrain = false;
					Projection pOn, pOff;
					std::string error;
					Assert::IsTrue(Build(on, false, pOn, error), W(error).c_str());
					Assert::IsTrue(Build(off, false, pOff, error), W(error).c_str());
					Assert::AreEqual(pOn.renderParams.deband_params == nullptr, pOff.renderParams.deband_params == nullptr);
					if (!pOff.renderParams.deband_params)
						continue;
					Assert::AreEqual(0.0f, pOff.renderParams.deband_params->grain);
					Assert::IsTrue(pOn.renderParams.deband_params->grain > 0.0f, L"on: libplacebo's grain");
					Assert::AreEqual(pOn.renderParams.deband_params->threshold, pOff.renderParams.deband_params->threshold);
					Assert::AreEqual(pOn.renderParams.deband_params->radius, pOff.renderParams.deband_params->radius);
					Assert::AreEqual(pOn.renderParams.deband_params->iterations, pOff.renderParams.deband_params->iterations);
				}
			Settings defaults;
			Assert::IsFalse(defaults.debandGrain, L"the faithful default: no added grain");
			Settings none;
			none.debandStrength = "off";
			Projection p;
			std::string error;
			Assert::IsTrue(Build(none, false, p, error));
			Assert::IsTrue(p.renderParams.deband_params == nullptr, L"debanding off stays off");
		}

		// The new profile keys validate with exactly their values.
		TEST_METHOD(GrainKeysValidate)
		{
			std::string expected;
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_grain", "preserve", expected));
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_grain", "blend", expected));
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_grain", "hold", expected));
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_source_warp_filter", "catmull", expected));
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_source_warp_filter", "bilinear", expected));
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_source_warp_filter", "lanczos", expected));
			for (const char* key : { "residual_stabilise_grain_guard", "deband_vp_grain_guard" })
			{
				Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key, "on", expected));
				Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key, "off", expected));
				Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, "auto", expected));
			}
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("scaling", "deband_grain", "on", expected));
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("scaling", "deband_grain", "off", expected));
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("scaling", "deband_grain", "light", expected));
		}
	};
}
