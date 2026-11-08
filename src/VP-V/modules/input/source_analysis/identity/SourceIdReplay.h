// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Source-ID replay (input.source_analysis (identity/), doc 78 SID-B): feeds the rows of a source-resolution history CSV
// (SourceHistoryLog.h; one file or the files of a rotated session concatenated) through the identity Pipeline in source
// time, so a captured session can be re-run against changed thresholds and give the same trace every time.
//
// What a "reading" row carries (columns by NAME, never by position): the classical snapshot behind the votes (declared,
// confidence, axis_x / axis_y / axis_conf, kernel_id / kernel_votes / kernel_agree, bitrate_quality, detail_above) and the
// QualitySample (frame_ratio, apl, blank, cut, fade, frame_quality); t_src and seq give the source time and sequence. The
// frame quality is the recorded one (Pipeline::Push qualityOverride), the relative-noise tiles are not stored. A
// "discontinuity" row resets the pipeline. No clock, no thread, no randomness.
#include <VP-V/modules/input/source_analysis/identity/SourceIdPipeline.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace SourceIdentity
{
	// One CSV line split into fields ("..." quoting, "" for a quote). No newline inside a field (the writer never emits one).
	inline std::vector<std::string> SplitCsvLine(const std::string& line)
	{
		std::vector<std::string> out;
		std::string cur;
		bool quoted = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			const char c = line[i];
			if (quoted)
			{
				if (c == '"')
				{
					if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
					else quoted = false;
				}
				else
					cur += c;
			}
			else if (c == '"' && cur.empty())
				quoted = true;
			else if (c == ',')
			{
				out.push_back(cur);
				cur.clear();
			}
			else if (c != '\r')
				cur += c;
		}
		out.push_back(cur);
		return out;
	}

	struct ReplayStep
	{
		int64_t timeUs = 0;
		uint64_t seq = 0;
		bool discontinuity = false;
		uint32_t changedMask = 0;        // fields the consensus changed on this row
		float ratioH = 1.0f, ratioV = 1.0f;
		int tier = -1;
		uint32_t consensusGen = 0;
		bool Changed(Field f) const { return (changedMask & FieldBit(f)) != 0; }
	};

	class SrmReplay
	{
	public:
		// Replays `text` through `pipeline`; `steps` gets one entry per replayed row. False + `error` when the header lacks a
		// column the replay needs. `nativeHeight` is only the notice wording's (not recorded).
		static bool Run(const std::string& text, Pipeline& pipeline, std::vector<ReplayStep>& steps, std::string& error, int nativeHeight = 1080)
		{
			steps.clear();
			error.clear();
			std::map<std::string, size_t> col;
			bool haveHeader = false;
			uint64_t lastSeq = 0;
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty())
					continue;
				const std::vector<std::string> f = SplitCsvLine(line);
				if (f.size() >= 2 && f[0] == "time" && f[1] == "event")
				{
					// A header (the first line of every file of a rotated session).
					col.clear();
					for (size_t i = 0; i < f.size(); ++i)
						col[f[i]] = i;
					haveHeader = true;
					for (const char* need : { "event", "t_src", "seq", "declared", "confidence", "detail_above", "frame_quality" })
						if (col.find(need) == col.end())
						{
							error = std::string("the history header has no column '") + need + "'";
							return false;
						}
					continue;
				}
				if (!haveHeader)
				{
					error = "no header line before the first row";
					return false;
				}
				const std::string& event = Get(f, col, "event");
				if (event == "discontinuity")
				{
					pipeline.Discontinuity(lastSeq + 1, 1);
					ReplayStep s;
					s.discontinuity = true;
					s.seq = lastSeq + 1;
					const Record* held = nullptr;
					Record r;
					if (pipeline.Held().ReadLatest(0, r)) held = &r;
					if (held) { s.ratioH = held->ratioH; s.ratioV = held->ratioV; s.tier = held->tier; s.consensusGen = held->consensusGen; }
					steps.push_back(s);
					continue;
				}
				if (event != "reading")
					continue;
				FrameInput fin;
				fin.seq = static_cast<uint64_t>(Num(f, col, "seq", 0.0));
				fin.timeUs = static_cast<int64_t>(std::llround(Num(f, col, "t_src", 0.0) * 1e6));
				fin.streamKey = 1;
				fin.classical.reading = true;
				fin.classical.declaredRatio = Num(f, col, "declared", 1.0);
				fin.classical.confidence = Num(f, col, "confidence", 0.0);
				fin.classical.axisRatioX = Num(f, col, "axis_x", 1.0);
				fin.classical.axisRatioY = Num(f, col, "axis_y", 1.0);
				fin.classical.axisConfidence = Num(f, col, "axis_conf", 0.0);
				fin.classical.kernel = static_cast<int>(Num(f, col, "kernel_id", -1.0));
				fin.classical.kernelVotes = static_cast<int>(Num(f, col, "kernel_votes", 0.0));
				fin.classical.kernelAgree = static_cast<int>(Num(f, col, "kernel_agree", 0.0));
				fin.classical.bitrateQuality = Num(f, col, "bitrate_quality", 1.0);
				fin.classical.detailAbove = Num(f, col, "detail_above", 0.0) != 0.0;
				fin.blank = Num(f, col, "blank", 0.0) != 0.0;
				fin.cut = Num(f, col, "cut", 0.0) != 0.0;
				fin.fade = Num(f, col, "fade", 0.0) != 0.0;
				fin.apl = Num(f, col, "apl", 0.5);
				fin.deliveredRatio = fin.blank ? 0.0 : Num(f, col, "frame_ratio", 1.0);
				fin.nativeHeight = nativeHeight;
				const double quality = Num(f, col, "frame_quality", 1.0);
				const Record rec = pipeline.Push(fin, &quality);
				lastSeq = fin.seq;
				ReplayStep s;
				s.timeUs = fin.timeUs;
				s.seq = fin.seq;
				s.changedMask = pipeline.LastResult().changedMask;
				s.ratioH = rec.ratioH;
				s.ratioV = rec.ratioV;
				s.tier = rec.tier;
				s.consensusGen = rec.consensusGen;
				steps.push_back(s);
			}
			if (!haveHeader)
			{
				error = "no header line";
				return false;
			}
			return true;
		}

		// One line per step; the same file always gives the same text.
		static std::string Trace(const std::vector<ReplayStep>& steps)
		{
			std::ostringstream o;
			o.setf(std::ios::fixed);
			o.precision(4);
			for (const ReplayStep& s : steps)
				o << (s.discontinuity ? "D " : "R ") << s.timeUs << ' ' << s.seq << " rH=" << s.ratioH << " rV=" << s.ratioV << " tier=" << s.tier
					<< " mask=" << s.changedMask << " gen=" << s.consensusGen << '\n';
			return o.str();
		}

	private:
		static const std::string& Get(const std::vector<std::string>& f, const std::map<std::string, size_t>& col, const char* name)
		{
			static const std::string empty;
			const auto it = col.find(name);
			return it != col.end() && it->second < f.size() ? f[it->second] : empty;
		}
		static double Num(const std::vector<std::string>& f, const std::map<std::string, size_t>& col, const char* name, double fallback)
		{
			const std::string& s = Get(f, col, name);
			if (s.empty())
				return fallback;
			char* end = nullptr;
			const double v = std::strtod(s.c_str(), &end);
			return end == s.c_str() ? fallback : v;
		}
	};
}
