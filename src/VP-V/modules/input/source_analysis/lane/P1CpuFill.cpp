// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// D3 / FIX3-D6 (102b CO-DEFAULT-MOTIONAHEAD-U3): zero-wait no-lane CPU owner fill for kP1HalfLuma.
// Built without the precompiled header (pure CPU + VP-V headers only; zero pl_gpu / zero libplacebo).
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

			class P1CpuModule;

			std::atomic<P1CpuModule*>& RegisteredP1CpuModule()
			{
				static std::atomic<P1CpuModule*> module{ nullptr };
				return module;
			}

			class P1CpuModule : public Hub::ISkeletonModule
			{
			public:
				~P1CpuModule() override
				{
					P1CpuModule* self = this;
					RegisteredP1CpuModule().compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
				}

				const char* Name() const override { return "input.source_analysis.p1_cpu"; }

				void OnLifecycle(RE::LifecycleFacts& f) override
				{
					if (f.host && (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown))
						Forget(f.host);
				}

				bool OnFrame(RE::FrameContext& c) override
				{
					if (c.phase != RE::FramePhase::Entrance || c.host == nullptr)
						return false;
					if (Hub::ExtensionHub::Instance().OriginalPathway(c.host))
						return false;
					bool needSync = false;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						for (HostEntry& h : hosts_)
						{
							if (h.host == c.host)
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
					return false;
				}

				void NoteDemand(const void* host, bool active, Skeleton::IExternalDemand* demand = nullptr)
				{
					if (host == nullptr)
						return;
					if (active && Hub::ExtensionHub::Instance().OriginalPathway(host))
						return;
					{
						std::lock_guard<std::mutex> lock(mutex_);
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
					if (host == nullptr || !source.IsValid() || Hub::ExtensionHub::Instance().OriginalPathway(host))
						return;
					std::vector<AnalysisLumaSource> dropped;   // destroyed AFTER mutex_ unlock (lock-order invariant)
					bool needSync = false;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						HostEntry* entry = FindOrAddLocked(host, true);
						if (entry != nullptr)
						{
							entry->offered = true;
							entry->lastOfferAt = entry->entrances;
							if (!entry->ring.empty() && seq < entry->ring.back().seq)
							{
								// Sequence regression (seek / stream restart): drop stale offers cleanly.
								for (Offer& o : entry->ring)
									dropped.push_back(std::move(o.source));
								entry->ring.clear();
							}
							bool updated = false;
							for (Offer& o : entry->ring)
							{
								if (o.seq == seq)
								{
									dropped.push_back(std::move(o.source));
									o.source = source;
									updated = true;
									break;
								}
							}
							if (!updated)
							{
								while (entry->ring.size() >= kMaxSourceOffers)
								{
									dropped.push_back(std::move(entry->ring.front().source));
									entry->ring.pop_front();
								}
								entry->ring.push_back(Offer{ seq, source });
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
					if (host == nullptr)
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
					P1CpuModule* const self = this;
					fill.fill = [self](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&,
						VpSkeleton::OwnerFillOutput& out) -> bool {
						if (req.host != nullptr && Hub::ExtensionHub::Instance().OriginalPathway(req.host))
						{
							out.declineReason = "p1_cpu: original pathway";
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
					uint16_t wantDepth = 0;
					{
						std::lock_guard<std::mutex> lock(mutex_);
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
					if (ok)
						demand->UnregisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu, kP1CpuUnit);
					const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(
						VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu, kP1CpuUnit, MakeOwnerFill(wantDepth));
					const bool now = r == VpSkeleton::SkeletonMemo::Registration::Registered;
					regDepth_ = now ? wantDepth : uint16_t(0);
					registered_.store(now, std::memory_order_release);
				}

				mutable std::mutex mutex_;   // leaf mutex: protects hosts_ and desiredDepth_ only
				std::vector<HostEntry> hosts_;
				uint16_t desiredDepth_ = 0;

				std::mutex regMutex_;        // serializes Skeleton IExternalDemand Register/Unregister calls
				uint16_t regDepth_ = 0;
				std::atomic<bool> registered_{ false };
			};
		}

		void RegisterP1CpuModule()
		{
			if (RegisteredP1CpuModule().load(std::memory_order_acquire) != nullptr)
				return;
			P1CpuModule* const module = new P1CpuModule();
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(module));
			RegisteredP1CpuModule().store(module, std::memory_order_release);
		}

		void NoteP1CpuDemand(const void* host, bool active)
		{
			P1CpuModule* const module = RegisteredP1CpuModule().load(std::memory_order_acquire);
			if (module == nullptr || host == nullptr)
				return;
			module->NoteDemand(host, active);
		}

		void OfferP1CpuSource(const void* host, uint64_t seq, const AnalysisLumaSource& source)
		{
			P1CpuModule* const module = RegisteredP1CpuModule().load(std::memory_order_acquire);
			if (module == nullptr || host == nullptr)
				return;
			module->OfferSource(host, seq, source);
		}
	}
}
