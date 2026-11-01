// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4-5: the VP_MOTION_BENCH csv writer writes exactly the documented columns; closed means no file and no cost.
#include <VP-V/modules/motion/estimation/global_model/link/MotionBenchWriter.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(MotionBenchWriterTests)
	{
	public:
		TEST_METHOD(WritesTheDocumentedHeaderAndRows)
		{
			const std::filesystem::path p = std::filesystem::temp_directory_path() / "camera-k1-bench-test.csv";
			{
				CameraLink::MotionBenchWriter w;
				w.Open(p.string().c_str());
				Assert::IsTrue(w.IsOpen());
				CameraLink::BenchRow r;
				r.pair = 12; r.seq = 10; r.switchMask = 4 | 8; r.d = 2; r.confidence = 0.91; r.reason = "ok";
				r.narrow = 100; r.full = 20; r.escalated = 3; r.skipped = 0; r.governorState = 1; r.gpuMs = 0.25;
				w.Write(r);
				r.gpuMs = -1;
				w.Write(r);
				w.Close();
				Assert::IsFalse(w.IsOpen());
				w.Write(r);   // closed: ignored
			}
			std::ifstream in(p);
			std::string l0, l1, l2, l3;
			std::getline(in, l0);
			std::getline(in, l1);
			std::getline(in, l2);
			std::getline(in, l3);
			Assert::AreEqual(std::string("pair,seq,switch_mask,d,confidence,reason,narrow,full,escalated,skipped,governor_state,gpu_ms"), l0);
			Assert::AreEqual(std::string("12,10,12,2,0.91,ok,100,20,3,0,1,0.2500"), l1);
			Assert::AreEqual(std::string("12,10,12,2,0.91,ok,100,20,3,0,1,"), l2, L"unmeasured gpu_ms is an empty cell");
			Assert::IsTrue(l3.empty(), L"nothing after Close");
			in.close();
			std::filesystem::remove(p);
		}

		TEST_METHOD(SwitchMaskBitsAreStable)
		{
			CameraModel::MeasurementSwitches s;
			Assert::AreEqual(0u, CameraLink::SwitchMask(s));
			s.modelGlobal = true; s.objects = true;
			Assert::AreEqual(1u | 1024u, CameraLink::SwitchMask(s));
			Assert::AreEqual(std::string("motion.guided_pass"), std::string(CameraLink::kTimerGuidedPass));
		}
	};
}
