// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// CameraSettings (link/CameraSettings.h): the [vpv.camera] keys of camera K3. Every default is off except camera_model (camera defaults (b): on), the Original pathway forces
// every key off, an invalid value keeps the default and is listed, unknown keys are listed and never rewritten, the file text
// round-trips, and the switches are inert unless camera_model is on. CPU only, no GPU.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/global_model/link/CameraSettings.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraSettingsTestsNs
{
	using namespace Vpv::Camera;

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-camera-settings-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << text;
		}
		~TempCfg() { std::remove(path.c_str()); }
		CameraSettings Load() const
		{
			const std::string p = path;
			return Vpv::Camera::Load([p](ConfigFile& c) { return c.Load(p, ConfigFile::ReadPolicy::Fresh); });
		}
	};

	// Every boolean key set to `on`, the overlay to `vectors`.
	CameraSettings AllOn()
	{
		CameraSettings s;
		s.model = true;
		s.overlay = OverlayMode::Vectors;
		s.labelsGpu = true;
		s.meas.modelGlobal = s.meas.guidedCandidate = s.meas.narrowWindow = s.meas.e1Skip = s.meas.staticGate = true;
		s.meas.cutSkip = s.meas.fadeNormalised = s.meas.flashBridge = s.meas.temporalSeed = s.meas.tinyMoverRoi = true;
		s.use.grain = s.use.detail = s.use.dither = s.use.deband = s.use.filler = s.use.dejudder = true;
		return s;
	}

	TEST_CLASS(CameraSettingsTests)
	{
	public:
		TEST_METHOD(EveryDefaultIsOffExceptTheModel)
		{
			const CameraSettings s;
			Assert::IsTrue(s.model, L"camera defaults (b): the model is on by default");
			Assert::IsTrue(s.overlay == OverlayMode::Off);
			Assert::IsFalse(s.labelsGpu);
			Assert::IsFalse(s.meas.Any());
			Assert::IsFalse(s.use.Any());
			Assert::IsTrue(s.AnySet());
			Assert::IsTrue(s.Wanted());
			for (const auto& kv : KeyValues(s))
				Assert::AreEqual(std::string(kv.first == "camera_model" ? "on" : "off"), kv.second, std::wstring(kv.first.begin(), kv.first.end()).c_str());
			// A file without the section, and an unreadable file, both give the defaults.
			Assert::IsTrue(TempCfg("[general]\nx=1\n").Load() == s);
			Assert::IsTrue(Vpv::Camera::Load([](ConfigFile&) { return false; }) == s);
		}

		TEST_METHOD(TheKeyListIsComplete)
		{
			// 1 + 1 + 1 + 10 measurement switches + 6 consumers; every key validates, and a foreign key is not ours.
			Assert::AreEqual<size_t>(19, kCameraKeyCount);
			std::set<std::string> seen;
			for (size_t i = 0; i < kCameraKeyCount; ++i)
			{
				const std::string key = kCameraKeys[i];
				Assert::IsTrue(seen.insert(key).second, L"duplicate key");
				Assert::IsTrue(Validate(key, key == "camera_model_overlay" ? "labels" : "on") == Verdict::Valid, std::wstring(key.begin(), key.end()).c_str());
				Assert::IsTrue(Validate(key, "sideways") == Verdict::Invalid);
			}
			Assert::IsTrue(Validate("camera_link", "report") == Verdict::NotMine, L"camera_link stays in [vpv.skeleton]");
			Assert::IsTrue(Validate("camera_meas_objects", "on") == Verdict::NotMine, L"objects has no key: it stays off");
			Assert::AreEqual<size_t>(kCameraKeyCount, KeyValues(CameraSettings()).size());
		}

		TEST_METHOD(EveryMeasurementSwitchHasAKeyAndNothingElseIsSet)
		{
			// All 10 camera_meas_* keys on sets every member of MeasurementSwitches except `objects`.
			std::string text = "[vpv.camera]\n";
			for (size_t i = 0; i < kCameraKeyCount; ++i)
				if (std::string(kCameraKeys[i]).compare(0, 12, "camera_meas_") == 0)
					text += std::string(kCameraKeys[i]) + ": on\n";
			const CameraSettings s = TempCfg(text).Load();
			const CameraModel::MeasurementSwitches& m = s.meas;
			Assert::IsTrue(m.modelGlobal && m.guidedCandidate && m.narrowWindow && m.e1Skip && m.staticGate);
			Assert::IsTrue(m.cutSkip && m.fadeNormalised && m.flashBridge && m.temporalSeed && m.tinyMoverRoi);
			Assert::IsFalse(m.objects, L"objects has no key and stays off");
			Assert::IsTrue(s.model, L"the default (on) is untouched");
			Assert::IsFalse(s.use.Any());
			Assert::IsTrue(s.problems.empty());
			// Each key alone sets exactly its own member.
			int alone = 0;
			for (size_t i = 0; i < kCameraKeyCount; ++i)
			{
				const std::string key = kCameraKeys[i];
				if (key.compare(0, 12, "camera_meas_") != 0)
					continue;
				const CameraSettings one = TempCfg("[vpv.camera]\n" + key + ": on\n").Load();
				const uint32_t mask = (one.meas.modelGlobal ? 1u : 0u) | (one.meas.guidedCandidate ? 2u : 0u) | (one.meas.narrowWindow ? 4u : 0u) |
					(one.meas.e1Skip ? 8u : 0u) | (one.meas.staticGate ? 16u : 0u) | (one.meas.cutSkip ? 32u : 0u) |
					(one.meas.fadeNormalised ? 64u : 0u) | (one.meas.flashBridge ? 128u : 0u) | (one.meas.temporalSeed ? 256u : 0u) |
					(one.meas.tinyMoverRoi ? 512u : 0u);
				Assert::IsTrue(mask != 0 && (mask & (mask - 1)) == 0, L"exactly one member");
				++alone;
			}
			Assert::AreEqual(10, alone);
		}

		TEST_METHOD(EachConsumerKeyReachesItsFlag)
		{
			struct Case { const char* key; bool UseSwitches::* member; };
			const Case cases[] = { { "camera_use_grain", &UseSwitches::grain }, { "camera_use_detail", &UseSwitches::detail },
				{ "camera_use_dither", &UseSwitches::dither }, { "camera_use_deband", &UseSwitches::deband },
				{ "camera_use_filler", &UseSwitches::filler }, { "camera_use_dejudder", &UseSwitches::dejudder } };
			for (const Case& c : cases)
			{
				const CameraSettings s = TempCfg(std::string("[vpv.camera]\n") + c.key + ": on\n").Load();
				Assert::IsTrue(s.use.*(c.member), std::wstring(c.key, c.key + std::strlen(c.key)).c_str());
				Assert::IsTrue(s.use.Any());
				for (const Case& other : cases)
					if (&other != &c)
						Assert::IsFalse(s.use.*(other.member));
			}
		}

		TEST_METHOD(TheFileTextRoundTrips)
		{
			const CameraSettings all = AllOn();
			Assert::IsTrue(TempCfg(ToConfigText(all)).Load() == all);
			CameraSettings mixed;
			mixed.model = true;
			mixed.overlay = OverlayMode::Residual;
			mixed.meas.guidedCandidate = true;
			mixed.meas.staticGate = true;
			mixed.use.grain = true;
			mixed.use.dejudder = true;
			const CameraSettings back = TempCfg(ToConfigText(mixed)).Load();
			Assert::IsTrue(back == mixed);
			Assert::IsTrue(back.overlay == OverlayMode::Residual);
			Assert::IsFalse(back.labelsGpu);
			Assert::IsFalse(back.use.detail);
			Assert::AreEqual(ToConfigText(mixed), ToConfigText(back));
			Assert::IsTrue(back.problems.empty() && back.unknownKeys.empty());
			// The defaults serialise (model on, the rest off) and read back as defaults.
			Assert::IsTrue(TempCfg(ToConfigText(CameraSettings())).Load() == CameraSettings());
		}

		TEST_METHOD(CaseAndBlanksAreTolerated)
		{
			const CameraSettings s = TempCfg("[VPV.Camera]\nCamera_Model :  ON \ncamera_model_overlay: Labels\n").Load();
			Assert::IsTrue(s.model);
			Assert::IsTrue(s.overlay == OverlayMode::Labels);
			Assert::IsTrue(s.problems.empty());
		}

		TEST_METHOD(AnInvalidValueKeepsTheDefaultAndIsListed)
		{
			const CameraSettings s = TempCfg("[vpv.camera]\ncamera_model: maybe\ncamera_model_overlay: sideways\ncamera_use_grain: on\n").Load();
			Assert::IsTrue(s.model, L"kept at the default (on)");
			Assert::IsTrue(s.overlay == OverlayMode::Off, L"kept off");
			Assert::IsTrue(s.use.grain, L"a valid key beside an invalid one still applies");
			Assert::AreEqual<size_t>(2, s.problems.size());
			Assert::IsTrue(s.problems[0].find("camera_model 'maybe' is not valid (on|off)") != std::string::npos);
			Assert::IsTrue(s.problems[1].find("camera_model_overlay 'sideways' is not valid (off|labels|residual|vectors)") != std::string::npos);
		}

		TEST_METHOD(UnknownKeysAreListedAndLeftAlone)
		{
			const std::string text = "[vpv.camera]\ncamera_model: on\ncamera_future_key: 7\ncamera_use_grain: on\n";
			const TempCfg cfg(text);
			const CameraSettings s = cfg.Load();
			Assert::IsTrue(s.model && s.use.grain, L"the known keys apply");
			Assert::AreEqual<size_t>(1, s.unknownKeys.size());
			Assert::AreEqual(std::string("camera_future_key"), s.unknownKeys[0]);
			Assert::IsTrue(s.problems.empty(), L"an unknown key is a note, not an invalid value");
			// Reading never rewrites the file: its bytes are unchanged.
			std::ifstream in(cfg.path, std::ios::binary);
			const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			Assert::AreEqual(text, after);
		}

		TEST_METHOD(TheOriginalPathwayForcesEverythingOff)
		{
			CameraSettings s = AllOn();
			s.problems.push_back("camera_model 'x' is not valid (on|off); kept on");
			s.originalPathway = true;
			Assert::IsFalse(s.ModelWanted());
			Assert::IsFalse(s.Wanted());
			Assert::IsFalse(s.Measurement().Any());
			Assert::IsFalse(s.Use().Any());
			Assert::IsTrue(s.Overlay() == OverlayMode::Off);
			Assert::IsFalse(s.LabelsGpu());
			const CameraSettings e = s.Effective();
			Assert::IsTrue(e.originalPathway);
			Assert::IsFalse(e.AnySet());
			Assert::IsTrue(e.problems.empty(), L"nothing is shown under the Original pathway");
		}

		TEST_METHOD(SwitchesAreInertWhileTheModelIsOff)
		{
			CameraSettings s = AllOn();
			s.model = false;
			Assert::IsTrue(s.AnySet());
			Assert::IsTrue(s.Wanted(), L"the status must show a set switch that cannot act");
			Assert::IsFalse(s.ModelWanted());
			Assert::IsFalse(s.Measurement().Any());
			Assert::IsFalse(s.Use().Any());
			Assert::IsTrue(s.Overlay() == OverlayMode::Off);
			Assert::IsFalse(s.LabelsGpu());
			s.model = true;
			Assert::IsTrue(s.Measurement().Any());
			Assert::IsTrue(s.Use().grain && s.Use().dejudder);
			Assert::IsTrue(s.Overlay() == OverlayMode::Vectors);
			Assert::IsTrue(s.LabelsGpu());
		}
	};
}
