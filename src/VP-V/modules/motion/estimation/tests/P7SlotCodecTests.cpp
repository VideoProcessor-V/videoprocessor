// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P7 slot codec (L-B4): the byte form round-trips every field a P9 consumer reads, and a smoother fed with decoded
// records gives the same smoothed parameters as one fed with the originals. CPU only.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/P7SlotCodec.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelTemporal.h>
#include <VP-V/core/Products.h>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;

		CameraMotion Rec(uint64_t seq)
		{
			CameraMotion m;
			m.seq = seq; m.pairFrom = seq - 1; m.pairTo = seq; m.pairSeconds = 1.0 / 24.0;
			m.resyncGeneration = 3; m.rebaseGeneration = 5; m.valid = true;
			m.params.type = ModelType::Similarity;
			m.params.h[2] = 1.5 * static_cast<double>(seq); m.params.h[5] = -0.25 * static_cast<double>(seq); m.params.h[0] = 1.001;
			m.rates.tx = 36.0 * static_cast<double>(seq); m.rates.ty = -6.0 * static_cast<double>(seq); m.rates.logScale = 0.01; m.rates.angle = -0.002;
			m.confidence = 0.875; m.reason = Reason::PostCutSeeded;
			m.seededFromNext = true; m.bridged = false; m.inFade = true; m.smoothed = false;
			m.fadeGain = 0.5; m.fadeOffset = 0.125;
			m.picture.left = 0; m.picture.top = 140; m.picture.right = 3840; m.picture.bottom = 2020;
			return m;
		}
	}

	TEST_CLASS(P7SlotCodecTests)
	{
	public:
		TEST_METHOD(RoundTripKeepsEveryFieldASmootherReads)
		{
			const CameraMotion a = Rec(42);
			std::vector<uint8_t> buf(P7Slot::kBytes);
			Assert::AreEqual(static_cast<size_t>(P7Slot::kBytes), P7Slot::Encode(a, buf.data(), buf.size()));
			CameraMotion b;
			Assert::IsTrue(P7Slot::Decode(buf.data(), buf.size(), b));
			Assert::AreEqual(a.seq, b.seq); Assert::AreEqual(a.pairFrom, b.pairFrom); Assert::AreEqual(a.pairTo, b.pairTo);
			Assert::AreEqual(a.pairSeconds, b.pairSeconds);
			Assert::AreEqual(a.resyncGeneration, b.resyncGeneration); Assert::AreEqual(a.rebaseGeneration, b.rebaseGeneration);
			Assert::AreEqual(a.valid, b.valid);
			Assert::IsTrue(a.params.type == b.params.type); Assert::IsTrue(a.reason == b.reason);
			for (int i = 0; i < 9; ++i) Assert::AreEqual(a.params.h[i], b.params.h[i]);
			Assert::AreEqual(a.rates.tx, b.rates.tx); Assert::AreEqual(a.rates.ty, b.rates.ty);
			Assert::AreEqual(a.rates.logScale, b.rates.logScale); Assert::AreEqual(a.rates.angle, b.rates.angle);
			Assert::AreEqual(a.confidence, b.confidence); Assert::AreEqual(a.fadeGain, b.fadeGain); Assert::AreEqual(a.fadeOffset, b.fadeOffset);
			Assert::AreEqual(a.seededFromNext, b.seededFromNext); Assert::AreEqual(a.inFade, b.inFade);
			Assert::AreEqual(a.bridged, b.bridged); Assert::AreEqual(a.smoothed, b.smoothed);
			Assert::AreEqual(a.picture.top, b.picture.top); Assert::AreEqual(a.picture.right, b.picture.right);
			Assert::AreEqual(a.picture.bottom, b.picture.bottom); Assert::AreEqual(a.picture.left, b.picture.left);
			Assert::IsTrue(!b.blocks && b.objects.empty());
		}

		TEST_METHOD(ShortBufferVersionAndEnumAreRefused)
		{
			const CameraMotion a = Rec(7);
			std::vector<uint8_t> buf(P7Slot::kBytes);
			Assert::AreEqual(static_cast<size_t>(0), P7Slot::Encode(a, buf.data(), P7Slot::kBytes - 1));
			Assert::AreEqual(static_cast<size_t>(P7Slot::kBytes), P7Slot::Encode(a, buf.data(), buf.size()));
			CameraMotion b;
			Assert::IsFalse(P7Slot::Decode(buf.data(), P7Slot::kBytes - 1, b));
			std::vector<uint8_t> bad = buf; bad[0] = 99;
			Assert::IsFalse(P7Slot::Decode(bad.data(), bad.size(), b));
			bad = buf; bad[2] = 200;
			Assert::IsFalse(P7Slot::Decode(bad.data(), bad.size(), b));
		}

		TEST_METHOD(SmootherOnDecodedRecordsEqualsSmootherOnOriginals)
		{
			FixedLagSmoother direct(VpSkeleton::kMaxCameraLead), viaSlot(VpSkeleton::kMaxCameraLead);
			for (uint64_t s = 10; s < 20; ++s)
			{
				CameraMotion m = Rec(s);
				m.params.h[2] += (s % 3) * 0.37;
				direct.Push(m);
				std::vector<uint8_t> buf(P7Slot::kBytes);
				P7Slot::Encode(m, buf.data(), buf.size());
				CameraMotion d;
				Assert::IsTrue(P7Slot::Decode(buf.data(), buf.size(), d));
				viaSlot.Push(d);
			}
			for (uint64_t s = 10; s < 18; ++s)
			{
				Params x, y;
				const bool a = direct.Smoothed(s, x), b = viaSlot.Smoothed(s, y);
				Assert::AreEqual(a, b);
				if (a) for (int i = 0; i < 9; ++i) Assert::AreEqual(x.h[i], y.h[i]);
			}
		}
	};
}
