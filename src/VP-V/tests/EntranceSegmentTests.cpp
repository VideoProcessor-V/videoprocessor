#include "CppUnitTest.h"

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/segments/EntranceSegment.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// S1, the entrance segment (VP-V/segments/EntranceSegment.h; WP-8 commit 1). CPU only: the stage
// table, the walk's plan for every slot switch (pipeline_slots_off, Original pathway), the order
// taken from the validated registry (OrderValidator / SlotContracts; nothing hard-coded), and the
// placeholder stages' pass-through. The pictures themselves (byte identity with the host's former
// code) need the GPU: see docs/design/m2-entrance-callsites.md, "GPU tests for later".
namespace EntranceSegmentTestsNs
{
	using PipelineSlots::Id;
	using EntranceSegment::Stage;

	const Id kS1Slots[] = { Id::ScalingRebase, Id::RepairEncoding, Id::RepairDeband, Id::RepairTextureRestore,
		Id::ScalingRoute4k, Id::ScalingRefine, Id::ScalingChromaUpscale };

	// Every S1 slot with a component (placeholders too, as when a net registers), enabled.
	PipelineSlots::Registry AllS1Slots()
	{
		PipelineSlots::Registry registry;
		for (Id id : kS1Slots)
		{
			registry.SetComponent(id, true);
			registry.SetEnabled(id, true);
		}
		return registry;
	}

	bool Contains(const std::vector<Stage>& plan, Stage stage)
	{
		return std::find(plan.begin(), plan.end(), stage) != plan.end();
	}

	// A host with no GPU that records what the segment asks of it (audit F1): with the moved slots
	// off, the segment must never prepare a pass (Dispatch), touch a module (the module pointers are
	// null: a call would crash) or engage a fallback.
	class FakeHost final : public EntranceSegment::Host
	{
	public:
		int dispatches = 0, engaged = 0, fieldAhead = 0;
		pl_gpu Gpu() const override { return nullptr; }
		pl_dispatch Dispatch() override { ++dispatches; return nullptr; }
		void EngageFallback(const std::string&, const std::string&) override { ++engaged; }
		void ReleaseFallback(const std::string&) override {}
		bool FieldAhead(uint64_t, uint64_t, std::string&) override { ++fieldAhead; return false; }
		Field FieldOf(uint64_t) const override { return Field(); }
		NextUpload Next() const override { return NextUpload(); }
	};

	// The host-owned per-frame state the segment reads, with every module pointer null.
	struct FakeState
	{
		PipelineSlots::Registry slots;
		bool entranceOnce = true;
		double rebaseFactor = 0.5, rebaseFactorX = 0.5, fromFactor = 0.75, fromFactorX = 0.75;
		double debandThreshold = 3.0;   // VP deband "on" for this frame: only the slot switch stops it
		VpDebander::Params deband;
		DetailPulse::Params pulse;
		TextureRestore::Params restore;
		bool pulseRuns = true, restoreRuns = true;
		std::string pulseInactive, restoreInactive;
		uint64_t pulseAhead = 0;

		EntranceSegment::Bindings Bind()
		{
			EntranceSegment::Bindings b;   // sourceRebase, debander, detailPulse, textureRestore: null
			b.slots = &slots;
			b.entranceOnceSetting = &entranceOnce;
			b.rebaseFactor = &rebaseFactor;
			b.rebaseFactorX = &rebaseFactorX;
			b.rebaseFromFactor = &fromFactor;
			b.rebaseFromFactorX = &fromFactorX;
			b.debandThreshold = &debandThreshold;
			b.vpDebandParams = &deband;
			b.detailPulseParams = &pulse;
			b.textureRestoreParams = &restore;
			b.detailPulseRuns = &pulseRuns;
			b.textureRestoreRuns = &restoreRuns;
			b.detailPulseInactive = &pulseInactive;
			b.textureRestoreInactive = &restoreInactive;
			b.detailPulseAheadSequence = &pulseAhead;
			return b;
		}
	};

	pl_frame SourceFrame()
	{
		pl_frame f{};
		f.num_planes = 2;   // no textures: the segment must not need any with its slots off
		f.planes[0].components = 1;
		f.planes[1].components = 2;
		f.crop = { 0.0f, 140.0f, 3840.0f, 2020.0f };
		f.repr.bits.sample_depth = 16;
		f.repr.bits.color_depth = 10;
		return f;
	}

	void AssertSameFrame(const pl_frame& a, const pl_frame& b)
	{
		Assert::AreEqual(a.num_planes, b.num_planes);
		for (int p = 0; p < a.num_planes; ++p)
		{
			Assert::IsTrue(a.planes[p].texture == b.planes[p].texture);
			Assert::AreEqual(a.planes[p].components, b.planes[p].components);
		}
		Assert::AreEqual(a.crop.x0, b.crop.x0);
		Assert::AreEqual(a.crop.y0, b.crop.y0);
		Assert::AreEqual(a.crop.x1, b.crop.x1);
		Assert::AreEqual(a.crop.y1, b.crop.y1);
		Assert::AreEqual(a.repr.bits.color_depth, b.repr.bits.color_depth);
	}

	// Every path of the one call, on a source frame, twice per source frame (the second a repeat).
	void RunEveryPath(EntranceSegment::Segment& segment, FakeHost& host)
	{
		const pl_frame source = SourceFrame();
		for (uint64_t sequence = 1; sequence <= 3; ++sequence)
			for (int repeat = 0; repeat < 2; ++repeat)
			{
				EntranceSegment::Request main;
				main.sequence = sequence;
				main.source = &source;
				main.image = &source;
				main.nextSequence = sequence + 1;
				pl_frame frame = source;
				Assert::IsTrue(segment.Run(main, frame));
				AssertSameFrame(source, frame);

				EntranceSegment::Request phase = main;   // a picture between frames
				phase.phasePicture = true;
				frame = source;
				Assert::IsTrue(segment.Run(phase, frame));
				AssertSameFrame(source, frame);

				EntranceSegment::Request perPicture = phase;   // B already processed as its own A
				perPicture.perPicture = true;
				frame = source;
				Assert::IsTrue(segment.Run(perPicture, frame));
				AssertSameFrame(source, frame);

				EntranceSegment::Request extra;
				extra.path = EntranceSegment::Path::Extra;
				extra.sequence = sequence;
				frame = source;
				Assert::IsTrue(segment.Run(extra, frame));
				AssertSameFrame(source, frame);

				EntranceSegment::Request from;
				from.path = EntranceSegment::Path::CrossfadeFrom;
				from.sequence = sequence;
				frame = source;
				Assert::IsTrue(segment.Run(from, frame), L"no re-base on the path being left: the fade can run");
				AssertSameFrame(source, frame);
			}
		Assert::AreEqual(0, host.dispatches, L"no pass was prepared");
		Assert::AreEqual(0, host.engaged, L"no fallback");
	}

	TEST_CLASS(EntranceSegmentTests)
	{
	public:
		TEST_METHOD(EveryEntranceSlotHasAStageAndNothingElseDoes)
		{
			for (size_t i = 0; i < PipelineSlots::kCount; ++i)
			{
				const Id id = static_cast<Id>(i);
				Assert::AreEqual(PipelineSlots::IsEntranceStage(id), EntranceSegment::StageFor(id) != Stage::None,
					L"S1 covers exactly the registry's entrance stages");
			}
		}

		TEST_METHOD(OnlyTheBuiltStagesAreWired)
		{
			Assert::IsTrue(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingRebase)));
			Assert::IsTrue(EntranceSegment::Wired(EntranceSegment::StageFor(Id::RepairDeband)));
			Assert::IsTrue(EntranceSegment::Wired(EntranceSegment::StageFor(Id::RepairTextureRestore)));
			// Placeholders: the held repair (A) slot until the net runner is wired, 4K route, refine, chroma.
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::RepairEncoding)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingRoute4k)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingRefine)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingChromaUpscale)));
			Assert::IsFalse(EntranceSegment::Wired(Stage::None));
		}

		TEST_METHOD(DefaultRegistryRunsOnlyTheBuiltStagesPlaceholdersCostNothing)
		{
			// Zero-cost rule: placeholders have no component by default, so the walk never reaches them.
			const std::vector<Stage> plan = EntranceSegment::Plan(PipelineSlots::Registry());
			const std::vector<Stage> expected = { Stage::Rebase, Stage::Deband, Stage::Detail };
			Assert::IsTrue(plan == expected);
		}

		TEST_METHOD(EachS1SlotSwitchesOffAloneAndTheRestKeepTheirOrder)
		{
			// pipeline_slots_off: one slot off removes exactly its stage (no pass: the picture passes
			// unchanged); the others run in the same order. Covers re-base, the repair (A)
			// placeholder, deband, texture restore / detail pulse, 4K route, refine and chroma.
			const std::vector<Stage> all = EntranceSegment::Plan(AllS1Slots());
			Assert::AreEqual(sizeof(kS1Slots) / sizeof(kS1Slots[0]), all.size());
			for (Id id : kS1Slots)
			{
				const Stage stage = EntranceSegment::StageFor(id);
				PipelineSlots::Registry registry = AllS1Slots();
				Assert::IsTrue(Contains(EntranceSegment::Plan(registry), stage), L"on: the stage runs");
				registry.SetEnabled(id, false);
				const std::vector<Stage> plan = EntranceSegment::Plan(registry);
				Assert::IsFalse(Contains(plan, stage), L"off: the stage is not called");
				std::vector<Stage> expected = all;
				expected.erase(std::find(expected.begin(), expected.end(), stage));
				Assert::IsTrue(plan == expected, L"the other stages keep their order");
				// On again: back in its place.
				registry.SetEnabled(id, true);
				Assert::IsTrue(EntranceSegment::Plan(registry) == all);
			}
		}

		TEST_METHOD(EveryS1SlotOffRunsNoStage)
		{
			PipelineSlots::Registry registry = AllS1Slots();
			for (Id id : kS1Slots)
				registry.SetEnabled(id, false);
			Assert::IsTrue(EntranceSegment::Plan(registry).empty());
		}

		TEST_METHOD(OriginalPathwayRunsNoS1Stage)
		{
			// Every VP addition off (ADR-0019): S1 is empty, so the frame reaches libplacebo untouched.
			PipelineSlots::Registry registry = AllS1Slots();
			registry.ApplyOriginalPathway();
			Assert::IsTrue(EntranceSegment::Plan(registry).empty());
		}

		TEST_METHOD(TheWalkFollowsAValidatedReorder)
		{
			// The order comes from the registry (OrderValidator), never from S1: the owner's experiments.
			PipelineSlots::Registry registry = AllS1Slots();
			std::string reason;
			Assert::IsTrue(registry.SetOrderByNames("repair.encoding, scaling.rebase", &reason));
			std::vector<Stage> plan = EntranceSegment::Plan(registry);
			Assert::IsTrue(plan.size() >= 2 && plan[0] == Stage::RepairA && plan[1] == Stage::Rebase);
			Assert::IsTrue(registry.SetOrderByNames("repair.deband > repair.encoding", &reason));
			plan = EntranceSegment::Plan(registry);
			Assert::IsTrue(std::find(plan.begin(), plan.end(), Stage::Deband) < std::find(plan.begin(), plan.end(), Stage::RepairA));
		}

		TEST_METHOD(ARejectedReorderLeavesTheWalkUnchanged)
		{
			PipelineSlots::Registry registry = AllS1Slots();
			const std::vector<Stage> before = EntranceSegment::Plan(registry);
			std::string reason;
			Assert::IsFalse(registry.SetOrderByNames("scaling.route_4k, repair.deband", &reason));
			Assert::IsTrue(EntranceSegment::Plan(registry) == before);
		}

		TEST_METHOD(StageListComesFromTheRegistryOrderAndSlotContracts)
		{
			PipelineSlots::Registry registry;
			std::vector<Id> designed;
			for (Id id : registry.CurrentOrder())
				if (PipelineSlots::IsEntranceStage(id))
					designed.push_back(id);
			std::vector<Id> listed;
			for (const SegmentRunner::StageInfo& s : EntranceSegment::EntranceStageInfo(registry))
			{
				listed.push_back(s.slot);
				const PipelineSlots::Contract c = PipelineSlots::ContractOf(s.slot);
				Assert::AreEqual(static_cast<int>(c.needRes), static_cast<int>(s.needs.res));
				Assert::AreEqual(static_cast<int>(c.giveRes), static_cast<int>(s.gives.res));
			}
			Assert::IsTrue(listed == designed);
			// A validated reorder is reflected.
			registry.SetComponent(Id::RepairEncoding, true);
			registry.SetEnabled(Id::RepairEncoding, true);
			std::string reason;
			Assert::IsTrue(registry.SetOrderByNames("repair.encoding, scaling.rebase", &reason));
			const std::vector<SegmentRunner::StageInfo> moved = EntranceSegment::EntranceStageInfo(registry);
			Assert::IsTrue(moved.size() >= 2 && moved[0].slot == Id::RepairEncoding && moved[1].slot == Id::ScalingRebase);
		}

		TEST_METHOD(ContractsWalkFromTheSourceEntry)
		{
			// Every stage's needs are met by the entry and the stages before it, BTB/WTW are carried
			// (no S1 stage clips them) and no stage requires valid vectors (each falls back without).
			SegmentRunner::Flow f = EntranceSegment::EntranceEntry();
			Assert::AreEqual(static_cast<int>(SegmentRunner::Range::Extended), static_cast<int>(f.range));
			for (const SegmentRunner::StageInfo& s : EntranceSegment::EntranceStageInfo(PipelineSlots::Registry()))
			{
				Assert::IsTrue((s.needs.res & f.res) != 0);
				Assert::IsTrue((s.needs.light & f.light) != 0);
				Assert::IsTrue((s.needs.format & f.format) != 0);
				Assert::IsTrue((s.needs.range & f.range) != 0);
				Assert::IsFalse(s.needs.vectors);
				Assert::AreNotEqual(static_cast<int>(SegmentRunner::Range::Clipped), static_cast<int>(s.gives.range));
				if (s.gives.res) f.res = s.gives.res;
			}
		}

		TEST_METHOD(SegmentWithItsMovedSlotsOffLeavesTheFrameAndCallsNoModule)
		{
			// pipeline_slots_off = scaling.rebase, repair.deband, repair.texture_restore (every stage
			// would run on this frame otherwise: factors 0.5, threshold 3, both detail stages on).
			FakeState state;
			for (Id id : { Id::ScalingRebase, Id::RepairDeband, Id::RepairTextureRestore })
				state.slots.SetEnabled(id, false);
			FakeHost host;
			EntranceSegment::Segment segment(host, state.Bind());
			RunEveryPath(segment, host);
		}

		TEST_METHOD(SegmentOnTheOriginalPathwayLeavesTheFrameAndCallsNoModule)
		{
			FakeState state;
			state.slots.ApplyOriginalPathway();
			FakeHost host;
			EntranceSegment::Segment segment(host, state.Bind());
			Assert::IsFalse(segment.Once(), L"B3 follows the Original pathway");
			RunEveryPath(segment, host);
			Assert::IsNull(segment.ProcessedA(SourceFrame(), 1, 2, nullptr), L"no ring picture without B3");
		}

		TEST_METHOD(SegmentSeesExactlyTheSlotSwitchedOff)
		{
			// One of the three moved slots off at a time: the segment's walk lacks exactly that stage.
			for (Id off : { Id::ScalingRebase, Id::RepairDeband, Id::RepairTextureRestore })
			{
				FakeState state;
				state.slots.SetEnabled(off, false);
				const std::vector<Stage> plan = EntranceSegment::Plan(state.slots);
				Assert::IsFalse(Contains(plan, EntranceSegment::StageFor(off)));
				for (Id on : { Id::ScalingRebase, Id::RepairDeband, Id::RepairTextureRestore })
					if (on != off)
						Assert::IsTrue(Contains(plan, EntranceSegment::StageFor(on)));
			}
		}

		TEST_METHOD(PlaceholderStagePassesThePictureThroughUnchanged)
		{
			// The held repair (A) slot and the other placeholders: pass-through, no pass, no texture.
			pl_frame in{};
			in.num_planes = 2;
			in.planes[0].texture = reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x1000));
			in.planes[1].texture = reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x2000));
			in.planes[0].components = 1;
			in.planes[1].components = 2;
			in.crop = { 1.0f, 2.0f, 1919.0f, 1078.0f };
			in.repr.bits.sample_depth = 16;
			in.repr.bits.color_depth = 10;
			pl_frame out{};
			Assert::IsFalse(EntranceSegment::Placeholder(in, out), L"a placeholder never reports that it applied");
			Assert::AreEqual(in.num_planes, out.num_planes);
			for (int p = 0; p < 2; ++p)
			{
				Assert::IsTrue(in.planes[p].texture == out.planes[p].texture, L"the same planes: no new texture");
				Assert::AreEqual(in.planes[p].components, out.planes[p].components);
			}
			Assert::AreEqual(in.crop.x0, out.crop.x0);
			Assert::AreEqual(in.crop.y1, out.crop.y1);
			Assert::AreEqual(in.repr.bits.color_depth, out.repr.bits.color_depth);
		}
	};
}
