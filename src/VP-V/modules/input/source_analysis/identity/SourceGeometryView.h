// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The ONE read API of the P14g record's CONTENT (doc 78 D14, section 6): every consumer that wants the geometry of the delivered
// picture calls Confident() or Provisional(), never the record's fields directly. Registered as a shared unit (tools/check_shared_units.py,
// U21). It reads through the skeleton's SourceGeometry::Read() (SourceGeometryProduct.h): the cause of an unusable record (absent, stale,
// another unit version, not sealed yet, interval not measured) and its visible fallback decision come from there.
//
//   Confident(rec, delivered, nowUs)    the APPLIED geometry: only axes the chain reads CONFIDENT (above c1; not sticky). For re-base, TD's coded
//                                       raster, the adaptive scaler and the chroma stage.
//   Provisional(rec, delivered, nowUs)  the MEASURED geometry (the current best, moves at boundaries). For the frame-pattern
//                                       (quality_pattern / coding_evidence) detector's grid placement and for discontinuity gens.
//
// The two return DIFFERENT types (ConfidentGeometry, ProvisionalGeometry; neither converts to the other), so a consumer cannot hand a
// provisional value to code that needs a decision by accident.
//
// Record absent, unusable (see Read()): the DELIVERED geometry (ratio 1, n = N = the delivered size) and an engaged FallbackCatalog
// decision `product.source_geometry` naming the consumer and the cause (Ctrl+I line, Config status, Ctrl+Alt+D); it recovers by itself
// when a fresh record returns (the next call). A fresh record whose axis is UNSURE is NOT a fallback: the axis simply makes
// no claim (confident = false, delivered values). ConfidentOf() is the single definition of "which axes carry a claim" (the consensus voter
// and the gate use it too).
//
// Pure functions over plain structs (no monitor, no clock: `nowUs` is passed in). Header-only, C++14.
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <cstdint>
#include <limits>
#include <string>

namespace SourceGeometry
{
	// The delivered raster (the active area when the caller knows it, else the frame): the stand-in geometry.
	struct Delivered
	{
		int width = 0, height = 0;
	};

	struct KernelView
	{
		uint8_t family = kKernelFamilyUnknown;
		int index = -1;                 // KernelBank index, -1 = unknown ("n known, kernel unsure")
		double paramA = 0.0, paramB = 0.0;
		double conf = 0.0;
		bool Known() const { return index >= 0; }
	};

	struct ConfidentAxis
	{
		bool confident = false;         // the reading is CONFIDENT on this axis (applied state Latched = the reserved numeric of confident)
		LatchState state = LatchState::Unknown;
		int n = 0, N = 0;               // n == N when native or when no claim is made
		double ratio = 1.0;
		double phase = 0.0;
		KernelView kernel;
		SharpenGeom sharpen;
		DoubleGeom dbl;
		double conf = 0.0;
		double llr = 0.0;
		uint32_t reasonBits = 0;
	};

	struct ConfidentGeometry
	{
		bool fromRecord = false;        // false: the delivered geometry stands in (see fallback / reason)
		ConfidentAxis axis[2];
		uint64_t appliedGen = 0;        // bumps when an applied field changed (0 without a record)
		ActiveArea area;
		CodedRaster codedRaster = CodedRaster::Unknown;
		unsigned cause = 0;             // kCause* when the delivered geometry stands in
		std::string reason;             // CauseWord(cause) ("" with a usable record)
		FallbackCatalog::Decision fallback;   // engaged exactly when !fromRecord
		bool AnyConfident() const { return axis[0].confident || axis[1].confident; }
	};

	struct ProvisionalAxis
	{
		bool known = false;             // a best hypothesis exists (state != Unknown, n > 0)
		LatchState state = LatchState::Unknown;   // the MEASURED state (the provisional hypothesis, not the decision)
		int n = 0, N = 0;
		double ratio = 1.0;
		double phase = 0.0;
		KernelView kernel;
		SharpenGeom sharpen;
		DoubleGeom dbl;
		double conf = 0.0;
		double llr = 0.0;
		uint32_t reasonBits = 0;
	};

	struct ProvisionalGeometry
	{
		bool fromRecord = false;
		ProvisionalAxis axis[2];
		uint64_t geometryGen = 0;       // bumps when a measured field changed (a discontinuity source for pattern detectors)
		uint64_t appliedGen = 0;
		ActiveArea area;
		CodedRaster codedRaster = CodedRaster::Unknown;
		unsigned cause = 0;
		std::string reason;
		FallbackCatalog::Decision fallback;
		bool AnyKnown() const { return axis[0].known || axis[1].known; }
	};

	namespace detail
	{
		inline KernelView KernelOf(const AxisGeom& a)
		{
			KernelView k;
			k.family = a.kernelFamily;
			k.index = a.kernelIndex;
			k.paramA = a.kernelParamA;
			k.paramB = a.kernelParamB;
			k.conf = a.kernelConf;
			return k;
		}

		inline int DeliveredSize(const Delivered& d, int axis) { return axis == 0 ? d.width : d.height; }

		// The skeleton's Read() over the record: `rec` may be null (nothing published). The call itself is the request. `readCause` is
		// what HeldState::ReadLatest said when it returned no record: an undecodable newest record (another unit version, corrupt) is
		// named, not reported as "absent".
		inline ReadResult ReadOf(const Record* rec, unsigned readCause, Consumer consumer, uint64_t frameSeq, int64_t nowUs)
		{
			if (rec == nullptr && (readCause == kCauseUnitVersion || readCause == kCauseCorrupt))
			{
				ReadResult r;
				r.requested = true;
				r.cause = readCause;
				r.reason = std::string(Name(consumer)) + ": source geometry " + CauseWord(readCause) + ", " + ClassicalForm(consumer);
				r.fallback = FallbackCatalog::ProductFallback(kFallbackId, r.reason);
				return r;
			}
			Entry e;
			if (rec != nullptr)
				e = rec->AsEntry();
			return Read(consumer, &e, true, frameSeq, nowUs);
		}

		// The record was already chosen by HeldState::ReadLatest(seq): any sequence is fine here.
		constexpr uint64_t kAnySeq = (std::numeric_limits<uint64_t>::max)();
	}

	// Which axes carry a claim (applied state Latched = confident, with a size), with their applied values. No freshness check: the caller
	// has one (Read() / Record::Cause()). The delivered values stand in for an axis without a claim.
	inline ConfidentGeometry ConfidentOf(const Record& rec, const Delivered& delivered)
	{
		ConfidentGeometry g;
		g.fromRecord = true;
		g.fallback = FallbackCatalog::Released(kFallbackId);
		g.appliedGen = rec.appliedGen;
		g.area = rec.area;
		g.codedRaster = rec.codedRaster;
		for (int a = 0; a < 2; ++a)
		{
			const AxisGeom& s = rec.applied[a];
			ConfidentAxis& o = g.axis[a];
			o.n = o.N = detail::DeliveredSize(delivered, a);
			o.state = s.state;
			o.conf = s.conf;
			o.llr = s.llr;
			o.reasonBits = s.reasonBits;
			// Only a decided axis makes a claim; Unstable applies the higher n by design (visible through the state).
			o.confident = s.state == LatchState::Latched && s.n > 0 && s.N > 0;
			if (!o.confident)
				continue;
			o.n = s.n;
			o.N = s.N;
			o.ratio = s.ratio;
			o.phase = s.phase;
			o.kernel = detail::KernelOf(s);
			o.sharpen = s.sharpen;
			o.dbl = s.dbl;
		}
		return g;
	}

	inline ConfidentGeometry Confident(const Record* rec, const Delivered& delivered, int64_t nowUs,
		Consumer consumer = Consumer::Status, uint64_t frameSeq = detail::kAnySeq, unsigned readCause = 0)
	{
		const ReadResult rr = detail::ReadOf(rec, readCause, consumer, frameSeq, nowUs);
		if (!rr.usable)
		{
			ConfidentGeometry g;
			for (int a = 0; a < 2; ++a)
				g.axis[a].n = g.axis[a].N = detail::DeliveredSize(delivered, a);   // the delivered geometry stands in
			g.cause = rr.cause;
			g.reason = CauseWord(rr.cause);
			g.fallback = rr.fallback;
			return g;
		}
		return ConfidentOf(*rec, delivered);
	}

	inline ProvisionalGeometry Provisional(const Record* rec, const Delivered& delivered, int64_t nowUs,
		Consumer consumer = Consumer::QualityPattern, uint64_t frameSeq = detail::kAnySeq, unsigned readCause = 0)
	{
		ProvisionalGeometry g;
		for (int a = 0; a < 2; ++a)
			g.axis[a].n = g.axis[a].N = detail::DeliveredSize(delivered, a);
		const ReadResult rr = detail::ReadOf(rec, readCause, consumer, frameSeq, nowUs);
		if (!rr.usable)
		{
			g.cause = rr.cause;
			g.reason = CauseWord(rr.cause);
			g.fallback = rr.fallback;
			return g;
		}
		g.fromRecord = true;
		g.fallback = FallbackCatalog::Released(kFallbackId);
		g.geometryGen = rec->geometryGen;
		g.appliedGen = rec->appliedGen;
		g.area = rec->area;
		g.codedRaster = rec->codedRaster;
		for (int a = 0; a < 2; ++a)
		{
			const AxisGeom& s = rec->measured[a];
			ProvisionalAxis& o = g.axis[a];
			o.state = s.state;
			o.conf = s.conf;
			o.llr = s.llr;
			o.reasonBits = s.reasonBits;
			o.known = s.state != LatchState::Unknown && s.n > 0 && s.N > 0;
			if (!o.known)
				continue;
			o.n = s.n;
			o.N = s.N;
			o.ratio = s.ratio;
			o.phase = s.phase;
			o.kernel = detail::KernelOf(s);
			o.sharpen = s.sharpen;
			o.dbl = s.dbl;
		}
		return g;
	}

	// The reasons of an abstention as words (Chain::kReason* bits), "" when none of them applies.
	inline std::string ReasonWords(uint32_t bits)
	{
		std::string w;
		auto add = [&](uint32_t bit, const char* text) {
			if ((bits & bit) == 0)
				return;
			if (!w.empty())
				w += ", ";
			w += text;
		};
		add(SourceIdentity::Chain::kReasonDark, "dark");
		add(SourceIdentity::Chain::kReasonFlat, "flat");
		add(SourceIdentity::Chain::kReasonSoft, "soft");
		add(SourceIdentity::Chain::kReasonNoPower, "no power");
		add(SourceIdentity::Chain::kReasonLowDetail, "low detail");
		add(SourceIdentity::Chain::kReasonMixed, "overlay");
		// SID r2 (M4): the safety-rule bits (zero while the rules are off).
		add(SourceIdentity::Chain::kReasonNonConventional, "not a conventional size");
		add(SourceIdentity::Chain::kReasonLatchBar, "latch bar");
		add(SourceIdentity::Chain::kReasonLatchRelease, "latch released");
		add(SourceIdentity::Chain::kReasonMenuHold, "menu");
		add(SourceIdentity::Chain::kReasonAbrStep, "rendition step");
		return w;
	}

	// The chain part of the Ctrl+I / Config status line (doc 78 section 7): the confident geometry per axis, or why the classical
	// voters decide ("classical (chain: <state>: <reasons>)"). `p` supplies the measured state and reasons of an UNSURE axis.
	inline std::string ChainStatusText(const ConfidentGeometry& g, const ProvisionalGeometry& p)
	{
		if (!g.fromRecord)
			return std::string("classical (chain: ") + g.reason + ")";
		static const char* const kAxisName[2] = { "H", "V" };
		std::string out;
		bool any = false;
		for (int a = 0; a < 2; ++a)
		{
			if (!out.empty())
				out += "; ";
			if (g.axis[a].confident)
			{
				any = true;
				out += kAxisName[a];
				if (g.axis[a].n == g.axis[a].N)
					out += " native";
				else
				{
					out += " " + std::to_string(g.axis[a].n) + "->" + std::to_string(g.axis[a].N) + " ";
					out += g.axis[a].kernel.Known() ? SourceIdentity::Chain::Kernel(g.axis[a].kernel.index).name : "kernel unsure";
				}
			}
			else
			{
				out += std::string(kAxisName[a]) + " classical (" + StateText(p.axis[a].state);
				const std::string why = ReasonWords(p.axis[a].reasonBits);
				if (!why.empty())
					out += ": " + why;
				out += ")";
			}
		}
		return any ? "chain: " + out : "classical (chain: " + std::string(StateText(p.axis[0].state)) + ")";
	}

	// The status wording of the confident view: its fallback while the delivered geometry stands in, else the per-axis states.
	inline std::string StatusText(const ConfidentGeometry& g)
	{
		if (!g.fromRecord)
			return std::string("Source geometry: delivered (") + g.reason + ")";
		return std::string("Source geometry: H ") + StateText(g.axis[0].state) + ", V " + StateText(g.axis[1].state);
	}
}
