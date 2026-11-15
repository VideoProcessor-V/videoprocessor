// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The P7 slot byte form is CameraLink::EncodeRecordPod / DecodeRecordPod (global_model/link/CameraRecordPool.h, magic CRD1),
// the codec the producer writes (CameraFitWorker / CameraFitProducer). These tests pin what a P9 / FixedLagSmoother consumer
// reads through it (L-B4 audit MUST 1: coverage of the removed duplicate P7SlotCodec moved here):
//   1. round-trip: every field the codec carries survives; blocks / objects stay off the slot;
//   2. refusal: a short output buffer encodes nothing, a short input or a bad magic decodes nothing (the target is untouched);
//   3. smoother parity: a FixedLagSmoother fed decoded records smooths exactly like one fed the originals.
// CPU only.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelTemporal.h>
#include <VP-V/core/Products.h>
#include "CppUnitTest.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;

		CameraMotion PodRec(uint64_t seq)
		{
			CameraMotion m;
			m.seq = seq; m.pairFrom = seq - 1; m.pairTo = seq; m.pairSeconds = 1.0 / 24.0;
			m.resyncGeneration = 3; m.rebaseGeneration = 5; m.valid = true;
			m.params.type = ModelType::Similarity;
			m.params.h[0] = 1.001; m.params.h[2] = 1.5 * static_cast<double>(seq); m.params.h[5] = -0.25 * static_cast<double>(seq);
			m.smoothedParams.type = ModelType::Translation;
			m.smoothedParams.h[2] = 0.75 * static_cast<double>(seq); m.smoothedParams.h[5] = 0.125;
			m.rates.tx = 36.0 * static_cast<double>(seq); m.rates.ty = -6.0 * static_cast<double>(seq);
			m.rates.logScale = 0.01; m.rates.angle = -0.002;
			m.confidence = 0.875; m.reason = Reason::PostCutSeeded;
			m.signals.inlierShare = 0.9; m.signals.precision = 0.8; m.signals.coverage = 0.7; m.signals.temporal = 0.6;
			m.signals.conditioning = 0.5; m.signals.engineAgreement = 0.4; m.signals.inverse = 0.3; m.signals.ambiguity = 0.2;
			m.signals.logit = -1.25; m.signals.haveBackward = true; m.signals.haveEngine = false;
			m.seededFromNext = true; m.bridged = false; m.inFade = true; m.smoothed = false; m.labelsOnGpu = true;
			m.fadeGain = 0.5; m.fadeOffset = 0.125;
			m.samples = 1296; m.hypotheses = 64; m.costMicros = 321;
			m.picture.left = 0; m.picture.top = 140; m.picture.right = 3840; m.picture.bottom = 2020;
			return m;
		}

		size_t PodEncode(const CameraMotion& m, std::vector<uint8_t>& buf)
		{
			buf.assign(CameraLink::kRecordPodMaxBytes, 0);
			return CameraLink::EncodeRecordPod(m, buf.data(), buf.size());
		}
	}

	TEST_CLASS(CameraRecordPodTests)
	{
	public:
		TEST_METHOD(RoundTripKeepsEveryCarriedField)
		{
			const CameraMotion a = PodRec(42);
			std::vector<uint8_t> buf;
			const size_t n = PodEncode(a, buf);
			Assert::IsTrue(n > 0 && n <= CameraLink::kRecordPodMaxBytes, L"fits the declared 1 KB P7 slot");
			CameraMotion b;
			Assert::IsTrue(CameraLink::DecodeRecordPod(buf.data(), n, b));
			Assert::AreEqual(a.seq, b.seq); Assert::AreEqual(a.pairFrom, b.pairFrom); Assert::AreEqual(a.pairTo, b.pairTo);
			Assert::AreEqual(a.pairSeconds, b.pairSeconds);
			Assert::AreEqual(a.resyncGeneration, b.resyncGeneration); Assert::AreEqual(a.rebaseGeneration, b.rebaseGeneration);
			Assert::AreEqual(a.valid, b.valid);
			Assert::IsTrue(a.params.type == b.params.type); Assert::IsTrue(a.reason == b.reason);
			for (int i = 0; i < 9; ++i) Assert::AreEqual(a.params.h[i], b.params.h[i]);
			Assert::IsTrue(a.smoothedParams.type == b.smoothedParams.type);
			for (int i = 0; i < 9; ++i) Assert::AreEqual(a.smoothedParams.h[i], b.smoothedParams.h[i]);
			Assert::AreEqual(a.rates.tx, b.rates.tx); Assert::AreEqual(a.rates.ty, b.rates.ty);
			Assert::AreEqual(a.rates.logScale, b.rates.logScale); Assert::AreEqual(a.rates.angle, b.rates.angle);
			Assert::AreEqual(a.confidence, b.confidence);
			Assert::AreEqual(a.signals.inlierShare, b.signals.inlierShare); Assert::AreEqual(a.signals.precision, b.signals.precision);
			Assert::AreEqual(a.signals.coverage, b.signals.coverage); Assert::AreEqual(a.signals.temporal, b.signals.temporal);
			Assert::AreEqual(a.signals.conditioning, b.signals.conditioning);
			Assert::AreEqual(a.signals.engineAgreement, b.signals.engineAgreement);
			Assert::AreEqual(a.signals.inverse, b.signals.inverse); Assert::AreEqual(a.signals.ambiguity, b.signals.ambiguity);
			Assert::AreEqual(a.signals.logit, b.signals.logit);
			Assert::AreEqual(a.signals.haveBackward, b.signals.haveBackward); Assert::AreEqual(a.signals.haveEngine, b.signals.haveEngine);
			Assert::AreEqual(a.fadeGain, b.fadeGain); Assert::AreEqual(a.fadeOffset, b.fadeOffset);
			Assert::AreEqual(a.seededFromNext, b.seededFromNext); Assert::AreEqual(a.inFade, b.inFade);
			Assert::AreEqual(a.bridged, b.bridged); Assert::AreEqual(a.smoothed, b.smoothed); Assert::AreEqual(a.labelsOnGpu, b.labelsOnGpu);
			Assert::AreEqual(a.samples, b.samples); Assert::AreEqual(a.hypotheses, b.hypotheses); Assert::AreEqual(a.costMicros, b.costMicros);
			Assert::AreEqual(a.picture.left, b.picture.left); Assert::AreEqual(a.picture.top, b.picture.top);
			Assert::AreEqual(a.picture.right, b.picture.right); Assert::AreEqual(a.picture.bottom, b.picture.bottom);
			Assert::IsTrue(!b.blocks && b.objects.empty(), L"per-block arrays and objects stay in the camera store, not the slot");
		}

		TEST_METHOD(ShortBufferAndBadMagicAreRefused)
		{
			const CameraMotion a = PodRec(7);
			std::vector<uint8_t> buf;
			const size_t n = PodEncode(a, buf);
			Assert::IsTrue(n > 4);
			std::vector<uint8_t> tiny(n - 1, 0);
			Assert::AreEqual(size_t(0), CameraLink::EncodeRecordPod(a, tiny.data(), tiny.size()), L"one byte short encodes nothing");
			CameraMotion b;
			b.seq = 999;
			Assert::IsFalse(CameraLink::DecodeRecordPod(buf.data(), n - 1, b), L"short input");
			Assert::AreEqual<uint64_t>(999u, b.seq, L"a refused decode leaves the target untouched");
			std::vector<uint8_t> bad = buf;
			bad[0] ^= 0xFF;
			Assert::IsFalse(CameraLink::DecodeRecordPod(bad.data(), n, b), L"bad magic");
			Assert::AreEqual<uint64_t>(999u, b.seq);
			Assert::IsTrue(CameraLink::DecodeRecordPod(buf.data(), n, b));
			Assert::AreEqual<uint64_t>(7u, b.seq);
		}

		TEST_METHOD(SmootherOnDecodedRecordsEqualsSmootherOnOriginals)
		{
			FixedLagSmoother direct(VpSkeleton::kMaxCameraLead), viaSlot(VpSkeleton::kMaxCameraLead);
			for (uint64_t s = 10; s < 20; ++s)
			{
				CameraMotion m = PodRec(s);
				m.params.h[2] += static_cast<double>(s % 3) * 0.37;
				direct.Push(m);
				std::vector<uint8_t> buf;
				const size_t n = PodEncode(m, buf);
				Assert::IsTrue(n > 0);
				CameraMotion d;
				Assert::IsTrue(CameraLink::DecodeRecordPod(buf.data(), n, d));
				viaSlot.Push(d);
			}
			int smoothed = 0;
			for (uint64_t s = 10; s < 18; ++s)
			{
				Params x, y;
				const bool a = direct.Smoothed(s, x), b = viaSlot.Smoothed(s, y);
				Assert::AreEqual(a, b);
				if (a)
				{
					++smoothed;
					Assert::IsTrue(x.type == y.type);
					for (int i = 0; i < 9; ++i) Assert::AreEqual(x.h[i], y.h[i]);
				}
			}
			Assert::IsTrue(smoothed > 0, L"the window produced smoothed parameters (not a vacuous comparison)");
		}

		// Gate condition on WAIVER-B4-P7CODEC: a ModelType / Reason byte outside its enum is refused and the target untouched;
		// the last enumerator of each is still accepted. Byte offsets follow EncodeRecordPod's order: magic 4, seq / pairFrom /
		// pairTo / pairSeconds / resyncGeneration / rebaseGeneration 6 x 8 = 52, valid @52, params.type @53, reason @54,
		// flags @55, params.h 9 x 8 @56, smoothedParams.type @128.
		TEST_METHOD(OutOfRangeModelTypeOrReasonIsRefused)
		{
			const CameraMotion a = PodRec(21);
			std::vector<uint8_t> buf;
			const size_t n = PodEncode(a, buf);
			Assert::IsTrue(n > 129);
			Assert::AreEqual<uint8_t>(uint8_t(a.params.type), buf[53], L"offset of params.type");
			Assert::AreEqual<uint8_t>(uint8_t(a.reason), buf[54], L"offset of reason");
			Assert::AreEqual<uint8_t>(uint8_t(a.smoothedParams.type), buf[128], L"offset of smoothedParams.type");
			const size_t offsets[] = { 53, 54, 128 };
			const uint8_t lasts[] = { uint8_t(ModelType::Homography), uint8_t(Reason::Disabled), uint8_t(ModelType::Homography) };
			for (int k = 0; k < 3; ++k)
			{
				std::vector<uint8_t> bad = buf;
				bad[offsets[k]] = uint8_t(lasts[k] + 1);
				CameraMotion b;
				b.seq = 999;
				Assert::IsFalse(CameraLink::DecodeRecordPod(bad.data(), n, b), L"one past the last enumerator");
				Assert::AreEqual<uint64_t>(999u, b.seq, L"a refused decode leaves the target untouched");
				bad[offsets[k]] = 0xFF;
				Assert::IsFalse(CameraLink::DecodeRecordPod(bad.data(), n, b), L"0xFF");
				Assert::AreEqual<uint64_t>(999u, b.seq);
				bad[offsets[k]] = lasts[k];
				Assert::IsTrue(CameraLink::DecodeRecordPod(bad.data(), n, b), L"the last enumerator is accepted");
				Assert::AreEqual<uint64_t>(21u, b.seq);
			}
		}
	};
}
