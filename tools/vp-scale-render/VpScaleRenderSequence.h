#pragma once

// VpScaleRender's pure sequence rules (fix/vpscalerender-motion, 2026-10-24).
//
// Round 1's picture_seq pan renders never ran VP's motion vectors: the de-judder strength was 0
// with nothing else starting the estimate (the grain map never starts it on its own,
// DejudderPolicy::Decide), and every picture boundary re-sent the previous frame's sequence and
// timestamp (a 0-tick step: "capture timestamp discontinuity", a motion invalidation per pass).
// These rules are header-only and pure (no renderer, no GPU) so VideoProcessor-Test checks them
// on every build: the session timestamps, the motion overrides and the result-line contract.
// docs/VP-scale-render-motion.md describes the job keys built on them.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace VpScaleRenderSequence
{
	// One clock for the whole session: frame n (0-based, counted over every picture of the job,
	// warm-up included) carries the capture sequence n + 1 and the timestamp
	// startTicks + n * period, computed as an exact rational (no per-frame rounding drift). The
	// renderer judges each step against llround(ticksPerSecond / rate); exact stamps differ from it
	// by at most one tick, always a Normal step (CaptureTimestampGap::Classify).
	struct SessionClock
	{
		long long startTicks = 0;
		long long ticksPerSecond = 10000000;
		long long rateNumerator = 24000;
		long long rateDenominator = 1001;

		long long Stamp(long long n) const
		{
			// n * den * tps fits in 64 bits for any real session (n < 10^7 at 10 MHz, den 1001).
			return startTicks + (n * rateDenominator * ticksPerSecond + rateNumerator / 2) / rateNumerator;
		}
		static unsigned long long Sequence(long long n) { return static_cast<unsigned long long>(n) + 1; }
		double PeriodSeconds() const { return static_cast<double>(rateDenominator) / static_cast<double>(rateNumerator); }
	};

	// The renderer settings a motion render needs (job key `motion vectors`). The estimate runs
	// "alone" for the occlusion mask on a film-like source with no de-judder strength
	// (DejudderPolicy::Decide: Active, no extras), so every source frame gets an A->B estimate
	// while no refresh is ever warped: each capture still shows exactly one source frame. The
	// occlusion mask itself is only applied by a sigma-delta shader; temporal_dither is forced off
	// (audit 2026-10-24: its default is auto, which may add the sigma-delta) so the mask changes no
	// output pixel. The training renders are the input of nets that run BEFORE VP's output dither
	// (the scaler's sub-pass), so a render without dither is the right training input whatever the
	// shipped dither setting is. Interpolation, hiccup repair and libplacebo's smooth motion stay
	// off (they would mix or replace frames). Every key is valid in the display group ([vprenderer]
	// and its variants: input / motion keys are accepted there, RendererProfileConfig).
	struct Override { const char* key; const char* value; };
	inline const std::vector<Override>& MotionOverrides()
	{
		static const std::vector<Override> overrides = {
			{ "occlusion_mask", "on" },
			{ "dejudder_strength", "0" },
			{ "dejudder_strength_even", "0" },
			{ "interpolation", "off" },
			{ "interpolation_fill_gaps", "off" },
			{ "motion_engine", "vp" },
			{ "smooth_motion", "off" },
			{ "temporal_dither", "off" },
		};
		return overrides;
	}

	inline std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
		while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
		return s.substr(a, b - a);
	}

	inline std::string Lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// The renderer's child namespaces below [vprenderer] (RendererProfileConfig::IsRendererChildNamespace):
	// independent profile groups with their own key sets, never display variants.
	inline bool IsChildNamespace(const std::string& name)
	{
		for (const char* child : { "input", "input_processing", "scaling", "motion", "color", "output", "viewport", "zoom", "subtitles" })
		{
			const std::string root(child);
			if (name == root || (name.size() > root.size() && name.compare(0, root.size(), root) == 0 && name[root.size()] == '.'))
				return true;
		}
		return false;
	}

	// Where the motion overrides go, by the form of the file (audit 2026-10-24: never into the child
	// groups, where the keys are invalid and the whole unified model is dropped):
	// - unified form with [vprenderer]: [vprenderer] and its display variants [vprenderer.<name>];
	// - a file without [vprenderer]: the display section the renderer reads (RendererConfigView:
	//   [vpvr.display], else [display], else [libplacebo]); if there is none, one is added - [vprenderer]
	//   when the file is unified (it has [general] / profile sections), else [display] (a legacy file
	//   stays legacy).
	// Override keys are stripped from every other renderer-owned section and profile section
	// ([vprenderer.*] child groups, [profiles.*]), so no profile can switch them back.
	struct SectionPlan
	{
		std::set<std::string> targets;   // lower-case section names that get the overrides
		std::string added;               // a section the text did not have, added at the end ("" = none)
		bool unified = false;
	};

	inline SectionPlan PlanMotionSections(const std::vector<std::string>& sectionsLower)
	{
		SectionPlan plan;
		const std::set<std::string> all(sectionsLower.begin(), sectionsLower.end());
		for (const std::string& s : sectionsLower)
			if (s == "vprenderer" || s == "general" || s == "profile_groups" || s == "event_actions" ||
				s.rfind("profiles.", 0) == 0 || s.rfind("profile_groups.", 0) == 0 || s.rfind("vprenderer.", 0) == 0)
				plan.unified = true;
		// the display variants [vprenderer.<name>] (selectable display profiles: they may set the keys too)
		for (const std::string& s : sectionsLower)
			if (s.rfind("vprenderer.", 0) == 0)
			{
				const std::string name = s.substr(11);
				if (!name.empty() && name.find('.') == std::string::npos && !IsChildNamespace(name))
					plan.targets.insert(s);
			}
		// the display baseline the renderer reads (RendererConfigView::TryGetDisplayString): [vprenderer], else the
		// legacy [vpvr.display] / [display] / [libplacebo]; adding [vprenderer] next to a legacy section would move the
		// lookup away from it, so a legacy section is used when it exists.
		if (all.count("vprenderer"))
		{
			plan.targets.insert("vprenderer");
			return plan;
		}
		for (const char* legacy : { "vpvr.display", "display", "libplacebo" })
			if (all.count(legacy))
			{
				plan.targets.insert(legacy);
				return plan;
			}
		plan.added = plan.unified ? "vprenderer" : "display";
		plan.targets.insert(plan.added);
		return plan;
	}

	inline bool StripsOverrides(const std::string& sectionLower)
	{
		return sectionLower == "vprenderer" || sectionLower.rfind("vprenderer.", 0) == 0 || sectionLower == "vpvr.display" ||
			sectionLower == "display" || sectionLower == "libplacebo" || sectionLower.rfind("profiles.", 0) == 0;
	}

	// The configuration text with the motion overrides (see PlanMotionSections). Everything else
	// (comments, other keys, shader sections, the file's form) is kept. The job's own configuration
	// file is never changed: the tool writes this text to a file of its own and validates it with
	// the renderer's own RendererProfileConfig before rendering (VpScaleRenderCore).
	inline std::string WithMotionOverrides(const std::string& text, SectionPlan* planOut = nullptr)
	{
		std::set<std::string> keys;
		for (const Override& o : MotionOverrides()) keys.insert(o.key);
		std::vector<std::string> lines, sections;
		{
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const std::string t = Trim(line);
				if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
					sections.push_back(Lower(Trim(t.substr(1, t.size() - 2))));
				lines.push_back(line);
			}
		}
		const SectionPlan plan = PlanMotionSections(sections);
		if (planOut) *planOut = plan;
		std::ostringstream out;
		std::string current;
		for (const std::string& line : lines)
		{
			const std::string t = Trim(line);
			if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
			{
				current = Lower(Trim(t.substr(1, t.size() - 2)));
				out << line << "\n";
				if (plan.targets.count(current))
					for (const Override& o : MotionOverrides())
						out << o.key << ": " << o.value << "\n";
				continue;
			}
			if (StripsOverrides(current) && !t.empty() && t[0] != '#' && t[0] != ';')
			{
				const size_t sep = t.find_first_of(":=");
				if (sep != std::string::npos && keys.count(Lower(Trim(t.substr(0, sep)))))
					continue;
			}
			out << line << "\n";
		}
		if (!plan.added.empty())
		{
			out << "[" << plan.added << "]\n";
			for (const Override& o : MotionOverrides())
				out << o.key << ": " << o.value << "\n";
		}
		return out.str();
	}

	// Renderer log lines that mean the configuration (or a part of it) was NOT applied: a motion /
	// training render must fail on them, never run with other settings than asked for.
	inline bool IsConfigRejectionLine(const std::string& line)
	{
		if (line.find("unified renderer configuration ignored") != std::string::npos) return true;
		if (line.find("exists but is not applied") != std::string::npos) return true;
		if ((line.rfind("profile '", 0) == 0 || line.rfind("display rule '", 0) == 0) && line.find("': invalid ") != std::string::npos) return true;
		return false;
	}

	// The value of a top-level "key": value in a capture sidecar (string or number), "" if absent.
	inline std::string JsonField(const std::string& json, const std::string& key)
	{
		const std::string k = "\"" + key + "\": ";
		const size_t at = json.find(k);
		if (at == std::string::npos) return std::string();
		size_t p = at + k.size();
		if (p < json.size() && json[p] == '"')
		{
			const size_t e = json.find('"', p + 1);
			return e == std::string::npos ? std::string() : json.substr(p + 1, e - p - 1);
		}
		size_t e = p;
		while (e < json.size() && json[e] != ',' && json[e] != '\n' && json[e] != '}') ++e;
		return Trim(json.substr(p, e - p));
	}

	// "... motion invalidated (<cause>; <N> in total) ..." -> N (and the cause), else -1.
	inline long long InvalidationTotal(const std::string& line, std::string* cause = nullptr)
	{
		const size_t at = line.find("motion invalidated (");
		if (at == std::string::npos) return -1;
		const size_t semi = line.find("; ", at);
		const size_t total = line.find(" in total)", at);
		if (semi == std::string::npos || total == std::string::npos || total < semi) return -1;
		if (cause) *cause = line.substr(at + 20, semi - at - 20);
		return std::strtoll(line.c_str() + semi + 2, nullptr, 10);
	}

	// One capture of a picture_seq render, as the result file reports it.
	struct Capture
	{
		std::string tag;         // the picture the captured frame belongs to ("-" unknown)
		int frame = -1;          // its frame index (-1: unknown or the repeated tail)
		std::string png, json;
		unsigned long long sourceSequence = 0;
		std::string requested;   // legacy: the picture playing at the request; per-frame: = tag
		bool ok = false;
		std::string motionMode;  // hybrid_motion_mode from the sidecar ("-" if the DLL has none)
		long long estimates = -1, invalidations = -1;
	};

	// The result line: the first seven columns as before (the TrainKit reads `len >= 7`), then
	// hybrid_motion_mode, motion_estimates and motion_invalidations ("-" when unknown).
	inline std::string ResultLine(const Capture& c)
	{
		std::ostringstream s;
		s << (c.tag.empty() ? "-" : c.tag) << " " << c.frame << " " << (c.png.empty() ? "-" : c.png) << " "
			<< (c.json.empty() ? "-" : c.json) << " " << c.sourceSequence << " " << (c.requested.empty() ? "-" : c.requested)
			<< (c.ok ? " ok" : " fail") << " " << (c.motionMode.empty() ? "-" : c.motionMode) << " ";
		if (c.estimates >= 0) s << c.estimates; else s << "-";
		s << " ";
		if (c.invalidations >= 0) s << c.invalidations; else s << "-";
		return s.str();
	}

	// The motion check (job key `require_motion <share>`): after the settle, at least `share` of
	// the captures ran the grain map on vectors, no invalidation happened, and the estimate ran
	// for most source frames sent.
	// Per-frame coverage of a per_frame render (audit 2026-10-24: no silent drops): every frame of every
	// picture captured exactly once; a capture the tool cannot map to a sent frame is "unmapped";
	// waits that gave up (queue back-pressure, a capture request never taken) are counted too.
	struct Coverage
	{
		long long expected = 0, captured = 0, missing = 0, duplicates = 0, unmapped = 0, timeouts = 0;
		std::vector<std::string> examples;   // a few "tag:frame" of the missing / duplicate frames
		long long Failures() const { return missing + duplicates + unmapped + timeouts; }
	};

	// pictures: (tag, frames) in play order; caps: (tag, frame) of every capture of the measured frames
	// (settle / tail captures excluded by the caller; tag "-" = unmapped).
	inline Coverage CoverageOf(const std::vector<std::pair<std::string, int>>& pictures,
		const std::vector<std::pair<std::string, int>>& caps, long long timeouts)
	{
		Coverage c;
		c.timeouts = timeouts;
		std::map<std::pair<std::string, int>, int> count;
		for (const auto& cap : caps)
		{
			if (cap.first == "-" || cap.second < 0) { ++c.unmapped; continue; }
			++count[cap];
		}
		for (const auto& pic : pictures)
			for (int k = 0; k < pic.second; ++k)
			{
				++c.expected;
				const auto it = count.find(std::make_pair(pic.first, k));
				const int n = it == count.end() ? 0 : it->second;
				if (n >= 1) ++c.captured;
				if (n == 0) { ++c.missing; if (c.examples.size() < 6) c.examples.push_back("missing " + pic.first + ":" + std::to_string(k)); }
				if (n > 1) { c.duplicates += n - 1; if (c.examples.size() < 6) c.examples.push_back("duplicate " + pic.first + ":" + std::to_string(k)); }
			}
		return c;
	}

	// The motion check (job key `require_motion <share> [max_invalidations] [max_missing]`): after the
	// settle, at least `share` of the captures ran the grain map on vectors, at most max_invalidations
	// invalidations (from the capture sidecars), the estimate ran for most frames, and every measured
	// frame was captured exactly once (at most max_missing missing / duplicate / unmapped / timed out).
	struct MotionCheck
	{
		int captures = 0, vectors = 0, noVectors = 0, other = 0;
		long long framesSent = 0;      // source frames sent after the settle
		long long estimates = -1;      // estimates after the settle (-1 unknown)
		long long invalidations = 0;   // invalidations after the settle
		bool invalidationsFromSidecar = false;
		bool metadataKnown = false;    // the DLL writes hybrid_motion_mode
		Coverage coverage;

		double VectorShare() const { return captures ? static_cast<double>(vectors) / captures : 0.0; }
		bool Passes(double share, long long maxInvalidations, std::string& why, long long maxMissing = 0) const
		{
			std::ostringstream s;
			bool ok = true;
			if (!metadataKnown) { s << "the renderer DLL writes no hybrid_motion_mode (rebuild from fix/vpscalerender-motion or later); "; ok = false; }
			if (captures == 0) { s << "no captures after the settle; "; ok = false; }
			else if (VectorShare() < share) { s << "vectors on " << vectors << " of " << captures << " captures (< " << share << "); "; ok = false; }
			if (invalidations > maxInvalidations) { s << invalidations << " motion invalidations after the settle (> " << maxInvalidations << "); "; ok = false; }
			if (estimates >= 0 && framesSent > 0 && estimates * 10 < framesSent * 8) { s << estimates << " estimates for " << framesSent << " frames; "; ok = false; }
			if (coverage.Failures() > maxMissing)
			{
				s << "coverage: " << coverage.missing << " missing, " << coverage.duplicates << " duplicate, " << coverage.unmapped << " unmapped, "
					<< coverage.timeouts << " timed-out of " << coverage.expected << " frames (> " << maxMissing << ")";
				for (const std::string& e : coverage.examples) s << "; " << e;
				s << "; ";
				ok = false;
			}
			why = s.str();
			return ok;
		}
	};
}
