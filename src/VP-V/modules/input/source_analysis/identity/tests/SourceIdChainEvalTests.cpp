/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Machine-time evaluation of source identity on the SID-D identity reels (doc 78 section 9): CPU only, no GPU. SKIPS (passes without
// doing anything) unless VP_SOURCE_ID_REELS names the reels.json written by tools/eval/source_id/make_identity_reels.py.
//   VP_SOURCE_ID_REELS          path of reels.json (required to run)
//   VP_SOURCE_ID_REELS_ONLY     optional: comma list of reel-id substrings (a reel runs when its id contains any of them)
//   VP_SOURCE_ID_EVAL_OUT       output directory (default: <folder of reels.json>\eval)
//   VP_SOURCE_ID_REELS_FFMPEG   ffmpeg.exe (default VP_SOURCE_ID_EVAL_FFMPEG, then D:\tools\ffmpeg\bin\ffmpeg.exe)
//   VP_SOURCE_ID_REELS_FFMPEG_THREADS  decoder/filter threads for the ffmpeg child (default 3; the runner keeps the total <= 6)
// Each reel is decoded through ONE ffmpeg pipe: its inputs plus its stored -filter_complex (the PLAYER CHAIN: upscale kernel, USM,
// dither, 8/10-bit, crop/pad, overlays, menus, FRC, splices) ending in [vout], delivered as gray16le at the reel's full raster.
// Per frame the 10-bit luma CODE plane (y16 >> (16 - bits), bits = the segment's bit depth from the manifest, scaled to 10-bit codes
// like the SourceFrameTap plane) is offered to the optional evaluator seam (SID-C2 plugs the chain estimator in here), AutoFactors runs
// on both classical lanes (consensus OFF = today, consensus ON) and, every 0.5 s of the source clock, tile copies drive the real
// monitors exactly as SourceIdStreamingEvalTests / the render loop do. Output per run:
//   evaluations.csv  one row per analysis per lane (state, applied n, classical readings, consensus record, chain-estimator columns)
//   events.csv       engages, declared-ratio changes, ABR events, notices (per lane, frame-accurate)
//   reels_done.csv   per reel: frames expected / decoded, wall seconds, ffmpeg exit state
// No hard assertions beyond "every selected reel decoded its frames": the numbers are the result, scored by
// tools/eval/source_id/score_identity.py. Deterministic: source time = frame index / reel rate, no wall clock enters a decision.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/RealClipLibrary.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidChain
	{
		// ---- the evaluator seam (SID-C2 plugs the chain estimator in) ----
		// luma: 10-bit codes (0..1023) of the delivered raster, row stride in samples, valid only during the call.
		using FrameEvaluator = std::function<void(const uint16_t* luma, int width, int height, int strideSamples, uint64_t seq, double sourceSeconds)>;
		struct EvalSeam
		{
			FrameEvaluator frame;                                          // every decoded frame
			std::function<void(const std::string& reelId)> reelBegin;      // before the first frame of a reel
			std::function<void(const std::string& reelId)> reelEnd;        // after the last frame
			// Optional: a row for the lane "chain" at the 0.5 s analysis cadence. csvRow must be the evaluations.csv columns from `fx`
			// onward in header order (fx,fy,n_x,n_y,state,...,ms,kernel,sharpen_amount,double_n1_x,double_n1_y,active_x,active_y,
			// active_w,active_h,llr_x,llr_y; unknown = empty). State values: UNKNOWN PROVISIONAL CANDIDATE LATCHED HOLD UNSTABLE
			// ABSTAIN (see tools/eval/source_id/README.md). Return false to skip the row.
			std::function<bool(double sourceSeconds, std::string& csvRow)> chainRow;
		};
		inline EvalSeam& Seam()
		{
			static EvalSeam s;
			return s;
		}

		struct SegInfo { int start = 0, frames = 0, bits = 10; std::string regime; };
		struct ReelSpec
		{
			std::string id;
			int fpsNum = 24, fpsDen = 1;
			int width = 0, height = 0, frames = 0;
			std::vector<std::pair<std::string, std::string>> inputs;   // path, ss ("" = none)
			std::string filter;
			std::vector<SegInfo> segs;
			double Fps() const { return static_cast<double>(fpsNum) / fpsDen; }
			int SegAt(int frame) const
			{
				for (size_t i = 0; i < segs.size(); ++i)
					if (frame >= segs[i].start && frame < segs[i].start + segs[i].frames)
						return static_cast<int>(i);
				return segs.empty() ? 0 : static_cast<int>(segs.size()) - 1;
			}
		};

		inline std::string Env(const char* name) { return RealClip::Env(name); }

		inline std::string DirOf(const std::string& path)
		{
			const size_t p = path.find_last_of("\\/");
			return p == std::string::npos ? std::string(".") : path.substr(0, p);
		}

		inline std::vector<std::string> SplitCommas(const std::string& s)
		{
			std::vector<std::string> out;
			std::string cur;
			for (char c : s)
			{
				if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
				else cur.push_back(c);
			}
			if (!cur.empty()) out.push_back(cur);
			return out;
		}

		inline bool ReadFile(const std::string& path, std::string& text)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			std::ostringstream ss;
			ss << in.rdbuf();
			text = ss.str();
			return true;
		}

		inline std::vector<ReelSpec> ReadReels(const std::string& path, std::string& error)
		{
			std::vector<ReelSpec> reels;
			std::string text;
			if (!ReadFile(path, text))
			{
				error = "cannot read " + path;
				return reels;
			}
			RealClip::Json doc;
			try { doc = RealClip::Json::Parse(text); }
			catch (const std::exception& e) { error = std::string("reels.json: ") + e.what(); return reels; }
			const RealClip::Json* list = doc.Find("reels");
			if (!list || list->type != RealClip::Json::Array)
			{
				error = "reels.json has no reels array";
				return reels;
			}
			const std::string pix = doc.Str("output_pix_fmt", "gray16le");
			if (pix != "gray16le")
			{
				error = "unsupported output_pix_fmt " + pix + " (this harness reads gray16le)";
				return reels;
			}
			for (const RealClip::Json& r : list->a)
			{
				ReelSpec s;
				s.id = r.Str("id");
				const RealClip::Json* fps = r.Find("fps");
				if (fps && fps->a.size() == 2)
				{
					s.fpsNum = static_cast<int>(std::lround(fps->a[0].n));
					s.fpsDen = (std::max)(1, static_cast<int>(std::lround(fps->a[1].n)));
				}
				s.width = r.Int("width");
				s.height = r.Int("height");
				s.frames = r.Int("frames");
				s.filter = r.Str("filter_complex");
				const RealClip::Json* in = r.Find("inputs");
				if (in)
					for (const RealClip::Json& i : in->a)
						s.inputs.emplace_back(i.Str("path"), i.Str("ss"));
				const RealClip::Json* segs = r.Find("segments");
				if (segs)
					for (const RealClip::Json& g : segs->a)
					{
						SegInfo si;
						si.start = g.Int("out_start_frame");
						si.frames = g.Int("out_frames");
						si.bits = g.Int("bits", 10);
						si.regime = g.Str("regime");
						s.segs.push_back(si);
					}
				if (!s.id.empty() && s.width > 0 && s.height > 0 && s.frames > 0 && !s.inputs.empty() && !s.filter.empty())
					reels.push_back(std::move(s));
			}
			return reels;
		}

		struct Lane
		{
			SourceResolutionMonitor::Monitor monitor;
			std::string name;
			double fx = 1.0, fy = 1.0;
			uint64_t noticeSeen = 0;
			unsigned long long events = 0;
			double declared = 1.0;
			bool consensus = false;
		};

		inline double MsSince(const std::chrono::steady_clock::time_point& t0)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}

		inline std::string CsvText(const std::string& s)
		{
			std::string o = "\"";
			for (char c : s)
			{
				if (c == '"') o += "\"\"";
				else if (c == '\n' || c == '\r') o += ' ';
				else o += c;
			}
			return o + "\"";
		}
	}

	TEST_CLASS(SourceIdChainEvalTests)
	{
	public:
		TEST_METHOD(ReplayTheIdentityReelsThroughTheClassicalMonitor)
		{
			using namespace SidChain;
			const std::string reelsPath = Env("VP_SOURCE_ID_REELS");
			if (reelsPath.empty())
			{
				Logger::WriteMessage("SourceIdChainEvalTests: skipped (VP_SOURCE_ID_REELS not set)");
				return;
			}
			std::string error;
			const std::vector<ReelSpec> all = ReadReels(reelsPath, error);
			if (all.empty())
			{
				Logger::WriteMessage(("SourceIdChainEvalTests: no reels (" + error + ")").c_str());
				Assert::Fail(L"VP_SOURCE_ID_REELS is set but holds no usable reels");
				return;
			}
			std::string ffmpeg = Env("VP_SOURCE_ID_REELS_FFMPEG");
			if (ffmpeg.empty()) ffmpeg = Env("VP_SOURCE_ID_EVAL_FFMPEG");
			if (ffmpeg.empty()) ffmpeg = "D:\\tools\\ffmpeg\\bin\\ffmpeg.exe";
			std::string threads = Env("VP_SOURCE_ID_REELS_FFMPEG_THREADS");
			if (threads.empty()) threads = "3";
			std::string outDir = Env("VP_SOURCE_ID_EVAL_OUT");
			if (outDir.empty()) outDir = DirOf(reelsPath) + "\\eval";
			CreateDirectoryA(outDir.c_str(), nullptr);
			const std::string fcDir = outDir + "\\fc";
			CreateDirectoryA(fcDir.c_str(), nullptr);
			const std::vector<std::string> only = SplitCommas(Env("VP_SOURCE_ID_REELS_ONLY"));

			std::ofstream evals(outDir + "\\evaluations.csv"), events(outDir + "\\events.csv"), done(outDir + "\\reels_done.csv");
			evals << "reel,lane,out_frame,t,seg,bits,fx,fy,n_x,n_y,state,declared,conf,frame_quality,noise,classical_quality,tier,level,rec_ratio_h,rec_ratio_v,gen,reason,ms,"
				"kernel,sharpen_amount,double_n1_x,double_n1_y,active_x,active_y,active_w,active_h,llr_x,llr_y\n";
			events << "reel,lane,t,out_frame,seg,type,a,b,detail\n";
			done << "reel,frames_expected,frames_decoded,wall_s,state\n";
			evals << std::fixed << std::setprecision(6);
			events << std::fixed << std::setprecision(6);
			done << std::fixed << std::setprecision(1);

			int ran = 0, failures = 0;
			for (const ReelSpec& reel : all)
			{
				if (!only.empty())
				{
					bool hit = false;
					for (const std::string& o : only)
						if (reel.id.find(o) != std::string::npos) { hit = true; break; }
					if (!hit)
						continue;
				}
				const auto wall0 = std::chrono::steady_clock::now();
				const std::string fcPath = fcDir + "\\" + reel.id + ".fc.txt";
				{
					std::ofstream fc(fcPath, std::ios::binary);
					fc << reel.filter;
				}
				std::ostringstream cmd;
				cmd << "\"\"" << ffmpeg << "\" -v error -nostdin -filter_threads " << threads << " -filter_complex_threads 1";
				for (const auto& in : reel.inputs)
				{
					cmd << " -threads " << threads;
					if (!in.second.empty())
						cmd << " -ss " << in.second;
					cmd << " -i \"" << in.first << "\"";
				}
				cmd << " -/filter_complex \"" << fcPath << "\" -map \"[vout]\" -pix_fmt gray16le -f rawvideo -\"";
				FILE* pipe = _popen(cmd.str().c_str(), "rb");
				if (!pipe)
				{
					done << reel.id << ',' << reel.frames << ",0,0,popen_failed\n";
					++failures;
					continue;
				}

				Lane off, on;
				off.name = "off";
				on.name = "on";
				on.consensus = true;
				on.monitor.SetSourceIdConsensus(true);
				Lane* lanes[2] = { &off, &on };
				const int W = reel.width, H = reel.height;
				const double black = 64.0 / 1023.0;
				const double fps = reel.Fps();
				double lastSubmit = -1e9;
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H), codes(static_cast<size_t>(W) * H);
				int g = 0;
				if (Seam().reelBegin)
					Seam().reelBegin(reel.id);
				while (fread(raw.data(), sizeof(uint16_t), raw.size(), pipe) == raw.size())
				{
					const int seg = reel.SegAt(g);
					const int bits = reel.segs.empty() ? 10 : reel.segs[seg].bits;
					const int shift = 16 - bits;
					const int up = bits < 10 ? 10 - bits : 0, down = bits > 10 ? bits - 10 : 0;
					for (size_t i = 0; i < raw.size(); ++i)
						codes[i] = static_cast<uint16_t>(((raw[i] >> shift) >> down) << up);
					const double t = g / fps;
					const uint64_t seq = static_cast<uint64_t>(g) + 1;
					if (Seam().frame)
						Seam().frame(codes.data(), W, H, W, seq, t);
					if (t - lastSubmit >= 0.5 - 1e-9)
					{
						lastSubmit = t;
						double ms[2] = { 0.0, 0.0 };
						for (int li = 0; li < 2; ++li)
						{
							Lane* l = lanes[li];
							const auto t0 = std::chrono::steady_clock::now();
							const int tile = l->monitor.TileFor(H);
							int base = 0;
							const std::vector<SourceResolution::TilePlace> places =
								SourceResolution::PlaceTiles(0, 0, W, H, tile, l->monitor.DenseGrid(), &base);
							std::vector<float> values(places.size() * static_cast<size_t>(tile) * tile);
							std::vector<float> tx, ty;
							for (size_t p = 0; p < places.size(); ++p)
							{
								const SourceResolution::TilePlace& pl = places[p];
								for (int y = 0; y < tile; ++y)
								{
									const int sy = (std::min)(H - 1, pl.y0 + y);
									const uint16_t* row = codes.data() + static_cast<size_t>(sy) * W;
									float* dst = &values[(p * tile + y) * tile];
									for (int x = 0; x < tile; ++x)
										dst[x] = static_cast<float>(row[(std::min)(W - 1, pl.x0 + x)]) / 1023.0f;
								}
								tx.push_back(pl.centreX);
								ty.push_back(pl.centreY);
							}
							l->monitor.AnalyseTilesForTest(values, static_cast<int>(places.size()), t, W, H, tx, ty, base, tile, black, seq);
							ms[li] = MsSince(t0);
						}
						for (int li = 0; li < 2; ++li)
						{
							Lane* l = lanes[li];
							const SourceResolutionMonitor::Snapshot s = l->monitor.Read();
							SourceIdentity::Record rec;
							const bool have = l->consensus && l->monitor.SourceIdRecord(rec);
							const bool engaged = l->fx < 1.0 - 1e-9 || l->fy < 1.0 - 1e-9;
							const char* state = engaged ? "LATCHED" : (s.declaredRatio < 0.999 ? "CANDIDATE" : "UNKNOWN");
							const long nx = std::lround(l->fx * W), ny = std::lround(l->fy * H);
							evals << reel.id << ',' << l->name << ',' << g << ',' << t << ',' << seg << ',' << bits << ',' << l->fx << ',' << l->fy << ','
								<< nx << ',' << ny << ',' << state << ',' << s.declaredRatio << ',' << s.confidence << ',' << s.frameQuality << ','
								<< s.frameNoise << ',' << s.bitrateQuality << ',' << (have ? rec.tier : -2) << ',' << (have ? rec.level : -1.0f) << ','
								<< (have ? rec.ratioH : -1.0f) << ',' << (have ? rec.ratioV : -1.0f) << ',' << (have ? rec.consensusGen : 0ull) << ','
								<< (have ? SourceIdentity::ReasonText(static_cast<SourceIdentity::ReasonCode>(rec.reason[static_cast<int>(SourceIdentity::Field::Quality)])) : "none")
								<< ',' << ms[li] << ",,,,,,,,,,\n";
							if (std::fabs(s.declaredRatio - l->declared) > 1e-9)
							{
								events << reel.id << ',' << l->name << ',' << t << ',' << g << ',' << seg << ",declared," << l->declared << ',' << s.declaredRatio << ",\n";
								l->declared = s.declaredRatio;
							}
							if (have && l->monitor.SourceIdEvents() != l->events)
							{
								l->events = l->monitor.SourceIdEvents();
								events << reel.id << ',' << l->name << ',' << t << ',' << g << ',' << seg << ",abr," << (rec.abrDirection == 1 ? "drop" : "rise") << ','
									<< rec.abrMagnitude << ",\"time_s=" << rec.abrTimeUs / 1e6 << " ratio_step=" << rec.abrRatioStep << " conf=" << rec.abrConfidence << "\"\n";
							}
							std::string text;
							if (l->consensus && l->monitor.TakeSourceIdNotice(l->noticeSeen, text))
								events << reel.id << ',' << l->name << ',' << t << ',' << g << ',' << seg << ",notice,,," << CsvText(text) << '\n';
						}
						if (Seam().chainRow)
						{
							std::string row;
							if (Seam().chainRow(t, row))
								evals << reel.id << ",chain," << g << ',' << t << ',' << seg << ',' << bits << ',' << row << '\n';
						}
					}
					for (int li = 0; li < 2; ++li)
					{
						Lane& l = *lanes[li];
						std::string why;
						double nx = 1.0, ny = 1.0;
						l.monitor.AutoFactors(W, H, 0.8, why, nx, ny, t, seq);
						if (std::fabs(nx - l.fx) > 1e-9 || std::fabs(ny - l.fy) > 1e-9)
						{
							events << reel.id << ',' << l.name << ',' << t << ',' << g << ',' << seg << ",engage," << l.fy << ',' << ny << ',' << CsvText(why) << '\n';
							l.fx = nx;
							l.fy = ny;
						}
					}
					++g;
				}
				const int exitCode = _pclose(pipe);
				if (Seam().reelEnd)
					Seam().reelEnd(reel.id);
				const bool ok = (g == reel.frames) && exitCode == 0;
				if (!ok)
					++failures;
				done << reel.id << ',' << reel.frames << ',' << g << ',' << MsSince(wall0) / 1000.0 << ','
					 << (ok ? "ok" : (g == 0 ? "no_frames" : (g != reel.frames ? "frame_count_mismatch" : "ffmpeg_exit")))
					 << '\n';
				evals.flush();
				events.flush();
				done.flush();
				Logger::WriteMessage((reel.id + ": " + std::to_string(g) + "/" + std::to_string(reel.frames) + " frames").c_str());
				++ran;
			}
			Logger::WriteMessage(("SourceIdChainEvalTests: " + std::to_string(ran) + " reels, " + std::to_string(failures) + " failures; output " + outDir).c_str());
			Assert::IsTrue(ran > 0, L"no reel matched VP_SOURCE_ID_REELS_ONLY");
			Assert::AreEqual(0, failures, L"a reel did not decode all of its frames");
		}
	};
}
