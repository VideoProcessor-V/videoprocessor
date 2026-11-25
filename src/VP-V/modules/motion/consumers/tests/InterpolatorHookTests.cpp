// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>
#include <fstream>
#include <sstream>

namespace VideoProcessorTest
{
	TEST_CLASS(InterpolatorHookTests)
	{
	public:


		TEST_METHOD(InterpolatorKeepsGrainRmsAtEveryPhase)
		{
			// interpolation_grain preserve: the luma and chroma grain RMS of every synthesised
			// phase within +-5 % of the source's (white and soft, 0.5 to 4 codes) on a fractional
			// pan; the luma matches the CPU reference model; blend (the legacy) carries about half;
			// blend after preserve is the never-switched object's output byte for byte.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, margin = 24;
			const double vx = 3.3, vy = 1.7;
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			for (bool soft : { false, true })
				for (double sigma : { 0.5, 1.0, 2.0, 4.0 })
				{
					const GrainFrames gf = MakeGrainFrames(width, height, 2, vx, vy, sigma, soft, 31u + static_cast<unsigned>(sigma * 4) + (soft ? 100u : 0u));
					DejudderGpuPlanes planesA, planesB;
					UploadDejudderFrame(gpu, gf.frames[0], planesA);
					UploadDejudderFrame(gpu, gf.frames[1], planesB);
					Interpolator::FrameInterpolator legacy(fixture.Log(), gpu, true), preserve(fixture.Log(), gpu, true);
					legacy.SetGrain(InterpolationPolicy::Grain::Blend);   // the object default is Preserve since the integration
					preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
					const double sourceLuma = GrainModel::Rms(gf.lumaGrain[0], margin);
					const double sourceChroma = GrainModel::Rms(ChromaGrainOf(gf.frames[0].chroma, width, height, 0), margin / 2);
					for (float t : { 0.2f, 0.4f, 0.5f, 0.6f, 0.8f })
					{
						Dejudder::Planes out;
						Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, out), InterpolatorError(preserve, log).c_str());
						const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
						const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
						const double rl = GrainModel::Rms(LumaGrainOf(luma, width, height, t, vx, vy), margin) / sourceLuma;
						const double rc = GrainModel::Rms(ChromaGrainOf(chroma, width, height, 0), margin / 2) / sourceChroma;
						// The CPU reference model on the same frames (16-bit codes).
						GrainModel::Image a(width, height), b(width, height);
						for (size_t i = 0; i < a.v.size(); ++i) { a.v[i] = gf.frames[0].luma[i]; b.v[i] = gf.frames[1].luma[i]; }
						const GrainModel::Image model = GrainModel::InterpolatePreserve(a, b, t, { vx, vy });
						double worst = 0.0;
						for (int y = margin; y < height - margin; ++y)
							for (int x = margin; x < width - margin; ++x)
								worst = (std::max)(worst, std::fabs(luma[static_cast<size_t>(y) * width + x] - model.at(x, y)));
						// Blend (legacy) on the never-switched object.
						Dejudder::Planes old;
						Assert::IsTrue(legacy.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, old), InterpolatorError(legacy, log).c_str());
						const std::vector<uint16_t> oldLuma = DownloadPlane(gpu, old.luma);
						const std::vector<uint16_t> oldChroma = DownloadPlane(gpu, old.chroma);
						const double rb = GrainModel::Rms(LumaGrainOf(oldLuma, width, height, t, vx, vy), margin) / sourceLuma;
						char text[240];
						snprintf(text, sizeof(text), "%s sigma %.1f t %.1f: preserve luma %.3f chroma %.3f (model max diff %.1f 16-bit codes); blend luma %.3f",
							soft ? "soft" : "white", sigma, t, rl, rc, worst, rb);
						Logger::WriteMessage(text);
						const std::wstring wtext(text, text + strlen(text));
						Assert::IsTrue(rl > 0.95 && rl < 1.05, wtext.c_str());
						Assert::IsTrue(rc > 0.95 && rc < 1.05, wtext.c_str());
						Assert::IsTrue(worst <= 96.0, (L"the shader is the reference model: " + wtext).c_str());
						if (t == 0.5f) Assert::IsTrue(rb < 0.8, (L"blend averages grain away: " + wtext).c_str());
						// The legacy switch restored on the switched object: byte for byte.
						preserve.SetGrain(InterpolationPolicy::Grain::Blend);
						Dejudder::Planes back;
						Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, back), InterpolatorError(preserve, log).c_str());
						Assert::IsTrue(DownloadPlane(gpu, back.luma) == oldLuma, L"blend luma byte for byte");
						Assert::IsTrue(DownloadPlane(gpu, back.chroma) == oldChroma, L"blend chroma byte for byte");
						preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
					}
					Assert::AreEqual(5ull, preserve.GrainPreservedRuns());
					Assert::AreEqual(0ull, legacy.GrainPreservedRuns());
				}
			// The fallback blend (untrusted vectors) splits the same way: the grain is kept.
			{
				const GrainFrames gf = MakeGrainFrames(width, height, 2, 0.0, 0.0, 2.0, false, 77u);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, gf.frames[0], planesA);
				UploadDejudderFrame(gpu, gf.frames[1], planesB);
				Interpolator::FrameInterpolator preserve(fixture.Log(), gpu, true);
				preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
				Dejudder::Planes out;
				Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, Interpolator::Motion(), 0.5f,
					InterpolationPolicy::Fallback::Blend, out), InterpolatorError(preserve, log).c_str());
				const double r = GrainModel::Rms(LumaGrainOf(DownloadPlane(gpu, out.luma), width, height, 0.0, 0.0, 0.0), margin) /
					GrainModel::Rms(gf.lumaGrain[0], margin);
				Assert::IsTrue(r > 0.95 && r < 1.05, (L"fallback blend keeps the grain: " + std::to_wstring(r)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorGrainNotSmearedAcrossFrames)
		{
			// 50p on 60 Hz through the shader: per cycle at most as many near-duplicate output
			// pairs (lag-1 correlation along the pan > 0.5) as a hold has repeats (one), every other
			// pair below 0.10; blend (the legacy) shares grain over several pairs.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, margin = 24;
			const double vx = 3.3, vy = 1.7, ratio = 50.0 / 60.0;
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			for (bool soft : { false, true })
			{
				const GrainFrames gf = MakeGrainFrames(width, height, 8, vx, vy, 2.0, soft, 41u + (soft ? 100u : 0u));
				std::vector<std::unique_ptr<DejudderGpuPlanes>> planes;
				for (const DejudderFrame& f : gf.frames)
				{
					planes.push_back(std::make_unique<DejudderGpuPlanes>());
					UploadDejudderFrame(gpu, f, *planes.back());
				}
				for (bool preserveMode : { true, false })
				{
					Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
					interpolator.SetGrain(preserveMode ? InterpolationPolicy::Grain::Preserve : InterpolationPolicy::Grain::Blend);
					std::vector<GrainModel::Image> grains;
					std::vector<double> positions;
					for (int n = 0; n < 7; ++n)
					{
						const double x = n * ratio;
						const int m = static_cast<int>(std::floor(x + 1e-9));
						Dejudder::Planes out;
						Assert::IsTrue(interpolator.Run(planes[m]->planes, planes[m + 1]->planes, InterpolatorMotion(field),
							static_cast<float>(x - m), InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
						grains.push_back(LumaGrainOf(DownloadPlane(gpu, out.luma), width, height, x, vx, vy));
						positions.push_back(x);
					}
					int shared = 0, smeared = 0;
					std::string text = std::string(soft ? "soft" : "white") + (preserveMode ? " preserve" : " blend") + " lag-1:";
					for (int n = 0; n + 1 < 7; ++n)
					{
						const double d = positions[n + 1] - positions[n];
						const double c = GrainModel::Corr(GrainModel::Warp(grains[n], d * vx, d * vy, true), grains[n + 1], margin);
						char v[32];
						snprintf(v, sizeof(v), " %.2f", c);
						text += v;
						if (c > 0.5) ++shared;
						else if (c >= 0.10) ++smeared;
					}
					Logger::WriteMessage(text.c_str());
					const std::wstring wtext(text.begin(), text.end());
					if (preserveMode)
						Assert::IsTrue(shared <= 1 && smeared == 0, wtext.c_str());
					else
						Assert::IsTrue(smeared >= 2, wtext.c_str());
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EntranceOnceInterpolatorIsExactOnStillOrInvalidVectors)
		{
			// B3 (ADR-0024 criterion 1) at its own boundary: the interpolator reading PROCESSED planes
			// (another size than the estimate: plane_scale 1.5, and 2) returns the processed A
			// byte for byte when A and B are the same picture with zero vectors, and when there are
			// no valid vectors (the hold), at every phase, in both grain modes.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 96, height = 64;
			const DejudderFrame frame = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return static_cast<uint16_t>(20000 + 7 * DejudderTexture(x + 1000 * c, y) % 20000); });
			DejudderGpuPlanes a, b;
			UploadDejudderFrame(gpu, frame, a);
			UploadDejudderFrame(gpu, frame, b);   // the same picture in other textures (B processed ahead)
			const std::vector<uint16_t> lumaA = DownloadPlane(gpu, a.planes.luma), chromaA = DownloadPlane(gpu, a.planes.chroma);
			for (double scale : { 1.5, 2.0 })
			{
				const int ew = static_cast<int>(width * scale), eh = static_cast<int>(height * scale);
				InterpolatorField still;
				MakeInterpolatorField(gpu, ew, eh, 1.0f, [](int, int) { return std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 0.0f }; }, still);
				for (bool preserve : { true, false })
					for (bool vectors : { true, false })
					{
						Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
						interpolator.SetGrain(preserve ? InterpolationPolicy::Grain::Preserve : InterpolationPolicy::Grain::Blend);
						for (float t : { 0.25f, 0.5f, 0.75f })
						{
							Dejudder::Planes out;
							const Interpolator::Motion motion = vectors ? InterpolatorMotion(still) : Interpolator::Motion();
							Assert::IsTrue(interpolator.Run(a.planes, b.planes, motion, t, InterpolationPolicy::Fallback::Hold, out, ew, eh),
								InterpolatorError(interpolator, log).c_str());
							char name[128];
							snprintf(name, sizeof(name), "scale %.1f %s %s t=%.2f", scale, preserve ? "preserve" : "blend",
								vectors ? "zero vectors" : "no vectors", t);
							const std::wstring wname(name, name + strlen(name));
							Assert::IsTrue(DownloadPlane(gpu, out.luma) == lumaA, (wname + L": luma is the processed A").c_str());
							Assert::IsTrue(DownloadPlane(gpu, out.chroma) == chromaA, (wname + L": chroma is the processed A").c_str());
						}
					}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorT0IsAExactlyT1IsB)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 16, y); },
				[](int x, int y, int c) { return DejudderTexture((x - 8) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ 16.0f, 0.0f, 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			Dejudder::Planes out;
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
			{
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.0f, fallback, out));
				Assert::IsTrue(out.luma == planesA.planes.luma && out.chroma == planesA.planes.chroma, L"t = 0 is A itself");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 1.0f, fallback, out));
				Assert::IsTrue(out.luma == planesB.planes.luma && out.chroma == planesB.planes.chroma, L"t = 1 is B itself");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), -0.5f, fallback, out));
				Assert::IsTrue(out.luma == planesA.planes.luma, L"before A: A");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 1.5f, fallback, out));
				Assert::IsTrue(out.luma == planesB.planes.luma, L"after B: B");
			}
			Assert::AreEqual(0ull, interpolator.Runs(), L"nothing dispatched at the ends");
			// In between: new planes in the input formats and sizes.
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			Assert::IsTrue(out.luma != planesA.planes.luma && out.luma != planesB.planes.luma);
			Assert::IsTrue(out.luma->params.format == planesA.planes.luma->params.format &&
				out.chroma->params.format == planesA.planes.chroma->params.format, L"the input plane formats");
			Assert::AreEqual(width, out.luma->params.w);
			Assert::AreEqual(width / 2, out.chroma->params.w);
			// The dispatched path is exact too: with no motion between identical frames
			// any phase gives the frame back code for code.
			InterpolatorField still;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 0.0f }; }, still);
			for (float t : { 1e-4f, 0.25f, 0.5f, 0.9999f })
			{
				Assert::IsTrue(interpolator.Run(planesA.planes, planesA.planes, InterpolatorMotion(still), t,
					InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
				Assert::IsTrue(DownloadPlane(gpu, out.luma) == a.luma, L"luma exact");
				Assert::IsTrue(DownloadPlane(gpu, out.chroma) == a.chroma, L"chroma exact");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorPanLandsAtTheTruePhase)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			static constexpr int dx = 16, dy = 8;   // per source frame: whole pixels at t = 0.25 / 0.5 / 0.75
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ static_cast<float>(dx), static_cast<float>(dy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			for (int quarter = 1; quarter <= 3; ++quarter)
			{
				const float t = 0.25f * quarter;
				const int sx = dx * quarter / 4, sy = dy * quarter / 4;
				Dejudder::Planes out;
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
					InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
				int checked = 0, exact = 0;
				for (int y = dy + 2; y < height - dy - 2; ++y)
					for (int x = dx + 2; x < width - dx - 2; ++x)
					{
						++checked;
						exact += std::abs(static_cast<int>(luma[static_cast<size_t>(y) * width + x]) -
							static_cast<int>(DejudderTexture(x - sx, y - sy))) <= 1;
					}
				Assert::AreEqual(checked, exact, (L"luma at the true phase, t = " + std::to_wstring(t)).c_str());
				int chromaChecked = 0, chromaExact = 0;
				for (int y = dy + 2; y < height - dy - 2; ++y)
					for (int x = dx / 2 + 2; x < width / 2 - dx / 2 - 2; ++x)
						for (int c = 0; c < 2; ++c)
						{
							++chromaChecked;
							chromaExact += std::abs(static_cast<int>(chroma[(static_cast<size_t>(y) * (width / 2) + x) * 2 + c]) -
								static_cast<int>(DejudderTexture((x - sx / 2) * 2, y - sy, 7 + c))) <= 1;
						}
				Assert::AreEqual(chromaChecked, chromaExact, (L"chroma at the true phase, t = " + std::to_wstring(t)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// The same pan through the matcher's own field (the de-judder's estimate).
		TEST_METHOD(InterpolatorPanLandsAtTheTruePhaseWithTheMatchersField)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes),
				std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			Interpolator::Motion motion;
			motion.field = warp.Field();
			motion.frameTrust = warp.FrameTrust();
			motion.minTrustedShare = warp.GetSettings().minTrustedShare;
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			Dejudder::Planes out;
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, motion, 0.5f,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, close = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 24; x < width - 24; ++x)
				{
					++checked;
					close += std::abs(static_cast<int>(luma[static_cast<size_t>(y) * width + x]) -
						static_cast<int>(DejudderTexture(x - dx / 2, y - dy / 2))) <= 64;
				}
			Assert::IsTrue(close * 100 >= checked * 95,
				(L"pixels at the true phase (6, 3): " + std::to_wstring(close) + L" of " + std::to_wstring(checked)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorOcclusionHoleHoldsNearest)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 16, y, 3); },
				[](int x, int y, int c) { return DejudderTexture((x - 8) * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks 6..9 x 2..5 (source px 96..159 x 32..95) untrusted: the hole.
			auto inHole = [](int bx, int by) { return bx >= 6 && bx <= 9 && by >= 2 && by <= 5; };
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 0.8f, [&](int bx, int by) {
				return std::array<float, 4>{ 16.0f, 0.0f, inHole(bx, by) ? 0.0f : 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
				for (float t : { 0.25f, 0.75f })
				{
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t, fallback, out),
						InterpolatorError(interpolator, log).c_str());
					const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
					int checked = 0, right = 0;
					for (int y = 32; y < 96; ++y)
						for (int x = 96; x < 160; ++x)
						{
							const size_t i = static_cast<size_t>(y) * width + x;
							const double expected = fallback == InterpolationPolicy::Fallback::Hold ?
								(t < 0.5f ? a.luma[i] : b.luma[i]) : (1.0 - t) * a.luma[i] + t * b.luma[i];
							++checked;
							right += std::abs(luma[i] - expected) <= (fallback == InterpolationPolicy::Fallback::Hold ? 0.0 : 1.0);
						}
					Assert::AreEqual(checked, right, (std::wstring(fallback == InterpolationPolicy::Fallback::Hold ?
						L"hold: the nearest frame's own pixels, t = " : L"blend: the mix, t = ") + std::to_wstring(t)).c_str());
				}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorCutHoldsWholeFrame)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,   // another scene
				[](int x, int y) { return DejudderTexture(x, y, 11); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 13 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// A few blocks still claim trust, but the share (0.1) is below min_trusted_share.
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 0.1f, [](int bx, int) {
				return std::array<float, 4>{ 8.0f, 4.0f, bx < 2 ? 1.0f : 0.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
				for (float t : { 0.1f, 0.49f, 0.51f, 0.9f })
				{
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t, fallback, out),
						InterpolatorError(interpolator, log).c_str());
					const DejudderFrame& nearest = t < 0.5f ? a : b;
					Assert::IsTrue(DownloadPlane(gpu, out.luma) == nearest.luma, L"a cut: the nearest frame's luma whole");
					Assert::IsTrue(DownloadPlane(gpu, out.chroma) == nearest.chroma, L"a cut: the nearest frame's chroma whole");
				}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Occlusion (B's mask): a trusted A vector whose landing in B is content A did not have
		// takes A alone (A's content goes behind it); an untrusted position whose own block of B
		// is new (disoccluded) takes B at any phase - only B has it.
		TEST_METHOD(InterpolatorOcclusionTakesAAndDisocclusionTakesB)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y, 4); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks x 0..9 still, x 13.. moving right by 16 px (away from the hole). Blocks
			// x 2..4: trusted, but B's content there is new (occlusion: A alone); blocks x 10..12:
			// untrusted and new in B. Block 12's neighbour 13 moves away from it (uncovered:
			// B); blocks 10 and 11 have nothing moving away (a bad match: the hold, A at t 0.25).
			// Checked over blocks 0..12 (the moving region's own warp is not the subject).
			auto occluded = [](int bx) { return bx >= 2 && bx <= 4; };
			auto disoccluded = [](int bx) { return bx >= 10 && bx <= 12; };
			auto uncovered = [](int bx) { return bx == 12; };
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f, [&](int bx, int) {
				return std::array<float, 4>{ bx >= 13 ? 16.0f : 0.0f, 0.0f, disoccluded(bx) ? 0.0f : 1.0f, 0.0f }; }, field);
			const int bx = width / 16, by = height / 16;
			std::vector<float> mask(static_cast<size_t>(bx) * by * 4, 1.0f);
			for (int y = 0; y < by; ++y)
				for (int x = 0; x < bx; ++x)
					if (occluded(x) || disoccluded(x))
						mask[(static_cast<size_t>(y) * bx + x) * 4] = 0.0f;
			field.occlusion = MakeFloatTexture(gpu, bx, by, mask);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			const float t = 0.25f;   // nearer A: the hold would show A
			Dejudder::Planes out;
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, right = 0;
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < 13 * 16; ++x)
				{
					const int blockX = x / 16;
					const size_t i = static_cast<size_t>(y) * width + x;
					double expected;
					if (occluded(blockX)) expected = a.luma[i];
					else if (uncovered(blockX)) expected = b.luma[i];
					else if (disoccluded(blockX)) expected = a.luma[i];   // no uncover: the hold (t < 0.5)
					else expected = (1.0 - t) * a.luma[i] + t * b.luma[i];
					++checked;
					right += std::abs(luma[i] - expected) <= 1.0;
				}
			Assert::AreEqual(checked, right, L"occluded: A alone; uncovered: B; new without uncover: the hold; elsewhere the time blend");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// No valid vectors (an invalidation, no estimate for the pair): Stage B still draws
		// every refresh - hold shows the nearest frame whole, blend the time-weighted mix.
		TEST_METHOD(InterpolatorWithoutVectorsHoldsNearestOrBlends)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 8, y, 5); },
				[](int x, int y, int c) { return DejudderTexture((x - 4) * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			Interpolator::Motion none;   // no field, no trust
			for (float t : { 0.2f, 0.8f })
			{
				Dejudder::Planes out;
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, none, t, InterpolationPolicy::Fallback::Hold, out),
					InterpolatorError(interpolator, log).c_str());
				Assert::IsTrue(DownloadPlane(gpu, out.luma) == (t < 0.5f ? a.luma : b.luma), L"hold: the nearest frame");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, none, t, InterpolationPolicy::Fallback::Blend, out),
					InterpolatorError(interpolator, log).c_str());
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				for (size_t i = 0; i < luma.size(); ++i)
					if (std::abs(luma[i] - ((1.0 - t) * a.luma[i] + t * b.luma[i])) > 1.0)
						Assert::Fail(L"blend: the time-weighted mix");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// HDR / 10-bit precision: a ramp over every 10-bit code (P010 layout: the code
		// in the top 10 bits), BTB and WTW included, and a PQ-coded ramp at 16 bits,
		// moved by whole pixels: the picture at the phase keeps every code exactly
		// (no 8-bit step, nothing clamped).
		TEST_METHOD(InterpolatorTenBitAndPqRampKeepPrecision)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 1024 + 64, height = 32;
			static constexpr int dx = 32;
			auto tenBit = [](int x) { return static_cast<uint16_t>((std::min)((std::max)(x - 32, 0), 1023) << 6); };
			auto pq = [](int x) {
				// The PQ (SMPTE ST 2084) code of a luminance ramp 0..10000 nits, 16-bit.
				const double y = std::pow((std::min)((std::max)(x - 32, 0), 1023) / 1023.0, 3.0);
				const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
				const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
				const double p = std::pow(y, m1);
				return static_cast<uint16_t>(std::lround(65535.0 * std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2)));
			};
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			interpolator.SetGrain(InterpolationPolicy::Grain::Blend);   // legacy: written before the grain path became the default
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ static_cast<float>(dx), 0.0f, 1.0f, 0.0f }; }, field);
			for (int ramp = 0; ramp < 2; ++ramp)
			{
				auto code = [&](int x) { return ramp == 0 ? tenBit(x) : pq(x); };
				const DejudderFrame a = MakeDejudderFrame(width, height, [&](int x, int) { return code(x); },
					[&](int x, int, int c) { return static_cast<uint16_t>(c ? code(2 * x) : 32768); });
				const DejudderFrame b = MakeDejudderFrame(width, height, [&](int x, int) { return code(x - dx); },
					[&](int x, int, int c) { return static_cast<uint16_t>(c ? code(2 * (x - dx / 2)) : 32768); });
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				for (int quarter = 1; quarter <= 3; ++quarter)
				{
					const int sx = dx * quarter / 4;
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.25f * quarter,
						InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
					const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
					for (int y = 0; y < height; ++y)
						for (int x = dx + 2; x < width - dx - 2; ++x)
						{
							const uint16_t got = luma[static_cast<size_t>(y) * width + x];
							if (got != code(x - sx))
								Assert::Fail((std::wstring(ramp == 0 ? L"10-bit" : L"PQ") + L" ramp code changed at x " +
									std::to_wstring(x) + L": " + std::to_wstring(got) + L" for " +
									std::to_wstring(code(x - sx))).c_str());
						}
					// Also verify chroma plane precision (Cb = 32768 constant, Cr = shifted ramp at half width).
					const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
					const int cw = width / 2, ch = height / 2, csx = sx / 2;
					for (int y = 0; y < ch; ++y)
						for (int x = dx / 2 + 2; x < cw - dx / 2 - 2; ++x)
						{
							const size_t idx = (static_cast<size_t>(y) * cw + x) * 2;
							Assert::AreEqual(static_cast<int>(32768), static_cast<int>(chroma[idx]),
								L"interpolated Cb must stay at 32768");
							const uint16_t gotCr = chroma[idx + 1];
							const uint16_t expCr = code(2 * (x - csx));
							if (std::abs(static_cast<int>(gotCr) - static_cast<int>(expCr)) > 1)
								Assert::Fail((std::wstring(ramp == 0 ? L"10-bit" : L"PQ") + L" chroma Cr changed at x " +
									std::to_wstring(x) + L": " + std::to_wstring(gotCr) + L" for " +
									std::to_wstring(expCr)).c_str());
						}
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// W23-B3 J#6 (MC-INTERP GEM-138 residual): stillHere short-circuit returns a0 bit-for-bit on
		// stationary pixels without blanket-skipping moving regions; single cat_a/cat_b under EXACT_EQUAL
		// preserves exact equal moving samples bit-for-bit and preserves grain RMS on moving grain regions.
		BEGIN_TEST_METHOD_ATTRIBUTE(McInterpStillHereAndExactEqualBitExact)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(McInterpStillHereAndExactEqualBitExact)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			struct LogCaptureGuard
			{
				TargetLutGpuFixture* f = nullptr;
				LogCaptureGuard(TargetLutGpuFixture* fix, std::string* dst) : f(fix) { f->CaptureLog(dst); }
				~LogCaptureGuard() { if (f) f->CaptureLog(nullptr); }
				LogCaptureGuard(const LogCaptureGuard&) = delete;
				LogCaptureGuard& operator=(const LogCaptureGuard&) = delete;
			};

			auto hashU16 = [](const std::vector<uint16_t>& v) -> uint64_t {
				uint64_t h = 14695981039346656037ull;
				const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
				const size_t n = v.size() * sizeof(uint16_t);
				for (size_t i = 0; i < n; ++i)
				{
					h ^= p[i];
					h *= 1099511628211ull;
				}
				return h;
			};

			const int width = 256, height = 128;
			const int dx = 4;
			auto baseSignal = [](double x, double y) -> double {
				return 32000.0 + 7000.0 * std::sin(6.283185307 * x / 31.0)
					+ 5000.0 * std::cos(6.283185307 * y / 29.0)
					+ 2500.0 * std::sin(6.283185307 * (x + y) / 13.0);
			};

			std::mt19937 rngA(13801u), rngB(13802u);
			std::normal_distribution<double> grainDist(0.0, 512.0);   // ~2.0 8-bit codes in 16-bit units

			DejudderFrame frameA, frameB;
			frameA.width = frameB.width = width;
			frameA.height = frameB.height = height;
			frameA.luma.resize(static_cast<size_t>(width) * height);
			frameB.luma.resize(static_cast<size_t>(width) * height);
			frameA.chroma.assign(static_cast<size_t>(width / 2) * height * 2, 32768u);
			frameB.chroma.assign(static_cast<size_t>(width / 2) * height * 2, 32768u);

			auto clampU16 = [](double v) -> uint16_t {
				return static_cast<uint16_t>(std::lround((std::min)(65535.0, (std::max)(0.0, v))));
			};

			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const size_t idx = static_cast<size_t>(y) * width + x;
					if (x < 128)
					{
						// Left half: stationary region with grain, bit-identical between A and B.
						const double g = grainDist(rngA);
						const uint16_t px = clampU16(baseSignal(x, y) + g);
						frameA.luma[idx] = px;
						frameB.luma[idx] = px;
					}
					else if (y < 64)
					{
						// Right-top strip: noiseless moving structure shifted by exact integer dx = 4 (exercises EXACT_EQUAL).
						frameA.luma[idx] = clampU16(baseSignal(x, y));
						frameB.luma[idx] = clampU16(baseSignal(x - dx, y));
					}
					else
					{
						// Right-bottom strip: moving structure shifted by dx = 4 + independent per-frame grain.
						const double gA = grainDist(rngA);
						const double gB = grainDist(rngB);
						frameA.luma[idx] = clampU16(baseSignal(x, y) + gA);
						frameB.luma[idx] = clampU16(baseSignal(x - dx, y) + gB);
					}
				}
			}

			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, frameA, planesA);
			UploadDejudderFrame(gpu, frameB, planesB);

			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[dx](int bx, int) -> std::array<float, 4> {
					if (bx < 4) return { 0.04f, -0.03f, 1.0f, 0.0f };   // dot(v,v) < 1/256 -> stillHere
					if (bx < 8) return { 0.30f, 0.20f, 1.0f, 0.0f };    // dot(v,v) < 0.25 && a0==b0 -> stillHere
					return { static_cast<float>(dx), 0.0f, 1.0f, 0.0f };
				}, field);

			std::string log;
			{
				LogCaptureGuard cap(&fixture, &log);
				Interpolator::FrameInterpolator preserveDefault(fixture.Log(), gpu, true);
				Interpolator::FrameInterpolator preserveExact(fixture.Log(), gpu, true);
				Interpolator::FrameInterpolator preserveExactRepeat(fixture.Log(), gpu, true);
				Interpolator::FrameInterpolator legacyBlend(fixture.Log(), gpu, true);
				preserveDefault.SetGrain(InterpolationPolicy::Grain::Preserve);
				preserveExact.SetGrain(InterpolationPolicy::Grain::Preserve);
				preserveExactRepeat.SetGrain(InterpolationPolicy::Grain::Preserve);
				legacyBlend.SetGrain(InterpolationPolicy::Grain::Blend);

				// Independent source grain RMS in the moving grain strip interior.
				auto stripGrainRms = [&](const std::vector<uint16_t>& img, double shiftX) -> double {
					double sq = 0.0;
					int n = 0;
					for (int y = 72; y < 112; ++y)
						for (int x = 148; x < 236; ++x)
						{
							const double d = static_cast<double>(img[static_cast<size_t>(y) * width + x])
								- baseSignal(static_cast<double>(x) - shiftX, static_cast<double>(y));
							sq += d * d;
							++n;
						}
					return std::sqrt(sq / static_cast<double>(n));
				};
				const double sourceGrainRms = stripGrainRms(frameA.luma, 0.0);
				Assert::IsTrue(sourceGrainRms > 350.0, L"synthetic moving strip carries real grain");

				for (float t : { 0.4f, 0.5f, 0.6f })
				{
					Dejudder::Planes outDef{}, outEx{}, outEx2{};
					Assert::IsTrue(preserveDefault.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
						InterpolationPolicy::Fallback::Hold, outDef, 0, 0), InterpolatorError(preserveDefault, log).c_str());
					Assert::IsTrue(preserveExact.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
						InterpolationPolicy::Fallback::Hold, outEx, width, height), InterpolatorError(preserveExact, log).c_str());
					Assert::IsTrue(preserveExactRepeat.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
						InterpolationPolicy::Fallback::Hold, outEx2, width, height), InterpolatorError(preserveExactRepeat, log).c_str());

					const std::vector<uint16_t> lumaDef = DownloadPlane(gpu, outDef.luma);
					const std::vector<uint16_t> lumaEx = DownloadPlane(gpu, outEx.luma);
					const std::vector<uint16_t> lumaEx2 = DownloadPlane(gpu, outEx2.luma);

					// Deterministic capture-hash pin for EXACT_EQUAL path.
					Assert::AreEqual(hashU16(lumaEx), hashU16(lumaEx2), L"EXACT_EQUAL output hash is deterministic across runs");
					Assert::IsTrue(lumaEx == lumaEx2, L"EXACT_EQUAL output pixels are bit-identical across runs");

					// 1. Stationary stillHere interior (both sub-regions bx < 4 and 4 <= bx < 8): exact bit-for-bit a0.
					for (int y = 16; y < 112; ++y)
						for (int x = 16; x < 112; ++x)
						{
							const size_t idx = static_cast<size_t>(y) * width + x;
							Assert::AreEqual(static_cast<int>(frameA.luma[idx]), static_cast<int>(lumaDef[idx]),
								L"default mode: stationary stillHere pixel must match a0 bit-for-bit");
							Assert::AreEqual(static_cast<int>(frameA.luma[idx]), static_cast<int>(lumaEx[idx]),
								L"EXACT_EQUAL mode: stationary stillHere pixel must match a0 bit-for-bit");
						}

					// 2. Negative control: moving region genuinely moves (differs substantially from a0).
					double sumDiffDef = 0.0, sumDiffEx = 0.0;
					int maxDiffDef = 0, maxDiffEx = 0, countMove = 0;
					for (int y = 16; y < 112; ++y)
						for (int x = 144; x < 240; ++x)
						{
							const size_t idx = static_cast<size_t>(y) * width + x;
							const int dd = std::abs(static_cast<int>(lumaDef[idx]) - static_cast<int>(frameA.luma[idx]));
							const int de = std::abs(static_cast<int>(lumaEx[idx]) - static_cast<int>(frameA.luma[idx]));
							sumDiffDef += dd;
							sumDiffEx += de;
							if (dd > maxDiffDef) maxDiffDef = dd;
							if (de > maxDiffEx) maxDiffEx = de;
							++countMove;
						}
					Assert::IsTrue(sumDiffDef / countMove > 200.0 && maxDiffDef > 2000,
						L"negative control (default): moving region must differ strongly from a0");
					Assert::IsTrue(sumDiffEx / countMove > 200.0 && maxDiffEx > 2000,
						L"negative control (EXACT_EQUAL): moving region must differ strongly from a0");

					if (t == 0.5f)
					{
						// 3. EXACT_EQUAL exact sample pin on integer-shifted noiseless strip (pa = x-2, pb = x+2 -> ca == cb).
						for (int y = 16; y < 48; ++y)
							for (int x = 144; x < 240; ++x)
							{
								const uint16_t expected = frameA.luma[static_cast<size_t>(y) * width + (x - 2)];
								const uint16_t got = lumaEx[static_cast<size_t>(y) * width + x];
								Assert::AreEqual(static_cast<int>(expected), static_cast<int>(got),
									L"EXACT_EQUAL: equal moving samples ca==cb must be bit-exact");
							}

						// 4. Grain preservation on moving grain strip + Blend negative control on exact same input.
						Dejudder::Planes outBlend{};
						Assert::IsTrue(legacyBlend.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, outBlend, width, height),
							InterpolatorError(legacyBlend, log).c_str());
						const std::vector<uint16_t> lumaBlend = DownloadPlane(gpu, outBlend.luma);
						const double ratioDef = stripGrainRms(lumaDef, 2.0) / sourceGrainRms;
						const double ratioEx = stripGrainRms(lumaEx, 2.0) / sourceGrainRms;
						const double ratioBlend = stripGrainRms(lumaBlend, 2.0) / sourceGrainRms;
						Assert::IsTrue(ratioDef > 0.85 && ratioDef < 1.15, L"default preserve keeps moving grain RMS");
						Assert::IsTrue(ratioEx > 0.85 && ratioEx < 1.15, L"EXACT_EQUAL preserve keeps moving grain RMS");
						Assert::IsTrue(ratioBlend < 0.80, L"negative control: Grain::Blend attenuates moving grain RMS below 0.80");
					}
				}
			}
			AssertCleanLibplaceboLog(log);
		}

		// W23-B3 J#6 / M2#1: 24p->120Hz (5 phases t = 0.0, 0.2, 0.4, 0.6, 0.8) for one source pair (seqA, seqB)
		// computes kBandMemo ONCE per source frame (fills == 2 total) and reuses cached bands across all interpolated
		// phases, with bit-exact output identity vs memo=nullptr fallback (which falls back on every interpolated phase).
		BEGIN_TEST_METHOD_ATTRIBUTE(McInterp24pTo120HzOneBandMemoComputePerFrame)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(McInterp24pTo120HzOneBandMemoComputePerFrame)
		{
			namespace GB = Vpv::GrainBand;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			struct LogCaptureGuard
			{
				TargetLutGpuFixture* f = nullptr;
				LogCaptureGuard(TargetLutGpuFixture* fix, std::string* dst) : f(fix) { f->CaptureLog(dst); }
				~LogCaptureGuard() { if (f) f->CaptureLog(nullptr); }
				LogCaptureGuard(const LogCaptureGuard&) = delete;
				LogCaptureGuard& operator=(const LogCaptureGuard&) = delete;
			};
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct MemoGpuGuard
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				explicit MemoGpuGuard(pl_gpu g) : ownerDev(g) {}
				~MemoGpuGuard()
				{
					memo.ReleaseGpuSpots();
					ownerDev.Release();
				}
				MemoGpuGuard(const MemoGpuGuard&) = delete;
				MemoGpuGuard& operator=(const MemoGpuGuard&) = delete;
			};

			auto hashU16 = [](const std::vector<uint16_t>& v) -> uint64_t {
				uint64_t h = 14695981039346656037ull;
				const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
				const size_t n = v.size() * sizeof(uint16_t);
				for (size_t i = 0; i < n; ++i)
				{
					h ^= p[i];
					h *= 1099511628211ull;
				}
				return h;
			};

			const int width = 256, height = 128;
			const int hw = (width + 1) / 2, hh = (height + 1) / 2;
			const double vx = 3.3, vy = 1.7;
			const GrainFrames gf = MakeGrainFrames(width, height, 2, vx, vy, 2.0, false, 73u);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, gf.frames[0], planesA);
			UploadDejudderFrame(gpu, gf.frames[1], planesB);

			auto uploadHalfR32F = [&](const std::vector<uint16_t>& fullLuma) {
				std::vector<float> half(static_cast<size_t>(hw) * hh, 0.0f);
				for (int qy = 0; qy < hh; ++qy)
					for (int qx = 0; qx < hw; ++qx)
					{
						const int x0 = (std::min)(2 * qx, width - 1), x1 = (std::min)(2 * qx + 1, width - 1);
						const int y0 = (std::min)(2 * qy, height - 1), y1 = (std::min)(2 * qy + 1, height - 1);
						const float a = static_cast<float>(fullLuma[static_cast<size_t>(y0) * width + x0]) / 65535.0f;
						const float b = static_cast<float>(fullLuma[static_cast<size_t>(y0) * width + x1]) / 65535.0f;
						const float c = static_cast<float>(fullLuma[static_cast<size_t>(y1) * width + x0]) / 65535.0f;
						const float d = static_cast<float>(fullLuma[static_cast<size_t>(y1) * width + x1]) / 65535.0f;
						half[static_cast<size_t>(qy) * hw + qx] = 0.25f * (a + b + c + d);
					}
				pl_tex_params p{};
				p.w = hw; p.h = hh; p.format = pl_find_named_fmt(gpu, "r32f"); p.sampleable = true; p.initial_data = half.data();
				return pl_tex_create(gpu, &p);
			};

			TexGuard halfA(gpu, uploadHalfR32F(gf.frames[0].luma));
			TexGuard halfB(gpu, uploadHalfR32F(gf.frames[1].luma));
			Assert::IsTrue(halfA.tex && halfB.tex, L"half-res P1 textures created");

			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[vx, vy](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);

			MemoGpuGuard gpuMemo(gpu);
			int hostTag = 0;
			gpuMemo.memo.SetHost(&hostTag);
			gpuMemo.memo.SetRenderThread(std::this_thread::get_id());
			gpuMemo.memo.SetGpuDispatcherPresent(true);
			gpuMemo.memo.SetGpuBudget(true, uint64_t(1) << 28);
			gpuMemo.memo.SetGpuDevice(&gpuMemo.ownerDev);

			const uint64_t seqA = 200u, seqB = 201u;
			pl_tex halfAPtr = halfA.tex, halfBPtr = halfB.tex;
			VpSkeleton::OwnerFill p1Fill;
			p1Fill.view = true;
			p1Fill.frameScoped = true;
			p1Fill.fill = [halfAPtr, halfBPtr, hw, hh, seqA, seqB](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				pl_tex chosen = (req.seq == seqA) ? halfAPtr : (req.seq == seqB ? halfBPtr : nullptr);
				if (!chosen)
					return false;
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(chosen));
				out.gpu.width = static_cast<uint32_t>(hw);
				out.gpu.height = static_cast<uint32_t>(hh);
				out.gpu.bytes = 0;
				return true;
			};
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(p1Fill)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			const uint64_t reserve = GB::BandMemoReserveBytes(VpSkeleton::Dims{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu,
				GB::kBandMemoUnit, GB::MakeBandMemoOwnerFill(reserve, counters)) == Reg::Registered);

			// Luma-only local views (keep planesA/planesB intact for RAII teardown) so chroma fallback does not blur accounting.
			Dejudder::Planes aLumaOnly = planesA.planes;
			Dejudder::Planes bLumaOnly = planesB.planes;
			aLumaOnly.chroma = nullptr;
			bLumaOnly.chroma = nullptr;

			std::string log;
			{
				LogCaptureGuard cap(&fixture, &log);
				Interpolator::FrameInterpolator interpMemo(fixture.Log(), gpu, true);
				Interpolator::FrameInterpolator interpFallback(fixture.Log(), gpu, true);
				interpMemo.SetGrain(InterpolationPolicy::Grain::Preserve);
				interpFallback.SetGrain(InterpolationPolicy::Grain::Preserve);

				const float phases[5] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f };
				std::vector<uint16_t> phaseLuma[5];
				for (int p = 0; p < 5; ++p)
				{
					Dejudder::Planes outMemo{}, outFb{};
					Assert::IsTrue(interpMemo.Run(aLumaOnly, bLumaOnly, InterpolatorMotion(field), phases[p],
						InterpolationPolicy::Fallback::Hold, outMemo, 0, 0, &gpuMemo.memo, seqA, seqB),
						InterpolatorError(interpMemo, log).c_str());
					Assert::IsTrue(interpFallback.Run(aLumaOnly, bLumaOnly, InterpolatorMotion(field), phases[p],
						InterpolationPolicy::Fallback::Hold, outFb, 0, 0, nullptr, seqA, seqB),
						InterpolatorError(interpFallback, log).c_str());

					const std::vector<uint16_t> lumaMemo = DownloadPlane(gpu, outMemo.luma);
					const std::vector<uint16_t> lumaFb = DownloadPlane(gpu, outFb.luma);
					Assert::AreEqual(hashU16(lumaFb), hashU16(lumaMemo),
						L"24p->120Hz phase output hash with kBandMemo must match memo=nullptr fallback");
					Assert::IsTrue(lumaMemo == lumaFb,
						L"24p->120Hz phase output pixels with kBandMemo must match memo=nullptr fallback bit-for-bit");
					phaseLuma[p] = lumaMemo;
				}

				// 1 compute for seqA + 1 compute for seqB across all 5 phases (phase 0.0 is passthrough; phases 0.2..0.8 reuse).
				Assert::AreEqual(uint64_t(2), counters->fills.load(),
					L"24p->120Hz must compute kBandMemo exactly ONCE for seqA and ONCE for seqB (fills == 2)");
				Assert::AreEqual(1ull, interpMemo.Passthroughs(), L"phase 0.0 is passthrough");
				Assert::AreEqual(4ull, interpMemo.GrainPreservedRuns(), L"phases 0.2, 0.4, 0.6, 0.8 run grain-preserved path");
				Assert::AreEqual(8ull, interpMemo.BandMemoHits(), L"all 4 interpolated phases hit kBandMemo for both seqA and seqB");
				Assert::AreEqual(0ull, interpMemo.BandMemoFallbacks(), L"zero BandMemo fallbacks on luma with memo registered");
				Assert::AreEqual(uint64_t(6), gpuMemo.memo.CountersOf(VpSkeleton::kBandMemo).reusedMemo,
					L"8 acquires minus 2 fills == 6 memo reuses across the 24p->120Hz cadence");

				// Negative control: without kBandMemo (memo=nullptr), every interpolated phase falls back (recomputes privately).
				Assert::AreEqual(1ull, interpFallback.Passthroughs(), L"fallback phase 0.0 is passthrough");
				Assert::AreEqual(4ull, interpFallback.GrainPreservedRuns(), L"fallback runs 4 interpolated phases");
				Assert::AreEqual(0ull, interpFallback.BandMemoHits(), L"memo=nullptr has zero BandMemo hits");
				Assert::AreEqual(8ull, interpFallback.BandMemoFallbacks(),
					L"negative control: memo=nullptr falls back on every interpolated phase (4 phases x 2 frames == 8)");
				Assert::IsFalse(interpFallback.LastBandMemoFallback().empty(),
					L"negative control: memo=nullptr records visible fallback reason");

				// Non-vacuity: interpolated phases 0.2 and 0.6 genuinely differ (not a frozen frame).
				Assert::AreNotEqual(hashU16(phaseLuma[1]), hashU16(phaseLuma[3]),
					L"phases t=0.2 and t=0.6 produce distinct interpolated frames");
				double sumDiff = 0.0;
				for (size_t i = 0; i < phaseLuma[1].size(); ++i)
					sumDiff += std::abs(static_cast<int>(phaseLuma[1][i]) - static_cast<int>(phaseLuma[3][i]));
				Assert::IsTrue(sumDiff / static_cast<double>(phaseLuma[1].size()) > 200.0,
					L"phases t=0.2 and t=0.6 differ substantially in luma");
			}
			AssertCleanLibplaceboLog(log);
		}

		// W23-Final J#7 (MC-CHURN GEM-147): FrameInterpolator stack descriptors + bitmask header cache parity across
		// distinct A!=B (Blend), identical A==B (#define src_b src_a), and Grain::Preserve (LowBand stack descriptors + PRESERVE_HEADER).
		TEST_METHOD(McChurnInterpolatorStackDescriptorsAndHeaderCacheParity)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const int width = 256, height = 128;
			const double vx = 4.0, vy = 2.0;
			const GrainFrames gf = MakeGrainFrames(width, height, 2, vx, vy, 2.0, false, 91u);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, gf.frames[0], planesA);
			UploadDejudderFrame(gpu, gf.frames[1], planesB);

			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[vx, vy](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);

			Interpolator::FrameInterpolator interp(fixture.Log(), gpu, true);
			auto meanAbsDiffU16 = [](const std::vector<uint16_t>& u, const std::vector<uint16_t>& v) -> double {
				Assert::AreEqual(u.size(), v.size());
				double sum = 0.0;
				for (size_t i = 0; i < u.size(); ++i)
					sum += std::abs(static_cast<int>(u[i]) - static_cast<int>(v[i]));
				return sum / static_cast<double>(u.size());
			};

			// (a) Distinct frames A != B, Blend mode (no grain bands, planeMask = 0), repeated calls bit-exact.
			interp.SetGrain(InterpolationPolicy::Grain::Blend);
			Dejudder::Planes outBlend1{}, outBlend2{};
			Assert::IsTrue(interp.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outBlend1), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> blendLuma1 = DownloadPlane(gpu, outBlend1.luma);
			const std::vector<uint16_t> blendChroma1 = DownloadPlane(gpu, outBlend1.chroma);
			Assert::IsTrue(interp.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outBlend2), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> blendLuma2 = DownloadPlane(gpu, outBlend2.luma);
			const std::vector<uint16_t> blendChroma2 = DownloadPlane(gpu, outBlend2.chroma);
			Assert::IsTrue(blendLuma1 == blendLuma2 && blendChroma1 == blendChroma2,
				L"Blend A!=B repeat must be bit-exact across cached header hit");

			// (b) Identical frames A == B, Blend mode (#define src_b src_a bitmask cache entry), repeated calls bit-exact.
			Dejudder::Planes outSame1{}, outSame2{};
			Assert::IsTrue(interp.Run(planesA.planes, planesA.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outSame1), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> sameLuma1 = DownloadPlane(gpu, outSame1.luma);
			Assert::IsTrue(interp.Run(planesA.planes, planesA.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outSame2), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> sameLuma2 = DownloadPlane(gpu, outSame2.luma);
			Assert::IsTrue(sameLuma1 == sameLuma2,
				L"Blend A==B (#define src_b src_a) repeat must be bit-exact");

			// Negative control: A != B at t = 0.5 genuinely interpolates between A and B (distinct from A == B identity).
			Assert::IsTrue(meanAbsDiffU16(blendLuma1, sameLuma1) > 200.0,
				L"negative control: A!=B at t=0.5 differs substantially from A==B");
			Assert::IsTrue(meanAbsDiffU16(blendLuma1, gf.frames[0].luma) > 200.0 &&
				meanAbsDiffU16(blendLuma1, gf.frames[1].luma) > 200.0,
				L"negative control: A!=B at t=0.5 genuinely interpolates between A and B (matches neither endpoint)");

			// (c) Grain::Preserve (LowBand stack descriptors + PRESERVE_HEADER cache entries for A!=B and A==B).
			interp.SetGrain(InterpolationPolicy::Grain::Preserve);
			Dejudder::Planes outPres1{}, outPres2{};
			Assert::IsTrue(interp.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outPres1), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> presLuma1 = DownloadPlane(gpu, outPres1.luma);
			const std::vector<uint16_t> presChroma1 = DownloadPlane(gpu, outPres1.chroma);
			Assert::IsTrue(interp.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outPres2), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> presLuma2 = DownloadPlane(gpu, outPres2.luma);
			const std::vector<uint16_t> presChroma2 = DownloadPlane(gpu, outPres2.chroma);
			Assert::IsTrue(presLuma1 == presLuma2 && presChroma1 == presChroma2,
				L"Grain::Preserve A!=B repeat must be bit-exact");
			Assert::IsTrue(interp.GrainPreservedRuns() >= 2ull, L"Grain::Preserve path actually executed");

			Dejudder::Planes outPresSame1{}, outPresSame2{};
			Assert::IsTrue(interp.Run(planesA.planes, planesA.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outPresSame1), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> presSame1 = DownloadPlane(gpu, outPresSame1.luma);
			Assert::IsTrue(interp.Run(planesA.planes, planesA.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, outPresSame2), InterpolatorError(interp, log).c_str());
			const std::vector<uint16_t> presSame2 = DownloadPlane(gpu, outPresSame2.luma);
			Assert::IsTrue(presSame1 == presSame2,
				L"Grain::Preserve A==B (#define src_b src_a + low_b low_a) repeat must be bit-exact");

			// Negative control: Grain::Preserve A!=B differs from both A==B and plain Blend on grainy moving frames.
			Assert::IsTrue(meanAbsDiffU16(presLuma1, presSame1) > 200.0,
				L"negative control: Grain::Preserve A!=B differs substantially from A==B");
			Assert::IsTrue(presLuma1 != blendLuma1,
				L"negative control: Grain::Preserve output differs from plain Blend on grainy frames");

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// W23-Audit-Fix1 POD1-H1 / POD1-M6 / MED-3: verify live unprocessed-plane FrameInterpolator call sites pass
		// SkeletonMemo + (seqA, seqB) and surface LastBandMemoFallback() into FallbackCatalog, FrameInterpolator
		// derives grid pitch without hardcoded 16.0, and MotionHostHelpers derives holdFeatherPx from referenceHeight.
		TEST_METHOD(LiveCallSitesWireBandMemoStatusAndDerivedGridPitch)
		{
			const std::string repoRoot = [&]() {
				std::string dir = __FILE__;
				const size_t pos = dir.find("\\src\\VP-V\\");
				return pos != std::string::npos ? dir.substr(0, pos) : std::string(".");
			}();
			auto readSrc = [&](const std::string& rel) -> std::string {
				std::ifstream f(repoRoot + "\\" + rel);
				Assert::IsTrue(f.is_open(), std::wstring(rel.begin(), rel.end()).c_str());
				std::ostringstream ss;
				ss << f.rdbuf();
				return ss.str();
			};

			// 1. POD1-H1: DejudderExtrasPresent.cpp & MotionHostHelpers.cpp (PhaseTimeline.inl) wire memo + seqA/seqB + FallbackCatalog.
			const std::string extras = readSrc("src\\VP-V\\modules\\motion\\consumers\\DejudderExtrasPresent.cpp");
			Assert::IsTrue(extras.find("Vpv::Skeleton::MemoForHost(h.host, f.sourceSequence)") != std::string::npos,
				L"POD1-H1: DejudderExtrasPresent must acquire host SkeletonMemo");
			Assert::IsTrue(extras.find("fm.memo.get(), f.sourceSequence, f.nextSourceSequence") != std::string::npos,
				L"POD1-H1: DejudderExtrasPresent must pass memo, seqA, seqB to FrameInterpolator::Run");
			Assert::IsTrue(extras.find("frameInterpolator->LastBandMemoFallback()") != std::string::npos,
				L"POD1-H1: DejudderExtrasPresent must surface LastBandMemoFallback() into FallbackCatalog");

			const std::string helpers = readSrc("src\\VP-V\\modules\\motion\\consumers\\MotionHostHelpers.cpp");
			Assert::IsTrue(helpers.find("vectorEngine.SkeletonMemoFor(dejudderExtras.vpPhaseSeqA)") != std::string::npos,
				L"POD1-H1: RunVpPhaseInterpolationHost must acquire SkeletonMemo");
			Assert::IsTrue(helpers.find("memo.get(), dejudderExtras.vpPhaseSeqA, dejudderExtras.vpPhaseSeqB") != std::string::npos,
				L"POD1-H1: RunVpPhaseInterpolationHost must pass memo, seqA, seqB to FrameInterpolator::Run");
			Assert::IsTrue(helpers.find("frameInterpolator->LastBandMemoFallback()") != std::string::npos,
				L"POD1-H1: RunVpPhaseInterpolationHost must surface LastBandMemoFallback() into FallbackCatalog");

			// 2. POD1-M6: zero hardcoded 16.0 grid division/multiplication in FrameInterpolator.cpp shaders.
			const std::string interp = readSrc("src\\VP-V\\modules\\motion\\consumers\\FrameInterpolator.cpp");
			Assert::IsTrue(interp.find("/ 16.0") == std::string::npos && interp.find("* 16.0") == std::string::npos,
				L"POD1-M6: FrameInterpolator.cpp must derive field_pitch/occ_pitch instead of hardcoding 16.0");
			Assert::IsTrue(interp.find("field_pitch") != std::string::npos && interp.find("occ_pitch") != std::string::npos,
				L"POD1-M6: FrameInterpolator.cpp must pass derived field_pitch and occ_pitch uniforms");

			// 3. MED-3: MotionHostHelpers::RenderedWarpSettings derives from referenceHeight (no hardcoded 2160.0f).
			Assert::IsTrue(helpers.find("2160.0f") == std::string::npos,
				L"MED-3: MotionHostHelpers.cpp must not hardcode 2160.0f");
			Assert::IsTrue(helpers.find("referenceHeight") != std::string::npos,
				L"MED-3: MotionHostHelpers.cpp must derive holdFeatherPx scaling from referenceHeight");

			// 4. FallbackCatalog decision contract + negative control.
			const FallbackCatalog::Decision engaged = FallbackCatalog::ProductFallback("product.interpolator", "kBandMemo unavailable");
			const FallbackCatalog::Decision released = FallbackCatalog::Released("product.interpolator");
			Assert::IsTrue(engaged.engaged && engaged.id == "product.interpolator" && engaged.reason.find("kBandMemo unavailable") != std::string::npos);
			Assert::IsFalse(released.engaged, L"negative control: empty LastBandMemoFallback releases product.interpolator cleanly");
		}
	};
}
