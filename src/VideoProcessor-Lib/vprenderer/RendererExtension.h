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

#include <libplacebo/renderer.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Device;
class ConfigFile;

namespace RendererExtension
{
	struct Context { const void* host = nullptr; void* hostView = nullptr; };

	// ---- Lifecycle -------------------------------------------------------------------------
	enum class Lifecycle { GpuCreated, GpuReleased, SettingsApplied, DisplayChange, StateAccepted, StateRefused, Shutdown };
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
	enum class SettingsPhase { ReadProfile, ReadGlobal, PostLoad, Fingerprint, Delta, CopyLive };
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
	enum class FramePhase { Convert, AfterPublish, CollectAhead, QueueTarget, Begin, PlanOutputs, Entrance, Upload,
		CollectHooks, Draw, AfterRender, PlanPresent, AfterPresent, PresentSample };
	enum : uint32_t { kRequestMixerFlush = 1, kRequestComponentReset = 2 };

	// Phase payloads (generic, host-type based; later packages may extend them).
	struct OutputPlan { std::vector<int64_t> sequences; std::vector<double> phases; };
	struct PresentPlan { uint32_t interval = 1; uint32_t flags = 0; };
	struct PresentFacts { int64_t presentQpc = 0, endQpc = 0; uint32_t syncInterval = 1; bool dropped = false; };
	struct UploadJob { const void* source = nullptr; void* texture = nullptr; };
	struct ConvertJob { const void* frame = nullptr; const void* state = nullptr; void* payload = nullptr; };
	struct DrawJob { int errors = 0; int retried = 0; bool rendered = false; };
	struct QueueView { const void* queue = nullptr; size_t size = 0; };
	struct QueueTargetView { int64_t targetQpc = 0; size_t index = 0; };

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

	// True when an extension handled the phase (a take-over); the first handler stops the chain.
	inline bool Frame(FrameContext& c) noexcept
	{
		if (!Any()) return false;
		bool handled = false;
		detail::ForEach(c, [&](IRendererExtension& e) { if (!handled) handled = e.OnFrame(c); });
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
	inline void NotifyAfterRender(const void* host, int64_t seq, int64_t generation, int errors, int retried, bool rendered) noexcept
	{
		if (!Any()) return;
		DrawJob job; job.errors = errors; job.retried = retried; job.rendered = rendered;
		FrameContext c; c.host = host; c.phase = FramePhase::AfterRender; c.seq = seq; c.generation = generation; c.phaseData = &job;
		Frame(c);
	}
}
