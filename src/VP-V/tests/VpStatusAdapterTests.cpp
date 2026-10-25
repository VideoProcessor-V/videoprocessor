#include "pch.h"
#include "CppUnitTest.h"

// WP-11 before/after: the Ctrl+I lines, the crash-state lines and the Ctrl+Alt+D bundle lines
// produced through the new path (renderer GetModuleStatus -> VpvGui adapter -> overlay
// extension rows) are byte-identical to the old path (per-line getters -> dialog -> overlay
// fields) for representative states.
//
// The "Legacy" namespace holds verbatim copies of the old code at chore/early-migrations
// ac49a1de (VideoProcessorDlg.cpp UpdateStatsOverlay / ContentIntentLine / AmbientOverlayLine /
// KeyOverrideSummary / BuildStateReportInputs, StatsOverlayWindow.cpp DrawStats); only member
// accesses became parameters. Do not "fix" them: they are the reference.

#include <IRenderer.h>
#include <IntentInterpreterSupport.h>
#include <ShortcutDefaults.h>
#include <VP-V/gui/VpStatusAdapter.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VpStatusAdapterTest
{
	namespace
	{
		struct FakeLine
		{
			bool known = false;
			CString text;
		};

		bool Get(const FakeLine& line, CString& status)
		{
			status.Empty();
			if (!line.known)
				return false;
			status = line.text;
			return true;
		}

		// A renderer with the old per-line getters. moduleEntry: it also answers the generic
		// entry exactly as the patched LibplaceboVideoRenderer::GetModuleStatus does
		// (docs/design/wp11-callsites.patch). Without it, it is a renderer that has no VP module
		// (DirectShow): every VP getter keeps its default.
		class FakeRenderer : public IVideoRenderer
		{
		public:
			bool moduleEntry = true;
			FakeLine dejudder, fallbacks, led, source, presentation, stall, output;

			size_t GetConvertedQueueSize() override { return 0; }
			bool OnVideoState(VideoStateComPtr&) override { return true; }
			void OnVideoFrame(VideoFrame&) override {}
			HRESULT OnWindowsEvent(LONG_PTR, LONG_PTR) override { return S_OK; }
			void Build() override {}
			void Start() override {}
			void Stop() override {}
			void Reset() override {}
			void OnSize() override {}
			void OnPaint() override {}
			void SetFrameQueueMaxSize(size_t) override {}
			void SetSceneAwareTimingCorrection(bool) override {}
			size_t GetFrameQueueSize() override { return 0; }
			double EntryLatencyMs() const override { return 0; }
			double ExitLatencyMs() const override { return 0; }
			uint64_t DroppedFrameCount() const override { return 0; }

			bool GetDejudderStatus(CString& s) const override { return moduleEntry ? Get(dejudder, s) : IVideoRenderer::GetDejudderStatus(s); }
			bool GetFallbacksStatus(CString& s) const override { return moduleEntry ? Get(fallbacks, s) : IVideoRenderer::GetFallbacksStatus(s); }
			bool GetLedDriveStatus(CString& s) const override { return moduleEntry ? Get(led, s) : IVideoRenderer::GetLedDriveStatus(s); }
			bool GetSourceResolutionStatus(CString& s) const override { return moduleEntry ? Get(source, s) : IVideoRenderer::GetSourceResolutionStatus(s); }
			bool GetPresentationTimingStatus(CString& s) const override { return Get(presentation, s); }
			bool GetRenderStallStatus(CString& s) const override { return Get(stall, s); }
			bool GetOutputModeInfo(CString& s) const override { return Get(output, s); }

			bool GetModuleStatus(const char* module, CString& status) const override
			{
				if (!moduleEntry)
					return IVideoRenderer::GetModuleStatus(module, status);
				// The patched LibplaceboVideoRenderer::GetModuleStatus body.
				std::wstring text;
				const bool answered = VpStatusText::RendererModuleStatus<wchar_t>(module,
					[this](VpStatusText::RendererLines<wchar_t>& lines)
					{
						CString line;   // read after each getter (argument order is unspecified)
						const auto take = [&line](VpStatusText::Line<wchar_t>& to, bool known) { to.Set(known, line.GetString()); };
						take(lines.dejudder, GetDejudderStatus(line));
						take(lines.fallbacks, GetFallbacksStatus(line));
						take(lines.ledDrive, GetLedDriveStatus(line));
						take(lines.sourceResolution, GetSourceResolutionStatus(line));
					}, text);
				status = text.c_str();
				return answered;
			}
		};

		// The dialog's own state behind its VP lines.
		struct DialogState
		{
			std::vector<CString> keyOverrideItems;
			bool ambientOn = false;
			std::string ambientValue = "lit";
			bool ambientOverride = false;
			bool ambientManual = false;
			std::string ambientPort;
			bool ambientSensed = false;
			double ambientLux = 0.0;
			ContentIntentStatus content;
			IntentStatus intent;
		};

		namespace Legacy
		{
			// KeyOverrideSummary's join (the items are gathered the same way before and after).
			CString KeyOverrideSummary(const std::vector<CString>& items)
			{
				CString summary;
				auto add = [&summary](const CString& item)
				{
					if (!summary.IsEmpty())
						summary += TEXT(", ");
					summary += item;
				};
				for (const CString& item : items)
					add(item);
				return summary;
			}

			CString ContentIntentLine(const ContentIntentStatus& m_contentStatus)
			{
				if (!m_contentStatus.enabled)
					return CString();
				CString line;
				if (m_contentStatus.content == ContentClass::Unknown)
					line.Format(TEXT("classifying (%hs)"), m_contentStatus.reason);
				else
					line.Format(TEXT("%hs (%hs)"), ToString(m_contentStatus.content), DisplayName(m_contentStatus.source));
				if (m_contentStatus.candidate != ContentClass::Unknown && m_contentStatus.candidate != m_contentStatus.content)
				{
					CString pending;
					pending.Format(TEXT(", next %hs at a cut"), ToString(m_contentStatus.candidate));
					line += pending;
				}
				return line;
			}

			CString AmbientOverlayLine(const DialogState& d)
			{
				if (!d.ambientOn)
					return CString();
				CString line(d.ambientValue.c_str());
				if (d.ambientOverride)
					line += TEXT(" (key)");
				else if (d.ambientManual)
					line += TEXT(" (manual)");
				else if (d.ambientPort.empty())
					line += TEXT(" (sensor: no port set; saved room)");
				else if (d.ambientSensed)
				{
					CString reading;
					reading.Format(TEXT(" (sensor %.1f lux)"), d.ambientLux);
					line += reading;
				}
				else
					line += TEXT(" (sensor not reporting; saved room)");
				return line;
			}

			// The old StatsData VP fields.
			struct Stats
			{
				CString keyOverrides, room, content, motionIntent, ambientIntent, blackIntent, displayIntent;
				CString dejudderStatus, fallbacksStatus;
			};

			// UpdateStatsOverlay's VP parts (inside the RENDERING branch).
			Stats Fill(IVideoRenderer* m_videoRenderer, bool isAlphaRenderer, const DialogState& d)
			{
				Stats stats;
				if (isAlphaRenderer)
					m_videoRenderer->GetDejudderStatus(stats.dejudderStatus);
				m_videoRenderer->GetFallbacksStatus(stats.fallbacksStatus);   // r6 fix 7
				stats.keyOverrides = KeyOverrideSummary(d.keyOverrideItems);
				stats.room = AmbientOverlayLine(d);
				stats.content = ContentIntentLine(d.content);
				stats.motionIntent = CString(IntentInterpreter::MotionIntentLine(d.intent.motion).c_str());
				stats.ambientIntent = CString(IntentInterpreter::AmbientIntentLine(d.intent.ambient).c_str());
				stats.blackIntent = CString(IntentInterpreter::BlackIntentLine(d.intent.black).c_str());
				stats.displayIntent = CString(IntentInterpreter::DisplayIntentLine(d.intent.display).c_str());
				return stats;
			}

			// DrawStats: the text handed to TextOut for the VP rows, in its two places.
			void Draw(const Stats& m_stats, std::vector<CString>& afterRule, std::vector<CString>& afterShaders)
			{
				CString line;
				if (!m_stats.keyOverrides.IsEmpty())
				{
					line.Format(TEXT("Key override:     %-s"), static_cast<LPCTSTR>(m_stats.keyOverrides));
					afterRule.push_back(line);
				}
				if (!m_stats.room.IsEmpty())
				{
					line.Format(TEXT("Room:             %-s"), static_cast<LPCTSTR>(m_stats.room));
					afterRule.push_back(line);
				}
				if (!m_stats.content.IsEmpty())
				{
					line.Format(TEXT("Content:          %-s"), static_cast<LPCTSTR>(m_stats.content));
					afterRule.push_back(line);
				}
				const std::pair<LPCTSTR, const CString*> intentLines[] = {
					{ TEXT("Motion intent:    %-s"), &m_stats.motionIntent }, { TEXT("Ambient:          %-s"), &m_stats.ambientIntent },
					{ TEXT("Black:            %-s"), &m_stats.blackIntent }, { TEXT("Display:          %-s"), &m_stats.displayIntent } };
				for (const auto& intentLine : intentLines)
				{
					if (intentLine.second->IsEmpty())
						continue;
					line.Format(intentLine.first, static_cast<LPCTSTR>(*intentLine.second));
					afterRule.push_back(line);
				}
				if (!m_stats.dejudderStatus.IsEmpty())
				{
					line.Format(TEXT("De-judder:        %s"), static_cast<LPCTSTR>(m_stats.dejudderStatus));
					afterShaders.push_back(line);
				}
				if (!m_stats.fallbacksStatus.IsEmpty())
					afterShaders.push_back(m_stats.fallbacksStatus);
			}

			// The crash-state block's renderer lines.
			std::map<std::string, std::string> CrashState(IVideoRenderer* m_videoRenderer, const DialogState& d)
			{
				std::map<std::string, std::string> crashState;
				const auto rendererLine = [&](const char* key, bool known, const CString& text)
				{
					crashState[key] = known && !text.IsEmpty() ? std::string(CStringA(text).GetString()) : std::string("-");
				};
				CString text;
				if (m_videoRenderer)
				{
					rendererLine("de-judder", m_videoRenderer->GetDejudderStatus(text), text);
					rendererLine("fallbacks", m_videoRenderer->GetFallbacksStatus(text), text);
					rendererLine("presentation", m_videoRenderer->GetPresentationTimingStatus(text), text);
					rendererLine("render stall", m_videoRenderer->GetRenderStallStatus(text), text);
					rendererLine("Dynamic Drive", m_videoRenderer->GetLedDriveStatus(text), text);
					rendererLine("source resolution", m_videoRenderer->GetSourceResolutionStatus(text), text);
					rendererLine("content", true, ContentIntentLine(d.content));
					rendererLine("motion intent", true, CString(IntentInterpreter::MotionIntentLine(d.intent.motion).c_str()));
					rendererLine("ambient", true, CString(IntentInterpreter::AmbientIntentLine(d.intent.ambient).c_str()));
					rendererLine("black intent", true, CString(IntentInterpreter::BlackIntentLine(d.intent.black).c_str()));
					rendererLine("display intent", true, CString(IntentInterpreter::DisplayIntentLine(d.intent.display).c_str()));
				}
				else
				{
					static const char* const kRendererKeys[] = {
						"de-judder", "fallbacks", "presentation", "render stall", "Dynamic Drive", "source resolution", "content",
						"motion intent", "ambient", "black intent", "display intent" };
					for (const char* key : kRendererKeys)
						crashState[key] = std::string("-");
				}
				return crashState;
			}

			std::string FallbacksHeading(IVideoRenderer* m_videoRenderer)
			{
				CString text;
				return m_videoRenderer && m_videoRenderer->GetFallbacksStatus(text) ?
					std::string(CStringA(text).GetString()) : std::string();
			}

			std::vector<std::pair<std::string, std::string>> Bundle(IVideoRenderer* m_videoRenderer)
			{
				std::vector<std::pair<std::string, std::string>> settings;
				if (m_videoRenderer)
				{
					CString text;
					if (m_videoRenderer->GetPresentationTimingStatus(text))
						settings.emplace_back("presentation", std::string(CStringA(text).GetString()));
					if (m_videoRenderer->GetDejudderStatus(text))
						settings.emplace_back("dejudder", std::string(CStringA(text).GetString()));
					if (m_videoRenderer->GetFallbacksStatus(text))
						settings.emplace_back("fallbacks", std::string(CStringA(text).GetString()));
					if (m_videoRenderer->GetRenderStallStatus(text))
						settings.emplace_back("render_stall", std::string(CStringA(text).GetString()));
					if (m_videoRenderer->GetOutputModeInfo(text))
						settings.emplace_back("output_mode", std::string(CStringA(text).GetString()));
				}
				return settings;
			}
		}

		// ---- the new path -------------------------------------------------------------------------
		struct Row
		{
			CString label;
			CString value;
		};

		CString NewAmbientLine(const DialogState& d)
		{
			// The patched CVideoProcessorDlg::AmbientOverlayLine.
			VpStatusText::AmbientFacts facts;
			facts.enabled = d.ambientOn;
			if (facts.enabled)
			{
				facts.value = d.ambientValue;
				facts.keyOverride = d.ambientOverride;
				facts.manual = d.ambientManual;
				facts.portSet = !d.ambientPort.empty();
				facts.sensed = facts.portSet && d.ambientSensed;
				facts.lux = facts.sensed ? d.ambientLux : 0.0;
			}
			return VpvGui::AmbientLine(facts);
		}

		VpvGui::DialogLines NewDialogLines(const DialogState& d)
		{
			VpvGui::DialogLines lines;
			lines.keyOverrides = VpvGui::KeyOverrideSummary(d.keyOverrideItems);
			lines.room = NewAmbientLine(d);
			lines.content = VpvGui::ContentIntentLine(d.content);
			lines.intent = d.intent;
			return lines;
		}

		// The patched DrawStats' drawExtraRows.
		std::vector<CString> NewDraw(const std::vector<Row>& rows)
		{
			std::vector<CString> drawn;
			CString line;
			for (const Row& row : rows)
			{
				if (row.label.IsEmpty())
					line = row.value;
				else
					line.Format(TEXT("%-18s%s"), static_cast<LPCTSTR>(row.label), static_cast<LPCTSTR>(row.value));
				drawn.push_back(line);
			}
			return drawn;
		}

		struct MapRecorder
		{
			std::map<std::string, std::string> values;
			void SetValue(const std::string& key, const std::string& value) { values[key] = value; }
		};

		void AssertSameLines(const std::vector<CString>& expected, const std::vector<CString>& actual, const wchar_t* what)
		{
			Assert::AreEqual(expected.size(), actual.size(), what);
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(std::wstring(expected[i].GetString()), std::wstring(actual[i].GetString()), what);
		}

		// Old and new for one state: overlay text (both places), row counts (overlay height),
		// crash state, fallbacks heading and the bundle.
		void AssertIdentical(IVideoRenderer* renderer, bool isAlphaRenderer, const DialogState& d)
		{
			std::vector<CString> oldA, oldB;
			if (renderer)
				Legacy::Draw(Legacy::Fill(renderer, isAlphaRenderer, d), oldA, oldB);
			const VpvGui::Snapshot snapshot = VpvGui::Poll(renderer);
			std::vector<Row> rowsA, rowsB;
			if (renderer)
				VpvGui::FillCtrlIRows(rowsA, rowsB, snapshot, NewDialogLines(d), isAlphaRenderer);
			AssertSameLines(oldA, NewDraw(rowsA), L"Ctrl+I rows after Shader Rule");
			AssertSameLines(oldB, NewDraw(rowsB), L"Ctrl+I rows after the shader list");

			MapRecorder recorder;
			VpvGui::RecordCrashState(recorder, renderer, snapshot,
				renderer ? VpvGui::ContentIntentLine(d.content) : CString(), d.intent);
			Assert::IsTrue(Legacy::CrashState(renderer, d) == recorder.values, L"crash-state lines");
			Assert::AreEqual(Legacy::FallbacksHeading(renderer), VpvGui::FallbacksHeading(snapshot), L"fallbacks heading");

			std::vector<std::pair<std::string, std::string>> bundle;
			VpvGui::AppendBundleSettings(bundle, renderer);
			Assert::IsTrue(Legacy::Bundle(renderer) == bundle, L"Ctrl+Alt+D lines");
		}

		void SetText(char (&field)[64], const char* text)
		{
			size_t i = 0;
			for (; text[i] && i + 1 < sizeof(field); ++i) field[i] = text[i];
			field[i] = 0;
		}

		DialogState EverythingOn()
		{
			DialogState d;
			d.keyOverrideItems = { CString(TEXT("motion Natural")), CString(TEXT("fullscreen on")) };
			d.ambientOn = true;
			d.ambientValue = "dark";
			d.ambientPort = "COM4";
			d.ambientSensed = true;
			d.ambientLux = 3.25;
			d.content.enabled = true;
			d.content.content = ContentClass::Sports;
			d.content.source = IntentSource::Inferred;
			d.content.candidate = ContentClass::Film;
			d.intent.motion.enabled = true;
			d.intent.motion.appliedScale = 0.75f;
			d.intent.motion.source = IntentSource::Inferred;
			SetText(d.intent.motion.reason, "steady pan");
			d.intent.ambient.enabled = true;
			d.intent.ambient.sensorValid = true;
			d.intent.ambient.smoothedLux = 3.2f;
			d.intent.black.mode = BlackIntentMode::LedProtect;
			d.intent.black.valid = true;
			d.intent.black.stepsBelow = 2;
			d.intent.black.stepsTotal = 9;
			d.intent.display.enabled = true;
			return d;
		}

		void SetAllRendererLines(FakeRenderer& r)
		{
			r.dejudder = { true, TEXT("on (24p in 60 Hz, 3:2), ahead 98%, render-ahead lead 4.2 ms, late 0, dropped 0, gate level 1, motion: NVIDIA optical flow") };
			r.fallbacks = { true, TEXT("Fallbacks engaged: dither (libplacebo dither; sigma-delta dropped: x=1); presentation (open loop: no display statistics, estimated rate)") };
			r.led = { true, TEXT("source (from mode) (mode compensate), driving the projector, light last sent 51 (confirmed 51)") };
			r.source = { true, TEXT("1920x1080 effective, quality high, 10-bit; source_resolution auto") };
			r.presentation = { true, TEXT("ready") };
			r.stall = { true, TEXT("none") };
			r.output = { true, TEXT("Target BT.2020 | RGB -> F/PQ/10 | STATUS: ok") };
		}
	}

	TEST_CLASS(VpStatusAdapterTests)
	{
	public:
		TEST_METHOD(VpRendererEverythingOnIsIdentical)
		{
			FakeRenderer r;
			SetAllRendererLines(r);
			AssertIdentical(&r, true, EverythingOn());
		}

		TEST_METHOD(VpRendererQuietStateIsIdentical)
		{
			// Nothing of VP's on: no de-judder line, "Fallbacks engaged: none", LED off.
			FakeRenderer r;
			r.dejudder = { false, CString() };
			r.fallbacks = { true, TEXT("Fallbacks engaged: none") };
			r.led = { true, TEXT("off") };
			r.source = { true, TEXT("unknown; source_resolution auto") };
			r.presentation = { false, CString() };
			AssertIdentical(&r, true, DialogState());
		}

		TEST_METHOD(KnownButEmptyLinesAreIdentical)
		{
			FakeRenderer r;
			r.dejudder = { true, CString() };
			r.fallbacks = { true, CString() };
			r.led = { true, CString() };
			r.source = { false, CString() };
			r.stall = { true, CString() };
			r.output = { true, CString() };
			AssertIdentical(&r, true, DialogState());
		}

		TEST_METHOD(NonAlphaRendererHidesTheDejudderLine)
		{
			FakeRenderer r;
			SetAllRendererLines(r);
			AssertIdentical(&r, false, EverythingOn());
		}

		TEST_METHOD(RendererWithoutTheVpModuleIsIdentical)
		{
			// DirectShow: no GetModuleStatus, no VP getters.
			FakeRenderer r;
			r.moduleEntry = false;
			r.presentation = { true, TEXT("warming") };
			AssertIdentical(&r, false, EverythingOn());
			AssertIdentical(&r, true, DialogState());
		}

		TEST_METHOD(NoRendererIsIdentical)
		{
			AssertIdentical(nullptr, false, EverythingOn());
		}

		TEST_METHOD(DialogLineVariantsAreIdentical)
		{
			FakeRenderer r;
			SetAllRendererLines(r);
			DialogState d = EverythingOn();
			// Content: classifying, classified without a pending class, candidate = content.
			d.content.content = ContentClass::Unknown;
			SetText(d.content.reason, "waiting for a shot");
			AssertIdentical(&r, true, d);
			d.content.content = ContentClass::Film;
			d.content.candidate = ContentClass::Film;
			AssertIdentical(&r, true, d);
			d.content.candidate = ContentClass::Unknown;
			d.content.source = IntentSource::DvL1;
			AssertIdentical(&r, true, d);
			// Room: each suffix.
			d.ambientSensed = false;
			AssertIdentical(&r, true, d);
			d.ambientPort.clear();
			AssertIdentical(&r, true, d);
			d.ambientManual = true;
			AssertIdentical(&r, true, d);
			d.ambientOverride = true;
			AssertIdentical(&r, true, d);
			d.ambientOn = false;
			AssertIdentical(&r, true, d);
			// Intent: each function off in turn, then black without statistics.
			d.intent.motion.enabled = false;
			AssertIdentical(&r, true, d);
			d.intent.ambient.sensorValid = false;
			AssertIdentical(&r, true, d);
			d.intent.black.valid = false;
			AssertIdentical(&r, true, d);
			d.intent.black.mode = BlackIntentMode::Off;
			d.intent.display.enabled = false;
			d.keyOverrideItems.clear();
			AssertIdentical(&r, true, d);
		}

		TEST_METHOD(NonAsciiTextIsNotReencoded)
		{
			// The generic entry carries wide text end to end: a renderer line with characters
			// outside ASCII reaches the overlay unchanged (the crash state narrows it as before).
			FakeRenderer r;
			r.dejudder = { true, CString(L"on \u2192 60 Hz, motion: caf\u00e9") };
			r.fallbacks = { true, CString(L"Fallbacks engaged: geometry (NLS safe fit \u2014 hook failure)") };
			DialogState d;
			d.keyOverrideItems = { CString(L"profil cin\u00e9ma") };
			AssertIdentical(&r, true, d);
		}

		TEST_METHOD(NewlinesAndBackslashesInRendererLinesAreIdentical)
		{
			// Audit LOW-1: a reason built from an error string may hold a newline; the module
			// text escapes it, so the GUI still gets the old text exactly.
			FakeRenderer r;
			r.dejudder = { true, CString(L"on\r\nsecond line") };
			r.fallbacks = { true, CString(L"Fallbacks engaged: early upload (not available (D3D11: E_FAIL\nat C:\\x\\))") };
			r.source = { true, CString(L"trailing backslash \\") };
			AssertIdentical(&r, true, DialogState());
		}

		TEST_METHOD(ModuleIndexAndUnknownModules)
		{
			FakeRenderer r;
			CString text;
			Assert::IsTrue(r.GetModuleStatus("", text));
			Assert::AreEqual(std::wstring(L"module=vp"), std::wstring(text.GetString()));
			Assert::IsFalse(r.GetModuleStatus("subtitles", text));
			Assert::IsTrue(text.IsEmpty());
			r.moduleEntry = false;
			Assert::IsFalse(r.GetModuleStatus("vp", text));
			Assert::IsFalse(r.InvokeModuleCommand("vp", "anything", text), L"the default command entry handles nothing");
		}

		TEST_METHOD(VpCommandsDispatchToTheirHandlers)
		{
			int room = 0, report = 0;
			VpvGui::CommandHost host;
			host.roomLights = [&room] { ++room; };
			host.stateReport = [&report] { ++report; };
			Assert::IsTrue(VpvGui::DispatchCommand(ID_COMMAND_ROOM_LIGHTS, host));
			Assert::IsTrue(VpvGui::DispatchCommand(ID_COMMAND_STATE_REPORT, host));
			Assert::IsTrue(VpvGui::DispatchCommand(ID_COMMAND_VP_LAST, host), L"a free VP id is VP's, unhandled");
			Assert::IsFalse(VpvGui::DispatchCommand(33205, host), L"Bill's subtitle toggle is not VP's");
			Assert::AreEqual(1, room);
			Assert::AreEqual(1, report);
		}

		TEST_METHOD(EveryVpShortcutHasACompiledDefaultEntry)
		{
			// The Config documentation and ShortcutDefaultsTests key on ShortcutDefaults.h.
			for (const auto& def : VpvShortcuts::kDefinitions)
				Assert::IsNotNull(FindShortcutDefault(def.configKey), CString(def.configKey));
		}
	};
}
