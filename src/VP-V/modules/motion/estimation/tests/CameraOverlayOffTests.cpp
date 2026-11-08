// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Camera K3 overlay switch (camera_model_overlay). The overlay itself is not built yet (no fitted labels reach an overlay stage: the
// fit has no vector engine, K2's P8 is not instantiated), so what is pinned here is the OFF contract: default off, forced off under
// the Original pathway and while the model is off, no overlay text anywhere when off, and a request
// that cannot be met is SHOWN (never silently dropped). CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/global_model/link/CameraSettings.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraStatus.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraOverlayOffTestsNs
{
	using namespace Vpv::Camera;

	bool AnyMentions(const StatusText& t, const char* part)
	{
		for (const std::vector<std::string>* v : { &t.lines, &t.fallbacks, &t.detail })
			for (const std::string& l : *v)
				if (l.find(part) != std::string::npos)
					return true;
		return false;
	}

	StatusInput Running()
	{
		StatusInput in;
		in.cam.model = true;
		in.linkReport = true;
		in.coreAlive = true;
		in.engineAvailable = true;
		in.fit.have = true;
		in.fit.type = CameraModel::ModelType::Similarity;
		in.fit.confidence = 0.9;
		in.fit.published = true;
		return in;
	}

	TEST_CLASS(CameraOverlayOffTests)
	{
	public:
		TEST_METHOD(TheOverlayIsOffByDefaultAndSaysNothing)
		{
			Assert::IsTrue(CameraSettings().overlay == OverlayMode::Off);
			Assert::IsTrue(CameraSettings().Overlay() == OverlayMode::Off);
			for (bool model : { false, true })
			{
				StatusInput in = Running();
				in.cam.model = model;
				const StatusText t = BuildStatus(in);
				Assert::IsFalse(AnyMentions(t, "overlay"), L"overlay off: no overlay text in the line, the fallbacks or Ctrl+Alt+D");
			}
		}

		TEST_METHOD(EveryOverlayModeIsOffUnderTheOriginalPathwayAndWhileTheModelIsOff)
		{
			for (OverlayMode m : { OverlayMode::Labels, OverlayMode::Residual, OverlayMode::Vectors })
			{
				CameraSettings s;
				s.model = false;   // the model is on by default
				s.overlay = m;
				Assert::IsTrue(s.Overlay() == OverlayMode::Off, L"the model is off");
				s.model = true;
				Assert::IsTrue(s.Overlay() == m);
				s.originalPathway = true;
				Assert::IsTrue(s.Overlay() == OverlayMode::Off, L"Original forces it off");
				StatusInput in = Running();
				in.cam = s;
				const StatusText t = BuildStatus(in);
				Assert::IsTrue(t.lines.empty() && t.fallbacks.empty() && t.detail.empty());
			}
		}

		TEST_METHOD(ARequestThatCannotDrawIsShownNeverSilent)
		{
			for (OverlayMode m : { OverlayMode::Labels, OverlayMode::Residual, OverlayMode::Vectors })
			{
				StatusInput in = Running();
				in.cam.overlay = m;
				const StatusText t = BuildStatus(in);
				const std::string mode = OverlayModeText(m);
				bool line = false, fallback = false;
				for (const std::string& l : t.lines)
					line = line || l.find("camera overlay: " + mode + " requested, but it cannot draw yet") != std::string::npos;
				for (const std::string& l : t.fallbacks)
					fallback = fallback || l.find("camera_model_overlay: " + mode + " requested") != std::string::npos;
				Assert::IsTrue(line, L"the status line says so");
				Assert::IsTrue(fallback, L"and so does Fallbacks engaged");
			}
		}

		TEST_METHOD(AnOverlayWhileTheModelIsOffIsShownAsInert)
		{
			StatusInput in;
			in.cam.model = false;
			in.cam.overlay = OverlayMode::Labels;
			const StatusText t = BuildStatus(in);
			Assert::IsTrue(AnyMentions(t, "camera_model_overlay=labels"));
			Assert::IsTrue(AnyMentions(t, "camera_model is off"));
		}

		TEST_METHOD(OnlyTheFourValuesParse)
		{
			Assert::IsTrue(Validate("camera_model_overlay", "off") == Verdict::Valid);
			Assert::IsTrue(Validate("camera_model_overlay", "labels") == Verdict::Valid);
			Assert::IsTrue(Validate("camera_model_overlay", "residual") == Verdict::Valid);
			Assert::IsTrue(Validate("camera_model_overlay", "vectors") == Verdict::Valid);
			Assert::IsTrue(Validate("camera_model_overlay", "on") == Verdict::Invalid);
			Assert::IsTrue(Validate("camera_model_overlay", "") == Verdict::Invalid);
		}
	};
}
