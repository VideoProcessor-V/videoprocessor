// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q2-B5b (scaling.rebase): GPU WARP contract proof that SourceRebase::Module::Apply delegates
// both-axes luma (polar EWA Lanczos sharp) and separable 2-pass chroma (Axis::X rg32f -> Axis::Y rg16)
// directly to ScalerPass::Module bit-for-bit, with same-input kernel-mismatch negative controls and
// ChromaSiting::ShiftAfterResample preservation.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ChromaSiting.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SourceRebaseScalerPassTestsNs
{
	std::wstring Wide(const std::string& s)
	{
		return std::wstring(s.begin(), s.end());
	}

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;

		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_WARN;
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11)
				return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11 ? d3d11->gpu : nullptr; }
	};

	struct TexGuard
	{
		pl_gpu gpu = nullptr;
		pl_tex tex = nullptr;
		TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
		~TexGuard() { if (tex && gpu) pl_tex_destroy(gpu, &tex); }
		TexGuard(const TexGuard&) = delete;
		TexGuard& operator=(const TexGuard&) = delete;
	};

	struct RebaseGuard
	{
		pl_gpu gpu = nullptr;
		SourceRebase::Module& mod;
		RebaseGuard(pl_gpu g, SourceRebase::Module& m) : gpu(g), mod(m) {}
		~RebaseGuard() { mod.Release(gpu); }
		RebaseGuard(const RebaseGuard&) = delete;
		RebaseGuard& operator=(const RebaseGuard&) = delete;
	};

	struct ScalerGuard
	{
		pl_gpu gpu = nullptr;
		ScalerPass::Module& mod;
		ScalerGuard(pl_gpu g, ScalerPass::Module& m) : gpu(g), mod(m) {}
		~ScalerGuard() { mod.Release(gpu); }
		ScalerGuard(const ScalerGuard&) = delete;
		ScalerGuard& operator=(const ScalerGuard&) = delete;
	};

	bool DownloadBytes(pl_gpu gpu, pl_tex tex, std::vector<uint8_t>& out)
	{
		if (!gpu || !tex || !tex->params.format)
			return false;
		const pl_fmt fmt = tex->params.format;
		pl_tex_params rp{};
		rp.w = tex->params.w;
		rp.h = tex->params.h;
		rp.format = fmt;
		rp.host_readable = true;
		rp.blit_dst = true;
		TexGuard readable(gpu, pl_tex_create(gpu, &rp));
		if (!readable.tex)
			return false;
		pl_tex_blit_params bp{};
		bp.src = tex;
		bp.dst = readable.tex;
		pl_tex_blit(gpu, &bp);
		out.assign(static_cast<size_t>(rp.w) * rp.h * fmt->texel_size, 0);
		pl_tex_transfer_params dp{};
		dp.tex = readable.tex;
		dp.ptr = out.data();
		return pl_tex_download(gpu, &dp);
	}

	TEST_CLASS(SourceRebaseScalerPassGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(LumaPolarAndChromaSeparableMatchScalerPassBitForBit)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LumaPolarAndChromaSeparableMatchScalerPassBitForBit)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create libplacebo WARP test device");
			pl_gpu gpu = dev.Gpu();

			const int lumaW = 128, lumaH = 96;
			const int chromaW = lumaW / 2, chromaH = lumaH / 2;
			const double factor = 0.5;
			const int dstLumaW = SourceRebase::Scaled(lumaW, factor);
			const int dstLumaH = SourceRebase::Scaled(lumaH, factor);
			const int dstChromaW = SourceRebase::Scaled(chromaW, factor);
			const int dstChromaH = SourceRebase::Scaled(chromaH, factor);

			// High-contrast diagonal + checkerboard edge pattern so polar EWA Lanczos sharp and
			// separable 2-pass Lanczos 3 produce clearly distinct outputs on the exact same input.
			std::vector<uint16_t> lumaData(static_cast<size_t>(lumaW) * lumaH);
			for (int y = 0; y < lumaH; ++y)
				for (int x = 0; x < lumaW; ++x)
				{
					const bool diag = ((x + y) / 5) % 2 == 0;
					const double ripple = 0.12 * std::sin(0.65 * x) * std::cos(0.55 * y);
					const double v = (std::min)(0.95, (std::max)(0.05, (diag ? 0.80 : 0.20) + ripple));
					lumaData[static_cast<size_t>(y) * lumaW + x] = static_cast<uint16_t>(std::lround(v * 65535.0));
				}

			std::vector<uint16_t> chromaData(static_cast<size_t>(chromaW) * chromaH * 2);
			for (int y = 0; y < chromaH; ++y)
				for (int x = 0; x < chromaW; ++x)
				{
					const bool step = ((x * 2 + y) / 4) % 2 == 0;
					const double cb = step ? 0.82 : 0.18;
					const double cr = (((x + y * 3) / 5) % 2 == 0) ? 0.78 : 0.22;
					const size_t idx = (static_cast<size_t>(y) * chromaW + x) * 2;
					chromaData[idx + 0] = static_cast<uint16_t>(std::lround(cb * 65535.0));
					chromaData[idx + 1] = static_cast<uint16_t>(std::lround(cr * 65535.0));
				}

			pl_fmt r16 = pl_find_named_fmt(gpu, "r16");
			pl_fmt rg16 = pl_find_named_fmt(gpu, "rg16");
			Assert::IsTrue(r16 != nullptr && rg16 != nullptr, L"r16 and rg16 formats required");

			pl_tex_params lp{};
			lp.w = lumaW; lp.h = lumaH; lp.format = r16; lp.sampleable = true; lp.initial_data = lumaData.data();
			TexGuard lumaTex(gpu, pl_tex_create(gpu, &lp));
			Assert::IsNotNull(lumaTex.tex, L"luma r16 upload failed");

			pl_tex_params cp{};
			cp.w = chromaW; cp.h = chromaH; cp.format = rg16; cp.sampleable = true; cp.initial_data = chromaData.data();
			TexGuard chromaTex(gpu, pl_tex_create(gpu, &cp));
			Assert::IsNotNull(chromaTex.tex, L"chroma rg16 upload failed");

			pl_frame src{};
			src.num_planes = 2;
			src.planes[0].texture = lumaTex.tex;
			src.planes[0].components = 1;
			src.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			src.planes[0].shift_x = 0.0f;
			src.planes[0].shift_y = 0.0f;
			src.planes[1].texture = chromaTex.tex;
			src.planes[1].components = 2;
			src.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			src.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			src.planes[1].shift_x = -0.5f;
			src.planes[1].shift_y = 0.25f;
			src.repr.sys = PL_COLOR_SYSTEM_BT_709;
			src.repr.levels = PL_COLOR_LEVELS_LIMITED;
			src.color = pl_color_space_bt709;
			src.crop = { 0.0f, 0.0f, static_cast<float>(lumaW), static_cast<float>(lumaH) };

			SourceRebase::Module rebase;
			RebaseGuard rebaseGuard(gpu, rebase);
			rebase.SetKernel(-1);
			rebase.separable = true;
			rebase.antiring = 0.5f;

			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dev.dispatch, src, factor, factor, rebased, 0),
				Wide("SourceRebase::Apply failed: " + rebase.LastError()).c_str());
			Assert::AreEqual(0ull, rebase.Failures());
			Assert::IsFalse(rebase.KernelUsed());
			Assert::IsNotNull(rebased.planes[0].texture);
			Assert::IsNotNull(rebased.planes[1].texture);
			Assert::AreEqual(dstLumaW, rebased.planes[0].texture->params.w);
			Assert::AreEqual(dstLumaH, rebased.planes[0].texture->params.h);
			Assert::AreEqual(dstChromaW, rebased.planes[1].texture->params.w);
			Assert::AreEqual(dstChromaH, rebased.planes[1].texture->params.h);

			std::vector<uint8_t> rebaseLumaBytes, rebaseChromaBytes;
			Assert::IsTrue(DownloadBytes(gpu, rebased.planes[0].texture, rebaseLumaBytes), L"luma download failed");
			Assert::IsTrue(DownloadBytes(gpu, rebased.planes[1].texture, rebaseChromaBytes), L"chroma download failed");

			// 1. Luma (plane 0, kernel = -1, !oneAxis) matches ScalerPass::Module with Kind::EwaLanczosSharp bit-for-bit!
			{
				ScalerPass::Module polarPass;
				ScalerGuard polarGuard(gpu, polarPass);
				ScalerPass::Settings st;
				st.filter.kind = ScalerKernels::Kind::EwaLanczosSharp;
				st.axis = ScalerPass::Axis::Both;
				st.plane = ScalerPass::Plane::Luma;
				st.precision = ScalerPass::Precision::Target;
				st.targetFormat = r16;
				st.antiring = rebase.antiring;
				const ScalerPass::Source spSrc = { lumaTex.tex, 0.0f, 0.0f, static_cast<float>(lumaW), static_cast<float>(lumaH) };
				pl_tex polarOut = nullptr;
				Assert::IsTrue(polarPass.Apply(gpu, dev.dispatch, spSrc, dstLumaW, dstLumaH, st, polarOut, 0),
					Wide("polar ScalerPass failed: " + polarPass.LastError()).c_str());
				std::vector<uint8_t> polarBytes;
				Assert::IsTrue(DownloadBytes(gpu, polarOut, polarBytes));
				Assert::IsTrue(rebaseLumaBytes == polarBytes,
					L"B5b: SourceRebase luma must match ScalerPass(EwaLanczosSharp) bit-for-bit");

				// Mandatory same-input negative control: separable Lanczos3 on the exact same sharp edge pattern MUST differ!
				ScalerPass::Module sepPass;
				ScalerGuard sepGuard(gpu, sepPass);
				ScalerPass::Settings sepSt;
				sepSt.filter.kind = ScalerKernels::Kind::Lanczos;
				sepSt.filter.radius = 3;
				sepSt.axis = ScalerPass::Axis::Both;
				sepSt.plane = ScalerPass::Plane::Luma;
				sepSt.precision = ScalerPass::Precision::Target;
				sepSt.order = ScalerPass::Order::AcrossFirst;
				sepSt.targetFormat = r16;
				sepSt.antiring = rebase.antiring;
				pl_tex sepOut = nullptr;
				Assert::IsTrue(sepPass.Apply(gpu, dev.dispatch, spSrc, dstLumaW, dstLumaH, sepSt, sepOut, 0),
					Wide("separable negative-control ScalerPass failed: " + sepPass.LastError()).c_str());
				std::vector<uint8_t> sepBytes;
				Assert::IsTrue(DownloadBytes(gpu, sepOut, sepBytes));
				Assert::IsFalse(rebaseLumaBytes == sepBytes,
					L"negative control: separable Lanczos3 must differ from polar EwaLanczosSharp on the exact same diagonal edges");
			}

			// 2. Chroma (plane 1, separable = true, !oneAxis) matches ScalerPass 2-pass (Axis::X rg32f -> Axis::Y rg16) bit-for-bit!
			{
				const auto renderCaps = static_cast<enum pl_fmt_caps>(
					PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR);
				pl_fmt rg32f = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32, renderCaps);
				Assert::IsNotNull(rg32f, L"rg32f intermediate format required on WARP");

				ScalerPass::Module passXMod, passYMod;
				ScalerGuard guardX(gpu, passXMod), guardY(gpu, passYMod);

				ScalerPass::Settings stX;
				stX.filter.kind = ScalerKernels::Kind::Lanczos;
				stX.filter.radius = 3;
				stX.axis = ScalerPass::Axis::X;
				stX.plane = ScalerPass::Plane::Chroma;
				stX.precision = ScalerPass::Precision::Target;
				stX.targetFormat = rg32f;
				stX.antiring = rebase.antiring;
				const ScalerPass::Source cSrc = { chromaTex.tex, 0.0f, 0.0f, static_cast<float>(chromaW), static_cast<float>(chromaH) };
				pl_tex tempTex = nullptr;
				Assert::IsTrue(passXMod.Apply(gpu, dev.dispatch, cSrc, dstChromaW, chromaH, stX, tempTex, 0),
					Wide("chroma pass X failed: " + passXMod.LastError()).c_str());

				ScalerPass::Settings stY = stX;
				stY.axis = ScalerPass::Axis::Y;
				stY.targetFormat = rg16;
				const ScalerPass::Source tempSrc = { tempTex, 0.0f, 0.0f, static_cast<float>(tempTex->params.w), static_cast<float>(tempTex->params.h) };
				pl_tex chromaOut = nullptr;
				Assert::IsTrue(passYMod.Apply(gpu, dev.dispatch, tempSrc, dstChromaW, dstChromaH, stY, chromaOut, 0),
					Wide("chroma pass Y failed: " + passYMod.LastError()).c_str());

				std::vector<uint8_t> scalerChromaBytes;
				Assert::IsTrue(DownloadBytes(gpu, chromaOut, scalerChromaBytes));
				Assert::IsTrue(rebaseChromaBytes == scalerChromaBytes,
					L"B5b: SourceRebase separable chroma must match ScalerPass 2-pass (Axis::X rg32f -> Axis::Y rg16) bit-for-bit");

				// Mandatory same-input negative control: polar EWA Lanczos sharp on the exact same chroma input MUST differ!
				ScalerPass::Module polarChromaMod;
				ScalerGuard polarChromaGuard(gpu, polarChromaMod);
				ScalerPass::Settings polarC;
				polarC.filter.kind = ScalerKernels::Kind::EwaLanczosSharp;
				polarC.axis = ScalerPass::Axis::Both;
				polarC.plane = ScalerPass::Plane::Chroma;
				polarC.precision = ScalerPass::Precision::Target;
				polarC.targetFormat = rg16;
				polarC.antiring = rebase.antiring;
				pl_tex polarChromaOut = nullptr;
				Assert::IsTrue(polarChromaMod.Apply(gpu, dev.dispatch, cSrc, dstChromaW, dstChromaH, polarC, polarChromaOut, 0));
				std::vector<uint8_t> polarChromaBytes;
				Assert::IsTrue(DownloadBytes(gpu, polarChromaOut, polarChromaBytes));
				Assert::IsFalse(rebaseChromaBytes == polarChromaBytes,
					L"negative control: polar EwaLanczosSharp chroma must differ from 2-pass separable Lanczos3 on the exact same sharp chroma pattern");
			}

			// 3. Siting shift and crop preservation via ChromaSiting::ShiftAfterResample.
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[0].shift_x, factor), rebased.planes[0].shift_x, 1e-6f);
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[0].shift_y, factor), rebased.planes[0].shift_y, 1e-6f);
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[1].shift_x, factor), rebased.planes[1].shift_x, 1e-6f);
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[1].shift_y, factor), rebased.planes[1].shift_y, 1e-6f);
			Assert::AreEqual(static_cast<float>(dstLumaW), rebased.crop.x1, 1e-5f);
			Assert::AreEqual(static_cast<float>(dstLumaH), rebased.crop.y1, 1e-5f);
		}
	};
}
