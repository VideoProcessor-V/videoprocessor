/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Slice 21 W21-2 (W8): the source-resolution look-ahead at a 12-bit code range (CodedLumaSampler, 4095). It reaches the same decisions
// as the 10-bit read of the same picture, its axis-level blank threshold keeps its meaning (a one-native-code texture is still blank),
// and the walk of a packed DV frame reads the frame's one shared 12-bit decode when the host kept it, else its own samples, counted
// once per visit. CPU only. New class in a new file (rule 28: SourceResolutionHookTests is not edited).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/CodedLumaSampler.h>
#include <VP-V/modules/input/source_analysis/SourceAheadWalk.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>

#include <VideoFrameEncoding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LookAheadAt4095TestsNs
{
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Monitor = SourceResolutionMonitor::Monitor;
	using VpSkeleton::SourceFrameView;

	// 4096 r210 words (alignment 2, big endian): pixel `code` (row-major, 64 wide) carries intensity code. Limited range: the tap is the code.
	struct Ramp
	{
		std::vector<uint8_t> bytes;
		AnalysisLumaSource source;
		Ramp()
		{
			const int width = 64, height = 64;
			bytes.assign(static_cast<size_t>(width) * height * 4, 0);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t word = ((code >> 4) << 10) | (code & 15u);
				for (unsigned k = 0; k < 4; ++k) bytes[code * 4 + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
			}
			source.format = AnalysisLumaFormat::PackedDolbyVision422;
			source.encoding = VideoFrameEncoding::R210;
			source.width = width;
			source.height = height;
			source.rgbBitAlignment = 2;
			source.fullRangeYuv = false;
			source.data = bytes.data();
			source.dataBytes = bytes.size();
			source.rowBytes = static_cast<size_t>(width) * 4;
		}
	};

	// ---- the walk, run against a fake host (the LookAheadWalkTests pattern) -------------------------------------------------------
	struct FakeMode { double RefreshRateHz() const { return 24.0; } };
	struct FakeState { std::shared_ptr<FakeMode> displayMode; bool dolbyVisionTunnel = true; };
	struct FakeFrame
	{
		long long GetTimingTimestamp() const { return 0; }
		void SourceBufferAddRef() {}
	};
	struct FakeIdentity { int sourceFormatGeneration = 0; };
	struct FakeQueued
	{
		FakeFrame frame;
		std::shared_ptr<FakeState> state;
		std::shared_ptr<const int> converted;
		std::shared_ptr<int> analysis;
		FakeIdentity activePictureIdentity;
		unsigned long long sourceSequence = 0;
	};
	struct Prepared { AnalysisLumaSource source; };

	// What the fake monitor saw: each LookAheadFrame call's sequence, the declared code range and five samples read through the sampler.
	struct Seen
	{
		std::vector<uint64_t> sequences;
		std::vector<uint32_t> codeMaximums;
		std::vector<int> samples;   // five per call: pixel (0,0), (1,0), (0,1), (63,0), (63,63)
		int plainCalls = 0;
	};

	struct FakeMonitor
	{
		Seen* seen = nullptr;
		template <class F> void LookAheadFrame(unsigned long long sequence, int, int, F&& f, int, int, int, int)
		{
			Visit(sequence, f, std::is_same<typename std::decay<F>::type, SourceResolutionMonitor::CodedLumaSampler>());
		}
		template <class F> void Visit(unsigned long long, F&, std::false_type) { ++seen->plainCalls; }
		template <class F> void Visit(unsigned long long sequence, F& f, std::true_type)
		{
			seen->sequences.push_back(sequence);
			seen->codeMaximums.push_back(f.codeMaximum);
			const int at[5][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 63, 0 }, { 63, 63 } };
			for (const auto& p : at) seen->samples.push_back(f(p[0], p[1]));
		}
	};

	AnalysisLumaSource* gPrepared = nullptr;

	struct FakeImpl
	{
		FallbackStatus::Registry fallbacks;
		void SyncFallback(const FallbackCatalog::Decision& d) { FallbackCatalog::Sync(fallbacks, d); }
		FallbackCatalog::LookAheadFailures sourceAheadFailures = FallbackCatalog::SourceResolutionWorker();
		struct { struct { FakeMonitor sourceMonitor; } sourceAnalysis; } vpv;
		template <class... A> static Prepared PrepareFrameAnalysis(A&&...)
		{
			Prepared p;
			p.source = *gPrepared;
			return p;
		}
	};

	// One walk visit of the frame `sequence` of `source`.
	void Walk(FakeImpl& impl, AnalysisLumaSource& source, uint64_t sequence)
	{
		gPrepared = &source;
		std::vector<FakeQueued> frames(1);
		frames[0].state = std::make_shared<FakeState>();
		frames[0].state->displayMode = std::make_shared<FakeMode>();
		frames[0].sourceSequence = sequence;
		SourceResolution::SourceAheadView<FakeImpl, FakeQueued> view{ impl, frames };
		SourceResolution::RunAheadWalk(view);
		gPrepared = nullptr;
	}

	struct ComputeOnceGuard
	{
		ComputeOnceGuard() { ComputeOnce::SetEnabled(true); ComputeOnce::Global().Reset(); }
		~ComputeOnceGuard() { ComputeOnce::Global().Reset(); ComputeOnce::SetEnabled(false); }
	};

	int PrivateAheadRows(uint32_t& frames, uint64_t& calls)
	{
		int rows = 0;
		frames = 0;
		calls = 0;
		for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
			if (r.unit == "analysis.tap12" && r.variant == "private:source-look-ahead samples")
			{
				++rows;
				frames = static_cast<uint32_t>(r.frames);
				calls = r.calls;
			}
		return rows;
	}

	// ---- the look-ahead fixture (SourceResolutionHookTests shape), through two monitors in lockstep ----------------------------------
	const int FW = 1024, FH = 512;

	std::vector<double> Upscaled(unsigned seed, double level)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> noise(0.0, 0.08);
		std::vector<double> f(static_cast<size_t>(FW) * FH);
		const int lw = FW / 2, lh = FH / 2;
		std::vector<double> low(static_cast<size_t>(lw) * lh);
		for (double& v : low) v = level + noise(rng);
		for (int y = 0; y < FH; ++y)
			for (int x = 0; x < FW; ++x)
			{
				const double u = (x + 0.5) * 0.5 - 0.5, w = (y + 0.5) * 0.5 - 0.5;
				const int x0 = (std::max)(0, (std::min)(lw - 2, static_cast<int>(std::floor(u))));
				const int y0 = (std::max)(0, (std::min)(lh - 2, static_cast<int>(std::floor(w))));
				const double fx = (std::min)(1.0, (std::max)(0.0, u - x0)), fy = (std::min)(1.0, (std::max)(0.0, w - y0));
				auto at = [&](int xx, int yy) { return low[static_cast<size_t>(yy) * lw + xx]; };
				f[static_cast<size_t>(y) * FW + x] = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
			}
		return f;
	}

	std::vector<double> Native(unsigned seed, double level)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> noise(0.0, 0.08);
		std::vector<double> f(static_cast<size_t>(FW) * FH);
		for (double& v : f) v = level + noise(rng);
		return f;
	}

	int Code10(double v) { return static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 1023.0)); }

	// The monitor's own analysis of a displayed frame (8 tiles, 10-bit codes), fed identically to both monitors.
	void Analyse(Monitor& m, const std::vector<double>& f, double t)
	{
		const int T = Monitor::kTile;
		std::vector<float> tiles(static_cast<size_t>(8) * T * T);
		for (int i = 0; i < 8; ++i)
		{
			const int x0 = (i % 4) * (FW - T) / 3, y0 = (i / 4) * (FH - T);
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x)
					tiles[(static_cast<size_t>(i) * T + y) * T + x] = static_cast<float>(Code10(f[static_cast<size_t>(y0 + y) * FW + x0 + x]) / 1023.0);
		}
		m.AnalyseTilesForTest(tiles, 8, t, FW, FH);
	}

	bool WaitReady(Monitor& m)
	{
		for (int tries = 0; tries < 400 && !m.Ahead().ready; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return m.Ahead().ready;
	}

	// Upscaled shots until auto waits for the next cut, one monitor (`b` may be null: the lockstep partner). Returns the time reached.
	double UntilWaiting(Monitor& a, Monitor* b, uint64_t& seq)
	{
		double t = 0.0;
		std::string whyA, whyB;
		for (int shot = 0; shot < 12; ++shot)
			for (int i = 0; i < 8; ++i)
			{
				const std::vector<double> f = Upscaled(50 + shot * 13 + i, shot % 2 ? 0.55 : 0.35);
				Analyse(a, f, t);
				const double fa = a.AutoFactor(FH, 0.8, whyA, t, ++seq);
				if (b)
				{
					Analyse(*b, f, t);
					const double fb = b->AutoFactor(FH, 0.8, whyB, t, seq);
					Assert::AreEqual(fa, fb, 1e-12, L"the same factor while the shots run");
					Assert::AreEqual(whyA, whyB);
				}
				t += 0.5;
				if (whyA == "waiting for the next cut or black") return t;
			}
		return -1.0;
	}

	// A frame ahead to the 10-bit monitor (plain sampler, codes 0..1023) and to the 12-bit one (CodedLumaSampler{4 * code, 4095}).
	void AheadBoth(Monitor& plain, Monitor& wide, uint64_t seq, const std::vector<double>& f)
	{
		const Monitor::LumaSampler read10 = [&f](int x, int y) { return Code10(f[static_cast<size_t>(y) * FW + x]); };
		plain.LookAheadFrame(seq, FW, FH, read10, 0, 0, FW, FH);
		SourceResolutionMonitor::CodedLumaSampler read12;
		read12.read = [&f](int x, int y) { return 4 * Code10(f[static_cast<size_t>(y) * FW + x]); };
		read12.codeMaximum = 4095;
		wide.LookAheadFrame(seq, FW, FH, read12, 0, 0, FW, FH);
	}

	// A flat near-black picture with a two-level texture of `amplitude` native codes (a hash of the position, so the sparse sample grid
	// of any step sees both levels).
	int Textured(int x, int y, int blackCode, int amplitude)
	{
		const uint32_t h = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u);
		return blackCode + (((h >> 5) & 1u) ? amplitude : 0);
	}

	TEST_CLASS(LookAheadAt4095Tests)
	{
	public:
		// W8: the look-ahead fed a picture at 12 bits (code = 4 x the 10-bit code, CodedLumaSampler 4095) decides what it decides on the
		// 10-bit read: engaged at an agreeing cut, cancelled at a disagreeing one. The two monitors run in lockstep over the same
		// frames. The 12-bit normalisation (4c / 4095) is not bit-equal to c / 1023, so the discrete decisions must be equal and the
		// scalar ratio close; the 10-bit monitor shows the fixture's known outcomes, so the comparison is not between two idle monitors.
		TEST_METHOD(LookAheadAt4095MatchesTenBitDecisions)
		{
			for (int scenario = 1; scenario <= 2; ++scenario)
			{
				Monitor plain, wide;
				plain.SetLatchFixes(false);
				wide.SetLatchFixes(false);
				uint64_t seq = 0;
				const double t = UntilWaiting(plain, &wide, seq);
				Assert::IsTrue(t > 0.0, L"auto reached waiting");
				if (scenario == 1)
				{
					AheadBoth(plain, wide, seq + 1, Upscaled(999, 0.35));
					AheadBoth(plain, wide, seq + 2, Upscaled(1000, 0.75));
				}
				else
				{
					AheadBoth(plain, wide, seq + 1, Upscaled(1999, 0.35));
					AheadBoth(plain, wide, seq + 2, Native(2000, 0.75));
				}
				Assert::IsTrue(WaitReady(plain), L"the 10-bit monitor analysed the frame ahead");
				Assert::IsTrue(WaitReady(wide), L"the 12-bit monitor analysed the frame ahead");
				const Monitor::AheadState a = plain.Ahead(), b = wide.Ahead();
				Assert::AreEqual<unsigned long long>(a.eventSequence, b.eventSequence, L"the same event frame");
				Assert::AreEqual<unsigned long long>(a.targetSequence, b.targetSequence, L"the same analysed frame");
				Assert::AreEqual(a.ready, b.ready);
				Assert::AreEqual(a.informative, b.informative, L"the same spectra decision");
				Assert::IsTrue(std::fabs(a.ratio - b.ratio) < 0.02, L"the same measured ratio to within the normalisation difference");
				std::string whyPlain, whyWide;
				Assert::AreEqual(1.0, plain.AutoFactor(FH, 0.8, whyPlain, t, seq + 1), 1e-9, L"not before the cut frame");
				Assert::AreEqual(1.0, wide.AutoFactor(FH, 0.8, whyWide, t, seq + 1), 1e-9);
				const double fPlain = plain.AutoFactor(FH, 0.8, whyPlain, t + 0.04, seq + 2);
				const double fWide = wide.AutoFactor(FH, 0.8, whyWide, t + 0.04, seq + 2);
				Assert::AreEqual(fPlain, fWide, 1e-12, L"the same decision at the cut frame");
				Assert::AreEqual(whyPlain, whyWide, L"the same reason");
				const Monitor::AheadState a2 = plain.Ahead(), b2 = wide.Ahead();
				Assert::AreEqual<unsigned long long>(a2.verified, b2.verified);
				Assert::AreEqual<unsigned long long>(a2.cancelled, b2.cancelled);
				if (scenario == 1)
				{
					Assert::AreEqual(0.5, fPlain, 1e-9, L"engaged on the cut frame");
					Assert::AreEqual<unsigned long long>(1ull, a2.verified);
				}
				else
				{
					Assert::AreEqual(1.0, fPlain, 1e-9, L"stays native");
					Assert::AreEqual<unsigned long long>(1ull, a2.cancelled);
				}
			}
		}

		// W8: the blank threshold is an axis-level fraction (3 / kAxisMax), whatever the sampler's code range. A flat near-black frame
		// whose texture is ONE native code (a quarter of an axis code at 12 bits, one axis code at 10) is blank at both depths, so the
		// look-ahead flags it as an event; a texture of 16 axis codes is not blank at either (the control: the threshold still bites).
		TEST_METHOD(OneNativeCodeTextureDoesNotChangeBlank)
		{
			struct Case { bool wide; int amplitude; bool blank; const wchar_t* name; };
			const Case cases[] = {
				{ true, 1, true, L"12-bit, one native code" },
				{ false, 1, true, L"10-bit, one native code" },
				{ true, 16, true, L"12-bit, four axis codes (spread about 2 axis codes) is still blank" },
				{ true, 64, false, L"12-bit, sixteen axis codes" },
				{ false, 16, false, L"10-bit, sixteen axis codes" },
			};
			for (const Case& c : cases)
			{
				Monitor m;
				m.SetLatchFixes(false);
				uint64_t seq = 0;
				Assert::IsTrue(UntilWaiting(m, nullptr, seq) > 0.0, c.name);
				const int black = c.wide ? 256 : 64;
				const int amplitude = c.amplitude;
				if (c.wide)
				{
					SourceResolutionMonitor::CodedLumaSampler s;
					s.read = [black, amplitude](int x, int y) { return Textured(x, y, black, amplitude); };
					s.codeMaximum = 4095;
					m.LookAheadFrame(seq + 1, FW, FH, s, 0, 0, FW, FH);
				}
				else
				{
					const Monitor::LumaSampler s = [black, amplitude](int x, int y) { return Textured(x, y, black, amplitude); };
					m.LookAheadFrame(seq + 1, FW, FH, s, 0, 0, FW, FH);
				}
				Assert::AreEqual<unsigned long long>(c.blank ? seq + 1 : 0ull, m.Ahead().eventSequence, c.name);
			}
		}

		// W8 (share): after a Host Process at S, the walk's lookup finds the frame's kept 12-bit decode and its luma is the view the
		// tap delivered; the walk reads that decode (the ramp's known pixel codes) and notes no private row of its own.
		TEST_METHOD(WalkReadsTheSharedDecodeAfterProcess)
		{
			ComputeOnceGuard guard;
			const uint64_t S = 3000000000ull;
			Ramp ramp;
			VpFrameAnalysis::Host host;
			SourceFrameView delivered;
			bool got = false;
			std::vector<uint16_t> deliveredLuma;
			Tap::Register(&host, [&](const SourceFrameView& v) {
				got = true;
				delivered = v;
				for (uint32_t y = 0; y < v.height; ++y)
					for (uint32_t x = 0; x < v.width; ++x)
						deliveredLuma.push_back(v.luma[static_cast<size_t>(y) * v.strideSamples + x]);
			});
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			AnalysisLumaSource source = ramp.source;
			VpFrameAnalysis::Host::AttachIfEnabled(source);
			host.OnArrival(S, source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::IsTrue(got, L"the tap delivered the frame");
			Assert::AreEqual<int>(4, delivered.msbShift);

			SourceFrameView shared;
			const bool found = VpFrameAnalysis::FindSharedViewOf12(S, source, shared);
			bool sameLuma = found && shared.width == delivered.width && shared.height == delivered.height;
			for (uint32_t y = 0; sameLuma && y < shared.height; ++y)
				for (uint32_t x = 0; sameLuma && x < shared.width; ++x)
					sameLuma = shared.luma[static_cast<size_t>(y) * shared.strideSamples + x] == deliveredLuma[static_cast<size_t>(y) * shared.width + x];
			Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(S + 1, source, delivered), L"another frame is not the kept one");

			FakeImpl impl;
			Seen seen;
			impl.vpv.sourceAnalysis.sourceMonitor.seen = &seen;
			Walk(impl, source, S);
			uint32_t frames = 0;
			uint64_t calls = 0;
			const int privateRows = PrivateAheadRows(frames, calls);
			Tap::Unregister(&host);
			host.Configure(false);

			Assert::IsTrue(found, L"FindSharedViewOf12 finds the frame Process kept");
			Assert::IsTrue(sameLuma, L"the kept decode is the delivered view");
			Assert::AreEqual<size_t>(1, seen.sequences.size(), L"the walk looked at the frame once");
			Assert::AreEqual<uint64_t>(S, seen.sequences[0]);
			Assert::AreEqual<uint32_t>(4095u, seen.codeMaximums[0], L"a DV frame is read at the 12-bit range");
			const int expected[5] = { 0, 1, 64, 63, 4095 };   // pixel (x, y) carries code y * 64 + x
			for (int i = 0; i < 5; ++i)
				Assert::AreEqual(expected[i], seen.samples[static_cast<size_t>(i)], L"the walk reads the 12-bit code");
			Assert::AreEqual(0, privateRows, L"no private decode of its own while the shared one is held");
		}

		// W8 (no share): without a Process the walk reads its own samples through the same law, and notes ONE private row for the frame,
		// once per visit (not per sample): visiting the frame twice is two calls of one frame.
		TEST_METHOD(WalkWithoutProcessNotesOneOwnSampleRowPerVisit)
		{
			ComputeOnceGuard guard;
			const uint64_t S = 3100000000ull;
			Ramp ramp;
			AnalysisLumaSource source = ramp.source;
			SourceFrameView none;
			Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(S, source, none), L"nothing kept for this frame");
			FakeImpl impl;
			Seen seen;
			impl.vpv.sourceAnalysis.sourceMonitor.seen = &seen;
			Walk(impl, source, S);
			Walk(impl, source, S);
			uint32_t frames = 0;
			uint64_t calls = 0;
			const int rows = PrivateAheadRows(frames, calls);
			Assert::AreEqual(1, rows, L"one private row");
			Assert::AreEqual<uint32_t>(1u, frames, L"for one frame");
			Assert::AreEqual<uint64_t>(2u, calls, L"counted once per visit, not once per sample (five samples a visit)");
			Assert::AreEqual<size_t>(2, seen.sequences.size());
			const int expected[5] = { 0, 1, 64, 63, 4095 };
			for (size_t visit = 0; visit < 2; ++visit)
			{
				Assert::AreEqual<uint32_t>(4095u, seen.codeMaximums[visit]);
				for (int i = 0; i < 5; ++i)
					Assert::AreEqual(expected[i], seen.samples[visit * 5 + static_cast<size_t>(i)], L"its own samples carry the same 12-bit codes");
			}
			Assert::AreEqual(0, seen.plainCalls);
		}
	};
}
