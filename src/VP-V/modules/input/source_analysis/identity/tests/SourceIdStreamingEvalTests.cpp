/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Machine-time evaluation of P14 on the streaming suite (tools/eval/suites/streaming_*.json): CPU only, no GPU. SKIPS (passes
// without doing anything) unless VP_SOURCE_ID_EVAL_DIR names a folder holding reels.tsv, written by
// tools/eval/source_id/make_abr_reels.py. Each reel is a list of segments of the suite's decoded rung files (the same content
// timeline at different rungs, spliced at IDR points like a player's ABR switch). The test decodes them with ffmpeg (10-bit luma),
// feeds tile copies every 0.5 s of the frame clock to TWO real source-resolution monitors - consensus off (today) and consensus
// on - exactly as the render loop does (AnalyseTilesForTest + AutoFactors on every frame) and writes CSV traces that
// tools/eval/source_id/replay.py scores:
//   frames.csv   per analysed frame: frame quality, noise, classical bitrate quality, tier, level, consensus axes, reasons
//   events.csv   every ABR event: detection time and the boundary time
//   engages.csv  every change of the re-base factor, per lane
//   notices.csv  every fired one-time notice
// No hard assertions beyond "it ran": the numbers are the result (RESULTS.md), not a pass/fail gate.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidEval
	{
		struct Segment { std::string path; int start = 0, end = 0; };
		struct Reel
		{
			std::string id, kind;
			double fps = 24.0;
			int width = 0, height = 0;
			std::vector<Segment> segments;
		};

		inline std::string Env(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			if (_dupenv_s(&v, &n, name) != 0 || !v)
				return std::string();
			std::string s(v);
			free(v);
			return s;
		}

		inline std::vector<std::string> Split(const std::string& s, char sep)
		{
			std::vector<std::string> out;
			std::string cur;
			for (char c : s)
			{
				if (c == sep) { out.push_back(cur); cur.clear(); }
				else cur.push_back(c);
			}
			out.push_back(cur);
			return out;
		}

		inline std::vector<Reel> ReadManifest(const std::string& path)
		{
			std::vector<Reel> reels;
			std::ifstream in(path);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const std::vector<std::string> f = Split(line, '\t');
				if (f.size() < 6 || f[0] == "id")
					continue;
				Reel r;
				r.id = f[0];
				r.kind = f[1];
				r.fps = atof(f[2].c_str());
				r.width = atoi(f[3].c_str());
				r.height = atoi(f[4].c_str());
				for (const std::string& seg : Split(f[5], ';'))
				{
					const size_t at = seg.rfind('@');
					const size_t dash = seg.find('-', at);
					if (at == std::string::npos || dash == std::string::npos) continue;
					Segment s;
					s.path = seg.substr(0, at);
					s.start = atoi(seg.substr(at + 1, dash - at - 1).c_str());
					s.end = atoi(seg.substr(dash + 1).c_str());
					r.segments.push_back(s);
				}
				if (r.fps > 0 && r.width > 0 && r.height > 0 && !r.segments.empty())
					reels.push_back(r);
			}
			return reels;
		}

		// Decodes a segment's frames (10-bit luma) through ffmpeg and calls back with each frame's samples.
		template <class Fn>
		inline bool DecodeSegment(const std::string& ffmpeg, const Segment& seg, int width, int height, Fn&& fn)
		{
			std::ostringstream cmd;
			cmd << "\"\"" << ffmpeg << "\" -v error -i \"" << seg.path << "\" -an -vf trim=start_frame=" << seg.start
				<< ":end_frame=" << seg.end << " -pix_fmt gray10le -f rawvideo -\"";
			FILE* pipe = _popen(cmd.str().c_str(), "rb");
			if (!pipe)
				return false;
			std::vector<uint16_t> frame(static_cast<size_t>(width) * height);
			while (fread(frame.data(), sizeof(uint16_t), frame.size(), pipe) == frame.size())
				fn(frame);
			_pclose(pipe);
			return true;
		}

		struct Lane
		{
			SourceResolutionMonitor::Monitor monitor;
			double fx = 1.0, fy = 1.0;
			uint64_t noticeSeen = 0;
			unsigned long long events = 0;
			bool consensus = false;
		};
	}

	TEST_CLASS(SourceIdStreamingEvalTests)
	{
	public:
		TEST_METHOD(ReplayTheAbrReelsThroughTheRealMonitor)
		{
			using namespace SidEval;
			const std::string dir = Env("VP_SOURCE_ID_EVAL_DIR");
			if (dir.empty())
			{
				Logger::WriteMessage("SourceIdStreamingEvalTests: skipped (VP_SOURCE_ID_EVAL_DIR not set)");
				return;
			}
			const std::string manifest = dir + "\\reels.tsv";
			const std::vector<Reel> all = ReadManifest(manifest);
			if (all.empty())
			{
				Logger::WriteMessage("SourceIdStreamingEvalTests: skipped (no reels.tsv)");
				return;
			}
			std::string ffmpeg = Env("VP_SOURCE_ID_EVAL_FFMPEG");
			if (ffmpeg.empty())
				ffmpeg = "D:\\tools\\ffmpeg-full\\bin\\ffmpeg.exe";
			const std::string only = Env("VP_SOURCE_ID_EVAL_ONLY");

			std::ofstream frames(dir + "\\frames.csv"), events(dir + "\\events.csv"), engages(dir + "\\engages.csv"), notices(dir + "\\notices.csv");
			frames << "reel,t,frame_quality,noise,classical_quality,tier,level,ratioH,ratioV,gen,quality_reason,declared,conf\n";
			events << "reel,detect_t,event_t,direction,magnitude,ratio_step,confidence\n";
			engages << "reel,lane,t,from_y,to_y,why\n";
			notices << "reel,t,text\n";

			int done = 0;
			for (const Reel& reel : all)
			{
				if (!only.empty() && reel.id.find(only) == std::string::npos)
					continue;
				Lane off, on;
				on.consensus = true;
				on.monitor.SetSourceIdConsensus(true);
				Lane* lanes[2] = { &off, &on };
				const double black = 64.0 / 1023.0;
				double lastSubmit = -1e9;
				int g = 0;
				for (const Segment& seg : reel.segments)
				{
					DecodeSegment(ffmpeg, seg, reel.width, reel.height, [&](const std::vector<uint16_t>& luma) {
						const double t = g / reel.fps;
						const uint64_t seq = static_cast<uint64_t>(g) + 1;
						if (t - lastSubmit >= 0.5 - 1e-9)
						{
							lastSubmit = t;
							for (Lane* l : lanes)
							{
								const int tile = l->monitor.TileFor(reel.height);
								int base = 0;
								const std::vector<SourceResolution::TilePlace> places =
									SourceResolution::PlaceTiles(0, 0, reel.width, reel.height, tile, l->monitor.DenseGrid(), &base);
								std::vector<float> values(places.size() * static_cast<size_t>(tile) * tile);
								std::vector<float> tx, ty;
								for (size_t p = 0; p < places.size(); ++p)
								{
									const SourceResolution::TilePlace& pl = places[p];
									for (int y = 0; y < tile; ++y)
									{
										const int sy = (std::min)(reel.height - 1, pl.y0 + y);
										const uint16_t* row = luma.data() + static_cast<size_t>(sy) * reel.width;
										float* dst = &values[(p * tile + y) * tile];
										for (int x = 0; x < tile; ++x)
											dst[x] = static_cast<float>(row[(std::min)(reel.width - 1, pl.x0 + x)]) / 1023.0f;
									}
									tx.push_back(pl.centreX);
									ty.push_back(pl.centreY);
								}
								l->monitor.AnalyseTilesForTest(values, static_cast<int>(places.size()), t, reel.width, reel.height, tx, ty, base,
									tile, black, seq);
							}
							// One row per analysed frame of the consensus lane.
							const SourceResolutionMonitor::Snapshot s = on.monitor.Read();
							SourceIdentity::Record rec;
							const bool have = on.monitor.SourceIdRecord(rec);
							frames << reel.id << ',' << t << ',' << s.frameQuality << ',' << s.frameNoise << ',' << s.bitrateQuality << ','
								<< (have ? rec.tier : -2) << ',' << (have ? rec.level : -1.0f) << ',' << (have ? rec.ratioH : -1.0f) << ','
								<< (have ? rec.ratioV : -1.0f) << ',' << (have ? rec.consensusGen : 0ull) << ','
								<< (have ? SourceIdentity::ReasonText(static_cast<SourceIdentity::ReasonCode>(rec.reason[static_cast<int>(SourceIdentity::Field::Quality)])) : "none")
								<< ',' << s.declaredRatio << ',' << s.confidence << '\n';
							if (have && on.monitor.SourceIdEvents() != on.events)
							{
								on.events = on.monitor.SourceIdEvents();
								events << reel.id << ',' << t << ',' << rec.abrTimeUs / 1e6 << ',' << (rec.abrDirection == 1 ? "drop" : "rise") << ','
									<< rec.abrMagnitude << ',' << rec.abrRatioStep << ',' << rec.abrConfidence << '\n';
							}
							std::string text;
							if (on.monitor.TakeSourceIdNotice(on.noticeSeen, text))
								notices << reel.id << ',' << t << ",\"" << text << "\"\n";
						}
						for (int i = 0; i < 2; ++i)
						{
							Lane& l = *lanes[i];
							std::string why;
							double nx = 1.0, ny = 1.0;
							l.monitor.AutoFactors(reel.width, reel.height, 0.8, why, nx, ny, t, seq);
							if (std::fabs(nx - l.fx) > 1e-9 || std::fabs(ny - l.fy) > 1e-9)
							{
								engages << reel.id << ',' << (i == 0 ? "off" : "on") << ',' << t << ',' << l.fy << ',' << ny << ",\"" << why << "\"\n";
								l.fx = nx;
								l.fy = ny;
							}
						}
						++g;
					});
				}
				++done;
				Logger::WriteMessage((reel.id + ": " + std::to_string(g) + " frames").c_str());
			}
			Logger::WriteMessage(("SourceIdStreamingEvalTests: " + std::to_string(done) + " reels").c_str());
			Assert::IsTrue(done >= 0);
		}
	};
}
