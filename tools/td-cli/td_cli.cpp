// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// td_cli: CPU command-line driver for the TemporalDetail reference model
// (src/VP-V/modules/repair/temporal_detail/TemporalDetailModel.h). Measurement tool only: no GPU, no
// renderer, no config. One Model per run (= per stream), Process() per frame in source order.
//
// Code values: the model is calibrated in 8-bit-equivalent codes (its absolute floors, e.g. the base-match
// noise allowance of 0.75 codes, are 8-bit codes), so luma of bit depth bd is divided by 2^(bd-8) on the way
// in and the model output multiplied back (exact: a pixel the model leaves alone returns bit-identical).
//
// Motion: block vectors in plane pixels on a block grid (MotionPair, TemporalDetailMotion.h): Backward(seq) =
// pair seq -> seq-1 on frame seq's grid, Forward(seq) = pair seq-1 -> seq on frame seq-1's grid. VP's vector
// engine needs the GPU, so vectors come from an offline stand-in (tools/td-cli/td_flow.py: OpenCV DIS flow).
//
// Modes
//   td_cli --stdio
//       Binary pipe for the evaluation harness (tools/eval/candidates_temporal_detail.py). stdin: one text line
//       "TDCLI1 <w> <h> <bitdepth> <hz> <planeLines> <mode off|steady> <budget> <clean 0|1> <guard 0|1>
//        <strength> <maxAge> <boostMax> [<strongerMargin> <baseNoise> <baseGrainSigmas> <uncPx> [<ax0> <ay0> <ax1> <ay1> [<forceWeight>]]]\n",
//       then per frame:
//         "FRM1" u64 seq, i32 flags (1 cut, 2 fade), f64 luma[w*h] (codes at <bitdepth>),
//         i32 hasB [i32 gw, i32 gh, f64 blockPx, f32 vx[n], f32 vy[n], f32 trust[n]],
//         i32 hasF [same]
//       and "END1". stdout per frame: "OUT1", f64 luma[w*h] (codes, unrounded), f64 ms, i32 len, char stats[len].
//   td_cli --in <file.yuv|file.y4m> --vectors <file.tdv> --out <dir> [--w W --h H --pix-fmt yuv420p|yuv420p10le]
//          [--hz 24] [--lines 1080] [--start-index 0] [--frames N] [--mode steady|off] [--budget 0.08]
//          [--no-clean] [--no-guard] [--strength 1] [--max-age 2] [--boost-max 0.15] [--stronger-margin 0.15]
//          [--base-noise 0.75] [--base-grain-sigmas 3] [--unc-px 0.5] [--active x0,y0,x1,y1]
//       Reads planar 4:2:0 frames (luma processed, chroma passed through), writes the evaluation harness capture
//       directory (NNNNNN.yuv + capture.json, tools/eval/README.md) with source_frame_index = start + n, and
//       td_stats.txt (per-frame CPU ms and model diagnostics).
//
// Vector file (.tdv, little-endian, written by td_flow.py): "TDV1", i32 frames, i32 gw, i32 gh, f64 blockPx,
// i32 w, i32 h; per frame: i32 hasB, i32 hasF, f32 bvx[n], bvy[n], btrust[n], fvx[n], fvy[n], ftrust[n].
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailMotion.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
	using namespace TemporalDetail;

	struct Options
	{
		int w = 0, h = 0, bitdepth = 8;
		double hz = 23.976, lines = 1080.0;
		std::string mode = "steady";
		double budget = kGrainBudget;
		bool clean = true, guard = true;
		double strength = 1.0, maxAge = 2.0, boostMax = 0.15;
		double strongerMargin = kStrongerMargin, baseNoise = kBaseNoiseCodes, baseGrainSigmas = kBaseGrainSigmas, uncPx = kUncertaintyPx;
		int active[4] = { 0, 0, 0, 0 };   // x0 y0 x1 y1; x1/y1 <= 0 = whole plane
		double forceWeight = 0.0;          // measurement: transfer ceiling (Model::Tuning::forceWeight)
	};

	[[noreturn]] void Die(const std::string& why)
	{
		std::fprintf(stderr, "td_cli: %s\n", why.c_str());
		std::exit(2);
	}

	Model MakeModel(const Options& o)
	{
		Settings s;
		s.mode = (o.mode == "off") ? Mode::Off : Mode::Steady;
		s.strength = o.strength;
		s.maxAgeSeconds = o.maxAge;
		s.boostMax = o.boostMax;
		Model m(s);
		Model::Tuning t;
		t.grainBudget = o.budget;
		t.templateClean = o.clean;
		t.guard = o.guard;
		t.strongerMargin = o.strongerMargin;
		t.baseNoiseCodes = o.baseNoise;
		t.baseGrainSigmas = o.baseGrainSigmas;
		t.uncertaintyPx = o.uncPx;
		t.forceWeight = o.forceWeight;
		m.SetTuning(t);
		return m;
	}

	std::string StatsLine(const Model& m, double ms)
	{
		const Diagnostics& d = m.Diag();
		double wsum = 0.0, wmax = 0.0, bsum = 0.0;
		int active = 0;
		for (double w : d.weight) { wsum += w; if (w > wmax) wmax = w; if (w > 0.05) ++active; }
		for (double b : d.boost) bsum += b;
		const double nb = d.weight.empty() ? 1.0 : static_cast<double>(d.weight.size());
		// gate means over all blocks (why the module does or does not act); 0 where a block has no reference
		auto mean = [&](const std::vector<double>& v) { double s = 0.0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
		char buf[1024];
		std::snprintf(buf, sizeof buf,
			"{\"ms\":%.3f,\"status\":\"%s\",\"weight_mean\":%.6f,\"weight_max\":%.6f,\"active_share\":%.6f,"
			"\"boost_mean\":%.6f,\"live_slots\":%d,\"refreshes\":%d,\"retires\":%d,\"resets\":%d,"
			"\"guard_trips\":%d,\"guard_active\":%d,\"addback_blocks\":%d,"
			"\"g_evidence\":%.5f,\"g_hard\":%.5f,\"g_target\":%.5f,\"g_stronger\":%.5f,\"g_highband\":%.5f,\"g_focus\":%.5f,"
			"\"g_corr\":%.5f,\"g_base\":%.5f,\"g_unc_px\":%.5f,\"g_tau1\":%.5f,\"g_tau2\":%.5f,\"g_rebounds\":%.5f}",
			ms, d.status.c_str(), wsum / nb, wmax, active / nb, bsum / nb, d.liveSlots, d.refreshes, d.retires, d.resets,
			d.guardTrips, d.guardActive, d.addBackBlocks,
			mean(d.evidence), mean(d.hard), mean(d.target), mean(d.stronger), mean(d.highBand), mean(d.focus),
			mean(d.corrGate), mean(d.baseMatch), mean(d.uncertainty), mean(d.tau1), mean(d.tau2), mean(d.rebounds));
		return buf;
	}

	// ---------------------------------------------------------------- binary IO
	void ReadExact(std::FILE* f, void* p, size_t n)
	{
		if (n && std::fread(p, 1, n, f) != n) Die("short read");
	}
	template <class T> T ReadPod(std::FILE* f) { T v{}; ReadExact(f, &v, sizeof v); return v; }
	void WriteExact(std::FILE* f, const void* p, size_t n)
	{
		if (n && std::fwrite(p, 1, n, f) != n) Die("short write");
	}

	bool ReadPair(std::FILE* f, MotionPair& m)
	{
		const int32_t has = ReadPod<int32_t>(f);
		if (!has) { m = MotionPair(); return false; }
		m.valid = true;
		m.gridW = ReadPod<int32_t>(f);
		m.gridH = ReadPod<int32_t>(f);
		m.blockPx = ReadPod<double>(f);
		if (m.gridW <= 0 || m.gridH <= 0 || m.gridW > 4096 || m.gridH > 4096 || !(m.blockPx > 0.0)) Die("bad motion grid");
		const size_t n = static_cast<size_t>(m.gridW) * m.gridH;
		m.vx.resize(n); m.vy.resize(n); m.trust.resize(n);
		ReadExact(f, m.vx.data(), n * sizeof(float));
		ReadExact(f, m.vy.data(), n * sizeof(float));
		ReadExact(f, m.trust.data(), n * sizeof(float));
		return true;
	}

	int RunStdio()
	{
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);
#endif
		char line[512] = {};
		if (!std::fgets(line, sizeof line, stdin)) Die("no header");
		Options o;
		char tag[16] = {}, mode[16] = {};
		int clean = 1, guard = 1;
		const int got = std::sscanf(line, "%15s %d %d %d %lf %lf %15s %lf %d %d %lf %lf %lf %lf %lf %lf %lf %d %d %d %d %lf", tag, &o.w,
			&o.h, &o.bitdepth, &o.hz, &o.lines, mode, &o.budget, &clean, &guard, &o.strength, &o.maxAge, &o.boostMax,
			&o.strongerMargin, &o.baseNoise, &o.baseGrainSigmas, &o.uncPx, &o.active[0], &o.active[1], &o.active[2], &o.active[3], &o.forceWeight);
		if ((got != 13 && got != 17 && got != 21 && got != 22) || std::strcmp(tag, "TDCLI1") != 0) Die("bad header");
		if (o.w <= 0 || o.h <= 0 || o.bitdepth < 8 || o.bitdepth > 16) Die("bad geometry");
		o.mode = mode;
		o.clean = clean != 0;
		o.guard = guard != 0;
		Model model = MakeModel(o);
		const double k = std::ldexp(1.0, o.bitdepth - 8);
		const size_t n = static_cast<size_t>(o.w) * o.h;
		std::vector<double> buf(n);
		for (;;)
		{
			char t[4];
			ReadExact(stdin, t, 4);
			if (std::memcmp(t, "END1", 4) == 0) break;
			if (std::memcmp(t, "FRM1", 4) != 0) Die("bad frame tag");
			FieldMotionSource motion;
			Model::FrameIn in;
			in.seq = ReadPod<uint64_t>(stdin);
			const int32_t flags = ReadPod<int32_t>(stdin);
			ReadExact(stdin, buf.data(), n * sizeof(double));
			in.luma = Plane(o.w, o.h);
			for (size_t i = 0; i < n; ++i) in.luma.v[i] = buf[i] / k;
			MotionPair b, f;
			if (ReadPair(stdin, b)) motion.SetBackward(in.seq, b);
			if (ReadPair(stdin, f)) motion.SetForward(in.seq, f);
			in.contentHz = o.hz;
			in.planeLines = o.lines;
			in.cut = (flags & 1) != 0;
			in.fade = (flags & 2) != 0;
			in.motion = &motion;
			in.activeX0 = o.active[0]; in.activeY0 = o.active[1]; in.activeX1 = o.active[2]; in.activeY1 = o.active[3];
			const auto t0 = std::chrono::steady_clock::now();
			const Plane out = model.Process(in);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (out.w != o.w || out.h != o.h) Die("model returned a different raster");
			for (size_t i = 0; i < n; ++i) buf[i] = out.v[i] * k;
			const std::string st = StatsLine(model, ms);
			WriteExact(stdout, "OUT1", 4);
			WriteExact(stdout, buf.data(), n * sizeof(double));
			WriteExact(stdout, &ms, sizeof ms);
			const int32_t len = static_cast<int32_t>(st.size());
			WriteExact(stdout, &len, sizeof len);
			WriteExact(stdout, st.data(), st.size());
			std::fflush(stdout);
		}
		return 0;
	}

	// ---------------------------------------------------------------- file mode
	struct Reader
	{
		std::FILE* f = nullptr;
		bool y4m = false;
		int w = 0, h = 0, bitdepth = 8;
		size_t frameBytes() const { const size_t s = bitdepth > 8 ? 2 : 1; return static_cast<size_t>(w) * h * 3 / 2 * s; }

		void Open(const std::string& path, int ww, int hh, const std::string& pixFmt)
		{
			f = std::fopen(path.c_str(), "rb");
			if (!f) Die("cannot open " + path);
			y4m = path.size() > 4 && path.substr(path.size() - 4) == ".y4m";
			if (y4m)
			{
				std::string hdr;
				int c;
				while ((c = std::fgetc(f)) != EOF && c != '\n') hdr.push_back(static_cast<char>(c));
				if (hdr.compare(0, 9, "YUV4MPEG2") != 0) Die("not a y4m file");
				std::istringstream ss(hdr);
				std::string tok;
				std::string cs = "420";
				while (ss >> tok)
				{
					if (tok[0] == 'W') w = std::atoi(tok.c_str() + 1);
					else if (tok[0] == 'H') h = std::atoi(tok.c_str() + 1);
					else if (tok[0] == 'C') cs = tok.substr(1);
				}
				if (cs.compare(0, 3, "420") != 0) Die("y4m: only 4:2:0 supported (C" + cs + ")");
				bitdepth = (cs.find("p10") != std::string::npos) ? 10 : (cs.find("p12") != std::string::npos) ? 12 : 8;
			}
			else
			{
				w = ww; h = hh;
				if (pixFmt == "yuv420p") bitdepth = 8;
				else if (pixFmt == "yuv420p10le") bitdepth = 10;
				else if (pixFmt == "yuv420p12le") bitdepth = 12;
				else if (pixFmt == "yuv420p16le") bitdepth = 16;
				else Die("unsupported pix_fmt " + pixFmt);
			}
			if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) Die("bad raster");
		}

		bool Next(std::vector<uint8_t>& raw)
		{
			if (y4m)
			{
				std::string tag;
				int c;
				while ((c = std::fgetc(f)) != EOF && c != '\n') tag.push_back(static_cast<char>(c));
				if (c == EOF) return false;
				if (tag.compare(0, 5, "FRAME") != 0) Die("y4m: bad frame header");
			}
			raw.resize(frameBytes());
			return std::fread(raw.data(), 1, raw.size(), f) == raw.size();
		}
	};

	std::string Arg(int argc, char** argv, const char* name, const char* def)
	{
		for (int i = 1; i + 1 < argc; ++i)
			if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
		return def ? def : "";
	}
	bool Flag(int argc, char** argv, const char* name)
	{
		for (int i = 1; i < argc; ++i)
			if (std::strcmp(argv[i], name) == 0) return true;
		return false;
	}

	int RunFiles(int argc, char** argv)
	{
		Options o;
		const std::string inPath = Arg(argc, argv, "--in", ""), vecPath = Arg(argc, argv, "--vectors", ""),
			outDir = Arg(argc, argv, "--out", ""), pixFmt = Arg(argc, argv, "--pix-fmt", "yuv420p10le");
		if (inPath.empty() || vecPath.empty() || outDir.empty()) Die("--in, --vectors and --out are required");
		o.hz = std::atof(Arg(argc, argv, "--hz", "23.976").c_str());
		o.lines = std::atof(Arg(argc, argv, "--lines", "0").c_str());
		o.mode = Arg(argc, argv, "--mode", "steady");
		o.budget = std::atof(Arg(argc, argv, "--budget", "0.08").c_str());
		o.clean = !Flag(argc, argv, "--no-clean");
		o.guard = !Flag(argc, argv, "--no-guard");
		o.strength = std::atof(Arg(argc, argv, "--strength", "1").c_str());
		o.maxAge = std::atof(Arg(argc, argv, "--max-age", "2").c_str());
		o.boostMax = std::atof(Arg(argc, argv, "--boost-max", "0.15").c_str());
		o.strongerMargin = std::atof(Arg(argc, argv, "--stronger-margin", "0.15").c_str());
		o.baseNoise = std::atof(Arg(argc, argv, "--base-noise", "0.75").c_str());
		o.baseGrainSigmas = std::atof(Arg(argc, argv, "--base-grain-sigmas", "3").c_str());
		o.uncPx = std::atof(Arg(argc, argv, "--unc-px", "0.5").c_str());
		if (std::sscanf(Arg(argc, argv, "--active", "0,0,0,0").c_str(), "%d,%d,%d,%d", &o.active[0], &o.active[1], &o.active[2], &o.active[3]) != 4)
			Die("--active x0,y0,x1,y1");
		const long long start = std::atoll(Arg(argc, argv, "--start-index", "0").c_str());
		const long long maxFrames = std::atoll(Arg(argc, argv, "--frames", "0").c_str());
		Reader rd;
		rd.Open(inPath, std::atoi(Arg(argc, argv, "--w", "0").c_str()), std::atoi(Arg(argc, argv, "--h", "0").c_str()), pixFmt);
		o.w = rd.w; o.h = rd.h; o.bitdepth = rd.bitdepth;
		if (o.lines <= 0.0) o.lines = o.h;

		std::FILE* vf = std::fopen(vecPath.c_str(), "rb");
		if (!vf) Die("cannot open " + vecPath);
		char vt[4];
		ReadExact(vf, vt, 4);
		if (std::memcmp(vt, "TDV1", 4) != 0) Die("bad vector file");
		const int32_t vFrames = ReadPod<int32_t>(vf), gw = ReadPod<int32_t>(vf), gh = ReadPod<int32_t>(vf);
		const double blockPx = ReadPod<double>(vf);
		const int32_t vw = ReadPod<int32_t>(vf), vh = ReadPod<int32_t>(vf);
		if (vw != o.w || vh != o.h) Die("vector file raster differs from the input");
		const size_t gn = static_cast<size_t>(gw) * gh;

		Model model = MakeModel(o);
		const double k = std::ldexp(1.0, o.bitdepth - 8);
		const size_t n = static_cast<size_t>(o.w) * o.h;
		const bool wide = o.bitdepth > 8;
		std::vector<uint8_t> raw;
		std::ofstream stats(outDir + "/td_stats.txt");
		std::ostringstream frames;
		long long count = 0;
		while ((maxFrames <= 0 || count < maxFrames) && rd.Next(raw))
		{
			if (count >= vFrames) Die("vector file has fewer frames than the input");
			const int32_t hasB = ReadPod<int32_t>(vf), hasF = ReadPod<int32_t>(vf);
			auto readPair = [&](bool has) {
				MotionPair m;
				m.valid = has; m.gridW = gw; m.gridH = gh; m.blockPx = blockPx;
				m.vx.resize(gn); m.vy.resize(gn); m.trust.resize(gn);
				ReadExact(vf, m.vx.data(), gn * 4); ReadExact(vf, m.vy.data(), gn * 4); ReadExact(vf, m.trust.data(), gn * 4);
				return m;
			};
			const MotionPair b = readPair(hasB != 0), f = readPair(hasF != 0);
			FieldMotionSource motion;
			Model::FrameIn in;
			in.seq = static_cast<uint64_t>(count);
			if (b.valid) motion.SetBackward(in.seq, b);
			if (f.valid) motion.SetForward(in.seq, f);
			in.luma = Plane(o.w, o.h);
			for (size_t i = 0; i < n; ++i)
				in.luma.v[i] = (wide ? (raw[2 * i] | (raw[2 * i + 1] << 8)) : raw[i]) / k;
			in.contentHz = o.hz;
			in.planeLines = o.lines;
			in.motion = &motion;
			in.activeX0 = o.active[0]; in.activeY0 = o.active[1]; in.activeX1 = o.active[2]; in.activeY1 = o.active[3];
			const auto t0 = std::chrono::steady_clock::now();
			const Plane out = model.Process(in);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			const double maxCode = std::ldexp(1.0, o.bitdepth) - 1.0;
			for (size_t i = 0; i < n; ++i)
			{
				const double v = std::fmin(maxCode, std::fmax(0.0, std::floor(out.v[i] * k + 0.5)));
				const unsigned u = static_cast<unsigned>(v);
				if (wide) { raw[2 * i] = static_cast<uint8_t>(u & 0xff); raw[2 * i + 1] = static_cast<uint8_t>(u >> 8); }
				else raw[i] = static_cast<uint8_t>(u);
			}
			char name[32];
			std::snprintf(name, sizeof name, "%06lld.yuv", count);
			std::FILE* of = std::fopen((outDir + "/" + name).c_str(), "wb");
			if (!of) Die("cannot write into " + outDir);
			WriteExact(of, raw.data(), raw.size());
			std::fclose(of);
			stats << count << " " << StatsLine(model, ms) << "\n";
			frames << (count ? ",\n  " : "  ") << "{\"file\":\"" << name << "\",\"source_frame_index\":" << (start + count) << "}";
			++count;
		}
		std::fclose(vf);
		std::ofstream cj(outDir + "/capture.json");
		cj << "{\"schema\":1,\"producer\":\"td_cli (TemporalDetail CPU model, DIS-flow vectors)\",\"vp_commit\":\"\","
		   << "\"config_hash\":\"mode=" << o.mode << ";budget=" << o.budget << ";clean=" << o.clean << ";guard=" << o.guard << "\","
		   << "\"width\":" << o.w << ",\"height\":" << o.h << ",\"pix_fmt\":\"" << (o.bitdepth == 8 ? "yuv420p" : o.bitdepth == 10 ? "yuv420p10le" : o.bitdepth == 12 ? "yuv420p12le" : "yuv420p16le")
		   << "\",\"bitdepth\":" << o.bitdepth << ",\"range\":\"limited\",\"matrix\":\"709\",\"transfer\":\"sdr\",\"fps\":" << o.hz
		   << ",\n\"frames\":[\n" << frames.str() << "\n]}\n";
		std::fprintf(stderr, "td_cli: %lld frames -> %s\n", count, outDir.c_str());
		return count > 0 ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if (Flag(argc, argv, "--stdio")) return RunStdio();
	if (argc < 2 || Flag(argc, argv, "--help"))
	{
		std::fprintf(stderr, "usage: td_cli --stdio | td_cli --in <yuv|y4m> --vectors <tdv> --out <dir> [options] (see td_cli.cpp)\n");
		return argc < 2 ? 2 : 0;
	}
	return RunFiles(argc, argv);
}
