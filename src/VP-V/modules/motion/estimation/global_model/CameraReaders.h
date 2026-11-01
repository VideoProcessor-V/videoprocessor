// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link layer (K0): how camera-model state reacts to Indicator changes. One reader policy
// table for every camera component (so each stays correct at the same discontinuities):
//   full             Reset     a source change / seek: the fit history is meaningless
//   continuity       Reset     a slip (drop, repeat, resync): temporal state spans a gap
//   timing           Relatch   frame timing changed: re-derive dt, keep the state
//   rebaseH/V        Ignore    the camera model works on the source plane, not the re-based one
//   settings[motion] Reset     motion settings changed the vectors the fit reads
//   graph            Reset     a graph rebuild invalidates in-flight lane work
// Pure, header-only, C++14, no Windows, no GPU.
#pragma once

#include <VP-V/core/Indicator.h>

#include "CameraAccessor.h"
#include "GlobalModelFit.h"
#include "GlobalModelTemporal.h"

#include <array>
#include <cstdint>
#include <functional>

namespace CameraLink
{
	inline std::array<VpIndicator::Policy, VpIndicator::kCounterCount> CameraReaderPolicies()
	{
		using namespace VpIndicator;
		std::array<Policy, kCounterCount> p{};
		for (Policy& x : p)
			x = Policy::Ignore;
		p[kFull] = Policy::Reset;
		p[kContinuity] = Policy::Reset;
		p[kTiming] = Policy::Relatch;
		p[kRebaseH] = Policy::Ignore;
		p[kRebaseV] = Policy::Ignore;
		p[kSettingsMotion] = Policy::Reset;
		p[kGraph] = Policy::Reset;
		return p;
	}

	// The counters the camera readers observe (every one with a non-Ignore policy, plus the explicit
	// Ignore rebase pair).
	inline VpIndicator::CounterMask CameraReaderReads()
	{
		using namespace VpIndicator;
		return Bit(kFull) | Bit(kContinuity) | Bit(kTiming) | Bit(kRebaseH) | Bit(kRebaseV)
			| Bit(kSettingsMotion) | Bit(kGraph);
	}

	struct CameraReaderHooks
	{
		std::function<void()> reset;
		std::function<void()> relatchTiming;
	};

	// Translates one Change through the policies the reader registered with: any relevant Reset
	// counter calls reset() ONCE; a relevant Relatch counter calls relatchTiming() once; Ignore (and any
	// other policy) does nothing. Returns true when anything was called.
	inline bool ApplyCameraChange(const VpIndicator::Indicator::Change& c, const CameraReaderHooks& hooks)
	{
		bool doReset = false;
		bool doRelatch = false;
		if (c.policies)
		{
			for (int i = 0; i < int(VpIndicator::kCounterCount); ++i)
			{
				if ((c.relevant & (VpIndicator::CounterMask(1u) << unsigned(i))) == 0)
					continue;
				const VpIndicator::Policy p = (*c.policies)[size_t(i)];
				if (p == VpIndicator::Policy::Reset)
					doReset = true;
				else if (p == VpIndicator::Policy::Relatch)
					doRelatch = true;
			}
		}
		if (doReset && hooks.reset)
			hooks.reset();
		if (doRelatch && hooks.relatchTiming)
			hooks.relatchTiming();
		return doReset || doRelatch;
	}

	// The hooks run INSIDE Indicator::Observe on the render thread: they must be short and must NOT call
	// Observe or SealAndObserve (re-entrant Observe is not guarded); Advance is allowed (clamped above the seal).
	//
	// Registers one camera reader (setup only, before the first Observe). Returns the reader index
	// or -1 (Indicator::Register).
	inline int RegisterCameraReader(VpIndicator::Indicator& indicator, const char* name, CameraReaderHooks hooks)
	{
		return indicator.Register(name, CameraReaderReads(), CameraReaderPolicies(),
			[hooks](const VpIndicator::Indicator::Change& c) { ApplyCameraChange(c, hooks); });
	}

	// Convenience binders: Reset the component; they have no timing state to relatch (the temporal
	// filter takes dt per call). The referenced object must outlive the Indicator's readers.
	// NOT for cross-thread use: the hook runs on the thread that calls Observe (the render thread), while a Fitter belongs
	// to ONE motion worker (Fitter is not thread-safe). Binding it here would reset it under the worker's feet. Use it
	// only when the Fitter and Observe share a thread (tests, single-threaded hosts); K1 resets the worker's fitter by
	// posting the reset to that worker instead.
	inline CameraReaderHooks BindFitter(CameraModel::Fitter& f)
	{
		CameraReaderHooks h;
		h.reset = [&f]() { f.Reset(); };
		return h;
	}
	inline CameraReaderHooks BindTemporalFilter(CameraModel::TemporalFilter& t)
	{
		CameraReaderHooks h;
		h.reset = [&t]() { t.Reset(); };
		return h;
	}
	inline CameraReaderHooks BindBlendState(CameraModel::BlendState& b)
	{
		CameraReaderHooks h;
		h.reset = [&b]() { b.Reset(); };
		return h;
	}

	// Runs both hook sets (a then b) in that order.
	inline CameraReaderHooks Merge(CameraReaderHooks a, CameraReaderHooks b)
	{
		CameraReaderHooks h;
		h.reset = [a, b]() { if (a.reset) a.reset(); if (b.reset) b.reset(); };
		h.relatchTiming = [a, b]() { if (a.relatchTiming) a.relatchTiming(); if (b.relatchTiming) b.relatchTiming(); };
		return h;
	}

	// True when frames `from` and `to` are separated by a slip (or a full change): their continuity
	// stamps differ, so a pair or a smoothing window spanning them is invalid. An ancient stamp
	// (event ring overwritten) also reports true: never temporal.
	inline bool PairSpansSlip(const VpIndicator::Indicator& indicator, uint64_t from, uint64_t to)
	{
		const VpIndicator::CounterMask m = VpIndicator::Bit(VpIndicator::kFull) | VpIndicator::Bit(VpIndicator::kContinuity);
		return !VpIndicator::TemporalValid(indicator, from, to, m);
	}
}
