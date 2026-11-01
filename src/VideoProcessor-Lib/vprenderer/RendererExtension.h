// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// Generic, inert extension points for the renderer. With nothing registered every
// dispatch helper returns immediately (or returns the caller's own value), so the
// renderer behaves exactly as without this header.
//
// Threading contract: Register() is called only during start-up, before any renderer,
// capture or file thread runs; afterwards the registry is read-only. Dispatch takes no
// lock. Registration order is dispatch order. ResetForTesting() is for unit tests only.
// Callbacks marked "must not block" run on capture/file threads. A throw from an
// extension never reaches the caller: it is reported to OnWorkerFault("extension", what).

#include <libplacebo/renderer.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Device;

namespace RendererExtension
{
	enum class RateKind { SourceSignal, DisplayRefresh, PresentClock };
	enum class Event { SourceFrame, FrameRetired, FrameDropped, FrameTiming, Cut, Discontinuity, QueueReset,
		QueueFlush, SourceFormatChange, CadenceLock, CadenceUnlock, GateTransition, DeviceError };
	enum class RenderPhase { FrameStart, BeforeSubmit, AfterSubmit, PostSubmitGap, Presented };
	enum class StatusKind { Lines, Fallbacks, Traces };

	struct EventFacts
	{
		Event kind = Event::SourceFrame;
		int64_t sequence = -1;
		int64_t timeTicks = 0; // 100 ns units, QPC-derived
		double value = 0;
		int64_t detail = 0;
		const char* text = nullptr;
	};

	struct IngressFacts
	{
		int64_t sequence = -1;
		int64_t arrivalTicks = 0;
		int64_t sourceTicks = 0;
		double nominalRateHz = 0;
		double measuredRateHz = 0;
		int width = 0;
		int height = 0;
		int encoding = 0;
		bool discontinuity = false;
	};

	struct RenderPhaseFacts
	{
		int64_t sequence = -1;
		int64_t timeTicks = 0;
		double budgetMs = 0;
		double spentMs = 0;
	};

	struct SettingsSource
	{
		virtual ~SettingsSource() = default;
		virtual bool Get(const char* section, const char* key, std::string& value) const = 0;
	};

	class IRendererExtension
	{
	public:
		virtual ~IRendererExtension() = default;
		virtual void OnSettings(const SettingsSource&) {}
		virtual void OnGpuCreated(pl_gpu, ID3D11Device*) {}
		virtual void OnGpuReleased() {}
		virtual void OnIngress(const IngressFacts&) {} // capture/file thread: must not block or lock
		virtual void OnEvent(const EventFacts&) {}
		virtual void OnRenderPhase(RenderPhase, const RenderPhaseFacts&) {} // render thread
		virtual double ResolveRate(RateKind, double billValue) { return billValue; }
		virtual void OnWorkerFault(const char*, const char*) {}
		virtual void OnWorkerOk(const char*) {}
		virtual void CollectHooks(int, std::vector<const pl_hook*>&) {}
		virtual void CollectStatus(StatusKind, std::vector<std::string>&) {}
		virtual void* QueryInterface(const char*) { return nullptr; }
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
		inline void Fault(const char* what) noexcept
		{
			for (auto& e : Extensions())
			{
				try { e->OnWorkerFault("extension", what); }
				catch (...) {}
			}
		}

		template<class F> void ForEach(F&& f) noexcept
		{
			for (auto& e : Extensions())
			{
				try { f(*e); }
				catch (const std::exception& x) { Fault(x.what()); }
				catch (...) { Fault("unknown exception"); }
			}
		}
	}

	inline void Settings(const SettingsSource& s) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnSettings(s); }); }
	inline void GpuCreated(pl_gpu gpu, ID3D11Device* dev) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnGpuCreated(gpu, dev); }); }
	inline void GpuReleased() noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnGpuReleased(); }); }
	inline void Ingress(const IngressFacts& f) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnIngress(f); }); }
	inline void Emit(const EventFacts& f) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnEvent(f); }); }
	inline void Phase(RenderPhase p, const RenderPhaseFacts& f) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnRenderPhase(p, f); }); }
	inline void WorkerFault(const char* w, const char* what) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnWorkerFault(w, what); }); }
	inline void WorkerOk(const char* w) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.OnWorkerOk(w); }); }
	inline void CollectHooks(int stage, std::vector<const pl_hook*>& out) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.CollectHooks(stage, out); }); }
	inline void CollectStatus(StatusKind k, std::vector<std::string>& out) noexcept { if (!Any()) return; detail::ForEach([&](IRendererExtension& e) { e.CollectStatus(k, out); }); }

	// Chained in registration order: each extension receives the previous result.
	inline double ResolveRate(RateKind kind, double billValue) noexcept
	{
		if (!Any()) return billValue;
		double value = billValue;
		detail::ForEach([&](IRendererExtension& e) { value = e.ResolveRate(kind, value); });
		return value;
	}

	// First non-null answer in registration order.
	inline void* Query(const char* iid) noexcept
	{
		if (!Any()) return nullptr;
		void* found = nullptr;
		detail::ForEach([&](IRendererExtension& e) { if (!found) found = e.QueryInterface(iid); });
		return found;
	}
}
