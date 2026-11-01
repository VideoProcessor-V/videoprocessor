// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-2: PairInput assembly and the stamp -> generation mapping (F3). CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/PairAssembly.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(PairAssemblyTests)
	{
	public:
		static VpIndicator::Stamp Base()
		{
			VpIndicator::Stamp s;
			s.era = 7;
			for (size_t i = 0; i < size_t(VpIndicator::kCounterCount); ++i)
				s.v[i] = static_cast<uint32_t>(3 + i);
			return s;
		}

		TEST_METHOD(GenerationChangesOnlyOnFullContinuityAndMotionSettings)
		{
			using namespace VpIndicator;
			const Stamp base = Base();
			const uint64_t g0 = CameraLink::GenerationsOf(base);
			Assert::AreEqual(g0, CameraLink::GenerationsOf(base), L"deterministic");
			const Counter moves[] = { kFull, kContinuity, kSettingsMotion };
			for (Counter c : moves)
			{
				Stamp s = base;
				s.v[c] += 1;
				Assert::IsTrue(CameraLink::GenerationsOf(s) != g0, L"must change");
			}
			Stamp era = base;
			era.era += 1;
			Assert::IsTrue(CameraLink::GenerationsOf(era) != g0, L"a new era is a new generation");
			const Counter quiet[] = { kTiming, kRebaseH, kRebaseV, kGraph, kSettingsRebase, kSettingsAnalysis, kSettingsOutput };
			for (Counter c : quiet)
			{
				Stamp s = base;
				s.v[c] += 5;
				Assert::AreEqual(g0, CameraLink::GenerationsOf(s), L"must NOT change (re-base is Ignore, timing relatches)");
			}
		}

		TEST_METHOD(AncientAndSlipAreNotFitted)
		{
			CameraSynth::Field f(CameraSynth::Grid4K());
			f.FromModel(CameraSynth::PanModel(10, 0));
			auto copy = CameraLinkTest::CopyOf(f, 4);
			CameraLink::PairFacts facts;
			facts.pairSeconds = 1.0 / 24;
			CameraModel::PairInput in;
			VpIndicator::Stamp ancient = Base();
			ancient.ancient = true;
			Assert::IsTrue(CameraLink::AssemblePair(*copy, facts, ancient, in) == CameraLink::AssembleStatus::NotFittedAncient);
			facts.spansSlip = true;
			Assert::IsTrue(CameraLink::AssemblePair(*copy, facts, Base(), in) == CameraLink::AssembleStatus::NotFittedSlip);
			facts.spansSlip = false;
			CameraLink::FieldCopy empty;
			Assert::IsTrue(CameraLink::AssemblePair(empty, facts, Base(), in) == CameraLink::AssembleStatus::NoField);
			const CameraModel::CameraMotion nf = CameraLink::NotFittedRecord(4, 3, 4);
			Assert::IsFalse(nf.valid);
			Assert::IsTrue(nf.reason == CameraModel::Reason::GenerationReset);
		}

		TEST_METHOD(RectFlagsEngineGlobalAndPointersMapThrough)
		{
			CameraSynth::Field f(CameraSynth::Grid4K());
			f.FromModel(CameraSynth::PanModel(10, 2), true);
			auto copy = CameraLinkTest::CopyOf(f, 9);
			CameraLink::PairFacts facts;
			facts.pairSeconds = 0.05;
			facts.picture.left = 0; facts.picture.top = 120; facts.picture.right = 3840; facts.picture.bottom = 2040;
			facts.cut = true; facts.fade = true; facts.flash = true;
			facts.fadeGain = 0.5; facts.fadeOffset = 0.1;
			facts.haveEngineGlobal = true; facts.engineGx = 3; facts.engineGy = -4;
			facts.deadlineMicros = 12345;
			CameraModel::PairInput in;
			CameraLink::AssembleInfo info;
			const VpIndicator::Stamp st = Base();
			Assert::IsTrue(CameraLink::AssemblePair(*copy, facts, st, in, &info) == CameraLink::AssembleStatus::Ready);
			Assert::IsFalse(info.pictureWasEmpty);
			Assert::IsFalse(info.noBackward);
			Assert::IsTrue(in.picture == facts.picture);
			Assert::IsTrue(in.cut && in.fade && in.flash);
			Assert::AreEqual(0.5, in.fadeGain);
			Assert::IsTrue(in.haveEngineGlobal && in.engineGx == 3 && in.engineGy == -4);
			Assert::AreEqual(int64_t(12345), in.deadlineMicros);
			Assert::AreEqual(uint64_t(9), in.seq);
			Assert::AreEqual(uint64_t(8), in.pairFrom);
			Assert::AreEqual(uint64_t(9), in.pairTo);
			Assert::AreEqual(0.05, in.pairSeconds);
			Assert::AreEqual(uint64_t(0), in.rebaseGeneration, L"re-base generation is always 0");
			Assert::AreEqual(CameraLink::GenerationsOf(st), in.resyncGeneration);
			Assert::IsTrue(in.fwdX == copy->fwdX.data() && in.bwdY == copy->bwdY.data(), L"points into the copy, no second copy");
			Assert::IsTrue(in.textGuard == nullptr && in.texture == nullptr && in.pixelResidual == nullptr);
		}

		TEST_METHOD(EmptyPictureMeansFullFrameAndNoBackwardIsNeutral)
		{
			CameraSynth::Field f(CameraSynth::Grid4K());
			f.FromModel(CameraSynth::PanModel(10, 0), false);
			auto copy = CameraLinkTest::CopyOf(f, 3);
			CameraLink::PairFacts facts;
			facts.pairSeconds = 1.0 / 24;
			CameraModel::PairInput in;
			CameraLink::AssembleInfo info;
			Assert::IsTrue(CameraLink::AssemblePair(*copy, facts, Base(), in, &info) == CameraLink::AssembleStatus::Ready);
			Assert::IsTrue(info.pictureWasEmpty && info.noBackward);
			Assert::AreEqual(int(f.gridW * f.blockSize), in.picture.right);
			Assert::AreEqual(int(f.gridH * f.blockSize), in.picture.bottom);
			Assert::IsTrue(in.bwdX == nullptr && in.bwdY == nullptr);
			// The assembled pair fits: a pan over the full frame is Ok.
			CameraModel::Fitter fitter;
			const CameraModel::CameraMotion r = fitter.Fit(in);
			Assert::IsTrue(r.valid && r.reason == CameraModel::Reason::Ok);
		}
	};
}
