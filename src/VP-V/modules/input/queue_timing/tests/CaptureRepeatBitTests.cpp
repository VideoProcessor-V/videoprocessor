// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// C8-PROD (R14-59): the always-on ingress capture signature and its captureRepeat bit. New tests only (a new file).
// CPU only, deterministic: the bit survives on unlocked queued frames, pulldown on/off parity of the shared hash compare, the consumer
// rows of R14-57 (frozen-12, dup2, still, aerial crop, Missing/stale -> block test backup), the diag line, and the measured cost of
// one 64-row hash at 4K.

#include <VP-V/modules/input/queue_timing/CadenceFeed.h>
#include <VP-V/modules/repair/grain_split/GrainLevel.h>
#include <VP-V/core/FallbackCatalog.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CaptureRepeatBitNs
{
	using GrainLevel::CaptureBit;
	using GrainLevel::RepeatFact;

	inline void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	inline void Require(bool ok, const std::string& what)
	{
		if (!ok) Log("FAILED: " + what);
		const std::wstring w(what.begin(), what.end());
		Assert::IsTrue(ok, w.c_str());
	}

	constexpr size_t kRow = 256, kRows = 128;
	// A frame whose sampled rows depend on `picture` (a repeat is the same picture id).
	inline std::vector<uint8_t> MakeFrame(uint32_t picture)
	{
		std::vector<uint8_t> f(kRow * kRows);
		uint32_t s = picture * 2654435761u + 12345u;
		for (auto& b : f) { s = s * 1664525u + 1013904223u; b = static_cast<uint8_t>(s >> 24); }
		return f;
	}
	inline uint64_t Sig(uint32_t picture) { const std::vector<uint8_t> f = MakeFrame(picture); return Pulldown::Signature(f.data(), kRow, kRows); }

	// The ingress per-frame step, as IngressPolicy.inl runs it: ONE signature, the always-on capture compare, and (when on) the
	// pulldown's own compare against its own stored signature (m_pulldownSignature).
	struct Ingress
	{
		Vpv::Ingress::CaptureSignature capture;
		uint64_t pulldownSignature = 0;
		bool pulldownRepeat = false;
		Vpv::Ingress::CadenceFacts Frame(uint32_t picture, bool pulldownOn)
		{
			const uint64_t sig = Sig(picture);
			Vpv::Ingress::CadenceFacts facts;
			facts.captureValid = true; facts.valid = true;
			facts.captureRepeat = capture.Step(sig);
			if (pulldownOn)
			{
				pulldownRepeat = sig != 0 && sig == pulldownSignature;
				pulldownSignature = sig;
			}
			return facts;
		}
	};

	TEST_CLASS(CaptureRepeatBitTests)
	{
	public:
		TEST_METHOD(TheBitIsKeptOnUnlockedQueuedFrames)
		{
			Ingress in;
			const Pulldown::Decision unlocked;   // not locked: the frame is queued, a repeat included
			const Vpv::Ingress::CadenceFacts a = in.Frame(1, true), b = in.Frame(1, true), c = in.Frame(2, true);
			const FrameAnalysis::Cadence ra = Vpv::Ingress::ToRecord(Vpv::Ingress::FactsOf(unlocked, a));
			const FrameAnalysis::Cadence rb = Vpv::Ingress::ToRecord(Vpv::Ingress::FactsOf(unlocked, b));
			const FrameAnalysis::Cadence rc = Vpv::Ingress::ToRecord(Vpv::Ingress::FactsOf(unlocked, c));
			Require(ra.captureValid && !ra.captureRepeat, "first frame: bit valid, not a repeat");
			Require(rb.captureValid && rb.captureRepeat, "unlocked queued repeat: the bit is kept");
			Require(rc.captureValid && !rc.captureRepeat, "a new picture: Different");
			Require(rb.filmHz == 0.0 && rb.phase == -1, "pulldown fields stay unknown while unlocked");
			Require(!Vpv::Ingress::ToRecord(Vpv::Ingress::CadenceFacts{}).captureValid, "no signature taken (non-capture source): not valid");
		}

		TEST_METHOD(PulldownOnAndOffGiveTheSameBitsAndTheSameDrops)
		{
			// 3:2 cadence in 60 Hz: pictures 1 1 2 2 2 3 3 4 4 4 ...
			const uint32_t pics[] = { 1, 1, 2, 2, 2, 3, 3, 4, 4, 4, 5, 5, 6, 6, 6 };
			Ingress on, off;
			for (uint32_t p : pics)
			{
				const bool bitOn = on.Frame(p, true).captureRepeat, bitOff = off.Frame(p, false).captureRepeat;
				Require(bitOn == bitOff, "the capture bit is identical with pulldown on or off");
				Require(on.pulldownRepeat == bitOn, "pulldown repeat (the drop input) is the unchanged compare");
			}
			// toggle edge: pulldown off for a stretch keeps its stored signature (m_pulldownSignature semantics unchanged),
			// while the capture compare keeps tracking every frame.
			Ingress t;
			t.Frame(10, true);
			t.Frame(11, false); t.Frame(12, false);       // pulldown off: its signature stays at picture 10
			const Vpv::Ingress::CadenceFacts f = t.Frame(12, true);
			Require(f.captureRepeat, "capture compare tracked through the off stretch: 12 repeats 12");
			Require(!t.pulldownRepeat, "pulldown compares against its old stored signature exactly as before (not a repeat)");
		}

		TEST_METHOD(ConsumerRowsFromTheBits)
		{
			const int cur = 8;   // window of 13: 8 back, current, 4 ahead
			// frozen-12: every frame repeats its predecessor
			std::vector<CaptureBit> frozen(13, CaptureBit::Same);
			int decided = 0;
			std::vector<RepeatFact> f = GrainLevel::ResolveRepeatFacts(frozen, cur, &decided);
			int repeats = 0;
			for (int i = 0; i < 13; ++i) if (i != cur && f[static_cast<size_t>(i)] == RepeatFact::Repeat) ++repeats;
			Require(repeats == 12 && decided == 12, "frozen-12: all twelve neighbours are Repeat, decided by bits");
			// dup2: t-1 and t+1 repeat t (bit(t) Same, bit(t+1) Same), the rest differ
			std::vector<CaptureBit> dup(13, CaptureBit::Different); dup[cur] = CaptureBit::Same; dup[cur + 1] = CaptureBit::Same;
			f = GrainLevel::ResolveRepeatFacts(dup, cur, &decided);
			Require(f[cur - 1] == RepeatFact::Repeat && f[cur + 1] == RepeatFact::Repeat, "dup2: the two repeats");
			Require(f[cur - 2] == RepeatFact::Distinct && f[cur + 2] == RepeatFact::Distinct && decided == 12, "dup2: the rest are Distinct, all decided by bits");
			// still / aerial crop: independent frames, every bit Different: nothing fires
			std::vector<CaptureBit> indep(13, CaptureBit::Different);
			f = GrainLevel::ResolveRepeatFacts(indep, cur);
			bool none = true;
			for (int i = 0; i < 13; ++i) if (f[static_cast<size_t>(i)] == RepeatFact::Repeat) none = false;
			Require(none, "still / aerial crop: 0 false fires from the bits");
			// Missing (store off, stale epoch, late, non-capture): Unknown, the block test runs as the backup
			std::vector<CaptureBit> missing(13, CaptureBit::Missing);
			f = GrainLevel::ResolveRepeatFacts(missing, cur, &decided);
			bool allUnknown = true;
			for (int i = 0; i < 13; ++i) if (f[static_cast<size_t>(i)] != RepeatFact::Unknown) allUnknown = false;
			Require(allUnknown && decided == 0, "Missing bits: Unknown for every neighbour, nothing decided");
			// the chain needs every bit: a Missing makes it Unknown; a Different anywhere wins
			std::vector<CaptureBit> mixed(13, CaptureBit::Same); mixed[cur - 1] = CaptureBit::Missing;
			f = GrainLevel::ResolveRepeatFacts(mixed, cur);
			Require(f[cur - 1] == RepeatFact::Repeat && f[cur - 2] == RepeatFact::Unknown, "a Missing in the chain makes the farther neighbour Unknown");
			mixed[cur - 1] = CaptureBit::Different;
			f = GrainLevel::ResolveRepeatFacts(mixed, cur);
			Require(f[cur - 2] == RepeatFact::Distinct, "a Different in the chain: Distinct");
		}

		TEST_METHOD(DiagLineNamesTheSourceAndTheCounts)
		{
			const std::string bits = FallbackCatalog::GrainSepRepeatDiagLine(false, 12, 12, 0, 0);
			Require(bits.find("capture bit") != std::string::npos && bits.find("droppedByFacts 12") != std::string::npos && bits.find("blockTestRuns 0") != std::string::npos, "all by bits: " + bits);
			const std::string bt = FallbackCatalog::GrainSepRepeatDiagLine(false, 12, 0, 3, 12);
			Require(bt.find("block test") != std::string::npos && bt.find("droppedByBlockTest 3") != std::string::npos, "all by the block test: " + bt);
			Require(FallbackCatalog::GrainSepRepeatDiagLine(true, 12, 0, 0, 0).find("off") != std::string::npos, "off");
		}

		// Measurement (R14-59 item 5): one 64-row hash of a 4K frame, the always-on cost. Reported in the TRX output.
		TEST_METHOD(HashCostAt4KIsMeasured)
		{
			const size_t rowBytes = 3840 * 4, height = 2160;   // 8-bit BGRA-sized rows, the widest common row
			std::vector<uint8_t> buf(rowBytes * height, 0x5A);
			for (size_t i = 0; i < buf.size(); i += 4099) buf[i] = static_cast<uint8_t>(i);
			uint64_t acc = 0;
			const int n = 2000;
			for (int i = 0; i < 50; ++i) acc ^= Pulldown::Signature(buf.data(), rowBytes, height);   // warm
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < n; ++i) { buf[static_cast<size_t>(i) % 4096] ^= 1; acc ^= Pulldown::Signature(buf.data(), rowBytes, height); }
			const auto t1 = std::chrono::steady_clock::now();
			const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / n;
			Log("MEASURED always-on 64-row hash at 4K (3840x2160, rowBytes 15360): " + std::to_string(ms) + " ms per frame (acc " + std::to_string(acc) + ")");
			Require(ms < 0.5, "the always-on hash stays under 0.5 ms per 4K frame (estimate 0.05)");
		}
	};
}
