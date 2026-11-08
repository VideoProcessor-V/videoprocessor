// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared fixtures of the SID tests: P14g records and reading evidence built by hand (no estimator needed).
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainReading.h>

#include <cstdint>

namespace Tests
{
	namespace GeoFx
	{
		namespace G = SourceIdentity::Geometry;
		namespace C = SourceIdentity::Chain;

		// A CONFIDENT (applied) axis: n of N, KernelBank index `kernel` (-1 = unsure).
		inline G::AxisGeom AppliedAxis(int n, int N, int kernel)
		{
			G::AxisGeom a;
			a.state = G::LatchState::Latched;
			a.n = n;
			a.N = N;
			a.ratio = static_cast<float>(static_cast<double>(n) / static_cast<double>(N));
			a.kernelIndex = static_cast<int8_t>(kernel);
			a.kernelFamily = kernel >= 0 ? static_cast<uint8_t>(C::Kernel(kernel).family) : G::kKernelFamilyUnknown;
			a.kernelConf = kernel >= 0 ? 0.9f : 0.2f;
			a.conf = 0.99f;
			a.llr = 20.0f;
			return a;
		}

		inline G::AxisGeom MeasuredAxis(G::LatchState st, int n, int N)
		{
			G::AxisGeom a;
			a.state = st;
			a.n = n;
			a.N = N;
			a.ratio = N > 0 ? static_cast<float>(static_cast<double>(n) / static_cast<double>(N)) : 1.0f;
			a.llr = 3.0f;
			return a;
		}

		inline G::Record MakeRecord(uint64_t seq, int64_t timeUs, const G::AxisGeom& h, const G::AxisGeom& v)
		{
			G::Record r;
			r.evalSeq = seq;
			r.evalTimeUs = timeUs;
			r.maxAgeUs = 20000000;
			r.applied[0] = h;
			r.applied[1] = v;
			r.measured[0] = h;
			r.measured[1] = v;
			r.geometryGen = 1;
			r.appliedGen = 1;
			return r;
		}

		// Reading level like the shipped evidence settings (c1 about 14.9).
		inline C::ReadingLevels Levels()
		{
			C::EvidenceSettings es;
			return C::ReadingLevels::From(C::ComputeThresholds(es));
		}

		// One informative evaluation of an axis of N pixels: the accumulated totals (best n with `bestLlr`, native with `nativeLlr`) and
		// this evaluation's increments (`incN` gets `inc`, native gets `nativeInc` when `nativeCounted`).
		struct Ev
		{
			C::FieldEvidence e;
			explicit Ev(int N)
			{
				e.evaluated = true;
				e.informative = true;
				e.axisLen = N;
			}
			Ev& Best(int n, double llr) { e.hasBest = true; e.bestN = n; e.bestLlr = llr; return *this; }
			Ev& Prior(double prior) { e.prior = prior; return *this; }   // the two-axis prior INSIDE bestLlr (negative: the axes disagree)
			Ev& Native(double llr, int evals = 1) { e.nativeLlr = llr; e.nativeEvals = evals; return *this; }
			Ev& Inc(int n, double inc, int kernel = -1, double kconf = 0.0, uint32_t kernelClass = 0)
			{
				C::HypothesisIncrement& h = e.inc[e.incCount++];
				h.n = n; h.increment = inc; h.kernelIndex = kernel; h.kernelConf = kconf; h.kernelClass = kernelClass;
				return *this;
			}
			Ev& NativeInc(double inc) { e.nativeCounted = true; e.nativeIncrement = inc; return *this; }
			Ev& Hold() { e.informative = false; e.hold = true; return *this; }
			Ev& NotInformative(uint32_t reason) { e.informative = false; e.reason = reason; return *this; }
		};

		inline C::EvalContext Ctx(int64_t timeUs)
		{
			C::EvalContext c;
			c.timeUs = timeUs;
			c.seq = static_cast<uint64_t>(timeUs / 1000);
			return c;
		}
	}
}
