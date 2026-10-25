// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

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
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
