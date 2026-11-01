// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Item 16: the Dolby Vision metadata reader of the central Indicator (DvResyncReader.h), the adapter
// around the real DvMetadataHold::Tracker, and the visible lost-metadata fallback (DvFallbacks.h).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/DvFallbacks.h>
#include <VP-V/modules/input/signal/DvResyncReader.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpDvResync;
using VpResync::Cause;

namespace Tests
{
	namespace DvResync
	{
		inline DolbyVisionTunnelFrameMetadata Dm(uint16_t scenePq)
		{
			DolbyVisionTunnelFrameMetadata md;
			md.valid = true;
			md.sourceMinPq = 62;
			md.sourceMaxPq = 3079;
			md.hasSceneMetadata = true;
			md.sceneMaxPq = scenePq;
			md.sceneAvgPq = 1200;
			md.trimCount = 2;
			md.trims[0].level = 2;
			md.trims[1].level = 8;
			return md;
		}

		inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		struct Rig
		{
			std::shared_ptr<VpIndicator::Indicator> ind;
			Reader reader;
			MetadataHolder<> holder;
			DolbyVisionTunnelFrameMetadata dm = Dm(2800);
			uint64_t next = 1;

			explicit Rig(bool rate = true)
			{
				ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
				if (rate)
					holder.SetRate(50.0, 2.0);
				holder.Bind(reader);
				reader.RegisterOn(ind);
			}

			// Seals the next frame (the reader acts inside), then the picture arrives with or without metadata.
			MetadataHolder<>::Shown Frame(bool withDm)
			{
				const uint64_t seq = next++;
				ind->SealAndObserve(seq);
				return holder.OnPicture(withDm ? &dm : nullptr, seq);
			}
			void Run(unsigned frames)
			{
				for (unsigned i = 0; i < frames; ++i)
					Frame(true);
			}
		};
	}

	TEST_CLASS(DvResyncReaderTests)
	{
	public:
		TEST_METHOD(SpecReadsFullContinuityAndOutputSettings)
		{
			const ReaderSpec& s = Spec();
			Assert::AreEqual<std::string>("dv-metadata", s.name);
			const VpIndicator::CounterMask want = VpIndicator::Bit(VpIndicator::kFull) | VpIndicator::Bit(VpIndicator::kContinuity) |
				VpIndicator::Bit(VpIndicator::kSettingsOutput);
			Assert::AreEqual<uint32_t>(want, s.reads);
			Assert::IsTrue(s.policies[VpIndicator::kFull] == VpIndicator::Policy::Relatch);
			Assert::IsTrue(s.policies[VpIndicator::kContinuity] == VpIndicator::Policy::Hold);
			Assert::IsTrue(s.policies[VpIndicator::kSettingsOutput] == VpIndicator::Policy::Relatch);
			Assert::IsTrue(s.policies[VpIndicator::kTiming] == VpIndicator::Policy::Ignore);
			DvResync::Rig r;
			Assert::IsTrue(r.reader.ReaderId() >= 0);
			Assert::AreEqual<std::string>("dv-metadata", r.ind->ReaderName(r.reader.ReaderId()));
		}

		TEST_METHOD(EveryFullCauseDropsTheHeldMetadataAndRelatchesFromTheNextValidPicture)
		{
			const Cause causes[] = { Cause::InPlaceSourceChange, Cause::RendererReset, Cause::LiveQueueReset, Cause::FileDiscontinuity,
				Cause::QueueGenerationOther, Cause::SourceFormatChanged, Cause::CounterBackwards, Cause::TimestampBreakLarge,
				Cause::StartAfterSourceChange, Cause::CalibrationStart, Cause::CalibrationEnd };
			for (Cause c : causes)
			{
				DvResync::Rig r;
				r.holder.AdvanceHdrInferenceEpoch();
				r.Run(4);   // seq 1..4, metadata latched, tunnel detected
				Assert::IsTrue(r.holder.HaveMetadata());
				Assert::IsTrue(r.holder.TunnelDetected());
				Assert::AreEqual<uint64_t>(0, r.holder.RelatchCount(), L"the latching first observe does not act");
				r.ind->Advance(VpIndicator::kFull, c, 5);
				r.ind->SealAndObserve(5);   // the reader acts here, before the picture
				Assert::AreEqual<uint64_t>(1, r.holder.RelatchCount(), DvResync::W(VpResync::Token(c)).c_str());
				Assert::IsFalse(r.holder.HaveMetadata(), L"held metadata dropped");
				Assert::IsFalse(r.holder.TunnelDetected(), L"tunnel detection dropped");
				Assert::IsTrue(r.holder.AwaitingRelatch());
				Assert::AreEqual<uint32_t>(1, r.holder.HdrInferenceEpoch(), L"the HDR inference epoch is kept");
				const auto shown = r.holder.OnPicture(&r.dm, 5);
				Assert::IsTrue(shown.haveMetadata);
				Assert::IsTrue(r.holder.HaveMetadata() && r.holder.TunnelDetected());
				Assert::IsFalse(r.holder.AwaitingRelatch());
				Assert::AreEqual<uint64_t>(1, r.holder.Relatched());
				Assert::AreEqual<uint32_t>(1, r.holder.HdrInferenceEpoch());
			}
		}

		TEST_METHOD(MinorKeepsTheMetadata)
		{
			const Cause causes[] = { Cause::MissedFrames, Cause::TimestampBreak, Cause::StaleDelivery, Cause::OverflowDrop,
				Cause::GapOther, Cause::DriftDrop, Cause::PulldownRephase };
			for (Cause c : causes)
			{
				DvResync::Rig r;
				r.Run(4);
				r.ind->Advance(VpIndicator::kContinuity, c, 5, 2);
				r.ind->SealAndObserve(5);
				Assert::AreEqual<uint64_t>(1, r.reader.Holds(), DvResync::W(VpResync::Token(c)).c_str());
				Assert::AreEqual<uint64_t>(0, r.reader.Relatches());
				Assert::AreEqual<uint64_t>(1, r.holder.HoldCount());
				Assert::IsTrue(r.holder.HaveMetadata(), L"MINOR keeps the metadata");
				Assert::IsTrue(r.holder.TunnelDetected());
				Assert::IsFalse(r.holder.AwaitingRelatch());
				Assert::AreEqual<uint16_t>(2800, r.holder.Retained().sceneMaxPq);
			}
		}

		TEST_METHOD(TimingAndUnrelatedCountersAreIgnored)
		{
			DvResync::Rig r;
			r.Run(3);
			r.ind->Advance(VpIndicator::kTiming, Cause::RateStep, 4);
			r.ind->Advance(VpIndicator::kRebaseH, Cause::RebaseDecision, 4);
			r.ind->Advance(VpIndicator::kGraph, Cause::GraphRebuild, 4);
			r.ind->Advance(VpIndicator::kSettingsMotion, Cause::SettingsApply, 4);
			r.ind->Advance(VpIndicator::kSettingsAnalysis, Cause::SettingsApply, 4);
			r.Run(3);
			Assert::AreEqual<uint64_t>(0, r.reader.Relatches());
			Assert::AreEqual<uint64_t>(0, r.reader.Holds());
			Assert::AreEqual<uint64_t>(1, r.reader.Calls(), L"only the latching first call");
			Assert::IsTrue(r.holder.HaveMetadata());
		}

		TEST_METHOD(OutputSettingsRelatch)
		{
			DvResync::Rig r;
			r.Run(4);
			r.ind->Advance(VpIndicator::kSettingsOutput, Cause::SettingsApply, 5);
			r.Frame(true);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches());
			Assert::AreEqual<uint64_t>(1, r.holder.RelatchCount());
			Assert::AreEqual<uint64_t>(1, r.holder.Relatched(), L"re-latched from the same frame's valid metadata");
		}

		TEST_METHOD(ActsExactlyOncePerCarryingFrame)
		{
			DvResync::Rig r;
			r.Run(4);
			// A storm: three FULL events and a MINOR at the same frame act once, as a relatch.
			r.ind->Advance(VpIndicator::kFull, Cause::LiveQueueReset, 5);
			r.ind->Advance(VpIndicator::kFull, Cause::RendererReset, 5);
			r.ind->Advance(VpIndicator::kFull, Cause::InPlaceSourceChange, 5);
			r.ind->Advance(VpIndicator::kContinuity, Cause::MissedFrames, 5);
			r.Frame(true);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches());
			Assert::AreEqual<uint64_t>(0, r.reader.Holds(), L"the relatch covers the continuity change");
			const Reader::Pending p = r.reader.Take();
			Assert::IsTrue(p.relatch && !p.hold);
			Assert::AreEqual<uint32_t>(1, p.relatches);
			Assert::AreEqual<uint64_t>(5, p.seq);
			const Reader::Pending again = r.reader.Take();
			Assert::IsFalse(again.relatch || again.hold, L"Take clears");
			r.Run(2);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches(), L"quiet frames do not act");
			r.ind->Advance(VpIndicator::kFull, Cause::FileDiscontinuity, 8);
			r.Frame(true);
			Assert::AreEqual<uint64_t>(2, r.reader.Relatches(), L"the next carrying frame acts once more");
			Assert::AreEqual<uint64_t>(0, r.reader.Duplicates());
		}

		TEST_METHOD(ANewEraDoesNotApplyASpuriousFull)
		{
			DvResync::Rig r;
			r.Run(3);
			r.ind->Advance(VpIndicator::kFull, Cause::RendererReset, 4);
			r.Frame(true);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches());
			r.reader.Take();

			// A new Indicator (Shadow::Configure): its history already holds FULL events, but the reader
			// latches at its first observe and acts only on what changes AT that frame.
			auto era2 = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			era2->Advance(VpIndicator::kFull, Cause::Start, 0);
			era2->Advance(VpIndicator::kFull, Cause::StartAfterSourceChange, 0);
			era2->Advance(VpIndicator::kContinuity, Cause::GapOther, 0);
			r.reader.RegisterOn(era2);
			r.reader.RegisterOn(era2);   // a no-op for the Indicator already registered
			Assert::AreEqual<uint32_t>(2, r.reader.Eras());
			era2->SealAndObserve(100);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches(), L"no spurious FULL on the new era");
			Assert::AreEqual<uint64_t>(0, r.reader.Holds());
			Assert::IsFalse(r.reader.Peek().relatch);
			Assert::IsTrue(r.holder.HaveMetadata(), L"the holder is untouched by the latch");
			// but a real change on the new era acts
			era2->Advance(VpIndicator::kFull, Cause::InPlaceSourceChange, 102);
			era2->SealAndObserve(101);
			Assert::AreEqual<uint64_t>(1, r.reader.Relatches());
			era2->SealAndObserve(102);
			Assert::AreEqual<uint64_t>(2, r.reader.Relatches());
			Assert::IsFalse(r.holder.HaveMetadata());
		}

		TEST_METHOD(ReRegistrationDropsPendingFlags)
		{
			DvResync::Rig r;
			r.Run(3);
			r.ind->Advance(VpIndicator::kFull, Cause::RendererReset, 4);
			r.Frame(true);
			Assert::IsTrue(r.reader.Peek().relatch);
			auto era2 = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			r.reader.RegisterOn(era2);
			Assert::IsFalse(r.reader.Peek().relatch);
		}

		TEST_METHOD(DvSwitchSequenceThroughTheHostHooks)
		{
			DvResync::Rig r;
			r.Run(3);
			const Mode order[] = { Mode::Tunnel, Mode::Lldv, Mode::Hdr10, Mode::Sdr };
			// the host moves tunnel -> lldv -> hdr10 -> sdr: each switch is a FULL and the new mode is set first
			uint64_t relatches = 0;
			for (size_t i = 1; i < 4; ++i)
			{
				r.holder.SetMode(order[i]);
				r.ind->Advance(VpIndicator::kFull, Cause::SourceFormatChanged, r.next);
				const bool carries = ModeCarriesMetadata(order[i]);
				const auto shown = r.Frame(carries);
				++relatches;
				Assert::AreEqual<uint64_t>(relatches, r.holder.RelatchCount(), DvResync::W(ModeName(order[i])).c_str());
				Assert::AreEqual<bool>(carries, shown.haveMetadata);
				Assert::AreEqual<bool>(carries, r.holder.HaveMetadata());
				Assert::IsFalse(r.holder.Lost());
				Assert::IsFalse(r.holder.Fallback().engaged);
			}
			Assert::IsTrue(r.holder.GetMode() == Mode::Sdr);
			// hdr10 / sdr without metadata is not a loss, however long it lasts
			for (int i = 0; i < 200; ++i)
				Assert::IsFalse(r.Frame(false).lost);
			Assert::AreEqual<uint32_t>(0, r.holder.LostFrames());
			Assert::IsFalse(r.holder.Fallback().engaged);
			// and back to a mode with metadata relatches from the first valid picture
			r.holder.SetMode(Mode::Lldv);
			r.ind->Advance(VpIndicator::kFull, Cause::SourceFormatChanged, r.next);
			r.Frame(true);
			Assert::IsTrue(r.holder.HaveMetadata());
			Assert::AreEqual<uint64_t>(4, r.holder.RelatchCount());
		}

		TEST_METHOD(RpuLossHoldsThenShowsTheVisibleFallbackThenReturnRelatches)
		{
			DvResync::Rig r;
			r.Run(5);
			const uint32_t hold = r.holder.Hold().HoldPictures();
			Assert::AreEqual<uint32_t>(13, hold, L"0.25 s at the measured 50 Hz");
			for (uint32_t i = 1; i <= hold; ++i)
			{
				const auto s = r.Frame(false);
				Assert::IsFalse(s.lost, L"inside the hold");
				Assert::IsTrue(s.phase == DvMetadataHold::Phase::Held);
				Assert::IsTrue(s.haveMetadata && s.metadata.reusedPreviousDm);
				Assert::IsTrue(s.metadata.hasSceneMetadata, L"the held DM is whole inside the hold");
				Assert::IsFalse(r.holder.Fallback().engaged);
			}
			const auto lost = r.Frame(false);
			Assert::IsTrue(lost.lost);
			Assert::AreEqual<uint32_t>(hold + 1, lost.lostFrames);
			Assert::IsTrue(lost.phase == DvMetadataHold::Phase::Static);
			Assert::IsTrue(lost.haveMetadata);
			Assert::IsFalse(lost.metadata.hasSceneMetadata, L"static part only: no scene L1");
			Assert::AreEqual<uint8_t>(0, lost.metadata.trimCount);
			Assert::IsTrue(r.holder.Retained().hasSceneMetadata, L"the retained DM stays whole");
			const FallbackCatalog::Decision d = r.holder.Fallback();
			Assert::IsTrue(d.engaged);
			Assert::AreEqual<std::string>("dv_metadata_lost", d.id);
			Assert::IsTrue(d.reason.find("no Dolby Vision metadata for 14 pictures") != std::string::npos, DvResync::W(d.reason).c_str());
			Assert::IsTrue(r.holder.StatusText().find("LOST for 14") != std::string::npos, DvResync::W(r.holder.StatusText()).c_str());

			// the RPU returns: latched from the next valid picture, the fallback released
			r.dm = DvResync::Dm(2500);
			const auto back = r.Frame(true);
			Assert::IsFalse(back.lost);
			Assert::IsTrue(back.phase == DvMetadataHold::Phase::Own);
			Assert::IsFalse(back.metadata.reusedPreviousDm);
			Assert::AreEqual<uint16_t>(2500, back.metadata.sceneMaxPq);
			Assert::AreEqual<uint16_t>(2500, r.holder.Retained().sceneMaxPq);
			Assert::AreEqual<uint32_t>(0, r.holder.LostFrames());
			const FallbackCatalog::Decision released = r.holder.Fallback();
			Assert::IsFalse(released.engaged);
			Assert::AreEqual<std::string>("dv_metadata_lost", released.id);
		}

		TEST_METHOD(AFullDuringAnRpuLossKeepsTheLossVisibleUntilTheReturnRelatches)
		{
			DvResync::Rig r;
			r.Run(5);
			const uint32_t hold = r.holder.Hold().HoldPictures();
			for (uint32_t i = 0; i < hold + 3; ++i)
				r.Frame(false);
			Assert::IsTrue(r.holder.Lost());
			r.ind->Advance(VpIndicator::kFull, Cause::RendererReset, r.next);
			const auto s = r.Frame(false);
			Assert::AreEqual<uint64_t>(1, r.holder.RelatchCount());
			Assert::IsFalse(s.haveMetadata, L"nothing retained after the relatch: black");
			Assert::IsTrue(s.lost, L"the loss in progress stays visible");
			Assert::IsTrue(r.holder.Fallback().engaged);
			const auto back = r.Frame(true);
			Assert::IsTrue(back.haveMetadata && !back.lost);
			Assert::AreEqual<uint64_t>(1, r.holder.Relatched());
			Assert::IsFalse(r.holder.Fallback().engaged);
		}

		TEST_METHOD(AnUnmeasuredRateIsSaidSo)
		{
			DvResync::Rig r(false);
			Assert::IsFalse(r.holder.RateKnown());
			r.Run(2);
			r.Frame(false);
			Assert::IsTrue(r.holder.StatusText().find("frame rate not measured") != std::string::npos);
		}

		TEST_METHOD(FallbackBuilder)
		{
			const FallbackCatalog::Decision off = DvMetadataLost(false, 99);
			Assert::IsFalse(off.engaged);
			Assert::AreEqual<std::string>("dv_metadata_lost", off.id);
			Assert::IsTrue(off.reason.empty());
			const FallbackCatalog::Decision on = DvMetadataLost(true, 40);
			Assert::IsTrue(on.engaged);
			Assert::AreEqual<std::string>("dv_metadata_lost", on.id);
			Assert::IsTrue(on.reason.find("40 pictures") != std::string::npos);
			Assert::IsTrue(DvMetadataLost(true, -5).reason.find("for 0 pictures") != std::string::npos);
		}

		TEST_METHOD(GenericHooksActObserverAndRefusedRegistration)
		{
			DvResync::Rig r;   // holder bound; replace the hooks with a plain recorder
			std::vector<std::pair<Reader::Action, uint64_t>> got;
			std::vector<std::pair<std::string, uint64_t>> acks;
			r.reader.SetHooks([&](const Reader::Act& a) { got.emplace_back(a.action, a.seq); },
				[&](const Reader::Act& a) { got.emplace_back(a.action, a.seq); });
			r.reader.SetActObserver([&](const char* name, uint64_t seq) { acks.emplace_back(name, seq); });
			r.Run(3);
			r.ind->Advance(VpIndicator::kContinuity, Cause::MissedFrames, 4);
			r.ind->Advance(VpIndicator::kFull, Cause::RendererReset, 6);
			r.Run(4);
			Assert::AreEqual<size_t>(2, got.size());
			Assert::IsTrue(got[0].first == Reader::Action::Hold && got[0].second == 4);
			Assert::IsTrue(got[1].first == Reader::Action::Relatch && got[1].second == 6);
			Assert::AreEqual<size_t>(2, acks.size());
			Assert::AreEqual<std::string>("dv-metadata", acks[0].first);
			Assert::AreEqual<uint64_t>(6, acks[1].second);
			Assert::IsFalse(r.holder.AwaitingRelatch(), L"the replaced hook did not reach the holder");

			// an Indicator that has already observed refuses the registration: said, never silent
			auto late = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			late->SealAndObserve(1);
			Reader refused;
			refused.RegisterOn(late);
			Assert::AreEqual<int>(-1, refused.ReaderId());
			Assert::IsTrue(refused.StatusText().find("NOT registered") != std::string::npos);
			Assert::IsTrue(Reader().StatusText().find("not registered") != std::string::npos);
		}
	};
}
