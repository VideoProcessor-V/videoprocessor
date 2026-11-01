// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4-1/2: the p7g_camera_guide declaration pins, and lane == live (the same unit BuildGuide, byte-equal); a late
// lane result is discarded. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraGuideProduct.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceScheduler.h>

#include <map>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct Link : CameraLink::ICameraLink
		{
			std::shared_ptr<const CameraModel::CameraMotion> rec;
			std::shared_ptr<const CameraLink::FieldCopy> field;
			std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t) const override { return rec; }
			std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t s) const override { return s == 1 ? rec : nullptr; }
			std::shared_ptr<const CameraLink::FieldCopy> FieldOf(uint64_t s) const override { return s == 1 ? field : nullptr; }
			bool LabelsFor(uint64_t, CameraLink::CameraConsumer, CameraLink::CameraLabelsView&) override { return false; }
			std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t) override { return nullptr; }
		};

		void Fill(Link& l)
		{
			CameraSynth::Field f(CameraSynth::Grid4K());
			f.FromModel(CameraSynth::PanModel(12, 0));
			f.AddNoise(0.05, 11);
			l.field = CameraLinkTest::CopyOf(f, 1);
			CameraModel::Fitter fitter;
			l.rec = std::make_shared<const CameraModel::CameraMotion>(fitter.Fit(f.Input(1.0 / 24, 1)));
		}

		CameraLink::GuidanceSettings Settings()
		{
			CameraLink::GuidanceSettings s;
			s.switches.modelGlobal = s.switches.narrowWindow = s.switches.e1Skip = s.switches.staticGate = true;
			return s;
		}
	}

	TEST_CLASS(GuideProductTests)
	{
	public:
		TEST_METHOD(DeclarationIsPinned)
		{
			using namespace VpSkeleton;
			Assert::AreEqual(int(kSkeletonProductEnd), int(CameraLink::kP7gCameraGuide), L"id after the last SA0 id");
			const Registry reg = CameraLink::SkeletonProductsWithGuide(3);
			const char* why = "";
			Assert::IsTrue(reg.Validate(&why), L"dense, inputs known, no cycle");
			Assert::AreEqual(size_t(kSkeletonProductEnd) + 1, reg.Size());
			const ProductDecl& p = reg.Get(CameraLink::kP7gCameraGuide);
			Assert::AreEqual(std::string("p7g_camera_guide"), std::string(p.token));
			Assert::IsTrue(p.backend == Backend::Cpu && p.tier == Tier::T0 && p.residency == Residency::SystemRam);
			Assert::AreEqual(2, int(p.inputCount));
			Assert::AreEqual(int(kP7CameraFit), int(p.inputs[0].product));
			Assert::AreEqual(3, int(p.inputs[0].lag));
			Assert::AreEqual(int(kP4SourceStats), int(p.inputs[1].product));
			Assert::AreEqual(0, int(p.inputs[1].lag));
			Assert::AreEqual(uint32_t(20), uint32_t(p.size.terms[0].bytesPerCell));
			Dims d4k;
			d4k.width = 3840; d4k.height = 2160;
			Assert::AreEqual(uint64_t(240u * 135u * 20u + 256u), BytesOf(p.size, d4k), L"about 0.6 MB at 4K");
			// The lag floor.
			const Registry low = CameraLink::SkeletonProductsWithGuide(0);
			Assert::AreEqual(int(CameraLink::kMinGuideLag), int(low.Get(CameraLink::kP7gCameraGuide).inputs[0].lag));
			// The first products are untouched (registry prefix identical to SkeletonProducts()).
			const Registry base = SkeletonProducts();
			for (size_t i = 0; i < base.Size(); ++i)
				Assert::AreEqual(std::string(base.All()[i].token), std::string(reg.All()[i].token));
			// The row and the consumer.
			const Catalog::ProductRow row = CameraLink::GuideRow();
			Assert::AreEqual(int(CameraLink::kP7gCameraGuide), int(row.id));
			Assert::AreEqual(CameraLink::kCameraUnitVersion, row.unitVersion, L"same unit as P7..P9");
			const ConsumerDecl c = CameraLink::GuideConsumerDecl();
			Assert::AreEqual(1, int(c.count));
			Assert::AreEqual(int(CameraLink::kP7gCameraGuide), int(c.consumes[0].product));
		}

		TEST_METHOD(AGraphWithTheGuideConsumerKeepsP7AtHistoryDAndBuilds)
		{
			using namespace VpSkeleton;
			const Registry reg = CameraLink::SkeletonProductsWithGuide(3);
			std::vector<ConsumerDecl> cs;
			cs.push_back(CameraLink::GuideConsumerDecl());
			const Graph g = BuildGraph(reg, cs, CameraLinkTest::Settings(2));
			Assert::IsTrue(g.valid, L"valid graph");
			Assert::IsTrue(g.Has(CameraLink::kP7gCameraGuide) && g.Has(kP7CameraFit) && g.Has(kP4SourceStats));
			Assert::IsTrue(g.Find(kP7CameraFit)->history >= 3, L"P7 is kept d frames");
			const Graph none = BuildGraph(reg, {}, CameraLinkTest::Settings(2));
			Assert::IsFalse(none.Has(CameraLink::kP7gCameraGuide), L"absent when nobody wants it");
		}

		TEST_METHOD(LaneAndLiveResultsAreByteEqual)
		{
			Link link;
			Fill(link);
			CameraLink::GuidanceScheduler lane(link, Settings());
			CameraLink::GuidanceScheduler live(link, Settings());
			lane.SetLag(2);
			live.SetLag(2);
			Assert::IsTrue(lane.PublishLane(3), L"the worker published first");
			const CameraLink::GuideResult a = lane.Guide(3);
			const CameraLink::GuideResult b = live.Guide(3);
			Assert::IsTrue(a.fromLane && !b.fromLane);
			Assert::IsTrue(a.frame != nullptr && b.frame != nullptr);
			Assert::IsTrue(CameraLink::SameGuide(*a.frame, *b.frame), L"same function, same bytes");
			Assert::IsTrue(a.frame->HasBlocks());
			Assert::AreEqual(uint64_t(1), lane.Counters().fromLane);
			Assert::AreEqual(uint64_t(1), live.Counters().fromLive);
		}

		TEST_METHOD(ALateLaneResultIsDiscardedNotMerged)
		{
			Link link;
			Fill(link);
			CameraLink::GuidanceScheduler s(link, Settings());
			s.SetLag(2);
			const CameraLink::GuideResult live = s.Guide(3);
			Assert::IsTrue(live.frame != nullptr && !live.fromLane);
			Assert::IsFalse(s.PublishLane(3), L"the render asked first");
			Assert::AreEqual(uint64_t(1), s.Counters().laneDiscarded);
			// A second ask recomputes live (the lane slot was never filled).
			const CameraLink::GuideResult again = s.Guide(3);
			Assert::IsFalse(again.fromLane);
			Assert::IsTrue(CameraLink::SameGuide(*live.frame, *again.frame));
		}

		TEST_METHOD(BuildGuideIsDeterministic)
		{
			Link link;
			Fill(link);
			CameraLink::GuideInputs in;
			in.record = link.rec.get();
			in.prevField = link.field.get();
			in.ctx.nextPairSeconds = 1.0 / 24;
			const CameraLink::GuidanceSettings s = Settings();
			const CameraLink::GuideResult a = CameraLink::BuildGuide(s.switches, s.config, in);
			const CameraLink::GuideResult b = CameraLink::BuildGuide(s.switches, s.config, in);
			Assert::IsTrue(a.frame && b.frame && CameraLink::SameGuide(*a.frame, *b.frame));
			// A cut flag changes the plan (skip), proving the P4(N) flags are inputs.
			in.ctx.cut = true;
			CameraModel::MeasurementSwitches cs = s.switches;
			cs.cutSkip = true;
			const CameraLink::GuideResult c = CameraLink::BuildGuide(cs, s.config, in);
			Assert::IsTrue(c.frame != nullptr && c.frame->skipMatcher);
		}
	};
}
