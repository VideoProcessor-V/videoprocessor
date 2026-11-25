// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line implementations of VpSkeleton::SceneRecordReader (see SceneRecordReader.h).

#include <VP-V/modules/control/skeleton/SceneRecordReader.h>

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <vector>

namespace VpSkeleton
{
	namespace SceneRecordReader
	{
		namespace
		{
			// Non-mutating mirror of SkeletonMemo::Acquire's queue-window / ahead-plan admission checks
			// (SkeletonAcquire.h:967-982). Prevents look-ahead readers (e.g. SRM LookAheadFrameCoded) from
			// triggering SkeletonMemo::Fail() and polluting SkeletonMemo::lastCause_[kP4SourceStats] (MUST-9).
			bool AheadAdmittedNonMutating(const VpSkeleton::SkeletonMemo& memo, uint64_t seq, const char*& reason)
			{
				if (!memo.AheadOfSeal(seq))
					return true;
				const VpSkeleton::QueueWindow w = memo.Window();
				const bool hasWindow = (w.wired != 0 || w.current != 0);
				if (hasWindow)
				{
					if (seq > w.current + uint64_t(w.ahead))
					{
						reason = VpSkeleton::Token(VpSkeleton::AcquireCause::NotQueued);
						return false;
					}
					if (seq > w.current + uint64_t(w.aheadCompute))
					{
						reason = VpSkeleton::Token(VpSkeleton::AcquireCause::NotPlannedAhead);
						return false;
					}
					if (w.presentKnown && w.current > w.presented &&
						w.current - w.presented > uint64_t(w.history) + uint64_t(w.renderAhead) + 1u + uint64_t(w.ahead))
					{
						reason = VpSkeleton::Token(VpSkeleton::AcquireCause::PresentsStalled);
						return false;
					}
				}
				const VpSkeleton::AheadPlan plan = memo.Plan();
				uint16_t plannedDepth = 0;
				if (hasWindow && plan.Has(uint16_t(VpSkeleton::kP4SourceStats), 0u, &plannedDepth) &&
					seq > w.current + uint64_t(plannedDepth))
				{
					reason = VpSkeleton::Token(VpSkeleton::AcquireCause::NotPlannedAhead);
					return false;
				}
				return true;
			}
		}

		bool SceneOf(uint64_t seq, SceneVerdict& out)
		{
			out = SceneVerdict{};
			FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::SingleStore();
			if (!store)
				return false;
			FrameAnalysis::Scene scene;
			if (!store->PeekScene(seq, scene))
				return false;
			out.cut = scene.cut;
			out.fade = scene.fade;
			out.fadeKeep = scene.fadeKeep;
			return true;
		}

		bool AcquireScene(uint64_t seq, SceneVerdict& out, const char** why)
		{
			out = SceneVerdict{};
			const char* reason = "";
			bool found = false;
			const std::vector<const void*> owners = VpFrameAnalysis::SourceFrameTap::StoreOwners();
			if (owners.size() != 1)
				reason = owners.empty() ? "no store owner" : "renderer ambiguous";
			else if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
			{
				// MUST-9: try non-mutating store peek first (safe on any thread, never touches SkeletonMemo).
				if (FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(owners[0]))
				{
					FrameAnalysis::Scene scene;
					if (store->PeekScene(seq, scene))
					{
						out.cut = scene.cut;
						out.fade = scene.fade;
						out.fadeKeep = scene.fadeKeep;
						if (why != nullptr)
							*why = "";
						return true;
					}
				}
				// MUST-9: before entering SceneOf's live FillScene -> memo->Acquire(P4) path, guard off-thread,
				// released-frame, and unplanned-ahead requests so SkeletonMemo::Fail() is never polluted.
				const Vpv::Skeleton::FrameMemo fm = demand->MemoFor(owners[0], seq);
				if (fm.Valid() && !fm.memo->OnRenderThread())
					reason = "scene not published (off render thread)";
				else if (fm.Valid() && (fm.memo->Released(seq) || (seq > 0 && fm.memo->Released(seq - 1))))
					reason = VpSkeleton::Token(VpSkeleton::AcquireCause::FrameReleased);
				else if (fm.Valid() && !AheadAdmittedNonMutating(*fm.memo, seq, reason))
				{
					// reason populated by AheadAdmittedNonMutating (NotQueued / NotPlannedAhead / PresentsStalled)
				}
				else
				{
					const Vpv::Skeleton::SceneAnswer a = demand->SceneOf(owners[0], seq);
					if (a.known)
					{
						out.cut = a.cut;
						out.fade = a.fade;
						out.fadeKeep = a.fadeKeep;
						found = true;
					}
					else
						reason = (a.why != nullptr && a.why[0] != 0) ? a.why : "scene unknown";
				}
			}
			else
				reason = "no skeleton module";
			if (why != nullptr)
				*why = reason;
			return found;
		}

		bool AcquireSourceStats(uint64_t seq, VpUnit::SourceStats::Record& out, const char** why)
		{
			out = VpUnit::SourceStats::Record{};
			const char* reason = "";
			bool found = false;
			const std::vector<const void*> owners = VpFrameAnalysis::SourceFrameTap::StoreOwners();
			if (owners.size() != 1)
				reason = owners.empty() ? "no store owner" : "renderer ambiguous";
			else if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
			{
				// MUST-9: try non-mutating lane-published lease read first (safe on any thread, zero memo lastCause_ mutation).
				const VpSkeleton::LaneLease lease = demand->LeaseFor(owners[0], VpSkeleton::kP4SourceStats);
				if (lease.Valid() && lease.bytes != nullptr && lease.bytes->Bytes() >= sizeof(VpUnit::SourceStats::Record))
				{
					std::vector<uint8_t> laneBuf;
					const VpSkeleton::PublishedRead pr = VpSkeleton::ReadPublishedBytes(
						lease, VpSkeleton::kP4SourceStats, seq, lease.bytes->Bytes(),
						[&laneBuf](const uint8_t* src, size_t n) {
							laneBuf.assign(src, src + n);
						});
					if (pr == VpSkeleton::PublishedRead::Copied)
					{
						if (VpUnit::SourceStats::ReadRecord(laneBuf.data(), laneBuf.size(), &out))
						{
							if (why != nullptr)
								*why = "";
							return true;
						}
						if (why != nullptr)
							*why = "p4 record version mismatch";
						return false;
					}
				}

				const Vpv::Skeleton::FrameMemo fm = demand->MemoFor(owners[0], seq);
				if (!fm.Valid())
					reason = "no frame memo";
				else
				{
					// MUST-9: try non-mutating memo CopyPublished on any thread (accepts both padded kRecordBytes and compact sizeof(Record)).
					std::vector<uint8_t> recBuf(VpUnit::SourceStats::kRecordBytes, 0);
					size_t copiedBytes = 0;
					if (fm.memo->CopyPublished(VpSkeleton::kP4SourceStats, seq, recBuf.data(), recBuf.size()))
						copiedBytes = recBuf.size();
					else if (sizeof(VpUnit::SourceStats::Record) != VpUnit::SourceStats::kRecordBytes &&
						fm.memo->CopyPublished(VpSkeleton::kP4SourceStats, seq, recBuf.data(), sizeof(VpUnit::SourceStats::Record)))
						copiedBytes = sizeof(VpUnit::SourceStats::Record);

					if (copiedBytes > 0)
					{
						if (VpUnit::SourceStats::ReadRecord(recBuf.data(), copiedBytes, &out))
							found = true;
						else
							reason = "p4 record version mismatch";
					}
					else if (!fm.memo->HasOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu))
						reason = "no analysis lane";
					else if (!fm.memo->OnRenderThread())
						reason = "p4 not published (off render thread)";
					else if (fm.memo->Released(seq))
						reason = VpSkeleton::Token(VpSkeleton::AcquireCause::FrameReleased);
					else if (!AheadAdmittedNonMutating(*fm.memo, seq, reason))
					{
						// reason populated by AheadAdmittedNonMutating (NotQueued / NotPlannedAhead / PresentsStalled)
					}
					else
					{
						const VpSkeleton::AcquireResult ar = fm.memo->Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);
						if (!ar.Ok())
							reason = VpSkeleton::Token(ar.cause);
						else if (VpUnit::SourceStats::ReadRecord(ar.cpu, ar.cpuBytes, &out))
							found = true;
						else
							reason = "p4 record version mismatch";
					}
				}
			}
			else
				reason = "no skeleton module";
			if (why != nullptr)
				*why = reason;
			return found;
		}
	}
}
