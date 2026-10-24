#include "pch.h"
#include <wincodec.h>
#include <atlbase.h>

#include "CppUnitTest.h"

#include "HardwarePrerequisites.h"
#include "../../tools/vp-scale-render/VpScaleRenderCore.h"
#include "vprenderer/CaptureTimestampGap.h"
#include "vprenderer/DejudderPolicy.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// fix/vpscalerender-motion (2026-10-24): VpScaleRender's picture_seq renders (the TrainKit's D
// pan data) must run VP's motion vectors continuously, so the hybrid scaler's grain map and vector
// refinements are genuinely active. Round 1 never ran them (de-judder strength 0 started no
// estimate; every picture boundary re-sent a timestamp: a discontinuity per pass). These tests
// keep it from silently breaking again: the pure rules on every build, and the real renderer
// (GpuHardware) through the tool's own code (tools/vp-scale-render/VpScaleRenderCore.h).
namespace VpScaleRenderMotionTests
{
	namespace Seq = VpScaleRenderSequence;

	TEST_CLASS(VpScaleRenderSequenceRules)
	{
	public:
		TEST_METHOD(SessionTimestampsAreNormalStepsAtEveryRateAndAcrossPictures)
		{
			// One counter over the whole job: every step, picture boundaries included, is one
			// period as the renderer judges it (llround(ticks / rate)), never a break or a miss.
			const long long rates[][2] = { { 12000, 1001 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 48, 1 }, { 60000, 1001 } };
			for (const long long tps : { 10000000LL, 24000000LL, 3579545LL })
				for (const auto& r : rates)
				{
					Seq::SessionClock clock;
					clock.startTicks = 123456789;
					clock.ticksPerSecond = tps;
					clock.rateNumerator = r[0];
					clock.rateDenominator = r[1];
					const long long expected = std::llround(static_cast<double>(tps) / (static_cast<double>(r[0]) / r[1]));
					for (long long n = 1; n < 20000; ++n)
					{
						const auto step = CaptureTimestampGap::Classify(clock.Stamp(n) - clock.Stamp(n - 1), expected, tps);
						Assert::IsTrue(step.kind == CaptureTimestampGap::Kind::Normal, L"every step is one period");
						Assert::AreEqual(Seq::SessionClock::Sequence(n - 1) + 1, Seq::SessionClock::Sequence(n));
					}
					// No drift: frame n is within a tick of n exact periods.
					const double exact = 19999.0 * static_cast<double>(tps) * r[1] / r[0];
					Assert::IsTrue(std::fabs(static_cast<double>(clock.Stamp(19999) - clock.startTicks) - exact) <= 1.0, L"no drift");
				}
		}

		TEST_METHOD(Round1BoundaryRepeatWasADiscontinuity)
		{
			// The round-1 defect, kept as a reference: the next picture re-sent the last frame's
			// timestamp (0 ticks) - a Break, i.e. "capture timestamp discontinuity" and a motion
			// invalidation at every pass; a skipped counter (2 periods) read as a missed frame.
			const long long expected = std::llround(10000000.0 * 1001.0 / 12000.0);
			Assert::IsTrue(CaptureTimestampGap::Classify(0, expected, 10000000).kind == CaptureTimestampGap::Kind::Break);
			Assert::IsTrue(CaptureTimestampGap::Classify(2 * expected, expected, 10000000).kind == CaptureTimestampGap::Kind::Missed);
		}

		TEST_METHOD(MotionOverridesStartTheEstimateWithoutExtras)
		{
			// The overrides: the estimate runs alone (occlusion mask) at strength 0, so every source
			// frame is estimated and no refresh is warped. The round-1 settings (strength 0, grain
			// map only) started nothing: the grain map never starts the estimate.
			std::map<std::string, std::string> o;
			for (const auto& v : Seq::MotionOverrides()) o[v.key] = v.value;
			Assert::AreEqual(std::string("on"), o["occlusion_mask"]);
			Assert::AreEqual(std::string("0"), o["dejudder_strength"]);
			Assert::AreEqual(std::string("0"), o["dejudder_strength_even"]);
			Assert::AreEqual(std::string("off"), o["interpolation"]);
			for (const double source : { 11.988, 12.0, 23.976, 24.0, 25.0, 29.97, 47.952, 48.0 })
				for (const double display : { 59.0, 60.0, 59.94, 120.0, 144.0, 24.0 })
				{
					DejudderPolicy::Input in;
					in.strength = 0.0;
					in.strengthEven = 0.0;
					in.occlusionMask = true;
					in.grainMap = true;
					in.sourceHz = source;
					in.displayHz = display;
					in.haveNext = true;
					in.nextMatches = true;
					Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Active, L"the estimate runs for every film-like rate");
					Assert::AreEqual(0.0, DejudderPolicy::EffectiveStrength(in), L"no extras: captures show whole source frames");
					in.occlusionMask = false;   // round 1
					Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Off, L"the grain map alone starts nothing");
				}
			// A video-rate source never runs it alone: picture_seq renders must declare a film-like rate.
			DejudderPolicy::Input video;
			video.occlusionMask = true;
			video.sourceHz = 59.94;
			video.displayHz = 60.0;
			video.haveNext = video.nextMatches = true;
			Assert::IsTrue(DejudderPolicy::Decide(video) == DejudderPolicy::Reason::VideoRateMaskOnly);
		}

		TEST_METHOD(MotionOverridesRewriteOnlyTheirKeysInEveryRendererSection)
		{
			const std::string cfg =
				"[general]\npersist_profile_selection: false\n"
				"[vprenderer]\nquality: high\ndejudder_strength: 0.5\n# a comment\nadaptive_w_base: 0.2\nunknown_key: kept\n"
				"[vprenderer.Default]\nquality: high\nocclusion_mask: off\ninterpolation: on\n"
				"[shader.standard.ssim_downscaler]\nshader_type: custom\nglsl_file: SSimDownscaler.glsl\ninterpolation: on\n";
			const std::string out = Seq::WithMotionOverrides(cfg);
			auto count = [&](const std::string& s) { size_t c = 0; for (size_t p = out.find(s); p != std::string::npos; p = out.find(s, p + 1)) ++c; return c; };
			Assert::AreEqual(size_t(2), count("occlusion_mask: on"));
			Assert::AreEqual(size_t(0), count("occlusion_mask: off"));
			Assert::AreEqual(size_t(0), count("dejudder_strength: 0.5"));
			Assert::AreEqual(size_t(2), count("dejudder_strength: 0\n"));
			Assert::AreEqual(size_t(1), count("interpolation: on"), L"a shader section is not a renderer section");
			Assert::AreEqual(size_t(1), count("# a comment"));
			Assert::AreEqual(size_t(1), count("unknown_key: kept"));
			Assert::AreEqual(size_t(1), count("adaptive_w_base: 0.2"));
			Assert::AreEqual(size_t(1), count("glsl_file: SSimDownscaler.glsl"));
			const std::string bare = Seq::WithMotionOverrides("[general]\nx: 1\n");
			Assert::IsTrue(bare.find("[vprenderer]\nocclusion_mask: on") != std::string::npos, L"a missing [vprenderer] is added");
		}

		TEST_METHOD(ResultLineKeepsTheSevenColumnsTheTrainKitReads)
		{
			Seq::Capture c;
			c.tag = "pan0";
			c.frame = 7;
			c.png = "C:\\s\\a.png";
			c.json = "C:\\s\\a.json";
			c.sourceSequence = 99;
			c.requested = "pan0";
			c.ok = true;
			c.motionMode = "vectors";
			c.estimates = 120;
			c.invalidations = 2;
			std::istringstream s(Seq::ResultLine(c));
			std::vector<std::string> f;
			for (std::string t; s >> t;) f.push_back(t);
			Assert::AreEqual(size_t(10), f.size());
			Assert::AreEqual(std::string("pan0"), f[0]);
			Assert::AreEqual(std::string("7"), f[1]);
			Assert::AreEqual(std::string("99"), f[4]);
			Assert::AreEqual(std::string("ok"), f[6]);
			Assert::AreEqual(std::string("vectors"), f[7]);
			Seq::Capture missing;
			std::istringstream m(Seq::ResultLine(missing));
			f.clear();
			for (std::string t; m >> t;) f.push_back(t);
			Assert::AreEqual(std::string("-"), f[2], L"a missing png is '-' (the TrainKit skips it)");
			Assert::AreEqual(std::string("fail"), f[6]);
		}

		TEST_METHOD(SidecarAndLogParsing)
		{
			const std::string json = "{\n  \"tone_mapping\": \"auto\",\n  \"hybrid_motion_mode\": \"vectors\",\n  \"motion_estimates\": 41,\n  \"motion_invalidations\": 0,\n  \"x\": 1\n}\n";
			Assert::AreEqual(std::string("vectors"), Seq::JsonField(json, "hybrid_motion_mode"));
			Assert::AreEqual(std::string("41"), Seq::JsonField(json, "motion_estimates"));
			Assert::AreEqual(std::string("0"), Seq::JsonField(json, "motion_invalidations"));
			Assert::AreEqual(std::string(), Seq::JsonField(json, "absent"));
			std::string cause;
			Assert::AreEqual(17LL, Seq::InvalidationTotal("De-judder: motion invalidated (capture timestamp discontinuity; 17 in total); the vectors' users fall back", &cause));
			Assert::AreEqual(std::string("capture timestamp discontinuity"), cause);
			Assert::AreEqual(-1LL, Seq::InvalidationTotal("De-judder: motion estimated for 5 source frames"));
		}

		TEST_METHOD(MotionCheckFailsOnEachMissingCondition)
		{
			Seq::MotionCheck c;
			c.captures = 40; c.vectors = 40; c.framesSent = 40; c.estimates = 40; c.invalidations = 0; c.metadataKnown = true;
			std::string why;
			Assert::IsTrue(c.Passes(0.9, 0, why), std::wstring(why.begin(), why.end()).c_str());
			Seq::MotionCheck d = c; d.vectors = 30;
			Assert::IsFalse(d.Passes(0.9, 0, why));
			d = c; d.invalidations = 1;
			Assert::IsFalse(d.Passes(0.9, 0, why));
			Assert::IsTrue(d.Passes(0.9, 1, why));
			d = c; d.estimates = 10;
			Assert::IsFalse(d.Passes(0.9, 0, why));
			d = c; d.metadataKnown = false;
			Assert::IsFalse(d.Passes(0.9, 0, why));
			d = c; d.captures = 0; d.vectors = 0;
			Assert::IsFalse(d.Passes(0.9, 0, why));
		}

		TEST_METHOD(JobKeysAndDefaults)
		{
			std::istringstream legacy("dll a\nconfig b\nshots c\nresult d\nsource 64 32 12000 1001\nwindow 32 16\npicture_seq p x 12 4\n");
			VpScaleRenderCore::Job j;
			Assert::IsTrue(j.Parse(legacy));
			Assert::IsFalse(j.motion);
			Assert::IsFalse(j.perFrame, L"legacy jobs keep the after-complete captures");
			Assert::AreEqual(0.0, j.settle);
			std::istringstream motion("dll a\nconfig b\nshots c\nresult d\nsource 64 32 12000 1001\nwindow 32 16\nmotion vectors\nrequire_motion 0.9 1\npicture_seq p x 12\n");
			VpScaleRenderCore::Job m;
			Assert::IsTrue(m.Parse(motion));
			Assert::IsTrue(m.motion && m.perFrame);
			Assert::AreEqual(3.0, m.settle);
			Assert::AreEqual(0.9, m.requireMotion);
			Assert::AreEqual(1LL, m.maxInvalidations);
		}
	};

	// ---- The real renderer through the tool's code ----

	std::string ModuleFolder()
	{
		char here[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&ModuleFolder), &self);
		GetModuleFileNameA(self, here, MAX_PATH);
		const std::string binary(here);
		return binary.substr(0, binary.find_last_of("\\/") + 1);
	}

	std::string TempFolder(const std::string& name)
	{
		char temp[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, temp);
		std::string folder = std::string(temp) + "vp-scale-render-motion";
		CreateDirectoryA(folder.c_str(), nullptr);
		folder += "\\" + name;
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	// A grain-like picture: a smooth, gently varying base with fixed per-pixel detail of film-grain
	// amplitude (about 3 8-bit codes rms: below the hybrid's edge threshold, above its grain ramp).
	// Spatially it reads as grain to the scalar (per-frame) measure; temporally it is clean, which
	// only the motion-compensated temporal error of the vectors can see - and on a pan only when
	// the vectors follow the motion.
	double Texture(double x, double y)
	{
		auto hash = [](int i, int j) { unsigned h = static_cast<unsigned>(i) * 374761393u + static_cast<unsigned>(j) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 1023) / 1023.0; };
		const double fine = hash(static_cast<int>(std::floor(x)) + 7919, static_cast<int>(std::floor(y)) - 104729) - 0.5;
		return 0.45 + 0.12 * std::cos(x * 0.011 + y * 0.004) + 0.08 * std::cos(x * 0.003 - y * 0.017) + 0.048 * fine;
	}

	// `frames` frames of a pan at (vx, vy) px per frame (0, 0: a still), w x h luma codes, to `path`,
	// with film-like grain: independent in every frame (+-grain codes, triangular), so only a
	// motion-compensated temporal error tells it from the moving texture.
	void WritePan(const std::string& path, int w, int h, int frames, double vx, double vy, int grain = 0)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		std::vector<uint16_t> row(static_cast<size_t>(w));
		unsigned state = 12345u;
		auto noise = [&]() { state = state * 1664525u + 1013904223u; const int a = static_cast<int>((state >> 8) % (2 * grain + 1));
			state = state * 1664525u + 1013904223u; const int b = static_cast<int>((state >> 8) % (2 * grain + 1)); return (a + b) / 2 - grain; };
		for (int t = 0; t < frames; ++t)
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
					row[x] = static_cast<uint16_t>((std::max)(4L, (std::min)(1019L, std::lround(64 + 876 * (std::min)(1.0, (std::max)(0.0,
						Texture(x + 0.5 + t * vx, y + 0.5 + t * vy)))) + (grain > 0 ? noise() : 0))));
				out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * 2));
			}
	}

	// The capture PNG's R channel as R10 codes (16-bit PNG: code c stored as (c << 6) | (c >> 4)).
	std::vector<int> LoadCaptureLuma(const std::string& png, int& w, int& h)
	{
		std::vector<int> codes;
		w = h = 0;
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		CComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return codes;
		CComPtr<IWICBitmapDecoder> decoder;
		const std::wstring wpng(png.begin(), png.end());
		if (FAILED(factory->CreateDecoderFromFilename(wpng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return codes;
		CComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return codes;
		CComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter))) return codes;
		if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return codes;
		UINT uw = 0, uh = 0;
		converter->GetSize(&uw, &uh);
		std::vector<uint16_t> rgba(static_cast<size_t>(uw) * uh * 4);
		if (FAILED(converter->CopyPixels(nullptr, uw * 8, static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data())))) return codes;
		w = static_cast<int>(uw);
		h = static_cast<int>(uh);
		codes.resize(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < codes.size(); ++i)
			codes[i] = rgba[i * 4] >> 6;
		return codes;
	}

	void ToTestLog(const std::string& line) { Logger::WriteMessage((line + "\n").c_str()); }

	struct Render
	{
		VpScaleRenderCore::Outcome outcome;
		std::map<int, std::string> pngByFrame;   // frame index -> the first capture of it
	};

	// One VpScaleRender job as the TrainKit writes it (the hybrid downscaler, w 0.2), 2:1 down.
	Render RenderSequence(const std::string& name, const std::string& raw, int frames, bool motion, double requireMotion)
	{
		const std::string folder = TempFolder(name);
		const std::string here = ModuleFolder();
		const std::string dll = here + "vprenderer-beta\\VideoProcessorVPRendererBeta.dll";
		SetEnvironmentVariableA("VP_TEST_SHADER_DIR", (here + "vprenderer-beta").c_str());
		{
			const std::string cache = here + "vp-harness-shader-cache";
			CreateDirectoryA(cache.c_str(), nullptr);
			CreateDirectoryA((cache + "\\vprenderer-beta").c_str(), nullptr);
			SetEnvironmentVariableA("VP_TEST_SHADER_CACHE_DIR", cache.c_str());
		}
		const std::string kv = "quality: high\ndithering: off\ndeband: off\ndeband_vp: off\ntemporal_dither: off\nsource_resolution: native\n"
			"adaptive_grain_map: on\nadaptive_chroma_ssim: off\ninterpolation: off\ninterpolation_fill_gaps: off\ndejudder_strength: 0\n"
			"fbo_float32: on\ndownscaler: lanczos\ndownscale_light: linear\nadaptive_downscale: on\nadaptive_w_base: 0.2\nadaptive_faithful: ortho\n";
		{
			std::ofstream cfg(folder + "\\render.cfg", std::ios::trunc);
			cfg << "[general]\npersist_profile_selection: false\n[vprenderer]\n" << kv << "[vprenderer.Default]\n" << kv
				<< "[shader.standard]\ntype: multi\nlabel: Off\n[shader.standard.ssim_downscaler]\nshader_type: custom\nlabel: SSimDownscaler\n"
				"stage: post_resize\norder: 20\nglsl_file: SSimDownscaler.glsl\n";
		}
		DeleteFileA((folder + "\\result.txt").c_str());
		DeleteFileA((folder + "\\vp.log").c_str());
		std::ostringstream jobText;
		jobText << "dll " << dll << "\nconfig " << folder << "\\render.cfg\nshots " << folder << "\\shots\nresult " << folder
			<< "\\result.txt\nlog " << folder << "\\vp.log\nsource 1920 1080 12000 1001\nwindow 960 540\nshaders standard.ssim_downscaler\n"
			<< "require_hook SSimDownscaler.glsl\ncapture per_frame\nsettle 2\n" << (motion ? "motion vectors\n" : "")
			<< (requireMotion >= 0.0 ? "require_motion " + std::to_string(requireMotion) + " 0\n" : "")
			<< "picture_seq " << name << " " << raw << " " << frames << "\n";
		VpScaleRenderCore::Job job;
		std::istringstream in(jobText.str());
		Assert::IsTrue(job.Parse(in), L"the job parses");
		Render r;
		r.outcome = VpScaleRenderCore::RunJob(job, &ToTestLog);
		for (const auto& c : r.outcome.captures)
			if (c.tag == name && c.frame >= 0 && !c.png.empty() && !r.pngByFrame.count(c.frame))
				r.pngByFrame[c.frame] = c.png;
		return r;
	}

	// Mean absolute difference (codes) of capture a at (x, y) and capture b at (x + dx, y + dy), interior only.
	double MeanAbsDiff(const std::string& a, const std::string& b, int dx = 0, int dy = 0)
	{
		int wa = 0, ha = 0, wb = 0, hb = 0;
		const std::vector<int> x = LoadCaptureLuma(a, wa, ha), y = LoadCaptureLuma(b, wb, hb);
		if (x.empty() || wa != wb || ha != hb) return -1.0;
		double sum = 0.0;
		long long count = 0;
		for (int j = 16; j < ha - 16; ++j)
			for (int i = 16; i < wa - 16; ++i, ++count)
				sum += std::abs(x[static_cast<size_t>(j) * wa + i] - y[static_cast<size_t>(j + dy) * wa + (i + dx)]);
		return count ? sum / count : -1.0;
	}

	TEST_CLASS(VpScaleRenderMotionGpu)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(PanRendersRunContinuousVectors)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PanRendersRunContinuousVectors)
		{
			std::string reason;
			if (!HardwarePrerequisites::RequireConditionOrSkip("PanRendersRunContinuousVectors",
				HardwarePrerequisites::HardwareD3D11Renderer(reason), reason)) return;
			const int frames = 36;
			const std::string pan = TempFolder("sources") + "\\pan.raw", still = TempFolder("sources") + "\\still.raw";
			WritePan(pan, 1920, 1080, frames, 4.0, 2.0);   // whole pixels: exact motion, 2 x 1 output px
			WritePan(still, 1920, 1080, frames, 0.0, 0.0);

			// 1. motion vectors: every capture after the settle ran the grain map on vectors, no
			// invalidation, an estimate for (nearly) every source frame (the tool's own check, exit 7).
			const Render on = RenderSequence("pan_vectors", pan, frames, true, 0.9);
			const auto& c = on.outcome.check;
			Logger::WriteMessage((on.outcome.summary + "\n").c_str());
			Assert::AreEqual(0, on.outcome.rc, (L"motion check / captures: " + std::wstring(on.outcome.summary.begin(), on.outcome.summary.end())).c_str());
			Assert::IsTrue(c.metadataKnown, L"the renderer reports hybrid_motion_mode");
			Assert::IsTrue(c.captures >= frames * 2 / 3, L"most frames captured in one pass");
			Assert::IsTrue(c.VectorShare() >= 0.9, L"vectors on the captures");
			Assert::AreEqual(0LL, c.invalidations, L"no motion invalidation after the settle");
			Assert::IsTrue(c.estimates * 10 >= c.framesSent * 8, L"an estimate per source frame");

			// 2. The same pan without the motion mode (round 1's settings): the grain map runs, but
			// without vectors.
			const Render off = RenderSequence("pan_novectors", pan, frames, false, -1.0);
			Logger::WriteMessage((off.outcome.summary + "\n").c_str());
			Assert::IsTrue(off.outcome.check.captures > 0, L"captures without the motion mode");
			Assert::AreEqual(0, off.outcome.check.vectors, L"without the motion mode no capture has vectors");

			// 3. The vectors path is genuinely in the pixels: the same source frame renders differently
			// with and without vectors (the grain map's temporal term), and each capture is named after
			// the frame it shows (frame-exact, checked by the motion between consecutive captures).
			int compared = 0;
			double diffSum = 0.0;
			for (const auto& kv : on.pngByFrame)
			{
				const auto other = off.pngByFrame.find(kv.first);
				if (other == off.pngByFrame.end()) continue;
				const double d = MeanAbsDiff(kv.second, other->second);
				Assert::IsTrue(d >= 0.0, L"captures load");
				diffSum += d;
				++compared;
			}
			Logger::WriteMessage(("pan: frames compared " + std::to_string(compared) + ", mean |vectors - no vectors| " +
				std::to_string(compared ? diffSum / compared : 0.0) + " codes\n").c_str());
			Assert::IsTrue(compared >= frames / 2, L"frames captured in both renders");
			Assert::IsTrue(diffSum / compared > 0.05, L"the vector path changes the output (the grain map is driven by the vectors)");
			// Frame-exact mapping: the pan moves the content by (4, 2) source px per frame, (2, 1) output
			// px, so frame k + 1 at (x, y) is frame k at (x + 2, y + 1) when both captures are named right.
			double aligned = 0.0, unaligned = 0.0;
			int pairs = 0;
			for (const auto& kv : on.pngByFrame)
			{
				const auto next = on.pngByFrame.find(kv.first + 1);
				if (next == on.pngByFrame.end()) continue;
				aligned += MeanAbsDiff(next->second, kv.second, 2, 1);
				unaligned += MeanAbsDiff(next->second, kv.second, 0, 0);
				++pairs;
			}
			Logger::WriteMessage(("pan: consecutive captures, shifted by the motion " + std::to_string(pairs ? aligned / pairs : -1.0) +
				" codes, unshifted " + std::to_string(pairs ? unaligned / pairs : -1.0) + " over " + std::to_string(pairs) + " pairs\n").c_str());
			Assert::IsTrue(pairs >= frames / 2, L"consecutive frames captured");
			Assert::IsTrue(aligned * 4.0 < unaligned, L"every capture is named after the frame it shows");

			// 4. A still sequence of the same picture: the vectors run there too (zero motion). The
			// grain map responds to the motion only through the vectors: with them, the pan's map
			// (motion-compensated temporal error: the grain) matches the still's, so the vectors
			// change the pan's output about as much as the still's; without compensation the moving
			// texture would read as grain.
			const Render st = RenderSequence("still_vectors", still, frames, true, 0.9);
			Logger::WriteMessage((st.outcome.summary + "\n").c_str());
			Assert::AreEqual(0, st.outcome.rc, L"the still render passes the motion check");
			const Render stOff = RenderSequence("still_novectors", still, frames, false, -1.0);
			int stillCompared = 0;
			double stillDiff = 0.0;
			for (const auto& kv : st.pngByFrame)
			{
				const auto other = stOff.pngByFrame.find(kv.first);
				if (other == stOff.pngByFrame.end()) continue;
				stillDiff += MeanAbsDiff(kv.second, other->second);
				++stillCompared;
			}
			Logger::WriteMessage(("still: frames compared " + std::to_string(stillCompared) + ", mean |vectors - no vectors| " +
				std::to_string(stillCompared ? stillDiff / stillCompared : 0.0) + " codes\n").c_str());
			Assert::IsTrue(stillCompared >= frames / 2, L"still frames captured in both renders");
			Assert::IsTrue(stillDiff / stillCompared > 0.05, L"the vector path changes the still's output too");
			// Measured 2026-10-24 (RTX 3050 Ti): pan 0.118, still 0.122 codes. Vectors that did not follow
			// the pan would leave the moving detail in the temporal error (read as grain, like the
			// scalar): the pan's difference would collapse towards 0.
			Assert::IsTrue(diffSum / compared >= 0.6 * (stillDiff / stillCompared),
				L"on the pan the vectors' grain map matches the still's (it follows the motion)");
		}
	};
}
