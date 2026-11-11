// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// W1-C14: the one status reader gives exactly what the adapter's old inline code gave. The
// expected rows / pairs are composed here with the VpStatusText builders directly (the old
// adapter body); VpStatusAdapterTests covers the MFC thin client against the old dialog code.

#include <VP-V/modules/control/diagnostics/StatusExtension.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace StatusExtensionTest
{
	namespace
	{
		std::string Narrow(const std::wstring& text)
		{
			std::string out;
			for (wchar_t c : text)
				out += static_cast<char>(c);
			return out;
		}

		// Marks every non-ASCII code unit as <uXXXX>, so a value that reaches the caller's text
		// conversion is visible in the result; a pass-through that bypasses it would not be.
		std::string TaggedNarrow(const std::wstring& text)
		{
			static const char* const hex = "0123456789ABCDEF";
			std::string out;
			for (wchar_t c : text)
			{
				const unsigned v = static_cast<unsigned>(c);
				if (v < 0x80)
				{
					out += static_cast<char>(v);
					continue;
				}
				out += "<u";
				out += hex[(v >> 12) & 0xF];
				out += hex[(v >> 8) & 0xF];
				out += hex[(v >> 4) & 0xF];
				out += hex[v & 0xF];
				out += ">";
			}
			return out;
		}

		std::wstring Module()
		{
			VpStatusText::RendererLines<wchar_t> lines;
			lines.dejudder.Set(true, L"on (24p in 60 Hz)");
			lines.fallbacks.Set(true, L"Fallbacks engaged: dither (x; y)");
			lines.ledDrive.Set(true, L"");
			lines.resync.Set(true, L"shadow: 2 events");
			lines.resyncEvents.Set(true, L"e1\ne2");
			return VpStatusText::ComposeRendererModule(lines);
		}

		StatusExtension::DialogTexts Dialog()
		{
			StatusExtension::DialogTexts d;
			d.keyOverrides = L"Dither off";
			d.room = L"lit (key)";
			d.content = L"film (inferred)";
			d.motionIntent = L"smooth";
			d.blackIntent = L"-";
			return d;
		}
	}

	TEST_CLASS(StatusExtensionTests)
	{
	public:
		TEST_METHOD(ReadGivesTheLinesOrAllUnknown)
		{
			const auto snapshot = StatusExtension::Read(true, true, Module());
			Assert::IsTrue(snapshot.rendererPresent);
			Assert::IsTrue(snapshot.vp.dejudder.known);
			Assert::IsTrue(snapshot.vp.ledDrive.known);
			Assert::IsFalse(snapshot.vp.ledDrive.Shown());
			Assert::IsFalse(snapshot.vp.sourceResolution.known);
			// The renderer did not answer (or there is none): nothing is known, nothing invented.
			const auto silent = StatusExtension::Read(true, false, Module());
			Assert::IsTrue(silent.rendererPresent);
			Assert::IsFalse(silent.vp.fallbacks.known);
			const auto none = StatusExtension::Read(false, true, Module());
			Assert::IsFalse(none.rendererPresent);
			Assert::IsFalse(none.vp.dejudder.known);
		}

		TEST_METHOD(FallbacksHeadingIsTheNarrowedLineOrEmpty)
		{
			const auto snapshot = StatusExtension::Read(true, true, Module());
			Assert::AreEqual(std::string("Fallbacks engaged: dither (x; y)"), StatusExtension::FallbacksHeading(snapshot, Narrow));
			const auto unknown = StatusExtension::Read(true, false, std::wstring());
			Assert::AreEqual(std::string(), StatusExtension::FallbacksHeading(unknown, Narrow));
		}

		TEST_METHOD(CtrlIRowsMatchTheBuilderAndTheDejudderLineIsAlphaOnly)
		{
			const auto snapshot = StatusExtension::Read(true, true, Module());
			const StatusExtension::DialogTexts dialog = Dialog();
			for (const bool alpha : { true, false })
			{
				VpStatusText::CtrlILines<wchar_t> lines;
				lines.keyOverrides = dialog.keyOverrides;
				lines.room = dialog.room;
				lines.content = dialog.content;
				lines.motionIntent = dialog.motionIntent;
				lines.blackIntent = dialog.blackIntent;
				if (alpha)
					lines.dejudder = snapshot.vp.dejudder.text;
				lines.resync = snapshot.vp.resync.text;
				lines.fallbacks = snapshot.vp.fallbacks.text;
				const auto expected = VpStatusText::BuildCtrlIRows(lines);
				const auto actual = StatusExtension::CtrlIRows(snapshot, dialog, alpha);
				Assert::AreEqual(expected.afterShaderRule.size(), actual.afterShaderRule.size());
				Assert::AreEqual(expected.afterShaders.size(), actual.afterShaders.size());
				for (size_t i = 0; i < expected.afterShaderRule.size(); ++i)
				{
					Assert::IsTrue(expected.afterShaderRule[i].label == actual.afterShaderRule[i].label);
					Assert::IsTrue(expected.afterShaderRule[i].value == actual.afterShaderRule[i].value);
				}
				for (size_t i = 0; i < expected.afterShaders.size(); ++i)
				{
					Assert::IsTrue(expected.afterShaders[i].label == actual.afterShaders[i].label);
					Assert::IsTrue(expected.afterShaders[i].value == actual.afterShaders[i].value);
				}
				Assert::AreEqual(alpha ? size_t(3) : size_t(2), actual.afterShaders.size());   // [De-judder], Resync, heading
			}
		}

		TEST_METHOD(CrashStateKeepsTheOldKeyOrderAndDashesWithoutARenderer)
		{
			StatusExtension::HostLines host;
			host.presentation.Set(true, L"vsync ok");
			host.renderStall.Set(false, L"ignored");
			const auto live = StatusExtension::CrashState(StatusExtension::Read(true, true, Module()), host, Dialog(), Narrow);
			Assert::AreEqual(size_t(11), live.size());
			Assert::AreEqual(std::string("de-judder"), live[0].first);
			Assert::AreEqual(std::string("on (24p in 60 Hz)"), live[0].second);
			Assert::AreEqual(std::string("Fallbacks engaged: dither (x; y)"), live[1].second);
			Assert::AreEqual(std::string("vsync ok"), live[2].second);
			Assert::AreEqual(std::string("-"), live[3].second);    // render stall unknown
			Assert::AreEqual(std::string("-"), live[4].second);    // led drive known but empty
			Assert::AreEqual(std::string("film (inferred)"), live[6].second);
			const auto dead = StatusExtension::CrashState(StatusExtension::Read(false, false, std::wstring()), host, Dialog(), Narrow);
			Assert::AreEqual(size_t(11), dead.size());
			for (const auto& pair : dead)
				Assert::AreEqual(std::string("-"), pair.second);
		}

		TEST_METHOD(NonAsciiValuesGoThroughTheCallersNarrowFunction)
		{
			// Escaped, ASCII source: U+00E9 (e acute) and U+2014 (em dash).
			VpStatusText::RendererLines<wchar_t> lines;
			lines.dejudder.Set(true, L"caf\u00e9");
			lines.fallbacks.Set(true, L"Fallbacks engaged: a \u2014 b");
			lines.resyncEvents.Set(true, L"ev\u00e9");
			const auto snapshot = StatusExtension::Read(true, true, VpStatusText::ComposeRendererModule(lines));
			Assert::AreEqual(std::string("Fallbacks engaged: a <u2014> b"), StatusExtension::FallbacksHeading(snapshot, TaggedNarrow));

			StatusExtension::HostLines host;
			host.presentation.Set(true, L"p\u00e9");
			StatusExtension::DialogTexts dialog = Dialog();
			dialog.content = L"film \u2014 inferred";
			const auto crash = StatusExtension::CrashState(snapshot, host, dialog, TaggedNarrow);
			Assert::AreEqual(std::string("caf<u00E9>"), crash[0].second);
			Assert::AreEqual(std::string("Fallbacks engaged: a <u2014> b"), crash[1].second);
			Assert::AreEqual(std::string("p<u00E9>"), crash[2].second);
			Assert::AreEqual(std::string("film <u2014> inferred"), crash[6].second);

			const auto bundle = StatusExtension::Bundle(snapshot, host, TaggedNarrow);
			Assert::AreEqual(std::string("p<u00E9>"), bundle[0].second);
			Assert::AreEqual(std::string("caf<u00E9>"), bundle[1].second);
			Assert::AreEqual(std::string("ev<u00E9>"), bundle[bundle.size() - 1].second);
		}

		TEST_METHOD(BundleListsKnownLinesInTheOldOrder)
		{
			StatusExtension::HostLines host;
			host.presentation.Set(true, L"p");
			host.renderStall.Set(true, L"");     // known but empty is kept
			host.outputMode.Set(true, L"o");
			const auto pairs = StatusExtension::Bundle(StatusExtension::Read(true, true, Module()), host, Narrow);
			const char* keys[] = { "presentation", "dejudder", "fallbacks", "render_stall", "output_mode", "resync", "resync_events" };
			Assert::AreEqual(size_t(7), pairs.size());
			for (size_t i = 0; i < pairs.size(); ++i)
				Assert::AreEqual(std::string(keys[i]), pairs[i].first);
			Assert::AreEqual(std::string(), pairs[3].second);
			Assert::AreEqual(std::string("e1\ne2"), pairs[6].second);
		}
	};
}
