// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton SA1 GPU allocator core (src/VP-V/core/GpuAllocator.h, ScratchArena.h): the scratch arena
// plan (max over scopes, order independent), deterministic Acquire, graceful and visible fallbacks,
// the arena as a governor user, Transient residency, product textures (ring + live) and the VRAM
// ledger. A fake backend creates integer handles; CPU only, no GPU.

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GpuAllocatorTestsNs
{
	using namespace VpSkeleton;

	constexpr uint64_t kMB = 1024ull * 1024ull;

	struct FakeBackend
	{
		using Handle = int;
		int next = 0;
		int created = 0;
		int destroyed = 0;
		int failAfter = -1;   // Create fails once this many textures were created
		std::map<int, TexShape> shapes;

		Handle Create(const TexShape& s)
		{
			if (failAfter >= 0 && created >= failAfter)
				return 0;
			++created;
			const int h = ++next;
			shapes[h] = s;
			return h;
		}
		void Destroy(Handle& h)
		{
			if (h != 0)
				++destroyed;
			h = 0;
		}
	};

	TexShape Shape(uint32_t w, uint32_t h, TexFormat f)
	{
		TexShape s;
		s.w = w;
		s.h = h;
		s.fmt = f;
		return s;
	}

	Dims MkDims(uint32_t w, uint32_t h)
	{
		Dims d;
		d.width = w;
		d.height = h;
		return d;
	}

	ArenaRequest Req(uint16_t scope, const char* owner, Tier tier, const TexShape& s, uint8_t count)
	{
		ArenaRequest r;
		r.scope = scope;
		r.owner = owner;
		r.tier = tier;
		r.shape = s;
		r.count = count;
		return r;
	}

	bool SamePlan(const ArenaPlan& a, const ArenaPlan& b)
	{
		if (a.pools.size() != b.pools.size() || a.admitted.size() != b.admitted.size())
			return false;
		for (size_t i = 0; i < a.pools.size(); ++i)
			if (a.pools[i].shape != b.pools[i].shape || a.pools[i].capacity != b.pools[i].capacity)
				return false;
		for (size_t i = 0; i < a.admitted.size(); ++i) {
			const ArenaRequest& x = a.admitted[i];
			const ArenaRequest& y = b.admitted[i];
			if (x.scope != y.scope || std::string(x.owner) != std::string(y.owner) || x.shape != y.shape
				|| x.count != y.count || x.tier != y.tier)
				return false;
		}
		return a.bytes == b.bytes && a.maxScopeBytes == b.maxScopeBytes && a.sumAllBytes == b.sumAllBytes
			&& a.shedRequests == b.shedRequests;
	}

	ProductTexLayout P10Layout()
	{
		ProductTexLayout l;
		TexTerm a;
		a.shift = 0;
		a.fmt = TexFormat::RGBA32F;
		TexTerm b;
		b.shift = 4;
		b.fmt = TexFormat::RGBA32F;
		l.terms.push_back(a);
		l.terms.push_back(b);
		return l;
	}

	CardProfile Card4GB()
	{
		CardProfile c;
		c.key.luid = 1;
		c.vramBudgetBytes = 4096 * kMB;
		c.vramUsageBytes = 800 * kMB;
		c.systemRamFreeBytes = 16000 * kMB;
		c.computeScale = 1.0;
		c.uploadGBps = 12.0;
		c.ofaPresent = true;
		return c;
	}

	CardProfile Card3090()
	{
		CardProfile c;
		c.key.luid = 2;
		c.vramBudgetBytes = 24576 * kMB;
		c.vramUsageBytes = 1500 * kMB;
		c.systemRamFreeBytes = 32000 * kMB;
		c.computeScale = 5.0;
		c.uploadGBps = 12.0;
		c.ofaPresent = true;
		return c;
	}

	// The grain split's layouts as GrainSplit::Gpu::GrainLayout builds them (CPU copy: no libplacebo here): the per-pixel
	// terms are relative, the block grid is ABSOLUTE at ceil(w / B) x ceil(h / B) of the runtime block edge B.
	ProductTexLayout GrainLayoutCpu(bool compact, Dims d, uint32_t B)
	{
		ProductTexLayout l;
		TexTerm t;
		t.shift = 0;
		if (compact) {
			t.fmt = TexFormat::RG16F;
			l.terms.push_back(t);
			t.fmt = TexFormat::RG16;
			l.terms.push_back(t);
		} else {
			t.fmt = TexFormat::RGBA32F;
			l.terms.push_back(t);
		}
		TexTerm b;
		b.shift = 4;
		b.fmt = TexFormat::RGBA32F;
		b.absW = (d.width + B - 1) / B;
		b.absH = (d.height + B - 1) / B;
		l.terms.push_back(b);
		return l;
	}

	Registry P10Registry(bool transient, bool compact = false)
	{
		std::vector<ProductDecl> d = SkeletonProducts(compact).All();
		if (transient)
			d[kP10GrainSplit].residency = Residency::Transient;
		return Registry(std::move(d));
	}

	Graph P10Graph(const Registry& reg, Dims dims)
	{
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, kP10GrainSplit);
		GraphSettings gs;
		gs.dims = dims;
		gs.renderAhead = 2;
		gs.laneLead = 1;
		gs.ofaPresent = true;
		return BuildGraph(reg, { c }, gs);
	}

	// Requests derived from the source dimensions (nothing hard-coded).
	std::vector<ArenaRequest> RequestsFor(Dims d)
	{
		std::vector<ArenaRequest> r;
		r.push_back(Req(1, "grain", Tier::T1, Shape(d.width / 2, d.height / 2, TexFormat::RG32F), 2));
		r.push_back(Req(1, "bands", Tier::T2, Shape(d.width, d.height, TexFormat::R32F), 1));
		r.push_back(Req(2, "quiet", Tier::T0, Shape(d.width / 4, d.height / 4, TexFormat::R16F), 3));
		return r;
	}

	uint64_t SumLevel0(const std::vector<VramUser>& users)
	{
		uint64_t t = 0;
		for (const VramUser& u : users)
			t += u.levels.empty() ? 0 : u.levels[0];
		return t;
	}

	VramUser FixedUser(uint16_t id, const char* name, uint64_t bytes)
	{
		VramUser u;
		u.id = id;
		u.name = name;
		u.tier = Tier::T0;
		u.levels.push_back(bytes);
		u.levelText.push_back(nullptr);
		return u;
	}

	TEST_CLASS(GpuAllocatorTests)
	{
	public:
		TEST_METHOD(TexFormatBytes)
		{
			const TexFormat f[] = { TexFormat::R8, TexFormat::R16, TexFormat::RG16, TexFormat::R16F, TexFormat::RG16F,
				TexFormat::RGBA16F, TexFormat::R32F, TexFormat::RG32F, TexFormat::RGBA32F };
			const uint32_t want[] = { 1, 2, 4, 2, 4, 8, 4, 8, 16 };
			for (size_t i = 0; i < 9; ++i)
				Assert::AreEqual(want[i], BytesPerTexel(f[i]), L"bytes per texel of each format");
			Assert::AreEqual(uint64_t(100 * 50 * 8), ShapeBytes(Shape(100, 50, TexFormat::RGBA16F)), L"shape bytes");
			Assert::AreEqual(uint64_t(0), ShapeBytes(Shape(0, 50, TexFormat::R8)), L"empty shape is free");
		}

		TEST_METHOD(ArenaPlanIsMaxOverScopes)
		{
			const TexShape s1 = Shape(64, 64, TexFormat::R32F);
			const TexShape s2 = Shape(32, 32, TexFormat::RG16F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "a", Tier::T0, s1, 2));
			r.push_back(Req(1, "a", Tier::T0, s2, 1));
			r.push_back(Req(2, "b", Tier::T0, s1, 1));
			r.push_back(Req(2, "b", Tier::T0, s2, 3));
			const ArenaPlan p = PlanArena(r, Tier::T2);
			Assert::IsNotNull(p.FindPool(s1), L"pool for S1");
			Assert::IsNotNull(p.FindPool(s2), L"pool for S2");
			Assert::AreEqual(uint32_t(2), p.FindPool(s1)->capacity, L"S1 capacity is the busiest scope");
			Assert::AreEqual(uint32_t(3), p.FindPool(s2)->capacity, L"S2 capacity is the busiest scope");
			const uint64_t b1 = ShapeBytes(s1), b2 = ShapeBytes(s2);
			Assert::AreEqual(2 * b1 + 3 * b2, p.bytes, L"arena bytes = sum capacity x bytes");
			const uint64_t scopeA = 2 * b1 + b2, scopeB = b1 + 3 * b2;
			Assert::AreEqual(std::max(scopeA, scopeB), p.maxScopeBytes, L"busiest scope bytes");
			Assert::AreEqual(scopeA + scopeB, p.sumAllBytes, L"sum of all scopes");
			Assert::IsTrue(p.sumAllBytes > p.bytes, L"sharing saves memory");
		}

		TEST_METHOD(ArenaPlanIsOrderIndependent)
		{
			std::vector<ArenaRequest> base;
			base.push_back(Req(1, "alpha", Tier::T0, Shape(64, 32, TexFormat::R32F), 2));
			base.push_back(Req(2, "bravo", Tier::T1, Shape(32, 32, TexFormat::RG16F), 3));
			base.push_back(Req(1, "charlie", Tier::T2, Shape(16, 16, TexFormat::R8), 1));
			base.push_back(Req(3, "delta", Tier::T1, Shape(64, 32, TexFormat::R32F), 4));
			base.push_back(Req(2, "echo", Tier::T0, Shape(8, 8, TexFormat::RGBA16F), 2));
			const ArenaPlan ref = PlanArena(base, Tier::T1);
			Assert::AreEqual(uint32_t(1), ref.shedRequests, L"one T2 request shed");
			std::vector<size_t> idx(base.size());
			std::iota(idx.begin(), idx.end(), size_t(0));
			size_t perms = 0;
			do {
				std::vector<ArenaRequest> r;
				for (size_t i : idx)
					r.push_back(base[i]);
				Assert::IsTrue(SamePlan(ref, PlanArena(r, Tier::T1)), L"plan equals for every request order");
				++perms;
			} while (std::next_permutation(idx.begin(), idx.end()));
			Assert::AreEqual(size_t(120), perms, L"all permutations were tried");
		}

		TEST_METHOD(ArenaAcquireIsDeterministic)
		{
			const TexShape s = Shape(64, 64, TexFormat::R32F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "x", Tier::T0, s, 3));
			r.push_back(Req(2, "y", Tier::T0, s, 2));
			const ArenaPlan plan = PlanArena(r, Tier::T2);
			std::vector<int> seq[2];
			for (int run = 0; run < 2; ++run) {
				FakeBackend be;
				ScratchArena<FakeBackend> arena(be);
				Assert::IsTrue(arena.Reserve(plan), L"reserve succeeds");
				Assert::AreEqual(size_t(3), arena.PoolSize(s), L"pool holds the busiest scope's count");
				arena.BeginFrame();
				arena.BeginScope(1);
				for (int k = 0; k < 3; ++k)
					seq[run].push_back(arena.Acquire("x", s));
				arena.EndScope();
				arena.BeginScope(2);
				const int first = arena.Acquire("y", s);
				const int second = arena.Acquire("y", s);
				Assert::AreEqual(seq[run][0], first, L"scope 2 reuses pool slot 0");
				Assert::AreEqual(seq[run][1], second, L"scope 2 reuses pool slot 1");
				arena.EndScope();
				Assert::AreEqual(1, seq[run][0], L"first acquire gets the first pooled handle");
				Assert::AreEqual(2, seq[run][1], L"second acquire gets the second pooled handle");
				Assert::AreEqual(3, seq[run][2], L"third acquire gets the third pooled handle");
			}
			Assert::IsTrue(seq[0] == seq[1], L"two arenas built alike give the same handles");
		}

		TEST_METHOD(ArenaExhaustedReturnsNullAndEngagesFallback)
		{
			const TexShape s = Shape(32, 32, TexFormat::RG16F);
			FakeBackend be;
			ScratchArena<FakeBackend> arena(be);
			std::vector<ArenaRequest> r2;
			r2.push_back(Req(1, "xowner", Tier::T0, s, 2));
			Assert::IsTrue(arena.Reserve(PlanArena(r2, Tier::T2)), L"reserve capacity 2");
			Assert::AreEqual(2, be.created, L"two textures created");
			arena.BeginFrame();
			arena.BeginScope(1);
			Assert::IsTrue(arena.Acquire("xowner", s) != 0, L"first acquire");
			Assert::IsTrue(arena.Acquire("xowner", s) != 0, L"second acquire");
			Assert::AreEqual(0, arena.Acquire("xowner", s), L"third acquire is null");
			Assert::AreEqual(uint64_t(1), arena.Exhausted(), L"exhausted counted");
			const FallbackCatalog::Decision d = arena.FrameDecision();
			Assert::IsTrue(d.engaged, L"fallback engaged");
			Assert::AreEqual(std::string("scratch_arena"), d.id, L"fallback id");
			Assert::IsTrue(d.reason.find("scratch memory short") != std::string::npos, L"reason says memory short");
			Assert::IsTrue(d.reason.find("xowner") != std::string::npos, L"reason names the owner");
			arena.EndScope();
			// A frame that does not exhaust releases the fallback by itself.
			arena.BeginFrame();
			arena.BeginScope(1);
			arena.Acquire("xowner", s);
			arena.Acquire("xowner", s);
			arena.EndScope();
			Assert::IsFalse(arena.FrameDecision().engaged, L"released on a good frame");
			// A bigger plan recovers; existing textures are kept.
			std::vector<ArenaRequest> r3;
			r3.push_back(Req(1, "xowner", Tier::T0, s, 3));
			Assert::IsTrue(arena.Reserve(PlanArena(r3, Tier::T2)), L"reserve capacity 3");
			Assert::AreEqual(3, be.created, L"only one new texture created");
			Assert::AreEqual(0, be.destroyed, L"nothing destroyed on growth");
			arena.BeginFrame();
			arena.BeginScope(1);
			for (int k = 0; k < 3; ++k)
				Assert::IsTrue(arena.Acquire("xowner", s) != 0, L"all three acquires succeed after recovery");
			Assert::IsFalse(arena.FrameDecision().engaged, L"no fallback after recovery");
		}

		TEST_METHOD(ArenaShedTierReturnsNull)
		{
			const TexShape s1 = Shape(32, 32, TexFormat::R32F);
			const TexShape s2 = Shape(48, 16, TexFormat::R16F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "alpha", Tier::T0, s1, 1));
			r.push_back(Req(1, "bravo", Tier::T2, s2, 1));
			FakeBackend be;
			ScratchArena<FakeBackend> arena(be);
			Assert::IsTrue(arena.Reserve(PlanArena(r, Tier::T0)), L"reserve T0 plan");
			arena.BeginFrame();
			arena.BeginScope(1);
			Assert::AreEqual(0, arena.Acquire("bravo", s2), L"the shed T2 owner gets null");
			Assert::AreEqual(uint64_t(1), arena.Shed(), L"shed counted");
			const FallbackCatalog::Decision d = arena.FrameDecision();
			Assert::IsTrue(d.engaged, L"fallback engaged");
			Assert::IsTrue(d.reason.find("bravo") != std::string::npos, L"names the shed owner");
			Assert::IsTrue(d.reason.find("alpha") == std::string::npos, L"does not name the served owner");
			Assert::IsTrue(arena.Acquire("alpha", s1) != 0, L"the T0 owner is still served");
		}

		TEST_METHOD(ArenaMisuseIsCounted)
		{
			const TexShape s = Shape(32, 32, TexFormat::R32F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "x", Tier::T0, s, 2));
			FakeBackend be;
			ScratchArena<FakeBackend> arena(be);
			Assert::IsTrue(arena.Reserve(PlanArena(r, Tier::T2)), L"reserve");
			arena.BeginFrame();
			Assert::AreEqual(0, arena.Acquire("x", s), L"outside a scope");
			Assert::AreEqual(uint64_t(1), arena.OutsideScopeAcquires(), L"outside-scope counted");
			arena.BeginScope(1);
			int fromThread = -1;
			std::thread t([&]() { fromThread = arena.Acquire("x", s); });
			t.join();
			Assert::AreEqual(0, fromThread, L"another thread gets null");
			Assert::AreEqual(uint64_t(1), arena.CrossThreadAcquires(), L"cross-thread counted");
			Assert::IsTrue(arena.FrameDecision().engaged && arena.FrameDecision().reason.find("another thread") != std::string::npos,
				L"a cross-thread use is visible");
			bool scopeFromThread = true;
			std::thread t2([&]() { scopeFromThread = arena.BeginScope(2); });
			t2.join();
			Assert::IsFalse(scopeFromThread, L"another thread cannot open a scope on the owner's arena");
			Assert::AreEqual(uint64_t(2), arena.CrossThreadAcquires(), L"counted");
			Assert::AreEqual(uint64_t(0), arena.Acquired(), L"pools untouched by the foreign thread");
			Assert::AreEqual(1, arena.Acquire("x", s), L"the render thread still gets slot 0");
			Assert::AreEqual(0, arena.Acquire("nobody", s), L"unrequested owner gets null");
			Assert::AreEqual(0, arena.Acquire("x", Shape(7, 7, TexFormat::R8)), L"unrequested shape gets null");
			Assert::AreEqual(uint64_t(2), arena.Shed(), L"unrequested acquires counted as shed");
			arena.EndScope();
			// Audit: a non-owner can neither re-Reserve nor start a frame on the owner's arena; Unbind hands it over.
			const int createdBefore = be.created;
			bool reserved = true;
			std::thread t3([&]() { reserved = arena.Reserve(PlanArena(r, Tier::T2)); arena.BeginFrame(); });
			t3.join();
			Assert::IsFalse(reserved, L"another thread's Reserve is refused");
			Assert::AreEqual(createdBefore, be.created, L"and touches nothing");
			Assert::AreEqual(uint64_t(4), arena.CrossThreadAcquires(), L"Reserve and BeginFrame counted");
			arena.Unbind();
			std::thread t4([&]() { reserved = arena.Reserve(PlanArena(r, Tier::T2)); });
			t4.join();
			Assert::IsTrue(reserved, L"after Unbind another thread may take the arena");
			Assert::IsTrue(arena.BoundToOtherThread(), L"now owned by that thread");
			// An arena never bound: an Acquire is outside a scope, not a cross-thread use.
			ScratchArena<FakeBackend> fresh(be);
			Assert::AreEqual(0, fresh.Acquire("x", s), L"null");
			Assert::AreEqual(uint64_t(1), fresh.OutsideScopeAcquires(), L"counted as outside a scope");
			Assert::AreEqual(uint64_t(0), fresh.CrossThreadAcquires(), L"not as another thread");
		}

		TEST_METHOD(ArenaReserveFailureIsVisible)
		{
			const TexShape s = Shape(32, 32, TexFormat::R32F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "x", Tier::T0, s, 3));
			FakeBackend be;
			be.failAfter = 1;
			ScratchArena<FakeBackend> arena(be);
			Assert::IsFalse(arena.Reserve(PlanArena(r, Tier::T2)), L"reserve reports the failure");
			Assert::IsTrue(arena.CreateFailures() > 0, L"create failure counted");
			Assert::AreEqual(size_t(1), arena.PoolSize(s), L"the pool is short");
			const int createdBefore = be.created;
			arena.BeginFrame();
			arena.BeginScope(1);
			Assert::IsTrue(arena.Acquire("x", s) != 0, L"the one texture that exists is served");
			Assert::AreEqual(0, arena.Acquire("x", s), L"the missing texture is null");
			Assert::AreEqual(uint64_t(1), arena.Exhausted(), L"exhausted counted");
			Assert::IsTrue(arena.FrameDecision().engaged, L"fallback engaged");
			be.failAfter = -1;
			Assert::AreEqual(0, arena.Acquire("x", s), L"still null: Acquire never allocates");
			Assert::AreEqual(createdBefore, be.created, L"Acquire created nothing");
		}

		TEST_METHOD(ArenaGovernorLevels)
		{
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "a", Tier::T0, Shape(64, 64, TexFormat::R32F), 1));
			r.push_back(Req(1, "b", Tier::T1, Shape(128, 128, TexFormat::R32F), 1));
			r.push_back(Req(1, "c", Tier::T2, Shape(256, 256, TexFormat::R32F), 1));
			const uint64_t p0 = PlanArena(r, Tier::T0).bytes, p1 = PlanArena(r, Tier::T1).bytes,
				p2 = PlanArena(r, Tier::T2).bytes;
			// Two users: the T0 temporaries and the T1/T2 increment.
			const VramUser u0 = ScratchArenaT0User(r);
			const VramUser ux = ScratchArenaExtraUser(r);
			Assert::AreEqual(uint16_t(kUserScratchArena), u0.id, L"T0 arena user id");
			Assert::AreEqual(uint16_t(kUserScratchArenaExtra), ux.id, L"extra arena user id");
			Assert::IsTrue(u0.tier == Tier::T0 && ux.tier == Tier::T1, L"tiers T0 and T1");
			Assert::IsFalse(u0.lossless || ux.lossless, L"arena levels are not lossless");
			Assert::AreEqual(size_t(2), u0.levels.size(), L"T0 user: plan and off");
			Assert::AreEqual(p0, u0.levels[0], L"T0 user level 0 is the T0 plan");
			Assert::AreEqual(uint64_t(0), u0.levels[1], L"T0 user last level is off");
			Assert::AreEqual(size_t(3), ux.levels.size(), L"extra user: three levels");
			Assert::AreEqual(p2 - p0, ux.levels[0], L"extra level 0 is plan(T2) - plan(T0)");
			Assert::AreEqual(p1 - p0, ux.levels[1], L"extra level 1 is plan(T1) - plan(T0)");
			Assert::AreEqual(uint64_t(0), ux.levels[2], L"extra last level is off");
			Assert::IsTrue(std::string(ux.levelText[1]).find("T2 temporaries off") != std::string::npos, L"level 1 text");
			// Equal plans: duplicate levels are dropped; nothing optional = a single zero level.
			std::vector<ArenaRequest> only0;
			only0.push_back(Req(1, "a", Tier::T0, Shape(64, 64, TexFormat::R32F), 1));
			Assert::AreEqual(size_t(1), ScratchArenaExtraUser(only0).levels.size(), L"no optional temporaries: one level");
			// Grants (T0 grant, extra grant).
			ArenaGrant g = ArenaGrantFor(r, p0, p2 - p0);
			Assert::IsTrue(g.any && g.maxTier == Tier::T2, L"full grant keeps T2");
			g = ArenaGrantFor(r, p0 + 999, p2 - p0 + 12345);
			Assert::IsTrue(g.any && g.maxTier == Tier::T2, L"a bigger grant keeps T2");
			g = ArenaGrantFor(r, p0, p2 - p0 - 1);
			Assert::IsTrue(g.any && g.maxTier == Tier::T1, L"just below T2 keeps T1");
			g = ArenaGrantFor(r, p0, p1 - p0);
			Assert::IsTrue(g.any && g.maxTier == Tier::T1, L"exactly the T1 increment keeps T1");
			g = ArenaGrantFor(r, p0, p1 - p0 - 1);
			Assert::IsTrue(g.any && g.maxTier == Tier::T0, L"just below T1 keeps T0");
			g = ArenaGrantFor(r, p0 - 1, p2);
			Assert::IsFalse(g.any, L"below the T0 plan the arena is off, whatever the extra grant");
			Assert::IsFalse(ArenaGrantFor(r, 0, 0).any, L"zero grant is off");
			// The governor forces the OPTIONAL temporaries down first and never the T0 ones.
			const uint64_t fixed = 10 * kMB;
			GovernorInput gi;
			gi.card.vramBudgetBytes = uint64_t(double(fixed + p0 + (p1 - p0) + 100) / 0.8);
			gi.card.vramUsageBytes = 0;
			gi.headroom = 0.8;
			gi.users.push_back(FixedUser(kUserLibplacebo, "libplacebo intermediates", fixed));
			gi.users.push_back(u0);
			gi.users.push_back(ux);
			Assert::IsTrue(fixed + p2 > GovernorBudget(gi.card, 0, 0.8), L"full need exceeds the budget");
			Assert::IsTrue(fixed + p1 <= GovernorBudget(gi.card, 0, 0.8), L"the T1 level fits");
			const GovernorPlan plan = PlanVram(gi);
			Assert::AreEqual(size_t(1), plan.Find(kUserScratchArenaExtra)->level, L"optional arena stepped down one level");
			Assert::AreEqual(p1 - p0, plan.Find(kUserScratchArenaExtra)->bytes, L"optional arena granted the T1 increment");
			Assert::AreEqual(size_t(0), plan.Find(kUserScratchArena)->level, L"T0 temporaries untouched");
			bool found = false;
			for (const std::string& line : plan.lines)
				if (line.find("scratch arena") != std::string::npos && line.find("T2 temporaries off") != std::string::npos)
					found = true;
			Assert::IsTrue(found, L"governor line names the arena and the T2 step");
			// Heavy T1 pressure: even with the budget below the T0 plan + 0 extra, the extra user goes first.
			GovernorInput hard = gi;
			hard.card.vramBudgetBytes = uint64_t(double(fixed + p0 + 100) / 0.8);
			const GovernorPlan hp = PlanVram(hard);
			Assert::IsTrue(hp.Find(kUserScratchArenaExtra)->shed, L"optional temporaries shed first");
			Assert::AreEqual(size_t(0), hp.Find(kUserScratchArena)->level, L"T1 pressure never drops the T0 temporaries");
		}

		TEST_METHOD(TransientResidencyIsNotARingProduct)
		{
			const Registry sk = SkeletonProducts();
			ProductDecl tr = sk.Get(kP10GrainSplit);
			tr.residency = Residency::Transient;
			const uint64_t sizes[] = { 0, 1000, 5 * kMB, 4096 * kMB };
			for (uint64_t b : sizes)
				Assert::IsTrue(ResidencyFor(tr, b) == Residency::Transient, L"transient for any size");
			for (const ProductDecl& d : sk.All())
				for (uint64_t b : sizes) {
					// td.anchor_pass (doc 72) is declared Transient (the arena); every other product keeps Vram / SystemRam.
					const Residency want = d.residency == Residency::Transient ? Residency::Transient
						: d.residency == Residency::SystemRam && SystemRamAllowed(d, b) ? Residency::SystemRam : Residency::Vram;
					Assert::IsTrue(ResidencyFor(d, b) == want, L"existing products keep Vram/SystemRam (td.anchor_pass: Transient)");
				}
			const Registry reg = P10Registry(true);
			const Dims dims = MkDims(1920, 1080);
			const Graph g = P10Graph(reg, dims);
			Assert::IsTrue(g.valid, L"graph with a transient P10 is valid");
			TierInput in;
			in.headroom = 0.8;
			in.otherUsers.push_back(FixedUser(kUserLibplacebo, "libplacebo intermediates", 100 * kMB));
			AddScratchArenaUsers(in.otherUsers, RequestsFor(dims));
			const TierPlan plan = PlanTiers(reg, g, Card3090(), in);
			const TierEntry* e = plan.Find(kP10GrainSplit);
			Assert::IsNotNull(e, L"P10 has a tier entry");
			Assert::IsTrue(e->residency == Residency::Transient, L"entry is transient");
			Assert::IsTrue(e->grant == Grant::InRender, L"no lane grant");
			Assert::IsTrue(e->why == GrantWhy::RenderOnly, L"why render only");
			Assert::IsNull(plan.governor.Find(uint16_t(kUserProductBase + kP10GrainSplit)), L"no governor user for it");
			// P10 reads P4 (which reads P1) since SA2: those ancestors are ordinary products with their own grants. The total is
			// the other users plus the ancestors' grants, with nothing for P10.
			uint64_t ancestors = 0;
			for (const UserGrant& gr : plan.governor.grants)
				if (gr.id >= kUserProductBase && gr.id != uint16_t(kUserProductBase + kP10GrainSplit))
					ancestors += gr.bytes;
			Assert::AreEqual(SumLevel0(in.otherUsers) + ancestors, plan.vramBytes, L"vram total has no P10 ring");
			// The same graph with a normal P10 is a laned product (existing behaviour).
			const Registry normal = P10Registry(false);
			const TierPlan np = PlanTiers(normal, P10Graph(normal, dims), Card3090(), in);
			Assert::IsTrue(np.Laned(kP10GrainSplit), L"a normal product is laned on a big card");
			Assert::IsNotNull(np.governor.Find(uint16_t(kUserProductBase + kP10GrainSplit)), L"normal product is a governor user");
			Assert::IsTrue(np.vramBytes > plan.vramBytes, L"the ring costs memory only when not transient");
		}

		TEST_METHOD(ProductTexturesLanedVsLiveOnly)
		{
			const Registry reg = P10Registry(false);
			const Dims dims = MkDims(1920, 1080);
			const Graph g = P10Graph(reg, dims);
			Assert::IsTrue(g.valid, L"graph valid");
			const Node* node = g.Find(kP10GrainSplit);
			Assert::IsNotNull(node, L"P10 node");
			const ProductTexLayout layout = P10Layout();
			const TierPlan big = PlanTiers(reg, g, Card3090(), TierInput());
			Assert::IsTrue(big.Laned(kP10GrainSplit), L"laned on a big card");
			// Merge reconciliation: P10 reads the source ring P13 (RenderOnly views, SA0); a lane-visible view never moves its
			// reader into the render, while an ordinary render-only input still would.
			Assert::IsTrue(reg.Get(kP13SourceRing).laneVisible && !reg.Get(kP0RebaseDecision).laneVisible, L"only the source ring is a lane-visible view");
			Assert::IsTrue(big.Find(kP10GrainSplit)->why != GrantWhy::InputInRender, L"P13 does not demote P10");
			const size_t terms = layout.terms.size();
			{
				FakeBackend be;
				ProductTextures<FakeBackend> t(be);
				Assert::IsTrue(t.Allocate(*node, reg.Get(kP10GrainSplit), layout, dims, big), L"allocate laned");
				Assert::IsTrue(t.Laned(), L"laned");
				Assert::AreEqual(size_t(node->ring), t.RingBuffers(), L"ring buffers");
				Assert::AreEqual(size_t(node->livePool), t.LiveBuffers(), L"live buffers");
				Assert::AreEqual(size_t(be.created), (size_t(node->ring) + node->livePool) * terms, L"texture count");
				Assert::AreEqual((uint64_t(node->ring) + node->livePool) * layout.BytesPerBuffer(dims), t.Bytes(), L"bytes");
				Assert::AreEqual(node->ringBytes + node->liveBytes, t.Bytes(), L"bytes equal the graph's accounting");
				Assert::IsTrue(t.Ring(0, 0) != 0 && t.Live(0, 1) != 0, L"slots are reachable");
				t.Release();
				Assert::AreEqual(be.created, be.destroyed, L"release destroys everything");
				Assert::AreEqual(uint64_t(0), t.Bytes(), L"nothing held after release");
			}
			{
				FakeBackend be;
				ProductTextures<FakeBackend> t(be);
				Assert::IsTrue(t.Allocate(*node, reg.Get(kP10GrainSplit), layout, dims, TierPlan()), L"allocate live only");
				Assert::IsFalse(t.Laned(), L"not laned");
				Assert::AreEqual(size_t(0), t.RingBuffers(), L"no ring buffers");
				Assert::AreEqual(size_t(node->livePool), t.LiveBuffers(), L"live buffers");
				Assert::AreEqual(size_t(be.created), size_t(node->livePool) * terms, L"live texture count");
				Assert::AreEqual(uint64_t(node->livePool) * layout.BytesPerBuffer(dims), t.Bytes(), L"live bytes");
			}
			{
				FakeBackend be;
				be.failAfter = 3;
				ProductTextures<FakeBackend> t(be);
				Assert::IsFalse(t.Allocate(*node, reg.Get(kP10GrainSplit), layout, dims, big), L"failing backend fails the allocation");
				Assert::AreEqual(be.created, be.destroyed, L"partial textures destroyed");
				Assert::AreEqual(uint64_t(0), t.Bytes(), L"nothing held");
				Assert::AreEqual(1u, t.CreateFailures(), L"failure counted");
			}
		}

		TEST_METHOD(LayoutBytesEqualDeclaration)
		{
			const ProductDecl& decl = SkeletonProducts().Get(kP10GrainSplit);
			const ProductTexLayout layout = P10Layout();
			const Dims sizes[] = { MkDims(1920, 1080), MkDims(3840, 2160), MkDims(7680, 4320), MkDims(1280, 720),
				MkDims(1366, 769) };
			ProductTexLayout wrong = layout;
			wrong.terms[0].fmt = TexFormat::RGBA16F;
			for (const Dims& d : sizes) {
				Assert::IsTrue(layout.MatchesDecl(decl, d), L"layout matches the declaration");
				Assert::IsFalse(wrong.MatchesDecl(decl, d), L"a wrong layout does not match");
			}
			ProductTexLayout compact;
			TexTerm t;
			t.shift = 0;
			t.fmt = TexFormat::RG16F;
			compact.terms.push_back(t);
			t.fmt = TexFormat::RG16;
			compact.terms.push_back(t);
			t.shift = 4;
			t.fmt = TexFormat::RGBA32F;
			compact.terms.push_back(t);
			const Dims d = MkDims(1366, 769);
			const uint64_t blocks = uint64_t((1366 + 15) / 16) * uint64_t((769 + 15) / 16);
			Assert::AreEqual(uint64_t(1366) * 769 * 8 + blocks * 16, compact.BytesPerBuffer(d), L"8 B per pixel + 16 B per block");
		}

		TEST_METHOD(VramPerTierReport)
		{
			const Registry reg = P10Registry(false);
			const ProductDecl& decl = reg.Get(kP10GrainSplit);
			const ProductTexLayout layout = P10Layout();
			const Dims sizes[] = { MkDims(1920, 1080), MkDims(3840, 2160), MkDims(7680, 4320) };
			for (int cardIx = 0; cardIx < 2; ++cardIx) {
				const CardProfile card = cardIx == 0 ? Card4GB() : Card3090();
				const char* cardName = cardIx == 0 ? "4 GB laptop" : "24 GB 3090";
				for (const Dims& d : sizes) {
					const Graph g = P10Graph(reg, d);
					Assert::IsTrue(g.valid, L"graph valid");
					const Node* node = g.Find(kP10GrainSplit);
					Assert::IsNotNull(node, L"P10 node");
					const std::vector<ArenaRequest> req = RequestsFor(d);
					TierInput in;
					in.headroom = 0.8;
					AddScratchArenaUsers(in.otherUsers, req);
					const TierPlan plan = PlanTiers(reg, g, card, in);
					const std::string head = std::string("== ") + cardName + " at " + std::to_string(d.width) + "x"
						+ std::to_string(d.height);
					Logger::WriteMessage((head + "\n").c_str());
					for (const std::string& line : VramLedgerLines(plan, PlanArena(req, Tier::T2), reg))
						Logger::WriteMessage((line + "\n").c_str());
					for (size_t i = 0; i < layout.terms.size(); ++i) {
						const TexShape s = layout.ShapeOf(i, d);
						Logger::WriteMessage((std::string("  slot ") + std::to_string(i) + " " + Token(s.fmt) + " "
							+ std::to_string(s.w) + "x" + std::to_string(s.h) + ": " + FormatMB(ShapeBytes(s)) + "\n").c_str());
					}
					const uint64_t expect = uint64_t(d.width) * d.height * 16
						+ uint64_t((d.width + 15) / 16) * uint64_t((d.height + 15) / 16) * 16;
					Assert::AreEqual(expect, node->bufferBytes, L"buffer bytes by formula");
					Assert::AreEqual(BytesOf(decl.size, d), node->bufferBytes, L"buffer bytes from the declaration");
					Assert::AreEqual(node->bufferBytes * node->ring, node->ringBytes, L"ring bytes");
					Assert::AreEqual(node->bufferBytes * node->livePool, node->liveBytes, L"live bytes");
					const UserGrant* gr = plan.governor.Find(uint16_t(kUserProductBase + kP10GrainSplit));
					Assert::IsNotNull(gr, L"P10 grant");
					if (gr->level == 0)
						Assert::AreEqual(node->ringBytes + node->liveBytes, gr->bytes, L"level 0 grant is ring plus live");
					else
						Assert::AreEqual(node->liveBytes, gr->bytes, L"level 1 grant is the live pool");
					if (cardIx == 1 && d.width <= 3840)
						Assert::AreEqual(size_t(0), gr->level, L"the 3090 grants P10 in full");
					const std::vector<std::string> lines = VramLedgerLines(plan, PlanArena(req, Tier::T2), reg);
					Assert::IsTrue(lines.back().find("VRAM total") == 0, L"the ledger ends with the total");
					Assert::AreEqual(!plan.liveOverBudget, plan.vramBytes <= plan.budgetBytes, L"over-budget flag matches totals");
				}
			}
		}

		TEST_METHOD(LedgerListsEveryUser)
		{
			const Registry reg = P10Registry(true);
			const Dims dims = MkDims(1920, 1080);
			const Graph g = P10Graph(reg, dims);
			const std::vector<ArenaRequest> req = RequestsFor(dims);
			TierInput in;
			in.headroom = 0.8;
			in.otherUsers.push_back(FixedUser(kUserLibplacebo, "libplacebo intermediates", 100 * kMB));
			AddScratchArenaUsers(in.otherUsers, req);
			const TierPlan plan = PlanTiers(reg, g, Card3090(), in);
			const std::vector<std::string> lines = VramLedgerLines(plan, PlanArena(req, Tier::T2), reg);
			size_t tierLines = 0, planLines = 0, transientLines = 0;
			for (const std::string& l : lines) {
				if (l.find("VRAM T") == 0)
					++tierLines;
				if (l.find("scratch arena plan") != std::string::npos)
					++planLines;
				if (l.find("transient") != std::string::npos && l.find("grain split") != std::string::npos)
					++transientLines;
			}
			Assert::AreEqual(plan.governor.grants.size(), tierLines, L"one tier line per governor grant");
			Assert::AreEqual(size_t(1), planLines, L"one arena plan line");
			Assert::AreEqual(size_t(1), transientLines, L"one transient product line");
		}
		TEST_METHOD(RegionEvidenceAdoptsTheCoreGrainSplitProduct)
		{
			// SA0/SA2: the declarations live once in core/Products.h; the region-evidence module adds NO second
			// grain.split declaration: it consumes kP10GrainSplit / kP11RegionBlockStats; on a first-products-only
			// registry it completes it to the skeleton set (the same ids everywhere).
			std::vector<ProductDecl> decls = SkeletonProducts().All();
			const size_t before = decls.size();
			const RegionEvidence::ProductIds ids = RegionEvidence::AppendProducts(decls, MkDims(3840, 2160));
			Assert::AreEqual(int(kP10GrainSplit), int(ids.grainSplit), L"adopts the core P10");
			Assert::AreEqual(before, decls.size(), L"nothing is appended to the skeleton");
			Assert::AreEqual(int(kP11RegionBlockStats), int(ids.blockStats), L"the core block statistics");
			Assert::AreEqual(int(kP10cGrainSplitChroma), int(ids.grainSplitChroma), L"the chroma product is the core P10c");
			const std::vector<ConsumerDecl> readers = RegionEvidence::GrainConsumers(ids, 100);
			Assert::AreEqual(size_t(4), readers.size(), L"four declared readers");
			Assert::AreEqual(size_t(2), size_t(readers[2].count), L"the chroma-from-luma guard consumes luma and chroma");
			Assert::AreEqual(size_t(1), size_t(readers[1].count), L"temporal detail consumes luma only");
			Assert::IsTrue(Registry(decls).Validate(), L"registry validates");
			size_t tokens = 0;
			for (const ProductDecl& d : decls)
				tokens += std::string(d.token) == kTokenGrainSplit ? 1u : 0u;
			Assert::AreEqual(size_t(1), tokens, L"exactly one grain.split declaration");
			std::vector<ProductDecl> alone = FirstProducts().All();
			const RegionEvidence::ProductIds own = RegionEvidence::AppendProducts(alone, MkDims(3840, 2160));
			Assert::AreEqual(int(kP10GrainSplit), int(own.grainSplit), L"a first-products registry is completed to the skeleton: the same P10");
			Assert::AreEqual(SkeletonProducts().Size(), alone.size(), L"completed to the skeleton set");
		}

		TEST_METHOD(CompactGrainDeclaresHalfTheBytes)
		{
			const Dims d = MkDims(3840, 2160);
			const uint64_t full = BytesOf(SkeletonProducts(false).Get(kP10GrainSplit).size, d);
			const uint64_t compact = BytesOf(SkeletonProducts(true).Get(kP10GrainSplit).size, d);
			Assert::AreEqual(uint64_t(d.width) * d.height * 8 + CellsAt(d, 4) * 16, compact, L"compact = 8 B/px + 16 B/block (N, e3, e1, e2)");
			Assert::AreEqual(uint64_t(d.width) * d.height * 16 + CellsAt(d, 4) * 16, full, L"full = 16 B/px + 16 B/block");
			Assert::IsTrue(compact < full, L"compact is smaller");
		}

		// Finisher MUST 3: the first-use binding is an atomic compare-exchange: of many threads racing for an unbound
		// object exactly one wins, every other one sees "bound to another thread".
		TEST_METHOD(ThreadBindingFirstUseRaceHasOneWinner)
		{
			for (int round = 0; round < 200; ++round) {
				ThreadBinding b;
				std::atomic<int> winners{ 0 };
				std::atomic<int> go{ 0 };
				std::vector<std::thread> ts;
				for (int i = 0; i < 6; ++i)
					ts.emplace_back([&]() {
						while (go.load() == 0) {}
						if (b.BindIfUnbound())
							winners.fetch_add(1);
					});
				go.store(1);
				for (std::thread& t : ts)
					t.join();
				Assert::AreEqual(1, winners.load(), L"exactly one thread owns it");
				Assert::IsTrue(b.Bound() && b.BoundToOther(), L"bound, and not to this (test) thread");
			}
			ThreadBinding mine;
			Assert::IsTrue(mine.BindIfUnbound() && mine.BindIfUnbound(), L"the owner binds again idempotently");
			Assert::IsTrue(mine.BoundToCaller() && !mine.BoundToOther(), L"owned by the caller");
			bool other = true;
			std::thread t([&]() { other = mine.BindIfUnbound(); });
			t.join();
			Assert::IsFalse(other, L"another thread is refused");
		}

		TEST_METHOD(ArenaNestedScopeIsRefusedAndVisible)
		{
			FakeBackend be;
			ScratchArena<FakeBackend> arena(be);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "outer", Tier::T0, Shape(8, 8, TexFormat::R8), 1));
			r.push_back(Req(2, "inner", Tier::T0, Shape(8, 8, TexFormat::R8), 1));
			arena.Reserve(PlanArena(r, Tier::T2));
			arena.BeginFrame();
			Assert::IsTrue(arena.BeginScope(1), L"the outer scope opens");
			Assert::IsFalse(arena.BeginScope(2), L"a nested scope is refused");
			Assert::AreEqual(uint64_t(1), arena.NestedScopes(), L"the refusal is counted");
			// The outer scope is untouched: its owner still gets its texture, the inner one (another scope) does not.
			Assert::IsTrue(arena.Acquire("outer", Shape(8, 8, TexFormat::R8)) != 0, L"outer scope still works");
			Assert::IsTrue(arena.Acquire("inner", Shape(8, 8, TexFormat::R8)) == 0, L"the refused scope owner gets nothing");
			arena.EndScope();   // closes the OUTER scope, once
			const FallbackCatalog::Decision d = arena.FrameDecision();
			Assert::IsTrue(d.engaged && d.reason.find("nested scratch scope refused") != std::string::npos, L"visible");
			arena.BeginFrame();
			Assert::IsFalse(arena.FrameDecision().engaged, L"released on a clean frame");
			Assert::IsTrue(arena.BeginScope(1), L"a scope opens again after the outer one closed");
			arena.EndScope();
		}

		TEST_METHOD(ArenaOwnerCannotTakeAnotherOwnersTextures)
		{
			FakeBackend be;
			ScratchArena<FakeBackend> arena(be);
			const TexShape s = Shape(16, 16, TexFormat::R32F);
			std::vector<ArenaRequest> r;
			r.push_back(Req(1, "alpha", Tier::T0, s, 1));
			r.push_back(Req(1, "beta", Tier::T0, s, 2));
			arena.Reserve(PlanArena(r, Tier::T2));
			Assert::AreEqual(size_t(3), arena.PoolSize(s), L"capacity = the scope total");
			arena.BeginFrame();
			arena.BeginScope(1);
			const int a0 = arena.Acquire("alpha", s);
			const int a1 = arena.Acquire("alpha", s);   // alpha asked for ONE
			Assert::IsTrue(a0 != 0, L"alpha gets its texture");
			Assert::IsTrue(a1 == 0, L"alpha cannot take the textures of beta");
			const int b0 = arena.Acquire("beta", s);
			const int b1 = arena.Acquire("beta", s);
			const int b2 = arena.Acquire("beta", s);
			Assert::IsTrue(b0 != 0 && b1 != 0 && b2 == 0, L"beta gets exactly its two");
			Assert::IsTrue(a0 != b0 && a0 != b1 && b0 != b1, L"disjoint textures");
			arena.EndScope();
			// Deterministic slices: alpha (sorted first) holds pool slot 0, beta slots 1 and 2, in any call order.
			arena.BeginFrame();
			arena.BeginScope(1);
			const int b0b = arena.Acquire("beta", s);
			const int a0b = arena.Acquire("alpha", s);
			arena.EndScope();
			Assert::AreEqual(a0, a0b, L"the slice of alpha does not depend on who asked first");
			Assert::AreEqual(b0, b0b, L"the slice of beta does not depend on who asked first");
		}

		TEST_METHOD(ProductTexturesRefuseALayoutThatDoesNotMatchTheDeclaration)
		{
			const Registry reg = P10Registry(false);
			const Dims dims = MkDims(1920, 1080);
			const Graph g = P10Graph(reg, dims);
			const Node* node = g.Find(kP10GrainSplit);
			Assert::IsNotNull(node, L"P10 node");
			const TierPlan big = PlanTiers(reg, g, Card3090(), TierInput());
			ProductTexLayout wrong = P10Layout();
			wrong.terms[0].fmt = TexFormat::RGBA16F;
			FakeBackend be;
			ProductTextures<FakeBackend> t(be);
			Assert::IsFalse(t.Allocate(*node, reg.Get(kP10GrainSplit), wrong, dims, big), L"a mismatching layout is refused");
			Assert::AreEqual(0, be.created, L"nothing was created");
			Assert::AreEqual(1u, t.Refusals(), L"counted");
			const FallbackCatalog::Decision d = t.RefusalDecision();
			Assert::IsTrue(d.engaged && d.id == "product textures", L"visible");
			Assert::IsTrue(t.Allocate(*node, reg.Get(kP10GrainSplit), P10Layout(), dims, big), L"the matching layout allocates");
			Assert::IsFalse(t.RefusalDecision().engaged, L"the refusal clears on the next good allocation");
		}

		// Finisher MUST 1: the declaration is matched PER SHIFT, not by term position. Compact (A rg16f + B rg16 at shift 0,
		// absolute blocks) has three terms against P10's two declared terms; it must allocate at block edges 16 and 32.
		TEST_METHOD(ProductTexturesAllocateCompactAtBlock16And32)
		{
			const Registry reg = P10Registry(false, true);
			const Dims sizes[] = { MkDims(1920, 1080), MkDims(3840, 2160), MkDims(7680, 4320), MkDims(1366, 769) };
			for (const Dims& dims : sizes) {
				const Graph g = P10Graph(reg, dims);
				Assert::IsTrue(g.valid, L"graph valid");
				const Node* node = g.Find(kP10GrainSplit);
				Assert::IsNotNull(node, L"P10 node");
				const TierPlan big = PlanTiers(reg, g, Card3090(), TierInput());
				for (uint32_t B : { 16u, 32u }) {
					const ProductTexLayout layout = GrainLayoutCpu(true, dims, B);
					Assert::AreEqual(size_t(3), layout.terms.size(), L"three textures per buffer");
					Assert::IsTrue(layout.MatchesDecl(reg.Get(kP10GrainSplit), dims), L"compact matches the compact declaration");
					FakeBackend be;
					ProductTextures<FakeBackend> t(be);
					Assert::IsTrue(t.Allocate(*node, reg.Get(kP10GrainSplit), layout, dims, TierPlan()), L"compact allocates (live)");
					Assert::AreEqual(0u, t.Refusals(), L"no refusal");
					Assert::AreEqual(size_t(node->livePool) * 3, size_t(be.created), L"three textures per live buffer");
					Assert::IsTrue(t.Bytes() <= node->liveBytes, L"never more than the declaration counts");
					if (B == 16)
						Assert::AreEqual(node->liveBytes, t.Bytes(), L"at B = 16 exactly what the declaration counts");
					ProductTextures<FakeBackend> laned(be);
					Assert::IsTrue(laned.Allocate(*node, reg.Get(kP10GrainSplit), layout, dims, big), L"compact allocates (laned)");
					// The compact layout against the FULL declaration (8 B/px vs 16 B/px) is refused, and Full against Compact.
					Assert::IsFalse(layout.MatchesDecl(SkeletonProducts(false).Get(kP10GrainSplit), dims), L"compact vs full declaration refused");
					Assert::IsFalse(GrainLayoutCpu(false, dims, B).MatchesDecl(reg.Get(kP10GrainSplit), dims), L"full vs compact declaration refused");
					Assert::IsTrue(GrainLayoutCpu(false, dims, B).MatchesDecl(SkeletonProducts(false).Get(kP10GrainSplit), dims), L"full vs full matches");
				}
			}
		}

		// Finisher SHOULD: an absolute grid is bounded in EACH axis, not only in total bytes.
		TEST_METHOD(AbsoluteGridIsCheckedPerAxis)
		{
			const ProductDecl& decl = SkeletonProducts(true).Get(kP10GrainSplit);
			const Dims d = MkDims(1920, 1080);   // declared grid 120 x 68
			ProductTexLayout l = GrainLayoutCpu(true, d, 32);   // 60 x 34
			Assert::IsTrue(l.MatchesDecl(decl, d), L"within the grid");
			l.terms[2].absW = 121;   // wider than declared, total bytes still far below (121 x 34 < 120 x 68)
			Assert::IsTrue(uint64_t(121) * 34 < CellsAt(d, 4), L"total within the declared bytes");
			Assert::IsFalse(l.MatchesDecl(decl, d), L"wider than the declared grid is refused");
			l.terms[2].absW = 60;
			l.terms[2].absH = 69;
			Assert::IsFalse(l.MatchesDecl(decl, d), L"taller than the declared grid is refused");
			// An undeclared shift is refused; a missing declared shift is refused.
			ProductTexLayout extra = GrainLayoutCpu(true, d, 16);
			extra.terms[1].shift = 1;
			Assert::IsFalse(extra.MatchesDecl(decl, d), L"a term at an undeclared shift is refused");
			ProductTexLayout missing = GrainLayoutCpu(true, d, 16);
			missing.terms.erase(missing.terms.begin(), missing.terms.begin() + 2);
			Assert::IsFalse(missing.MatchesDecl(decl, d), L"a declared shift without textures is refused");
		}

		TEST_METHOD(ChromaProductUsesTheActualPlaneBytes)
		{
			// P10c plans for 4:2:0 but the governor and the allocator use the REAL chroma plane dims.
			const Registry reg = SkeletonProducts();
			const ProductDecl& decl = reg.Get(kP10cGrainSplitChroma);
			Assert::IsTrue(decl.planningSize, L"the chroma SizeRule is a planning figure");
			const Dims dims = MkDims(1920, 1080);
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP10cGrainSplitChroma);
			GraphSettings gs;
			gs.dims = dims;
			gs.renderAhead = 2;
			gs.laneLead = 1;
			const Graph g = BuildGraph(reg, { c }, gs);
			const Node* node = g.Find(kP10cGrainSplitChroma);
			Assert::IsNotNull(node, L"P10c node");
			// A 4:2:2 chroma plane (half width, full height): not the planned geometry.
			ProductTexLayout layout;
			for (int plane = 0; plane < 2; ++plane) {
				TexTerm split;
				split.fmt = TexFormat::RGBA32F;
				split.absW = dims.width / 2;
				split.absH = dims.height;
				layout.terms.push_back(split);
				TexTerm blocks;
				blocks.fmt = TexFormat::RGBA32F;
				blocks.absW = (split.absW + 15) / 16;
				blocks.absH = (split.absH + 15) / 16;
				layout.terms.push_back(blocks);
			}
			const uint64_t actual = layout.BytesPerBuffer(dims);
			Assert::IsTrue(actual != BytesOf(decl.size, dims), L"the real geometry differs from the plan");
			TierInput in;
			ActualBuffer ab;
			ab.id = kP10cGrainSplitChroma;
			ab.bytesPerBuffer = actual;
			in.actual.push_back(ab);
			const TierPlan plan = PlanTiers(reg, g, Card3090(), in);
			const UserGrant* grant = plan.governor.Find(uint16_t(kUserProductBase + kP10cGrainSplitChroma));
			Assert::IsNotNull(grant, L"governor user");
			FakeBackend be;
			ProductTextures<FakeBackend> t(be);
			Assert::IsTrue(t.Allocate(*node, decl, layout, dims, plan), L"a planning-size declaration accepts the actual layout");
			Assert::AreEqual(grant->bytes, t.Bytes(), L"the governor grant equals the bytes really allocated");
			const TierPlan planned = PlanTiers(reg, g, Card3090(), TierInput());
			Assert::IsTrue(planned.governor.Find(uint16_t(kUserProductBase + kP10cGrainSplitChroma))->bytes != grant->bytes,
				L"without the actual bytes the governor would count the plan");
		}

		TEST_METHOD(GrainStorageLedgerComparesPeaksIncludingScratch)
		{
			// compact 8N + 32 vs full 16N + 16 B/px: compact saves only when ring + live > 2.
			const Dims d = MkDims(1920, 1080);
			const uint64_t px = uint64_t(d.width) * d.height;
			const uint64_t blocks = CellsAt(d, 4);
			const std::vector<std::string> two = GrainSplit::StorageLedgerLines(d, 2);
			const std::vector<std::string> six = GrainSplit::StorageLedgerLines(d, 6);
			Assert::AreEqual(size_t(2), two.size(), L"two lines");
			Assert::IsTrue(two[1].find("saves nothing") != std::string::npos, L"N = 2: no saving");
			Assert::IsTrue(six[1].find("saves ") != std::string::npos && six[1].find("saves nothing") == std::string::npos, L"N = 6 saves");
			const uint64_t fullPeak = (px * 16 + blocks * 16) * 6 + px * 16 + blocks * 16;
			const uint64_t compactPeak = (px * 8 + blocks * 16) * 6 + px * 32 + blocks * 16;
			Assert::IsTrue(six[0].find(FormatMB(fullPeak)) != std::string::npos, L"full peak line");
			Assert::IsTrue(six[1].find(FormatMB(compactPeak)) != std::string::npos, L"compact peak line");
			Assert::IsTrue(six[1].find("8 x 6 + 32 B/px") != std::string::npos, L"compact peak formula");
		}
	};
}
