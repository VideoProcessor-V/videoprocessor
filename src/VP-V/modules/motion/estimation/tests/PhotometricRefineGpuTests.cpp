// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// GPU form of the direct photometric refinement (doc 103 s5 R3-GPU stage 1): every call of the CPU reference's cases
// (PhotometricRefineTests.cpp, same scenes) is run on the CPU and on the GPU and compared - model within 1e-4 engine px
// at the picture corners / centre, identical status, candidates and pixels, gain / offset within 1e-4 - and each GPU
// call is repeated and must be byte-equal. The device is the DXGI adapter whose description contains VP_TEST_ADAPTER
// (for example NVIDIA), else WARP. GpuCostLine (VP_R3_GPU_COST=1) logs the cost at Q / H / F for 1080 and 4K.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/PhotometricRefine.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricRefineGpu.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "PhotometricRefineScenes.h"
#include "CppUnitTest.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;
		using namespace R3Scenes;
		using Microsoft::WRL::ComPtr;

		void GLog(const char* text)
		{
			Logger::WriteMessage(text);
		}

		struct Rig
		{
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11DeviceContext> context;
			std::string adapter;
			std::string error;
			RefineUploadInput input;
		};

		// One device per test process: VP_TEST_ADAPTER selects an adapter by description substring (case-insensitive);
		// unset = WARP. A named adapter that is not present is an error (never a silent WARP run).
		Rig& TheRig()
		{
			static std::unique_ptr<Rig> rig;
			if (rig)
				return *rig;
			rig.reset(new Rig());
			const std::string want = EnvText("VP_TEST_ADAPTER");
			const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			HRESULT hr = E_FAIL;
			if (!want.empty())
			{
				std::string lw = want;
				for (char& c : lw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				ComPtr<IDXGIFactory1> factory;
				if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()))))
				{
					ComPtr<IDXGIAdapter1> ad;
					for (UINT i = 0; factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) == S_OK; ++i)
					{
						DXGI_ADAPTER_DESC1 d = {};
						ad->GetDesc1(&d);
						std::string name;
						for (const wchar_t* p = d.Description; *p; ++p)
							name += (*p < 128) ? static_cast<char>(*p) : '?';
						std::string ln = name;
						for (char& c : ln) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
						if (ln.find(lw) == std::string::npos)
							continue;
						hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &rig->device, nullptr, &rig->context);
						if (FAILED(hr))
							hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &rig->device, nullptr, &rig->context);
						rig->adapter = name;
						break;
					}
				}
				if (FAILED(hr))
					rig->error = "adapter '" + want + "' not found or no device";
			}
			else
			{
				hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &rig->device, nullptr, &rig->context);
				if (FAILED(hr))
					hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &rig->device, nullptr, &rig->context);
				rig->adapter = "WARP";
				if (FAILED(hr))
					rig->error = "no WARP device";
			}
			if (SUCCEEDED(hr))
			{
				std::string why;
				if (!rig->input.Init(rig->device.Get(), &why))
					rig->error = why;
			}
			return *rig;
		}

		Rig& RequireRig()
		{
			Rig& g = TheRig();
			if (!g.error.empty())
			{
				const std::string m = "GPU rig: " + g.error + "\n";
				GLog(m.c_str());
			}
			Assert::IsTrue(g.error.empty(), L"no test device");
			return g;
		}

		// Equivalence record of one test.
		struct Eq
		{
			int calls = 0, gpuCalls = 0;
			double maxModel = 0, maxGain = 0, maxOffset = 0, maxResRel = 0;
		};

		bool SameBytes(const RefineResult& a, const RefineResult& b)
		{
			return std::memcmp(a.model.h, b.model.h, sizeof(a.model.h)) == 0 && a.model.type == b.model.type && a.status == b.status
				&& std::memcmp(&a.gain, &b.gain, sizeof(double)) == 0 && std::memcmp(&a.offset, &b.offset, sizeof(double)) == 0
				&& std::memcmp(&a.residualBefore, &b.residualBefore, sizeof(double)) == 0 && std::memcmp(&a.residualAfter, &b.residualAfter, sizeof(double)) == 0
				&& std::memcmp(&a.robustSigma, &b.robustSigma, sizeof(double)) == 0 && std::memcmp(&a.correctionPx, &b.correctionPx, sizeof(double)) == 0
				&& std::memcmp(&a.lastStepPx, &b.lastStepPx, sizeof(double)) == 0 && a.iterations == b.iterations && a.candidates == b.candidates
				&& a.pixels == b.pixels && a.maskUsed == b.maskUsed;
		}

		// One call on both: returns the GPU result (asserted equivalent to the CPU's; the GPU repeat byte-equal).
		RefineResult Both(Eq& eq, const char* name, const Params& model, const LumaPairView& view, const Rect& pic, const InlierMask& mask,
			const RefineOptions& o, RefineResult* cpuOut = nullptr)
		{
			Rig& g = RequireRig();
			const RefineResult rc = PhotometricRefine(model, view, pic, mask, o);
			PhotometricRefinerGpu gpu(o);
			std::string why;
			const bool init = gpu.Init(g.device.Get(), &why);
			if (!init)
			{
				const std::string m = std::string("GPU refiner unavailable: ") + why + "\n";
				GLog(m.c_str());
			}
			Assert::IsTrue(init, L"GPU refiner did not initialise");
			RefineGpuLuma gl;
			g.input.Set(view);
			if (!g.input.Prepare(g.context.Get(), &gl, &why))
				gl = RefineGpuLuma();
			const RefineResult r1 = gpu.Refine(g.context.Get(), model, gl, pic, mask);
			const RefineResult r2 = gpu.Refine(g.context.Get(), model, gl, pic, mask);
			++eq.calls;
			const double s = view.sourcePxPerPixel > 0 ? view.sourcePxPerPixel : 1.0;
			const double diag = std::sqrt(static_cast<double>(pic.Width()) * pic.Width() + static_cast<double>(pic.Height()) * pic.Height());
			const double dModel = pic.Empty() ? 0.0 : MaxCornerDifference(r1.model, rc.model, pic) * diag / s;
			const double dGain = std::fabs(r1.gain - rc.gain), dOffset = std::fabs(r1.offset - rc.offset);
			const double resRel = std::fabs(r1.residualAfter - rc.residualAfter) / (std::max)(1e-12, std::fabs(rc.residualAfter));
			if (r1.status != rc.status || dModel > 1e-4 || dGain > 1e-4 || dOffset > 1e-4 || r1.candidates != rc.candidates || r1.pixels != rc.pixels)
			{
				char msg[512];
				std::snprintf(msg, sizeof(msg), "MISMATCH %s: cpu %s / gpu %s, model %.3g engine px, gain %.3g, offset %.3g, cand %u/%u, px %u/%u, it %d/%d, "
					"cost %.9g/%.9g -> %.9g/%.9g, error '%s'\n", name, RefineStatusText(rc.status), RefineStatusText(r1.status), dModel, dGain, dOffset,
					rc.candidates, r1.candidates, rc.pixels, r1.pixels, rc.iterations, r1.iterations, rc.residualBefore, r1.residualBefore, rc.residualAfter,
					r1.residualAfter, gpu.LastError().c_str());
				GLog(msg);
			}
			Assert::IsTrue(r1.status == rc.status, L"GPU status differs from the CPU reference");
			Assert::AreEqual(rc.candidates, r1.candidates, L"candidates differ");
			Assert::AreEqual(rc.pixels, r1.pixels, L"pixels differ");
			Assert::IsTrue(r1.maskUsed == rc.maskUsed, L"maskUsed differs");
			Assert::IsTrue(r1.model.type == rc.model.type, L"model type differs");
			Assert::IsTrue(dModel <= 1e-4, L"GPU model differs from the CPU reference by more than 1e-4 engine px");
			Assert::IsTrue(dGain <= 1e-4 && dOffset <= 1e-4, L"GPU gain / offset differ by more than 1e-4");
			Assert::IsTrue(SameBytes(r1, r2), L"GPU repeat not byte-equal");
			eq.maxModel = (std::max)(eq.maxModel, dModel);
			eq.maxGain = (std::max)(eq.maxGain, dGain);
			eq.maxOffset = (std::max)(eq.maxOffset, dOffset);
			if (rc.status == RefineStatus::Refined || RefineFellBack(rc.status))
			{
				eq.maxResRel = (std::max)(eq.maxResRel, resRel);
				++eq.gpuCalls;
			}
			if (cpuOut)
				*cpuOut = rc;
			return r1;
		}

		void Report(const char* name, const Eq& eq)
		{
			char msg[320];
			std::snprintf(msg, sizeof(msg), "gpu eq %s [%s]: %d calls (%d through the kernels), max model diff %.3g engine px, gain %.3g, offset %.3g, cost rel %.3g\n",
				name, TheRig().adapter.c_str(), eq.calls, eq.gpuCalls, eq.maxModel, eq.maxGain, eq.maxOffset, eq.maxResRel);
			GLog(msg);
		}

		void Pan(Eq& eq, const char* name, int frameW, int frameH, double s, double tx, double ty, double errX, double errY, int pairs, uint64_t seed)
		{
			const int w = (frameW + static_cast<int>(s) - 1) / static_cast<int>(s), h = (frameH + static_cast<int>(s) - 1) / static_cast<int>(s);
			const std::vector<Wave> waves = MakeWaves(seed, s, 90.0);
			const Rect pic = Pic(frameW, frameH);
			for (int k = 0; k < pairs; ++k)
			{
				std::vector<Layer> la(1, Shifted(waves, k * tx, k * ty));
				std::vector<Layer> lb(1, Shifted(waves, (k + 1) * tx, (k + 1) * ty));
				const std::vector<uint8_t> A = To8(RenderD(w, h, s, la, 1.0, 0.0, seed * 31 + 2 * k, 1.0));
				const std::vector<uint8_t> B = To8(RenderD(w, h, s, lb, 1.0, 0.0, seed * 31 + 2 * k + 1, 1.0));
				Both(eq, name, Translation(tx + errX, ty + errY), View8(A, B, w, h, s), pic, InlierMask(), RefineOptions());
			}
		}

		std::vector<uint16_t> To10(const std::vector<double>& v)
		{
			std::vector<uint16_t> o(v.size());
			for (size_t k = 0; k < v.size(); ++k)
				o[k] = static_cast<uint16_t>((std::min)(1023.0, (std::max)(0.0, std::floor(v[k] * (1023.0 / 255.0) + 0.5))));
			return o;
		}

		LumaPairView View16(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, int w, int h, double s, double codeScale)
		{
			LumaPairView v;
			v.from.data16 = a.data();
			v.to.data16 = b.data();
			v.from.width = v.to.width = w;
			v.from.height = v.to.height = h;
			v.from.stride = v.to.stride = w;
			v.sourcePxPerPixel = s;
			v.codeScale = codeScale;
			return v;
		}
	}

	TEST_CLASS(PhotometricRefineGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuPan1080Half)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuPan1080Half)
		{
			Eq eq;
			Pan(eq, "1080 pan", 1920, 1080, 2.0, 2.5, 0.0, 0.8, -0.5, 8, 11);
			Report("1080 pan", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuPan4KQuarter)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuPan4KQuarter)
		{
			Eq eq;
			Pan(eq, "4K pan", 3840, 2160, 4.0, 5.0, 0.0, 1.7, 0.6, 8, 12);
			Report("4K pan", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuDiagonalPan)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuDiagonalPan)
		{
			Eq eq;
			Pan(eq, "diagonal pan", 1920, 1080, 2.0, 2.5, 1.5, -0.7, 0.6, 6, 13);
			Report("diagonal pan", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuFadePan)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuFadePan)
		{
			const int w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(21, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 1, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 0.8, 10.0, 2, 1.0));
			Eq eq;
			const RefineResult r = Both(eq, "fade", Translation(tx + 0.8, -0.4), View8(A, B, w, h, s), Pic(1920, 1080), InlierMask(), RefineOptions());
			Assert::IsTrue(r.status == RefineStatus::Refined);
			Report("fade", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuMoverMaskedAndUnmasked)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuMoverMaskedAndUnmasked)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> bg = MakeWaves(31, s, 90.0);
			const std::vector<Wave> fg = MakeWaves(32, s, 100.0);
			const double l = 700, t = 300, rw = 640, rh = 380, mx = 14, my = -9;
			std::vector<Layer> la(1, Shifted(bg, 0, 0)), lb(1, Shifted(bg, tx, 0));
			Layer ma = Shifted(fg, 0, 0);
			ma.clip = true; ma.l = l; ma.t = t; ma.r = l + rw; ma.b = t + rh;
			Layer mb = Shifted(fg, mx, my);
			mb.clip = true; mb.l = l + mx; mb.t = t + my; mb.r = l + rw + mx; mb.b = t + rh + my;
			la.push_back(ma);
			lb.push_back(mb);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, la, 1.0, 0.0, 3, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, lb, 1.0, 0.0, 4, 1.0));
			BlockData blocks;
			blocks.blockSize = 32;
			blocks.gridW = (frameW + 31) / 32;
			blocks.gridH = (frameH + 31) / 32;
			blocks.label.assign(static_cast<size_t>(blocks.gridW) * blocks.gridH, static_cast<uint8_t>(Label::Camera));
			blocks.excluded.assign(blocks.label.size(), 0);
			const double ul = (std::min)(l, l + mx) - 4, ut = (std::min)(t, t + my) - 4, ur = (std::max)(l + rw, l + rw + mx) + 4, ub = (std::max)(t + rh, t + rh + my) + 4;
			for (int by = 0; by < blocks.gridH; ++by)
				for (int bx = 0; bx < blocks.gridW; ++bx)
				{
					const double x0 = bx * 32.0, y0 = by * 32.0;
					if (x0 + 32 > ul && x0 < ur && y0 + 32 > ut && y0 < ub)
						blocks.label[static_cast<size_t>(by) * blocks.gridW + bx] = static_cast<uint8_t>(Label::Object);
				}
			// An excluded (bar / text-guard) row as well, so the GPU's excluded lookup is exercised.
			for (int bx = 0; bx < blocks.gridW; ++bx)
				blocks.excluded[static_cast<size_t>(2) * blocks.gridW + bx] = 1;
			Eq eq;
			const RefineResult masked = Both(eq, "mover masked", Translation(tx + 0.8, 0.5), View8(A, B, w, h, s), Pic(frameW, frameH), MaskOf(blocks), RefineOptions());
			const RefineResult unmasked = Both(eq, "mover unmasked", Translation(tx + 0.8, 0.5), View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask(), RefineOptions());
			Assert::IsTrue(masked.maskUsed && !unmasked.maskUsed);
			Report("mover", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuSimilarityAndAffine)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuSimilarityAndAffine)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0;
			const Rect pic = Pic(frameW, frameH);
			Params truth;
			truth.type = ModelType::Similarity;
			const double zc = 1.002 * std::cos(0.001), zs = 1.002 * std::sin(0.001);
			truth.h[0] = zc; truth.h[1] = -zs; truth.h[2] = 2.5;
			truth.h[3] = zs; truth.h[4] = zc; truth.h[5] = -1.0;
			bool ok = false;
			const Params inv = Invert(truth, &ok);
			Assert::IsTrue(ok);
			const std::vector<Wave> waves = MakeWaves(41, s, 90.0);
			const double C[2] = { pic.CentreX(), pic.CentreY() };
			Layer lb;
			lb.waves = &waves;
			lb.m[0] = inv.h[0]; lb.m[1] = inv.h[1]; lb.m[2] = inv.h[3]; lb.m[3] = inv.h[4];
			lb.c[0] = inv.h[2] + C[0] - (inv.h[0] * C[0] + inv.h[1] * C[1]);
			lb.c[1] = inv.h[5] + C[1] - (inv.h[3] * C[0] + inv.h[4] * C[1]);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 5, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, lb), 1.0, 0.0, 6, 1.0));
			Params start = truth;
			start.h[2] += 0.7;
			start.h[5] -= 0.4;
			start.h[0] -= 0.0002;
			start.h[4] -= 0.0002;
			Params startAff = start;
			startAff.type = ModelType::Affine;
			Params startHom = start;
			startHom.type = ModelType::Homography;
			startHom.h[6] = 1e-7;
			Eq eq;
			Both(eq, "similarity", start, View8(A, B, w, h, s), pic, InlierMask(), RefineOptions());
			Both(eq, "affine", startAff, View8(A, B, w, h, s), pic, InlierMask(), RefineOptions());
			Both(eq, "homography", startHom, View8(A, B, w, h, s), pic, InlierMask(), RefineOptions());
			RefineOptions bil;
			bil.interp = RefineInterp::Bilinear;
			Both(eq, "similarity bilinear", start, View8(A, B, w, h, s), pic, InlierMask(), bil);
			Report("similarity / affine", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuFlatFrame)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuFlatFrame)
		{
			const int w = 960, h = 540;
			const double s = 2.0;
			const std::vector<Wave> waves = MakeWaves(51, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 7, 1.0));
			const std::vector<uint8_t> flat(static_cast<size_t>(w) * h, 128);
			const Params start = Translation(2.9, -0.3);
			Eq eq;
			const RefineResult r1 = Both(eq, "flat r1", start, View8(A, flat, w, h, s), Pic(1920, 1080), InlierMask(), RefineOptions());
			Assert::IsTrue(std::memcmp(r1.model.h, start.h, sizeof(start.h)) == 0);
			RefineOptions o;
			o.fadeTerms = false;
			o.translationOnly = true;
			o.maxCorrectionLumaPx = 1000.0;
			const RefineResult r2 = Both(eq, "flat r2", start, View8(A, flat, w, h, s), Pic(1920, 1080), InlierMask(), o);
			Assert::IsTrue(r2.status == RefineStatus::FellBackNoImprovement);
			// The flat frame as the template: too few gradient pixels.
			Both(eq, "flat template", start, View8(flat, A, w, h, s), Pic(1920, 1080), InlierMask(), RefineOptions());
			Report("flat", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuTooFarSkipsAndCounters)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuTooFarSkipsAndCounters)
		{
			const int frameW = 1920, frameH = 1080, w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(61, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 8, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 1.0, 0.0, 9, 1.0));
			const std::vector<uint8_t> flat(static_cast<size_t>(w) * h, 128);
			const Params start = Translation(tx + 0.8, 0.0);
			Eq eq;
			RefineOptions tight;
			tight.maxCorrectionLumaPx = 0.05;
			Assert::IsTrue(Both(eq, "too far", start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask(), tight).status == RefineStatus::FellBackTooFar);
			Both(eq, "good", start, View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask(), RefineOptions());
			Both(eq, "no luma", start, LumaPairView(), Pic(frameW, frameH), InlierMask(), RefineOptions());
			LumaPairView mismatched = View8(A, B, w, h, s);
			mismatched.to.width = w - 1;
			Both(eq, "mismatched", start, mismatched, Pic(frameW, frameH), InlierMask(), RefineOptions());
			Both(eq, "model none", Params(), View8(A, B, w, h, s), Pic(frameW, frameH), InlierMask(), RefineOptions());
			Both(eq, "flat", start, View8(A, flat, w, h, s), Pic(frameW, frameH), InlierMask(), RefineOptions());
			Both(eq, "picture too large", start, View8(A, B, w, h, s), Pic(3840, 2160), InlierMask(), RefineOptions());

			// Counters on one GPU refiner, as the CPU test: 1 refined, 1 fell back, 3 no luma, 1 other.
			Rig& g = RequireRig();
			PhotometricRefinerGpu gpu;
			std::string why;
			Assert::IsTrue(gpu.Init(g.device.Get(), &why));
			RefineGpuLuma ab, af;
			g.input.Set(View8(A, B, w, h, s));
			Assert::IsTrue(g.input.Prepare(g.context.Get(), &ab, &why));
			Assert::IsTrue(gpu.Refine(g.context.Get(), start, ab, Pic(frameW, frameH), InlierMask()).status == RefineStatus::Refined);
			Assert::IsTrue(gpu.Refine(g.context.Get(), start, RefineGpuLuma(), Pic(frameW, frameH), InlierMask()).status == RefineStatus::SkippedNoLuma);
			Assert::IsTrue(gpu.Refine(g.context.Get(), Params(), ab, Pic(frameW, frameH), InlierMask()).status == RefineStatus::SkippedModel);
			Assert::IsTrue(gpu.Refine(g.context.Get(), start, ab, Pic(3840, 2160), InlierMask()).status == RefineStatus::SkippedNoLuma);
			Assert::IsTrue(gpu.Refine(nullptr, start, ab, Pic(frameW, frameH), InlierMask()).status == RefineStatus::SkippedNoLuma);
			g.input.Set(View8(A, flat, w, h, s));
			Assert::IsTrue(g.input.Prepare(g.context.Get(), &af, &why));
			Assert::IsTrue(RefineFellBack(gpu.Refine(g.context.Get(), start, af, Pic(frameW, frameH), InlierMask()).status));
			const RefineCounters& c = gpu.Counters();
			Assert::AreEqual(static_cast<uint64_t>(1), c.refined);
			Assert::AreEqual(static_cast<uint64_t>(1), c.fellBack);
			Assert::AreEqual(static_cast<uint64_t>(3), c.skippedNoLuma);
			Assert::AreEqual(static_cast<uint64_t>(1), c.skippedOther);
			Report("too far / skips", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuDeterministicAndAsync)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuDeterministicAndAsync)
		{
			const int w = 960, h = 540;
			const double s = 2.0, tx = 2.5;
			const std::vector<Wave> waves = MakeWaves(71, s, 90.0);
			const std::vector<uint8_t> A = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 10, 1.0));
			const std::vector<uint8_t> B = To8(RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0.5)), 0.9, 5.0, 11, 1.0));
			Params start;
			start.type = ModelType::Similarity;
			start.h[2] = tx + 0.6;
			start.h[5] = 0.1;
			Eq eq;
			const RefineResult r1 = Both(eq, "deterministic", start, View8(A, B, w, h, s), Pic(1920, 1080), InlierMask(), RefineOptions());
			Assert::IsTrue(r1.status == RefineStatus::Refined);
			// A fresh refiner, polled asynchronously (Collect without waiting until it is ready), gives the same bytes.
			Rig& g = RequireRig();
			PhotometricRefinerGpu gpu;
			std::string why;
			Assert::IsTrue(gpu.Init(g.device.Get(), &why));
			RefineGpuLuma gl;
			g.input.Set(View8(A, B, w, h, s));
			Assert::IsTrue(g.input.Prepare(g.context.Get(), &gl, &why));
			Assert::IsTrue(gpu.Submit(g.context.Get(), start, gl, Pic(1920, 1080), InlierMask()));
			RefineResult r2;
			int polls = 0;
			while (!gpu.Collect(g.context.Get(), false, &r2))
				++polls;
			Assert::IsTrue(SameBytes(r1, r2), L"async collect differs");
			char msg[96];
			std::snprintf(msg, sizeof(msg), "async: %d polls before the result\n", polls);
			GLog(msg);
			Report("deterministic", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuCostLineCases)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuCostLineCases)
		{
			// The CPU CostLine's three cases (1080 half 8-bit, 4K quarter 8-bit, 4K half R16 = the non-unit code path).
			struct Case { const char* name; int frameW, frameH; double s; bool r16; };
			const Case cases[3] = { { "1080 half 8-bit", 1920, 1080, 2.0, false }, { "4K quarter 8-bit", 3840, 2160, 4.0, false }, { "4K half R16", 3840, 2160, 2.0, true } };
			Eq eq;
			for (int ci = 0; ci < 3; ++ci)
			{
				const Case& cs = cases[ci];
				const int si = static_cast<int>(cs.s);
				const int w = (cs.frameW + si - 1) / si, h = (cs.frameH + si - 1) / si;
				const double tx = 1.25 * cs.s;
				const std::vector<Wave> waves = MakeWaves(81 + ci, cs.s, 90.0);
				const std::vector<double> Ad = RenderD(w, h, cs.s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 12, 1.0);
				const std::vector<double> Bd = RenderD(w, h, cs.s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 1.0, 0.0, 13, 1.0);
				const std::vector<uint8_t> A8 = To8(Ad), B8 = To8(Bd);
				const std::vector<uint16_t> A16 = To16(Ad), B16 = To16(Bd);
				const LumaPairView v = cs.r16 ? View16(A16, B16, w, h, cs.s, 255.0 / 65535.0) : View8(A8, B8, w, h, cs.s);
				const RefineResult r = Both(eq, cs.name, Translation(tx + 0.4 * cs.s, 0.0), v, Pic(cs.frameW, cs.frameH), InlierMask(), RefineOptions());
				Assert::IsTrue(r.status == RefineStatus::Refined);
			}
			Report("cost-line cases", eq);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuRealTextureKnownShift)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuRealTextureKnownShift)
		{
			// The CPU test's real-texture pairs (VP_R3_REAL_GRAY / _W / _H) at s = 1, 2 and 4.
			const std::string path = EnvText("VP_R3_REAL_GRAY");
			const int fullW = std::atoi(EnvText("VP_R3_REAL_W").c_str()), fullH = std::atoi(EnvText("VP_R3_REAL_H").c_str());
			if (path.empty() || fullW <= 0 || fullH <= 0)
			{
				GLog("gpu real texture: VP_R3_REAL_GRAY / _W / _H not set, not run\n");
				return;
			}
			std::vector<uint8_t> full(static_cast<size_t>(fullW) * fullH);
			{
				std::ifstream in(path, std::ios::binary);
				Assert::IsTrue(static_cast<bool>(in), L"real texture file not readable");
				in.read(reinterpret_cast<char*>(full.data()), static_cast<std::streamsize>(full.size()));
				Assert::IsTrue(in.gcount() == static_cast<std::streamsize>(full.size()), L"real texture file shorter than W x H");
			}
			const int margin = 16;
			const int shifts[6][2] = { { 3, 1 }, { 5, -2 }, { 7, 3 }, { -2, 5 }, { 1, -3 }, { 6, 2 } };
			const int scales[3] = { 1, 2, 4 };
			for (int si = 0; si < 3; ++si)
			{
				const int s = scales[si];
				const int cw = (fullW - 2 * margin) / s * s, ch = (fullH - 2 * margin) / s * s;
				const Rect pic = Pic(cw, ch);
				Eq eq;
				int refined = 0;
				for (int k = 0; k < 6; ++k)
				{
					const int sx = shifts[k][0], sy = shifts[k][1];
					const std::vector<uint8_t> A = BoxCrop(full, fullW, margin, margin, cw, ch, s, 900 + 2 * k, 1.0);
					const std::vector<uint8_t> B = BoxCrop(full, fullW, margin - sx, margin - sy, cw, ch, s, 901 + 2 * k, 1.0);
					const RefineResult r = Both(eq, "real texture", Translation(sx + 0.4 * s, sy - 0.3 * s), View8(A, B, cw / s, ch / s, s), pic, InlierMask(), RefineOptions());
					if (r.status == RefineStatus::Refined)
						++refined;
				}
				char name[96];
				std::snprintf(name, sizeof(name), "real texture %dx%d s %d (refined %d/6)", fullW, fullH, s, refined);
				Report(name, eq);
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuCostLine)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuCostLine)
		{
			// Q / H / F for 1080 and 4K: Q = quarter 8-bit (s 4), H = HalfLuma8 (s 2), F = full-resolution 10-bit codes as R16
			// (the AnalysisLumaSource upload, codeScale 255 / 1023, s 1). Input path: CPU upload. Per call: wall ms (Submit to
			// the collected result), GPU ms (timestamps), upload ms (UpdateSubresource CPU time + the copy until the GPU is
			// idle), pixels, iterations; QUALITY 2 band: ns per unit (one candidate pixel, or one sampled pixel-iteration
			// counted as 16 taps) must lie in a sane GPU band. Opt-in (VP_R3_GPU_COST=1): timing belongs under GPU-QUIET.
			if (EnvText("VP_R3_GPU_COST") != "1")
			{
				GLog("gpu cost line: VP_R3_GPU_COST not set, not run\n");
				return;
			}
			Rig& g = RequireRig();
			struct Case { const char* name; int frameW, frameH; int s; bool full10; };
			const Case cases[6] = { { "1080 Q", 1920, 1080, 4, false }, { "1080 H", 1920, 1080, 2, false }, { "1080 F", 1920, 1080, 1, true },
				{ "4K Q", 3840, 2160, 4, false }, { "4K H", 3840, 2160, 2, false }, { "4K F", 3840, 2160, 1, true } };
			for (int ci = 0; ci < 6; ++ci)
			{
				const Case& cs = cases[ci];
				const int w = (cs.frameW + cs.s - 1) / cs.s, h = (cs.frameH + cs.s - 1) / cs.s;
				const double s = cs.s, tx = 1.25 * s;
				const std::vector<Wave> waves = MakeWaves(181 + ci, s, 90.0);
				const std::vector<double> Ad = RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, 0, 0)), 1.0, 0.0, 112, 1.0);
				const std::vector<double> Bd = RenderD(w, h, s, std::vector<Layer>(1, Shifted(waves, tx, 0)), 1.0, 0.0, 113, 1.0);
				const std::vector<uint8_t> A8 = To8(Ad), B8 = To8(Bd);
				const std::vector<uint16_t> A10 = To10(Ad), B10 = To10(Bd);
				const LumaPairView v = cs.full10 ? View16(A10, B10, w, h, s, 255.0 / 1023.0) : View8(A8, B8, w, h, s);
				PhotometricRefinerGpu gpu;
				std::string why;
				Assert::IsTrue(gpu.Init(g.device.Get(), &why));
				gpu.EnableTiming(true);
				ComPtr<ID3D11Query> idle;
				D3D11_QUERY_DESC qd = {};
				qd.Query = D3D11_QUERY_EVENT;
				Assert::IsTrue(SUCCEEDED(g.device->CreateQuery(&qd, &idle)));
				std::vector<double> wall, gms, ums;
				RefineResult r;
				for (int k = 0; k < 6; ++k)   // first call warms up (texture / buffer creation), not counted
				{
					RefineGpuLuma gl;
					const auto u0 = std::chrono::steady_clock::now();
					g.input.Set(v);
					Assert::IsTrue(g.input.Prepare(g.context.Get(), &gl, &why));
					g.context->End(idle.Get());
					g.context->Flush();
					while (g.context->GetData(idle.Get(), nullptr, 0, 0) == S_FALSE) {}
					const double up = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
					const auto t0 = std::chrono::steady_clock::now();
					r = gpu.Refine(g.context.Get(), Translation(tx + 0.4 * s, 0.0), gl, Pic(cs.frameW, cs.frameH), InlierMask());
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					if (k == 0)
						continue;
					wall.push_back(ms);
					ums.push_back(up);
					gms.push_back(gpu.LastTiming().valid ? gpu.LastTiming().gpuMs : -1.0);
				}
				std::sort(wall.begin(), wall.end());
				std::sort(gms.begin(), gms.end());
				std::sort(ums.begin(), ums.end());
				const double work = static_cast<double>(w) * h + static_cast<double>(r.pixels) * (r.iterations + 1) * 16.0;
				const double nsPerUnit = gms[2] * 1e6 / work;
				char msg[400];
				std::snprintf(msg, sizeof(msg), "gpu cost %s [%s] %dx%d s %d, cpu upload%s: wall %.3f ms (min %.3f max %.3f), gpu %.3f ms, upload %.3f ms, "
					"%u px, %d it, %s, %.4f ns/unit\n", cs.name, g.adapter.c_str(), w, h, cs.s, cs.full10 ? " (10-bit R16)" : " (8-bit)", wall[2], wall[0],
					wall[4], gms[2], ums[2], r.pixels, r.iterations, RefineStatusText(r.status), nsPerUnit);
				GLog(msg);
				Assert::IsTrue(r.status == RefineStatus::Refined);
				Assert::IsTrue(gms[2] > 0, L"no GPU timing");
				Assert::IsTrue(nsPerUnit > 0.0005 && nsPerUnit < 50.0, L"cost outside the plausible GPU band");
			}
		}
	};
}
