// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// P13-DECL (core/RingContract.h, core/ReserveRows.h): the keyed-ring contract and the reserve rows are DECLARATIONS.
// They show bytes, take none, change no graph and no slot count, and the window they name is derived, never a constant.

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/ReserveRows.h>
#include <VP-V/core/RingContract.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ReserveRowsTestsNs
{
	using namespace VpSkeleton;

	static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

	Graph GraphWithP13Reader(uint8_t lag, uint8_t laneLead)
	{
		const Registry reg = SkeletonProducts();
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, kP13SourceRing, lag);
		GraphSettings gs;
		gs.dims.width = 64;
		gs.dims.height = 64;
		gs.laneLead = laneLead;
		return BuildGraph(reg, { c }, gs);
	}

	TEST_CLASS(ReserveRowsTests)
	{
	public:
		TEST_METHOD(TheRingContractIsKeyedDerivedAndPinsThree)
		{
			const KeyedRingContract c = SourceRingContract();
			Assert::IsTrue(c.keyed && c.backDerived && c.leadDerived);
			Assert::AreEqual(3, int(c.maxPins));
			const Registry reg = SkeletonProducts();
			Assert::IsTrue(reg.Get(kP13SourceRing).ring == c, L"the P13 declaration carries the contract");
			Assert::AreEqual(uint64_t(0), uint64_t(reg.Get(kP13SourceRing).size.fixedBytes), L"declaration only: 0 bytes");
			Assert::IsTrue(KeyedRingContract() != c, L"no other product claims it");
			for (ProductId i = 0; i < reg.Size(); ++i)
				if (i != kP13SourceRing)
					Assert::IsTrue(reg.Get(i).ring == KeyedRingContract(), L"only P13 declares a keyed ring");
		}

		TEST_METHOD(TheWindowIsBackPlusOnePlusLeadAndFollowsTheGraph)
		{
			Assert::AreEqual(uint32_t(5), RingWindowFrames(2, 2));
			const Graph a = GraphWithP13Reader(1, 1);
			const Graph b = GraphWithP13Reader(3, 4);
			GraphSettings sa; sa.laneLead = 1;
			GraphSettings sb; sb.laneLead = 4;
			uint32_t wa = 0, wb = 0;
			Assert::IsTrue(DerivedRingWindow(a, sa, &wa));
			Assert::IsTrue(DerivedRingWindow(b, sb, &wb));
			Assert::AreEqual(uint32_t(a.Find(kP13SourceRing)->historyFull) + 1u + 1u, wa);
			Assert::AreEqual(uint32_t(b.Find(kP13SourceRing)->historyFull) + 1u + 4u, wb);
			Assert::IsTrue(wb > wa, L"a deeper reader and a longer lead give a longer window: it is not a constant");
		}

		TEST_METHOD(NoReaderMeansNoWindow)
		{
			const Registry reg = SkeletonProducts();
			const std::vector<ConsumerDecl> none;
			GraphSettings gs;
			const Graph g = BuildGraph(reg, none, gs);
			uint32_t w = 77;
			Assert::IsFalse(DerivedRingWindow(g, gs, &w));
			Assert::AreEqual(uint32_t(77), w, L"left alone");
		}

		TEST_METHOD(ReserveRowsAreDeclaredNotAllocatedPerTier)
		{
			for (ReserveTier t : { ReserveTier::FullBudget, ReserveTier::ReducedBudget }) {
				const ReserveRow* ring = ReserveRingRow(t);
				Assert::IsNotNull(ring);
				Assert::AreEqual(3, int(ring->pins));
				Assert::IsTrue(ring->bytes4K > 0);
			}
			size_t n = 0;
			for (const ReserveRow& r : ReserveRows()) {
				Assert::IsTrue(r.kind == Catalog::FlagKind::DeclaredNotAllocated);
				++n;
			}
			Assert::IsTrue(n >= 8);
			Assert::AreEqual(uint64_t((8 + 3) * (kReserveLumaBytesPerFrame4K + kReserveChromaBytesPerFrame4K)), ReserveRingRow(ReserveTier::FullBudget)->bytes4K);
			Assert::IsTrue(ReserveRingRow(ReserveTier::ReducedBudget)->bytes4K < ReserveRingRow(ReserveTier::FullBudget)->bytes4K);
		}

		TEST_METHOD(TheTdRolesAreKtdIdsAndStayInsideTheCeiling)
		{
			const uint64_t full = ReserveTdBytes4K(ReserveTier::FullBudget);
			Assert::IsTrue(full > 0 && full <= kReserveTdCeilingBytes4K, L"about 450 MB at 4K, never over");
			Assert::IsTrue(ReserveTdBytes4K(ReserveTier::ReducedBudget) < full);
			size_t roles = 0;
			for (const ReserveRow& r : ReserveRows())
				if (r.tier == ReserveTier::FullBudget && r.product != kP13SourceRing) {
					Assert::IsTrue(r.product == kTdMemory || r.product == kTdMaps || r.product == kTdAnchorRefs || r.product == kTdDelta || r.product == kTdAnchorPass || r.product == kTdBlocks, L"a kTd* id");
					++roles;
				}
			Assert::AreEqual(size_t(6), roles);
		}

		TEST_METHOD(BytesScaleWithThePictureAndZeroMeansThePlanningSize)
		{
			const ReserveRow& r = *ReserveRingRow(ReserveTier::FullBudget);
			Dims uhd; uhd.width = 3840; uhd.height = 2160;
			Dims hd; hd.width = 1920; hd.height = 1080;
			Assert::AreEqual(r.bytes4K, ReserveRowBytes(r, uhd));
			Assert::AreEqual(r.bytes4K, ReserveRowBytes(r, Dims()));
			Assert::AreEqual(r.bytes4K / 4u, ReserveRowBytes(r, hd));
		}

		TEST_METHOD(ADerivedWindowOverTheReserveIsCappedVisibly)
		{
			bool capped = false;
			const uint32_t cap = ReserveRingRow(ReserveTier::ReducedBudget)->ringFrames;
			Assert::AreEqual(cap, ClampWindowToReserve(cap + 5u, ReserveTier::ReducedBudget, &capped));
			Assert::IsTrue(capped);
			Assert::AreEqual(uint32_t(2), ClampWindowToReserve(2, ReserveTier::ReducedBudget, &capped));
			Assert::IsFalse(capped);
		}

		TEST_METHOD(TheStatusLinesShowEachTierAsReservedNotAllocated)
		{
			Dims uhd; uhd.width = 3840; uhd.height = 2160;
			const std::vector<std::string> lines = ReserveStatusLines(uhd);
			Assert::AreEqual(size_t(2), lines.size());
			Assert::IsTrue(Has(lines[0], "full-budget tier") && Has(lines[1], "reduced-budget tier"));
			for (const std::string& l : lines) {
				Assert::IsTrue(Has(l, "reserved, not allocated"), L"visible, per tier");
				Assert::IsTrue(Has(l, "source ring") && Has(l, "temporal-detail store") && Has(l, "MB"));
				Assert::IsTrue(l.compare(0, 17, "Skeleton reserve,") == 0);
			}
			Assert::IsTrue(Has(ReserveStatusLines(Dims())[0], "4K planning size"));
		}

		TEST_METHOD(TheReserveDoesNotChangeTheGraph)
		{
			const Graph before = GraphWithP13Reader(2, 2);
			(void)ReserveRows();
			(void)ReserveStatusLines(Dims());
			const Graph after = GraphWithP13Reader(2, 2);
			Assert::AreEqual(int(before.Find(kP13SourceRing)->ring), int(after.Find(kP13SourceRing)->ring), L"slot count unchanged");
			Assert::IsTrue(before.nodes.size() == after.nodes.size());
		}
	};
}
