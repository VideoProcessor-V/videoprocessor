// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Resync Q1 (plan 50 Q1, doc 57 section 1): the resync work behind the generic renderer hooks.
//
// ResyncExtension is the FIRST ISkeletonModule of the VP-V ExtensionHub. The renderer hands its
// resync facts to the hub at the existing call sites (ingress, queue generation, late slips, state
// accept/refuse, frame begin, settings apply, signal rate, status, command); this module turns them
// into the calls the renderer used to make itself on VpResync::Shadow / Drive, and owns the parts
// that had no home in the renderer: the ONE FrameSeal per frame (G3), the pulldown cursor (B2),
// the real queue capacity for the Indicator ring (G5), the software trigger (resync.inject, armed
// only with VP_RESYNC_INJECT=1, incl. VP_RESYNC_SCHEDULE), the DV metadata reader (null hooks
// until P13), the per-event settle metrics and the storm hold state.
//
// Ownership and lifetime: the renderer keeps owning its Shadow and Drive (its readers still take
// from the Drive at their own sites until their packages move them: doc 52 D2 alias). It declares
// one HostBinding member after them, keyed by its own identity (the `host` every hook passes);
// the binding owns the rest of the per-renderer state and is found again through a fixed,
// lock-free slot table (no lock on the ingress path). State is released only with that renderer,
// so a retiring renderer can never touch a replacement's state.
//
// Off (resync_event=off, or the Original pathway): every callback returns at the top; nothing is
// built, sealed or shown. Shadow: the facts reach the detector exactly as before; nothing is sealed.
// On: the seal and the readers act. A throw is contained by the hub (WorkerFault).

#include <VP-V/core/FrameSeal.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/input/queue_timing/PulldownResync.h>
#include <VP-V/modules/input/signal/DvResyncReader.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/input/signal/ResyncEventMetrics.h>
#include <VP-V/modules/input/signal/ResyncInject.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace Vpv
{
	namespace Resync
	{
		// Status line keys the renderer's module status reads back (VpStatusText::kResync / kResyncEvents).
		constexpr const char* kStatusLineKey = "resync";
		constexpr const char* kEventsLineKey = "resync_events";

		// Late-slip cause codes carried in QueueFacts::cause for QueueEvent::Drop.
		enum : int32_t { kDropKeptRunHole = 1, kDropDriftDrop = 2, kDropShedHeldExcess = 3 };
		// Refusal codes carried in LifecycleFacts::code for Lifecycle::StateRefused.
		enum : int32_t { kRefusalClassify = 0, kRefusalInitRace = 1, kRefusalSettingsRebuild = 2 };

		// The per-renderer state beyond the renderer's own Shadow / Drive.
		struct HostResync
		{
			HostResync(const void* hostKey, VpResync::Shadow& shadowRef, VpResync::Drive& driveRef);
			HostResync(const HostResync&) = delete;
			HostResync& operator=(const HostResync&) = delete;

			const void* host;
			VpResync::Shadow& shadow;   // owned by the renderer
			VpResync::Drive& drive;     // owned by the renderer

			// Render thread.
			VpSeal::FrameSeal seal;
			VpDvResync::Reader dv;
			size_t configuredCapacity = 0;   // queue capacity the Indicator ring was last sized for
			VpResync::Shadow::Mode mode = VpResync::Shadow::Mode::Off;
			int motionAheadFrames = 0;
			uint64_t lastSealedSeq = 0;
			uint64_t lastInjectSeq = 0;

			// Capture thread only (the pulldown runs in OnVideoFrame before the frame is queued).
			VpResync::Drive::Cursor pulldownCursor;

			// Any thread.
			VpResync::ResyncInject inject;
			VpResync::EventMetrics metrics;
			std::atomic<size_t> queueCapacity{ 0 };          // QueueFlush (BeginQueueGeneration, queue mutex)
			std::atomic<double> measuredRateHz{ 0.0 };       // last ingress measurement
			std::atomic<double> lastSealedSourceSeconds{ -1.0 };
			std::atomic<bool> stormHeld{ false };            // On only: the rendered frame is under the storm hold
			std::atomic<unsigned> sealLateProviders{ 0 };
			std::atomic<unsigned> sealRejectedReaders{ 0 };
			std::atomic<unsigned long long> injectUnavailable{ 0 };

			// Source seconds of recently enqueued frames, by sequence (single writer: the ingress, under the
			// renderer's queue mutex; reader: the render thread at the seal). A slot whose sequence does not
			// match reads as unknown (-1).
			static constexpr size_t kSourceTimeRing = 256;
			std::array<std::atomic<uint64_t>, kSourceTimeRing> ringSeq{};
			std::array<std::atomic<double>, kSourceTimeRing> ringSeconds{};
			void NoteSourceSeconds(uint64_t seq, double seconds);
			double SourceSecondsOf(uint64_t seq) const;

			// The render-thread status cache (seal / metrics / DV reader text), read by the GUI thread.
			mutable std::mutex statusMutex;
			std::string driveStatus;
		};

		// One member of the renderer's Impl, declared after its Shadow and Drive:
		//   Vpv::Resync::HostBinding resyncBinding{ this, resync, resyncDrive };
		// Registers this renderer's state under `host` for the lifetime of the member.
		class HostBinding
		{
		public:
			HostBinding(const void* host, VpResync::Shadow& shadow, VpResync::Drive& drive);
			~HostBinding();
			HostBinding(const HostBinding&) = delete;
			HostBinding& operator=(const HostBinding&) = delete;
			HostResync* State() const { return state_.get(); }
			bool Registered() const { return registered_; }

		private:
			std::unique_ptr<HostResync> state_;
			bool registered_ = false;
		};

		// What the pulldown does with the resync stamps for this frame (capture thread).
		struct PulldownStep
		{
			bool driving = false;        // resync_event = on: the timing re-latch replaces the fixed 0.5% rule
			bool reset = false;          // the renderer's reset request, or a FULL since the last frame
			bool relatchTiming = false;  // a timing event: re-latch the signal rate
		};
		// Today's R3 pulldown cursor rules, moved out of the renderer unchanged (capture thread).
		PulldownStep PulldownResyncStep(const void* host, bool resetRequest);

		class ResyncExtension : public Hub::ISkeletonModule
		{
		public:
			static constexpr const char* kQueryId = "vpv.resync/1";

			const char* Name() const override { return "resync"; }
			void OnLifecycle(RendererExtension::LifecycleFacts&) override;
			void OnIngress(RendererExtension::IngressContext&) override;
			void OnQueue(const RendererExtension::QueueFacts&) override;
			bool OnFrame(RendererExtension::FrameContext&) override;
			double OnResolve(RendererExtension::Resolve, const RendererExtension::Context&, double billValue) override;
			void OnStatus(const RendererExtension::Context&, RendererExtension::StatusKind, std::vector<std::string>&, const char*, const char*) override;
			bool OnCommand(const RendererExtension::Context&, const char*, const char*, std::string&) override;
			void* Query(const char* iid) override;

			// For later extensions (via Query(kQueryId)): this renderer's state and its FrameSeal, to add
			// providers / subscribers before frames flow (render thread). Null for an unknown host.
			static HostResync* For(const void* host);
			static VpSeal::FrameSeal* SealFor(const void* host);
			// True while the storm hold is in force for the frame last rendered by `host` (On only).
			static bool StormHeld(const void* host);
			// Bound hosts (tests / diagnostics) and bindings that found the slot table full.
			static size_t BoundHosts();
			static unsigned long long BindOverflows();
		};

		// Adds ResyncExtension to the hub. Start-up only; idempotent. Must run before any other module.
		void RegisterResyncExtension();
	}
}
