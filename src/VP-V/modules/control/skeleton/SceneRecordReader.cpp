// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line implementations of VpSkeleton::SceneRecordReader (see SceneRecordReader.h).

#include <VP-V/modules/control/skeleton/SceneRecordReader.h>

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace VpSkeleton
{
	namespace SceneRecordReader
	{
		namespace
		{
			// Matches SkeletonModule::SceneFactsFor's ahead prime span (ref 4 + context 3 + 1; SkeletonModule.cpp:599).
			constexpr uint64_t kScenePrimeFrames = 8;

			// Fixed-size per-owner capture-counter -> renderer sourceSequence ring (zero per-frame heap alloc).
			constexpr size_t kCaptureSeqRingSize = 64;
			struct CaptureSeqEntry
			{
				uint64_t captureCounter = 0;
				int64_t timestampTicks = 0;
				uint64_t sourceSeq = 0;
				uint64_t storeGeneration = 0;
				bool valid = false;
			};
			struct CaptureSeqOwnerRing
			{
				std::array<CaptureSeqEntry, kCaptureSeqRingSize> entries{};
				size_t next = 0;
			};
			struct CaptureSeqTable
			{
				std::mutex mutex;
				std::map<const void*, CaptureSeqOwnerRing> owners;
			};
			CaptureSeqTable& CaptureSeqs()
			{
				static CaptureSeqTable table;
				return table;
			}

			// Non-mutating check: is P4(s) already published in the lane slot (ring, or live on the render thread,
			// matching AnalysisLane::ReadOf's allowPublished = AheadOfSeal(s) rule) or in the memo?
			// When true, SkeletonMemo::Acquire serves s via G2(a)/G2(b) without calling Fail().
			bool HasPublishedP4(Vpv::Skeleton::IExternalDemand* demand, const void* owner,
				const VpSkeleton::SkeletonMemo& memo, uint64_t s, bool onRenderThread)
			{
				const VpSkeleton::LaneLease lease = demand->LeaseFor(owner, VpSkeleton::kP4SourceStats);
				if (lease.Valid() && lease.bytes != nullptr && lease.bytes->Bytes() >= sizeof(VpUnit::SourceStats::Record))
				{
					if (const VpSkeleton::SlotSet* set = lease.table->Find(VpSkeleton::kP4SourceStats))
					{
						if (onRenderThread)
						{
							const VpSkeleton::ReadInfo ri = set->Read(s, memo.AheadOfSeal(s));
							if (ri.kind != VpSkeleton::ReadInfo::None && ri.index >= 0)
								return true;
						}
						else
						{
							const VpSkeleton::PublishedRead pr = VpSkeleton::ReadPublishedBytes(
								*set, *lease.bytes, s, [](const uint8_t*, size_t) {});
							if (pr == VpSkeleton::PublishedRead::Copied)
								return true;
						}
					}
				}
				std::vector<uint8_t> probe(VpUnit::SourceStats::kRecordBytes, 0);
				if (memo.CopyPublished(VpSkeleton::kP4SourceStats, s, probe.data(), probe.size()))
					return true;
				if (sizeof(VpUnit::SourceStats::Record) != VpUnit::SourceStats::kRecordBytes &&
					memo.CopyPublished(VpSkeleton::kP4SourceStats, s, probe.data(), sizeof(VpUnit::SourceStats::Record)))
					return true;
				return false;
			}

			// Non-mutating mirror of SkeletonMemo::Acquire's queue-window / ahead-plan admission checks
			// (SkeletonAcquire.h:967-982). Prevents look-ahead readers (e.g. SRM LookAheadFrameCoded) from
			// triggering SkeletonMemo::Fail() and polluting SkeletonMemo::lastCause_[kP4SourceStats] (MUST-9).
			bool AheadAdmittedNonMutating(const VpSkeleton::SkeletonMemo& memo, uint64_t seq, const char*& reason)
			{
				if (!memo.AheadOfSeal(seq))
					return true;
				const VpSkeleton::QueueWindow w = memo.Window();
				// Contract: QueueWindow is default-zeroed before SkeletonMemo::SetWindow(); any non-zero
				// dimension (wired, current, ahead, back) indicates a configured window (windowSet_).
				const bool hasWindow = (w.wired != 0 || w.current != 0 || w.ahead != 0 || w.back != 0);
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
				// Check pre-published lane/memo P4 first for each frame in SceneFactsFor's [first .. seq] span
				// so Competitive-mode (back == 0) released frames and pre-published ahead slots still hit G2(a)/G2(b).
				const Vpv::Skeleton::FrameMemo fm = demand->MemoFor(owners[0], seq);
				bool blocked = false;
				if (fm.Valid() && !fm.memo->OnRenderThread())
				{
					reason = "scene not published (off render thread)";
					blocked = true;
				}
				else if (fm.Valid() && seq >= 2)
				{
					uint64_t first = seq - 1;
					if (fm.memo->AheadOfSeal(seq) && seq >= 3)
					{
						while (first > 1 && seq - first < kScenePrimeFrames && fm.memo->AheadOfSeal(first - 1))
							--first;
					}
					for (uint64_t s = first; s <= seq; ++s)
					{
						if (HasPublishedP4(demand, owners[0], *fm.memo, s, true))
							continue;
						if (fm.memo->Released(s))
						{
							reason = VpSkeleton::Token(VpSkeleton::AcquireCause::FrameReleased);
							blocked = true;
							break;
						}
						if (!AheadAdmittedNonMutating(*fm.memo, s, reason))
						{
							blocked = true;
							break;
						}
					}
				}
				if (!blocked)
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
					else if (!fm.memo->OnRenderThread())
						reason = !fm.memo->HasOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu)
							? "no analysis lane" : "p4 not published (off render thread)";
					else
					{
						// Check pre-published render-thread lane slots (including ReadInfo::Live skipped by ReadPublishedBytes)
						// before enforcing owner-fill / Released / AheadAdmitted guards, matching G2(a) precedence.
						const bool prePublished = HasPublishedP4(demand, owners[0], *fm.memo, seq, true);
						if (!prePublished && !fm.memo->HasOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu))
							reason = "no analysis lane";
						else if (!prePublished && fm.memo->Released(seq))
							reason = VpSkeleton::Token(VpSkeleton::AcquireCause::FrameReleased);
						else if (!prePublished && !AheadAdmittedNonMutating(*fm.memo, seq, reason))
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
			}
			else
				reason = "no skeleton module";
			if (why != nullptr)
				*why = reason;
			return found;
		}

		void RecordCaptureSequence(const void* owner, uint64_t captureCounter, int64_t timestampTicks, uint64_t sourceSeq)
		{
			if (!owner || captureCounter == 0 || sourceSeq == 0)
				return;
			const uint64_t gen = VpFrameAnalysis::SourceFrameTap::Generation(owner);
			CaptureSeqTable& t = CaptureSeqs();
			std::lock_guard<std::mutex> lock(t.mutex);
			CaptureSeqOwnerRing& ring = t.owners[owner];
			CaptureSeqEntry& slot = ring.entries[ring.next];
			slot.captureCounter = captureCounter;
			slot.timestampTicks = timestampTicks;
			slot.sourceSeq = sourceSeq;
			slot.storeGeneration = gen;
			slot.valid = true;
			ring.next = (ring.next + 1) % kCaptureSeqRingSize;
		}

		void ClearCaptureSequences(const void* owner)
		{
			if (!owner)
				return;
			CaptureSeqTable& t = CaptureSeqs();
			std::lock_guard<std::mutex> lock(t.mutex);
			t.owners.erase(owner);
		}

		bool ResolveCaptureSequence(uint64_t captureCounter, int64_t timestampTicks, uint64_t& sourceSeqOut)
		{
			sourceSeqOut = 0;
			if (captureCounter == 0)
				return false;
			const std::vector<const void*> owners = VpFrameAnalysis::SourceFrameTap::StoreOwners();
			if (owners.size() != 1)
				return false;
			const void* owner = owners[0];
			const uint64_t gen = VpFrameAnalysis::SourceFrameTap::Generation(owner);
			CaptureSeqTable& t = CaptureSeqs();
			std::lock_guard<std::mutex> lock(t.mutex);
			const auto it = t.owners.find(owner);
			if (it == t.owners.end())
				return false;
			const CaptureSeqOwnerRing& ring = it->second;
			for (size_t i = 0; i < kCaptureSeqRingSize; ++i)
			{
				const size_t idx = (ring.next + kCaptureSeqRingSize - 1 - i) % kCaptureSeqRingSize;
				const CaptureSeqEntry& e = ring.entries[idx];
				if (!e.valid || e.captureCounter != captureCounter || e.storeGeneration != gen)
					continue;
				if (timestampTicks != 0 && e.timestampTicks != 0 && e.timestampTicks != timestampTicks)
					continue;
				sourceSeqOut = e.sourceSeq;
				return true;
			}
			return false;
		}
	}
}
