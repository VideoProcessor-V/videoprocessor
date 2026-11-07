/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Machine-time evaluation of source identity on the SID-D identity reels (doc 78 section 9): CPU only, no GPU. SKIPS (passes without
// doing anything) unless VP_SOURCE_ID_REELS names the reels.json written by tools/eval/source_id/make_identity_reels.py.
//   VP_SOURCE_ID_REELS          path of reels.json (required to run)
//   VP_SOURCE_ID_REELS_ONLY     optional: comma list of reel-id substrings (a reel runs when its id contains any of them)
//   VP_SOURCE_ID_EVAL_OUT       output directory (default: <folder of reels.json>\eval)
//   VP_SOURCE_ID_EVAL_FORCE     1 = redo reels that already completed (default: completed reels are skipped, so a run resumes)
//   VP_SOURCE_ID_REELS_FFMPEG   ffmpeg.exe (default VP_SOURCE_ID_EVAL_FFMPEG, then D:\tools\ffmpeg\bin\ffmpeg.exe)
//   VP_SOURCE_ID_REELS_FFMPEG_THREADS  decoder threads PER INPUT of the ffmpeg child (default 2; the filter graph gets 2 threads, 1 when it
//                               contains the noise filter, for determinism). The runner pins the whole tree to 4 logical processors.
// Each reel is decoded through ONE ffmpeg pipe: its inputs plus its stored -filter_complex (the PLAYER CHAIN: upscale kernel, USM,
// dither, 8/10-bit, crop/pad, overlays, menus, FRC, splices) ending in [vout], delivered as gray16le at the reel's full raster.
// Per frame the 10-bit luma CODE plane (y16 >> (16 - bits), bits = the segment's bit depth from the manifest, scaled to 10-bit codes
// like the SourceFrameTap plane) is offered to the evaluator seam (ChainEvalSeam.h: SID-C2 registers the chain estimator there without
// touching this file), AutoFactors runs on both classical lanes (consensus OFF = today, consensus ON) and, every 0.5 s of the source
// clock, tile copies drive the real monitors exactly as SourceIdStreamingEvalTests / the render loop do.
// Output: ONE FILE SET PER REEL, written atomically (<out>\reels\<reel>.tmp renamed to <out>\reels\<reel> when the reel is complete):
//   evaluations.csv  one row per analysis per lane (state, applied n, classical readings, consensus record, chain-estimator columns)
//   events.csv       engages, declared-ratio changes, ABR events (with the boundary time), notices, chain events (per lane)
//   done.csv         frames expected / decoded, wall seconds, ffmpeg stderr lines, state
//   ffmpeg_stderr.txt
// plus <out>\reels_done.csv (one appended row per finished reel). A restart skips completed reels. Between reels the harness honours
// <out>\STOP (ends the run cleanly), pauses while D:\codex\build-slots\PRIORITY exists (integration builds first), warns once about a
// D:\codex\GPU-QUIET marker, and stops when the output drive has less than 40 GB free.
// No hard assertions beyond "every selected reel decoded its frames": the numbers are the result, scored by
// tools/eval/source_id/score_identity.py. Deterministic: source time = frame index / reel rate, no wall clock enters a decision.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/RealClipLibrary.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include "ChainEvalSeam.h"

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

	namespace SidChain
	{
		inline bool FileExistsA(const std::string& p)
		{
			const DWORD a = GetFileAttributesA(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES;
		}

		inline double FreeGb(const std::string& dir)
		{
			ULARGE_INTEGER freeBytes;
			if (!GetDiskFreeSpaceExA(dir.c_str(), &freeBytes, nullptr, nullptr))
				return 1e9;       // unknown: do not stop
			return static_cast<double>(freeBytes.QuadPart) / 1073741824.0;
		}

		inline int CountLines(const std::string& path)
		{
			std::ifstream in(path);
			std::string line;
			int n = 0;
			while (std::getline(in, line))
				if (!line.empty())
					++n;
			return n;
		}

		inline bool ReelDoneOk(const std::string& finalDir)
		{
			std::string text;
			if (!ReadFile(finalDir + "\\done.csv", text))
				return false;
			return text.find(",ok") != std::string::npos;
		}

		inline void RemoveTree(const std::string& dir)
		{
			const char* names[] = { "evaluations.csv", "events.csv", "done.csv", "ffmpeg_stderr.txt" };
			for (const char* n : names)
				DeleteFileA((dir + "\\" + n).c_str());
			RemoveDirectoryA(dir.c_str());
		}

		// Fields written empty when unknown.
		inline void Num(std::ostream& o, double v, bool known)
		{
			if (known)
				o << v;
		}

		inline std::string StateToken(const std::string& s)
		{
			return s.empty() ? std::string("UNKNOWN") : s;
		}

		// evaluations.csv columns after `bits` for a classical lane have this many trailing empty chain columns.
		static const char* kEvalHeader =
			"reel,lane,out_frame,t,seg,bits,fx,fy,n_x,n_y,state,declared,conf,frame_quality,noise,classical_quality,tier,level,rec_ratio_h,rec_ratio_v,gen,"
			"reason,reason_h,ms,cuts,latched_axes,kernel,sharpen_amount,double_n1_x,double_n1_y,active_x,active_y,active_w,active_h,measured_n_x,measured_n_y,"
			"state_x,state_y,llr_x,llr_y,power_x,power_y,spatial_x,spatial_y,reason_bits_x,reason_bits_y,reason_x,reason_y\n";
		static const char* kClassicalChainTail = ",,,,,,,,,,,,,,,,,,,,,,\n";   // 22 empty columns: kernel .. reason_y
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
			ApplyRegistrations();
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
			if (threads.empty()) threads = "2";
			std::string outDir = Env("VP_SOURCE_ID_EVAL_OUT");
			if (outDir.empty()) outDir = DirOf(reelsPath) + "\\eval";
			const bool force = Env("VP_SOURCE_ID_EVAL_FORCE") == "1";
			CreateDirectoryA(outDir.c_str(), nullptr);
			const std::string reelsDir = outDir + "\\reels";
			CreateDirectoryA(reelsDir.c_str(), nullptr);
			const std::vector<std::string> only = SplitCommas(Env("VP_SOURCE_ID_REELS_ONLY"));
			const std::string doneIndex = outDir + "\\reels_done.csv";
			const bool newIndex = !FileExistsA(doneIndex);
			std::ofstream index(doneIndex, std::ios::app);
			if (newIndex)
				index << "reel,frames_expected,frames_decoded,wall_s,ffmpeg_stderr_lines,state\n";

			int ran = 0, skipped = 0, failures = 0;
			bool stopped = false, warnedGpu = false;
			std::string stopWhy;
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
				const std::string finalDir = reelsDir + "\\" + reel.id;
				const std::string tmpDir = finalDir + ".tmp";
				if (!force && ReelDoneOk(finalDir))
				{
					++skipped;
					continue;
				}
				// ---- between reels: STOP file, PRIORITY pause, disk floor ----
				for (;;)
				{
					if (FileExistsA(outDir + "\\STOP")) { stopped = true; stopWhy = "STOP file"; break; }
					if (FreeGb(outDir) < 40.0)
					{
						stopped = true;
						stopWhy = "output drive below 40 GB free";
						std::ofstream(outDir + "\\STOPPED_LOW_DISK.txt") << stopWhy << "\n";
						++failures;
						break;
					}
					if (FileExistsA("D:\\codex\\GPU-QUIET") && !warnedGpu)
					{
						warnedGpu = true;
						Logger::WriteMessage("GPU-QUIET marker present: continuing (CPU only), PRIORITY would pause the run");
					}
					if (!FileExistsA("D:\\codex\\build-slots\\PRIORITY"))
						break;
					Logger::WriteMessage("PRIORITY marker present: paused (integration builds first)");
					Sleep(60000);
				}
				if (stopped)
					break;

				RemoveTree(tmpDir);
				CreateDirectoryA(tmpDir.c_str(), nullptr);
				const auto wall0 = std::chrono::steady_clock::now();
				const std::string fcPath = tmpDir + "\\graph.fc.txt";
				{
					std::ofstream fc(fcPath, std::ios::binary);
					fc << reel.filter;
				}
				const std::string errPath = tmpDir + "\\ffmpeg_stderr.txt";
				// threads: decoders per input, the filter graph 2 (1 with the noise filter: its output must not depend on slicing)
				const std::string fgThreads = reel.filter.find("noise=") != std::string::npos ? "1" : "2";
				std::ostringstream cmd;
				cmd << "\"\"" << ffmpeg << "\" -v error -nostdin -filter_threads " << fgThreads << " -filter_complex_threads " << fgThreads;
				for (const auto& in : reel.inputs)
				{
					cmd << " -threads " << threads;
					if (!in.second.empty())
						cmd << " -ss " << in.second;
					cmd << " -i \"" << in.first << "\"";
				}
				cmd << " -/filter_complex \"" << fcPath << "\" -map \"[vout]\" -pix_fmt gray16le -f rawvideo - 2> \"" << errPath << "\"\"";
				FILE* pipe = _popen(cmd.str().c_str(), "rb");
				std::ofstream evals(tmpDir + "\\evaluations.csv"), events(tmpDir + "\\events.csv"), done(tmpDir + "\\done.csv");
				evals << kEvalHeader;
				events << "reel,lane,t,event_t,out_frame,seg,type,a,b,detail\n";
				done << "reel,frames_expected,frames_decoded,wall_s,ffmpeg_stderr_lines,state\n";
				evals << std::fixed << std::setprecision(6);
				events << std::fixed << std::setprecision(6);
				done << std::fixed << std::setprecision(1);
				if (!pipe)
				{
					done << reel.id << ',' << reel.frames << ",0,0,0,popen_failed\n";
					evals.close(); events.close(); done.close();
					MoveFileExA(tmpDir.c_str(), (finalDir + ".failed").c_str(), MOVEFILE_REPLACE_EXISTING);
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
					const double t = g / fps;
					const uint64_t seq = static_cast<uint64_t>(g) + 1;
					const bool due = t - lastSubmit >= 0.5 - 1e-9;
					if (due || Seam().frame)
					{
						// 10-bit luma codes (8-bit segments x4, like the SourceFrameTap plane); skipped on frames nothing reads
						const int shift = 16 - bits;
						const int up = bits < 10 ? 10 - bits : 0, down = bits > 10 ? bits - 10 : 0;
						for (size_t i = 0; i < raw.size(); ++i)
							codes[i] = static_cast<uint16_t>(((raw[i] >> shift) >> down) << up);
					}
					if (Seam().frame)
						Seam().frame(codes.data(), W, H, W, seq, t);
					if (due)
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
							const char* reasonV = have ? SourceIdentity::ReasonText(static_cast<SourceIdentity::ReasonCode>(rec.reason[static_cast<int>(SourceIdentity::Field::AxisV)])) : "none";
							const char* reasonH = have ? SourceIdentity::ReasonText(static_cast<SourceIdentity::ReasonCode>(rec.reason[static_cast<int>(SourceIdentity::Field::AxisH)])) : "none";
							evals << reel.id << ',' << l->name << ',' << g << ',' << t << ',' << seg << ',' << bits << ',' << l->fx << ',' << l->fy << ','
								<< nx << ',' << ny << ',' << state << ',' << s.declaredRatio << ',' << s.confidence << ',' << s.frameQuality << ','
								<< s.frameNoise << ',' << s.bitrateQuality << ',' << (have ? rec.tier : -2) << ',' << (have ? rec.level : -1.0f) << ','
								<< (have ? rec.ratioH : -1.0f) << ',' << (have ? rec.ratioV : -1.0f) << ',' << (have ? rec.consensusGen : 0ull) << ','
								<< reasonV << ',' << reasonH << ',' << ms[li] << ',' << s.cuts << ',' << (have ? static_cast<int>(rec.latched) : 0)
								<< kClassicalChainTail;
							if (std::fabs(s.declaredRatio - l->declared) > 1e-9)
							{
								events << reel.id << ',' << l->name << ',' << t << ",," << g << ',' << seg << ",declared," << l->declared << ',' << s.declaredRatio << ",\n";
								l->declared = s.declaredRatio;
							}
							if (have && l->monitor.SourceIdEvents() != l->events)
							{
								l->events = l->monitor.SourceIdEvents();
								// event_t = the rung-change BOUNDARY the detector reports (comparable with replay.py's +-1 s boundary match)
								events << reel.id << ',' << l->name << ',' << t << ',' << rec.abrTimeUs / 1e6 << ',' << g << ',' << seg << ",abr," << (rec.abrDirection == 1 ? "drop" : "rise") << ','
									<< rec.abrMagnitude << ",\"ratio_step=" << rec.abrRatioStep << " conf=" << rec.abrConfidence << "\"\n";
							}
							std::string text;
							if (l->consensus && l->monitor.TakeSourceIdNotice(l->noticeSeen, text))
								events << reel.id << ',' << l->name << ',' << t << ",," << g << ',' << seg << ",notice,,," << CsvText(text) << '\n';
						}
						if (Seam().chainRow)
						{
							ChainRow cr;
							if (Seam().chainRow(t, cr))
							{
								const long cnx = std::lround(cr.x.appliedN), cny = std::lround(cr.y.appliedN);
								const std::string& kern = !cr.y.kernel.empty() ? cr.y.kernel : cr.x.kernel;
								const double sharp = (std::max)(cr.x.sharpenAmount, cr.y.sharpenAmount);
								evals << reel.id << ",chain," << g << ',' << t << ',' << seg << ',' << bits << ",,,";
								Num(evals, static_cast<double>(cnx), cr.x.appliedN > 0);
								evals << ',';
								Num(evals, static_cast<double>(cny), cr.y.appliedN > 0);
								evals << ',' << StateToken(cr.state) << ",,,,,,,,,,," << CsvText(cr.reason) << ",,";   // declared..gen, reason, reason_h, (ms below)
								Num(evals, cr.ms, cr.ms >= 0);
								evals << ",,," << CsvText(kern) << ',';                                              // cuts, latched_axes, kernel
								Num(evals, sharp, sharp >= 0);
								evals << ',';
								Num(evals, cr.x.doubleN1, cr.x.doubleN1 > 0);
								evals << ',';
								Num(evals, cr.y.doubleN1, cr.y.doubleN1 > 0);
								evals << ',';
								Num(evals, cr.activeX, cr.activeX >= 0); evals << ',';
								Num(evals, cr.activeY, cr.activeY >= 0); evals << ',';
								Num(evals, cr.activeW, cr.activeW >= 0); evals << ',';
								Num(evals, cr.activeH, cr.activeH >= 0); evals << ',';
								Num(evals, cr.x.measuredN, cr.x.measuredN > 0); evals << ',';
								Num(evals, cr.y.measuredN, cr.y.measuredN > 0); evals << ',';
								evals << StateToken(cr.x.state) << ',' << StateToken(cr.y.state) << ',' << cr.x.llr << ',' << cr.y.llr << ',' << CsvText(cr.x.power) << ',' << CsvText(cr.y.power) << ',';
								Num(evals, cr.x.spatialAgreement, cr.x.spatialAgreement >= 0); evals << ',';
								Num(evals, cr.y.spatialAgreement, cr.y.spatialAgreement >= 0); evals << ',';
								evals << cr.x.reasonBits << ',' << cr.y.reasonBits << ',' << CsvText(cr.x.reason) << ',' << CsvText(cr.y.reason) << '\n';
							}
						}
						if (Seam().chainEvents)
						{
							std::vector<ChainEvent> evs;
							Seam().chainEvents(t, evs);
							for (const ChainEvent& e : evs)
								events << reel.id << ",chain," << t << ',' << (e.eventTime >= 0 ? e.eventTime : t) << ',' << g << ',' << seg << ',' << e.type << ',' << e.fromN << ',' << e.toN
									<< ',' << CsvText("axis=" + e.axis + (e.detail.empty() ? std::string() : "; " + e.detail)) << '\n';
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
							events << reel.id << ',' << l.name << ',' << t << ",," << g << ',' << seg << ",engage," << l.fy << ',' << ny << ',' << CsvText(why) << '\n';
							l.fx = nx;
							l.fy = ny;
						}
					}
					++g;
				}
				const int exitCode = _pclose(pipe);
				if (Seam().reelEnd)
					Seam().reelEnd(reel.id);
				const int errLines = CountLines(errPath);
				const bool ok = (g == reel.frames) && exitCode == 0;
				const char* st = ok ? "ok" : (g == 0 ? "no_frames" : (g != reel.frames ? "frame_count_mismatch" : "ffmpeg_exit"));
				const double wall = MsSince(wall0) / 1000.0;
				done << reel.id << ',' << reel.frames << ',' << g << ',' << wall << ',' << errLines << ',' << st << '\n';
				evals.close();
				events.close();
				done.close();
				// atomic: the directory appears under its final name only when complete (a partial reel stays *.failed for inspection)
				RemoveTree(ok ? finalDir : finalDir + ".failed");
				MoveFileExA(tmpDir.c_str(), ok ? finalDir.c_str() : (finalDir + ".failed").c_str(), MOVEFILE_REPLACE_EXISTING);
				index << std::fixed << std::setprecision(1) << reel.id << ',' << reel.frames << ',' << g << ',' << wall << ',' << errLines << ',' << st << std::endl;
				if (!ok)
					++failures;
				Logger::WriteMessage((reel.id + ": " + std::to_string(g) + "/" + std::to_string(reel.frames) + " frames, " + st).c_str());
				++ran;
			}
			Logger::WriteMessage(("SourceIdChainEvalTests: " + std::to_string(ran) + " reels run, " + std::to_string(skipped) + " already complete, " + std::to_string(failures) +
								  " failures" + (stopped ? ", stopped: " + stopWhy : std::string()) + "; output " + outDir).c_str());
			if (!stopped)
				Assert::IsTrue(ran + skipped > 0, L"no reel matched VP_SOURCE_ID_REELS_ONLY");
			Assert::AreEqual(0, failures, L"a reel did not decode all of its frames (or the drive ran low)");
		}
	};
}
