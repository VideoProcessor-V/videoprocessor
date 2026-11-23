// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraSmoother.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		std::shared_ptr<CameraModel::CameraMotion> MakePanRecord(uint64_t seq, double tx, double ty = 0.0,
			double pairSeconds = 1.0 / 24.0, uint64_t resyncGen = 1, CameraModel::Reason reason = CameraModel::Reason::Ok)
		{
			auto m = std::make_shared<CameraModel::CameraMotion>();
			m->seq = seq;
			m->pairFrom = seq > 0 ? seq - 1 : 0;
			m->pairTo = seq;
			m->pairSeconds = pairSeconds;
			m->resyncGeneration = resyncGen;
			m->rebaseGeneration = 1;
			m->valid = CameraModel::ReasonUsable(reason);
			m->reason = reason;
			m->confidence = m->valid ? 0.92 : 0.1;
			m->picture = { 0, 0, 1920, 1080 };
			m->params.type = m->valid ? CameraModel::ModelType::Translation : CameraModel::ModelType::None;
			m->params.h[0] = 1.0; m->params.h[1] = 0.0; m->params.h[2] = m->valid ? tx : 0.0;
			m->params.h[3] = 0.0; m->params.h[4] = 1.0; m->params.h[5] = m->valid ? ty : 0.0;
			m->params.h[6] = 0.0; m->params.h[7] = 0.0; m->params.h[8] = 1.0;
			m->rates.tx = m->valid && pairSeconds > 0 ? tx / pairSeconds : 0.0;
			m->rates.ty = m->valid && pairSeconds > 0 ? ty / pairSeconds : 0.0;
			return m;
		}
	}

	TEST_CLASS(CameraSmootherTests)
	{
	public:
		TEST_METHOD(EffectiveLeadClampsToMaxCameraLeadAndZero)
		{
			Assert::AreEqual(0, CameraLink::EffectiveCameraLead(-3));
			Assert::AreEqual(0, CameraLink::EffectiveCameraLead(0));
			Assert::AreEqual(1, CameraLink::EffectiveCameraLead(1));
			Assert::AreEqual(int(VpSkeleton::kMaxCameraLead), CameraLink::EffectiveCameraLead(int(VpSkeleton::kMaxCameraLead)));
			Assert::AreEqual(int(VpSkeleton::kMaxCameraLead), CameraLink::EffectiveCameraLead(int(VpSkeleton::kMaxCameraLead) + 10));
		}

		TEST_METHOD(DeterminismIdenticalInputsProduceBitIdenticalSmoothedOutput)
		{
			const double jitter[8] = { 0.4, -0.5, 0.35, -0.45, 0.5, -0.3, 0.25, -0.4 };
			const int maxLead = static_cast<int>(VpSkeleton::kMaxCameraLead);
			CameraLink::CameraSmoother a(maxLead);
			CameraLink::CameraSmoother b(maxLead);
			for (int i = 0; i < 8; ++i)
			{
				auto rec = MakePanRecord(static_cast<uint64_t>(i + 1), 10.0 + jitter[i], -2.0 + 0.5 * jitter[i]);
				a.Push(rec);
				b.Push(rec);
			}
			for (uint64_t s = 1; s <= 8; ++s)
			{
				CameraLink::SmoothQueryResult qa, qb;
				Assert::AreEqual(a.Query(s, qa), b.Query(s, qb));
				Assert::AreEqual(qa.smoothed, qb.smoothed);
				Assert::IsNotNull(qa.record.get());
				const CameraModel::Params& pa = CameraLink::ActiveParams(*qa.record);
				const CameraModel::Params& pb = CameraLink::ActiveParams(*qb.record);
				Assert::AreEqual(int(pa.type), int(pb.type));
				for (int k = 0; k < 9; ++k)
					Assert::AreEqual(0, std::memcmp(&pa.h[k], &pb.h[k], sizeof(double)), L"bit-identical smoothed model");
			}
		}

		TEST_METHOD(SmoothsJitteryPanAndPreservesConstantPanExactly)
		{
			const int maxLead = static_cast<int>(VpSkeleton::kMaxCameraLead);
			// Constant pan: smoothed output equals the exact pan velocity.
			CameraLink::CameraSmoother constant(maxLead);
			for (uint64_t s = 1; s <= 9; ++s)
				constant.Push(MakePanRecord(s, 8.0, -3.0));
			CameraLink::SmoothQueryResult qc;
			Assert::IsTrue(constant.Query(5, qc));
			Assert::IsTrue(qc.smoothed, L"interior frame with full N+-k neighbours is smoothed");
			Assert::AreEqual(8.0, qc.record->smoothedParams.h[2], 1e-6);
			Assert::AreEqual(-3.0, qc.record->smoothedParams.h[5], 1e-6);

			// Jittery pan around 8.0 px: smoothed variance is strictly smaller than raw variance.
			CameraLink::CameraSmoother jittery(maxLead);
			const double raw[9] = { 9.0, 7.0, 9.0, 7.0, 9.0, 7.0, 9.0, 7.0, 9.0 };
			for (int i = 0; i < 9; ++i)
				jittery.Push(MakePanRecord(static_cast<uint64_t>(i + 1), raw[i]));
			double rawErr = 0.0, smoothErr = 0.0;
			int smoothedCount = 0;
			for (uint64_t s = 3; s <= 7; ++s)
			{
				CameraLink::SmoothQueryResult q;
				Assert::IsTrue(jittery.Query(s, q));
				Assert::IsTrue(q.smoothed);
				++smoothedCount;
				rawErr += std::fabs(raw[s - 1] - 8.0);
				smoothErr += std::fabs(q.record->smoothedParams.h[2] - 8.0);
			}
			Assert::AreEqual(5, smoothedCount);
			Assert::IsTrue(smoothErr < rawErr * 0.5, L"P9 smoother substantially attenuates pan jitter");
		}

		TEST_METHOD(CutAndGenerationBoundariesBreakSmoothingWindowCleanly)
		{
			const int maxLead = static_cast<int>(VpSkeleton::kMaxCameraLead);
			CameraLink::CameraSmoother sm(maxLead);
			// Segment 1: tx = 4.0 for seq 1..4; seq 5 is a cut; Segment 2: tx = 20.0 for seq 6..10.
			for (uint64_t s = 1; s <= 4; ++s)
				sm.Push(MakePanRecord(s, 4.0));
			sm.Push(MakePanRecord(5, 0.0, 0.0, 1.0 / 24.0, 1, CameraModel::Reason::Cut));
			for (uint64_t s = 6; s <= 10; ++s)
				sm.Push(MakePanRecord(s, 20.0));

			// Frame 4 (immediately before cut) must NOT bleed segment 2's tx=20 into segment 1.
			CameraLink::SmoothQueryResult q4;
			Assert::IsTrue(sm.Query(4, q4));
			Assert::IsTrue(q4.smoothed);
			Assert::AreEqual(4.0, q4.record->smoothedParams.h[2], 1e-6, L"pre-cut frame never blends across cut");

			// Frame 6 (immediately after cut) must NOT bleed segment 1's tx=4 into segment 2.
			CameraLink::SmoothQueryResult q6;
			Assert::IsTrue(sm.Query(6, q6));
			Assert::IsTrue(q6.smoothed);
			Assert::AreEqual(20.0, q6.record->smoothedParams.h[2], 1e-6, L"post-cut frame never blends across cut");

			// Generation boundary (resyncGeneration changes 1 -> 2 at seq 11) also breaks the window.
			for (uint64_t s = 11; s <= 15; ++s)
				sm.Push(MakePanRecord(s, -12.0, 0.0, 1.0 / 24.0, 2));
			CameraLink::SmoothQueryResult q11;
			Assert::IsTrue(sm.Query(11, q11));
			Assert::IsTrue(q11.smoothed);
			Assert::AreEqual(-12.0, q11.record->smoothedParams.h[2], 1e-6, L"generation change breaks smoothing window");

			// Non-monotonic sequence jump backwards triggers an automatic reset.
			const uint64_t resetsBefore = sm.Counters().resets.load();
			sm.Push(MakePanRecord(3, 1.0));
			Assert::AreEqual(resetsBefore + 1, sm.Counters().resets.load());
			CameraLink::SmoothQueryResult oldGone;
			Assert::IsFalse(sm.Query(11, oldGone), L"older sequence evicted after reset");
		}

		TEST_METHOD(LeadShortAndDepthZeroUseCausalP7WithVisibleStatusLine)
		{
			// Depth 0: causal P7 + visible "camera: causal (queue short)" line & engaged fallback decision.
			CameraLink::CameraSmoother depth0(0);
			Assert::IsTrue(depth0.IsCausalShortQueue());
			Assert::AreEqual(std::string(CameraLink::kCausalShortQueueText), depth0.StatusLine());
			const FallbackCatalog::Decision d0 = CameraLink::SmootherDecision(depth0.EffectiveLead(), true);
			Assert::IsTrue(d0.engaged);
			Assert::AreEqual(std::string(CameraLink::kIdSmoothedCausal), d0.id);
			Assert::AreEqual(std::string(CameraLink::kCausalShortQueueText), d0.reason);

			for (uint64_t s = 1; s <= 5; ++s)
				depth0.Push(MakePanRecord(s, 6.0 + double(s)));
			CameraLink::SmoothQueryResult q;
			Assert::IsTrue(depth0.Query(3, q));
			Assert::IsFalse(q.smoothed);
			Assert::IsTrue(q.causalShortQueue);
			Assert::AreEqual(9.0, CameraLink::ActiveParams(*q.record).h[2], 1e-6, L"depth 0 returns causal P7 unchanged");
			Assert::AreEqual(uint64_t(1), depth0.Counters().causalShortQueue.load());

			// Dynamic lead increase from measured queue depth: rebuilding unlocks smoothing without losing history.
			depth0.SetLaneLead(2);
			Assert::AreEqual(2, depth0.EffectiveLead());
			Assert::IsFalse(CameraLink::SmootherDecision(depth0.EffectiveLead(), true).engaged);
			CameraLink::SmoothQueryResult qAfter;
			Assert::IsTrue(depth0.Query(3, qAfter));
			Assert::IsTrue(qAfter.smoothed, L"once queue depth grows, smoothing activates over retained history");

			// Window short (future frames N+1..N+k have not arrived yet): causal fallback visibly counted.
			CameraLink::SmoothQueryResult qTail;
			Assert::IsTrue(depth0.Query(5, qTail));
			Assert::IsFalse(qTail.smoothed);
			Assert::IsTrue(qTail.causalWindowShort);
			Assert::AreEqual(uint64_t(1), depth0.Counters().causalWindowShort.load());
		}

		TEST_METHOD(SmoothedPodCodecRoundTripsAndRejectsBadInputs)
		{
			auto rec = MakePanRecord(42, 7.5, -2.25);
			rec->smoothed = true;
			rec->smoothedParams = rec->params;
			rec->smoothedParams.h[2] = 7.25;
			uint8_t buf[CameraLink::kSmoothedPodMaxBytes] = {};
			const size_t n = CameraLink::EncodeSmoothedPod(*rec, 2, buf, sizeof(buf));
			Assert::IsTrue(n > 0 && n <= CameraLink::kSmoothedPodMaxBytes);

			CameraModel::CameraMotion decoded;
			uint8_t lead = 0;
			Assert::IsTrue(CameraLink::DecodeSmoothedPod(buf, n, decoded, &lead));
			Assert::AreEqual(uint8_t(2), lead);
			Assert::AreEqual(uint64_t(42), decoded.seq);
			Assert::IsTrue(decoded.valid);
			Assert::IsTrue(decoded.smoothed);
			Assert::AreEqual(7.25, decoded.smoothedParams.h[2], 1e-9);
			Assert::AreEqual(-2.25, decoded.smoothedParams.h[5], 1e-9);

			// Short buffer and corrupted magic are refused cleanly.
			Assert::AreEqual(size_t(0), CameraLink::EncodeSmoothedPod(*rec, 2, buf, n - 1));
			CameraModel::CameraMotion untouched;
			untouched.seq = 999;
			Assert::IsFalse(CameraLink::DecodeSmoothedPod(buf, n - 1, untouched));
			Assert::AreEqual(uint64_t(999), untouched.seq);
			buf[0] ^= 0xFF;
			Assert::IsFalse(CameraLink::DecodeSmoothedPod(buf, n, untouched));
			Assert::AreEqual(uint64_t(999), untouched.seq);
		}

		TEST_METHOD(CameraLaneWithSmoothedProductPublishesAndReadsP9Slot)
		{
			auto ind = std::make_shared<VpIndicator::Indicator>(1, 256);
			CameraLink::CameraLane lane;
			Assert::IsTrue(lane.Build(1920, 1080, false, ind, true, 2), L"lane builds with kP9CameraSmoothed");
			Assert::IsTrue(lane.HasSmoothed());
			Assert::AreEqual(2, lane.EffectiveCameraLead());
			Assert::IsNotNull(lane.P9Bytes());
			Assert::AreEqual(CameraLink::kSmoothedPodMaxBytes, lane.P9Bytes()->Bytes());

			// With laneLead == 0, the graph substitutes causal P7 for P9 (no P9 node).
			CameraLink::CameraLane shortLane;
			Assert::IsTrue(shortLane.Build(1920, 1080, false, ind, true, 0));
			Assert::IsFalse(shortLane.HasSmoothed(), L"depth-0 lane reduces P9 to causal P7");
			Assert::AreEqual(0, shortLane.EffectiveCameraLead());
		}

		TEST_METHOD(CameraLinkCoreDrivesSmootherWhenP9WantedAndAllocatesNothingWhenOff)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore offCore(store);
			CameraLink::CameraLinkSettings offSet;
			offSet.p7Wanted = true;
			offSet.deterministic = true;
			offSet.p9Wanted = false;
			offCore.OnSettings(offSet);
			CameraLinkTest::Rig rig(2);
			offCore.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			Assert::IsNull(offCore.Smoother(), L"P9 default-off allocates no smoother");

			CameraLink::CameraLinkCore onCore(store);
			CameraLink::CameraLinkSettings onSet;
			onSet.p7Wanted = true;
			onSet.deterministic = true;
			onSet.p9Wanted = true;
			onSet.p9LaneLead = 0;   // starts at short queue -> visible causal status
			onCore.OnSettings(onSet);
			CameraLinkTest::Rig rig2(2);
			onCore.AttachGraph(rig2.table.get(), rig2.bytes.get(), rig2.indicator.get());
			Assert::IsNotNull(onCore.Smoother());
			std::vector<std::string> lines;
			onCore.CollectStatus(lines);
			bool sawCausal = false;
			for (const std::string& l : lines)
				if (l == CameraLink::kCausalShortQueueText)
					sawCausal = true;
			Assert::IsTrue(sawCausal, L"short-queue causal P9 fallback is visible in CollectStatus");

			onCore.UpdateP9Lead(2);
			Assert::AreEqual(2, onCore.Smoother()->EffectiveLead());
		}
	};
}
