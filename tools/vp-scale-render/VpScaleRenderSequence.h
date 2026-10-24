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
	// occlusion mask itself is only applied by a sigma-delta shader; the training renders have
	// none (temporal_dither off), so it changes no output pixel. Interpolation, hiccup repair
	// and libplacebo's smooth motion stay off (they would mix or replace frames).
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

	// A renderer section is [vprenderer] or a profile [vprenderer.<name>] (not the shader
	// sections). The input-rule sections ([vprenderer.input.*]) are profiles too.
	inline bool IsRendererSection(const std::string& header)
	{
		return header == "[vprenderer]" || header.rfind("[vprenderer.", 0) == 0;
	}

	// The configuration text with the motion overrides: in every renderer section an override
	// key's existing line is dropped and the overrides follow the section header. Everything
	// else (comments, other keys, shader sections) is kept as it was. With no [vprenderer]
	// section one is added. The job's own configuration file is never changed: the tool writes
	// this text to a file of its own.
	inline std::string WithMotionOverrides(const std::string& text)
	{
		std::set<std::string> keys;
		for (const Override& o : MotionOverrides()) keys.insert(o.key);
		std::istringstream in(text);
		std::ostringstream out;
		std::string line;
		bool inRenderer = false, sawGlobal = false;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::string t = Trim(line);
			if (!t.empty() && t.front() == '[')
			{
				inRenderer = IsRendererSection(t);
				sawGlobal = sawGlobal || t == "[vprenderer]";
				out << line << "\n";
				if (inRenderer)
					for (const Override& o : MotionOverrides())
						out << o.key << ": " << o.value << "\n";
				continue;
			}
			if (inRenderer && !t.empty() && t[0] != '#' && t[0] != ';')
			{
				const size_t colon = t.find_first_of(":=");
				if (colon != std::string::npos && keys.count(Trim(t.substr(0, colon))))
					continue;
			}
			out << line << "\n";
		}
		if (!sawGlobal)
		{
			out << "[vprenderer]\n";
			for (const Override& o : MotionOverrides())
				out << o.key << ": " << o.value << "\n";
		}
		return out.str();
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
	struct MotionCheck
	{
		int captures = 0, vectors = 0, noVectors = 0, other = 0;
		long long framesSent = 0;      // source frames sent after the settle
		long long estimates = -1;      // estimates after the settle (-1 unknown)
		long long invalidations = 0;   // invalidations after the settle
		bool metadataKnown = false;    // the DLL writes hybrid_motion_mode

		double VectorShare() const { return captures ? static_cast<double>(vectors) / captures : 0.0; }
		bool Passes(double share, long long maxInvalidations, std::string& why) const
		{
			std::ostringstream s;
			bool ok = true;
			if (!metadataKnown) { s << "the renderer DLL writes no hybrid_motion_mode (rebuild from fix/vpscalerender-motion or later); "; ok = false; }
			if (captures == 0) { s << "no captures after the settle; "; ok = false; }
			else if (VectorShare() < share) { s << "vectors on " << vectors << " of " << captures << " captures (< " << share << "); "; ok = false; }
			if (invalidations > maxInvalidations) { s << invalidations << " motion invalidations after the settle (> " << maxInvalidations << "); "; ok = false; }
			if (estimates >= 0 && framesSent > 0 && estimates * 10 < framesSent * 8) { s << estimates << " estimates for " << framesSent << " frames; "; ok = false; }
			why = s.str();
			return ok;
		}
	};
}
