/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SID-C2b's registration of the chain estimator with the evaluation harness seam (ChainEvalSeam.h; the harness itself,
// SourceIdChainEvalTests.cpp, is not touched). The harness offers every decoded plane of a reel (10-bit luma codes), asks for a "chain"
// row at its 0.5 s cadence and drains chain events; this file feeds the planes to a ChainEstimator that runs SYNCHRONOUSLY on the
// source clock (the injected clock is the frame's source time, so a run is deterministic: no wall clock enters a decision) and
// reports its record. Stand-ins for what the renderer supplies in production, both deliberately simple: a scene-cut / black detector
// (the monitor's cuts and blanks) and a debounced letterbox detector (Bill's active-picture rectangle).
//
// CALIBRATION DUMP (v3; contract D:\codex\vp-handover\sid\cal-dump-v3-contract.md): VP_SOURCE_ID_CAL_DUMP=<dir> writes <dir>\<reel id>.csv,
// one row per evaluation x axis x tested candidate, 27 columns (the 21 of v2, then the six of v3):
//   reel_id,title,eval_seq,time_s,axis,N,n,class,T,stage,best_kernel,kernel_margin,lines_passed,lines,power_seen,floor_codes,
//   truth_n,bit_depth,truth_kernel,truth_sharpen,shot_id,T_ev,informative,master,has_truth,truth_n1,cut_id
// class = K8 | K16 | Rational | Generic (ClassifyLatent of n over N); T = the trimmed-mean statistic of `stage` (coarse | exact);
// n and truth_n are in the SAME reference (the active-area axis the estimator measures: truth_n = reels.json truth_n x N / raster
// size, 0 = native); lines_passed = the line gate's informative lines (exact stage only, else 0); power_seen = the power probe saw its
// injection; floor_codes = the dof-normalised rounding floor handed to the evidence stage; shot_id = the reel truth's shot index
// (the master's shot boundaries mapped through the segment, segment index x 10000 + shots before the frame), else this file's own cut
// count. The truth comes from reels.json (VP_SOURCE_ID_REELS, the harness's own variable); without it the truth columns are 0 / empty.
// v3 columns: T_ev = the statistic ChainEvidence::Judge feeds the LLR (trimmed mean over the gated AND unmasked lines; EMPTY when the
// candidate was not judged: coarse rows, gate failure, mixed), while T keeps its v2 meaning for diagnostics; informative = 1 when the
// estimator would use the candidate (judged, enough gated lines, not clustered away); master = the reel's footage id (reels.json
// `master`), quoted; has_truth = 1 when the row has segment truth for this axis; truth_n1 = the intermediate latent of a double-scaled
// reel (reels.json `double.n1`, same reference as truth_n), 0 if none; cut_id = the estimator's own detected shot id (shot_id stays the
// reel truth's).
// CPU only; the unit test below feeds a few synthetic frames through the seam and reads the dump back.
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "ChainEvalSeam.h"

#include <VP-V/tests/support/RealClipLibrary.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/identity/SidDiagDump.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidChainReg
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		struct SegTruth
		{
			int idx = 0, start = 0, frames = 0, bits = 10, srcA = 0;
			int codedBits = 0;          // reels.json `coded_bits` (dump v4 hint; 0 = unknown)
			int truthN[2] = { 0, 0 };   // raster-referenced original size per axis
			int n1[2] = { 0, 0 };       // raster-referenced intermediate size of a double-scaled segment (0 = none)
			std::string kernel;         // "" = native / none
			double sharpen = 0.0;
		};
		struct ReelTruth
		{
			std::string id, title, master;
			int rasterW = 0, rasterH = 0;
			std::vector<SegTruth> segs;
			std::vector<int> shots;     // the master's cut frames
			bool haveShots = false;
			const SegTruth* SegAt(int frame) const
			{
				for (const SegTruth& s : segs)
					if (frame >= s.start && frame < s.start + s.frames)
						return &s;
				return segs.empty() ? nullptr : &segs.back();
			}
		};

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

		inline std::string Num(double v)
		{
			char b[48];
			std::snprintf(b, sizeof(b), "%.6g", v);
			return b;
		}

		inline std::string Upper(const char* s)
		{
			std::string o;
			for (; *s; ++s)
				o += static_cast<char>((*s >= 'a' && *s <= 'z') ? *s - 'a' + 'A' : *s);
			return o;
		}

		inline bool ReadTruth(const std::string& path, std::map<std::string, ReelTruth>& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			std::ostringstream ss;
			ss << in.rdbuf();
			RealClip::Json doc;
			try { doc = RealClip::Json::Parse(ss.str()); }
			catch (const std::exception&) { return false; }
			const RealClip::Json* reels = doc.Find("reels");
			if (!reels || reels->type != RealClip::Json::Array)
				return false;
			const RealClip::Json* masters = doc.Find("masters");
			for (const RealClip::Json& r : reels->a)
			{
				ReelTruth t;
				t.id = r.Str("id");
				t.title = r.Str("title");
				t.master = r.Str("master");
				t.rasterW = r.Int("width");
				t.rasterH = r.Int("height");
				const RealClip::Json* segs = r.Find("segments");
				if (segs)
					for (const RealClip::Json& g : segs->a)
					{
						SegTruth s;
						s.idx = g.Int("idx");
						s.start = g.Int("out_start_frame");
						s.frames = g.Int("out_frames");
						s.bits = g.Int("bits", 10);
						s.codedBits = g.Int("coded_bits", 0);
						s.srcA = g.Int("src_a");
						const RealClip::Json* tn = g.Find("truth_n");
						if (tn)
						{
							s.truthN[0] = tn->Int("x");
							s.truthN[1] = tn->Int("y");
						}
						const RealClip::Json* dbl = g.Find("double");
						if (dbl && dbl->type == RealClip::Json::Object)
						{
							const RealClip::Json* n1 = dbl->Find("n1");
							if (n1 && n1->type == RealClip::Json::Object)
							{
								s.n1[0] = n1->Int("x");
								s.n1[1] = n1->Int("y");
							}
						}
						const RealClip::Json* k = g.Find("kernel");
						if (k && k->type == RealClip::Json::Object)
							s.kernel = k->Str("name");
						const RealClip::Json* sh = g.Find("sharpening");
						if (sh && sh->type == RealClip::Json::Object)
							s.sharpen = sh->Num("amount");
						t.segs.push_back(s);
					}
				const RealClip::Json* m = masters ? masters->Find(t.master) : nullptr;
				const RealClip::Json* shots = m ? m->Find("shots") : nullptr;
				if (shots && shots->type == RealClip::Json::Array)
				{
					t.haveShots = true;
					for (const RealClip::Json& c : shots->a)
						t.shots.push_back(static_cast<int>(std::lround(c.n)));
				}
				if (!t.id.empty())
					out[t.id] = t;
			}
			return true;
		}

		// One reel's run state (the harness calls the seam on its own thread, one reel at a time).
		struct Rig
		{
			ChainFx::ManualClock clock;   // source time in ms: the estimator's clock
			std::unique_ptr<C::ChainEstimator> est;
			std::map<std::string, ReelTruth> truth;
			bool truthTried = false;
			ReelTruth cur;
			bool haveTruth = false;
			std::string reel;
			std::ofstream cal;
			bool calOpen = false;
			std::ofstream cost;   // <VP_SOURCE_ID_CAL_DUMP>\cost\<reel>.csv: the verifier work of every evaluation (builds, solves, cached MiB)
			bool costOpen = false;
			int rasterW = 0, rasterH = 0;
			double lastFrameT = 0.0;
			uint64_t lastFrameSeq = 0;
			// stand-in cut / black detector
			std::vector<float> prevBlocks;
			double diffAverage = 0.0;
			bool wasBlank = false;
			int cutsDetected = 0;
			// stand-in letterbox detector (debounced)
			C::LineRect active, candidate;
			int candidateFrames = 0;
			// wall time of the evaluation that ran in the last frame call
			double lastEvalMs = -1.0;
			uint64_t lastEvaluations = 0;
			// chain events derived from the record
			std::vector<SidChain::ChainEvent> pending;
			bool haveLast = false;
			G::LatchState lastApplied[2] = { G::LatchState::Unknown, G::LatchState::Unknown };
			G::LatchState lastState[2] = { G::LatchState::Unknown, G::LatchState::Unknown };
			double lastAppliedN[2] = { 0, 0 }, lastMeasuredN[2] = { 0, 0 };
		};

		inline Rig& R()
		{
			static Rig r;
			return r;
		}

		inline int ShotId(const Rig& r, uint64_t seq)
		{
			const int g = static_cast<int>(seq) - 1;
			if (r.haveTruth && r.cur.haveShots)
			{
				const SegTruth* s = r.cur.SegAt(g);
				const int masterFrame = (s ? s->srcA : 0) + (s ? g - s->start : g);
				int n = 0;
				for (int c : r.cur.shots)
					if (c <= masterFrame)
						++n;
				return (s ? s->idx : 0) * 10000 + n;
			}
			return r.cutsDetected;
		}

		inline void WriteCalibrationRows(Rig& r, const C::EvaluationDetail& d)
		{
			if (!r.calOpen)
				return;
			const int g = static_cast<int>(d.seq) - 1;
			const SegTruth* seg = r.haveTruth ? r.cur.SegAt(g) : nullptr;
			static const char* const kClass[4] = { "K8", "K16", "Rational", "Generic" };
			for (int a = 0; a < 2; ++a)
			{
				const C::AxisDetail& ax = d.axis[a];
				if (!ax.have)
					continue;
				const int rasterDim = a == 0 ? r.cur.rasterW : r.cur.rasterH;
				long truthN = 0;
				if (seg && rasterDim > 0 && seg->truthN[a] > 0 && seg->truthN[a] != rasterDim)
					truthN = std::lround(static_cast<double>(seg->truthN[a]) * ax.N / rasterDim);   // active-area reference; 0 = native
				long truthN1 = 0;   // the intermediate latent of a double-scaled reel, in the same (active-area) reference as truth_n
				if (seg && rasterDim > 0 && seg->n1[a] > 0)
					truthN1 = std::lround(static_cast<double>(seg->n1[a]) * ax.N / rasterDim);
				for (const C::CandidateDetail& cd : ax.cands)
				{
					const int cls = C::ClassifyLatent(cd.n, ax.N);
					r.cal << CsvText(r.cur.id.empty() ? r.reel : r.cur.id) << ',' << CsvText(r.cur.title) << ',' << d.seq << ','
						<< Num(static_cast<double>(d.timeUs) / 1e6) << ',' << (a == 0 ? 'x' : 'y') << ',' << ax.N << ',' << cd.n << ','
						<< kClass[cls < 0 || cls > 3 ? 3 : cls] << ',' << Num(cd.exact ? cd.stat : cd.coarseStat) << ',' << (cd.exact ? "exact" : "coarse") << ','
						<< CsvText(cd.kernel >= 0 ? std::string(C::Kernel(cd.kernel).name) : std::string()) << ',' << Num(cd.kernelMargin) << ','
						<< cd.linesGated << ',' << ax.lines << ',' << (ax.probeSeen ? 1 : 0) << ',' << Num(ax.floorCodes) << ',' << truthN << ','
						<< (seg ? seg->bits : (d.stepCodes == 4 ? 8 : 10)) << ',' << CsvText(seg ? seg->kernel : std::string()) << ','
						<< Num(seg ? seg->sharpen : 0.0) << ',' << ShotId(r, d.seq) << ','
						<< (cd.evJudged ? Num(cd.evStat) : std::string()) << ',' << (cd.informative && cd.evJudged ? 1 : 0) << ','
						<< CsvText(r.cur.master) << ',' << (seg && seg->truthN[a] > 0 ? 1 : 0) << ',' << truthN1 << ',' << d.shotId << '\n';
				}
			}
		}

		// One row per evaluation: the cost counters (deterministic) next to the lines used by the exact stage.
		inline void WriteCostRow(Rig& r, const C::EvaluationDetail& d)
		{
			if (!r.costOpen)
				return;
			r.cost << d.seq << ',' << Num(static_cast<double>(d.timeUs) / 1e6);
			for (int a = 0; a < 2; ++a)
				r.cost << ',' << d.axis[a].operatorBuilds << ',' << d.axis[a].solves << ',' << d.axis[a].solvesReused << ',' << d.axis[a].exactLines;
			r.cost << ',' << Num(d.cachedMB) << '\n';
		}

		// Test seam for the cost levers (read only here, in the test build; the shipped estimator reads no environment):
		// VP_SOURCE_ID_LEVER = none | step3 (16 exact lines on a confident axis) | coarse1 (one coarse window) | coarse2 (coarse step 2 + refine).
		inline void ApplyLever(C::EstimatorSettings& s)
		{
			// SID-5 item 3 (doc 100 s4 P0): VP_SOURCE_ID_SAFETY_RULES=1 runs the chain lane with the r2 safety rules (R2 + R3 + R5 + R7 + N1);
			// unset = the default (off), byte for byte.
			if (RealClip::Env("VP_SOURCE_ID_SAFETY_RULES") == "1")
				s.safetyRules = true;
			const std::string lever = RealClip::Env("VP_SOURCE_ID_LEVER");
			if (lever == "step3")
				s.confidentAxisExactLines = 16;
			else if (lever == "coarse1")
				s.verifier.coarseWindows = 1;
			else if (lever == "coarse2")
			{
				s.verifier.coarseStep = 2;
				s.verifier.coarseRefine = true;
			}
		}

		inline void ReelBegin(const std::string& reelId)
		{
			Rig& r = R();
			if (!r.truthTried)
			{
				r.truthTried = true;
				const std::string path = RealClip::Env("VP_SOURCE_ID_REELS");
				if (!path.empty())
					ReadTruth(path, r.truth);
			}
			r.reel = reelId;
			const auto it = r.truth.find(reelId);
			r.haveTruth = it != r.truth.end();
			r.cur = r.haveTruth ? it->second : ReelTruth();
			r.cur.id = reelId;
			r.rasterW = r.rasterH = 0;
			r.prevBlocks.clear();
			r.diffAverage = 0.0;
			r.wasBlank = false;
			r.cutsDetected = 0;
			r.active = C::LineRect();
			r.candidate = C::LineRect();
			r.candidateFrames = 0;
			r.lastEvalMs = -1.0;
			r.lastEvaluations = 0;
			r.pending.clear();
			r.haveLast = false;
			r.clock.Set(0.0);
			C::EstimatorSettings settings;
			ApplyLever(settings);
			r.est.reset(new C::ChainEstimator(settings, r.clock.Fn()));
			r.est->SetSynchronous(true);
			r.est->SetDiagnosticTag(reelId + ".chain");   // dump v4 (VP_SID_DUMP_V4_DIR): <dir>\<reel>.chain.b_*.csv
			r.est->SetEvaluationObserver([](const C::EvaluationDetail& d) { WriteCalibrationRows(R(), d); WriteCostRow(R(), d); });
			r.est->SetMode(C::EstimatorMode::Shadow);
			if (r.cal.is_open())
				r.cal.close();
			r.calOpen = false;
			if (r.cost.is_open())
				r.cost.close();
			r.costOpen = false;
			const std::string dir = RealClip::Env("VP_SOURCE_ID_CAL_DUMP");
			if (!dir.empty())
			{
				std::string name = reelId;
				for (char& c : name)
					if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
						c = '_';
				const std::string sep = (dir.back() == '\\' || dir.back() == '/') ? "" : "\\";
				r.cal.open(dir + sep + name + ".csv", std::ios::out | std::ios::trunc);
				if (r.cal.is_open())
				{
					r.calOpen = true;
					r.cal << "reel_id,title,eval_seq,time_s,axis,N,n,class,T,stage,best_kernel,kernel_margin,lines_passed,lines,power_seen,floor_codes,"
						"truth_n,bit_depth,truth_kernel,truth_sharpen,shot_id,T_ev,informative,master,has_truth,truth_n1,cut_id\n";
				}
				CreateDirectoryA((dir + sep + "cost").c_str(), nullptr);
				r.cost.open(dir + sep + "cost" + sep + name + ".csv", std::ios::out | std::ios::trunc);
				if (r.cost.is_open())
				{
					r.costOpen = true;
					r.cost << "eval_seq,time_s,builds_x,solves_x,reused_x,exact_lines_x,builds_y,solves_y,reused_y,exact_lines_y,cached_mb\n";
				}
			}
		}

		inline void ReelEnd(const std::string&)
		{
			Rig& r = R();
			if (r.cal.is_open())
				r.cal.close();
			r.calOpen = false;
			if (r.cost.is_open())
				r.cost.close();
			r.costOpen = false;
			if (r.est)
				r.est->Stop();
			r.est.reset();
		}

		// Stand-in for the monitor's cuts and blanks: a 16 x 9 grid of block means from every 4th pixel; a cut is a large jump of the grid
		// against the recent average jump, a blank is a near-black frame (a one-shot edge each).
		inline void DetectEvents(Rig& r, const uint16_t* luma, int w, int h, int stride, uint64_t seq)
		{
			const int gx = 16, gy = 9;
			std::vector<float> blocks(static_cast<size_t>(gx) * gy, 0.0f);
			std::vector<int> counts(blocks.size(), 0);
			for (int y = 0; y < h; y += 4)
			{
				const int by = (std::min)(gy - 1, y * gy / h);
				const uint16_t* row = luma + static_cast<size_t>(y) * stride;
				for (int x = 0; x < w; x += 4)
				{
					const size_t b = static_cast<size_t>(by) * gx + (std::min)(gx - 1, x * gx / w);
					blocks[b] += row[x];
					++counts[b];
				}
			}
			float mean = 0.0f, top = 0.0f;
			for (size_t i = 0; i < blocks.size(); ++i)
			{
				blocks[i] = counts[i] > 0 ? blocks[i] / static_cast<float>(counts[i]) : 0.0f;
				mean += blocks[i];
				top = (std::max)(top, blocks[i]);
			}
			mean /= static_cast<float>(blocks.size());
			const bool blank = mean < 72.0f && top < 90.0f;
			if (blank && !r.wasBlank)
				r.est->NoteEvent(C::EstimatorEvent::Blank, seq);
			r.wasBlank = blank;
			if (!r.prevBlocks.empty() && !blank)
			{
				double diff = 0.0;
				for (size_t i = 0; i < blocks.size(); ++i)
					diff += std::fabs(blocks[i] - r.prevBlocks[i]);
				diff /= static_cast<double>(blocks.size());
				if (diff > 30.0 && diff > 4.0 * r.diffAverage)
				{
					r.est->NoteEvent(C::EstimatorEvent::Cut, seq);
					++r.cutsDetected;
				}
				else
					r.diffAverage += 0.1 * (diff - r.diffAverage);
			}
			r.prevBlocks = blocks;
		}

		// Stand-in for Bill's active-picture rectangle: black bars (codes <= 80 along whole sampled rows / columns) at the frame edges. A
		// candidate rectangle is adopted after 24 identical frames, and only when it keeps at least half of the frame in both directions
		// (a dark scene is not "all bars").
		inline C::LineRect DetectActive(const uint16_t* luma, int w, int h, int stride)
		{
			const uint16_t kBar = 80;
			auto rowDark = [&](int y) {
				const uint16_t* row = luma + static_cast<size_t>(y) * stride;
				for (int x = 0; x < w; x += 8)
					if (row[x] > kBar)
						return false;
				return true;
			};
			auto colDark = [&](int x) {
				for (int y = 0; y < h; y += 8)
					if (luma[static_cast<size_t>(y) * stride + x] > kBar)
						return false;
				return true;
			};
			int top = 0, bottom = h, left = 0, right = w;
			while (top < h / 2 && rowDark(top)) ++top;
			while (bottom > h / 2 && rowDark(bottom - 1)) --bottom;
			while (left < w / 2 && colDark(left)) ++left;
			while (right > w / 2 && colDark(right - 1)) --right;
			C::LineRect rc;
			rc.x = left;
			rc.y = top;
			rc.w = right - left;
			rc.h = bottom - top;
			if (rc.w * 2 < w || rc.h * 2 < h)
			{
				rc.x = 0; rc.y = 0; rc.w = w; rc.h = h;
			}
			return rc;
		}

		inline void OnFrame(const uint16_t* luma, int w, int h, int stride, uint64_t seq, double t)
		{
			Rig& r = R();
			if (!r.est || luma == nullptr || w < 1 || h < 1)
				return;
			if (r.rasterW == 0)
			{
				r.rasterW = w;
				r.rasterH = h;
				if (r.cur.rasterW == 0)
				{
					r.cur.rasterW = w;
					r.cur.rasterH = h;
				}
				r.active.x = 0; r.active.y = 0; r.active.w = w; r.active.h = h;
				r.candidate = r.active;
			}
			r.clock.Set(t * 1000.0);
			r.lastFrameT = t;
			r.lastFrameSeq = seq;
			DetectEvents(r, luma, w, h, stride, seq);
			const C::LineRect found = DetectActive(luma, w, h, stride);
			if (found.x == r.candidate.x && found.y == r.candidate.y && found.w == r.candidate.w && found.h == r.candidate.h)
			{
				if (++r.candidateFrames >= 24)
					r.active = r.candidate;
			}
			else
			{
				r.candidate = found;
				r.candidateFrames = 1;
			}
			if (r.haveTruth)
			{
				const SegTruth* sg = r.cur.SegAt(static_cast<int>(seq) - 1);
				if (sg)
					r.est->SetDiagnosticDepths(sg->codedBits, sg->bits);   // dump v4 hint only
			}
			r.est->NoteFrame(seq, t, r.active.x, r.active.y, r.active.w, r.active.h);
			VpSkeleton::SourceFrameView v;
			v.seq = seq;
			v.luma = luma;
			v.width = static_cast<uint32_t>(w);
			v.height = static_cast<uint32_t>(h);
			v.strideSamples = static_cast<uint32_t>(stride);
			v.msbShift = 6;
			const uint64_t before = r.est->GetCounters().evaluations;
			const auto t0 = std::chrono::steady_clock::now();
			r.est->Deliver(v);
			if (r.est->GetCounters().evaluations != before)
			{
				r.lastEvalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				r.lastEvaluations = r.est->GetCounters().evaluations;
			}
		}

		inline const char* FamilyName(int kernelIndex)
		{
			if (kernelIndex < 0)
				return "";
			switch (C::Kernel(kernelIndex).family)
			{
			case C::Family::Bilinear: return "bilinear";
			case C::Family::Bicubic: return "bicubic";
			case C::Family::Lanczos: return "lanczos";
			case C::Family::Spline: return "spline";
			}
			return "";
		}

		inline bool IsConfident(G::LatchState s) { return s == G::LatchState::Latched; }   // Latched = the reserved numeric of CONFIDENT

		inline void TrackEvents(Rig& r, const G::Record& rec, const double measuredN[2], const double appliedN[2])
		{
			const char* const axisName[2] = { "x", "y" };
			for (int a = 0; a < 2; ++a)
			{
				const G::LatchState applied = rec.applied[a].state;
				const G::LatchState state = rec.measured[a].state;
				if (r.haveLast)
				{
					SidChain::ChainEvent e;
					e.axis = axisName[a];
					if (IsConfident(applied) && !IsConfident(r.lastApplied[a]))
					{
						e.type = "confident";
						e.toN = appliedN[a];
						r.pending.push_back(e);
					}
					else if (IsConfident(applied) && IsConfident(r.lastApplied[a]) && std::fabs(appliedN[a] - r.lastAppliedN[a]) > 0.5)
					{
						e.type = "change";
						e.fromN = r.lastAppliedN[a];
						e.toN = appliedN[a];
						r.pending.push_back(e);
					}
					else if (!IsConfident(applied) && IsConfident(r.lastApplied[a]))
					{
						e.type = "unsure";
						e.fromN = r.lastAppliedN[a];
						e.detail = "no longer confident";
						r.pending.push_back(e);
					}
					if (state == G::LatchState::Hold && r.lastState[a] != G::LatchState::Hold)
					{
						SidChain::ChainEvent h;
						h.type = "hold";
						h.axis = axisName[a];
						r.pending.push_back(h);
					}
					if (measuredN[a] > 0 && r.lastMeasuredN[a] > 0 && std::fabs(measuredN[a] - r.lastMeasuredN[a]) > 1.5 && IsConfident(applied))
					{
						SidChain::ChainEvent g;
						g.type = "geometry";
						g.axis = axisName[a];
						g.fromN = r.lastMeasuredN[a];
						g.toN = measuredN[a];
						r.pending.push_back(g);
					}
				}
				r.lastApplied[a] = applied;
				r.lastState[a] = state;
				r.lastAppliedN[a] = appliedN[a];
				r.lastMeasuredN[a] = measuredN[a];
			}
			r.haveLast = true;
		}

		inline bool OnChainRow(double, SidChain::ChainRow& row)
		{
			Rig& r = R();
			if (!r.est)
				return false;
			G::Record rec;
			if (!r.est->Held().ReadLatest(0, rec))
				return false;
			const C::ChainStatusInput in = r.est->StatusInput();
			const int dims[2] = { r.rasterW, r.rasterH };
			double measuredN[2] = { 0, 0 }, appliedN[2] = { 0, 0 };
			SidChain::AxisRow* rows[2] = { &row.x, &row.y };
			uint32_t bits = 0;
			for (int a = 0; a < 2; ++a)
			{
				const G::AxisGeom& m = rec.measured[a];
				const G::AxisGeom& p = rec.applied[a];
				SidChain::AxisRow& ar = *rows[a];
				ar.state = Upper(G::StateText(m.state));
				if (m.n > 0 && m.N > 0)
					measuredN[a] = static_cast<double>(m.n) / m.N * dims[a];   // raster-referenced
				if (IsConfident(p.state) && p.n > 0 && p.N > 0)
					appliedN[a] = static_cast<double>(p.n) / p.N * dims[a];
				ar.measuredN = measuredN[a];
				ar.appliedN = appliedN[a];
				ar.kernel = FamilyName(p.kernelIndex);
				ar.llr = m.llr;
				ar.reasonBits = m.reasonBits;
				ar.reason = G::ReasonWords(m.reasonBits);
				bits |= m.reasonBits;
				if (in.haveEval && in.last.axis[a].probeRan)
					ar.power = in.last.axis[a].probeSeen ? "seen" : "unseen";
				if (in.haveEval && in.last.axis[a].gatedLines > 0)
					ar.spatialAgreement = 1.0 - static_cast<double>(in.last.axis[a].maskedLines) / in.last.axis[a].gatedLines;
			}
			const G::LatchState sx = rec.measured[0].state, sy = rec.measured[1].state;
			if (IsConfident(rec.applied[0].state) && IsConfident(rec.applied[1].state))
				row.state = "CONFIDENT";
			else if (sx == G::LatchState::Hold || sy == G::LatchState::Hold)
				row.state = "HOLD";
			else if (sx == G::LatchState::Provisional || sy == G::LatchState::Provisional)
				row.state = "PROVISIONAL";
			else if (sx == G::LatchState::Abstain || sy == G::LatchState::Abstain)
				row.state = "ABSTAIN";
			else
				row.state = "UNKNOWN";
			row.reason = G::ReasonWords(bits);
			if (rec.area.conf > 0.0f)
			{
				row.activeX = rec.area.x;
				row.activeY = rec.area.y;
				row.activeW = rec.area.w;
				row.activeH = rec.area.h;
			}
			row.ms = r.lastEvalMs;
			TrackEvents(r, rec, measuredN, appliedN);
			return true;
		}

		inline void OnChainEvents(double, std::vector<SidChain::ChainEvent>& events)
		{
			Rig& r = R();
			for (const SidChain::ChainEvent& e : r.pending)
				events.push_back(e);
			r.pending.clear();
		}

		// Applied by the harness (ApplyRegistrations) once at the start of its run.
		static SidChain::Registrar registration([](SidChain::EvalSeam& s) {
			s.reelBegin = &ReelBegin;
			s.reelEnd = &ReelEnd;
			s.frame = &OnFrame;
			s.chainRow = &OnChainRow;
			s.chainEvents = &OnChainEvents;
		});
	}

	TEST_CLASS(ChainEvalRegistrationTests)
	{
	public:
		TEST_METHOD(TheEstimatorIsRegisteredWithTheSeamAndWritesTheCalibrationDump)
		{
			SidChain::ApplyRegistrations();
			SidChain::EvalSeam& seam = SidChain::Seam();
			Assert::IsTrue(static_cast<bool>(seam.frame) && static_cast<bool>(seam.reelBegin) && static_cast<bool>(seam.reelEnd) &&
				static_cast<bool>(seam.chainRow) && static_cast<bool>(seam.chainEvents), L"every seam callback is registered");

			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			const std::string dir = std::string(temp) + "sid_cal_dump_test";
			CreateDirectoryA(dir.c_str(), nullptr);
			const std::string file = dir + "\\test.reel.csv";
			DeleteFileA(file.c_str());
			_putenv_s("VP_SOURCE_ID_CAL_DUMP", dir.c_str());

			seam.reelBegin("test.reel");
			const ChainFx::Plane plane = ChainFx::MakePlane(320, 240, 5, 4);
			bool anyRow = false;
			for (int i = 0; i < 6; ++i)
			{
				const double t = 1.1 * i;
				seam.frame(plane.luma.data(), plane.w, plane.h, plane.w, static_cast<uint64_t>(i) + 1, t);
				SidChain::ChainRow row;
				if (seam.chainRow(t, row))
				{
					anyRow = true;
					Assert::IsFalse(row.state.empty());
					Assert::IsFalse(row.x.state.empty());
				}
				std::vector<SidChain::ChainEvent> events;
				seam.chainEvents(t, events);
			}
			seam.reelEnd("test.reel");
			_putenv_s("VP_SOURCE_ID_CAL_DUMP", "");
			Assert::IsTrue(anyRow, L"the estimator published and the seam got a row");

			std::ifstream in(file);
			Assert::IsTrue(in.good(), L"the calibration CSV was written");
			std::string header, line;
			std::getline(in, header);
			Assert::AreEqual(std::string("reel_id,title,eval_seq,time_s,axis,N,n,class,T,stage,best_kernel,kernel_margin,lines_passed,lines,power_seen,"
				"floor_codes,truth_n,bit_depth,truth_kernel,truth_sharpen,shot_id,T_ev,informative,master,has_truth,truth_n1,cut_id"), header);
			// A CSV row split that honours the quoted fields (reel_id, title, best_kernel, truth_kernel, master).
			auto fields = [](const std::string& l) {
				std::vector<std::string> out;
				std::string cur;
				bool quoted = false;
				for (size_t i = 0; i < l.size(); ++i)
				{
					const char ch = l[i];
					if (quoted)
					{
						if (ch == '"')
						{
							if (i + 1 < l.size() && l[i + 1] == '"')
							{
								cur += '"';
								++i;
							}
							else
								quoted = false;
						}
						else
							cur += ch;
					}
					else if (ch == '"')
						quoted = true;
					else if (ch == ',')
					{
						out.push_back(cur);
						cur.clear();
					}
					else
						cur += ch;
				}
				out.push_back(cur);
				return out;
			};
			Assert::AreEqual<size_t>(27, fields(header).size(), L"dump v3: 21 v2 columns + 6");
			int rows = 0, judged = 0;
			while (std::getline(in, line))
				if (!line.empty())
				{
					++rows;
					const std::vector<std::string> f = fields(line);
					Assert::AreEqual<size_t>(27, f.size(), L"every row has all 27 columns");
					if (f[9] == "coarse")
						Assert::AreEqual(std::string(), f[21], L"T_ev is empty for a candidate that was not judged (coarse rows)");
					Assert::IsTrue(f[22] == "0" || f[22] == "1", L"informative is a flag");
					if (f[22] == "1")
					{
						Assert::IsFalse(f[21].empty(), L"an informative row has its judged statistic");
						++judged;
					}
					Assert::AreEqual(std::string(), f[23], L"no reels.json: the master is empty (quoted)");
					Assert::AreEqual(std::string("0"), f[24], L"no reels.json: no truth");
					Assert::AreEqual(std::string("0"), f[25], L"no double-scale truth");
					Assert::IsFalse(f[26].empty(), L"cut_id is the estimator's own shot id");
				}
			in.close();
			Assert::IsTrue(rows > 0, L"one row per evaluation x axis x tested candidate");
			(void)judged;
			DeleteFileA(file.c_str());
			RemoveDirectoryA(dir.c_str());
		}
	};

	// Dump v4 (identity/SidDiagDump.h, doc 100 s4 P0): OFF by default (nothing written), and DECISION NEUTRAL: with it on every published
	// record, status line, counter, evaluation detail and monitor snapshot / factor is byte-identical to a run with it off. Plus the column
	// contract of the four files (headers = the SidDiagDump.h headers, every row has all columns).
	namespace SidDumpV4
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;
		namespace D = SourceIdentity::Diag;

		inline std::string Num(double v)   // full precision: the identity comparison must see every bit
		{
			char b[48];
			std::snprintf(b, sizeof(b), "%.17g", v);
			return b;
		}

		inline std::vector<std::string> Split(const std::string& l)
		{
			std::vector<std::string> out;
			std::string cur;
			bool quoted = false;
			for (size_t i = 0; i < l.size(); ++i)
			{
				const char ch = l[i];
				if (quoted)
				{
					if (ch == '"')
					{
						if (i + 1 < l.size() && l[i + 1] == '"') { cur += '"'; ++i; }
						else quoted = false;
					}
					else cur += ch;
				}
				else if (ch == '"') quoted = true;
				else if (ch == ',') { out.push_back(cur); cur.clear(); }
				else cur += ch;
			}
			out.push_back(cur);
			return out;
		}

		inline std::string TempDir(const char* name)
		{
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			const std::string dir = std::string(temp) + name;
			CreateDirectoryA(dir.c_str(), nullptr);
			return dir;
		}

		inline int CountCsv(const std::string& dir)
		{
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((dir + "\\*.csv").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE)
				return 0;
			int n = 0;
			do { ++n; } while (FindNextFileA(h, &fd));
			FindClose(h);
			return n;
		}

		inline void ClearCsv(const std::string& dir)
		{
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((dir + "\\*.csv").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE)
				return;
			do { DeleteFileA((dir + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
			FindClose(h);
		}

		// Rows of a dump file (header checked against `header`, every row with all columns). -1 = the file is missing.
		inline int CheckFile(const std::string& path, const char* header)
		{
			std::ifstream in(path);
			if (!in.good())
				return -1;
			std::string h, line;
			std::getline(in, h);
			Assert::AreEqual(std::string(header), h, L"dump v4 header = the SidDiagDump.h contract");
			const size_t cols = Split(h).size();
			int rows = 0;
			while (std::getline(in, line))
				if (!line.empty())
				{
					++rows;
					Assert::AreEqual(cols, Split(line).size(), L"every dump v4 row has all columns");
				}
			return rows;
		}

		// A 2x (640x360 -> 1280x720) linear upscale of a fixture plane: conventional sizes exist on this raster.
		inline ChainFx::Plane Upscaled(uint32_t seed, int step)
		{
			const ChainFx::Plane lowRes = ChainFx::MakePlane(640, 360, seed, 1);
			ChainFx::Plane p;
			p.w = 1280;
			p.h = 720;
			p.luma.assign(static_cast<size_t>(p.w) * p.h, 0);
			for (int y = 0; y < p.h; ++y)
				for (int x = 0; x < p.w; ++x)
				{
					const double sx = (x + 0.5) / 2.0 - 0.5, sy = (y + 0.5) / 2.0 - 0.5;
					const int ix = (std::max)(0, (std::min)(lowRes.w - 2, static_cast<int>(std::floor(sx))));
					const int iy = (std::max)(0, (std::min)(lowRes.h - 2, static_cast<int>(std::floor(sy))));
					const double ax = (std::min)(1.0, (std::max)(0.0, sx - ix)), ay = (std::min)(1.0, (std::max)(0.0, sy - iy));
					auto at = [&](int xx, int yy) { return static_cast<double>(lowRes.luma[static_cast<size_t>(yy) * lowRes.w + xx]); };
					double v = (at(ix, iy) * (1 - ax) + at(ix + 1, iy) * ax) * (1 - ay) + (at(ix, iy + 1) * (1 - ax) + at(ix + 1, iy + 1) * ax) * ay;
					v = std::floor(v / step + 0.5) * step;
					p.luma[static_cast<size_t>(y) * p.w + x] = static_cast<uint16_t>(v);
				}
			return p;
		}

		inline std::string Hex(const G::Packed& p)
		{
			static const char* const k = "0123456789abcdef";
			std::string s;
			for (size_t i = 0; i < G::kRecordBytesMax; ++i)
			{
				s += k[p.bytes[i] >> 4];
				s += k[p.bytes[i] & 15];
			}
			return s;
		}

		// Everything the chain estimator publishes over 14 s (a cut at 7 s), as one text: per frame the packed record and the status line,
		// per evaluation the observer detail, at the end the counters.
		inline std::string RunEstimator(bool rules, const std::string& tag)
		{
			ChainFx::ManualClock clock;
			C::ChainEstimator est(C::EstimatorSettings(), clock.Fn());
			est.SetSynchronous(true);
			est.SetSafetyRules(rules);
			est.SetDiagnosticTag(tag);
			est.SetDiagnosticDepths(8, 10);
			std::ostringstream o;
			est.SetEvaluationObserver([&o](const C::EvaluationDetail& d) {
				o << "E" << d.seq << ',' << d.timeUs << ',' << d.shotId << ',' << d.stepCodes << '\n';
				for (int a = 0; a < 2; ++a)
					for (const C::CandidateDetail& c : d.axis[a].cands)
						o << a << ',' << c.n << ',' << c.exact << ',' << Num(c.stat) << ',' << Num(c.coarseStat) << ',' << c.kernel << ','
							<< c.linesGated << ',' << c.informative << ',' << c.evJudged << ',' << Num(c.evStat) << '\n';
			});
			est.SetMode(C::EstimatorMode::Shadow);
			const ChainFx::Plane a = Upscaled(7, 1), b = Upscaled(11, 1);
			for (int i = 0; i < 56; ++i)
			{
				const double t = 0.25 * i;
				clock.Set(t * 1000.0);
				const uint64_t seq = static_cast<uint64_t>(i) + 1;
				if (i == 28)
					est.NoteEvent(C::EstimatorEvent::Cut, seq);
				ChainFx::Feed(est, i < 28 ? a : b, seq, t);
				G::Record rec;
				if (est.Held().ReadLatest(0, rec))
					o << "R" << seq << ',' << Hex(G::Pack(rec)) << '\n';
				o << "S" << est.StatusLine() << '\n';
			}
			const C::EstimatorCounters c = est.GetCounters();
			o << "C" << c.evaluations << ',' << c.delivered << ',' << c.dropped << ',' << c.windowClears << ',' << c.latch.entries << ','
				<< c.latch.releases << '\n';
			est.Stop();
			return o.str();
		}

		// One analysed frame of 8 tiles: random texture (native-looking) or the same texture pixel-doubled (a 2x upscale).
		inline void AnalyseFrame(SourceResolutionMonitor::Monitor& m, double seconds, uint64_t seq, bool upscaled)
		{
			const int T = 128, kTiles = 8;
			std::vector<float> data(static_cast<size_t>(T) * T * kTiles);
			uint32_t x = 12345u + static_cast<uint32_t>(seq);
			for (int t = 0; t < kTiles; ++t)
				for (int yy = 0; yy < T; ++yy)
					for (int xx = 0; xx < T; ++xx)
					{
						const int key = upscaled ? ((yy / 2) * T + xx / 2) : (yy * T + xx);
						uint32_t h = x ^ (static_cast<uint32_t>(t) * 2654435761u) ^ (static_cast<uint32_t>(key) * 40503u);
						h = h * 1664525u + 1013904223u;
						h ^= h >> 13;
						data[(static_cast<size_t>(t) * T + yy) * T + xx] = static_cast<float>(0.3 + 0.4 * (static_cast<double>(h & 0xFFFFFF) / 16777216.0));
					}
			const std::vector<float> none;
			m.AnalyseTilesForTest(data, kTiles, seconds, 3840, 2160, none, none, kTiles, T, 64.0 / 1023.0, seq);
		}

		inline std::string RunMonitor(const std::string& tag)
		{
			SourceResolutionMonitor::Monitor m;
			m.SetSourceIdConsensus(true);
			m.SetDiagnosticTag(tag);
			m.SetDiagnosticDepths(8, 10);
			std::ostringstream o;
			for (int i = 0; i < 48; ++i)
			{
				const double t = 0.5 * i;
				const uint64_t seq = static_cast<uint64_t>(i) + 1;
				AnalyseFrame(m, t, seq, i >= 16 && i < 40);
				const SourceResolutionMonitor::Snapshot s = m.Read();
				o << s.frames << ',' << s.cuts << ',' << s.shots << ',' << Num(s.frameRatio) << ',' << Num(s.liveRatio) << ',' << Num(s.declaredRatio) << ','
					<< Num(s.confidence) << ',' << Num(s.axisRatioX) << ',' << Num(s.axisRatioY) << ',' << Num(s.axisConfidence) << ','
					<< Num(s.frameQuality) << ',' << Num(s.frameNoise) << ',' << Num(s.coherenceH) << ',' << Num(s.coherenceV) << ',' << s.kernel << ','
					<< Num(s.evidenceSeconds) << '\n';
				std::string why;
				double fx = 1.0, fy = 1.0;
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, t + 0.1, seq);
				o << Num(fx) << ',' << Num(fy) << ',' << why << '\n';
			}
			return o.str();
		}
	}

	TEST_CLASS(SidDiagDumpV4Tests)
	{
	public:
		TEST_METHOD(DumpV4IsOffByDefaultAndWritesNothing)
		{
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			Assert::IsTrue(SourceIdentity::Diag::DumpDirV4().empty(), L"unset / empty = off");
			const std::string dir = SidDumpV4::TempDir("sid_dump_v4_off");
			SidDumpV4::ClearCsv(dir);
			SourceIdentity::Diag::Tag tag;
			tag.Set("x");
			SourceIdentity::Diag::File f;
			Assert::IsFalse(f.Ready(tag, "a_frames", SourceIdentity::Diag::FramesHeaderV4()), L"no directory: the file is never opened");
			(void)SidDumpV4::RunEstimator(true, "off_est");
			(void)SidDumpV4::RunMonitor("off_mon");
			Assert::AreEqual(0, SidDumpV4::CountCsv(dir), L"nothing was written");
			RemoveDirectoryA(dir.c_str());
		}

		TEST_METHOD(DumpV4ChainEstimatorOutputIsIdenticalAndRowsFollowTheContract)
		{
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			const std::string off0 = SidDumpV4::RunEstimator(false, "est_rules_off");
			const std::string off1 = SidDumpV4::RunEstimator(true, "est_rules_on");
			const std::string dir = SidDumpV4::TempDir("sid_dump_v4_est");
			SidDumpV4::ClearCsv(dir);
			_putenv_s("VP_SID_DUMP_V4_DIR", dir.c_str());
			const std::string on0 = SidDumpV4::RunEstimator(false, "est_rules_off");
			const std::string on1 = SidDumpV4::RunEstimator(true, "est_rules_on");
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			Assert::IsTrue(off0.find("E") != std::string::npos, L"the estimator evaluated");
			Assert::IsTrue(off0 == on0, L"rules off: every record, status line, detail and counter identical with the dump on");
			Assert::IsTrue(off1 == on1, L"rules on: identical with the dump on");
			for (const char* t : { "est_rules_off", "est_rules_on" })
			{
				const int evals = SidDumpV4::CheckFile(dir + "\\" + t + ".b_evals.csv", SourceIdentity::Diag::EvalsHeaderV4());
				const int sizes = SidDumpV4::CheckFile(dir + "\\" + t + ".b_sizes.csv", SourceIdentity::Diag::SizesHeaderV4());
				Assert::IsTrue(evals > 0, L"one b_evals row per evaluation");
				Assert::IsTrue(sizes >= evals, L"b_sizes: every conventional size of the 1280x720 raster, per axis and evaluation");
			}
			// The depth hint lands in its own columns (coded 8, output 10), apart from the measured output step.
			std::ifstream in(dir + "\\est_rules_on.b_evals.csv");
			std::string h, row;
			std::getline(in, h);
			std::getline(in, row);
			const std::vector<std::string> hc = SidDumpV4::Split(h), rc = SidDumpV4::Split(row);
			for (size_t i = 0; i < hc.size(); ++i)
			{
				if (hc[i] == "coded_bits") Assert::AreEqual(std::string("8"), rc[i]);
				if (hc[i] == "output_bits") Assert::AreEqual(std::string("10"), rc[i]);
				if (hc[i] == "safety") Assert::AreEqual(std::string("1"), rc[i]);
			}
			in.close();
			SidDumpV4::ClearCsv(dir);
			RemoveDirectoryA(dir.c_str());
		}

		TEST_METHOD(DumpV4MonitorOutputIsIdenticalAndWritesOneRowPerAnalysedFrame)
		{
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			const std::string off = SidDumpV4::RunMonitor("mon");
			const std::string dir = SidDumpV4::TempDir("sid_dump_v4_mon");
			SidDumpV4::ClearCsv(dir);
			_putenv_s("VP_SID_DUMP_V4_DIR", dir.c_str());
			const std::string on = SidDumpV4::RunMonitor("mon");
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			Assert::IsTrue(off == on, L"every snapshot field and factor decision identical with the dump on");
			Assert::AreEqual(48, SidDumpV4::CheckFile(dir + "\\mon.a_frames.csv", SourceIdentity::Diag::FramesHeaderV4()), L"one row per analysed frame");
			Assert::IsTrue(SidDumpV4::CheckFile(dir + "\\mon.a_abr.csv", SourceIdentity::Diag::AbrHeaderV4()) >= 0, L"the ABR file exists (consensus on)");
			SidDumpV4::ClearCsv(dir);
			RemoveDirectoryA(dir.c_str());
		}
	};
}
