// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W1-C2a A2 / owner E3 (2026-11-11): the snapshot lease ends with the epoch through ONE VPV-GATED module function,
// EstimateState::EndSnapshotLease(originalPathway). The Original pathway never runs it (byte-identical to Bill:
// no matcher, no lease); the VP-V pathway ends the lease on the source side (the matcher side is the GPU test
// FieldSourceShimTests, which exercises InvalidateSnapshot on a real warp). CPU, table-driven over both pathways.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/motion/estimation/EstimateState.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(SnapshotLeaseGateTests)
	{
	public:
		struct Row { bool originalPathway; bool acts; const wchar_t* name; };

		TEST_METHOD(EndSnapshotLeaseRunsOnlyOnTheVpPathway)
		{
			const Row rows[] = {
				{ true, false, L"Original pathway: the call site does nothing" },
				{ false, true, L"VP-V pathway: the lease ends with the epoch" },
			};
			for (const Row& row : rows)
			{
				VpMotion::VectorEngine engine;
				VpMotion::EstimateState<int> state(engine);
				Dejudder::IFieldSource::Set captured;
				captured.from = 0;   // LiveFieldSet is false (no live field): the capture keeps these ids
				state.dejudderLiveFieldFrom = 41;
				state.CaptureSnapshot(captured, 7);
				uint64_t from = 0, to = 0;
				state.SnapshotPair(from, to);
				Assert::IsTrue(from == 41 && to == 0, L"a snapshot was captured");

				const bool acted = state.EndSnapshotLease(row.originalPathway);
				Assert::AreEqual(row.acts, acted, row.name);
				state.SnapshotPair(from, to);
				if (row.acts)
					Assert::IsTrue(from == 0 && to == 0, L"VP-V: the snapshot ended with the epoch");
				else
					Assert::IsTrue(from == 41 && to == 0, L"Original: the snapshot is untouched (nothing ran)");
				Assert::IsFalse(state.SnapshotLeaseLost(), L"an epoch move is never a lost lease");
				Assert::AreEqual(0ull, state.SnapshotLeaseLostReads());
				Dejudder::IFieldSource::Set set;
				const Dejudder::IFieldSource::SnapshotState snap = state.SnapshotFieldSet(7, set);
				Assert::IsTrue(snap == (row.acts ? Dejudder::IFieldSource::SnapshotState::None : Dejudder::IFieldSource::SnapshotState::Valid), row.name);
			}
		}

		TEST_METHOD(OriginalPathwayNeverEndsTheLeaseTwiceOver)
		{
			// Repeated epoch moves in Original leave the lease exactly as captured (nothing accumulates).
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			Dejudder::IFieldSource::Set captured;
			state.dejudderLiveFieldFrom = 5;
			state.CaptureSnapshot(captured, 3);
			for (int i = 0; i < 4; ++i)
				Assert::IsFalse(state.EndSnapshotLease(true));
			uint64_t from = 0, to = 0;
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 5 && to == 0);
			Assert::IsTrue(state.EndSnapshotLease(false));
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 0 && to == 0);
		}
	};
}
