/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// repair.stack (WP9): the repair-stack order TD -> deband -> contrast recovery as an explicit declaration. Order table, the earlier text
// order, today's behaviour while temporal detail is absent, the plan and the visible fallback status per temporal-detail state, the
// agreement with the catalog's pathway order and the debander's plan. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/repair/stack/RepairStackOrder.h>

#include <array>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace RepairStackNs
	{
		using namespace RepairStack;

		// The layers of `order` that are present, in the order's sequence.
		std::vector<Layer> Filter(const std::array<Layer, kLayerCount>& order, bool temporalDetail)
		{
			std::vector<Layer> out;
			for (Layer l : order)
				if (l != Layer::TemporalDetail || temporalDetail)
					out.push_back(l);
			return out;
		}

		std::vector<Layer> Of(const Plan& p) { return std::vector<Layer>(p.layers.begin(), p.layers.begin() + p.count); }

		TEST_CLASS(RepairStackOrderTests)
		{
		public:
			TEST_METHOD(TheDeclaredOrderIsTemporalDetailThenDebandThenContrastRecovery)
			{
				const std::array<Layer, kLayerCount>& o = Order();
				Assert::IsTrue(o[0] == Layer::TemporalDetail && o[1] == Layer::Deband && o[2] == Layer::ContrastRecovery);
				Assert::AreEqual(std::string("temporal detail -> deband -> contrast recovery"), OrderText());
				Assert::AreEqual(size_t(0), Rank(Layer::TemporalDetail));
				Assert::AreEqual(size_t(1), Rank(Layer::Deband));
				Assert::AreEqual(size_t(2), Rank(Layer::ContrastRecovery));
				Assert::IsTrue(Precedes(Layer::TemporalDetail, Layer::Deband));
				Assert::IsTrue(Precedes(Layer::Deband, Layer::ContrastRecovery));
				Assert::IsTrue(Precedes(Layer::TemporalDetail, Layer::ContrastRecovery));
				Assert::IsFalse(Precedes(Layer::Deband, Layer::TemporalDetail), L"deband is no longer ahead of temporal detail");
				Assert::IsFalse(Precedes(Layer::Deband, Layer::Deband));
				// Every layer exactly once, each with a name.
				std::vector<int> seen(kLayerCount, 0);
				for (Layer l : o)
				{
					++seen[size_t(l)];
					Assert::IsTrue(std::string(Name(l)) != "?" && !std::string(Name(l)).empty());
				}
				for (int n : seen)
					Assert::AreEqual(1, n);
			}

			TEST_METHOD(TheEarlierTextOrderDiffersOnlyInThePositionOfTemporalDetail)
			{
				const std::array<Layer, kLayerCount>& legacy = LegacyOrder();
				Assert::AreEqual(std::string("deband -> temporal detail -> contrast recovery"), OrderText(legacy));
				Assert::IsTrue(legacy[0] == Layer::Deband && legacy[1] == Layer::TemporalDetail && legacy[2] == Layer::ContrastRecovery);
				// Without temporal detail the two declarations are the same sequence: deband then contrast recovery.
				const std::vector<Layer> now = Filter(Order(), false);
				const std::vector<Layer> before = Filter(legacy, false);
				Assert::AreEqual(size_t(2), now.size());
				Assert::IsTrue(now == before, L"temporal detail absent: the layers keep their order, today's behaviour is unchanged");
				Assert::IsTrue(now[0] == Layer::Deband && now[1] == Layer::ContrastRecovery);
				// With it, only temporal detail moved.
				Assert::IsFalse(Filter(Order(), true) == Filter(legacy, true));
				Assert::IsTrue(Precedes(Layer::Deband, Layer::ContrastRecovery) == (Rank(Layer::Deband) < Rank(Layer::ContrastRecovery)));
			}

			TEST_METHOD(NotRequestedIsTodaysBehaviourWithNothingToSay)
			{
				// requested = false whatever the products say (today: no key asks for temporal detail).
				for (bool usable : { false, true })
				{
					const Plan p = Decide(false, usable, "ignored");
					Assert::IsTrue(Of(p) == Filter(Order(), false), L"deband, contrast recovery");
					Assert::IsFalse(p.temporalDetailRuns || p.debandIsFallbackLayer);
					Assert::AreEqual(std::string(), p.status, L"not a fallback: no line, byte-identical");
					Assert::IsFalse(p.Has(Layer::TemporalDetail));
					Assert::IsTrue(p.Has(Layer::Deband) && p.Has(Layer::ContrastRecovery));
				}
			}

			TEST_METHOD(RequestedAndUsableRunsTemporalDetailFirst)
			{
				const Plan p = Decide(true, true, "");
				Assert::IsTrue(Of(p) == std::vector<Layer>(Order().begin(), Order().end()));
				Assert::IsTrue(p.temporalDetailRuns);
				Assert::IsFalse(p.debandIsFallbackLayer);
				Assert::AreEqual(std::string(), p.status);
			}

			TEST_METHOD(RequestedButUnusableMakesDebandTheVisibleFallbackLayer)
			{
				const Plan p = Decide(true, false, "td.maps absent; coding_evidence stale");
				Assert::IsTrue(Of(p) == Filter(Order(), false), L"deband everywhere, then contrast recovery: today's pathway");
				Assert::IsFalse(p.temporalDetailRuns);
				Assert::IsTrue(p.debandIsFallbackLayer);
				Assert::IsTrue(p.Has(Layer::Deband) && !p.Has(Layer::TemporalDetail));
				Assert::IsTrue(p.status.find("temporal detail -> deband -> contrast recovery") != std::string::npos, L"the order is on the line");
				Assert::IsTrue(p.status.find("td.maps absent; coding_evidence stale") != std::string::npos, L"the reason is on the line");
				Assert::IsTrue(p.status.find("fallback layer") != std::string::npos);
				Assert::IsTrue(p.status.find("FL35") == std::string::npos);
				// No reason given: still a visible line.
				const Plan bare = Decide(true, false, "");
				Assert::IsTrue(bare.debandIsFallbackLayer && !bare.status.empty());
				Assert::IsTrue(bare.status.find("()") == std::string::npos);
				// It recovers by itself.
				Assert::IsTrue(Decide(true, true, "").status.empty());
			}

			TEST_METHOD(TheCatalogPathwayOrderAgreesWithTheDeclaredOrder)
			{
				using namespace VpSkeleton::Catalog;
				auto keyOf = [](const char* name) {
					for (const ConsumerRow& c : ConsumerRows())
						if (std::string(c.decl.name) == name)
							return PathwayKey(c);
					return uint32_t(0xFFFFFFFFu);
				};
				const uint32_t td = keyOf("temporal detail");
				const uint32_t deband = keyOf("debander (with hold)");
				const uint32_t cr = keyOf("contrast recovery need");
				Assert::IsTrue(td != 0xFFFFFFFFu && deband != 0xFFFFFFFFu && cr != 0xFFFFFFFFu, L"the three rows exist");
				Assert::IsTrue(td < deband, L"temporal detail before deband");
				Assert::IsTrue(deband < cr, L"deband before contrast recovery");
				// All three are in the repair unit of the pathway.
				for (const ConsumerRow& c : ConsumerRows())
					if (PathwayKey(c) == td || PathwayKey(c) == deband || PathwayKey(c) == cr)
						Assert::IsTrue(c.unit == PathwayUnit::Repair);
			}

			TEST_METHOD(TheDebanderDeclaresTodaysPlanWhileNothingIsRequested)
			{
				VpDebander::Module deband;
				pl_frame src{}, dst{};
				Assert::IsFalse(deband.Apply(nullptr, nullptr, src, VpDebander::Params(), dst), L"nothing to deband");
				const Plan p = deband.StackPlan();
				Assert::IsTrue(Of(p) == Filter(Order(), false));
				Assert::IsFalse(p.debandIsFallbackLayer);
				Assert::AreEqual(std::string(), deband.StackStatusLine(), L"no line while temporal detail is not requested");
			}
		};
	}
}
