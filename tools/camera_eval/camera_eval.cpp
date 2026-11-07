// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// camera_eval: offline CPU evaluation driver for VP's global camera model (CameraModel::Fitter and the
// measurement plan / reference matcher). Not part of the solution; see README.md and build_camera_eval.bat.
//
//   camera_eval fit   --vec DIR --frames N --w W --h H --block 16 --fps F --out FILE.camf [--csv FILE] [--phase1] [--two-layer [--dyn-texture [--dt-share F] [--dt-temporal F] [--dt-spatial F] [--dt-agree F]] [--tl-ring F] [--tl-swap S] [--tl-veto R] [--tl-floor F] [--tl-comp R]]
//   camera_eval match --yuv CODED.yuv --w W --h H --bitdepth 10 --frames N --vec DIR --ref DIR --fps F
//                     [--start 2] [--count 40] [--phase1] [--dump-flo DIR] --out FILE.csv
//
// --dump-flo DIR (match): per judged pair t writes DIR/pair_%06d/pred_off.flo and pred_on.flo, Middlebury .flo, full
// picture size, source px, frame t-1 -> t (p in t-1 moves to p + flow in t). Block vectors are expanded to pixels
// (each block covers `block` source px; the border strip beyond the last whole block repeats the edge block).
//
// Input flows: DIR/flow_%06d_%06d.npy (float32 LE, C order, shape (gh4, gw4, 2), 4 px grid, ch0 dx, ch1 dy;
// frame_b(p + F(p)) ~ frame_a(p) for file flow_a_b). For pair t = 1..N-1 the forward field is flow_(t-1)_(t) and
// the backward field flow_(t)_(t-1). Each is reduced to the fitter grid (gridW = floor(W/block), gridH =
// floor(H/block)): vector = mean of the 4 px cells in the block, trust = clamp(1 - RMS spread / block, 0, 1).
//
// .camf (little-endian, identical to the earlier camfit_cli): "CAMF"; i32 pairs; i32 gridW; i32 gridH;
// f64 blockPx; per pair: i32 valid; i32 modelType; f64 confidence; f64 h[9]; u8 label[gridW*gridH].
// h maps a point of frame t-1 to frame t in SOURCE px, origin at the picture centre.
// label: 0 unknown / outlier / excluded, 1 camera inlier, 2 object, 3 overlay.
//
// --phase1 restores the phase-1 fitter configuration: inlier-share ramp 0.4/0.9, no temporal noise floor, no
// verdict gate on the confidence.
#include <direct.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "GlobalModelFit.h"
#include "GlobalModelMeasurement.h"
#include "ReferenceMatcher.h"

using namespace CameraModel;

struct Flow { int gh = 0, gw = 0; std::vector<float> d; };
struct Reduced { std::vector<float> x, y, t; };

static bool Fail(const std::string& m) { fprintf(stderr, "camera_eval: error: %s\n", m.c_str()); return false; }

static bool ReadNpy(const std::string& path, Flow& f)
{
	FILE* fp = fopen(path.c_str(), "rb");
	if (!fp) return Fail("cannot open " + path);
	unsigned char hd[10];
	if (fread(hd, 1, 10, fp) != 10 || memcmp(hd, "\x93NUMPY", 6) != 0) { fclose(fp); return Fail("not a .npy file: " + path); }
	const int major = hd[6];
	size_t hlen = 0;
	if (major == 1) hlen = hd[8] | (hd[9] << 8);
	else if (major == 2 || major == 3)
	{
		unsigned char ex[2];
		if (fread(ex, 1, 2, fp) != 2) { fclose(fp); return Fail("truncated header: " + path); }
		hlen = (size_t)hd[8] | ((size_t)hd[9] << 8) | ((size_t)ex[0] << 16) | ((size_t)ex[1] << 24);
	}
	else { fclose(fp); return Fail("unsupported .npy version in " + path); }
	std::string h(hlen, '\0');
	if (fread(&h[0], 1, hlen, fp) != hlen) { fclose(fp); return Fail("truncated header: " + path); }
	if (h.find("'<f4'") == std::string::npos && h.find("\"<f4\"") == std::string::npos) { fclose(fp); return Fail("dtype is not '<f4' in " + path); }
	const size_t fo = h.find("fortran_order");
	if (fo == std::string::npos || h.find("False", fo) == std::string::npos) { fclose(fp); return Fail("fortran_order is not False in " + path); }
	const size_t sp = h.find("shape");
	const size_t lp = sp == std::string::npos ? sp : h.find('(', sp);
	int a = 0, b = 0, c = 0;
	if (lp == std::string::npos || sscanf(h.c_str() + lp, "(%d,%d,%d", &a, &b, &c) != 3 || c != 2 || a <= 0 || b <= 0) { fclose(fp); return Fail("shape is not (gh, gw, 2) in " + path); }
	f.gh = a; f.gw = b;
	f.d.resize((size_t)a * b * 2);
	const size_t n = fread(f.d.data(), sizeof(float), f.d.size(), fp);
	fclose(fp);
	if (n != f.d.size()) return Fail("truncated data in " + path);
	return true;
}

static bool Reduce(const Flow& f, int gridW, int gridH, int block, Reduced& r, const std::string& what)
{
	const int k = block / 4;
	if (f.gw < gridW * k || f.gh < gridH * k) return Fail(what + ": field " + std::to_string(f.gw) + "x" + std::to_string(f.gh) + " cells smaller than the requested grid");
	r.x.assign((size_t)gridW * gridH, 0); r.y = r.x; r.t = r.x;
	for (int by = 0; by < gridH; ++by)
		for (int bx = 0; bx < gridW; ++bx)
		{
			double sx = 0, sy = 0; int n = 0; bool ok = true;
			for (int j = 0; j < k && ok; ++j)
				for (int i = 0; i < k; ++i)
				{
					const float* v = &f.d[(((size_t)(by * k + j)) * f.gw + (bx * k + i)) * 2];
					if (!std::isfinite(v[0]) || !std::isfinite(v[1])) { ok = false; break; }
					sx += v[0]; sy += v[1]; ++n;
				}
			const size_t o = (size_t)by * gridW + bx;
			if (!ok || n == 0) continue;
			const double mx = sx / n, my = sy / n;
			double ss = 0;
			for (int j = 0; j < k; ++j)
				for (int i = 0; i < k; ++i)
				{
					const float* v = &f.d[(((size_t)(by * k + j)) * f.gw + (bx * k + i)) * 2];
					ss += (v[0] - mx) * (v[0] - mx) + (v[1] - my) * (v[1] - my);
				}
			r.x[o] = (float)mx; r.y[o] = (float)my;
			r.t[o] = (float)std::min(1.0, std::max(0.0, 1.0 - std::sqrt(ss / n) / block));
		}
	return true;
}

static std::string FlowPath(const std::string& dir, int a, int b)
{
	char buf[64];
	snprintf(buf, sizeof buf, "/flow_%06d_%06d.npy", a, b);
	return dir + buf;
}

static bool ReadPair(const std::string& dir, int t, int gridW, int gridH, int block, Reduced& F, Reduced& B)
{
	Flow ff, bf;
	if (!ReadNpy(FlowPath(dir, t - 1, t), ff) || !ReadNpy(FlowPath(dir, t, t - 1), bf)) return false;
	return Reduce(ff, gridW, gridH, block, F, "fwd") && Reduce(bf, gridW, gridH, block, B, "bwd");
}

// Two-layer constant overrides (tuning): ring fraction, swap seconds, border veto ratio, continuity floor, competitor min ratio.
// A negative entry keeps the Config default.
static double g_tl[5] = { -1, -1, -1, -1, -1 };
// Dynamic-texture detector overrides (tuning): minShare, minTemporal, minSpatial, agreeRatio. Negative = Config default.
static double g_dt[4] = { -1, -1, -1, -1 };

static Config MakeConfig(bool phase1, bool twoLayer = false, bool dynTexture = false)
{
	Config c;
	c.twoLayer = twoLayer;
	c.dynTexture = dynTexture;
	if (g_dt[0] >= 0) c.dynTextureCfg.minShare = g_dt[0];
	if (g_dt[1] >= 0) c.dynTextureCfg.minTemporal = g_dt[1];
	if (g_dt[2] >= 0) c.dynTextureCfg.minSpatial = g_dt[2];
	if (g_dt[3] >= 0) c.dynTextureCfg.agreeRatio = g_dt[3];
	if (g_tl[0] >= 0) c.borderRingFraction = g_tl[0];
	if (g_tl[1] >= 0) c.layerSwapSeconds = g_tl[1];
	if (g_tl[2] >= 0) c.borderVetoRatio = g_tl[2];
	if (g_tl[3] >= 0) c.continuityFloor = g_tl[3];
	if (g_tl[4] >= 0) c.competitorMinRatio = g_tl[4];
	if (phase1) { c.inlierShareLo = 0.4; c.inlierShareHi = 0.9; c.temporalNoiseFloor = false; c.verdictGatesConfidence = false; }
	return c;
}

struct Geometry { int W = 0, H = 0, block = 16, gridW = 0, gridH = 0; double fps = 0; };

// One pair through a (stateful) fitter. The returned record holds its block data.
static CameraMotion FitPair(Fitter& fit, const Reduced& F, const Reduced* B, const Geometry& g, uint64_t t)
{
	PairInput in;
	in.gridW = g.gridW; in.gridH = g.gridH; in.blockSize = g.block;
	in.picture.left = 0; in.picture.top = 0; in.picture.right = g.W; in.picture.bottom = g.H;
	in.fwdX = F.x.data(); in.fwdY = F.y.data(); in.fwdTrust = F.t.data();
	if (B) { in.bwdX = B->x.data(); in.bwdY = B->y.data(); in.bwdTrust = B->t.data(); }
	in.pairSeconds = 1.0 / g.fps;
	in.seq = t; in.pairFrom = t - 1; in.pairTo = t;
	return fit.Fit(in);
}

static std::vector<uint8_t> LabelsOf(const CameraMotion& m, size_t n)
{
	std::vector<uint8_t> l(n, 0);
	if (m.blocks && m.blocks->label.size() == n)
		for (size_t i = 0; i < n; ++i) l[i] = (m.blocks->excluded.size() == n && m.blocks->excluded[i]) ? 0 : m.blocks->label[i];
	return l;
}

template <class T> static void Put(FILE* fp, T v) { fwrite(&v, sizeof(T), 1, fp); }

struct Args
{
	std::string mode, vec, ref, yuv, out, csv, dumpFlo;
	int N = 0, W = 0, H = 0, block = 16, bitdepth = 10, start = 2, count = 40;
	double fps = 0;
	bool phase1 = false, twoLayer = false, dynTexture = false;
};

static bool Parse(int argc, char** argv, Args& a)
{
	if (argc < 2) return Fail("usage: camera_eval fit|match ... (see README.md)");
	a.mode = argv[1];
	for (int i = 2; i < argc; ++i)
	{
		const std::string s = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) { Fail("missing value for " + s); exit(2); } return argv[++i]; };
		if (s == "--vec") a.vec = next();
		else if (s == "--ref") a.ref = next();
		else if (s == "--yuv") a.yuv = next();
		else if (s == "--out") a.out = next();
		else if (s == "--csv") a.csv = next();
		else if (s == "--dump-flo") a.dumpFlo = next();
		else if (s == "--frames") a.N = atoi(next());
		else if (s == "--w") a.W = atoi(next());
		else if (s == "--h") a.H = atoi(next());
		else if (s == "--block") a.block = atoi(next());
		else if (s == "--bitdepth") a.bitdepth = atoi(next());
		else if (s == "--start") a.start = atoi(next());
		else if (s == "--count") a.count = atoi(next());
		else if (s == "--fps") a.fps = atof(next());
		else if (s == "--phase1") a.phase1 = true;
		else if (s == "--two-layer") a.twoLayer = true;
		else if (s == "--dyn-texture") a.dynTexture = true;
		else if (s == "--dt-share") g_dt[0] = atof(next());
		else if (s == "--dt-temporal") g_dt[1] = atof(next());
		else if (s == "--dt-spatial") g_dt[2] = atof(next());
		else if (s == "--dt-agree") g_dt[3] = atof(next());
		else if (s == "--tl-ring") g_tl[0] = atof(next());
		else if (s == "--tl-swap") g_tl[1] = atof(next());
		else if (s == "--tl-veto") g_tl[2] = atof(next());
		else if (s == "--tl-floor") g_tl[3] = atof(next());
		else if (s == "--tl-comp") g_tl[4] = atof(next());
		else return Fail("unknown argument " + s);
	}
	return true;
}

static int RunFit(const Args& a)
{
	if (a.vec.empty() || a.out.empty() || a.N < 2 || a.W <= 0 || a.H <= 0 || a.fps <= 0 || a.block < 4 || a.block % 4)
	{
		Fail("fit: need --vec --out --frames(>=2) --w --h --fps and a --block that is a multiple of 4");
		return 2;
	}
	Geometry g; g.W = a.W; g.H = a.H; g.block = a.block; g.fps = a.fps; g.gridW = a.W / a.block; g.gridH = a.H / a.block;
	Fitter fit(MakeConfig(a.phase1, a.twoLayer, a.dynTexture));
	FILE* csv = nullptr;
	if (!a.csv.empty())
	{
		csv = fopen(a.csv.c_str(), "wb");
		if (!csv) { Fail("cannot write " + a.csv); return 2; }
		fprintf(csv, "pair,valid,type,reason,confidence,samples,hypotheses,inlierShare,precision,coverage,temporal,conditioning,engineAgreement,inverse,ambiguity,logit,h0,h1,h2,h3,h4,h5,h6,h7,h8,labUnknown,labCamera,labObject,labOverlay,fitUs,layerFlags,layerRisk,border,continuity,haveObject,objId,objBlocks,objH2,objH5,dynShare,dynTemporal,dynSpatial,dynField,verdictFlags\n");
	}
	struct Rec { int valid, type; double conf, h[9]; std::vector<uint8_t> label; };
	std::vector<Rec> recs;
	for (int t = 1; t < a.N; ++t)
	{
		Reduced F, B;
		if (!ReadPair(a.vec, t, g.gridW, g.gridH, g.block, F, B)) { if (csv) fclose(csv); return 2; }
		const auto tStart = std::chrono::steady_clock::now();
		const CameraMotion m = FitPair(fit, F, &B, g, (uint64_t)t);
		const double fitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tStart).count();
		Rec r; r.valid = m.valid ? 1 : 0; r.type = (int)m.params.type; r.conf = m.confidence;
		memcpy(r.h, m.params.h, sizeof(r.h));
		r.label = LabelsOf(m, (size_t)g.gridW * g.gridH);
		recs.push_back(r);
		if (csv)
		{
			double nl[4] = { 0, 0, 0, 0 };
			for (uint8_t l : r.label) if (l < 4) nl[l] += 1;
			const double nb = (double)r.label.size();
			const Signals& s = m.signals;
			fprintf(csv, "%d,%d,%d,%s,%.6f,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", t, r.valid, r.type, ReasonText(m.reason), m.confidence, m.samples, m.hypotheses,
				s.inlierShare, s.precision, s.coverage, s.temporal, s.conditioning, s.engineAgreement, s.inverse, s.ambiguity, s.logit);
			for (int i = 0; i < 9; ++i) fprintf(csv, ",%.9g", r.h[i]);
			fprintf(csv, ",%.5f,%.5f,%.5f,%.5f", nl[0] / nb, nl[1] / nb, nl[2] / nb, nl[3] / nb);
			const ObjectModel& ol = m.objectLayer;
			fprintf(csv, ",%.1f,%u,%.4f,%.4f,%.4f,%d,%u,%u,%.6g,%.6g,%.4f,%.4f,%.4f,%.4f,%u\n", fitUs, (unsigned)m.layerFlags, m.layerRisk, s.border, s.continuity,
				m.haveObjectLayer ? 1 : 0, (unsigned)ol.id, ol.blocks, ol.h[2], ol.h[5], s.dynShare, s.dynTemporal, s.dynSpatial, s.dynField, (unsigned)m.verdictFlags);
		}
	}
	if (csv) fclose(csv);
	FILE* fp = fopen(a.out.c_str(), "wb");
	if (!fp) { Fail("cannot write " + a.out); return 2; }
	fwrite("CAMF", 1, 4, fp);
	Put<int32_t>(fp, (int32_t)recs.size()); Put<int32_t>(fp, g.gridW); Put<int32_t>(fp, g.gridH); Put<double>(fp, (double)g.block);
	for (const Rec& r : recs)
	{
		Put<int32_t>(fp, r.valid); Put<int32_t>(fp, r.type); Put<double>(fp, r.conf);
		fwrite(r.h, sizeof(double), 9, fp);
		fwrite(r.label.data(), 1, r.label.size(), fp);
	}
	if (fclose(fp) != 0) { Fail("write failed: " + a.out); return 2; }
	return 0;
}

// ---- match ----
// Luma plane of frame `f` of a 4:2:0 planar file with 16-bit little-endian samples (bitdepth-bit values),
// box-downscaled by 2 into the matcher's half-size working picture, in 8-bit-like code values (the matcher's
// tolerance of 3 codes per sample is on that scale): v / 2^(bitdepth-8).
static bool ReadLuma(FILE* fp, int W, int H, int bitdepth, int f, std::vector<float>& out, int& mw, int& mh)
{
	const long long frameBytes = (long long)W * H * 3;
	if (_fseeki64(fp, frameBytes * f, SEEK_SET) != 0) return Fail("seek failed");
	std::vector<uint16_t> y((size_t)W * H);
	if (fread(y.data(), 2, y.size(), fp) != y.size()) return Fail("short read of frame " + std::to_string(f));
	mw = W / 2; mh = H / 2;
	out.assign((size_t)mw * mh, 0.f);
	const float scale = 1.0f / (float)(1 << (bitdepth - 8));
	for (int j = 0; j < mh; ++j)
		for (int i = 0; i < mw; ++i)
		{
			const size_t o = (size_t)(2 * j) * W + 2 * i;
			out[(size_t)j * mw + i] = 0.25f * scale * (float)(y[o] + y[o + 1] + y[o + W] + y[o + W + 1]);
		}
	return true;
}

// Expand block vectors (matcher px) to a dense source-px .flo of the full picture.
static bool WriteFlo(const std::string& path, const Reference::MatchResult& r, const Geometry& g, double sourceToMatcher)
{
	FILE* fp = fopen(path.c_str(), "wb");
	if (!fp) return Fail("cannot write " + path);
	const float magic = 202021.25f;
	const int32_t w = g.W, h = g.H;
	fwrite(&magic, 4, 1, fp); fwrite(&w, 4, 1, fp); fwrite(&h, 4, 1, fp);
	std::vector<float> row((size_t)g.W * 2);
	for (int y = 0; y < g.H; ++y)
	{
		const int by = std::min(y / g.block, r.bh - 1);
		for (int x = 0; x < g.W; ++x)
		{
			const int bx = std::min(x / g.block, r.bw - 1);
			row[(size_t)x * 2] = (float)(r.vx[(size_t)by * r.bw + bx] / sourceToMatcher);
			row[(size_t)x * 2 + 1] = (float)(r.vy[(size_t)by * r.bw + bx] / sourceToMatcher);
		}
		fwrite(row.data(), 4, row.size(), fp);
	}
	return fclose(fp) == 0 || Fail("write failed: " + path);
}

struct EpeStats { int n = 0; double mean = 0, median = 0, gt1 = 0, gt3 = 0; };

static EpeStats Stats(std::vector<double>& e)
{
	EpeStats s;
	s.n = (int)e.size();
	if (e.empty()) return s;
	double sum = 0; int g1 = 0, g3 = 0;
	for (double v : e) { sum += v; if (v > 1) ++g1; if (v > 3) ++g3; }
	std::sort(e.begin(), e.end());
	s.mean = sum / e.size(); s.median = e[e.size() / 2]; s.gt1 = (double)g1 / e.size(); s.gt3 = (double)g3 / e.size();
	return s;
}

static void PrintStats(FILE* f, const EpeStats& s) { fprintf(f, ",%d,%.4f,%.4f,%.4f,%.4f", s.n, s.mean, s.median, s.gt1, s.gt3); }

static int RunMatch(const Args& a)
{
	if (a.yuv.empty() || a.vec.empty() || a.ref.empty() || a.out.empty() || a.N < 3 || a.W <= 0 || a.H <= 0 || a.fps <= 0 || a.bitdepth < 8 || a.bitdepth > 16 || a.block < 4 || a.block % 4 || a.start < 2)
	{
		Fail("match: need --yuv --vec --ref --out --frames --w --h --fps (--bitdepth 10, --start >= 2)");
		return 2;
	}
	Geometry g; g.W = a.W; g.H = a.H; g.block = a.block; g.fps = a.fps; g.gridW = a.W / a.block; g.gridH = a.H / a.block;
	FILE* yuv = fopen(a.yuv.c_str(), "rb");
	if (!yuv) { Fail("cannot open " + a.yuv); return 2; }
	_fseeki64(yuv, 0, SEEK_END);
	const long long size = _ftelli64(yuv);
	if (size != (long long)a.N * a.W * a.H * 3) { fclose(yuv); Fail("yuv size " + std::to_string(size) + " != frames*W*H*3 (4:2:0, 16-bit samples)"); return 2; }

	Fitter fitIn(MakeConfig(a.phase1));        // the fit of the engine-side input (coded NVOF) under test
	Fitter fitRef(MakeConfig(false));          // reference labels: always the current defaults
	CameraMotion prevRecord;                   // causal record of pair t-1 from the input

	FILE* out = fopen(a.out.c_str(), "wb");
	if (!out) { fclose(yuv); Fail("cannot write " + a.out); return 2; }
	fprintf(out, "pair,planActive,prevConfidence,prevReason,fallback,narrowBlocks,fullBlocks,evalsFull,evalsGuided,msFull,msGuided,narrowAccepted,escalated,camBlocks");
	for (const char* run : { "off", "on" })
		for (const char* set : { "all", "cam", "rest" })
			fprintf(out, ",%s_%s_n,%s_%s_mean,%s_%s_median,%s_%s_gt1,%s_%s_gt3", run, set, run, set, run, set, run, set, run, set);
	fprintf(out, "\n");

	const int last = std::min(a.N - 1, a.start + a.count - 1);
	std::vector<float> lumaPrev, lumaCur; int mw = 0, mh = 0;
	std::vector<int> skippedPairs;   // --dump-flo: pairs where the plan skipped the matcher
	for (int t = 1; t <= last; ++t)
	{
		// The input's causal fit of pair t (frames t-1 -> t); the plan for pair t+1 is built from it.
		Reduced F, B;
		if (!ReadPair(a.vec, t, g.gridW, g.gridH, g.block, F, B)) { fclose(out); fclose(yuv); return 2; }
		const CameraMotion rec = FitPair(fitIn, F, &B, g, (uint64_t)t);

		// Reference field and camera labels of pair t.
		Reduced RF, RB;
		if (!ReadPair(a.ref, t, g.gridW, g.gridH, g.block, RF, RB)) { fclose(out); fclose(yuv); return 2; }
		const CameraMotion refRec = FitPair(fitRef, RF, &RB, g, (uint64_t)t);

		if (t >= a.start)
		{
			// Match frames t-1 -> t with the plan built from the PREVIOUS pair's record (prevRecord).
			if (!ReadLuma(yuv, a.W, a.H, a.bitdepth, t - 1, lumaPrev, mw, mh)) { fclose(out); fclose(yuv); return 2; }
			if (!ReadLuma(yuv, a.W, a.H, a.bitdepth, t, lumaCur, mw, mh)) { fclose(out); fclose(yuv); return 2; }
			Reference::Plane from, to;
			from.w = to.w = mw; from.h = to.h = mh; from.px = lumaPrev.data(); to.px = lumaCur.data();

			MeasurementSwitches sw; sw.narrowWindow = true; sw.guidedCandidate = true;
			MeasurementInput mi; mi.prev = &prevRecord; mi.nextPairSeconds = 1.0 / g.fps;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), mi);

			const Reference::MatchConfig mc;
			const Reference::MatchResult off = Reference::Match(mc, from, to, nullptr, nullptr);
			const Reference::MatchResult on = Reference::Match(mc, from, to, plan.Active() ? &plan : nullptr, nullptr);
			if (off.bw != g.gridW || off.bh != g.gridH)
			{
				fclose(out); fclose(yuv);
				Fail("matcher grid " + std::to_string(off.bw) + "x" + std::to_string(off.bh) + " != fitter grid " + std::to_string(g.gridW) + "x" + std::to_string(g.gridH));
				return 2;
			}
			if (!a.dumpFlo.empty())
			{
				char sub[64];
				snprintf(sub, sizeof sub, "/pair_%06d", t);
				const std::string d = a.dumpFlo + sub;
				_mkdir(a.dumpFlo.c_str()); _mkdir(d.c_str());   // one level each; parents must exist
				// A plan that legitimately skips the matcher leaves pred_on empty: write the plan's seed field for that pair instead
				// and list the pair in DIR/skipped.txt (the CSV is unchanged).
				Reference::MatchResult onFlo = on;
				if (on.skipped)
				{
					const size_t nb = (size_t)on.bw * on.bh;
					const bool haveSeed = plan.seedX.size() == nb && plan.seedY.size() == nb;
					for (size_t i = 0; i < nb; ++i)
					{
						onFlo.vx[i] = haveSeed ? (float)(plan.seedX[i] * mc.sourceToMatcher) : 0.f;
						onFlo.vy[i] = haveSeed ? (float)(plan.seedY[i] * mc.sourceToMatcher) : 0.f;
					}
					skippedPairs.push_back(t);
					if (FILE* sk = fopen((a.dumpFlo + "/skipped.txt").c_str(), "wb"))
					{
						fprintf(sk, "# pairs whose plan skipped the matcher; pred_on.flo holds the plan seed field (zeros if none)\n");
						for (int sp : skippedPairs) fprintf(sk, "%d\n", sp);
						fclose(sk);
					}
				}
				if (!WriteFlo(d + "/pred_off.flo", off, g, mc.sourceToMatcher) || !WriteFlo(d + "/pred_on.flo", onFlo, g, mc.sourceToMatcher))
				{ fclose(out); fclose(yuv); return 2; }
			}
			const std::vector<uint8_t> refLab = LabelsOf(refRec, (size_t)g.gridW * g.gridH);
			int cam = 0; for (uint8_t l : refLab) if (l == 1) ++cam;
			fprintf(out, "%d,%d,%.4f,%s,%s,%u,%u,%llu,%llu,%.3f,%.3f,%u,%u,%d", t, plan.Active() ? 1 : 0, prevRecord.confidence, ReasonText(prevRecord.reason),
				plan.fallbackReason ? plan.fallbackReason : "", plan.counters.narrowBlocks, plan.counters.fullBlocks,
				(unsigned long long)off.evaluations, (unsigned long long)on.evaluations, off.ms, on.ms, on.narrowAccepted, on.escalated, cam);
			for (const Reference::MatchResult* r : { &off, &on })
			{
				std::vector<double> eAll, eCam, eRest;
				for (size_t i = 0; i < refLab.size(); ++i)
				{
					if (RF.t[i] <= 0.f) continue;   // the reference block has no valid vector
					// matcher px -> source px (sourceToMatcher 0.5). Direction: frame t-1 -> t, the flow files' convention.
					const double ex = r->vx[i] / mc.sourceToMatcher - RF.x[i], ey = r->vy[i] / mc.sourceToMatcher - RF.y[i];
					const double e = std::hypot(ex, ey);
					eAll.push_back(e);
					(refLab[i] == 1 ? eCam : eRest).push_back(e);
				}
				PrintStats(out, Stats(eAll)); PrintStats(out, Stats(eCam)); PrintStats(out, Stats(eRest));
			}
			fprintf(out, "\n");
			fflush(out);
		}
		prevRecord = rec;
	}
	fclose(out); fclose(yuv);
	return 0;
}

int main(int argc, char** argv)
{
	Args a;
	if (!Parse(argc, argv, a)) return 2;
	if (a.mode == "fit") return RunFit(a);
	if (a.mode == "match") return RunMatch(a);
	Fail("unknown mode " + a.mode);
	return 2;
}
