// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// camfit_cli: offline CPU driver for VP's global camera model fitter (CameraModel::Fitter) on block
// motion fields read from numpy .npy files.
//
//   camfit_cli --vec <dir> --frames N --w W --h H --block 16 --fps F --out <file.camf>
//   camfit_cli --selftest
//
// Input: <dir>/flow_%06d_%06d.npy (float32 LE, C order, shape (gh4, gw4, 2), 4 px grid, ch0 dx, ch1 dy;
// frame_b(p + F(p)) ~ frame_a(p) for file flow_a_b). For pair t = 1..N-1 the forward field is
// flow_(t-1)_(t) (frame t-1 grid) and the backward field flow_(t)_(t-1) (frame t grid). Each is reduced
// to the fitter grid: gridW = floor(W/block), gridH = floor(H/block) (a partial last row/column is
// dropped so block centres stay exact), vector = mean of the 4 px cells inside the block, trust =
// clamp(1 - RMS spread of the cells / block, 0, 1) with spread = distance of a cell vector from the mean.
// One stateful Fitter (default Config) runs the pairs in order.
//
// Output .camf, little-endian:
//   char[4] "CAMF"; i32 pairs; i32 gridW; i32 gridH; f64 blockPx;
//   per pair: i32 valid (CameraMotion::valid), i32 modelType (0 None,1 Translation,2 Similarity,
//             3 Affine,4 Homography), f64 confidence, f64 h[9], u8 label[gridW*gridH].
// h = CameraMotion::params.h: row-major 3x3 homogeneous matrix mapping a point in frame t-1 to frame t
//   (the actual pair), in SOURCE pixels with origin at the CENTRE of the picture (x right, y down).
//   Block (bx,by) centre = ((bx+0.5)*block - W/2, (by+0.5)*block - H/2). Pure translation: h[2]=dx, h[5]=dy
//   (displacement of content from t-1 to t, same sign as the forward flow). Identity when not valid.
// label = record.blocks->label per block: 0 Unknown/outlier/excluded (bars, untrusted), 1 camera inlier,
//   2 object (only if the fitter's object tracker is enabled; default Config has objects=false, so
//   expect 0/1), 3 overlay (static logo / zero-motion cluster). All 0 when the record has no block data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "../../src/VP-V/modules/motion/estimation/global_model/GlobalModelFit.h"

using namespace CameraModel;

struct Flow { int gh = 0, gw = 0; std::vector<float> d; };   // d: gh*gw*2

static bool Fail(const std::string& m) { fprintf(stderr, "camfit_cli: error: %s\n", m.c_str()); return false; }

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
	if (h.find("'<f4'") == std::string::npos && h.find("\"<f4\"") == std::string::npos) { fclose(fp); return Fail("dtype is not '<f4' in " + path + ": " + h); }
	const size_t fo = h.find("fortran_order");
	if (fo == std::string::npos || h.find("False", fo) == std::string::npos) { fclose(fp); return Fail("fortran_order is not False in " + path); }
	const size_t sp = h.find("shape");
	const size_t lp = sp == std::string::npos ? sp : h.find('(', sp);
	if (lp == std::string::npos) { fclose(fp); return Fail("no shape in " + path); }
	int a = 0, b = 0, c = 0;
	if (sscanf(h.c_str() + lp, "(%d,%d,%d", &a, &b, &c) != 3 || c != 2 || a <= 0 || b <= 0) { fclose(fp); return Fail("shape is not (gh, gw, 2) in " + path + ": " + h); }
	f.gh = a; f.gw = b;
	f.d.resize((size_t)a * b * 2);
	const size_t n = fread(f.d.data(), sizeof(float), f.d.size(), fp);
	fclose(fp);
	if (n != f.d.size()) return Fail("truncated data in " + path);
	return true;
}

struct Reduced { std::vector<float> x, y, t; };

// Reduce a 4 px-grid field to block-size blocks (mean; trust from the spread of the cells).
static bool Reduce(const Flow& f, int gridW, int gridH, int block, Reduced& r, const std::string& what)
{
	const int k = block / 4;
	if (f.gw < gridW * k || f.gh < gridH * k) return Fail(what + ": field " + std::to_string(f.gw) + "x" + std::to_string(f.gh) + " cells smaller than the requested " + std::to_string(gridW * k) + "x" + std::to_string(gridH * k));
	r.x.assign((size_t)gridW * gridH, 0); r.y = r.x; r.t = r.x;
	for (int by = 0; by < gridH; ++by)
		for (int bx = 0; bx < gridW; ++bx)
		{
			double sx = 0, sy = 0; int n = 0; bool ok = true;
			for (int j = 0; j < k && ok; ++j)
				for (int i = 0; i < k; ++i)
				{
					const float* v = &f.d[(((size_t)(by * k + j)) * f.gw + (bx * k + i)) * 2];
					if (!IsFiniteBits(v[0]) || !IsFiniteBits(v[1])) { ok = false; break; }
					sx += v[0]; sy += v[1]; ++n;
				}
			const size_t o = (size_t)by * gridW + bx;
			if (!ok || n == 0) continue;   // trust 0
			const double mx = sx / n, my = sy / n;
			double ss = 0;
			for (int j = 0; j < k; ++j)
				for (int i = 0; i < k; ++i)
				{
					const float* v = &f.d[(((size_t)(by * k + j)) * f.gw + (bx * k + i)) * 2];
					ss += (v[0] - mx) * (v[0] - mx) + (v[1] - my) * (v[1] - my);
				}
			const double rms = std::sqrt(ss / n);
			r.x[o] = (float)mx; r.y[o] = (float)my;
			r.t[o] = (float)std::min(1.0, std::max(0.0, 1.0 - rms / block));
		}
	return true;
}

struct PairOut { int valid = 0, type = 0; double conf = 0, h[9] = { 1,0,0,0,1,0,0,0,1 }; std::vector<uint8_t> label; Reason reason = Reason::NoInput; };

static PairOut RunPair(Fitter& fit, const Reduced& F, const Reduced& B, int gridW, int gridH, int block, int W, int H, double fps, uint64_t t)
{
	PairInput in;
	in.gridW = gridW; in.gridH = gridH; in.blockSize = block;
	in.picture.left = 0; in.picture.top = 0; in.picture.right = W; in.picture.bottom = H;
	in.fwdX = F.x.data(); in.fwdY = F.y.data(); in.fwdTrust = F.t.data();
	in.bwdX = B.x.data(); in.bwdY = B.y.data(); in.bwdTrust = B.t.data();
	in.pairSeconds = 1.0 / fps;
	in.seq = t; in.pairFrom = t - 1; in.pairTo = t;
	const CameraMotion m = fit.Fit(in);
	PairOut o;
	o.valid = m.valid ? 1 : 0; o.type = (int)m.params.type; o.conf = m.confidence; o.reason = m.reason;
	memcpy(o.h, m.params.h, sizeof(o.h));
	o.label.assign((size_t)gridW * gridH, 0);
	if (m.blocks && m.blocks->label.size() == o.label.size())
		for (size_t i = 0; i < o.label.size(); ++i) o.label[i] = (m.blocks->excluded.size() == o.label.size() && m.blocks->excluded[i]) ? 0 : m.blocks->label[i];
	return o;
}

static int SelfTest()
{
	const int W = 1920, H = 1080, block = 16, gw4 = W / 4, gh4 = H / 4;
	const int gridW = W / block, gridH = H / block;
	const float dx = 2.25f, dy = -1.5f;
	Flow ff, bf;
	ff.gh = bf.gh = gh4; ff.gw = bf.gw = gw4;
	ff.d.resize((size_t)gh4 * gw4 * 2); bf.d.resize(ff.d.size());
	for (size_t i = 0; i < (size_t)gh4 * gw4; ++i) { ff.d[2*i] = dx; ff.d[2*i+1] = dy; bf.d[2*i] = -dx; bf.d[2*i+1] = -dy; }
	Reduced F, B;
	if (!Reduce(ff, gridW, gridH, block, F, "selftest fwd") || !Reduce(bf, gridW, gridH, block, B, "selftest bwd")) return 2;
	Fitter fit;
	bool pass = true;
	for (uint64_t t = 1; t <= 3; ++t)
	{
		const PairOut o = RunPair(fit, F, B, gridW, gridH, block, W, H, 30.0, t);
		printf("pair %llu valid=%d type=%d conf=%.3f reason=%s tx=%.4f ty=%.4f\n", (unsigned long long)t, o.valid, o.type, o.conf, ReasonText(o.reason), o.h[2], o.h[5]);
		if (!o.valid || std::fabs(o.h[2] - dx) > 0.05 || std::fabs(o.h[5] - dy) > 0.05) pass = false;
	}
	printf(pass ? "PASS\n" : "FAIL\n");
	return pass ? 0 : 1;
}

template <class T> static void Put(FILE* fp, T v) { fwrite(&v, sizeof(T), 1, fp); }

int main(int argc, char** argv)
{
	std::string vec, out; int N = 0, W = 0, H = 0, block = 16; double fps = 0; bool self = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) { Fail("missing value for " + a); exit(2); } return argv[++i]; };
		if (a == "--selftest") self = true;
		else if (a == "--vec") vec = next();
		else if (a == "--out") out = next();
		else if (a == "--frames") N = atoi(next());
		else if (a == "--w") W = atoi(next());
		else if (a == "--h") H = atoi(next());
		else if (a == "--block") block = atoi(next());
		else if (a == "--fps") fps = atof(next());
		else { Fail("unknown argument " + a); return 2; }
	}
	if (self) return SelfTest();
	if (vec.empty() || out.empty() || N < 2 || W <= 0 || H <= 0 || fps <= 0 || block < 4 || block % 4) { Fail("usage: camfit_cli --vec <dir> --frames N(>=2) --w W --h H --block 16 (multiple of 4) --fps F --out <file.camf> | --selftest"); return 2; }
	const int gridW = W / block, gridH = H / block;
	Fitter fit;
	std::vector<PairOut> res;
	auto path = [&](int a, int b) { char buf[64]; snprintf(buf, sizeof buf, "/flow_%06d_%06d.npy", a, b); return vec + buf; };
	for (int t = 1; t < N; ++t)
	{
		Flow ff, bf; Reduced F, B;
		if (!ReadNpy(path(t - 1, t), ff) || !ReadNpy(path(t, t - 1), bf)) return 2;
		if (!Reduce(ff, gridW, gridH, block, F, "fwd") || !Reduce(bf, gridW, gridH, block, B, "bwd")) return 2;
		res.push_back(RunPair(fit, F, B, gridW, gridH, block, W, H, fps, (uint64_t)t));
		const PairOut& o = res.back();
		printf("pair %d valid=%d type=%d conf=%.3f reason=%s tx=%.4f ty=%.4f\n", t, o.valid, o.type, o.conf, ReasonText(o.reason), o.h[2], o.h[5]);
		if (!o.valid) fprintf(stderr, "camfit_cli: pair %d not usable: %s\n", t, ReasonText(o.reason));
	}
	FILE* fp = fopen(out.c_str(), "wb");
	if (!fp) { Fail("cannot write " + out); return 2; }
	fwrite("CAMF", 1, 4, fp);
	Put<int32_t>(fp, (int32_t)res.size()); Put<int32_t>(fp, gridW); Put<int32_t>(fp, gridH); Put<double>(fp, (double)block);
	for (const PairOut& o : res)
	{
		Put<int32_t>(fp, o.valid); Put<int32_t>(fp, o.type); Put<double>(fp, o.conf);
		fwrite(o.h, sizeof(double), 9, fp);
		fwrite(o.label.data(), 1, o.label.size(), fp);
	}
	if (fclose(fp) != 0) { Fail("write failed: " + out); return 2; }
	return 0;
}
