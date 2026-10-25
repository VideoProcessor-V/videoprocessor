#include "pch.h"
#include <wincodec.h>
#include <atlbase.h>

#include "CppUnitTest.h"

#include "HardwarePrerequisites.h"
#include "../../tools/vp-scale-render/VpScaleRenderCore.h"
#include "vprenderer/CaptureTimestampGap.h"
#include "vprenderer/DejudderPolicy.h"

#include <algorithm>
#include <cstring>
#include <set>
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
			Assert::AreEqual(std::string("off"), o["temporal_dither"], L"no sigma-delta: the occlusion mask changes no pixel");
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

		static std::string Section(const std::string& text, const std::string& name)
		{
			const std::string head = "[" + name + "]\n";
			const size_t a = text.find(head);
			if (a == std::string::npos) return std::string("<absent>");
			const size_t b = text.find("\n[", a + head.size() - 1);   // an empty section ends at the next header
			return text.substr(a + head.size(), b == std::string::npos ? std::string::npos : b + 1 - a - head.size());
		}

		static std::string WriteTemp(const std::string& name, const std::string& text)
		{
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			const std::string path = std::string(temp) + "vpsr-cfg-" + name;
			std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
			return path;
		}

		static std::string RepoFile(const std::string& relative)
		{
			std::string path = __FILE__;
			std::replace(path.begin(), path.end(), '/', '\\');
			const size_t at = path.rfind("\\src\\VideoProcessor-Test\\");
			Assert::IsTrue(at != std::string::npos);
			return path.substr(0, at) + "\\" + relative;
		}

		TEST_METHOD(MotionOverridesGoOnlyToTheDisplayGroupAndKeepTheModelValid)
		{
			// Audit 2026-10-24 (HIGH): the overrides must never land in the child groups ([vprenderer.input.*],
			// [vprenderer.motion.*], [vprenderer.color.*], ...), where the keys are invalid and the renderer drops the
			// whole unified model ("unified renderer configuration ignored"). They go to [vprenderer] and its display
			// variants; the same keys are removed from every child section so no profile can switch them back.
			const std::string cfg =
				"[general]\npersist_profile_selection: false\n"
				"[vprenderer]\nquality: high\ndejudder_strength: 0.5\n# a comment\nadaptive_w_base: 0.2\n"
				"[vprenderer.Default]\nquality: high\nocclusion_mask: off\ninterpolation: auto\n"
				"[vprenderer.input]\nlabel: Input\n"
				"[vprenderer.input.film]\nwhen: $source_rate < 30\ndejudder_strength: 0.7\n"
				"[vprenderer.motion]\nlabel: Motion\n"
				"[vprenderer.motion.smooth]\nsmooth_motion: always\n"
				"[shader.standard.ssim_downscaler]\nshader_type: custom\nglsl_file: SSimDownscaler.glsl\ninterpolation: on\n";
			Seq::SectionPlan plan;
			const std::string out = Seq::WithMotionOverrides(cfg, &plan);
			Assert::IsTrue(plan.unified && plan.added.empty());
			Assert::IsTrue(plan.targets == std::set<std::string>({ "vprenderer", "vprenderer.default" }), L"only [vprenderer] and its display variant");
			for (const char* sec : { "vprenderer", "vprenderer.Default" })
				for (const auto& o : Seq::MotionOverrides())
				{
					const std::string body = Section(out, sec);
					const std::string kv = std::string(o.key) + ": " + o.value + "\n";
					Assert::IsTrue(body.find(kv) != std::string::npos, (L"override in [" + std::wstring(sec, sec + strlen(sec)) + L"]").c_str());
				}
			Assert::AreEqual(std::string("when: $source_rate < 30\n"), Section(out, "vprenderer.input.film"), L"child group: key removed, nothing injected");
			Assert::AreEqual(std::string(""), Section(out, "vprenderer.motion.smooth"), L"child group: smooth_motion removed, nothing injected");
			Assert::AreEqual(std::string("label: Input\n"), Section(out, "vprenderer.input"));
			Assert::IsTrue(out.find("dejudder_strength: 0.5") == std::string::npos && out.find("occlusion_mask: off") == std::string::npos);
			Assert::IsTrue(Section(out, "shader.standard.ssim_downscaler").find("interpolation: on") != std::string::npos, L"shader sections untouched");
			Assert::IsTrue(out.find("# a comment") != std::string::npos && out.find("adaptive_w_base: 0.2") != std::string::npos);
		}

		TEST_METHOD(MotionOverridesOnTheRepoSampleAndTheDeployedFixtureStayValid)
		{
			// The repo's sample configuration and the deployed-style fixture: the form is kept (unified stays unified),
			// the renderer's own reader accepts the result whenever it accepts the original, and the keys sit in
			// [vprenderer] (the tool refuses to render, rc 8, when this ever fails).
			for (const char* rel : { "VideoProcessor.cfg", "test-fixtures\\deployed-VideoProcessor-20260807-current.cfg",
				"docs\\examples\\VideoProcessorRenderer.unified.proposed.cfg" })
			{
				const std::string path = RepoFile(rel);
				std::ifstream in(path, std::ios::binary);
				if (!in) { Logger::WriteMessage((std::string("absent: ") + rel + "\n").c_str()); continue; }
				const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				std::string errOriginal, errMotion;
				const bool okOriginal = VpScaleRenderCore::ValidateRendererConfig(path, errOriginal);
				Seq::SectionPlan plan;
				const std::string out = Seq::WithMotionOverrides(text, &plan);
				const std::string tmp = WriteTemp("sample.cfg", out);
				const bool okMotion = VpScaleRenderCore::ValidateRendererConfig(tmp, errMotion);
				ConfigFile a, b;
				a.Load(path);
				b.Load(tmp);
				Logger::WriteMessage((std::string(rel) + ": original " + (okOriginal ? "valid" : "rejected: " + errOriginal) + "; with overrides " +
					(okMotion ? "valid" : "rejected: " + errMotion) + "\n").c_str());
				Assert::AreEqual(RendererProfileConfig::IsUnified(a), RendererProfileConfig::IsUnified(b), L"the form is kept");
				if (okOriginal)
					Assert::IsTrue(okMotion, std::wstring(errMotion.begin(), errMotion.end()).c_str());
				std::string v;   // the renderer's own display-key lookup (RendererConfigView)
				Assert::IsTrue(RendererConfigView(b).TryGetDisplayString("occlusion_mask", v) && v == "on", L"the override is where the renderer reads it");
			}
		}

		TEST_METHOD(MotionOverridesKeepALegacyFileLegacy)
		{
			const std::string legacy = "[display]\nquality: high\ndejudder_strength: 0.4\n[shortcuts]\nx: y\n";
			Seq::SectionPlan plan;
			const std::string out = Seq::WithMotionOverrides(legacy, &plan);
			Assert::IsFalse(plan.unified);
			Assert::IsTrue(plan.targets == std::set<std::string>({ "display" }) && plan.added.empty());
			Assert::IsTrue(out.find("[vprenderer]") == std::string::npos, L"never flipped to the unified form");
			Assert::IsTrue(Section(out, "display").find("occlusion_mask: on\n") != std::string::npos);
			Assert::IsTrue(out.find("dejudder_strength: 0.4") == std::string::npos);
			const std::string bare = Seq::WithMotionOverrides("[shortcuts]\nx: y\n", &plan);
			Assert::IsTrue(plan.added == "display" && bare.find("[display]\nocclusion_mask: on") != std::string::npos, L"a legacy file gets a legacy display section");
			const std::string unifiedBare = Seq::WithMotionOverrides("[general]\nx: 1\n", &plan);
			Assert::IsTrue(plan.added == "vprenderer" && unifiedBare.find("[vprenderer]\nocclusion_mask: on") != std::string::npos);
			const std::string legacyExample = RepoFile("docs\\examples\\VideoProcessorRenderer.legacy.cfg");
			std::ifstream in(legacyExample, std::ios::binary);
			if (in)
			{
				const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				const std::string tmp = WriteTemp("legacy.cfg", Seq::WithMotionOverrides(text, &plan));
				ConfigFile a, b;
				a.Load(legacyExample);
				b.Load(tmp);
				Assert::AreEqual(RendererProfileConfig::IsUnified(a), RendererProfileConfig::IsUnified(b), L"the legacy example keeps its form");
			}
		}

		TEST_METHOD(ConfigRejectionLinesAreRecognised)
		{
			Assert::IsTrue(Seq::IsConfigRejectionLine("unified renderer configuration ignored: [vprenderer.input.film] key 'x' is not a valid input setting"));
			Assert::IsTrue(Seq::IsConfigRejectionLine("WARNING: [vprenderer.Default] exists but is not applied: this file is not read as a unified configuration"));
			Assert::IsTrue(Seq::IsConfigRejectionLine("profile 'vprenderer.Default': invalid dejudder_search_range '99'; keeping 8"));
			Assert::IsTrue(Seq::IsConfigRejectionLine("display rule 'x': invalid dithering value 'q'; retaining base setting"));
			Assert::IsFalse(Seq::IsConfigRejectionLine("De-judder: motion invalidated (frame gap; 1 in total)"));
		}

		TEST_METHOD(CoverageCountsMissingDuplicateAndUnmappedFrames)
		{
			const std::vector<std::pair<std::string, int>> pics = { { "a", 4 }, { "b", 3 } };
			Seq::Coverage full = Seq::CoverageOf(pics, { { "a", 0 }, { "a", 1 }, { "a", 2 }, { "a", 3 }, { "b", 0 }, { "b", 1 }, { "b", 2 } }, 0);
			Assert::AreEqual(0LL, full.Failures());
			Assert::AreEqual(7LL, full.captured);
			Seq::Coverage gaps = Seq::CoverageOf(pics, { { "a", 0 }, { "a", 0 }, { "a", 2 }, { "a", 3 }, { "-", -1 }, { "b", 0 }, { "b", 2 } }, 1);
			Assert::AreEqual(2LL, gaps.missing);
			Assert::AreEqual(1LL, gaps.duplicates);
			Assert::AreEqual(1LL, gaps.unmapped);
			Assert::AreEqual(1LL, gaps.timeouts);
			Assert::AreEqual(5LL, gaps.Failures());
			Seq::MotionCheck c;
			c.captures = 7; c.vectors = 7; c.metadataKnown = true; c.coverage = gaps;
			std::string why;
			Assert::IsFalse(c.Passes(0.9, 0, why), L"require_motion requires full coverage");
			Assert::IsTrue(why.find("coverage") != std::string::npos);
			Assert::IsTrue(c.Passes(0.9, 0, why, 5), L"unless max_missing allows it");
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
			std::istringstream norate("dll a\nconfig b\nshots c\nresult d\nsource 64 32\nwindow 32 16\npicture_seq p x 12 4\n");
			VpScaleRenderCore::Job bad;
			Assert::IsFalse(bad.Parse(norate), L"no silent 24000/1001: a source line without a rate is an error");
			Assert::IsTrue(bad.error.find("rate") != std::string::npos);
			std::istringstream legacy("dll a\nconfig b\nshots c\nresult d\nsource 64 32 12000 1001\nwindow 32 16\npicture_seq p x 12 4\n");
			VpScaleRenderCore::Job j;
			Assert::IsTrue(j.Parse(legacy));
			Assert::IsFalse(j.motion);
			Assert::IsFalse(j.perFrame, L"legacy jobs keep the after-complete captures");
			Assert::AreEqual(0.0, j.settle);
			std::istringstream motion("dll a\nconfig b\nshots c\nresult d\nsource 64 32 12000 1001\nwindow 32 16\nmotion vectors\nrequire_motion 0.9 1 2\npicture_seq p x 12\n");
			VpScaleRenderCore::Job m;
			Assert::IsTrue(m.Parse(motion));
			Assert::IsTrue(m.motion && m.perFrame);
			Assert::AreEqual(3.0, m.settle);
			Assert::AreEqual(0.9, m.requireMotion);
			Assert::AreEqual(1LL, m.maxInvalidations);
			Assert::AreEqual(2LL, m.maxMissing);
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

	// A picture with fine detail of film-grain amplitude (about 3 8-bit codes rms: below the hybrid's edge
	// threshold, above its grain ramp) FIXED in the picture (it moves with a pan), on a smooth base, plus REAL
	// grain: independent in every frame. Spatially the fixed detail reads as grain to the scalar (per-frame)
	// measure; temporally only the real grain is noise - which only the motion-compensated temporal error of
	// the vectors can tell, and on a pan only when the vectors follow the motion. `seed` picks the detail.
	double Texture(double x, double y, int seed = 0)
	{
		auto hash = [](int i, int j) { unsigned h = static_cast<unsigned>(i) * 374761393u + static_cast<unsigned>(j) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 1023) / 1023.0; };
		const double fine = hash(static_cast<int>(std::floor(x)) + 7919 + seed * 4099, static_cast<int>(std::floor(y)) - 104729 - seed * 811) - 0.5;
		return 0.45 + 0.12 * std::cos(x * 0.011 + y * 0.004 + seed) + 0.08 * std::cos(x * 0.003 - y * 0.017) + 0.048 * fine;
	}

	// `frames` frames, w x h luma codes, to `path`: a pan at (vx, vy) px per frame ((0, 0): a still) of
	// Texture, with real grain of +-`grain` codes (triangular, new in every frame). alternate: odd frames show
	// another picture (seed 1) - consecutive frames are unrelated, so whatever the estimate returns does not
	// describe a motion (the negative control).
	void WritePan(const std::string& path, int w, int h, int frames, double vx, double vy, int grain, bool alternate = false)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		std::vector<uint16_t> row(static_cast<size_t>(w));
		unsigned state = 12345u;
		auto noise = [&]() {
			if (grain <= 0) return 0;
			state = state * 1664525u + 1013904223u; const int a = static_cast<int>((state >> 8) % (2 * grain + 1));
			state = state * 1664525u + 1013904223u; const int b = static_cast<int>((state >> 8) % (2 * grain + 1));
			return (a + b) / 2 - grain; };
		for (int t = 0; t < frames; ++t)
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
					row[x] = static_cast<uint16_t>((std::max)(4L, (std::min)(1019L, std::lround(64 + 876 * (std::min)(1.0, (std::max)(0.0,
						Texture(x + 0.5 + t * vx, y + 0.5 + t * vy, alternate ? (t & 1) : 0)))) + noise())));
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

	// One VpScaleRender job as the TrainKit writes it (the hybrid downscaler, w 0.2), 2:1 down. Every motion
	// override is already in the configuration (occlusion_mask off), so a run with `motion vectors` differs from
	// one without in exactly one setting - occlusion_mask, the switch that starts the estimate (audit 2026-10-24:
	// no confound from the other overrides).
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
		std::string kv = "quality: high\ndithering: off\ndeband: off\ndeband_vp: off\nsource_resolution: native\n"
			"adaptive_grain_map: on\nadaptive_chroma_ssim: off\nfbo_float32: on\ndownscaler: lanczos\ndownscale_light: linear\n"
			"adaptive_downscale: on\nadaptive_w_base: 0.2\nadaptive_faithful: ortho\n";
		for (const auto& o : Seq::MotionOverrides())
			kv += std::string(o.key) + ": " + (std::string(o.key) == "occlusion_mask" ? std::string("off") : std::string(o.value)) + "\n";
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

	// Absolute identity (audit 2026-10-24): a capture against the 2x2 box downscale of source frames k - 1, k,
	// k + 1 of the raw it was rendered from (central window, source code domain); the index of the closest.
	int ClosestSourceFrame(const std::string& png, const std::string& raw, int k, int frames)
	{
		int w = 0, h = 0;
		const std::vector<int> cap = LoadCaptureLuma(png, w, h);
		if (cap.empty() || w != 960 || h != 540) return -100;
		std::ifstream in(raw, std::ios::binary);
		std::vector<uint16_t> f(1920 * 1080);
		double best = 1e30;
		int bestK = -100;
		for (int t = (std::max)(0, k - 1); t <= (std::min)(frames - 1, k + 1); ++t)
		{
			in.seekg(static_cast<std::streamoff>(t) * f.size() * 2);
			in.read(reinterpret_cast<char*>(f.data()), static_cast<std::streamsize>(f.size() * 2));
			double e = 0.0;
			long long n = 0;
			for (int y = 140; y < 400; ++y)
				for (int x = 220; x < 740; ++x, ++n)
				{
					const double s = 0.25 * (f[(2 * y) * 1920 + 2 * x] + f[(2 * y) * 1920 + 2 * x + 1] + f[(2 * y + 1) * 1920 + 2 * x] + f[(2 * y + 1) * 1920 + 2 * x + 1]);
					const double c = cap[static_cast<size_t>(y) * w + x] * 876.0 / 1023.0 + 64.0;
					e += (s - c) * (s - c);
				}
			if (e / n < best) { best = e / n; bestK = t; }
		}
		return bestK;
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
			const int frames = 36, grain = 6;
			const std::string pan = TempFolder("sources") + "\\pan.raw", still = TempFolder("sources") + "\\still.raw",
				alt = TempFolder("sources") + "\\alternating.raw";
			WritePan(pan, 1920, 1080, frames, 4.0, 2.0, grain);   // whole pixels: exact motion, (2, 1) output px per frame
			WritePan(still, 1920, 1080, frames, 0.0, 0.0, grain);
			WritePan(alt, 1920, 1080, frames, 4.0, 2.0, grain, true);

			// 1. motion vectors: every capture after the settle ran the grain map on vectors, no invalidation (from
			// the sidecars), an estimate for (nearly) every source frame, every frame captured exactly once.
			const Render on = RenderSequence("pan_vectors", pan, frames, true, 0.9);
			const auto& c = on.outcome.check;
			Logger::WriteMessage((on.outcome.summary + "\n").c_str());
			Assert::AreEqual(0, on.outcome.rc, (L"motion check / captures: " + std::wstring(on.outcome.summary.begin(), on.outcome.summary.end())).c_str());
			Assert::IsTrue(c.metadataKnown, L"the renderer reports hybrid_motion_mode");
			Assert::AreEqual(0LL, c.coverage.Failures(), L"every frame captured exactly once");
			Assert::AreEqual(static_cast<long long>(frames), c.coverage.captured);
			Assert::IsTrue(c.VectorShare() >= 0.9, L"vectors on the captures");
			Assert::IsTrue(c.invalidationsFromSidecar, L"invalidations counted from the capture sidecars");
			Assert::AreEqual(0LL, c.invalidations, L"no motion invalidation after the settle");
			Assert::IsTrue(c.estimates * 10 >= c.framesSent * 8, L"an estimate per source frame");

			// 2. Absolute identity: each capture shows the source frame it is named after (not a neighbour: an
			// off-by-one in every label would pass a relative check).
			int identified = 0, wrong = 0;
			for (const auto& kv : on.pngByFrame)
			{
				const int k = ClosestSourceFrame(kv.second, pan, kv.first, frames);
				if (k == kv.first) ++identified; else ++wrong;
			}
			Logger::WriteMessage(("absolute identity: " + std::to_string(identified) + " right, " + std::to_string(wrong) + " wrong\n").c_str());
			Assert::AreEqual(0, wrong, L"every capture is the frame it is named after");
			Assert::AreEqual(frames, identified);

			// 3. The control: the same configuration with occlusion_mask off (the only difference), so the grain map
			// runs without vectors; the vectors path is genuinely in the pixels.
			const Render off = RenderSequence("pan_control", pan, frames, false, -1.0);
			Logger::WriteMessage((off.outcome.summary + "\n").c_str());
			Assert::AreEqual(0, off.outcome.rc);
			Assert::AreEqual(0, off.outcome.check.vectors, L"without the estimate no capture has vectors");
			auto diffOf = [&](const Render& a, const Render& b) {
				int n = 0;
				double sum = 0.0;
				for (const auto& kv : a.pngByFrame)
				{
					const auto other = b.pngByFrame.find(kv.first);
					if (other == b.pngByFrame.end()) continue;
					const double d = MeanAbsDiff(kv.second, other->second);
					Assert::IsTrue(d >= 0.0, L"captures load");
					sum += d;
					++n;
				}
				Assert::IsTrue(n >= frames / 2, L"frames captured in both renders");
				return sum / n; };
			const double panDiff = diffOf(on, off);

			// 4. A still of the same picture (correct vectors: zero motion) and the NEGATIVE control: unrelated
			// consecutive frames (whatever the estimate returns describes no motion). The metric must separate them:
			// on the pan the vectors' grain map matches the still's (it follows the motion); with meaningless vectors
			// the difference collapses towards the control.
			const Render st = RenderSequence("still_vectors", still, frames, true, 0.9);
			Assert::AreEqual(0, st.outcome.rc, L"the still render passes the motion check");
			const Render stOff = RenderSequence("still_control", still, frames, false, -1.0);
			const double stillDiff = diffOf(st, stOff);
			const Render al = RenderSequence("alternating_vectors", alt, frames, true, -1.0);
			const Render alOff = RenderSequence("alternating_control", alt, frames, false, -1.0);
			const double altDiff = diffOf(al, alOff);
			Logger::WriteMessage(("mean |vectors - control| codes: pan " + std::to_string(panDiff) + ", still " + std::to_string(stillDiff) +
				", alternating (negative control) " + std::to_string(altDiff) + "\n").c_str());
			Assert::IsTrue(stillDiff > 0.05, L"the vector path changes the still's output");
			Assert::IsTrue(panDiff >= 0.6 * stillDiff, L"on the pan the vectors' grain map matches the still's (it follows the motion)");
			Assert::IsTrue(panDiff >= 3.0 * altDiff, L"meaningless vectors do not reproduce it (the metric separates)");
		}
	};
}
