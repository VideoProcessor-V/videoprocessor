// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Resync Q1: the ResyncExtension behind the generic renderer hooks. CPU only; the extension is
// driven directly (no global registry), with the renderer's Shadow / Drive owned by the test.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/FramesInFlight.h>
#include <VP-V/core/StatusText.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <vprenderer/AlphaQueuePolicy.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace ResyncExtensionQ1
	{
		namespace RE = RendererExtension;
		using Vpv::Resync::ResyncExtension;

		struct FakeRenderer
		{
			VpResync::Shadow resync;
			VpResync::Drive resyncDrive;
			Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		};

		void Apply(ResyncExtension& ext, const void* host, const char* mode, bool original = false, int motionAhead = 0)
		{
			RE::LifecycleFacts f;
			f.host = host; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = original ? 1 : 0;
			f.value = static_cast<double>(motionAhead);
			ext.OnLifecycle(f);
		}

		RE::IngressContext Frame(const void* host, int64_t seq, bool gap = false)
		{
			RE::IngressContext c;
			c.host = host; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000;
			c.gapBefore = gap; c.gapFrames = gap ? 2 : 0; c.gapCause = gap ? "missed frames" : nullptr;
			c.measuredRateHz = 24.0;
			return c;
		}

		VpResync::EnqueueFacts FactsOf(const RE::IngressContext& c)
		{
			VpResync::EnqueueFacts f;
			f.sequence = static_cast<uint64_t>(c.seq); f.queueGeneration = static_cast<uint64_t>(c.queueGeneration);
			f.stateKey = c.stateKey; f.formatKey = c.formatKey; f.frameCounter = static_cast<uint64_t>(c.frameCounter);
			f.timestampTicks = c.timestamp; f.ticksPerSecond = c.ticksPerSecond; f.gapBefore = c.gapBefore;
			f.gapFrames = c.gapFrames; f.gapCause = c.gapCause; f.sourceDiscontinuity = c.discontinuity;
			f.fileFrame = c.fromFile; f.measuredRateHz = c.measuredRateHz;
			f.overflowSuccessorSeq = static_cast<uint64_t>(c.overflowSuccessor);
			return f;
		}

		void Begin(ResyncExtension& ext, const void* host, int64_t seq)
		{
			RE::FrameContext c; c.host = host; c.phase = RE::FramePhase::Begin; c.seq = seq;
			Assert::IsFalse(ext.OnFrame(c), L"the seal never takes the frame over");
		}

		std::vector<std::string> StatusLines(ResyncExtension& ext, const void* host, RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = host;
			ext.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}

		TEST_CLASS(ResyncExtensionTests)
		{
		public:
			TEST_METHOD(OffBuildsNothing)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "off");
				for (int64_t s = 1; s <= 4; ++s) { RE::IngressContext c = Frame(&r, s); ext.OnIngress(c); Begin(ext, &r, s); }
				Assert::IsFalse(r.resync.Active());
				Assert::IsTrue(StatusLines(ext, &r, RE::StatusKind::Lines).empty(), L"no row anywhere in Off");
				Assert::AreEqual<uint64_t>(0, ResyncExtension::SealFor(&r)->Seals());
				Assert::AreEqual<uint64_t>(0, r.resync.PulldownCursorSeq());
			}

			TEST_METHOD(OriginalForcesOff)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "on", true);
				Assert::IsTrue(r.resync.GetMode() == VpResync::Shadow::Mode::Off);
				Apply(ext, &r, "on", false);
				Assert::IsTrue(r.resync.Driving());
			}

			TEST_METHOD(IngressMapsEveryEnqueueFact)
			{
				// The extension path and a direct Shadow fed the same facts record the same events.
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow", false, 3);
				VpResync::Shadow direct;
				direct.Configure(VpResync::Shadow::Mode::Shadow,
					FramesInFlight(AlphaQueuePolicy::HardCapacity(AlphaQueuePolicy::DEFAULT_DESIRED_DEPTH), 3, 0, 2, 0));
				for (int64_t s = 1; s <= 12; ++s)
				{
					RE::IngressContext c = Frame(&r, s, s == 6);
					if (s == 9) { c.discontinuity = true; c.overflowSuccessor = 8; }
					ext.OnIngress(c);
					direct.OnEnqueue(FactsOf(c), 1.0);
				}
				std::string viaExtension, viaDirect;
				Assert::IsTrue(r.resync.EventLines(viaExtension));
				Assert::IsTrue(direct.EventLines(viaDirect));
				// The era number is process-global (each Indicator gets the next one); everything after it must match.
				Assert::AreEqual(viaDirect.substr(viaDirect.find(' ')), viaExtension.substr(viaExtension.find(' ')));
				Assert::AreEqual<uint64_t>(12, r.resync.PulldownCursorSeq(), L"B2: the pulldown cursor follows the ingress");
			}

			TEST_METHOD(OneSealPerFrameInOnOnly)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				for (int64_t s = 1; s <= 3; ++s) { RE::IngressContext c = Frame(&r, s); ext.OnIngress(c); Begin(ext, &r, s); }
				Assert::AreEqual<uint64_t>(0, ResyncExtension::SealFor(&r)->Seals(), L"shadow never seals");
				Apply(ext, &r, "on");
				for (int64_t s = 4; s <= 8; ++s) { RE::IngressContext c = Frame(&r, s); ext.OnIngress(c); Begin(ext, &r, s); }
				Assert::AreEqual<uint64_t>(5, ResyncExtension::SealFor(&r)->Seals());
				Assert::AreEqual<uint64_t>(8, r.resyncDrive.LastSealed());
				Assert::IsTrue(r.resyncDrive.Registered() == r.resync.GetIndicator(), L"the Drive is registered by the seal's provider");
				Assert::IsFalse(ResyncExtension::StormHeld(&r));
			}

			TEST_METHOD(QueueCapacityResizesTheRingOnTheRenderThread)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "on");
				RE::QueueFacts q; q.host = &r; q.kind = RE::QueueEvent::Flush; q.generation = 2; q.frames = 64; q.text = "live queue reset";
				ext.OnQueue(q);
				RE::IngressContext c = Frame(&r, 1); c.queueGeneration = 2; ext.OnIngress(c);
				Begin(ext, &r, 1);
				Assert::IsTrue(r.resync.GetIndicator()->Capacity() >= VpIndicator::Indicator::RequiredCapacity(FramesInFlight(64, 0, 0, 2, 0)));
			}

			TEST_METHOD(LateSlipsReachTheShadow)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				for (int64_t s = 1; s <= 4; ++s) { RE::IngressContext c = Frame(&r, s); ext.OnIngress(c); }
				std::string before; r.resync.EventLines(before);
				RE::QueueFacts d; d.host = &r; d.kind = RE::QueueEvent::Drop; d.seq = 3; d.frames = 1; d.cause = Vpv::Resync::kDropDriftDrop;
				ext.OnQueue(d);
				std::string after; r.resync.EventLines(after);
				Assert::AreNotEqual(before, after);
			}

			TEST_METHOD(InjectUnarmedIsInert)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				RE::Context c; c.host = &r;
				std::string reply;
				Assert::IsTrue(ext.OnCommand(c, "resync", "inject full", reply));
				Assert::AreEqual(std::string("not armed"), reply);
				Assert::IsTrue(ext.OnCommand(c, "resync.inject", "seek", reply));
				Assert::AreEqual(std::string("not armed"), reply);
				Assert::IsFalse(ext.OnCommand(c, "dejudder", "inject full", reply), L"other modules are not answered");
				Assert::IsFalse(ext.OnCommand(c, "resync", "injection", reply));
			}

			TEST_METHOD(HostsAreKeyedAndReleasedWithTheirRenderer)
			{
				ResyncExtension ext;
				const size_t before = ResyncExtension::BoundHosts();
				const void* gone = nullptr;
				{
					FakeRenderer a, b;
					gone = &a;
					Assert::AreEqual(before + 2, ResyncExtension::BoundHosts());
					Assert::IsTrue(ResyncExtension::For(&a) != ResyncExtension::For(&b));
					Apply(ext, &a, "on");
					Assert::IsTrue(a.resync.Driving());
					Assert::IsFalse(b.resync.Active(), L"settings for one renderer never reach another");
				}
				Assert::AreEqual(before, ResyncExtension::BoundHosts());
				Assert::IsNull(ResyncExtension::For(gone));
				RE::IngressContext c = Frame(gone, 1);
				ext.OnIngress(c);   // a retired host: nothing happens
			}

			TEST_METHOD(SignalRateIsBillsValueUnlessDriving)
			{
				ResyncExtension ext;
				FakeRenderer r;
				RE::Context c; c.host = &r;
				Apply(ext, &r, "off");
				Assert::AreEqual(29.97, ext.OnResolve(RE::Resolve::SourceSignalRate, c, 29.97));
				Apply(ext, &r, "shadow");
				Assert::AreEqual(29.97, ext.OnResolve(RE::Resolve::SourceSignalRate, c, 29.97));
				Assert::AreEqual(60.0, ext.OnResolve(RE::Resolve::DisplayRefreshRate, c, 60.0));
				RE::Context none;
				Assert::AreEqual(25.0, ext.OnResolve(RE::Resolve::SourceSignalRate, none, 25.0));
			}

			TEST_METHOD(PulldownStepKeepsTodaysRules)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				Vpv::Resync::PulldownStep step = Vpv::Resync::PulldownResyncStep(&r, true);
				Assert::IsTrue(step.reset && !step.driving && !step.relatchTiming);
				step = Vpv::Resync::PulldownResyncStep(&r, false);
				Assert::IsFalse(step.reset || step.driving || step.relatchTiming);
				step = Vpv::Resync::PulldownResyncStep(nullptr, true);
				Assert::IsTrue(step.reset, L"no renderer: the request still resets, as before");
				Apply(ext, &r, "on");
				RE::IngressContext c = Frame(&r, 1); ext.OnIngress(c);
				step = Vpv::Resync::PulldownResyncStep(&r, false);
				Assert::IsTrue(step.driving);
				Assert::IsFalse(step.reset, L"entering on only latches");
				// A ring resize (new era) between two pulldown steps only re-latches: no reset from the restart.
				RE::QueueFacts q; q.host = &r; q.kind = RE::QueueEvent::Flush; q.generation = 1; q.frames = 64; q.text = "renderer reset";
				ext.OnQueue(q);
				Begin(ext, &r, 1);
				RE::IngressContext c2 = Frame(&r, 2); ext.OnIngress(c2);
				step = Vpv::Resync::PulldownResyncStep(&r, false);
				Assert::IsFalse(step.reset, L"an era change is a latch, not a reset");
			}

			TEST_METHOD(StatusLinesRoundTripIntoTheRendererLines)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				RE::IngressContext c = Frame(&r, 1); ext.OnIngress(c);
				const std::vector<std::string> lines = StatusLines(ext, &r, RE::StatusKind::Lines);
				Assert::AreEqual<size_t>(2, lines.size());
				VpStatusText::RendererLines<char> out;
				VpStatusText::SetExtensionLines(out, lines, [](const std::string& s) { return s; });
				std::string events;
				r.resync.EventLines(events);
				Assert::IsTrue(out.resync.known && out.resyncEvents.known);
				Assert::AreEqual(events, out.resyncEvents.text, L"shadow: the events line is unchanged");
				Assert::IsTrue(StatusLines(ext, &r, RE::StatusKind::Fallbacks).empty(), L"no fallback in shadow");
			}

			TEST_METHOD(RefusalCodesAreAccepted)
			{
				ResyncExtension ext;
				FakeRenderer r;
				Apply(ext, &r, "shadow");
				RE::LifecycleFacts f; f.host = &r; f.kind = RE::Lifecycle::StateRefused;
				f.code = Vpv::Resync::kRefusalInitRace; ext.OnLifecycle(f);
				f.code = Vpv::Resync::kRefusalSettingsRebuild; ext.OnLifecycle(f);
				f.code = Vpv::Resync::kRefusalClassify; ext.OnLifecycle(f);   // no states given: ignored
				RE::LifecycleFacts a; a.host = &r; a.kind = RE::Lifecycle::StateAccepted; a.subject = &r;
				ext.OnLifecycle(a);
				Assert::AreEqual(0ull, ResyncExtension::ContainedFaults(&r));
			}

			TEST_METHOD(ContainedFaultsAreVisible)
			{
				ResyncExtension ext;
				FakeRenderer r;
				const unsigned long long before = ResyncExtension::ContainedFaults(&r);
				std::vector<std::string> none;
				RE::Context c; c.host = &r;
				ext.OnStatus(c, RE::StatusKind::WorkerFault, none, "resync", "exception");
				Assert::AreEqual(before + 1, ResyncExtension::ContainedFaults(&r));
				const std::vector<std::string> fb = StatusLines(ext, &r, RE::StatusKind::Fallbacks);
				Assert::IsTrue(!fb.empty() && fb[0].compare(0, 13, "resync_fault:") == 0);
				std::string reply;
				Apply(ext, &r, "off");
				Assert::IsTrue(ext.OnCommand(c, "resync", "inject full", reply));
			}
		};
	}
}
