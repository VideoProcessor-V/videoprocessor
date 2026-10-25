/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// WP-14 (2026-10-25): the DV glue moved out of the renderer host (DvHostStage.h,
// DvGpuUnpackSession.h). CPU only, no GPU device: the fallback entries each hook point engages
// and releases, the ingress status tail, the tunnel deband skips, the Dolby-style curve switch
// and its fallback, the trims' states (idle, calibration LUT, shader missing) and the carry of
// "shader missing" into the next frame's curve decision, the GPU unpack session's log/fallback
// rules. The GPU sides (trim shader parity, the unpack itself) stay with the DV GPU checks.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/colour/sdr_tone_shape/DvHostStage.h>
#include <VP-V/modules/input/signal/DvGpuUnpackSession.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvHost
	{
		// The fallback registry as the host sees it: id -> reason while engaged.
		struct Registry
		{
			std::map<std::string, std::string> engaged;
			std::vector<std::string> released;
			DvHostStage::Engage Engage() { return [this](const char* id, const std::string& r) { engaged[id] = r; }; }
			DvHostStage::Release Release() { return [this](const char* id) { engaged.erase(id); released.push_back(id); }; }
			bool Has(const char* id) const { return engaged.count(id) != 0; }
		};

		// A loader whose shader files never load (no GPU, no shaders folder in the test).
		inline CustomHookChain::Loader NoShaders()
		{
			return {
				[](const std::string&, std::string&, std::string& reason) { reason = "not found (test)"; return false; },
				[](const std::string&) -> const pl_hook* { return nullptr; },
				[](const pl_hook*& hook) { hook = nullptr; } };
		}

		inline DolbyVisionTunnelFrameMetadata TrimmedDm()
		{
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = 3696;
			dm.hasSceneMetadata = true;
			dm.sceneMinPq = 62;
			dm.sceneAvgPq = 1500;
			dm.sceneMaxPq = 3000;
			DolbyVisionTunnelFrameMetadata::Trim t;
			t.level = 2;
			t.targetMaxPq = 2081;   // ~100 nits
			t.slope = 2400;
			t.offset = 2048;
			t.power = 2048;
			t.chromaWeight = 2048;
			t.saturationGain = 2048;
			dm.trims[dm.trimCount++] = t;
			return dm;
		}

		inline pl_frame Picture(bool dolbyVision)
		{
			pl_frame f{};
			f.repr = pl_color_repr_unknown;
			f.repr.sys = dolbyVision ? PL_COLOR_SYSTEM_DOLBYVISION : PL_COLOR_SYSTEM_BT_2020_NC;
			f.color = pl_color_space_hdr10;
			return f;
		}

		inline pl_frame Target(enum pl_color_transfer trc, float peakNits)
		{
			pl_frame f{};
			f.color.primaries = PL_COLOR_PRIM_BT_2020;
			f.color.transfer = trc;
			f.color.hdr.max_luma = peakNits;
			return f;
		}

		struct Run
		{
			std::string mode = "libplacebo";
			bool trims = true;
			std::string curve = "bt2390";
			std::string folder;
			bool colourRuns = true;
			VideoState state;

			void Frame(DvHostStage& stage, const pl_frame& image, const pl_frame& target,
				const DolbyVisionTunnelFrameMetadata* dm, pl_color_map_params& params, std::vector<const pl_hook*>& hooks)
			{
				CustomHookChain::TemplateValues templates;
				stage.OnPreOutput({ mode, trims, curve, folder, colourRuns, image, target, dm, 2.4, state, nullptr, 1 },
					params, templates, hooks);
			}
		};
	}

	TEST_CLASS(DvHostStageTests)
	{
	public:
		TEST_METHOD(MetadataHoldEngagesAndOutsideATunnelEverythingIsReleased)
		{
			using namespace DvHost;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			DolbyVisionTunnelFrameMetadata dm = TrimmedDm();
			dm.reusedPreviousDm = true;
			dm.dmHoldPhase = static_cast<uint8_t>(DvMetadataHold::Phase::Held);
			stage.OnTunnelState(true, &dm);
			Assert::IsTrue(reg.Has("DV metadata"));
			Assert::AreEqual(std::string("packet rejected: the previous picture's DM held"), reg.engaged["DV metadata"]);
			stage.OnTunnelState(true, nullptr);   // no payload: released
			Assert::IsFalse(reg.Has("DV metadata"));
			reg.released.clear();
			stage.OnTunnelState(false, &dm);       // outside a tunnel: the tunnel's entries released
			Assert::IsFalse(reg.Has("DV metadata"));
			for (const char* id : { "Dolby Vision metadata", "Dolby Vision source range", "Dolby Vision scene metadata", "Dolby Vision trims" })
				Assert::IsTrue(std::find(reg.released.begin(), reg.released.end(), id) != reg.released.end());
		}

		TEST_METHOD(DebandSkipsOnlyOnTunnelPicturesAndShowInTheIngressLine)
		{
			using namespace DvHost;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			DolbyVisionTunnelFrameMetadata dm;
			Assert::AreEqual(std::string("; DV trims: off; DV tone mapping: libplacebo"), stage.IngressSuffix(dm));
			std::string debandState;
			Assert::IsFalse(stage.SkipVpDeband(false, true, debandState));
			Assert::IsTrue(debandState.empty());
			Assert::IsFalse(reg.Has("VP deband"));
			Assert::IsFalse(stage.SkipVpDeband(true, false, debandState));   // VP deband off: nothing to skip
			Assert::IsTrue(stage.SkipVpDeband(true, true, debandState));
			Assert::IsTrue(reg.Has("VP deband"));
			Assert::IsFalse(debandState.empty());
			Assert::IsFalse(stage.SkipPlaneDeband(true, false));   // no deband requested
			Assert::IsFalse(stage.SkipPlaneDeband(false, true));
			Assert::IsTrue(stage.SkipPlaneDeband(true, true));
			const std::string tail = stage.IngressSuffix(dm);
			Assert::IsTrue(tail.find("libplacebo and VP deband off on the tunnel") != std::string::npos);
			Assert::IsFalse(stage.SkipVpDeband(false, true, debandState));
			Assert::IsFalse(reg.Has("VP deband"));
		}

		TEST_METHOD(NonDolbyVisionPictureLeavesTheCurveAndIdlesTheTrims)
		{
			using namespace DvHost;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			Run run;
			run.mode = "dolby_style";
			pl_color_map_params params = pl_color_map_default_params;
			params.tone_mapping_function = &pl_tone_map_spline;
			std::vector<const pl_hook*> hooks;
			const DolbyVisionTunnelFrameMetadata dm = TrimmedDm();
			run.Frame(stage, Picture(false), Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsTrue(params.tone_mapping_function == &pl_tone_map_spline, L"no DV picture: the configured curve");
			Assert::IsTrue(hooks.empty());
			Assert::IsFalse(reg.Has("DV tone mapping"));
			Assert::IsFalse(reg.Has("Dolby Vision trims"));
			Assert::AreEqual(std::string("DV trims: none"), stage.TrimsStatus());
		}

		TEST_METHOD(DolbyStyleRunsOnADolbyVisionPictureAndFallsBackVisiblyWithALut)
		{
			using namespace DvHost;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			Run run;
			run.mode = "dolby_style";
			run.trims = false;
			pl_color_map_params params = pl_color_map_default_params;
			params.tone_mapping_function = &pl_tone_map_spline;
			std::vector<const pl_hook*> hooks;
			const DolbyVisionTunnelFrameMetadata dm = TrimmedDm();
			const pl_frame image = Picture(true);
			run.Frame(stage, image, Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsTrue(params.tone_mapping_function == &DvDolbyStyle::Function(), L"the Dolby-style curve");
			Assert::IsFalse(reg.Has("DV tone mapping"));
			// A calibration LUT: bt2390, listed.
			pl_frame lutTarget = Target(PL_COLOR_TRC_PQ, 100.0f);
			static const int dummy = 0;
			lutTarget.lut = reinterpret_cast<const pl_custom_lut*>(&dummy);
			hooks.clear();
			run.Frame(stage, image, lutTarget, &dm, params, hooks);
			Assert::IsTrue(params.tone_mapping_function == &pl_tone_map_bt2390);
			Assert::IsTrue(reg.Has("DV tone mapping"));
			Assert::AreEqual(std::string("a calibration LUT is active: bt2390"), reg.engaged["DV tone mapping"]);
			// libplacebo mode again: the configured curve comes back, the entry goes.
			run.mode = "libplacebo";
			run.Frame(stage, image, Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsTrue(params.tone_mapping_function == &pl_tone_map_spline);
			Assert::IsFalse(reg.Has("DV tone mapping"));
		}

		TEST_METHOD(TrimsListTheirFallbacksAndAMissingShaderStopsTheDolbyStyleCurve)
		{
			using namespace DvHost;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			Run run;
			pl_color_map_params params = pl_color_map_default_params;
			std::vector<const pl_hook*> hooks;
			const DolbyVisionTunnelFrameMetadata dm = TrimmedDm();
			const pl_frame image = Picture(true);
			// A calibration LUT: the trims cannot act - listed with the reason.
			pl_frame lutTarget = Target(PL_COLOR_TRC_PQ, 100.0f);
			static const int dummy = 0;
			lutTarget.lut = reinterpret_cast<const pl_custom_lut*>(&dummy);
			run.Frame(stage, image, lutTarget, &dm, params, hooks);
			Assert::IsTrue(reg.Has("Dolby Vision trims"));
			Assert::AreEqual(std::string("off with a calibration LUT (it would act after the LUT)"), reg.engaged["Dolby Vision trims"]);
			// PQ output, no LUT: the L2 trim applies, but the shader does not load here.
			hooks.clear();
			run.Frame(stage, image, Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsTrue(hooks.empty());
			Assert::IsTrue(reg.Has("Dolby Vision trims"));
			Assert::AreEqual(0, reg.engaged["Dolby Vision trims"].compare(0, 11, "NOT applied"));
			Assert::AreEqual(std::string("DV trims: L2"), stage.TrimsStatus());
			const DvDolbyStyle::ReferenceConditions& rc = stage.ReferenceConditions();
			Assert::IsFalse(rc.dolbyStyleCurve);
			Assert::AreEqual(std::string("bt2390"), rc.curve);
			// The next frame asks for the Dolby-style curve: the missing trims shader stops it, visibly.
			run.mode = "dolby_style";
			run.Frame(stage, image, Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsTrue(reg.Has("DV tone mapping"));
			Assert::AreEqual(std::string("shaders/DV trims.glsl did not load: bt2390 + trims"), reg.engaged["DV tone mapping"]);
			Assert::IsTrue(params.tone_mapping_function == &pl_tone_map_bt2390);
			// Trims off: the trims entry is released and the status says off.
			run.trims = false;
			run.mode = "libplacebo";
			run.Frame(stage, image, Target(PL_COLOR_TRC_PQ, 100.0f), &dm, params, hooks);
			Assert::IsFalse(reg.Has("Dolby Vision trims"));
			Assert::AreEqual(std::string("DV trims: off"), stage.TrimsStatus());
		}

		TEST_METHOD(GpuUnpackSessionLogsOnceAndSwitchesTheSessionToTheCpuOnce)
		{
			DvGpuUnpackSession session;
			std::atomic<bool> failed{ false };
			int engaged = 0;
			std::string reason;
			const auto engage = [&](const char* id, const std::string& r) { ++engaged; reason = std::string(id) + ": " + r; };
			GpuDolbyVisionTunnelUnpack& unpack = session.Unpacker(nullptr, nullptr);   // no GPU: every unpack fails
			Assert::IsTrue(&unpack == &session.Unpacker(nullptr, nullptr), L"one unpack object per device");
			pl_tex planes[2] = {};
			const uint32_t pixels[4] = {};
			const bool unpacked = unpack.Unpack(VideoFrameEncoding::R210, pixels, 8, 2, 2, false, planes);
			Assert::IsFalse(unpacked);
			Assert::IsTrue(session.Finish(true, 2, 2, false, true, failed, engage), L"a success passes through");
			Assert::IsFalse(session.Finish(unpacked, 2, 2, false, false, failed, engage), L"a de-judder frame: skipped");
			Assert::IsFalse(failed.load());
			Assert::AreEqual(0, engaged);
			Assert::IsFalse(session.Finish(unpacked, 2, 2, false, true, failed, engage));
			Assert::IsTrue(failed.load());
			Assert::AreEqual(1, engaged);
			Assert::AreEqual(std::string("Dolby Vision GPU unpack: CPU unpack for the session: ") + unpack.Error(), reason);
			Assert::IsFalse(session.Finish(unpacked, 2, 2, false, true, failed, engage));
			Assert::AreEqual(1, engaged, L"engaged once per session");
			session.Reset();
			Assert::IsTrue(session.Unpacker(nullptr, nullptr).Error().empty(), L"a new unpack object after the reset");
		}
	};
}
