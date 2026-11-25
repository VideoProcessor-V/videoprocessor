// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// D3 / FIX3-D6 (102b CO-DEFAULT-MOTIONAHEAD-U3): zero-wait no-lane CPU owner fill for kP1HalfLuma.
// Built without the precompiled header (pure CPU + VP-V headers only; zero GPU device / zero libplacebo).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/P1CpuFill.h>

#include <AnalysisLumaSource.h>
#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/source_analysis/P1AcquireSource.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace FrameAnalysis
	{
		namespace
		{
			namespace RE = RendererExtension;

			// Exact catalog owner unit of kP1HalfLuma (core/ProductCatalog.h).
			const char* const kP1CpuUnit = "input/source_analysis/lane";
			constexpr size_t kMaxHosts = 16;   // the skeleton's renderer table (HostSlots)
			// Derived ring capacity (Rule 1): kAheadCeiling ahead frames + current frame + 1 look-back frame.
			constexpr size_t kMaxSourceOffers = static_cast<size_t>(VpSkeleton::kAheadCeiling) + 2u;

			class P1CpuState : public std::enable_shared_from_this<P1CpuState>
			{
			public:
				void Shutdown()
				{
					shutdown_.store(true, std::memory_order_release);
					std::vector<AnalysisLumaSource> dropped;   // destroyed AFTER locks unlock
					{
						std::lock_guard<std::mutex> regLock(regMutex_);
						std::lock_guard<std::mutex> lock(mutex_);
						for (HostEntry& h : hosts_)
							for (Offer& o : h.ring)
								dropped.push_back(std::move(o.source));
						hosts_.clear();
						desiredDepth_ = 0;
					}
				}

				void OnEntrance(const void* host)
				{
					if (host == nullptr || shutdown_.load(std::memory_order_acquire))
						return;
					if (Hub::ExtensionHub::Instance().OriginalPathway(host))
						return;
					bool needSync = false;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (shutdown_.load(std::memory_order_acquire))
							return;
						for (HostEntry& h : hosts_)
						{
							if (h.host == host)
							{
								++h.entrances;
								break;
							}
						}
						const uint16_t wantDepth = DesiredDepthLocked();
						needSync = !registered_.load(std::memory_order_acquire) || wantDepth != desiredDepth_;
						desiredDepth_ = wantDepth;
					}
					if (needSync)
						SyncRegistration(nullptr);
				}

				void NoteDemand(const void* host, bool active, Skeleton::IExternalDemand* demand = nullptr)
				{
					if (host == nullptr || shutdown_.load(std::memory_order_acquire))
						return;
					if (active && Hub::ExtensionHub::Instance().OriginalPathway(host))
						return;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (shutdown_.load(std::memory_order_acquire))
							return;
						HostEntry* entry = FindOrAddLocked(host, active);
						if (entry != nullptr)
							entry->demandActive = active;
						desiredDepth_ = DesiredDepthLocked();
					}
					SyncRegistration(demand);
				}

				void OfferSource(const void* host, uint64_t seq, const AnalysisLumaSource& source,
					Skeleton::IExternalDemand* demand = nullptr)
				{
					if (host == nullptr || !source.IsValid() || shutdown_.load(std::memory_order_acquire)
						|| Hub::ExtensionHub::Instance().OriginalPathway(host))
						return;
					std::vector<AnalysisLumaSource> dropped;   // destroyed AFTER mutex_ unlock (lock-order invariant)
					bool needSync = false;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (shutdown_.load(std::memory_order_acquire))
							return;
						HostEntry* entry = FindOrAddLocked(host, true);
						if (entry != nullptr)
						{
							entry->offered = true;
							entry->lastOfferAt = entry->entrances;
							// S1-H1: only wipe on a true large backwards seek outside the active offer window
							// (seq + kMaxSourceOffers < ring.back().seq, written overflow-safe); in-window
							// out-of-order offers such as MotionAheadProducer's (rq.to, rq.from) insert or
							// update in sorted sequence order without dropping newer offers.
							if (!entry->ring.empty() && seq < entry->ring.back().seq
								&& (entry->ring.back().seq - seq) > static_cast<uint64_t>(kMaxSourceOffers))
							{
								for (Offer& o : entry->ring)
									dropped.push_back(std::move(o.source));
								entry->ring.clear();
							}
							auto it = entry->ring.begin();
							while (it != entry->ring.end() && it->seq < seq)
								++it;
							if (it != entry->ring.end() && it->seq == seq)
							{
								dropped.push_back(std::move(it->source));
								it->source = source;
							}
							else
							{
								entry->ring.insert(it, Offer{ seq, source });
								while (entry->ring.size() > kMaxSourceOffers)
								{
									dropped.push_back(std::move(entry->ring.front().source));
									entry->ring.pop_front();
								}
							}
						}
						const uint16_t wantDepth = DesiredDepthLocked();
						needSync = !registered_.load(std::memory_order_acquire) || wantDepth != desiredDepth_;
						desiredDepth_ = wantDepth;
					}
					if (needSync)
						SyncRegistration(demand);
				}

				void Forget(const void* host, Skeleton::IExternalDemand* demand = nullptr)
				{
					if (host == nullptr)
						return;
					std::vector<AnalysisLumaSource> dropped;   // destroyed AFTER mutex_ unlock
					{
						std::lock_guard<std::mutex> lock(mutex_);
						for (size_t i = 0; i < hosts_.size(); ++i)
						{
							if (hosts_[i].host == host)
							{
								for (Offer& o : hosts_[i].ring)
									dropped.push_back(std::move(o.source));
								hosts_.erase(hosts_.begin() + std::ptrdiff_t(i));
								break;
							}
						}
						desiredDepth_ = DesiredDepthLocked();
					}
					SyncRegistration(demand);
				}

				bool FindSource(const void* host, uint64_t seq, AnalysisLumaSource& out) const
				{
					if (host == nullptr || shutdown_.load(std::memory_order_acquire))
						return false;
					std::lock_guard<std::mutex> lock(mutex_);
					for (const HostEntry& h : hosts_)
					{
						if (h.host != host)
							continue;
						for (const Offer& o : h.ring)
						{
							if (o.seq == seq)
							{
								out = o.source;
								return true;
							}
						}
						return false;
					}
					return false;
				}

			private:
				struct Offer
				{
					uint64_t seq = 0;
					AnalysisLumaSource source;
				};

				struct HostEntry
				{
					const void* host = nullptr;
					bool demandActive = false;
					bool offered = false;
					uint64_t entrances = 0;
					uint64_t lastOfferAt = 0;
					std::deque<Offer> ring;

					bool Active() const
					{
						return demandActive
							|| (offered && entrances >= lastOfferAt
								&& (entrances - lastOfferAt) <= static_cast<uint64_t>(VpSkeleton::kAheadCeiling));
					}
				};

				HostEntry* FindOrAddLocked(const void* host, bool create)
				{
					for (HostEntry& h : hosts_)
						if (h.host == host)
							return &h;
					if (!create || hosts_.size() >= kMaxHosts)
						return nullptr;
					HostEntry added;
					added.host = host;
					hosts_.push_back(std::move(added));
					return &hosts_.back();
				}

				uint16_t DesiredDepthLocked() const
				{
					for (const HostEntry& h : hosts_)
						if (h.Active())
							return VpSkeleton::kAheadCeiling;
					return uint16_t(0);
				}

				VpSkeleton::OwnerFill MakeOwnerFill(uint16_t aheadDepth)
				{
					VpSkeleton::OwnerFill fill;
					fill.renderThreadOnly = false;   // zero render-thread wait: runs on asking worker thread
					fill.frameScoped = false;
					fill.pixelData = true;
					// Derived look-back (Rule 1): current frame + pair-start seq - 1 for motion_ahead_request.
					fill.keepFrames = 2;
					fill.aheadDepth = aheadDepth;
					fill.aheadBy = aheadDepth > 0 ? "motion_ahead" : nullptr;
					std::weak_ptr<P1CpuState> weakSelf = shared_from_this();
					fill.fill = [weakSelf](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&,
						VpSkeleton::OwnerFillOutput& out) -> bool {
						if (req.host != nullptr && Hub::ExtensionHub::Instance().OriginalPathway(req.host))
						{
							out.declineReason = "p1_cpu: original pathway";
							return false;
						}
						const std::shared_ptr<P1CpuState> self = weakSelf.lock();
						if (!self)
						{
							out.declineReason = "p1_cpu: module shut down";
							return false;
						}
						AnalysisLumaSource src;
						if (!self->FindSource(req.host, req.seq, src))
						{
							out.declineReason = "p1_cpu: no source offered for frame";
							return false;
						}
						VpFrameAnalysis::DecodeSharedLuma(src);
						VpSkeleton::SourceFrameView view;
						if (!VpFrameAnalysis::SourceViewOf(src, view)
							|| view.msbShift != VpFrameAnalysis::P1AcquireSource::kExactP1TapMsbShift)
						{
							out.declineReason = "p1_cpu: source view unavailable";
							return false;
						}
						const ::ComputeOnce::FrameScope frameScope(
							static_cast<int64_t>(req.seq), "p1-cpu-fill", static_cast<int64_t>(req.id));
						const VpSkeleton::Dims dims{ view.width, view.height };
						out.bytes.resize(VpLaneProducts::HalfLumaBytes(dims));
						if (!VpLaneProducts::ComputeHalfLuma(view, out.bytes.data(), out.bytes.size()))
						{
							out.bytes.clear();
							out.declineReason = "p1_cpu: half-luma compute failed";
							return false;
						}
						return true;
					};
					return fill;
				}

				void SyncRegistration(Skeleton::IExternalDemand* demand)
				{
					// Lock order: regMutex_ -> mutex_ (brief leaf read) -> unlock mutex_ -> demand calls.
					// Fill lambda takes only leaf mutex_ (never regMutex_), so zero deadlock or stale-depth race.
					std::lock_guard<std::mutex> regLock(regMutex_);
					if (shutdown_.load(std::memory_order_acquire))
						return;
					uint16_t wantDepth = 0;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (shutdown_.load(std::memory_order_acquire))
							return;
						wantDepth = DesiredDepthLocked();
						desiredDepth_ = wantDepth;
					}
					const bool ok = registered_.load(std::memory_order_acquire);
					if (ok && regDepth_ == wantDepth)
						return;
					if (demand == nullptr)
						demand = static_cast<Skeleton::IExternalDemand*>(
							Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
					if (demand == nullptr)
						return;
					// S1-H2: atomic in-place ReplaceOwnerFill when already registered (zero unregistered NoOwnerFill gap).
					const VpSkeleton::SkeletonMemo::Registration r = ok
						? demand->ReplaceOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu, kP1CpuUnit, MakeOwnerFill(wantDepth))
						: demand->RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu, kP1CpuUnit, MakeOwnerFill(wantDepth));
					const bool now = r == VpSkeleton::SkeletonMemo::Registration::Registered
						|| r == VpSkeleton::SkeletonMemo::Registration::Replaced;
					if (now)
					{
						regDepth_ = wantDepth;
						registered_.store(true, std::memory_order_release);
					}
					else if (!ok)
					{
						regDepth_ = uint16_t(0);
						registered_.store(false, std::memory_order_release);
					}
				}

				std::atomic<bool> shutdown_{ false };

				mutable std::mutex mutex_;   // leaf mutex: protects hosts_ and desiredDepth_ only
				std::vector<HostEntry> hosts_;
				uint16_t desiredDepth_ = 0;

				std::mutex regMutex_;        // serializes Skeleton IExternalDemand Register/Replace calls
				uint16_t regDepth_ = 0;
				std::atomic<bool> registered_{ false };
			};

			// S1-M3: lifetime-safe global slot holding a shared_ptr<P1CpuState> so callers snapshot a strong
			// handle before operating and concurrent teardown (ResetModulesForTesting / Shutdown) can never UAF.
			struct P1CpuSlot
			{
				std::mutex mutex;
				std::shared_ptr<P1CpuState> active;
			};

			P1CpuSlot& GlobalP1CpuSlot()
			{
				static P1CpuSlot slot;
				return slot;
			}

			std::shared_ptr<P1CpuState> SnapshotP1CpuState()
			{
				std::lock_guard<std::mutex> lock(GlobalP1CpuSlot().mutex);
				return GlobalP1CpuSlot().active;
			}

			class P1CpuModule : public Hub::ISkeletonModule
			{
			public:
				explicit P1CpuModule(std::shared_ptr<P1CpuState> state) : state_(std::move(state)) {}

				~P1CpuModule() override
				{
					{
						std::lock_guard<std::mutex> lock(GlobalP1CpuSlot().mutex);
						if (GlobalP1CpuSlot().active == state_)
							GlobalP1CpuSlot().active.reset();
					}
					if (state_)
						state_->Shutdown();
				}

				const char* Name() const override { return "input.source_analysis.p1_cpu"; }

				void OnLifecycle(RE::LifecycleFacts& f) override
				{
					if (f.host && state_ && (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown))
						state_->Forget(f.host);
				}

				bool OnFrame(RE::FrameContext& c) override
				{
					if (c.phase != RE::FramePhase::Entrance || c.host == nullptr || !state_)
						return false;
					state_->OnEntrance(c.host);
					return false;
				}

			private:
				std::shared_ptr<P1CpuState> state_;
			};
		}

		void RegisterP1CpuModule()
		{
			std::shared_ptr<P1CpuState> state = std::make_shared<P1CpuState>();
			{
				std::lock_guard<std::mutex> lock(GlobalP1CpuSlot().mutex);
				if (GlobalP1CpuSlot().active != nullptr)
					return;
				GlobalP1CpuSlot().active = state;
			}
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new P1CpuModule(std::move(state))));
		}

		void NoteP1CpuDemand(const void* host, bool active)
		{
			const std::shared_ptr<P1CpuState> state = SnapshotP1CpuState();
			if (!state || host == nullptr)
				return;
			state->NoteDemand(host, active);
		}

		void OfferP1CpuSource(const void* host, uint64_t seq, const AnalysisLumaSource& source)
		{
			const std::shared_ptr<P1CpuState> state = SnapshotP1CpuState();
			if (!state || host == nullptr)
				return;
			state->OfferSource(host, seq, source);
		}
	}
}
