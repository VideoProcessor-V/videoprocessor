// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for input.light_level (LightLevelSettings.h, LightLevelLut.h, LightLevelGlsl.h). CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelGlsl.h>
#include "LightLevelGolden.generated.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using KeyMap = std::map<std::string, std::string>;

		LightLevel::Settings ReadMap(const KeyMap& m)
		{
			return LightLevel::Read([&m](const std::string& key, std::string& raw)
			{
				const auto it = m.find(key);
				if (it == m.end()) return false;
				raw = it->second;
				return true;
			});
		}

		LightLevel::Context Ctx(LightLevel::Context::Kind kind, bool fullRange = false, double sourcePeak = 0.0,
			double displayPeak = 0.0, double dynamicPeak = 0.0)
		{
			LightLevel::Context c;
			c.kind = kind;
			c.fullRange = fullRange;
			c.sourcePeakNits = sourcePeak;
			c.displayPeakNits = displayPeak;
			c.dynamicPeakNits = dynamicPeak;
			return c;
		}

		// Independent BT.1886 closed form (ITU-R BT.1886 annex 1), signal v in 0..1.
		double Bt1886(double v, double white, double black)
		{
			const double lw = std::pow(white, 1.0 / 2.4), lb = std::pow(black, 1.0 / 2.4);
			return std::pow(lw - lb, 2.4) * std::pow(std::max(v + lb / (lw - lb), 0.0), 2.4);
		}

		double LimitedSignal(double code) { return (code - 64.0) / 876.0; }

		bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

		bool AnyNoteContains(const LightLevel::Settings& s, const char* needle)
		{
			for (const auto& n : s.notes) if (Contains(n, needle)) return true;
			return false;
		}

		bool Close(double expected, double actual)
		{
			return std::fabs(expected - actual) <= 1e-9 * (1.0 + std::fabs(expected));
		}
	}

	TEST_CLASS(LightLevelLutTests)
	{
	public:

		// (1) HdrToSdr against tools/eval/tonemap_ref.py (generated golden), 3 peaks x all codes.
		TEST_METHOD(LightLevelHdrToSdrMatchesTonemapRefGolden)
		{
			const KeyMap keys = { {"sdr_target_nits", "100"}, {"sdr_black_nits", "0"}, {"peak_detection", "off"} };
			const LightLevel::Settings s = ReadMap(keys);
			Assert::AreEqual(100.0, s.sdrTargetNits, 0.0);
			Assert::AreEqual(0.0, s.sdrBlackNits, 0.0);
			Assert::IsTrue(s.notes.empty());
			const auto tone = LightLevel::detail::Evaluator::Resolve(s);

			double maxSdrError = 0.0, maxPqError = 0.0;
			for (int p = 0; p < LightLevelGolden::kPeakCount; ++p)
			{
				const double peak = LightLevelGolden::kPeaks[p];
				const LightLevel::Lut lut = LightLevel::Build(s, Ctx(LightLevel::Context::HdrToSdr, false, peak));
				Assert::IsTrue(lut.exactness == LightLevel::Exactness::Exact);
				const LightLevel::detail::Spline spline(peak, LightLevelGolden::kDstPeak, 0.0, 0.0, tone);
				for (int code = 64; code < LightLevel::kLutSize; ++code)   // below 64 is BTB (test 2)
				{
					const std::size_t i = static_cast<std::size_t>(code);
					const double sdrErr = std::fabs(lut.e[i].sdrCode - LightLevelGolden::kSdr[p][code]);
					const double v = std::min(std::max(LimitedSignal(code), 0.0), 1.0);
					const double pqErr = std::fabs(spline(v) - LightLevelGolden::kPq[p][code]);
					maxSdrError = std::max(maxSdrError, sdrErr);
					maxPqError = std::max(maxPqError, pqErr);
					Assert::IsTrue(sdrErr <= 0.5, L"sdr code within half a code of the golden");
				}
			}
			char message[160];
			std::snprintf(message, sizeof(message), "light level golden: max SDR code error %.3e, max PQ error %.3e",
				maxSdrError, maxPqError);
			Logger::WriteMessage(message);
			Assert::IsTrue(maxSdrError <= 1e-6, L"tight bound on the SDR code");
			Assert::IsTrue(maxPqError <= 1e-6, L"tight bound on the PQ output");
		}

		// (2) BTB codes are a lookup: J = S = V = 0, L = display black; Fetch clamps only the index below 0.
		TEST_METHOD(LightLevelBtbCodesGiveZeroJAndFetchClampsTheIndexOnly)
		{
			const KeyMap keys = { {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"} };
			const LightLevel::Settings s = ReadMap(keys);
			for (int kind = 0; kind < 3; ++kind)
			{
				const LightLevel::Context c = Ctx(static_cast<LightLevel::Context::Kind>(kind), false, 1000.0, 600.0);
				const LightLevel::Lut lut = LightLevel::Build(s, c);
				for (std::size_t code = 0; code < 64; ++code)
				{
					Assert::AreEqual(0.0, lut.e[code].J, 0.0, L"BTB J");
					Assert::AreEqual(0.0, lut.e[code].S, 0.0, L"BTB S");
					Assert::AreEqual(0.0, lut.e[code].V, 0.0, L"BTB V");
					Assert::AreEqual(0.05, lut.e[code].L, 1e-12, L"BTB L is the display black");
				}
				// Below 0 only the index clamps.
				for (double code : { -1.0, -5.0, -1e9 })
				{
					const LightLevel::Entry f = LightLevel::Fetch(lut, code);
					Assert::AreEqual(lut.e[0].L, f.L, 0.0);
					Assert::AreEqual(lut.e[0].J, f.J, 0.0);
				}
				// Above 1023 too; between codes it still interpolates (63.5 crosses the BTB edge).
				const LightLevel::Entry top = LightLevel::Fetch(lut, 5000.0);
				Assert::AreEqual(lut.e[1023].L, top.L, 0.0);
				Assert::AreEqual(lut.e[1023].J, top.J, 0.0);
				const LightLevel::Entry edge = LightLevel::Fetch(lut, 63.5);
				Assert::AreEqual(0.5 * lut.e[64].J, edge.J, 1e-12, L"interpolates across the BTB edge, not clamped");
			}
		}

		// (3) Determinism: identical inputs give byte-identical texels and the same fingerprint.
		TEST_METHOD(LightLevelBuildIsDeterministic)
		{
			const KeyMap keys = { {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"}, {"light_level_ambient_nits", "0.1"} };
			const LightLevel::Settings s1 = ReadMap(keys), s2 = ReadMap(keys);
			for (int kind = 0; kind < 3; ++kind)
			{
				const LightLevel::Context c = Ctx(static_cast<LightLevel::Context::Kind>(kind), false, 1000.0, 600.0);
				const LightLevel::Lut a = LightLevel::Build(s1, c), b = LightLevel::Build(s2, c);
				const std::vector<float> ta = LightLevel::PackTexels(a), tb = LightLevel::PackTexels(b);
				Assert::AreEqual(static_cast<std::size_t>(LightLevel::kLutSize) * 4, ta.size());
				Assert::AreEqual(ta.size(), tb.size());
				Assert::AreEqual(0, std::memcmp(ta.data(), tb.data(), ta.size() * sizeof(float)), L"byte-identical texels");
				Assert::IsTrue(a.fingerprint == b.fingerprint);
				Assert::IsTrue(a.fingerprint == LightLevel::Fingerprint(s1, c));
				Assert::IsTrue(a.status == b.status);
			}
			LightLevel::Settings other = s1;
			other.sdrTargetNits = 100.0;
			const LightLevel::Context c = Ctx(LightLevel::Context::Sdr);
			Assert::IsTrue(LightLevel::Fingerprint(s1, c) != LightLevel::Fingerprint(other, c));
		}

		// (4) No hard-coded nits: white, black, ambient and Weber all come from the configuration.
		TEST_METHOD(LightLevelNitsAreConfigDriven)
		{
			struct Set { const char* white; const char* black; const char* ambient; const char* weber; double w, b, a, k; };
			const Set sets[] = {
				{ "203", "0.05", "0.1", "0.02", 203.0, 0.05, 0.1, 0.02 },
				{ "150", "0.2", "0", "0.05", 150.0, 0.2, 0.0, 0.05 },
			};
			for (const Set& set : sets)
			{
				const KeyMap keys = { {"sdr_target_nits", set.white}, {"sdr_black_nits", set.black},
					{"light_level_ambient_nits", set.ambient}, {"light_level_weber", set.weber} };
				const LightLevel::Settings s = ReadMap(keys);
				Assert::IsTrue(s.notes.empty());
				const LightLevel::Lut lut = LightLevel::Build(s, Ctx(LightLevel::Context::Sdr));
				Assert::AreEqual(set.w, lut.e[940].L, 1e-9, L"L at reference white = sdr_target_nits");
				Assert::AreEqual(set.b, lut.e[64].L, 1e-9, L"L at black = sdr_black_nits");
				Assert::AreEqual(set.w, lut.e[1023].L, 1e-9, L"above-white is capped at white");
				Assert::AreEqual(LightLevel::pq::Oetf(set.w), lut.e[940].J, 1e-12);
				// V at code 64: one-sided step to 64.5 (63.5 is BTB, which reads as black), floor = black + ambient.
				const double dL = Bt1886(LimitedSignal(64.5), set.w, set.b) - set.b;
				const double expected = dL / (set.k * (set.b + (set.b + set.a)));
				Assert::AreEqual(expected, lut.e[64].V, 1e-9 * std::max(1.0, expected), L"V uses weber and the black+ambient floor");
				// Mid code against the closed form.
				Assert::AreEqual(Bt1886(LimitedSignal(500.0), set.w, set.b), lut.e[500].L, 1e-9);
			}
			// HDR to SDR: the tone-mapped peak lands on the configured white.
			const KeyMap keys = { {"sdr_target_nits", "150"}, {"sdr_black_nits", "0.2"} };
			const LightLevel::Lut hdr = LightLevel::Build(ReadMap(keys), Ctx(LightLevel::Context::HdrToSdr, false, 4000.0));
			Assert::AreEqual(150.0, hdr.e[940].L, 1e-6);
		}

		// (5) J is monotonic per kind.
		TEST_METHOD(LightLevelJIsMonotonicPerKind)
		{
			const LightLevel::Settings s = ReadMap({ {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"} });
			struct Case { LightLevel::Context ctx; int first; bool strictBetween; };
			const Case cases[] = {
				{ Ctx(LightLevel::Context::Sdr), 64, true },
				{ Ctx(LightLevel::Context::Sdr, true), 0, true },
				{ Ctx(LightLevel::Context::HdrPassthrough, false, 1000.0, 600.0), 64, false },
				{ Ctx(LightLevel::Context::HdrPassthrough, true, 1000.0), 0, false },
				{ Ctx(LightLevel::Context::HdrToSdr, false, 1000.0), 64, false },
				{ Ctx(LightLevel::Context::HdrToSdr, false, 10000.0), 64, false },
			};
			for (const Case& c : cases)
			{
				const LightLevel::Lut lut = LightLevel::Build(s, c.ctx);
				for (std::size_t i = static_cast<std::size_t>(c.first) + 1; i < static_cast<std::size_t>(LightLevel::kLutSize); ++i)
				{
					Assert::IsTrue(lut.e[i].J >= lut.e[i - 1].J - 1e-12, L"J non-decreasing");
					Assert::IsTrue(lut.e[i].L >= lut.e[i - 1].L - 1e-12, L"L non-decreasing");
				}
				Assert::IsTrue(lut.e[1023].J > lut.e[static_cast<std::size_t>(c.first)].J);
				if (c.strictBetween)
				{
					const std::size_t last = c.ctx.fullRange ? 1023 : 940;
					for (std::size_t i = static_cast<std::size_t>(c.first) + 1; i <= last; ++i)
						Assert::IsTrue(lut.e[i].J > lut.e[i - 1].J, L"J strictly increasing up to white");
				}
			}
		}

		// (6) Exactness and status: peak detection, unmodelled tone mapping, invalid values.
		TEST_METHOD(LightLevelExactnessStatusAndNotes)
		{
			const LightLevel::Context fixed = Ctx(LightLevel::Context::HdrToSdr, false, 1000.0);
			const LightLevel::Context dynamic = Ctx(LightLevel::Context::HdrToSdr, false, 1000.0, 0.0, 800.0);

			const LightLevel::Lut exact = LightLevel::Build(ReadMap({ {"peak_detection", "off"} }), fixed);
			Assert::IsTrue(exact.exactness == LightLevel::Exactness::Exact);
			Assert::IsTrue(Contains(exact.status, "exact"));

			// auto follows the quality preset: on for high / balanced, off for fast (LightLevel::PeakDetectionOn).
			Assert::IsTrue(LightLevel::PeakDetectionOn(ReadMap({})));
			Assert::IsTrue(LightLevel::PeakDetectionOn(ReadMap({ {"quality", "balanced"} })));
			Assert::IsFalse(LightLevel::PeakDetectionOn(ReadMap({ {"quality", "fast"} })));
			Assert::IsTrue(LightLevel::PeakDetectionOn(ReadMap({ {"quality", "fast"}, {"peak_detection", "on"} })));
			Assert::IsTrue(LightLevel::Build(ReadMap({ {"quality", "fast"} }), fixed).exactness == LightLevel::Exactness::Exact);

			for (const char* mode : { "on", "auto", "default", "high_quality" })
			{
				const LightLevel::Settings ps = ReadMap({ {"peak_detection", mode} });
				// With a VP peak estimate.
				const LightLevel::Lut lut = LightLevel::Build(ps, dynamic);
				Assert::IsTrue(lut.exactness == LightLevel::Exactness::Approx, L"peak detection on -> Approx");
				Assert::IsTrue(Contains(lut.status, "approx (dynamic tone mapping)"), L"status names the dynamic tone mapping");
				Assert::IsFalse(Contains(lut.status, "no VP peak estimate"));
				// Without one: still Approx, and the status says the curve is static.
				const LightLevel::Lut none = LightLevel::Build(ps, fixed);
				Assert::IsTrue(none.exactness == LightLevel::Exactness::Approx, L"peak detection on, no estimate -> Approx");
				Assert::IsTrue(Contains(none.status, "approx (dynamic tone mapping; static curve, no VP peak estimate)"));
			}
			const LightLevel::Lut off = LightLevel::Build(ReadMap({ {"peak_detection", "off"} }), dynamic);
			Assert::IsTrue(off.exactness == LightLevel::Exactness::Exact);
			Assert::IsTrue(off.fingerprint != exact.fingerprint, L"the measured peak is part of the context");

			// Every reason is named, joined with "; ".
			const LightLevel::Lut both = LightLevel::Build(ReadMap({ {"tone_mapping", "bt2390"}, {"peak_detection", "on"} }), dynamic);
			Assert::IsTrue(both.exactness == LightLevel::Exactness::Approx);
			Assert::IsTrue(Contains(both.status, "tone_mapping bt2390 not modelled; spline used; dynamic tone mapping)"));

			const LightLevel::Settings bt = ReadMap({ {"tone_mapping", "bt2390"}, {"peak_detection", "off"} });
			Assert::IsTrue(bt.notes.empty());
			const LightLevel::Lut unmodelled = LightLevel::Build(bt, fixed);
			Assert::IsTrue(unmodelled.exactness == LightLevel::Exactness::Approx, L"unmodelled tone_mapping -> Approx");
			Assert::IsTrue(Contains(unmodelled.status, "bt2390"));
			Assert::IsTrue(Contains(unmodelled.status, "not modelled"));
			Assert::IsTrue(LightLevel::Build(ReadMap({ {"tone_mapping", "spline"}, {"peak_detection", "off"} }), fixed).exactness == LightLevel::Exactness::Exact);

			// Invalid values: a note each, the default stays.
			const LightLevel::Settings bad = ReadMap({ {"tone_mapping", "banana"}, {"sdr_target_nits", "abc"},
				{"light_level_weber", "5"}, {"peak_detection", "maybe"}, {"hdr_tone_map_target_gamma", "9"},
				{"sdr_black_nits", "500"}, {"knee_default", "oops"} });
			Assert::IsTrue(AnyNoteContains(bad, "tone_mapping"));
			Assert::IsTrue(AnyNoteContains(bad, "sdr_target_nits"));
			Assert::IsTrue(AnyNoteContains(bad, "light_level_weber"));
			Assert::IsTrue(AnyNoteContains(bad, "peak_detection"));
			Assert::IsTrue(AnyNoteContains(bad, "hdr_tone_map_target_gamma"));
			Assert::IsTrue(AnyNoteContains(bad, "sdr_black_nits"));
			Assert::IsTrue(AnyNoteContains(bad, "knee_default"));
			Assert::AreEqual(203.0, bad.sdrTargetNits, 0.0);
			Assert::AreEqual(0.02, bad.weber, 0.0);
			Assert::AreEqual(0.0, bad.sdrBlackNits, 0.0);
			Assert::IsTrue(bad.toneMapping == "auto");
			Assert::IsTrue(ReadMap({}).notes.empty());

			// HDR passthrough is always an approximation of the display's own roll-off.
			const LightLevel::Lut pass = LightLevel::Build(ReadMap({}), Ctx(LightLevel::Context::HdrPassthrough, false, 1000.0, 600.0));
			Assert::IsTrue(pass.exactness == LightLevel::Exactness::Approx);
			Assert::IsFalse(pass.status.empty());
		}

		// (6b) HdrToSdr display model: libplacebo encodes L_out with the inverse of the target transfer (incl. black), so
		// with the display on auto the viewer sees L_out; a physical white of 2x the target doubles L.
		TEST_METHOD(LightLevelHdrToSdrDisplayModel)
		{
			const KeyMap keys = { {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"}, {"peak_detection", "off"} };
			const LightLevel::Settings s = ReadMap(keys);
			Assert::IsTrue(s.notes.empty());
			Assert::AreEqual(203.0, s.displayWhiteNits, 0.0);
			Assert::AreEqual(0.05, s.displayBlackNits, 0.0);
			const auto tone = LightLevel::detail::Evaluator::Resolve(s);
			const double peak = 1000.0;
			const LightLevel::detail::Spline spline(peak, 203.0, 0.0, 0.05, tone);
			const LightLevel::Lut lut = LightLevel::Build(s, Ctx(LightLevel::Context::HdrToSdr, false, peak));
			for (int code = 64; code < LightLevel::kLutSize; ++code)
			{
				const double v = std::min(std::max(LimitedSignal(code), 0.0), 1.0);
				const double out = std::min(std::max(LightLevel::pq::Eotf(spline(v)), 0.05), 203.0);
				Assert::IsTrue(Close(out, lut.e[static_cast<std::size_t>(code)].L), L"L == L_out with display auto");
			}

			// Physical display white 2x the target: black 0 BT.1886, mid code.
			const KeyMap base = { {"sdr_target_nits", "100"}, {"sdr_black_nits", "0"}, {"peak_detection", "off"} };
			KeyMap doubled = base;
			doubled["light_level_display_white_nits"] = "200";
			const LightLevel::Settings sb = ReadMap(base), sd = ReadMap(doubled);
			Assert::IsTrue(sd.notes.empty());
			Assert::AreEqual(200.0, sd.displayWhiteNits, 0.0);
			Assert::IsTrue(LightLevel::Fingerprint(sb, Ctx(LightLevel::Context::Sdr)) != LightLevel::Fingerprint(sd, Ctx(LightLevel::Context::Sdr)));
			const LightLevel::Lut sdrA = LightLevel::Build(sb, Ctx(LightLevel::Context::Sdr));
			const LightLevel::Lut sdrB = LightLevel::Build(sd, Ctx(LightLevel::Context::Sdr));
			Assert::IsTrue(Close(Bt1886(LimitedSignal(500.0), 200.0, 0.0), sdrB.e[500].L), L"Sdr shown on the physical display");
			Assert::IsTrue(Close(2.0 * sdrA.e[500].L, sdrB.e[500].L));
			const LightLevel::Lut hdrA = LightLevel::Build(sb, Ctx(LightLevel::Context::HdrToSdr, false, 1000.0));
			const LightLevel::Lut hdrB = LightLevel::Build(sd, Ctx(LightLevel::Context::HdrToSdr, false, 1000.0));
			Assert::IsTrue(Close(2.0 * hdrA.e[500].L, hdrB.e[500].L), L"HdrToSdr L scales with the physical white");
			Assert::AreEqual(hdrA.e[500].sdrCode, hdrB.e[500].sdrCode, 1e-12, L"the encoded signal does not depend on the display");

			// Invalid or white <= black: a note and the auto values.
			const LightLevel::Settings bad = ReadMap({ {"light_level_display_white_nits", "0.5"}, {"light_level_display_black_nits", "2"} });
			Assert::IsTrue(AnyNoteContains(bad, "light_level_display_white_nits"));
			Assert::AreEqual(203.0, bad.displayWhiteNits, 0.0);   // 0.5 is below the 1 nit minimum: auto
			Assert::AreEqual(2.0, bad.displayBlackNits, 0.0);     // valid on its own
			const LightLevel::Settings crossed = ReadMap({ {"light_level_display_white_nits", "50"}, {"light_level_display_black_nits", "60"} });
			Assert::IsTrue(AnyNoteContains(crossed, "must exceed"));
			Assert::AreEqual(203.0, crossed.displayWhiteNits, 0.0);
			Assert::IsTrue(ReadMap({ {"light_level_display_white_nits", "auto"} }).notes.empty());
		}

		// (7) The SDR tone shape changes the LUT only above the knee start.
		TEST_METHOD(LightLevelSdrToneShapeDiffersOnlyAboveKneeStart)
		{
			const LightLevel::Settings off = ReadMap({ {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"} });
			const LightLevel::Settings on = ReadMap({ {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"},
				{"sdr_tone_shape", "knee"}, {"sdr_knee_start", "0.8"}, {"sdr_knee_strength", "0.5"} });
			Assert::IsTrue(on.notes.empty());
			const LightLevel::Context c = Ctx(LightLevel::Context::Sdr);
			const LightLevel::Lut a = LightLevel::Build(off, c), b = LightLevel::Build(on, c);
			Assert::IsTrue(a.fingerprint != b.fingerprint);
			// The knee start is a fraction of peak in LINEAR light: signal = start ^ (1 / 2.4).
			const int startCode = static_cast<int>(64.0 + 876.0 * std::pow(0.8, 1.0 / 2.4));
			bool anyAbove = false;
			for (int code = 0; code < LightLevel::kLutSize; ++code)
			{
				const std::size_t i = static_cast<std::size_t>(code);
				const double rel = std::fabs(a.e[i].L - b.e[i].L) / std::max(a.e[i].L, 1e-9);
				if (code <= startCode - 2)
					Assert::IsTrue(rel <= 1e-9, L"identical below the knee start");
				else if (code >= startCode + 8 && code <= 940)
				{
					Assert::IsTrue(rel > 1e-5, L"differs above the knee start");
					anyAbove = true;
				}
			}
			Assert::IsTrue(anyAbove);
		}

		// (8) The cache rebuilds only when the fingerprint changes.
		TEST_METHOD(LightLevelCacheRebuildsOnlyOnChange)
		{
			LightLevel::LutCache cache;
			LightLevel::Settings s = ReadMap({ {"sdr_target_nits", "203"} });
			LightLevel::Context c = Ctx(LightLevel::Context::HdrToSdr, false, 1000.0);
			const LightLevel::Lut* first = &cache.Get(s, c);
			Assert::IsTrue(cache.LastRebuilt(), L"first Get builds");
			const std::string fp = first->fingerprint;
			Assert::IsTrue(&cache.Get(s, c) == first, L"same Lut object");
			Assert::IsFalse(cache.LastRebuilt(), L"same inputs: no rebuild");
			Assert::IsTrue(cache.Get(s, c).fingerprint == fp);
			Assert::IsFalse(cache.LastRebuilt());

			s.sdrTargetNits = 150.0;
			cache.Get(s, c);
			Assert::IsTrue(cache.LastRebuilt(), L"settings change rebuilds");
			cache.Get(s, c);
			Assert::IsFalse(cache.LastRebuilt());

			c.sourcePeakNits = 4000.0;
			cache.Get(s, c);
			Assert::IsTrue(cache.LastRebuilt(), L"context change rebuilds");
			c.dynamicPeakNits = 700.0;
			cache.Get(s, c);
			Assert::IsTrue(cache.LastRebuilt(), L"measured peak change rebuilds");
			cache.Get(s, c);
			Assert::IsFalse(cache.LastRebuilt());

			cache.Reset();
			cache.Get(s, c);
			Assert::IsTrue(cache.LastRebuilt(), L"Reset forces a rebuild");
		}

		// (9) Fetch: exact at integer codes, midpoint at half codes.
		TEST_METHOD(LightLevelFetchInterpolatesLinearly)
		{
			const LightLevel::Settings s = ReadMap({ {"sdr_target_nits", "203"}, {"sdr_black_nits", "0.05"} });
			const LightLevel::Lut lut = LightLevel::Build(s, Ctx(LightLevel::Context::HdrToSdr, false, 1000.0));
			for (int i = 0; i < LightLevel::kLutSize; ++i)
			{
				const LightLevel::Entry& e = lut.e[static_cast<std::size_t>(i)];
				const LightLevel::Entry f = LightLevel::Fetch(lut, static_cast<double>(i));
				Assert::AreEqual(e.L, f.L, 0.0);
				Assert::AreEqual(e.J, f.J, 0.0);
				Assert::AreEqual(e.S, f.S, 0.0);
				Assert::AreEqual(e.V, f.V, 0.0);
				Assert::AreEqual(e.sdrCode, f.sdrCode, 0.0);
			}
			for (int i = 0; i + 1 < LightLevel::kLutSize; ++i)
			{
				const LightLevel::Entry f = LightLevel::Fetch(lut, i + 0.5);
				const LightLevel::Entry& a = lut.e[static_cast<std::size_t>(i)];
				const LightLevel::Entry& b = lut.e[static_cast<std::size_t>(i) + 1];
				Assert::IsTrue(Close(0.5 * (a.L + b.L), f.L), L"L midpoint");
				Assert::IsTrue(Close(0.5 * (a.J + b.J), f.J), L"J midpoint");
				Assert::IsTrue(Close(0.5 * (a.S + b.S), f.S), L"S midpoint");
				Assert::IsTrue(Close(0.5 * (a.V + b.V), f.V), L"V midpoint");
				Assert::IsTrue(Close(0.5 * (a.sdrCode + b.sdrCode), f.sdrCode), L"sdrCode midpoint");
			}
		}

		// (10) The GLSL names what the host and the shaders bind.
		TEST_METHOD(LightLevelGlslDeclaresTheFetchAndJFunctions)
		{
			const std::string glsl = LightLevel::LightLevelGlsl();
			Assert::IsTrue(Contains(glsl, "light_level_lut_fetch"));
			Assert::IsTrue(Contains(glsl, "light_level_j"));
			Assert::IsTrue(Contains(glsl, "light_level_lut"));
			Assert::IsTrue(Contains(glsl, "texelFetch"));
		}
	};
}
