// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// nvof_dump: offline dump of NVIDIA Optical Flow (NVOFA) vectors for a raw planar 4:2:0 video file,
// using VP's NvOpticalFlow wrapper. Only luma is used (converted to 8 bit with rounding).
//
// Usage: nvof_dump --in f.yuv --w W --h H [--pix-fmt yuv420p10le|yuv420p] [--frames N] --out f.nvf
//                  [--preset slow|medium|fast]     |     nvof_dump --selftest
// GPU: the DXGI adapter whose description contains env VP_TEST_ADAPTER (case-insensitive), else the
// first adapter containing "NVIDIA".
//
// Padding: the engine works on a grid of 4x4 pixel cells. If W or H is not a multiple of 4 the luma
// is edge-replicated up to the next multiple of 4 internally (gw = ceil(W/4), gh = ceil(H/4)); the
// header still records the given W,H. 1920x1080 (480x270 cells) and 3840x2160 need no padding.
//
// .nvf file (little-endian):
//   "NVF1", i32 pairs, i32 w, i32 h, i32 grid(=4), i32 gw, i32 gh
//   per pair (t-1 -> t, t = 1..N-1):
//     f32 fwdX[gw*gh], f32 fwdY[gw*gh], f32 bwdX[gw*gh], f32 bwdY[gw*gh]   (pixels = S10.5 / 32.0)
//     u8 fwdCost[gw*gh], u8 bwdCost[gw*gh]                                  (higher = less sure)
//   Cells are row-major; cell (cx,cy) covers pixels [4cx,4cx+4) x [4cy,4cy+4).
//
// Vector meaning / sign convention: the NVIDIA header (nvOpticalFlowCommon.h, NV_OF_PRED_DIRECTION_BOTH)
// says the output "gives each pixel position change from inputFrame to referenceFrame" (and
// the reverse for the backward buffer). It does not spell out the sign explicitly (ambiguous).
// Convention assumed here (to be confirmed by --selftest, which the caller must run): a vector v at
// grid cell p of the `from` frame (frame t-1) is the content displacement, so the matching position
// in frame t is p + v. Backward vectors sit on frame t's grid and point into frame t-1:
// position_in_(t-1) = p + v. For frame1(x,y) = frame0(x-3,y+2): forward = (+3,-2), backward = (-3,+2).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atlbase.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/VP-V/modules/motion/estimation/NvOpticalFlow.h"

namespace
{
	struct Gpu
	{
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11DeviceContext> context;
	};

	std::string Lower(std::string s)
	{
		for (auto& c : s)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		return s;
	}

	bool CreateGpu(Gpu& gpu)
	{
		std::string want = "nvidia";
		const char* env = getenv("VP_TEST_ADAPTER");
		const bool fromEnv = env && *env;
		if (fromEnv)
			want = Lower(env);
		CComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
		{
			fprintf(stderr, "error: CreateDXGIFactory1 failed\n");
			return false;
		}
		CComPtr<IDXGIAdapter1> chosen;
		std::string chosenName;
		for (UINT i = 0;; ++i)
		{
			CComPtr<IDXGIAdapter1> a;
			if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_ADAPTER_DESC1 d{};
			a->GetDesc1(&d);
			char name[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
			if (Lower(name).find(want) != std::string::npos)
			{
				chosen = a;
				chosenName = name;
				break;
			}
		}
		if (!chosen)
		{
			fprintf(stderr, "error: no DXGI adapter whose description contains \"%s\" (%s)\n", want.c_str(),
				fromEnv ? "from VP_TEST_ADAPTER" : "default; set VP_TEST_ADAPTER to override");
			return false;
		}
		printf("adapter: %s\n", chosenName.c_str());
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL got{};
		const HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
			&gpu.device, &got, &gpu.context);
		if (FAILED(hr))
		{
			fprintf(stderr, "error: D3D11CreateDevice failed 0x%08lx\n", static_cast<unsigned long>(hr));
			return false;
		}
		return true;
	}

	CComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, bool staging)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = static_cast<UINT>(w);
		d.Height = static_cast<UINT>(h);
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
		d.BindFlags = staging ? 0u : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);   // as VP (NvOpticalFlowTests, DejudderWarp)
		d.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
		CComPtr<ID3D11Texture2D> t;
		const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &t);
		if (FAILED(hr))
		{
			fprintf(stderr, "error: CreateTexture2D %dx%d fmt %d failed 0x%08lx\n", w, h, static_cast<int>(fmt),
				static_cast<unsigned long>(hr));
			return nullptr;
		}
		return t;
	}

	struct Pair
	{
		std::vector<float> fx, fy, bx, by;
		std::vector<uint8_t> fc, bc;
	};

	class Runner
	{
	public:
		bool Init(Gpu& g, int pw, int ph, NvOpticalFlow::Preset preset)
		{
			gpu = &g;
			w = pw;
			h = ph;
			gw = pw / 4;
			gh = ph / 4;
			if (!engine.Open(g.device, g.context, pw, ph, preset, true))
			{
				fprintf(stderr, "error: NVOF open failed: %s\n", engine.LastError().c_str());
				return false;
			}
			for (int i = 0; i < 2; ++i)
			{
				in[i] = MakeTexture(g.device, pw, ph, NvOpticalFlow::Engine::InputFormat(), false);
				if (!in[i])
					return false;
			}
			fwd = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), false);
			bwd = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), false);
			fc = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::CostFormat(), false);
			bc = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::CostFormat(), false);
			sFwd = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), true);
			sBwd = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), true);
			sFc = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::CostFormat(), true);
			sBc = MakeTexture(g.device, gw, gh, NvOpticalFlow::Engine::CostFormat(), true);
			return fwd && bwd && fc && bc && sFwd && sBwd && sFc && sBc;
		}

		// Frame k lives in in[k&1]. Upload frame k, then RunPair(k) uses frames k-1 and k.
		void Upload(int k, const uint8_t* luma)
		{
			gpu->context->UpdateSubresource(in[k & 1], 0, nullptr, luma, static_cast<UINT>(w), 0);
		}

		bool RunPair(int k, Pair& out)
		{
			// continuous = temporal hints from the previous Execute (as in VP); none for the first pair.
			if (!engine.Run(in[(k - 1) & 1], in[k & 1], fwd, bwd, fc, bc, nullptr, k > 1))
			{
				fprintf(stderr, "error: NVOF run failed (pair %d): %s\n", k, engine.LastError().c_str());
				return false;
			}
			gpu->context->CopyResource(sFwd, fwd);
			gpu->context->CopyResource(sBwd, bwd);
			gpu->context->CopyResource(sFc, fc);
			gpu->context->CopyResource(sBc, bc);
			gpu->context->Flush();
			const size_t n = static_cast<size_t>(gw) * gh;
			out.fx.resize(n); out.fy.resize(n); out.bx.resize(n); out.by.resize(n);
			out.fc.resize(n); out.bc.resize(n);
			return ReadFlow(sFwd, out.fx, out.fy) && ReadFlow(sBwd, out.bx, out.by) && ReadCost(sFc, out.fc) &&
				ReadCost(sBc, out.bc);
		}

		int gw = 0, gh = 0;

	private:
		bool ReadFlow(ID3D11Texture2D* t, std::vector<float>& x, std::vector<float>& y)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(gpu->context->Map(t, 0, D3D11_MAP_READ, 0, &m)))
			{
				fprintf(stderr, "error: Map flow staging failed\n");
				return false;
			}
			for (int j = 0; j < gh; ++j)
			{
				const int16_t* row = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(j) * m.RowPitch);
				for (int i = 0; i < gw; ++i)
				{
					x[static_cast<size_t>(j) * gw + i] = row[2 * i] / 32.0f;
					y[static_cast<size_t>(j) * gw + i] = row[2 * i + 1] / 32.0f;
				}
			}
			gpu->context->Unmap(t, 0);
			return true;
		}
		bool ReadCost(ID3D11Texture2D* t, std::vector<uint8_t>& c)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(gpu->context->Map(t, 0, D3D11_MAP_READ, 0, &m)))
			{
				fprintf(stderr, "error: Map cost staging failed\n");
				return false;
			}
			for (int j = 0; j < gh; ++j)
				memcpy(&c[static_cast<size_t>(j) * gw], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(j) * m.RowPitch, static_cast<size_t>(gw));
			gpu->context->Unmap(t, 0);
			return true;
		}

		Gpu* gpu = nullptr;
		NvOpticalFlow::Engine engine;
		int w = 0, h = 0;
		CComPtr<ID3D11Texture2D> in[2], fwd, bwd, fc, bc, sFwd, sBwd, sFc, sBc;
	};

	// Edge-replicate a w*h plane into pw*ph.
	void Pad(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& dst, int pw, int ph)
	{
		dst.resize(static_cast<size_t>(pw) * ph);
		for (int y = 0; y < ph; ++y)
		{
			const int sy = std::min(y, h - 1);
			for (int x = 0; x < pw; ++x)
				dst[static_cast<size_t>(y) * pw + x] = src[static_cast<size_t>(sy) * w + std::min(x, w - 1)];
		}
	}

	float Median(std::vector<float> v)
	{
		if (v.empty())
			return 0.0f;
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	}

	int SelfTest()
	{
		Gpu& gpu = *new Gpu;   // deliberately leaked with r: the engine's teardown faults in this driver (see main)
		if (!CreateGpu(gpu))
			return 2;
		const int SW = 1920, SH = 1080;   // a size VP's engine tests cover
		auto hash = [](uint32_t x, uint32_t y) {
			uint32_t h = x * 374761393u + y * 668265263u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return static_cast<float>((h ^ (h >> 16)) & 0xFF);
		};
		// Hash noise smoothed by a 3x3 box, defined on all integers so the shifted copy needs no wrap.
		auto base = [&](int x, int y) {
			float s = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					s += hash(static_cast<uint32_t>(x + dx + 1000), static_cast<uint32_t>(y + dy + 1000));
			return s / 9.0f;
		};
		// The box leaves little contrast; stretch around the mean so the pattern is well textured.
		auto pix = [&](int x, int y) {
			const float v = (base(x, y) - 127.5f) * 4.0f + 127.5f;
			return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v + 0.5f)));
		};
		std::vector<uint8_t> f0(static_cast<size_t>(SW) * SH), f1(static_cast<size_t>(SW) * SH);
		for (int y = 0; y < SH; ++y)
			for (int x = 0; x < SW; ++x)
			{
				f0[static_cast<size_t>(y) * SW + x] = pix(x, y);
				f1[static_cast<size_t>(y) * SW + x] = pix(x - 3, y + 2);   // frame1(x,y) = frame0(x-3, y+2)
			}
		Runner& r = *new Runner;
		if (!r.Init(gpu, SW, SH, NvOpticalFlow::Preset::Slow))
			return 2;
		fprintf(stderr, "selftest: engine open\n");
		r.Upload(0, f0.data());
		r.Upload(1, f1.data());
		Pair p;
		if (!r.RunPair(1, p))
			return 2;
		std::vector<float> fx, fy, bx, by;
		const int margin = 6;   // interior cells only
		for (int cy = margin; cy < r.gh - margin; ++cy)
			for (int cx = margin; cx < r.gw - margin; ++cx)
			{
				const size_t i = static_cast<size_t>(cy) * r.gw + cx;
				fx.push_back(p.fx[i]); fy.push_back(p.fy[i]);
				bx.push_back(p.bx[i]); by.push_back(p.by[i]);
			}
		const float mfx = Median(fx), mfy = Median(fy), mbx = Median(bx), mby = Median(by);
		printf("median forward  = (%+.3f, %+.3f)  expected (+3, -2)\n", mfx, mfy);
		printf("median backward = (%+.3f, %+.3f)  expected (-3, +2)\n", mbx, mby);
		const bool pass = fabsf(mfx - 3.0f) <= 0.5f && fabsf(mfy + 2.0f) <= 0.5f && fabsf(mbx + 3.0f) <= 0.5f &&
			fabsf(mby - 2.0f) <= 0.5f;
		printf(pass ? "PASS\n" : "FAIL\n");
		return pass ? 0 : 1;
	}

	void PutI32(FILE* f, int32_t v) { fwrite(&v, 4, 1, f); }

	int Usage()
	{
		fprintf(stderr, "usage: nvof_dump --in f.yuv --w W --h H [--pix-fmt yuv420p10le|yuv420p] [--frames N] --out f.nvf "
			"[--preset slow|medium|fast]\n       nvof_dump --selftest\n");
		return 2;
	}
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::string in, out, pix = "yuv420p10le", preset = "slow";
	int w = 0, h = 0, frames = -1;
	bool selftest = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--selftest")
		{
			selftest = true;
		}
		else if (a == "--in" || a == "--out" || a == "--pix-fmt" || a == "--preset" || a == "--w" || a == "--h" || a == "--frames")
		{
			if (i + 1 >= argc)
			{
				fprintf(stderr, "error: %s needs a value\n", a.c_str());
				return Usage();
			}
			const char* v = argv[++i];
			if (a == "--in") in = v;
			else if (a == "--out") out = v;
			else if (a == "--pix-fmt") pix = v;
			else if (a == "--preset") preset = v;
			else if (a == "--w") w = atoi(v);
			else if (a == "--h") h = atoi(v);
			else frames = atoi(v);
		}
		else
		{
			fprintf(stderr, "error: unknown argument %s\n", a.c_str());
			return Usage();
		}
	}
	// TerminateProcess: skip the optical flow engine's teardown and DLL detach at process exit (it faults inside the driver after a completed
	// run on this laptop's driver); every output is closed and flushed first.
	if (selftest)
	{
		const int rc = SelfTest();
		fflush(stdout);
		fflush(stderr);
		TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));   // no DLL detach either
	}
	if (in.empty() || out.empty() || w <= 0 || h <= 0)
		return Usage();
	int bytes = 0;
	if (pix == "yuv420p10le") bytes = 2;
	else if (pix == "yuv420p") bytes = 1;
	else { fprintf(stderr, "error: unsupported --pix-fmt %s\n", pix.c_str()); return 2; }
	NvOpticalFlow::Preset pr;
	if (preset == "slow") pr = NvOpticalFlow::Preset::Slow;
	else if (preset == "medium") pr = NvOpticalFlow::Preset::Medium;
	else if (preset == "fast") pr = NvOpticalFlow::Preset::Fast;
	else { fprintf(stderr, "error: unsupported --preset %s\n", preset.c_str()); return 2; }

	const size_t cw = static_cast<size_t>((w + 1) / 2), ch = static_cast<size_t>((h + 1) / 2);
	const size_t lumaBytes = static_cast<size_t>(w) * h * bytes;
	const size_t frameBytes = lumaBytes + 2 * cw * ch * bytes;
	FILE* fi = fopen(in.c_str(), "rb");
	if (!fi) { fprintf(stderr, "error: cannot open %s\n", in.c_str()); return 2; }
	_fseeki64(fi, 0, SEEK_END);
	const long long size = _ftelli64(fi);
	_fseeki64(fi, 0, SEEK_SET);
	const int avail = static_cast<int>(size / static_cast<long long>(frameBytes));
	if (size % static_cast<long long>(frameBytes) != 0)
		fprintf(stderr, "warning: file size is not a whole number of frames; trailing %lld bytes ignored\n",
			size % static_cast<long long>(frameBytes));
	if (frames < 0 || frames > avail) frames = avail;
	if (frames < 2) { fprintf(stderr, "error: need at least 2 frames (have %d)\n", frames); fclose(fi); return 2; }

	const int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
	Gpu gpu;
	if (!CreateGpu(gpu)) { fclose(fi); return 2; }
	Runner r;
	// every exit after Init goes through Quit (TerminateProcess: no engine teardown); a failed run deletes the output
	auto quit = [&](int rc, FILE* o) { if (o) { fclose(o); if (rc) remove(out.c_str()); } fclose(fi); fflush(stdout); fflush(stderr); TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc)); return rc; };
	if (!r.Init(gpu, pw, ph, pr)) return quit(2, nullptr);
	FILE* fo = fopen(out.c_str(), "wb");
	if (!fo) { fprintf(stderr, "error: cannot create %s\n", out.c_str()); return quit(2, nullptr); }
	fwrite("NVF1", 1, 4, fo);
	PutI32(fo, frames - 1); PutI32(fo, w); PutI32(fo, h); PutI32(fo, 4); PutI32(fo, r.gw); PutI32(fo, r.gh);

	std::vector<uint8_t> raw(frameBytes), luma(static_cast<size_t>(w) * h), padded;
	for (int k = 0; k < frames; ++k)
	{
		if (fread(raw.data(), 1, frameBytes, fi) != frameBytes)
		{
			fprintf(stderr, "error: short read at frame %d\n", k);
			return quit(2, fo);

		}
		if (bytes == 2)
			for (size_t i = 0; i < luma.size(); ++i)
			{
				const int v = (raw[2 * i] | (raw[2 * i + 1] << 8)) & 0x3FF;
				luma[i] = static_cast<uint8_t>(std::min(255, (v + 2) >> 2));
			}
		else
			memcpy(luma.data(), raw.data(), luma.size());
		Pad(luma, w, h, padded, pw, ph);
		r.Upload(k, padded.data());
		if (k == 0)
			continue;
		Pair p;
		if (!r.RunPair(k, p)) return quit(2, fo);
		const size_t n = static_cast<size_t>(r.gw) * r.gh;
		fwrite(p.fx.data(), 4, n, fo); fwrite(p.fy.data(), 4, n, fo);
		fwrite(p.bx.data(), 4, n, fo); fwrite(p.by.data(), 4, n, fo);
		fwrite(p.fc.data(), 1, n, fo); fwrite(p.bc.data(), 1, n, fo);
		if (k % 50 == 0) printf("pair %d/%d\n", k, frames - 1);
	}
	if (ferror(fo)) { fprintf(stderr, "error: write failed on %s\n", out.c_str()); return quit(2, fo); }
	if (fclose(fo) != 0) { fprintf(stderr, "error: close failed on %s\n", out.c_str()); remove(out.c_str()); return quit(2, nullptr); }
	printf("wrote %s (%d pairs, %dx%d, grid 4, %dx%d cells)\n", out.c_str(), frames - 1, w, h, r.gw, r.gh);
	fflush(stdout);
	fflush(stderr);
	TerminateProcess(GetCurrentProcess(), 0);
}
