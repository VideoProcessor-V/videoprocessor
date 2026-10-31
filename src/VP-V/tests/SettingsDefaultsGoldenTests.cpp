// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// T5b: the CPU defaults golden. The renderer's own loader (through its settings exports, no device)
// evaluates a fixed set of configurations; the normalised text (\n, classic numbers) of what it
// resolves - the settings dump plus the effective-settings fingerprint split into fp[NNN]=value lines -
// is compared with the checked-in golden src\VP-V\tests\golden\cpu-defaults.golden.txt, followed by
// the VP-V tier defaults and the Original pathway overrides. A default that changes shows up as a
// differing line.
//
// Review rule: a mismatch is either a bug or an intended default change. Look at every differing line,
// keep the change only if the owner-visible behaviour is meant, then regenerate with
// VP_TEST_REGOLDEN=1 and commit the golden with the change that caused it.
//
// Cases: empty config; the repository sample VideoProcessor.cfg; [vpv] pipeline_original_pathway: on;
// each global tier (Classic, Low, Mid, Upper: every tiered unit's preset keys written). No input-profile
// mode is selectable for an unknown source (the loader takes the first profile), so none is added.

#include "SettingsTestSupport.h"

#include <RendererProfileConfig.h>
#include <VP-V/config/VpvTierTable.generated.h>
#include <VP-V/core/OriginalPathway.h>

#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SettingsDefaultsGoldenTest
{
	using namespace SettingsTestSupport;

	namespace
	{
		std::string GoldenPath() { return RepoRoot() + "src\\VP-V\\tests\\golden\\cpu-defaults.golden.txt"; }

		std::string Normalise(std::string text)
		{
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}

		void AppendCase(std::ostringstream& out, Renderer& renderer, const std::string& name, const std::string& config)
		{
			const Evaluation e = renderer.Evaluate(config);
			out << "### case " << name << "\n";
			if (!e.ok)
			{
				out << "(the renderer did not load this configuration)\n";
				return;
			}
			out << Normalise(e.describe);
			if (!e.describe.empty() && e.describe.back() != '\n') out << "\n";
			for (size_t i = 0; i < e.fingerprint.size(); ++i)
				out << "fp[" << std::setw(3) << std::setfill('0') << i << "]=" << e.fingerprint[i] << "\n";
		}

		// Writes the preset keys of every tiered unit at global tier g; [vpv] keys go to [vpv].
		std::string TierConfig(int g, const std::set<std::string>& vpvKeys)
		{
			std::string profile, vpv;
			const Vpv::Table& table = Vpv::TierTable();
			for (const Vpv::Unit& unit : table.units)
			{
				const int p = Vpv::GlobalToUnit(table, unit, g);
				if (p < 0) continue;
				for (const Vpv::KeyValue& kv : unit.presets[static_cast<size_t>(p)].set)
					(vpvKeys.count(kv.key) ? vpv : profile) += std::string(kv.key) + ": " + kv.value + "\n";
			}
			return Base() + profile + (vpv.empty() ? std::string() : "[vpv]\n" + vpv);
		}

		std::string BuildGolden(Renderer& renderer)
		{
			std::ostringstream out;
			out.imbue(std::locale::classic());
			out << "# CPU defaults golden (SettingsDefaultsGoldenTests). Regenerate with VP_TEST_REGOLDEN=1 after review.\n";
			const std::string headerText = ReadFile(RepoRoot() + "src\\VideoProcessor-Lib\\RendererProfileConfig.h");
			const std::vector<std::string> vpvList = VpvKeys(headerText);
			const std::set<std::string> vpvKeys(vpvList.begin(), vpvList.end());

			AppendCase(out, renderer, "empty", Base());
			AppendCase(out, renderer, "sample-VideoProcessor.cfg", ReadFile(RepoRoot() + "VideoProcessor.cfg"));
			AppendCase(out, renderer, "original-pathway-on", Base() + "[vpv]\npipeline_original_pathway: on\n");
			const Vpv::Table& table = Vpv::TierTable();
			for (size_t g = 0; g < table.tiers.size(); ++g)
				AppendCase(out, renderer, std::string("tier-") + table.tiers[g], TierConfig(static_cast<int>(g), vpvKeys));

			out << "### tier defaults\n";
			for (const Vpv::KeyValue& kv : table.defaults) out << kv.key << "=" << kv.value << "\n";
			out << "### original pathway overrides\n";
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
				out << o.key << "=" << o.value << " (" << o.slot << ")\n";
			return out.str();
		}

		std::vector<std::string> Lines(const std::string& text)
		{
			std::vector<std::string> lines = Split(text, '\n');
			return lines;
		}
	}

	TEST_CLASS(SettingsDefaultsGoldenTests)
	{
	public:
		TEST_METHOD(CpuDefaultsMatchTheCheckedInGolden)
		{
			Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const std::string actual = BuildGolden(renderer);
			Assert::IsTrue(actual.size() > 1000, L"the golden text was produced");

			char* regolden = nullptr;
			size_t regoldenLength = 0;
			const bool regenerate = _dupenv_s(&regolden, &regoldenLength, "VP_TEST_REGOLDEN") == 0 && regolden && std::string(regolden) == "1";
			std::free(regolden);
			if (regenerate)
			{
				const std::string path = GoldenPath();
				std::string folder = path.substr(0, path.find_last_of('\\'));
				CreateDirectoryA(folder.c_str(), nullptr);
				{
					std::ofstream file(path, std::ios::binary | std::ios::trunc);
					file << actual;
				}
				Logger::WriteMessage(("REGOLDEN wrote " + path + "\n").c_str());
				return;
			}

			const std::string expected = Normalise(ReadFile(GoldenPath()));
			Assert::IsTrue(!expected.empty(), L"src\\VP-V\\tests\\golden\\cpu-defaults.golden.txt exists (generate with VP_TEST_REGOLDEN=1)");
			if (expected == actual) return;
			const std::vector<std::string> a = Lines(actual), e = Lines(expected);
			std::ostringstream diff;
			int shown = 0, total = 0;
			for (size_t i = 0; i < std::max(a.size(), e.size()); ++i)
			{
				const std::string got = i < a.size() ? a[i] : std::string("(end)");
				const std::string want = i < e.size() ? e[i] : std::string("(end)");
				if (got == want) continue;
				++total;
				if (shown++ < 20) diff << "\n  line " << (i + 1) << ": golden '" << want << "' now '" << got << "'";
			}
			const std::string message = "CPU defaults differ from the golden in " + std::to_string(total) + " line(s):" + diff.str() +
				"\n  Review each, then regenerate with VP_TEST_REGOLDEN=1 after review.";
			Logger::WriteMessage((message + "\n").c_str());
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
	};
}
