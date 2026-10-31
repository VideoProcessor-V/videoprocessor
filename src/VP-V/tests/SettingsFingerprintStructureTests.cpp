// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// T5a: the settings structure, CPU tier. For every key the configuration validator knows (plus the
// [vpv] keys) a probe value the validator accepts must change the renderer's effective-settings
// fingerprint (otherwise a live edit of that key is not applied), the defaults the VP-V tables carry
// must be accepted by the validator and agree with the renderer's empty-config value, and every VP-V
// key must be documented. Exceptions live in the allow-lists below, each with its reason; an entry
// that no longer applies fails (the lists stay exact). Entries whose reason starts with "GAP:" are
// real gaps found by this test (reported, not fixed here).
//
// Config-control coverage (every setting has a Config widget) is NOT repeated here: the Qt Config tests
// enforce it (ConfigEditorWindowTests.cpp, CoverageAllowList).
//
// Needs the renderer DLL next to the test DLL (vprenderer\VideoProcessorVPRenderer.dll); loaded without
// creating a device.

#include "SettingsTestSupport.h"

#include <RendererProfileConfig.h>
#include <VP-V/config/VpvTierTable.generated.h>
#include <VP-V/core/OriginalPathway.h>

#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SettingsFingerprintStructure
{
	using namespace SettingsTestSupport;

	struct Exception
	{
		const char* key;
		const char* outcome;   // NO_ACCEPTED_PROBE / LOAD_REJECTED / FINGERPRINT_UNCHANGED / DEFAULT_REJECTED / DEFAULT_MISMATCH / UNDOCUMENTED
		const char* reason;
	};

	// ALLOW-LISTS (generated from the first run, then reviewed; keep exact).
	inline const std::vector<Exception>& FingerprintExceptions()
	{
		static const std::vector<Exception> list = {
			// ALLOWLIST-FINGERPRINT-BEGIN
			// [queue] keys: ValidateProfileSetting group "queue" (not a [vprenderer.*] key, so ValidateBaseSetting refuses them);
			// probing them needs a [queue] section; the queue settings are covered by their own tests.
			{ "active_picture_lookahead_frames", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "lead_frames", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "low_latency_path", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "queue_size", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "reset_after_render_restart_seconds", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "reset_queue_too_large_percent", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "startup_preroll_frames", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "steady_reserve_frames", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "target_frames", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			{ "target_latency_ms", "NO_ACCEPTED_PROBE", "[queue] key: validator group queue, not probed through [vprenderer.Default]" },
			// Subtitle profile keys: validator group "subtitles" (a subtitle profile section, not [vprenderer.*]).
			{ "background_edge", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "cleanup_fill", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "minimum_headroom_pixels", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "offset_pixels", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "rounded_corners", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "subtitle_generated_gray_border_color", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "subtitle_generated_gray_color", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "subtitle_script_hint", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "subtitle_text_color", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "temporal_fill", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "text_reduction_percent", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "type", "NO_ACCEPTED_PROBE", "subtitle profile key, not probed through [vprenderer.Default]" },
			{ "subtitle_detection_optimization", "NO_ACCEPTED_PROBE", "subtitle key with a mode parser (no generic probe); not probed" },
			// Display-metadata (lldv) keys and the zoom crop: their own validator groups.
			{ "dolby_vision_tunnel", "NO_ACCEPTED_PROBE", "lldv group key (display/DV metadata), not probed through [vprenderer.Default]" },
			{ "mastering_max_luminance", "NO_ACCEPTED_PROBE", "lldv group key (display/DV metadata), not probed through [vprenderer.Default]" },
			{ "mastering_min_luminance", "NO_ACCEPTED_PROBE", "lldv group key (display/DV metadata), not probed through [vprenderer.Default]" },
			{ "max_cll", "NO_ACCEPTED_PROBE", "lldv group key (display/DV metadata), not probed through [vprenderer.Default]" },
			{ "max_fall", "NO_ACCEPTED_PROBE", "lldv group key (display/DV metadata), not probed through [vprenderer.Default]" },
			{ "fixed_crop_aspect", "NO_ACCEPTED_PROBE", "zoom group key, not probed through [vprenderer.Default]" },
			// ALLOWLIST-FINGERPRINT-END
		};
		return list;
	}

	inline const std::vector<Exception>& DefaultsExceptions()
	{
		static const std::vector<Exception> list = {
			// ALLOWLIST-DEFAULTS-BEGIN
			{ "fbo_float32", "DEFAULT_MISMATCH", "the settings dump prints the tri-state as -1 for the table's auto" },
			// ALLOWLIST-DEFAULTS-END
		};
		return list;
	}

	inline const std::vector<Exception>& DocsExceptions()
	{
		static const std::vector<Exception> list = {
			// ALLOWLIST-DOCS-BEGIN
			{ "dejudder_est_still_zero", "UNDOCUMENTED", "GAP: not in docs/settings/VP-settings-reference.md (documentation gap)" },
			{ "dejudder_source_warp_filter", "UNDOCUMENTED", "GAP: not in docs/settings/VP-settings-reference.md (documentation gap)" },
			{ "entrance_gpu_drain", "UNDOCUMENTED", "GAP: expert [vpv] workaround not in docs/settings/VP-settings-reference.md (documentation gap)" },
			{ "pipeline_entrance_once", "UNDOCUMENTED", "GAP: B3 [vpv] switch not in docs/settings/VP-settings-reference.md (documentation gap)" },
			// ALLOWLIST-DOCS-END
		};
		return list;
	}

	inline std::string HeaderText()
	{
		return ReadFile(RepoRoot() + "src\\VideoProcessor-Lib\\RendererProfileConfig.h");
	}

	// The body of one validator function in the header (to the next top-level inline function).
	inline std::string FunctionBody(const std::string& text, const char* function)
	{
		const size_t start = text.find(function);
		if (start == std::string::npos) return std::string();
		const size_t end = text.find("\n\tinline ", start + 10);
		return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
	}

	inline std::set<std::string> ValidatedKeys(const std::string& text)
	{
		std::set<std::string> keys;
		static const std::regex marker(R"re(\bkey\s*==\s*"([^"]+)")re");
		for (const char* function : { "inline bool ValidateProfileSetting(", "inline bool ValidateBaseSetting(" })
		{
			const std::string body = FunctionBody(text, function);
			for (std::sregex_iterator it(body.begin(), body.end(), marker), last; it != last; ++it)
				keys.insert((*it)[1].str());
		}
		return keys;
	}

	// Choice strings quoted on the validator line(s) of `key == "<key>"` (any spacing).
	inline std::vector<std::string> ChoicesFor(const std::string& text, const std::string& key)
	{
		std::vector<std::string> choices;
		const std::regex needle("key\\s*==\\s*\"" + key + "\"");
		static const std::regex quoted("\"([^\"]*)\"");
		for (std::sregex_iterator hit(text.begin(), text.end(), needle), none; hit != none; ++hit)
		{
			const size_t at = static_cast<size_t>(hit->position());
			const size_t lineEnd = text.find('\n', at);
			const size_t next = lineEnd == std::string::npos ? std::string::npos : text.find('\n', lineEnd + 1);
			const std::string span = text.substr(at + hit->length(), next == std::string::npos ? std::string::npos : next - at - hit->length());
			const size_t brace = span.find('{');
			if (brace == std::string::npos) continue;
			const size_t close = span.find('}', brace);
			const std::string list = span.substr(brace, close == std::string::npos ? std::string::npos : close - brace);
			for (std::sregex_iterator it(list.begin(), list.end(), quoted), last; it != last; ++it)
				choices.push_back((*it)[1].str());
		}
		return choices;
	}

	inline std::vector<std::string> ProbeCandidates(const std::string& text, const std::string& key)
	{
		std::vector<std::string> probes = ChoicesFor(text, key);
		if (key == "pipeline_slots_off") probes.push_back("scaling.rebase");
		for (const char* generic : { "on", "off", "auto", "true", "false", "0", "1", "2", "0.5", "3", "10", "100",
			"native", "full", "limited", "none", "high", "low" })
			probes.push_back(generic);
		std::vector<std::string> unique;
		for (const std::string& p : probes)
			if (std::find(unique.begin(), unique.end(), p) == unique.end()) unique.push_back(p);
		return unique;
	}

	// The validator group (subtitles, input, queue, ...) whose block holds `key == "<key>"`; for reasons.
	inline std::string GroupOf(const std::string& text, const std::string& key)
	{
		const size_t at = text.find("key == \"" + key + "\"");
		if (at == std::string::npos) return "?";
		const size_t g = text.rfind("group == \"", at);
		if (g == std::string::npos) return "?";
		const size_t from = g + 10;
		return text.substr(from, text.find('"', from) - from);
	}

	// The settings dump prints some switches as integers: 1 = on, 0 = off.
	inline std::string IntegerSwitch(const std::string& rendered)
	{
		return rendered == "1" ? "on" : rendered == "0" ? "off" : rendered;
	}

	inline bool Accepts(const std::string& key, const std::string& value)
	{
		return RendererProfileConfig::ValidateBaseSetting(key, value);
	}

	inline std::string ConfigWith(const std::string& key, const std::string& value, bool vpv)
	{
		return Base() + (vpv ? "[vpv]\n" : "") + key + ": " + value + "\n";
	}

	inline const Exception* FindException(const std::vector<Exception>& list, const std::string& key, const char* outcome)
	{
		for (const Exception& e : list)
			if (key == e.key && std::string(outcome) == e.outcome) return &e;
		return nullptr;
	}

	// Checks `found` (key, outcome) pairs against `list`: each must be listed with a reason, and every
	// listed entry must have been found.
	inline void CheckAgainst(const char* what, const std::vector<Exception>& list,
		const std::vector<std::pair<std::string, std::string>>& found, const char* outcomeFilter)
	{
		std::ostringstream problems;
		int reported = 0;
		for (const auto& f : found)
		{
			const Exception* e = FindException(list, f.first, f.second.c_str());
			if (!e || std::string(e->reason).empty())
			{
				problems << "\n  unlisted: { \"" << f.first << "\", \"" << f.second << "\", \"\" },";
				++reported;
			}
		}
		for (const Exception& e : list)
		{
			if (outcomeFilter && std::string(e.outcome).rfind(outcomeFilter, 0) != 0) continue;
			bool seen = false;
			for (const auto& f : found) seen = seen || (f.first == e.key && f.second == e.outcome);
			if (!seen)
			{
				problems << "\n  stale: { \"" << e.key << "\", \"" << e.outcome << "\" }";
				++reported;
			}
		}
		if (reported > 0)
		{
			const std::string message = std::string(what) + ": " + std::to_string(reported) + " allow-list problem(s)" + problems.str();
			Logger::WriteMessage((message + "\n").c_str());
			const std::wstring wide(message.begin(), message.end());
			Assert::Fail(wide.c_str());
		}
	}

	inline std::set<std::string> DocumentedTicks(const std::string& docs)
	{
		std::set<std::string> ticks;
		static const std::regex tick("`([A-Za-z0-9_.\\-]+)`");
		for (std::sregex_iterator it(docs.begin(), docs.end(), tick), last; it != last; ++it)
			ticks.insert((*it)[1].str());
		return ticks;
	}
}

namespace SettingsFingerprintStructureTest
{
	using namespace SettingsTestSupport;
	using namespace SettingsFingerprintStructure;

	TEST_CLASS(SettingsFingerprintStructureTests)
	{
	public:
		// 2a: the key universe.
		TEST_METHOD(ValidatedAndVpvKeysAreEnumerated)
		{
			const std::string text = HeaderText();
			Assert::IsTrue(!text.empty(), L"RendererProfileConfig.h found from __FILE__");
			const std::set<std::string> keys = ValidatedKeys(text);
			const std::vector<std::string> vpv = VpvKeys(text);
			std::set<std::string> all(keys.begin(), keys.end());
			all.insert(vpv.begin(), vpv.end());
			Logger::WriteMessage(("keys: " + std::to_string(keys.size()) + " validated + " + std::to_string(vpv.size()) + " [vpv]\n").c_str());
			Assert::IsTrue(all.size() > 100, L"more than 100 keys");
			Assert::IsTrue(vpv.size() >= 5, L"the [vpv] key list was parsed");
			// Guard for the GPU harness: IsVpvSectionKey() in RendererCaptureHarnessTests.cpp duplicates this list.
			const std::set<std::string> expected = { "pipeline_original_pathway", "pipeline_order", "pipeline_slots_off",
				"calibration_lut_stage", "pipeline_entrance_once", "output_segments_merged", "frame_analysis_store",
				"resync_event", "entrance_gpu_drain", "hdr_inference" };
			Assert::IsTrue(std::set<std::string>(vpv.begin(), vpv.end()) == expected,
				L"the [vpv] keys in RendererProfileConfig.h changed: update IsVpvSectionKey() in RendererCaptureHarnessTests.cpp and this list");
		}

		// 2b: a validator-accepted probe changes the fingerprint (a live edit of the key is applied).
		TEST_METHOD(EveryAcceptedKeyChangesTheFingerprint)
		{
			Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const std::string text = HeaderText();
			std::vector<std::pair<std::string, bool>> universe;   // key, isVpv
			{
				const std::vector<std::string> vpv = VpvKeys(text);
				std::set<std::string> vpvSet(vpv.begin(), vpv.end());
				for (const std::string& k : vpv) universe.emplace_back(k, true);
				for (const std::string& k : ValidatedKeys(text))
					if (!vpvSet.count(k)) universe.emplace_back(k, false);
			}
			const Evaluation base = renderer.Evaluate(Base());
			Assert::IsTrue(base.ok, L"the base configuration loads");
			const Evaluation again = renderer.Evaluate(Base());
			Assert::IsTrue(again.ok && again.fingerprint == base.fingerprint, L"the same file twice gives the same fingerprint");
			Assert::IsTrue(base.fingerprint.size() > 100, L"the fingerprint has many fields");
			// Negative control: an unknown key must not move the fingerprint (it is not a hash of the file text).
			const Evaluation control = renderer.Evaluate(Base() + "# a comment that changes the file text only\n\n");
			Assert::IsTrue(control.ok && control.fingerprint == base.fingerprint, L"a comment leaves the fingerprint unchanged");

			std::vector<std::pair<std::string, std::string>> found;
			std::map<std::string, int> counts;
			for (const auto& entry : universe)
			{
				const std::string& key = entry.first;
				bool accepted = false, loaded = false, changed = false, describeChanged = false;
				for (const std::string& probe : ProbeCandidates(text, key))
				{
					if (!Accepts(key, probe)) continue;
					accepted = true;
					const Evaluation e = renderer.Evaluate(ConfigWith(key, probe, entry.second));
					if (!e.ok) continue;
					loaded = true;
					if (e.fingerprint != base.fingerprint) { changed = true; break; }
					if (e.describe != base.describe) describeChanged = true;
				}
				if (changed) { ++counts["changed"]; continue; }
				std::string outcome = !accepted ? "NO_ACCEPTED_PROBE" : !loaded ? "LOAD_REJECTED" : "FINGERPRINT_UNCHANGED";
				++counts[outcome];
				found.emplace_back(key, outcome);
				Logger::WriteMessage(("OUTCOME " + key + " " + outcome + " group=" + GroupOf(text, key) + "\n").c_str());
				if (outcome == "FINGERPRINT_UNCHANGED" && describeChanged)
					Logger::WriteMessage(("HINT GAP-CANDIDATE " + key + ": the settings dump changes but the fingerprint does not\n").c_str());
			}
			std::ostringstream summary;
			summary << "keys probed: " << universe.size();
			for (const auto& c : counts) summary << ", " << c.first << "=" << c.second;
			summary << "; allow-list entries: " << FingerprintExceptions().size() << "\n";
			Logger::WriteMessage(summary.str().c_str());
			CheckAgainst("fingerprint structure", FingerprintExceptions(), found, nullptr);
		}

		// 2c: the defaults the tables carry are valid and agree with the renderer's empty-config values.
		TEST_METHOD(TableDefaultsAreAcceptedAndMatchTheRenderer)
		{
			Renderer renderer;
			Assert::IsTrue(renderer.Ready());
			const Evaluation empty = renderer.Evaluate(Base());
			Assert::IsTrue(empty.ok);
			std::vector<std::pair<std::string, std::string>> found;
			auto check = [&](const std::string& key, const std::string& value, const char* source)
			{
				if (!Accepts(key, value))
					found.emplace_back(key, "DEFAULT_REJECTED");
				bool present = false;
				const std::string rendered = DescribeValue(empty.describe, key, &present);
				if (present && std::string(source) == "defaults" && !Vpv::SameValue(rendered, value) && !Vpv::SameValue(IntegerSwitch(rendered), value))
				{
					found.emplace_back(key, "DEFAULT_MISMATCH");
					Logger::WriteMessage(("DETAIL " + key + ": table '" + value + "', renderer '" + rendered + "'\n").c_str());
				}
			};
			for (const Vpv::KeyValue& kv : Vpv::TierTable().defaults) check(kv.key, kv.value, "defaults");
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides()) check(o.key, o.value, "original");
			std::sort(found.begin(), found.end());
			found.erase(std::unique(found.begin(), found.end()), found.end());
			CheckAgainst("table defaults", DefaultsExceptions(), found, "DEFAULT_");
		}

		// 2d: every VP-V key is documented.
		TEST_METHOD(EveryVpvKeyIsInTheSettingsReference)
		{
			const std::string docs = ReadFile(RepoRoot() + "docs\\settings\\VP-settings-reference.md");
			Assert::IsTrue(!docs.empty(), L"docs\\settings\\VP-settings-reference.md found");
			const std::set<std::string> ticks = DocumentedTicks(docs);
			std::set<std::string> keys;
			const Vpv::Table& table = Vpv::TierTable();
			for (const Vpv::KeyValue& kv : table.defaults) keys.insert(kv.key);
			for (const Vpv::Unit& u : table.units)
			{
				for (const Vpv::Toggle& t : u.toggles) keys.insert(t.key);
				for (const Vpv::Preset& p : u.presets)
					for (const Vpv::KeyValue& kv : p.set) keys.insert(kv.key);
			}
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides()) keys.insert(o.key);
			for (const std::string& k : VpvKeys(HeaderText())) keys.insert(k);
			keys.insert(table.pathwayKey);
			std::vector<std::pair<std::string, std::string>> found;
			for (const std::string& k : keys)
				if (!ticks.count(k)) found.emplace_back(k, "UNDOCUMENTED");
			Logger::WriteMessage(("VP-V keys: " + std::to_string(keys.size()) + ", undocumented: " + std::to_string(found.size()) + "\n").c_str());
			CheckAgainst("settings reference", DocsExceptions(), found, nullptr);
		}
	};
}

#include <VP-V/core/SlotRegistry.h>
#include "SlotOverrideLayout.h"

namespace SlotOverrideLayoutTest
{
	using namespace SettingsTestSupport;

	// T4 pre-check on the CPU (RTX run 2026-10-31): the configurations EverySlotOffEqualsItsOriginalOverrides
	// renders must load as the harness's input profile, with every override applied. Written into the input
	// profile, downscale_light made the loader reject the file and the GPU test compared against the
	// fallback configuration. This replicates Run's own header lines (RendererCaptureHarnessTests.cpp).
	TEST_CLASS(SlotOverrideLayoutTests)
	{
	public:
		TEST_METHOD(EverySlotsWrittenOverridesLoadAndApply)
		{
			Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const std::string rendering = "[vprenderer.Default]\ndithering: off\ndeband: off\n";
			auto head = [](const std::string& capturePath) {
				return "[general]\npersist_profile_selection: false\n[vprenderer.input.low_latency]\nwhen: $source_rate > 0\ncapture_path: " +
					capturePath + "\n";
			};
			std::string problems;
			int slots = 0, checked = 0;
			for (const PipelineSlots::Info& info : PipelineSlots::Slots())
			{
				if (!PipelineSlots::SwitchHonoured(info.id)) continue;
				++slots;
				const std::string slot = info.name;
				const SlotOverrideLayout::Layout layout = SlotOverrideLayout::ForSlot(slot, "gpu", "finish");
				const Evaluation off = renderer.Evaluate(head("gpu") + rendering + "[vpv]\nentrance_gpu_drain: finish\npipeline_slots_off: " + slot + "\n");
				const Evaluation written = renderer.Evaluate(head(layout.capturePath) + SlotOverrideLayout::Extra(layout, rendering));
				if (!off.ok || DescribeValue(off.describe, "rule").find("input/low_latency") == std::string::npos)
					problems += "\n  " + slot + ": the slot-off configuration does not load as the input profile";
				if (!written.ok || DescribeValue(written.describe, "rule").find("input/low_latency") == std::string::npos)
				{
					problems += "\n  " + slot + ": the written overrides do not load as the input profile (rule '" +
						DescribeValue(written.describe, "rule") + "')";
					continue;
				}
				for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
				{
					if (slot != o.slot || !o.value[0]) continue;
					bool present = false;
					const std::string rendered = DescribeValue(written.describe, o.key, &present);
					if (!present) continue;
					++checked;
					const std::string asSwitch = rendered == "1" ? "on" : rendered == "0" ? "off" : rendered;
					if (!Vpv::SameValue(rendered, o.value) && !Vpv::SameValue(asSwitch, o.value))
						problems += "\n  " + slot + ": " + o.key + " written '" + o.value + "' but loaded '" + rendered + "'";
				}
			}
			Logger::WriteMessage(("slots: " + std::to_string(slots) + ", override values checked: " + std::to_string(checked) + "\n").c_str());
			Assert::IsTrue(slots > 5, L"the switchable slots were found");
			Assert::IsTrue(problems.empty(), std::wstring(problems.begin(), problems.end()).c_str());
		}
	};
}
