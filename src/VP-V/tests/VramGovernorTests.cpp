// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S2 VRAM governor (src/VP-V/core/VramGovernor.h): every VRAM user registers levels and a
// tier; the governor sheds by tier and order, visibly. Synthetic 4 GB card at 4K with all features
// on (figures from bug hunt doc 38, high case), the transient 2x early-upload pool, a 3090, the
// skeleton products as users, pressure/headroom over time and determinism. CPU only.

#include <VP-V/core/VramGovernor.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VramGovernorTestsNs
{
	using namespace VpSkeleton;

	constexpr uint64_t kMB = 1024ull * 1024ull;

	VramUser User(uint16_t id, const char* name, Tier tier, uint8_t order, std::vector<uint64_t> mb,
		std::vector<const char*> text = {})
	{
		VramUser u;
		u.id = id;
		u.name = name;
		u.tier = tier;
		u.shedOrder = order;
		for (uint64_t m : mb)
			u.levels.push_back(m * kMB);
		u.levelText = text;
		return u;
	}

	// The 4K feature set, high case of doc 38, plus the skeleton products (R4b form).
	std::vector<VramUser> AllFeatures4k()
	{
		std::vector<VramUser> u;
		u.push_back(User(kUserLibplacebo, "libplacebo intermediates", Tier::T0, 0, { 660 }));
		u.push_back(User(kUserCapturePlanes, "capture planes", Tier::T0, 0, { 200 }));
		u.push_back(User(kUserNvofProducer, "vector engine", Tier::T0, 0, { 200, 120 },
			{ nullptr, "at quarter input" }));
		u.push_back(User(kUserRenderAhead, "render-ahead", Tier::T2, 0, { 200, 133, 67, 0 },
			{ nullptr, "depth 6 -> 4", "depth 6 -> 2", "off" }));
		u.push_back(User(kUserEarlyUpload, "early upload", Tier::T2, 1, { 800, 600, 400, 200 },
			{ nullptr, "slots 16 -> 12", "slots 16 -> 8", "slots 16 -> 4" }));
		u.push_back(User(kUserInterpolator, "interpolator", Tier::T2, 3, { 300, 0 }));
		u.push_back(User(kUserTemporalDetail, "temporal detail", Tier::T1, 0, { 400, 224, 0 },
			{ nullptr, "without future slots", "off" }));
		u.push_back(User(kUserCameraModel, "camera model", Tier::T1, 1, { 100, 50 }));
		u.push_back(User(kUserEnhancers, "texture restore / detail pulse", Tier::T1, 2, { 300, 150, 0 }));
		std::vector<ProductDecl> d = FirstProducts().All();
		d[kP0RebaseDecision].backend = Backend::Cpu;
		const Registry reg(std::move(d));
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, kP6EntranceAhead);
		AddConsume(c, kP5MonitorTiles);
		GraphSettings gs;
		gs.dims.width = 3840;
		gs.dims.height = 2160;
		AddProductUsers(reg, BuildGraph(reg, { c }, gs), u);
		return u;
	}

	CardProfile Card4GB()
	{
		CardProfile c;
		c.key.luid = 9;
		c.vramBudgetBytes = 3300 * kMB;   // WDDM budget on the 4 GB 3050 Ti
		c.vramUsageBytes = 450 * kMB;     // desktop and other apps; VP not started
		c.ofaPresent = true;
		return c;
	}

	uint8_t TierRank(Tier t) { return uint8_t(t); }

	// The ordering invariant: a user is reduced only if every user that sheds before it is at its
	// minimum.
	void AssertShedOrder(const GovernorInput& in, const GovernorPlan& p)
	{
		for (size_t i = 0; i < in.users.size(); ++i) {
			if (p.grants[i].level == 0)
				continue;
			for (size_t j = 0; j < in.users.size(); ++j) {
				const VramUser& a = in.users[j];
				const VramUser& b = in.users[i];
				const bool shedsFirst = TierRank(a.tier) > TierRank(b.tier)
					|| (a.tier == b.tier && (a.shedOrder < b.shedOrder || (a.shedOrder == b.shedOrder && a.id < b.id)));
				if (shedsFirst)
					Assert::AreEqual(a.levels.size() - 1, p.grants[j].level, L"an earlier user is at its minimum");
			}
		}
	}

	TEST_CLASS(VramGovernorTests)
	{
	public:
		TEST_METHOD(FourGigabyteCardAt4kAllFeaturesFitsByShedding)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = AllFeatures4k();
			uint64_t want = 0;
			for (const VramUser& u : in.users)
				want += u.levels[0];
			const GovernorPlan p = PlanVram(in);
			Assert::IsTrue(want > p.budgetBytes, L"all features at full need do not fit");
			Assert::IsTrue(p.fits);
			Assert::IsTrue(p.totalBytes <= p.budgetBytes);
			// Fixed users are never cut; render-ahead goes first, then early upload.
			Assert::AreEqual(size_t(0), p.Find(kUserLibplacebo)->level);
			Assert::AreEqual(size_t(0), p.Find(kUserCapturePlanes)->level);
			Assert::IsTrue(p.Find(kUserRenderAhead)->shed);
			Assert::IsTrue(p.Find(kUserEarlyUpload)->reduced);
			AssertShedOrder(in, p);
			// Every cutback is visible, one line each.
			size_t reduced = 0;
			for (const UserGrant& g : p.grants)
				reduced += g.reduced ? 1u : 0u;
			Assert::AreEqual(reduced, p.lines.size());
			bool sawSlots = false;
			for (const std::string& l : p.lines)
				sawSlots = sawSlots || l.find("early upload slots 16 -> ") != std::string::npos;
			Assert::IsTrue(sawSlots);
			std::string all;
			for (const std::string& l : p.lines)
				all += l + "\n";
			Logger::WriteMessage(all.c_str());
		}

		TEST_METHOD(TransientPoolAtASizeChangeShedsMoreButStaysVisible)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = AllFeatures4k();
			const GovernorPlan calm = PlanVram(in);
			in.transientBytes = 800 * kMB;   // the old pool alongside the new one (doc 38 M3)
			const GovernorPlan tr = PlanVram(in);
			Assert::IsTrue(tr.steps > calm.steps);
			Assert::IsTrue(tr.lines.size() >= calm.lines.size());
			AssertShedOrder(in, tr);
			if (!tr.fits)
				Assert::AreEqual(std::string("Video memory: over budget even at minimum settings"), tr.lines.back());
		}

		TEST_METHOD(OwnUsageIsReplannedNotCountedTwice)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = AllFeatures4k();
			const GovernorPlan before = PlanVram(in);
			// VP running with 1.5 GB allocated: CurrentUsage includes it, the budget must not shrink.
			in.card.vramUsageBytes += 1500 * kMB;
			in.ownUsageBytes = 1500 * kMB;
			const GovernorPlan running = PlanVram(in);
			Assert::AreEqual(before.budgetBytes, running.budgetBytes);
			Assert::AreEqual(before.steps, running.steps);
		}

		TEST_METHOD(BigCardShedsNothing)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.card.vramBudgetBytes = 23000 * kMB;
			in.users = AllFeatures4k();
			const GovernorPlan p = PlanVram(in);
			Assert::IsTrue(p.fits);
			Assert::AreEqual(uint32_t(0), p.steps);
			Assert::IsTrue(p.lines.empty());
		}

		TEST_METHOD(ImpossibleBudgetSaysSo)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.card.vramBudgetBytes = 900 * kMB;   // fixed users alone exceed it
			in.users = AllFeatures4k();
			const GovernorPlan p = PlanVram(in);
			Assert::IsFalse(p.fits);
			Assert::AreEqual(std::string("Video memory: over budget even at minimum settings"), p.lines.back());
			for (size_t i = 0; i < in.users.size(); ++i)
				Assert::AreEqual(in.users[i].levels.size() - 1, p.grants[i].level, L"every user at its minimum");
		}

		TEST_METHOD(SkeletonProductsFallBackToInRender)
		{
			std::vector<VramUser> u = AllFeatures4k();
			const VramUser* p6 = nullptr;
			const VramUser* p4 = nullptr;
			for (const VramUser& x : u) {
				if (x.id == kUserProductBase + kP6EntranceAhead)
					p6 = &x;
				if (x.id == kUserProductBase + kP4SourceStats)
					p4 = &x;
			}
			Assert::IsNotNull(p6);
			Assert::IsNull(p4, L"system-RAM products are not VRAM users");
			Assert::AreEqual(size_t(2), p6->levels.size());
			Assert::IsTrue(p6->levels[1] < p6->levels[0]);
			Assert::IsTrue(p6->tier == Tier::T1);
		}

		TEST_METHOD(GovernorShrinksOnPressureAndRecoversAfterADwell)
		{
			VramGovernor gov;
			GovernorInput in;
			in.card = Card4GB();
			in.users = AllFeatures4k();
			const uint32_t calmSteps = gov.Update(in).steps;
			const double h0 = gov.Headroom();
			GovernorInput pressed = in;
			pressed.card.vramUsageBytes = pressed.card.vramBudgetBytes + kMB;   // over the WDDM budget
			pressed.ownUsageBytes = pressed.card.vramUsageBytes - 450 * kMB;
			gov.Update(pressed);
			Assert::IsTrue(gov.Headroom() < h0);
			const uint32_t tight = gov.Update(in).steps;
			Assert::IsTrue(tight >= calmSteps, L"the smaller headroom keeps more shed");
			for (int i = 0; i < 200; ++i)
				gov.Update(in);
			Assert::IsTrue(gov.Headroom() >= h0);
		}

		TEST_METHOD(SameInputsGiveTheSameGovernorPlan)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = AllFeatures4k();
			in.transientBytes = 300 * kMB;
			const GovernorPlan a = PlanVram(in);
			const GovernorPlan b = PlanVram(in);
			Assert::AreEqual(a.grants.size(), b.grants.size());
			for (size_t i = 0; i < a.grants.size(); ++i)
				Assert::AreEqual(a.grants[i].level, b.grants[i].level);
			Assert::IsTrue(a.lines == b.lines);
		}
	};
}
