#include "CppUnitTest.h"

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/segments/EntranceSegment.h>

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// S1, the entrance segment (VP-V/segments/EntranceSegment.h; WP-8 commit 1). CPU only: the stage
// table, the placeholders' zero-cost rule and the segment's stage list against the slot registry.
// The pictures themselves (byte identity with the host's former code) need the GPU: see
// docs/design/m2-entrance-callsites.md, "GPU tests for later".
namespace EntranceSegmentTestsNs
{
	using PipelineSlots::Id;
	using EntranceSegment::Stage;

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
			// Placeholders: repair A (until the net runner is wired), 4K route, refine, chroma.
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::RepairEncoding)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingRoute4k)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingRefine)));
			Assert::IsFalse(EntranceSegment::Wired(EntranceSegment::StageFor(Id::ScalingChromaUpscale)));
			Assert::IsFalse(EntranceSegment::Wired(Stage::None));
		}

		TEST_METHOD(StageListFollowsTheDesignedOrder)
		{
			// The segment's list is the registry's designed entrance order (re-base, repair A,
			// deband, texture restore, 4K route, refine, chroma).
			PipelineSlots::Registry registry;
			const std::vector<SegmentRunner::StageInfo> stages = EntranceSegment::EntranceStageInfo();
			std::vector<Id> listed;
			for (const SegmentRunner::StageInfo& s : stages)
				listed.push_back(s.slot);
			std::vector<Id> designed;
			for (Id id : PipelineSlots::DefaultOrder())
				if (PipelineSlots::IsEntranceStage(id))
					designed.push_back(id);
			Assert::IsTrue(listed == designed);
			int last = -1;
			for (Id id : listed)
			{
				Assert::IsTrue(registry.Order(id) > last);
				last = registry.Order(id);
			}
		}

		TEST_METHOD(ContractsWalkFromTheSourceEntry)
		{
			// Every stage's needs are met by the entry and the stages before it, BTB/WTW are carried
			// (no S1 stage clips them) and no stage requires valid vectors (each falls back without).
			SegmentRunner::Flow f = EntranceSegment::EntranceEntry();
			Assert::AreEqual(static_cast<int>(SegmentRunner::Range::Extended), static_cast<int>(f.range));
			for (const SegmentRunner::StageInfo& s : EntranceSegment::EntranceStageInfo())
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

		TEST_METHOD(PlaceholdersNeverReachTheStageWalk)
		{
			// Zero-cost rule: a placeholder slot (no component) is not in ActiveEntrance, so the
			// segment's walk never even reaches its (empty) case.
			PipelineSlots::Registry registry;
			for (Id id : registry.ActiveEntrance())
				Assert::IsTrue(EntranceSegment::Wired(EntranceSegment::StageFor(id)));
		}
	};
}
