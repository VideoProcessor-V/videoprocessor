// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-5: R15 DecideIngress reaches the renderer registered under the call's host and nobody else; with none it is
// not handled (the host's pre-filled values stand); an unregistered renderer is shown under "Fallbacks engaged";
// through the hub, the decision's out fields (drop, gaps, film rate, arrival) reach the caller. CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/queue_timing/IngressPolicy.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;

		struct FakeRenderer
		{
			int calls = 0;
			bool drop = false;
			static bool Decide(void* self, RE::IngressContext& ic)
			{
				FakeRenderer& r = *static_cast<FakeRenderer*>(self);
				++r.calls;
				ic.drop = r.drop;
				ic.filmHz = 23.976;
				ic.gapBefore = true;
				ic.gapCause = "pulldown lock change";
				ic.arrivalQpc = 1234;
				return true;
			}
		};

		struct HubScope
		{
			struct Fwd : RE::IRendererExtension
			{
				bool OnFrame(RE::FrameContext& c) override { return Vpv::Hub::ExtensionHub::Instance().OnFrame(c); }
			};
			HubScope()
			{
				RE::ResetForTesting();
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new Vpv::Ingress::IngressPolicy()));
			}
			~HubScope()
			{
				RE::ResetForTesting();
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			}
		};
	}

	TEST_CLASS(IngressPolicyTests)
	{
	public:
		TEST_METHOD(NoRegisteredRendererLeavesTheHostValues)
		{
			HubScope hub;
			int key = 0;
			RE::IngressContext ic;
			ic.host = &key;
			ic.gapBefore = true;
			ic.gapCause = "missed frames";
			ic.arrivalQpc = 77;
			Assert::IsFalse(RE::DecideIngress(ic));
			Assert::IsFalse(ic.drop);
			Assert::IsTrue(ic.gapBefore);
			Assert::AreEqual(std::string("missed frames"), std::string(ic.gapCause));
			Assert::AreEqual<int64_t>(77, ic.arrivalQpc);
		}

		TEST_METHOD(TheDecisionReachesOnlyItsRendererAndComesBackToTheCaller)
		{
			HubScope hub;
			FakeRenderer a, b;
			b.drop = true;
			Vpv::Ingress::HostBinding bindA(&a, &FakeRenderer::Decide, &a);
			Vpv::Ingress::HostBinding bindB(&b, &FakeRenderer::Decide, &b);
			RE::IngressContext ic;
			ic.host = &a;
			Assert::IsTrue(RE::DecideIngress(ic));
			Assert::IsFalse(ic.drop, L"A keeps the frame");
			Assert::AreEqual(23.976, ic.filmHz);
			Assert::AreEqual<int64_t>(1234, ic.arrivalQpc);
			Assert::AreEqual(std::string("pulldown lock change"), std::string(ic.gapCause));
			RE::IngressContext id;
			id.host = &b;
			Assert::IsTrue(RE::DecideIngress(id) && id.drop, L"B drops its repeat");
			Assert::AreEqual(1, a.calls);
			Assert::AreEqual(1, b.calls);
		}

		TEST_METHOD(ARemovedRendererIsNeverReached)
		{
			HubScope hub;
			FakeRenderer a;
			{
				Vpv::Ingress::HostBinding bind(&a, &FakeRenderer::Decide, &a);
			}
			RE::IngressContext ic;
			ic.host = &a;
			Assert::IsFalse(RE::DecideIngress(ic));
			Assert::AreEqual(0, a.calls);
		}

		// R19: the renderer's VP-V target replaces Bill's for its host only; no renderer: Bill's value.
		TEST_METHOD(QueueTargetReachesOnlyItsRenderer)
		{
			HubScope hub;
			struct Target
			{
				static void Five(const void*, RE::QueueTargetContext& qc) { qc.target = 5; }
			};
			FakeRenderer a;
			int other = 0;
			Vpv::Ingress::HostBinding bind(&a, &FakeRenderer::Decide, &a, &Target::Five);
			RE::QueueTargetContext qc;
			qc.capacity = 8;
			Assert::AreEqual<size_t>(5, RE::ResolveQueueTarget(&a, qc, 4), L"the renderer's target");
			RE::QueueTargetContext qo;
			qo.capacity = 8;
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&other, qo, 4), L"another host keeps Bill's value");
			Vpv::Ingress::HostBinding noTarget(&other, &FakeRenderer::Decide, &other);
			RE::QueueTargetContext qn;
			qn.capacity = 8;
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&other, qn, 4), L"a binding without a target function keeps it too");
		}

		TEST_METHOD(AFullTableIsVisibleUnderFallbacks)
		{
			std::vector<std::unique_ptr<FakeRenderer>> renderers;
			std::vector<std::unique_ptr<Vpv::Ingress::HostBinding>> bindings;
			const size_t before = Vpv::Ingress::UnboundRenderers();
			for (int i = 0; i < 64; ++i)
			{
				renderers.emplace_back(new FakeRenderer());
				bindings.emplace_back(new Vpv::Ingress::HostBinding(renderers.back().get(), &FakeRenderer::Decide, renderers.back().get()));
				if (!bindings.back()->Registered())
					break;
			}
			Assert::IsFalse(bindings.back()->Registered(), L"the table is bounded");
			Assert::AreEqual(before + 1, Vpv::Ingress::UnboundRenderers());
			Vpv::Ingress::IngressPolicy module;
			std::vector<std::string> lines;
			module.OnStatus(RE::Context(), RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::AreEqual<size_t>(1, lines.size());
			Assert::IsTrue(lines[0].find("pulldown detection") != std::string::npos);
			bindings.clear();
			Assert::AreEqual(before, Vpv::Ingress::UnboundRenderers(), L"released with the renderer");
		}
	};
}
