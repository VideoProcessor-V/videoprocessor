// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// Generic, inert, phase-multiplexed extension points for the renderer. With nothing
// registered every dispatch helper returns immediately (or returns the caller's own value),
// so the renderer behaves exactly as without this header.
//
// Threading contract: Register() is called only during start-up, before any renderer,
// capture or file thread runs; afterwards the registry is read-only. Dispatch takes no
// lock. Registration order is dispatch order. ResetForTesting() is for unit tests only.
// Every context starts with the renderer identity (host) and an opaque per-renderer view
// (hostView): callbacks can run on a retirement worker after a replacement renderer has
// started, so extension state must be keyed by host. IngressContext runs on capture/file
// threads and must not block or lock. A throw from an extension never reaches the caller:
// it is reported to OnStatus(WorkerFault, "extension").
//
// M2 P0b (hook amendment R10-R20, doc 59 section 1.1): the per-frame phases PlanOutputs, PlanPresent,
// AfterPresent, PresentSample, ResolveTime, DecideIngress, Convert, AfterPublish, Upload and QueueTarget
// carry a typed payload in FrameContext::phaseData and have a one-line helper below. Every helper returns
// the caller's own value (or "not handled") when nothing is registered, and the host keeps its code as the
// fallback when no handler takes the phase over. Take-over phases are first-wins (the first handler that
// returns true stops the chain); fan-out phases call every extension in registration order (PhaseFansOut).
// A handler writes its payload only once it has succeeded: a throw is contained, but what it already wrote stays
// (except AfterPresent / R12, where the hub discards a throwing module's writes).

#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Device;
struct IDXGISwapChain;
class ConfigFile;
class VideoFrame;                    // host types the M2 phase payloads carry by pointer or reference
struct AlphaDxgiPresentationSample;

namespace RendererExtension
{
	struct Context { const void* host = nullptr; void* hostView = nullptr; };

	// ---- Lifecycle -------------------------------------------------------------------------
	enum class Lifecycle { GpuCreated, GpuReleased, SettingsApplied, DisplayChange, StateAccepted, StateRefused, Shutdown,
		QueueDepth /* value = frames the render-ahead plan keeps ahead of the present (a fact; change-only on the receiver) */ };
	struct LifecycleFacts : Context
	{
		Lifecycle kind = Lifecycle::GpuCreated;
		pl_gpu gpu = nullptr;
		ID3D11Device* device = nullptr;
		const void* subject = nullptr;
		const void* previous = nullptr;
		int32_t code = 0;
		double value = 0;
		int64_t detail = 0;
	};

	// ---- Settings load ---------------------------------------------------------------------
	enum class SettingsPhase { ReadProfile, ReadGlobal, PostLoad, Fingerprint, Delta, CopyLive, ReadBase };
	struct SettingsLoadContext : Context
	{
		SettingsPhase phase = SettingsPhase::ReadProfile;
		const ConfigFile* config = nullptr;
		const std::string* section = nullptr;
		const std::string* rule = nullptr;
		void* settings = nullptr; // the host's settings object
		const void* other = nullptr;
		std::string* out = nullptr; // fingerprint text sink
		std::vector<const char*>* changed = nullptr;
		bool includeLive = false;
		bool flag = false;        // phase input: ReadProfile = the rule's when: reads content; PostLoad = the host's own present_every_vsync
		void* result = nullptr;   // phase output: PostLoad = SettingsOverrides
	};

	// What an extension decides for settings the HOST owns (either/or), applied by the host after PostLoad.
	struct SettingsOverrides
	{
		bool hasPresentEveryVsync = false;
		bool presentEveryVsync = false;
		const char* chromaUpscaler = nullptr;   // nullptr = keep the host's value
	};

	// ---- Ingress (capture/file thread, before the frame is queued) ---------------------------
	struct IngressContext : Context
	{
		int64_t seq = -1, queueGeneration = 0, frameCounter = 0, timestamp = 0, ticksPerSecond = 0;
		uint64_t stateKey = 0, formatKey = 0;
		uint32_t gapFrames = 0;
		const char* gapCause = nullptr; // static text, valid for the call
		int64_t overflowSuccessor = 0; // != 0: an already-queued frame (this sequence) got an overflow-drop gap
		bool gapBefore = false, fromFile = false, discontinuity = false;
		double measuredRateHz = 0, nominalRateHz = 0;
		const void* frame = nullptr;
		const void* state = nullptr;
		// out
		bool drop = false;
		int64_t arrivalQpc = 0;
		double filmHz = 0;
		bool filmUpdate = false;
	};

	// ---- Queue -----------------------------------------------------------------------------
	enum class QueueEvent { Reset, LiveQueueReset, Flush, Drop, Discontinuity, Gap, FormatChange };
	struct QueueFacts : Context
	{
		QueueEvent kind = QueueEvent::Reset;
		int64_t seq = -1, generation = 0;
		uint32_t frames = 0;
		int32_t cause = 0;
		const char* text = nullptr;
	};

	// ---- Per-frame phases --------------------------------------------------------------------
	// New phases are appended (existing values never change). ResolveTime (R14) and DecideIngress (R15) are M2 P0b.
	enum class FramePhase { Convert, AfterPublish, CollectAhead, QueueTarget, Begin, PlanOutputs, Entrance, Upload,
		CollectHooks, Draw, AfterRender, PlanPresent, AfterPresent, PresentSample, ResolveTime, DecideIngress };
	// Fan-out phases: every extension is called in registration order and the result is the OR of the returns
	// (informational). Of the first five, ResolveTime and DecideIngress are new in M2 P0b; AfterPublish, QueueTarget,
	// AfterPresent and PresentSample already existed (first-wins, unused) and now fan out. M2 P4 adds Entrance
	// (the render thread's GPU-work and entrance-planning point), CollectHooks and AfterRender: observers, none of
	// the existing handlers returns true for them. All other phases keep the first-wins rule: the first handler
	// that returns true stops the chain.
	inline bool PhaseFansOut(FramePhase phase) noexcept
	{
		return phase == FramePhase::AfterPublish || phase == FramePhase::QueueTarget || phase == FramePhase::AfterPresent ||
			phase == FramePhase::PresentSample || phase == FramePhase::ResolveTime || phase == FramePhase::Entrance ||
			phase == FramePhase::CollectHooks || phase == FramePhase::AfterRender;
	}
	enum : uint32_t { kRequestMixerFlush = 1, kRequestComponentReset = 2 };

	// Phase payloads (generic, host-type based; later packages may extend them). The one-line helpers
	// at the end of the namespace fill FrameContext::phaseData with the matching payload.
	struct PresentFacts { int64_t presentQpc = 0, endQpc = 0; uint32_t syncInterval = 1; bool dropped = false; };
	// M2 P4. Draw (take-over, inside the host's draw callback; Continue = the host draws as before) and AfterRender
	// (fan-out, after the host's render call). Every pointer is valid for the call only; defaults are inert.
	enum class DrawResult { Continue, Drawn };
	struct DrawJob
	{
		int errors = 0; int retried = 0; bool rendered = false;
		const pl_render_errors* renderErrors = nullptr;   // AfterRender: libplacebo's error report of this render (read-only)
		const pl_frame* source = nullptr;                 // Draw: the picture the host is about to render
		pl_frame* frame = nullptr;                        // Draw: in/out, a handler may replace the picture
		const pl_frame* destination = nullptr;            // Draw: the target
		const pl_render_params* params = nullptr;         // Draw: this frame's parameters
		DrawResult result = DrawResult::Continue;         // Draw: out
	};
	struct QueueView { const void* queue = nullptr; size_t size = 0; };

	// R10 PlanOutputs (take-over; render loop, after the host's own timeline Begin, no renderMutex). In: the facts.
	// In/out: the host's output list. Default (no handler): the lists are left as the host built them.
	struct OutputPlan
	{
		int64_t seq = -1, nextSeq = -1, generation = 0;
		double ptsA = 0, ptsB = 0;                         // source times of A (shown) and B (next), seconds
		double filmHz = 0, captureHz = 0, displayHz = 0;
		bool discontinuity = false, fileFrame = false;
		size_t queueDepthAfterDequeue = 0, desiredDepth = 0;
		double holdFraction = 0;
		std::vector<double>* outputs = nullptr;            // the host's list; a handler may replace its content
		std::vector<double>* phases = nullptr;             // matching phases between A and B; null = not wanted
		bool skip = false;                                 // out: draw no output for this frame
		bool schedulesRefreshes = false;                   // out: the extension owns the refresh schedule
	};

	// R11 PlanPresent (take-over; render thread, renderMutex). Default interval 1 = the host's Present(1, 0).
	enum class PresentKind { Main, Repeat };
	struct PresentPlan
	{
		PresentKind kind = PresentKind::Main;
		IDXGISwapChain* native = nullptr;
		bool vpOwned = false;
		uint32_t interval = 1;
		uint32_t flags = 0;
	};

	// R12 AfterPresent (fan-out; render thread, renderMutex, after the main present). Each module writes only its own
	// contribution; the hub merges the contributions (extrasRan / committed OR, targetLeadMs / estimatedLeadMs max,
	// spentSeconds sum) and discards a throwing module's partial writes. Default = nothing ran.
	struct PresentOutcome
	{
		bool extrasRan = false, committed = false;
		double targetLeadMs = 0;      // raise-only: the host takes the larger of its own and this
		double estimatedLeadMs = 0;
		double spentSeconds = 0;
	};
	struct PresentContext
	{
		int64_t callbackQpc = 0, arrivalQpc = 0, mainCallQpc = 0, swapEndQpc = 0;
		double renderMs = 0, workMs = 0;
		pl_swapchain swapchain = nullptr;
		IDXGISwapChain* native = nullptr;
		const std::atomic<bool>* nextFrameArrived = nullptr;   // the host's flag: a newer source frame has arrived
		PresentOutcome outcome;                                // out
	};

	// R13 PresentSample (fan-out, chained: each handler sees the previous adjustment; render thread, in the
	// host's ObservePresent). Default: the sample is unchanged.
	struct PresentSampleView { AlphaDxgiPresentationSample* sample = nullptr; IDXGISwapChain* native = nullptr; };

	// R14 ResolveTime (fan-out, chained; render loop). Default: the host's value.
	enum class TimeKind { FramePresentEnd };
	struct TimeQuery { TimeKind kind = TimeKind::FramePresentEnd; int64_t billQpc = 0; int64_t qpc = 0; /* in/out, starts at billQpc */ };

	// R15 DecideIngress (take-over; capture thread, before the frame is queued; must not block or lock). Uses
	// IngressContext: the facts are the in fields; the caller pre-fills drop = false and gapBefore / gapFrames /
	// gapCause / discontinuity / filmHz / filmUpdate / arrivalQpc with the host's values, and a handler may
	// overwrite them (drop = a cadence repeat). Default (no handler): the host's values stand.
	// The R15 host is the renderer object itself (`this`), not its Impl: stable across Impl rebuilds (M2 P10-5).

	// R16 OnConvertedFrame (take-over; conversion thread, no queue lock) and R16 AfterPublish (fan-out; after the
	// payload is published and the queue notified). `payload` is the host's converted payload (opaque here);
	// `extension` is its R18 slot. The formatter mutex is reached through the callbacks (as in the host today).
	// Convert is first-wins: a handler that takes the frame (input.capture does for every raw route) ends the chain, so
	// a module that only OBSERVES converted frames uses AfterPublish (fan-out), or registers before the taker and
	// returns false.
	struct ConvertContext
	{
		const VideoFrame* frame = nullptr;
		const void* state = nullptr;
		void* payload = nullptr;
		std::shared_ptr<void>* extension = nullptr;
		void* formatterUser = nullptr;
		void (*lockFormatter)(void* user) = nullptr;
		void (*unlockFormatter)(void* user) = nullptr;
		// M2 P10: the host object a handler may call back (opaque here), and the handler's failure text. A handler that
		// returns true with a non-empty *error failed the frame exactly as the host's own conversion would have.
		void* hostObject = nullptr;
		std::string* error = nullptr;
	};
	struct PublishView
	{
		const VideoFrame* frame = nullptr;
		const void* state = nullptr;
		const void* payload = nullptr;
		std::shared_ptr<void> extension;                      // copy of the published payload's slot
	};

	// R17 TryUpload (take-over; render thread, renderMutex). Default false = the host's own upload.
	struct UploadContext
	{
		const void* payload = nullptr;
		const void* nextPayload = nullptr;
		const void* state = nullptr;
		int width = 0, height = 0;
		pl_tex (*textures)[2] = nullptr;
		pl_frame* image = nullptr;
		void* gpuTimer = nullptr;
		void* statusUser = nullptr;
		void (*setIngressStatus)(void* user, const char* text) = nullptr;
	};

	// R19 ResolveQueueTarget (fan-out, chained; any thread holding the host's queue mutex). Default = billTarget;
	// the helper clamps the chained result to capacity (when set).
	struct QueueTargetContext
	{
		size_t desired = 0, capacity = 0;
		bool holdActive = false;
		double holdFraction = 0;
		size_t cadenceScaledDepth = 0, creditFrames = 0, lookaheadFloor = 0;
		size_t billTarget = 0;                                // in: the host's own result
		size_t target = 0;                                    // in/out, starts at billTarget
	};

	// R20 timed file frames: the iid a file-frame provider answers to QueryExtension, the per-frame external timing
	// (the host's FileQueueFields, generic) and the host-side admission view the provider enqueues through. The
	// iid's interface text (vpv.timed_file_frames.v1) is owned by src/VP-V/abi/ (P2); P10 implements the view.
	constexpr const char kTimedFileFramesIid[] = "vpv.timed_file_frames.v1";
	struct ExternalTiming
	{
		int64_t presentationQpc = 0;
		bool paused = false, motionOff = false, discontinuity = false;
		double filmHz = 0;
	};
	class IQueueAdmission
	{
	public:
		virtual ~IQueueAdmission() = default;
		virtual void Enqueue(VideoFrame& frame, const void* state, uint64_t generation, int64_t callbackQpc, int64_t arrivalQpc,
			bool gapBefore, const char* gapCause, uint32_t gapFrames, double filmHz, bool filmUpdate, const ExternalTiming* external) = 0;
		virtual uint64_t QueueGeneration() const = 0;
		virtual bool SlotFree() const = 0;
		virtual uint64_t Dropped() const = 0;
	};

	struct FrameContext : Context
	{
		FramePhase phase = FramePhase::Begin;
		int64_t seq = -1, nextSeq = -1, generation = 0;
		bool cadenceRepeat = false;
		pl_frame* image = nullptr;
		pl_frame* target = nullptr;
		const pl_frame* baseTarget = nullptr;
		pl_render_params* params = nullptr;
		std::vector<const pl_hook*>* hooks = nullptr;
		int width = 0, height = 0;
		double captureRateHz = 0, nominalRateHz = 0;
		uint32_t requests = 0; // out: generic reset requests honoured by the host
		void* phaseData = nullptr; // one of the phase payload structs above
	};

	// ---- Resolve / status / command ----------------------------------------------------------
	enum class Resolve { SourceSignalRate, DisplayRefreshRate, PresentClockRate, PresentEndTime, TargetBlackNits, LayoutHold, UsablePanel };
	enum class StatusKind { Lines, Fallbacks, WorkerFault, WorkerOk };

	class IRendererExtension
	{
	public:
		virtual ~IRendererExtension() = default;
		virtual void OnLifecycle(LifecycleFacts&) {}
		virtual void OnSettingsLoad(SettingsLoadContext&) {}
		virtual void OnIngress(IngressContext&) {} // capture/file thread: must not block or lock
		virtual void OnQueue(const QueueFacts&) {}
		virtual bool OnFrame(FrameContext&) { return false; } // true = handled (take-over phases)
		virtual double OnResolve(Resolve, const Context&, double billValue) { return billValue; }
		virtual void OnStatus(const Context&, StatusKind, std::vector<std::string>&, const char* /*worker*/, const char* /*what*/) {}
		virtual bool OnCommand(const Context&, const char* /*module*/, const char* /*command*/, std::string& /*reply*/) { return false; }
		virtual void* Query(const char* /*iid*/) { return nullptr; }
	};

	using Registry = std::vector<std::unique_ptr<IRendererExtension>>;

	inline Registry& Extensions() { static Registry registry; return registry; }
	inline void Register(std::unique_ptr<IRendererExtension> extension)
	{
		if (extension) Extensions().push_back(std::move(extension));
	}
	inline bool Any() { return !Extensions().empty(); }
	inline void ResetForTesting() { Extensions().clear(); }

	namespace detail
	{
		inline bool& InFault() { static thread_local bool flag = false; return flag; }
		inline void Fault(const Context& ctx, const char* what) noexcept
		{
			if (InFault()) return; // a throw from a fault report is swallowed, never re-reported
			InFault() = true;
			for (auto& e : Extensions())
			{
				try { std::vector<std::string> none; e->OnStatus(ctx, StatusKind::WorkerFault, none, "extension", what); }
				catch (...) {}
			}
			InFault() = false;
		}

		template<class F> void ForEach(const Context& ctx, F&& f) noexcept
		{
			for (auto& e : Extensions())
			{
				try { f(*e); }
				catch (const std::exception& x) { Fault(ctx, x.what()); }
				catch (...) { Fault(ctx, "unknown exception"); }
			}
		}
	}

	inline void Notify(LifecycleFacts& f) noexcept { if (!Any()) return; detail::ForEach(f, [&](IRendererExtension& e) { e.OnLifecycle(f); }); }
	inline void SettingsLoad(SettingsLoadContext& c) noexcept { if (!Any()) return; detail::ForEach(c, [&](IRendererExtension& e) { e.OnSettingsLoad(c); }); }
	inline void Ingress(IngressContext& c) noexcept { if (!Any()) return; detail::ForEach(c, [&](IRendererExtension& e) { e.OnIngress(c); }); }
	inline void Queue(const QueueFacts& f) noexcept { if (!Any()) return; detail::ForEach(f, [&](IRendererExtension& e) { e.OnQueue(f); }); }

	// True when an extension handled the phase (a take-over); the first handler stops the chain. Fan-out
	// phases (PhaseFansOut) call every extension and return the OR of the results.
	inline bool Frame(FrameContext& c) noexcept
	{
		if (!Any()) return false;
		const bool fanOut = PhaseFansOut(c.phase);
		bool handled = false;
		detail::ForEach(c, [&](IRendererExtension& e)
		{
			if (fanOut) { if (e.OnFrame(c)) handled = true; }
			else if (!handled) handled = e.OnFrame(c);
		});
		return handled;
	}

	// Chained in registration order: each extension receives the previous result.
	inline double ResolveValue(Resolve kind, const Context& ctx, double billValue) noexcept
	{
		if (!Any()) return billValue;
		double value = billValue;
		detail::ForEach(ctx, [&](IRendererExtension& e) { value = e.OnResolve(kind, ctx, value); });
		return value;
	}

	inline void Status(const Context& ctx, StatusKind kind, std::vector<std::string>& lines, const char* worker = nullptr, const char* what = nullptr) noexcept
	{
		if (!Any()) return;
		detail::ForEach(ctx, [&](IRendererExtension& e) { e.OnStatus(ctx, kind, lines, worker, what); });
	}

	inline bool Command(const Context& ctx, const char* module, const char* command, std::string& reply) noexcept
	{
		if (!Any()) return false;
		bool handled = false;
		detail::ForEach(ctx, [&](IRendererExtension& e) { if (!handled) handled = e.OnCommand(ctx, module, command, reply); });
		return handled;
	}

	// First non-null answer in registration order.
	inline void* Query(const char* iid) noexcept
	{
		if (!Any()) return nullptr;
		void* found = nullptr;
		detail::ForEach(Context(), [&](IRendererExtension& e) { if (!found) found = e.Query(iid); });
		return found;
	}

	// ---- One-line call-site helpers (build the context from plain arguments) -------------------
	inline void NotifyLifecycle(Lifecycle kind, const void* host, pl_gpu gpu = nullptr, ID3D11Device* device = nullptr,
		const void* subject = nullptr, const void* previous = nullptr, int32_t code = 0, void* hostView = nullptr, double value = 0) noexcept
	{
		if (!Any()) return;
		LifecycleFacts f;
		f.host = host; f.hostView = hostView; f.kind = kind; f.gpu = gpu; f.device = device;
		f.subject = subject; f.previous = previous; f.code = code; f.value = value;
		Notify(f);
	}
	inline void NotifyQueue(QueueEvent kind, const void* host, int64_t seq = -1, int64_t generation = 0,
		uint32_t frames = 0, int32_t cause = 0, const char* text = nullptr) noexcept
	{
		if (!Any()) return;
		QueueFacts f;
		f.host = host; f.kind = kind; f.seq = seq; f.generation = generation; f.frames = frames; f.cause = cause; f.text = text;
		Queue(f);
	}
	inline double ResolveFor(Resolve kind, const void* host, double billValue) noexcept
	{
		if (!Any()) return billValue;
		Context c; c.host = host;
		return ResolveValue(kind, c, billValue);
	}
	inline void ReportWorker(const void* host, const char* worker, const char* what) noexcept // what == nullptr: worker ok
	{
		if (!Any()) return;
		Context c; c.host = host;
		std::vector<std::string> none;
		Status(c, what ? StatusKind::WorkerFault : StatusKind::WorkerOk, none, worker, what);
	}
	inline void CollectStatusLines(StatusKind kind, const void* host, std::vector<std::string>& lines) noexcept
	{
		if (!Any()) return;
		Context c; c.host = host;
		Status(c, kind, lines);
	}
	inline bool CommandFor(const void* host, const char* module, const char* command, std::string& reply) noexcept
	{
		if (!Any()) return false;
		Context c; c.host = host;
		return Command(c, module, command, reply);
	}
	// Appends the extension status lines (one per line) after `base`; unchanged when none are given.
	inline std::string WithStatusLines(StatusKind kind, const void* host, std::string base) noexcept
	{
		try
		{
			std::vector<std::string> lines;
			CollectStatusLines(kind, host, lines);
			std::string result = base;
			for (const std::string& l : lines) { result += "\n"; result += l; }
			base.swap(result);
		}
		catch (...) {}
		return base;
	}
	// A per-frame phase with no payload (e.g. Begin: the frame about to be rendered). True = handled.
	inline bool NotifyFrame(FramePhase phase, const void* host, int64_t seq, int64_t generation = 0) noexcept
	{
		if (!Any()) return false;
		FrameContext c; c.host = host; c.phase = phase; c.seq = seq; c.generation = generation;
		return Frame(c);
	}
	// Entrance (fan-out; render thread, renderMutex held, before the frame's hook list is built): the point where the
	// host's entrance-decision block starts. Handlers may record GPU work on their own objects and read the facts; they
	// never write the frame's textures, params or hooks (70b section 9.2).
	// OPEN ITEM: 'never write' is a contract, not enforced by the types: image and target are non-const pl_frame pointers;
	// const-ness comes in a later package (audit 2 of M2 P4).
	inline void NotifyEntrance(const void* host, int64_t seq, int64_t nextSeq, int64_t generation, pl_frame* image, pl_frame* target,
		int width, int height, bool cadenceRepeat, double captureRateHz) noexcept
	{
		if (!Any()) return;
		FrameContext c; c.host = host; c.phase = FramePhase::Entrance; c.seq = seq; c.nextSeq = nextSeq; c.generation = generation;
		c.image = image; c.target = target; c.width = width; c.height = height; c.cadenceRepeat = cadenceRepeat;
		c.captureRateHz = captureRateHz;
		Frame(c);
	}
	inline void NotifyAfterRender(const void* host, int64_t seq, int64_t generation, int errors, int retried, bool rendered,
		const pl_render_errors* renderErrors = nullptr) noexcept
	{
		if (!Any()) return;
		DrawJob job; job.errors = errors; job.retried = retried; job.rendered = rendered; job.renderErrors = renderErrors;
		FrameContext c; c.host = host; c.phase = FramePhase::AfterRender; c.seq = seq; c.generation = generation; c.phaseData = &job;
		Frame(c);
	}

	// ---- M2 P0b one-line helpers (R10-R19). Each returns the caller's own value / false with nothing registered. ----
	namespace detail
	{
		template<class T> bool Phase(FramePhase phase, const void* host, int64_t seq, int64_t generation, T& payload) noexcept
		{
			FrameContext c; c.host = host; c.phase = phase; c.seq = seq; c.generation = generation; c.phaseData = &payload;
			return Frame(c);
		}
	}
	// The typed payload of a phase (null when the host passed none).
	template<class T> T* PayloadOf(FrameContext& c) noexcept { return static_cast<T*>(c.phaseData); }

	inline bool PlanOutputs(const void* host, OutputPlan& plan) noexcept // R10: true = a handler replaced the plan
	{
		if (!Any()) return false;
		return detail::Phase(FramePhase::PlanOutputs, host, plan.seq, plan.generation, plan);
	}
	inline bool PlanPresent(const void* host, PresentPlan& plan) noexcept // R11: plan.interval stays 1 unless a handler decides
	{
		if (!Any()) return false;
		return detail::Phase(FramePhase::PlanPresent, host, -1, 0, plan);
	}
	inline PresentOutcome AfterPresent(const void* host, int64_t seq, int64_t generation, PresentContext& pc) noexcept // R12
	{
		if (Any()) detail::Phase(FramePhase::AfterPresent, host, seq, generation, pc);
		return pc.outcome;
	}
	inline void AdjustPresentationSample(const void* host, AlphaDxgiPresentationSample& sample, IDXGISwapChain* native) noexcept // R13
	{
		if (!Any()) return;
		PresentSampleView view; view.sample = &sample; view.native = native;
		detail::Phase(FramePhase::PresentSample, host, -1, 0, view);
	}
	inline int64_t ResolveTime(const void* host, TimeKind kind, int64_t billQpc) noexcept // R14
	{
		if (!Any()) return billQpc;
		TimeQuery q; q.kind = kind; q.billQpc = billQpc; q.qpc = billQpc;
		detail::Phase(FramePhase::ResolveTime, host, -1, 0, q);
		return q.qpc;
	}
	inline bool DecideIngress(IngressContext& c) noexcept // R15: true = a handler decided (read drop and the out fields)
	{
		if (!Any()) return false;
		return detail::Phase(FramePhase::DecideIngress, c.host, c.seq, c.queueGeneration, c);
	}
	inline bool OnConvertedFrame(const void* host, ConvertContext& cc) noexcept // R16: false = the host's own formatter
	{
		if (!Any()) return false;
		return detail::Phase(FramePhase::Convert, host, -1, 0, cc);
	}
	inline void AfterPublish(const void* host, PublishView& pv) noexcept // R16
	{
		if (!Any()) return;
		detail::Phase(FramePhase::AfterPublish, host, -1, 0, pv);
	}
	inline bool TryUpload(const void* host, UploadContext& uc) noexcept // R17: false = the host's own upload
	{
		if (!Any()) return false;
		return detail::Phase(FramePhase::Upload, host, -1, 0, uc);
	}
	inline size_t ResolveQueueTarget(const void* host, QueueTargetContext& qc, size_t billTarget) noexcept // R19
	{
		qc.billTarget = billTarget; qc.target = billTarget;
		if (!Any()) return billTarget;
		detail::Phase(FramePhase::QueueTarget, host, -1, 0, qc);
		// A handler never takes the target past the queue's capacity (or the host's own value, if that is larger).
		const size_t ceiling = qc.capacity > billTarget ? qc.capacity : billTarget;
		if (qc.capacity > 0 && qc.target > ceiling) qc.target = ceiling;
		return qc.target;
	}
}
