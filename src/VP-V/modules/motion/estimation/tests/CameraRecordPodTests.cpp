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

#include <cstring>
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

		// GEM-137 (AB1#1) + CE-WATER / C4: CRD2 carries verdictFlags, layerFlags, finalRefineCells, dyn/water/static/two-layer
		// signals, layerRisk, haveObjectLayer, objectLayer, plus the 6 appended CE-WATER/C4 signal doubles (dynCurvature,
		// dynRhoTemp, rawShare, shearEnergy, dipoleTB, dipoleLR; total 595 B) while still decoding legacy 366-byte CRD1 payloads
		// and refusing corrupt magic, truncated CRD2 tails, or out-of-range objectLayer.type without touching target.
		TEST_METHOD(Crd2PreservesVerdictFlagsSignalsAndDecodesLegacyCrd1)
		{
			constexpr size_t kCrd1Bytes = 366;
			constexpr size_t kCrd2Bytes = 595;
			constexpr size_t kObjectLayerTypeOffset = 454;

			CameraMotion a = PodRec(108);
			a.reason = Reason::Ambiguous;
			a.verdictFlags = static_cast<uint8_t>(kVerdictDynamicTexture | kVerdictStaticCamera);
			a.layerFlags = 0x57u;
			a.finalRefineCells = 14400u;
			a.signals.dynShare = 0.625;
			a.signals.dynTemporal = 0.7125;
			a.signals.dynSpatial = 0.8375;
			a.signals.dynField = 0.4375;
			a.signals.dynCurvature = 0.5625;
			a.signals.dynRhoTemp = -0.375;
			a.signals.waterExcludedShare = 0.3125;
			a.signals.rawShare = 0.8125;
			a.signals.shearEnergy = 1.625;
			a.signals.dipoleTB = 0.9375;
			a.signals.dipoleLR = 0.28125;
			a.signals.staticRing = 0.775;
			a.signals.staticFit = 0.225;
			a.signals.border = 0.825;
			a.signals.continuity = 0.9125;
			a.layerRisk = 0.6875;
			a.haveObjectLayer = true;
			a.objectLayer.id = 7u;
			a.objectLayer.type = ModelType::Affine;
			const double objH[9] = { 1.02, -0.015, 18.5, 0.012, 0.985, -9.25, 0.0, 0.0, 1.0 };
			for (int i = 0; i < 9; ++i) a.objectLayer.h[i] = objH[i];
			a.objectLayer.blocks = 312u;
			a.objectLayer.cx = -142.5;
			a.objectLayer.cy = 88.25;

			std::vector<uint8_t> buf;
			const size_t n = PodEncode(a, buf);
			Assert::AreEqual(kCrd2Bytes, n, L"CRD2 encoded byte length");
			Assert::IsTrue(n <= CameraLink::kRecordPodMaxBytes, L"CRD2 fits the 1 KB P7 slot");
			uint32_t magic = 0;
			std::memcpy(&magic, buf.data(), sizeof(magic));
			Assert::AreEqual<uint32_t>(CameraLink::kRecordPodMagic, magic, L"EncodeRecordPod writes CRD2 magic");
			Assert::AreEqual<uint32_t>(0x43524432u, magic, L"CRD2 magic value");

			// Encode must refuse buffers shorter than CRD2 (including exact CRD1 size: never silently downgrade).
			std::vector<uint8_t> shortOut(kCrd2Bytes - 1, 0);
			Assert::AreEqual(size_t(0), CameraLink::EncodeRecordPod(a, shortOut.data(), shortOut.size()), L"1 byte short of CRD2 encodes nothing");
			Assert::AreEqual(size_t(0), CameraLink::EncodeRecordPod(a, shortOut.data(), kCrd1Bytes), L"CRD1-sized buffer must not silently truncate CRD2");

			// (a) Full CRD2 round-trip into a pre-dirtied target preserves every v2 field and v1 prefix.
			CameraMotion b;
			b.seq = 55555u;
			b.verdictFlags = 0u;
			b.signals.border = 0.1;
			b.signals.dynCurvature = 0.99;
			b.signals.dynRhoTemp = 0.99;
			b.signals.rawShare = 0.01;
			b.signals.shearEnergy = 9.0;
			b.signals.dipoleTB = 9.0;
			b.signals.dipoleLR = 9.0;
			b.haveObjectLayer = false;
			Assert::IsTrue(CameraLink::DecodeRecordPod(buf.data(), n, b), L"CRD2 decode succeeds");
			Assert::AreEqual(a.seq, b.seq);
			Assert::IsTrue(a.reason == b.reason);
			Assert::AreEqual(a.confidence, b.confidence);
			Assert::AreEqual(a.verdictFlags, b.verdictFlags);
			Assert::AreEqual(a.layerFlags, b.layerFlags);
			Assert::AreEqual(a.finalRefineCells, b.finalRefineCells);
			Assert::AreEqual(a.signals.dynShare, b.signals.dynShare);
			Assert::AreEqual(a.signals.dynTemporal, b.signals.dynTemporal);
			Assert::AreEqual(a.signals.dynSpatial, b.signals.dynSpatial);
			Assert::AreEqual(a.signals.dynField, b.signals.dynField);
			Assert::AreEqual(a.signals.dynCurvature, b.signals.dynCurvature);
			Assert::AreEqual(a.signals.dynRhoTemp, b.signals.dynRhoTemp);
			Assert::AreEqual(a.signals.waterExcludedShare, b.signals.waterExcludedShare);
			Assert::AreEqual(a.signals.rawShare, b.signals.rawShare);
			Assert::AreEqual(a.signals.shearEnergy, b.signals.shearEnergy);
			Assert::AreEqual(a.signals.dipoleTB, b.signals.dipoleTB);
			Assert::AreEqual(a.signals.dipoleLR, b.signals.dipoleLR);
			Assert::AreEqual(a.signals.staticRing, b.signals.staticRing);
			Assert::AreEqual(a.signals.staticFit, b.signals.staticFit);
			Assert::AreEqual(a.signals.border, b.signals.border);
			Assert::AreEqual(a.signals.continuity, b.signals.continuity);
			Assert::AreEqual(a.layerRisk, b.layerRisk);
			Assert::AreEqual(a.haveObjectLayer, b.haveObjectLayer);
			Assert::AreEqual(a.objectLayer.id, b.objectLayer.id);
			Assert::IsTrue(a.objectLayer.type == b.objectLayer.type);
			for (int i = 0; i < 9; ++i)
				Assert::AreEqual(a.objectLayer.h[i], b.objectLayer.h[i]);
			Assert::AreEqual(a.objectLayer.blocks, b.objectLayer.blocks);
			Assert::AreEqual(a.objectLayer.cx, b.objectLayer.cx);
			Assert::AreEqual(a.objectLayer.cy, b.objectLayer.cy);

			// (b) Backward compatibility: legacy 366-byte CRD1 payload decodes cleanly with v1 fields intact
			// and v2 fields reset to CameraMotion defaults (even when decoding into a target pre-populated with v2 data).
			std::vector<uint8_t> legacyBuf(buf.begin(), buf.begin() + kCrd1Bytes);
			const uint32_t crd1Magic = CameraLink::kRecordPodMagicV1;
			Assert::AreEqual<uint32_t>(0x43524431u, crd1Magic, L"CRD1 magic value");
			std::memcpy(legacyBuf.data(), &crd1Magic, sizeof(crd1Magic));

			CameraMotion legacy = a;   // pre-dirty every v2 field to ensure CRD1 resets them to defaults
			Assert::IsTrue(CameraLink::DecodeRecordPod(legacyBuf.data(), legacyBuf.size(), legacy), L"legacy CRD1 payload decodes cleanly");
			Assert::AreEqual(a.seq, legacy.seq);
			Assert::AreEqual(a.pairFrom, legacy.pairFrom);
			Assert::AreEqual(a.pairTo, legacy.pairTo);
			Assert::AreEqual(a.pairSeconds, legacy.pairSeconds);
			Assert::AreEqual(a.resyncGeneration, legacy.resyncGeneration);
			Assert::AreEqual(a.rebaseGeneration, legacy.rebaseGeneration);
			Assert::AreEqual(a.valid, legacy.valid);
			Assert::IsTrue(a.params.type == legacy.params.type);
			Assert::IsTrue(a.reason == legacy.reason);
			for (int i = 0; i < 9; ++i) Assert::AreEqual(a.params.h[i], legacy.params.h[i]);
			Assert::IsTrue(a.smoothedParams.type == legacy.smoothedParams.type);
			for (int i = 0; i < 9; ++i) Assert::AreEqual(a.smoothedParams.h[i], legacy.smoothedParams.h[i]);
			Assert::AreEqual(a.confidence, legacy.confidence);
			Assert::AreEqual(a.signals.inlierShare, legacy.signals.inlierShare);
			Assert::AreEqual(a.signals.logit, legacy.signals.logit);
			Assert::AreEqual(a.samples, legacy.samples);
			Assert::AreEqual(a.costMicros, legacy.costMicros);
			Assert::AreEqual(a.picture.bottom, legacy.picture.bottom);

			const CameraMotion defaults;
			Assert::AreEqual(defaults.verdictFlags, legacy.verdictFlags);
			Assert::AreEqual(defaults.layerFlags, legacy.layerFlags);
			Assert::AreEqual(defaults.finalRefineCells, legacy.finalRefineCells);
			Assert::AreEqual(defaults.signals.dynShare, legacy.signals.dynShare);
			Assert::AreEqual(defaults.signals.dynTemporal, legacy.signals.dynTemporal);
			Assert::AreEqual(defaults.signals.dynSpatial, legacy.signals.dynSpatial);
			Assert::AreEqual(defaults.signals.dynField, legacy.signals.dynField);
			Assert::AreEqual(defaults.signals.dynCurvature, legacy.signals.dynCurvature);
			Assert::AreEqual(defaults.signals.dynRhoTemp, legacy.signals.dynRhoTemp);
			Assert::AreEqual(defaults.signals.waterExcludedShare, legacy.signals.waterExcludedShare);
			Assert::AreEqual(defaults.signals.rawShare, legacy.signals.rawShare);
			Assert::AreEqual(defaults.signals.shearEnergy, legacy.signals.shearEnergy);
			Assert::AreEqual(defaults.signals.dipoleTB, legacy.signals.dipoleTB);
			Assert::AreEqual(defaults.signals.dipoleLR, legacy.signals.dipoleLR);
			Assert::AreEqual(defaults.signals.staticRing, legacy.signals.staticRing);
			Assert::AreEqual(defaults.signals.staticFit, legacy.signals.staticFit);
			Assert::AreEqual(defaults.signals.border, legacy.signals.border);
			Assert::AreEqual(defaults.signals.continuity, legacy.signals.continuity);
			Assert::AreEqual(defaults.layerRisk, legacy.layerRisk);
			Assert::AreEqual(defaults.haveObjectLayer, legacy.haveObjectLayer);
			Assert::AreEqual(defaults.objectLayer.id, legacy.objectLayer.id);
			Assert::IsTrue(defaults.objectLayer.type == legacy.objectLayer.type);
			for (int i = 0; i < 9; ++i)
				Assert::AreEqual(defaults.objectLayer.h[i], legacy.objectLayer.h[i]);
			Assert::AreEqual(defaults.objectLayer.blocks, legacy.objectLayer.blocks);
			Assert::AreEqual(defaults.objectLayer.cx, legacy.objectLayer.cx);
			Assert::AreEqual(defaults.objectLayer.cy, legacy.objectLayer.cy);

			// Same-input negative control (Rule 3): prove legacy CRD1 loses v2 state that CRD2 preserves.
			Assert::AreNotEqual(a.verdictFlags, legacy.verdictFlags, L"CRD1 drops verdictFlags");
			Assert::AreNotEqual(a.signals.waterExcludedShare, legacy.signals.waterExcludedShare, L"CRD1 drops waterExcludedShare");
			Assert::AreNotEqual(a.signals.dynCurvature, legacy.signals.dynCurvature, L"CRD1 drops dynCurvature");
			Assert::AreNotEqual(a.signals.shearEnergy, legacy.signals.shearEnergy, L"CRD1 drops shearEnergy");
			Assert::AreNotEqual(a.haveObjectLayer, legacy.haveObjectLayer, L"CRD1 drops objectLayer");

			// (c) Negative controls: corrupt magic, truncated CRD2/CRD1 payloads, and out-of-range objectLayer.type
			// must all return false and leave the target CameraMotion completely untouched.
			auto makeSentinel = []() {
				CameraMotion s;
				s.seq = 77777u;
				s.confidence = 0.125;
				s.verdictFlags = kVerdictStaticCamera;
				s.layerFlags = 0xAAu;
				s.finalRefineCells = 999u;
				s.signals.waterExcludedShare = 0.99;
				s.signals.dynCurvature = 0.44;
				s.signals.shearEnergy = 2.25;
				s.signals.border = 0.125;
				s.layerRisk = 0.77;
				s.haveObjectLayer = true;
				s.objectLayer.id = 42u;
				s.objectLayer.type = ModelType::Similarity;
				s.objectLayer.blocks = 88u;
				return s;
			};
			auto assertSentinelUntouched = [](const CameraMotion& m) {
				Assert::AreEqual<uint64_t>(77777u, m.seq, L"sentinel seq untouched");
				Assert::AreEqual(0.125, m.confidence, L"sentinel confidence untouched");
				Assert::AreEqual<uint8_t>(kVerdictStaticCamera, m.verdictFlags, L"sentinel verdictFlags untouched");
				Assert::AreEqual<uint8_t>(0xAAu, m.layerFlags, L"sentinel layerFlags untouched");
				Assert::AreEqual<uint32_t>(999u, m.finalRefineCells, L"sentinel finalRefineCells untouched");
				Assert::AreEqual(0.99, m.signals.waterExcludedShare, L"sentinel waterExcludedShare untouched");
				Assert::AreEqual(0.44, m.signals.dynCurvature, L"sentinel dynCurvature untouched");
				Assert::AreEqual(2.25, m.signals.shearEnergy, L"sentinel shearEnergy untouched");
				Assert::AreEqual(0.125, m.signals.border, L"sentinel border untouched");
				Assert::AreEqual(0.77, m.layerRisk, L"sentinel layerRisk untouched");
				Assert::IsTrue(m.haveObjectLayer, L"sentinel haveObjectLayer untouched");
				Assert::AreEqual<uint8_t>(42u, m.objectLayer.id, L"sentinel objectLayer.id untouched");
				Assert::IsTrue(m.objectLayer.type == ModelType::Similarity, L"sentinel objectLayer.type untouched");
				Assert::AreEqual<uint32_t>(88u, m.objectLayer.blocks, L"sentinel objectLayer.blocks untouched");
			};

			// Corrupt magic ("CRD3" and "CRD0")
			for (uint32_t badMagic : { 0x43524433u, 0x43524430u })
			{
				std::vector<uint8_t> bad = buf;
				std::memcpy(bad.data(), &badMagic, sizeof(badMagic));
				CameraMotion target = makeSentinel();
				Assert::IsFalse(CameraLink::DecodeRecordPod(bad.data(), n, target), L"corrupt magic refused");
				assertSentinelUntouched(target);
			}

			// Truncated CRD2: 1 byte short (594 B), pre-CE-WATER CRD2 length (547 B), AND exact CRD1 length (366 B) with CRD2 magic
			for (size_t truncLen : { kCrd2Bytes - 1, size_t(547), kCrd1Bytes })
			{
				CameraMotion target = makeSentinel();
				Assert::IsFalse(CameraLink::DecodeRecordPod(buf.data(), truncLen, target), L"truncated CRD2 refused");
				assertSentinelUntouched(target);
			}

			// Truncated legacy CRD1: 1 byte short (365 B)
			{
				CameraMotion target = makeSentinel();
				Assert::IsFalse(CameraLink::DecodeRecordPod(legacyBuf.data(), kCrd1Bytes - 1, target), L"truncated CRD1 refused");
				assertSentinelUntouched(target);
			}

			// Out-of-range objectLayer.type in CRD2 tail (byte offset 454)
			Assert::AreEqual<uint8_t>(uint8_t(a.objectLayer.type), buf[kObjectLayerTypeOffset], L"offset of objectLayer.type");
			for (uint8_t badType : { uint8_t(uint8_t(ModelType::Homography) + 1), uint8_t(0xFFu) })
			{
				std::vector<uint8_t> bad = buf;
				bad[kObjectLayerTypeOffset] = badType;
				CameraMotion target = makeSentinel();
				Assert::IsFalse(CameraLink::DecodeRecordPod(bad.data(), n, target), L"out-of-range objectLayer.type refused");
				assertSentinelUntouched(target);
			}

			// Boundary positive control: last valid ModelType enumerator is accepted for objectLayer.type
			{
				std::vector<uint8_t> edge = buf;
				edge[kObjectLayerTypeOffset] = uint8_t(ModelType::Homography);
				CameraMotion target = makeSentinel();
				Assert::IsTrue(CameraLink::DecodeRecordPod(edge.data(), n, target), L"last valid ModelType accepted for objectLayer.type");
				Assert::AreEqual(a.seq, target.seq);
				Assert::IsTrue(target.objectLayer.type == ModelType::Homography);
			}
		}
	};
}
