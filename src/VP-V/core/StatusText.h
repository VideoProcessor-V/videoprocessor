#pragma once

#include <ModuleStatusText.h>   // src/VideoProcessor-Lib: the generic key/value status format

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// VP's status text, built in ONE place (WP-11; central-migration-needs.md section 8 item 3).
//
// Why: the Ctrl+I lines, the crash-state lines and the Ctrl+Alt+D bundle lines were composed
// inside VideoProcessorDlg.cpp and StatsOverlayWindow.cpp (Bill's files), so every VP status
// change collided with his OSD work (upstream forecast: 803 conflicted lines). The wording, the
// labels, the order and the "-" placeholders now live here, CPU-tested against verbatim copies
// of the old strings (src/VP-V/tests/StatusTextTests.cpp). The dialog and the overlay keep one
// call each (src/VP-V/gui/VpStatusAdapter.h); the renderer publishes its VP lines through the
// generic IVideoRenderer::GetModuleStatus entry, module "vp", in the ModuleStatusText format.
//
// Header-only, pure, C++14 (no Windows, no MFC): shared by the renderer DLL, the EXE and tests.
// Templated on the character type where text crosses CString, so nothing is re-encoded.
namespace VpStatusText
{
	// ---- the renderer's VP module through GetModuleStatus ---------------------------------------
	constexpr const char* kRendererModule = "vp";
	constexpr const char* kDejudder = "dejudder";
	constexpr const char* kFallbacks = "fallbacks";
	constexpr const char* kLedDrive = "led_drive";
	constexpr const char* kSourceResolution = "source_resolution";

	inline bool IsRendererModule(const char* module)
	{
		return module && std::string(module) == kRendererModule;
	}
	inline bool IsIndexRequest(const char* module) { return !module || module[0] == '\0'; }

	template <class Char>
	struct Line
	{
		bool known = false;                // the old getter returned true
		std::basic_string<Char> text;
		void Set(bool k, const std::basic_string<Char>& t) { known = k; text = k ? t : std::basic_string<Char>(); }
		// As the old call sites tested it: a line to show.
		bool Shown() const { return known && !text.empty(); }
	};

	// What the old per-line getters returned (GetDejudderStatus, GetFallbacksStatus,
	// GetLedDriveStatus, GetSourceResolutionStatus).
	template <class Char>
	struct RendererLines
	{
		Line<Char> dejudder;
		Line<Char> fallbacks;
		Line<Char> ledDrive;
		Line<Char> sourceResolution;
	};

	// Renderer side: only the known lines, in a fixed order. A known but empty line is kept
	// (key with an empty value), so "known" survives the round trip.
	template <class Char>
	std::basic_string<Char> ComposeRendererModule(const RendererLines<Char>& lines)
	{
		std::basic_string<Char> text;
		const std::pair<const char*, const Line<Char>*> order[] = {
			{ kDejudder, &lines.dejudder }, { kFallbacks, &lines.fallbacks },
			{ kLedDrive, &lines.ledDrive }, { kSourceResolution, &lines.sourceResolution } };
		for (const auto& item : order)
			if (item.second->known)
				ModuleStatusText::Append(text, item.first, item.second->text);
		return text;
	}

	// GUI side: the lines back; unknown keys (a newer renderer) are ignored.
	template <class Char>
	RendererLines<Char> ReadRendererModule(const std::basic_string<Char>& text)
	{
		RendererLines<Char> lines;
		for (const auto& entry : ModuleStatusText::Parse(text))
		{
			Line<Char>* line =
				ModuleStatusText::KeyEquals(entry.key, kDejudder) ? &lines.dejudder :
				ModuleStatusText::KeyEquals(entry.key, kFallbacks) ? &lines.fallbacks :
				ModuleStatusText::KeyEquals(entry.key, kLedDrive) ? &lines.ledDrive :
				ModuleStatusText::KeyEquals(entry.key, kSourceResolution) ? &lines.sourceResolution : nullptr;
			if (line && !line->known)
				line->Set(true, entry.value);
		}
		return lines;
	}

	// ASCII literal to either character type.
	// Only for this file's ASCII constants (keys, labels): bytes map one to one (Latin-1). Never
	// use it for renderer or ANSI text; that goes through CString / CStringA as before.
	template <class Char>
	std::basic_string<Char> Widen(const char* text)
	{
		std::basic_string<Char> out;
		for (const char* c = text; *c; ++c)
			out += static_cast<Char>(static_cast<unsigned char>(*c));
		return out;
	}

	// The renderer's whole GetModuleStatus answer: the index for module "", the VP lines for
	// "vp" (fill(lines) reads the renderer's getters), false for any other module.
	template <class Char, class Fill>
	bool RendererModuleStatus(const char* module, Fill fill, std::basic_string<Char>& out)
	{
		out.clear();
		if (IsIndexRequest(module))
		{
			out = ModuleStatusText::Index(std::vector<std::basic_string<Char>>{ Widen<Char>(kRendererModule) });
			return true;
		}
		if (!IsRendererModule(module))
			return false;
		RendererLines<Char> lines;
		fill(lines);
		out = ComposeRendererModule(lines);
		return true;
	}

	// ---- Ctrl+I rows ----------------------------------------------------------------------------
	// The legacy overlay draws a row as its label padded to the value column, then the value; a
	// row without a label is drawn as its value alone (the "Fallbacks engaged" heading). Bill's
	// row-based OSD (upstream) takes the same label/value pairs.
	constexpr size_t kValueColumn = 18;

	template <class Char>
	struct Row
	{
		std::basic_string<Char> label;
		std::basic_string<Char> value;
	};

	// The drawn text of a row (what the legacy overlay's TextOut gets).
	template <class Char>
	std::basic_string<Char> FormatRow(const Row<Char>& row)
	{
		if (row.label.empty())
			return row.value;
		std::basic_string<Char> text = row.label;
		while (text.size() < kValueColumn)
			text += static_cast<Char>(' ');
		return text + row.value;
	}

	// The VP lines the GUI shows in Ctrl+I. Empty = no row (as before).
	template <class Char>
	struct CtrlILines
	{
		std::basic_string<Char> keyOverrides;   // delta 6
		std::basic_string<Char> room;           // Stage 9 room lights
		std::basic_string<Char> content;        // content intent
		std::basic_string<Char> motionIntent;   // intent interpreter items 2-5
		std::basic_string<Char> ambientIntent;
		std::basic_string<Char> blackIntent;
		std::basic_string<Char> displayIntent;
		std::basic_string<Char> dejudder;       // renderer de-judder line (VP Renderer only)
		std::basic_string<Char> fallbacks;      // "Fallbacks engaged: ..." heading
	};

	template <class Char>
	struct CtrlIRows
	{
		std::vector<Row<Char>> afterShaderRule;   // drawn after "Shader Rule:", before "Shaders:"
		std::vector<Row<Char>> afterShaders;      // drawn after the active shader list
	};

	template <class Char>
	CtrlIRows<Char> BuildCtrlIRows(const CtrlILines<Char>& lines)
	{
		CtrlIRows<Char> rows;
		const auto add = [](std::vector<Row<Char>>& to, const char* label, const std::basic_string<Char>& value)
		{
			if (!value.empty())
				to.push_back({ Widen<Char>(label), value });
		};
		add(rows.afterShaderRule, "Key override:", lines.keyOverrides);
		add(rows.afterShaderRule, "Room:", lines.room);
		add(rows.afterShaderRule, "Content:", lines.content);
		add(rows.afterShaderRule, "Motion intent:", lines.motionIntent);
		add(rows.afterShaderRule, "Ambient:", lines.ambientIntent);
		add(rows.afterShaderRule, "Black:", lines.blackIntent);
		add(rows.afterShaderRule, "Display:", lines.displayIntent);
		add(rows.afterShaders, "De-judder:", lines.dejudder);
		add(rows.afterShaders, "", lines.fallbacks);   // the heading carries its own label
		return rows;
	}

	// ---- crash state (rolling state file, owner 2026-10-15) --------------------------------------
	// The renderer-owned keys, in the old order; "-" when the line is unknown or empty, and every
	// key "-" when there is no renderer (audit 2026-10-16: no dead renderer's values).
	constexpr const char* kCrashStateRendererKeys[] = {
		"de-judder", "fallbacks", "presentation", "render stall", "Dynamic Drive", "source resolution", "content",
		"motion intent", "ambient", "black intent", "display intent" };

	template <class Char>
	struct CrashStateInputs
	{
		Line<Char> dejudder, fallbacks, presentation, renderStall, ledDrive, sourceResolution;
		std::basic_string<Char> content, motionIntent, ambientIntent, blackIntent, displayIntent;   // always "known"
	};

	// (key, value) pairs; `narrow` converts a line the way the old code did (CStringA).
	template <class Char, class Narrow>
	std::vector<std::pair<std::string, std::string>> CrashStateRendererLines(bool rendererPresent,
		const CrashStateInputs<Char>& in, Narrow narrow)
	{
		std::vector<std::pair<std::string, std::string>> lines;
		if (!rendererPresent)
		{
			for (const char* key : kCrashStateRendererKeys)
				lines.emplace_back(key, "-");
			return lines;
		}
		const auto line = [&](const char* key, const Line<Char>& l)
		{
			lines.emplace_back(key, l.Shown() ? narrow(l.text) : std::string("-"));
		};
		const auto always = [&](const char* key, const std::basic_string<Char>& text)
		{
			Line<Char> l;
			l.Set(true, text);
			line(key, l);
		};
		line("de-judder", in.dejudder);
		line("fallbacks", in.fallbacks);
		line("presentation", in.presentation);
		line("render stall", in.renderStall);
		line("Dynamic Drive", in.ledDrive);
		line("source resolution", in.sourceResolution);
		always("content", in.content);
		always("motion intent", in.motionIntent);
		always("ambient", in.ambientIntent);
		always("black intent", in.blackIntent);
		always("display intent", in.displayIntent);
		return lines;
	}

	// ---- Ctrl+Alt+D bundle: the renderer's live lines, in the old order --------------------------
	template <class Char>
	struct BundleInputs
	{
		Line<Char> presentation, dejudder, fallbacks, renderStall, outputMode;
	};

	template <class Char, class Narrow>
	std::vector<std::pair<std::string, std::string>> BundleRendererSettings(const BundleInputs<Char>& in, Narrow narrow)
	{
		std::vector<std::pair<std::string, std::string>> settings;
		const auto add = [&](const char* key, const Line<Char>& l)
		{
			if (l.known)   // the old code added a known line even when empty
				settings.emplace_back(key, narrow(l.text));
		};
		add("presentation", in.presentation);
		add("dejudder", in.dejudder);
		add("fallbacks", in.fallbacks);
		add("render_stall", in.renderStall);
		add("output_mode", in.outputMode);
		return settings;
	}

	// ---- the dialog's own VP lines ----------------------------------------------------------------
	// Delta 6: "group selection, key value, ..." (what a key changed this session).
	template <class Char>
	std::basic_string<Char> KeyOverrideSummary(const std::vector<std::basic_string<Char>>& items)
	{
		std::basic_string<Char> summary;
		for (const auto& item : items)
		{
			if (!summary.empty())
				summary += Widen<Char>(", ");
			summary += item;
		}
		return summary;
	}

	// Content intent: "sports (inferred)" / "classifying (<reason>)" [+ ", next <class> at a cut"];
	// empty while intent_content is off. Narrow: the GUI converts it as %hs, as before.
	struct ContentFacts
	{
		bool enabled = false;
		bool classified = false;          // content != Unknown
		std::string reason;               // used while classifying
		std::string content;              // ToString(content)
		std::string source;               // DisplayName(source)
		std::string pendingCandidate;     // ToString(candidate) when known and different; else empty
	};
	inline std::string ContentLine(const ContentFacts& f)
	{
		if (!f.enabled)
			return std::string();
		std::string line = f.classified ? f.content + " (" + f.source + ")" : "classifying (" + f.reason + ")";
		if (!f.pendingCandidate.empty())
			line += ", next " + f.pendingCandidate + " at a cut";
		return line;
	}

	// Stage 9 room lights: "<room> (key|manual|sensor ...)"; empty while [ambient] mode is off.
	struct AmbientFacts
	{
		bool enabled = false;      // mode != Off
		std::string value;         // the current room ("lit" / "dark")
		bool keyOverride = false;  // the room_lights key holds it
		bool manual = false;       // mode manual
		bool portSet = false;      // sensor mode: a port is configured
		bool sensed = false;       // the sensor reports
		double lux = 0.0;
	};
	// The lux reading " (sensor 12.3 lux)". The GUI passes its own formatter: CString::Format
	// and snprintf can round a %.1f tie differently (3.25), and the old line used CString::Format.
	inline std::string DefaultLuxReading(double lux)
	{
		char reading[64];
		std::snprintf(reading, sizeof(reading), " (sensor %.1f lux)", lux);
		return reading;
	}
	template <class LuxReading>
	std::string AmbientLine(const AmbientFacts& f, LuxReading luxReading)
	{
		if (!f.enabled)
			return std::string();
		std::string line = f.value;
		if (f.keyOverride)
			line += " (key)";
		else if (f.manual)
			line += " (manual)";
		else if (!f.portSet)
			line += " (sensor: no port set; saved room)";
		else if (f.sensed)
			line += luxReading(f.lux);
		else
			line += " (sensor not reporting; saved room)";
		return line;
	}
	inline std::string AmbientLine(const AmbientFacts& f) { return AmbientLine(f, DefaultLuxReading); }

	// VP-V nets (net-runner wiring, WP-10): which nets run and their GPU cost; the bypasses are under
	// "Fallbacks engaged". Byte-identical to the draft in docs/design/net-runner-wiring.patch
	// (branch feat/nets) so WP-10 calls this instead of composing the line in the renderer.
	struct NetsFacts
	{
		bool repairLoaded = false;
		std::string repairName;
		bool repairFp16 = false;
		double repairMs = 0.0;
		bool dLoaded = false;
		std::string dName;
		double dMs = 0.0;
	};
	inline std::string NetsLine(const NetsFacts& f)
	{
		std::string nets = "Nets:";
		if (f.repairLoaded)
			nets += " " + f.repairName + (f.repairFp16 ? " (fp16" : " (fp32") + ", " +
				std::to_string(f.repairMs).substr(0, 4) + " ms)";
		if (f.dLoaded)
			nets += " " + f.dName + " (" + std::to_string(f.dMs).substr(0, 4) + " ms)";
		return nets == "Nets:" ? std::string("Nets: none running") : nets;
	}
}
