#pragma once

#include <VP-V/core/PassTimers.h>

#include <libplacebo/renderer.h>

#include <windows.h>

#include <cstdint>
#include <string>

// Efficiency timer plan (audit 2026-10-24 section 4): the renderer side of VP-V/core/PassTimers.h.
// The host gets one-line call sites only (vp-host-call-sites-only):
//   BeginFrame()            once per main refresh: decides whether this refresh is timed (1 in N)
//   Install(params)         chains a libplacebo info_callback (any existing one still runs) that
//                           adds each pass's `pass->last` (libplacebo's latest COMPLETED measurement
//                           of that pass: no wait, no flush) under the pass's description
//   CpuBegin / CpuMark      the RenderLocked CPU split; waits (vblank, present) are Kind::CpuWait
//   AddCpu                  a separately timed CPU interval (upload, an extra's vblank wait)
//   DueReadout(now)         the readout every intervalSeconds while frames are timed (the host logs
//                           it; the Ctrl+Alt+D bundle's log excerpt carries it)
// Off the timed refreshes every call returns at once. Nothing here changes a pixel.
namespace PassTimerHook
{
	class Hook
	{
	public:
		PassTimers::Accumulator acc;
		PassTimers::Sampler sampler{ 8 };
		double intervalSeconds = 30.0;

		bool BeginFrame()
		{
			sampling = sampler.Take(frames++);
			if (sampling)
				++timedFrames;
			return sampling;
		}
		bool Sampling() const { return sampling; }

		// Chains this hook in front of the params' callback for this render call.
		void Install(pl_render_params& params)
		{
			if (!sampling)
				return;
			if (params.info_callback == &Callback && params.info_priv == this)
				return;
			chainCallback = params.info_callback;
			chainPriv = params.info_priv;
			params.info_callback = &Callback;
			params.info_priv = this;
		}

		static void Callback(void* priv, const struct pl_render_info* info)
		{
			Hook* self = static_cast<Hook*>(priv);
			if (!self)
				return;
			if (info && info->pass && info->pass->shader)
				self->acc.AddGpuPassNs(info->pass->shader->description, info->pass->last);
			if (self->chainCallback)
				self->chainCallback(self->chainPriv, info);
		}

		static double NowMs()
		{
			static const double perMs = []() { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart > 0 ? static_cast<double>(f.QuadPart) / 1000.0 : 1.0; }();
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return static_cast<double>(t.QuadPart) / perMs;
		}
		void CpuBegin() { if (sampling) split.Begin(NowMs()); }
		void CpuMark(const char* name, PassTimers::Kind kind) { if (sampling) split.Mark(acc, name, kind, NowMs()); }
		void CpuEnd() { split.End(); }
		void AddCpu(const char* name, PassTimers::Kind kind, double ms) { if (sampling) acc.Add(kind, name, ms); }

		// The readout when due (empty otherwise); the statistics restart after each readout so
		// every readout covers its own interval.
		std::string DueReadout(double nowSeconds, size_t maxLines = 8)
		{
			if (timedFrames == 0 || acc.Empty())
				return std::string();
			if (lastReadout < 0.0)
			{
				lastReadout = nowSeconds;
				return std::string();
			}
			if (nowSeconds - lastReadout < intervalSeconds)
				return std::string();
			lastReadout = nowSeconds;
			std::string text = acc.Readout(sampler.EveryN(), maxLines);
			acc.Clear();
			timedFrames = 0;
			return text;
		}

	private:
		void (*chainCallback)(void*, const struct pl_render_info*) = nullptr;
		void* chainPriv = nullptr;
		PassTimers::CpuSplit split;
		uint64_t frames = 0;
		uint64_t timedFrames = 0;
		double lastReadout = -1.0;
		bool sampling = false;
	};
}
