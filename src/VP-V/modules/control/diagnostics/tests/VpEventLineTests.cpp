// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/control/diagnostics/VpEventLine.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// vp-event/1 (VPEV1) structured log lines: formatting goldens (a Python parser is written against
// these exact strings), the limiter, the process-wide sink, and the two emitters
// (ResyncShadow::Record and FallbackStatus::Registry). CPU only.
namespace
{
	std::mutex g_mutex;
	std::vector<std::string> g_lines;

	void CaptureSink(const char* line)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lines.push_back(line ? line : "");
	}

	// Installs the capturing sink and ALWAYS resets it to null at scope exit.
	struct SinkScope
	{
		SinkScope()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_lines.clear();
			VpEvent::SetSink(CaptureSink);
		}
		~SinkScope() { VpEvent::SetSink(nullptr); }
		static std::vector<std::string> Lines()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return g_lines;
		}
	};

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	std::vector<std::string> OnlyEvents(const std::vector<std::string>& lines)
	{
		std::vector<std::string> out;
		for (const std::string& l : lines)
			if (l.compare(0, 5, "VPEV1") == 0)
				out.push_back(l);
		return out;
	}
}

namespace Tests
{
	TEST_CLASS(VpEventLineTests)
	{
	public:
		// ---- goldens ----
		TEST_METHOD(GoldenResyncLine)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::resync, 1234.5,
				{ { "level", "minor" }, { "cause", "missed_frames" }, { "seq", (uint64_t)42 }, { "gap", (int64_t)3 }, { "mode", "shadow" } });
			Assert::AreEqual(std::string("VPEV1 t=1234.500 kind=resync level=minor cause=missed_frames seq=42 gap=3 mode=shadow"), line);
		}

		TEST_METHOD(GoldenQuotedValue)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::flag, 0.0, { { "source", "hotkey" }, { "tag", "needs space" } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=flag source=hotkey tag=\"needs space\""), line);
		}

		TEST_METHOD(GoldenEscapedQuotesAndBackslash)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::fallback, 5.25,
				{ { "state", "engaged" }, { "id", "capture GPU unpack" }, { "reason", std::string("CPU formatters: \"x\" \\ y") } });
			Assert::AreEqual(std::string("VPEV1 t=5.250 kind=fallback state=engaged id=\"capture GPU unpack\" reason=\"CPU formatters: \\\"x\\\" \\\\ y\""), line);
		}

		TEST_METHOD(GoldenEmptyValue)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::gate, 1.0, { { "from", "a" }, { "to", "b" }, { "reason", "" } });
			Assert::AreEqual(std::string("VPEV1 t=1.000 kind=gate from=a to=b reason=\"\""), line);
		}

		TEST_METHOD(GoldenControlCharactersBecomeSpaces)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::flag, 2.0, { { "source", "s" }, { "tag", std::string("a\r\nb\tc") } });
			Assert::AreEqual(std::string("VPEV1 t=2.000 kind=flag source=s tag=\"a  b c\""), line);
		}

		TEST_METHOD(GoldenFrameStatsDoubles)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::frame_stats, 60000.125,
				{ { "window_s", 60.0 }, { "n", (int64_t)3600 }, { "p50_ms", 16.667 }, { "p99_ms", 17.25 }, { "max_ms", 33.4 } });
			Assert::AreEqual(std::string("VPEV1 t=60000.125 kind=frame_stats window_s=60.000 n=3600 p50_ms=16.667 p99_ms=17.250 max_ms=33.400"), line);
		}

		// ---- numbers ----
		TEST_METHOD(DoubleFormatting)
		{
			auto one = [](double v) { return VpEvent::Format(VpEvent::Kind::stamp, 0.0, { { "dir", "x" }, { "index", v } }); };
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=0.000"), one(0.0));
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=-1.500"), one(-1.5));
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=0.001"), one(0.0005));
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=1.235"), one(1.2349999 + 0.0000002));
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=-0.002"), one(-0.0016));
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=0.000"), one(-0.0001), L"no negative zero");
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=stamp dir=x index=2.000"), one(1.9996));
		}

		TEST_METHOD(IntegerFormatting)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::repeat, 0.0,
				{ { "cause", "c" }, { "count", (int64_t)-9223372036854775807LL - 1 } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=repeat cause=c count=-9223372036854775808"), line);
			const std::string u = VpEvent::Format(VpEvent::Kind::repeat, 0.0,
				{ { "cause", "c" }, { "count", (uint64_t)18446744073709551615ULL } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=repeat cause=c count=18446744073709551615"), u);
			const std::string i = VpEvent::Format(VpEvent::Kind::repeat, 0.0, { { "cause", "c" }, { "count", -7 } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=repeat cause=c count=-7"), i);
			const std::string n = VpEvent::Format(VpEvent::Kind::repeat, 0.0, { { "cause", "c" }, { "count", 7u } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=repeat cause=c count=7"), n);
		}

		// ---- truncation ----
		TEST_METHOD(LongLastValueIsTruncatedAndStaysValid)
		{
			const std::string longText(2500, 'x');
			const std::string line = VpEvent::Format(VpEvent::Kind::gpu_error, 3.0,
				{ { "api", "d3d11" }, { "code", "0x887A0005" }, { "what", longText } });
			Assert::IsTrue(line.size() <= 1000, W(std::to_string(line.size())).c_str());
			Assert::IsTrue(line.compare(0, 62, "VPEV1 t=3.000 kind=gpu_error api=d3d11 code=0x887A0005 what=xx") == 0);
			Assert::IsTrue(line.size() >= 8 && line.compare(line.size() - 8, 8, " trunc=1") == 0);
		}

		TEST_METHOD(LongQuotedValueTruncationKeepsQuotesBalanced)
		{
			std::string longText;
			for (int i = 0; i < 700; ++i)
				longText += "\" ";   // every char escapes or is a space: quoted, expanding
			const std::string line = VpEvent::Format(VpEvent::Kind::flag, 0.0, { { "source", "s" }, { "tag", longText } });
			Assert::IsTrue(line.size() <= 1000);
			const std::string suffix = "\" trunc=1";
			Assert::IsTrue(line.size() > suffix.size() && line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0);
			// Quotes: every unescaped quote is a delimiter; there must be exactly two.
			int delimiters = 0;
			bool escaped = false;
			for (char c : line)
			{
				if (escaped) { escaped = false; continue; }
				if (c == '\\') { escaped = true; continue; }
				if (c == '"') ++delimiters;
			}
			Assert::AreEqual(2, delimiters);
		}

		TEST_METHOD(ShortLineIsNotMarkedTruncated)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::flag, 0.0, { { "source", "s" }, { "tag", "t" } });
			Assert::IsTrue(line.find("trunc") == std::string::npos);
		}

		// ---- kinds ----
		TEST_METHOD(EveryKindTokenRoundTrips)
		{
			const VpEvent::Kind all[] = {
				VpEvent::Kind::session, VpEvent::Kind::resync, VpEvent::Kind::fallback, VpEvent::Kind::gate,
				VpEvent::Kind::gpu_error, VpEvent::Kind::source_change, VpEvent::Kind::cadence, VpEvent::Kind::repeat,
				VpEvent::Kind::frame_stats, VpEvent::Kind::stamp, VpEvent::Kind::flag };
			Assert::AreEqual(VpEvent::kKindCount, sizeof(all) / sizeof(all[0]));
			for (VpEvent::Kind k : all)
			{
				VpEvent::Kind back = VpEvent::Kind::session;
				Assert::IsTrue(VpEvent::KindFromToken(VpEvent::KindToken(k), back));
				Assert::IsTrue(back == k);
			}
			VpEvent::Kind unused = VpEvent::Kind::gate;
			Assert::IsFalse(VpEvent::KindFromToken("nope", unused));
			Assert::IsFalse(VpEvent::KindFromToken("", unused));
			Assert::IsFalse(VpEvent::KindFromToken("resyncx", unused));
			Assert::IsFalse(VpEvent::KindFromToken(nullptr, unused));
			Assert::IsTrue(unused == VpEvent::Kind::gate, L"a failed parse leaves the output alone");
			Assert::AreEqual("source_change", VpEvent::KindToken(VpEvent::Kind::source_change));
		}

		TEST_METHOD(KeysAreForcedToTheKeyAlphabet)
		{
			const std::string line = VpEvent::Format(VpEvent::Kind::flag, 0.0, { { "Bad Key!", "v" } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=flag bad_key_=v"), line);
		}

		// ---- limiter ----
		TEST_METHOD(LimiterSuppressesAndReportsTheCount)
		{
			VpEvent::Limiter limiter(3);
			uint32_t suppressed = 99;
			for (int i = 0; i < 3; ++i)
			{
				Assert::IsTrue(limiter.Allow(VpEvent::Kind::gate, 100.0 + i, suppressed));
				Assert::AreEqual(0u, suppressed);
			}
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::gate, 500.0, suppressed));
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::gate, 900.0, suppressed));
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::gate, 1099.0, suppressed), L"first allowed at 100 is still inside the second");
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::gate, 1100.0, suppressed));
			Assert::AreEqual(3u, suppressed);
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::gate, 1101.0, suppressed));
			Assert::AreEqual(0u, suppressed);
		}

		TEST_METHOD(LimiterWindowRollsOver)
		{
			VpEvent::Limiter limiter(2);
			uint32_t s = 0;
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::resync, 0.0, s));
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::resync, 10.0, s));
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::resync, 20.0, s));
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::resync, 5000.0, s));
			Assert::AreEqual(1u, s);
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::resync, 5001.0, s));
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::resync, 5002.0, s));
		}

		TEST_METHOD(LimiterIsPerKindAndDefaultsToTwenty)
		{
			VpEvent::Limiter limiter;
			Assert::AreEqual(20u, limiter.PerSecond());
			uint32_t s = 0;
			for (int i = 0; i < 20; ++i)
				Assert::IsTrue(limiter.Allow(VpEvent::Kind::resync, i, s));
			Assert::IsFalse(limiter.Allow(VpEvent::Kind::resync, 21.0, s));
			Assert::IsTrue(limiter.Allow(VpEvent::Kind::gate, 21.0, s), L"another kind has its own budget");
		}

		TEST_METHOD(LimiterFormatLimitedAppendsSuppressed)
		{
			VpEvent::Limiter limiter(1);
			const std::string first = limiter.FormatLimited(VpEvent::Kind::gate, 0.0, { { "from", "a" }, { "to", "b" }, { "reason", "r" } });
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=gate from=a to=b reason=r"), first);
			Assert::IsTrue(limiter.FormatLimited(VpEvent::Kind::gate, 1.0, { { "from", "a" }, { "to", "b" }, { "reason", "r" } }).empty());
			Assert::IsTrue(limiter.FormatLimited(VpEvent::Kind::gate, 2.0, { { "from", "a" }, { "to", "b" }, { "reason", "r" } }).empty());
			const std::string next = limiter.FormatLimited(VpEvent::Kind::gate, 2000.0, { { "from", "a" }, { "to", "b" }, { "reason", "r" } });
			Assert::AreEqual(std::string("VPEV1 t=2000.000 kind=gate from=a to=b reason=r suppressed=2"), next);
		}

		// ---- sink ----
		TEST_METHOD(EmitWithoutASinkDoesNothing)
		{
			VpEvent::SetSink(nullptr);
			VpEvent::Emit(VpEvent::Kind::flag, { { "source", "s" }, { "tag", "t" } });
			{
				SinkScope scope;
			}
			VpEvent::Emit(VpEvent::Kind::flag, { { "source", "s" }, { "tag", "t" } });
			Assert::AreEqual(size_t(0), SinkScope::Lines().size());
		}

		TEST_METHOD(EmitWithASinkDeliversOneValidLine)
		{
			SinkScope scope;
			VpEvent::Emit(VpEvent::Kind::flag, { { "source", "hotkey" }, { "tag", "x" } });
			const std::vector<std::string> lines = SinkScope::Lines();
			Assert::AreEqual(size_t(1), lines.size());
			Assert::IsTrue(lines[0].compare(0, 8, "VPEV1 t=") == 0);
			Assert::IsTrue(lines[0].find(" kind=flag source=hotkey tag=x") != std::string::npos);
			Assert::IsTrue(VpEvent::NowMs() >= 0.0);
		}

		// ---- ResyncShadow ----
		TEST_METHOD(ResyncShadowEmitsExactlyOneEventLinePerRecordedEvent)
		{
			VpResync::Shadow shadow;
			std::vector<std::string> log;
			shadow.SetLogger([&log](const std::string& line) { log.push_back(line); });
			shadow.Configure(VpResync::Shadow::Mode::Shadow, 8);
			uint64_t seq = 0;
			auto push = [&](double now, bool gap)
			{
				VpResync::EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = 1;
				f.stateKey = 0x1000;
				f.formatKey = 7;
				f.frameCounter = 100 + seq;
				if (gap)
				{
					f.gapBefore = true;
					f.gapCause = "missed frames";
					f.gapFrames = 1;
				}
				shadow.OnEnqueue(f, now);
			};
			push(0.0, false);
			push(1.0, true);
			push(2.0, true);
			push(3.0, true);
			push(4.0, true);
			const std::vector<std::string> events = OnlyEvents(log);
			Assert::AreEqual(size_t(5), events.size(), L"one per recorded event, including the unlogged minors (text lines are throttled)");
			Assert::AreEqual(std::string("VPEV1 t=0.000 kind=resync level=full cause=start seq=1 mode=shadow"), events[0]);
			Assert::AreEqual(std::string("VPEV1 t=1000.000 kind=resync level=minor cause=missed_frames seq=2 gap=1 mode=shadow"), events[1]);
			Assert::AreEqual(std::string("VPEV1 t=4000.000 kind=resync level=minor cause=missed_frames seq=5 gap=1 mode=shadow"), events[4]);
			// The text lines are unchanged and throttled: FULL, then minors #1, #2 and #4.
			std::vector<std::string> text;
			for (const std::string& l : log)
				if (l.compare(0, 5, "VPEV1") != 0)
					text.push_back(l);
			Assert::AreEqual(size_t(4), text.size());
			Assert::AreEqual(std::string("Resync (shadow): FULL at seq 1 (start)"), text[0]);
			Assert::AreEqual(std::string("Resync (shadow): MINOR at seq 2 (missed frames; gap 1)"), text[1]);
		}

		TEST_METHOD(ResyncShadowEventLinesAreRateLimited)
		{
			VpResync::Shadow shadow;
			std::vector<std::string> log;
			shadow.SetLogger([&log](const std::string& line) { log.push_back(line); });
			shadow.Configure(VpResync::Shadow::Mode::Shadow, 8);
			uint64_t seq = 0;
			auto push = [&](double now, bool gap)
			{
				VpResync::EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = 1;
				f.stateKey = 0x1000;
				f.formatKey = 7;
				f.frameCounter = 100 + seq;
				if (gap)
				{
					f.gapBefore = true;
					f.gapCause = "missed frames";
					f.gapFrames = 1;
				}
				shadow.OnEnqueue(f, now);
			};
			push(0.0, false);
			for (int i = 0; i < 30; ++i)
				push(1.0 + 0.01 * i, true);   // 31 events inside one second
			Assert::AreEqual(size_t(20), OnlyEvents(log).size());
			push(5.0, true);
			const std::vector<std::string> events = OnlyEvents(log);
			Assert::AreEqual(size_t(21), events.size());
			Assert::AreEqual(std::string("VPEV1 t=5000.000 kind=resync level=minor cause=missed_frames seq=32 gap=1 mode=shadow suppressed=11"), events.back());
		}

		// ---- FallbackStatus ----
		TEST_METHOD(RegistryEmitsOnlyOnChange)
		{
			SinkScope scope;
			FallbackStatus::Registry registry;
			Assert::IsTrue(registry.Engage("capture GPU unpack", "CPU formatters"));
			Assert::IsFalse(registry.Engage("capture GPU unpack", "CPU formatters"), L"same reason: no change");
			Assert::IsTrue(registry.Engage("capture GPU unpack", "other reason"));
			Assert::IsFalse(registry.Release("never engaged"));
			Assert::IsTrue(registry.Release("capture GPU unpack"));
			Assert::IsFalse(registry.Release("capture GPU unpack"));
			const std::vector<std::string> lines = SinkScope::Lines();
			Assert::AreEqual(size_t(3), lines.size());
			Assert::IsTrue(lines[0].find(" kind=fallback state=engaged id=\"capture GPU unpack\" reason=\"CPU formatters\"") != std::string::npos);
			Assert::IsTrue(lines[1].find(" kind=fallback state=engaged id=\"capture GPU unpack\" reason=\"other reason\"") != std::string::npos);
			Assert::IsTrue(lines[2].find(" kind=fallback state=released id=\"capture GPU unpack\"") != std::string::npos);
			for (const std::string& l : lines)
				Assert::IsTrue(l.compare(0, 8, "VPEV1 t=") == 0);
		}

		TEST_METHOD(RegistryEmitsNothingWithoutASink)
		{
			VpEvent::SetSink(nullptr);
			FallbackStatus::Registry registry;
			{
				SinkScope scope;   // clears the capture, then resets the sink to null on exit
			}
			Assert::IsTrue(registry.Engage("a", "b"));
			Assert::IsTrue(registry.Release("a"));
			Assert::AreEqual(size_t(0), SinkScope::Lines().size());
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());
		}
	};
}
