// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>
#include <VP-V/modules/repair/grain_split/GrainSplitVariantOwner.h>
#include <vprenderer/GrainPreserve.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(VpDebanderHookTests)
	{
	public:


		TEST_METHOD(VpDebanderStrengthSnapsOccupancyReadingToContentLevels)
		{
			// Hunt 2 sweep: the occupancy reading puts 8-bit-in-10 at ~8.8; the snapped step is the real 4 codes.
			Assert::AreEqual(6.0, VpDebander::Strength(8.8, 10, 1.0, true, true), 1e-9, L"8.8-bit reading: the 8-bit step (6 codes)");
			Assert::IsTrue(VpDebander::Strength(8.8, 10, 1.0, true, false) < 4.0, L"unsnapped, the threshold falls below the 4-code step (the fault)");
			Assert::AreEqual(6.0, VpDebander::Strength(8.0, 10, 1.0, true, true), 1e-9, L"exact 8-bit unchanged");
			Assert::AreEqual(0.0, VpDebander::Strength(9.6, 10, 1.0, true, true), L"true 10-bit with grain (9.6) is not debanded");
		}

		TEST_METHOD(VpDebanderSmoothsBandsKeepsTextureAndHoldsAlongTheVectors)
		{
			// Stage 12 §2b / §2d (owner 2026-10-15: any low-bitrate source,
			// vector-informed). 10-bit codes in a P010-style r16 plane.
			Assert::AreEqual(0.0, VpDebander::Strength(10.0, 10, 1.0, true), L"true 10-bit, clean: nothing to deband");
			Assert::AreEqual(6.0, VpDebander::Strength(8.0, 10, 1.0, true), 1e-9, L"8-bit in 10: 1.5 x the 4-code step");
			Assert::IsTrue(VpDebander::Strength(10.0, 10, 0.2, true) > 6.0, L"low bitrate quality raises it");
			Assert::AreEqual(0.0, VpDebander::Strength(10.0, 10, 0.2, false), L"low_bitrate_handling off: quality ignored");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 512, H = 256;
			auto upload = [&](const std::vector<int>& codes) {
				std::vector<uint16_t> data(codes.size());
				for (size_t i = 0; i < codes.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(1023, (std::max)(0, codes[i])) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readback = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<double> v(static_cast<size_t>(W) * H);
				if (readable->params.format->type == PL_FMT_FLOAT)
				{
					std::vector<uint16_t> half(v.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < v.size(); ++i) v[i] = HalfBits(half[i]) * 65535.0 / 64.0;
				}
				else
				{
					std::vector<uint16_t> raw(v.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < v.size(); ++i) v[i] = raw[i] / 64.0;   // back to 10-bit codes
				}
				pl_tex_destroy(gpu, &readable);
				return v;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			// A shallow 8-bit ramp in the 10-bit signal (a sky): 4-code steps every 32 px.
			std::vector<int> ramp(static_cast<size_t>(W) * H), texture(ramp.size());
			std::mt19937 rng(9);
			std::normal_distribution<double> noise(0.0, 12.0);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					ramp[static_cast<size_t>(y) * W + x] = 4 * (100 + x / 32);
					texture[static_cast<size_t>(y) * W + x] = static_cast<int>(std::lround(500 + noise(rng)));
				}
			VpDebander::Params params;
			params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
			params.colorDepth = 10;
			params.radiusScale = 1080.0 / H;   // 16 px at this picture's 'height' scale
			params.grainGuard = false;   // written for the 2026-10-16 debander (the guard is the default since the integration)
			VpDebander::Module deband;
			pl_tex rampTex = upload(ramp), texTex = upload(texture);
			pl_frame out{};
			Assert::IsTrue(deband.Apply(gpu, dispatch, frameOf(rampTex), params, out), ExtendedRangeGpuProbe::ToWide(deband.LastError()).c_str());
			const std::vector<double> smoothed = readback(out.planes[0].texture);
			// Visible banding: how far the column means depart from a straight
			// ramp (least-squares line); a 4-code staircase is ~1.15 codes RMS.
			auto staircase = [&](auto at) {
				std::vector<double> col;
				for (int x = 80; x < W - 80; ++x)
				{
					double m = 0;
					for (int y = 40; y < H - 40; ++y) m += at(static_cast<size_t>(y) * W + x);
					col.push_back(m / (H - 80));
				}
				double sx = 0, sy = 0, sxx = 0, sxy = 0;
				const double k = static_cast<double>(col.size());
				for (size_t i = 0; i < col.size(); ++i) { sx += i; sy += col[i]; sxx += double(i) * i; sxy += i * col[i]; }
				const double b = (k * sxy - sx * sy) / (k * sxx - sx * sx), a = (sy - b * sx) / k;
				double e = 0;
				for (size_t i = 0; i < col.size(); ++i) e += (col[i] - a - b * i) * (col[i] - a - b * i);
				return std::sqrt(e / k);
			};
			const double stepIn = staircase([&](size_t i) { return static_cast<double>(ramp[i]); });
			const double stepOut = staircase([&](size_t i) { return smoothed[i]; });
			double meanIn = 0, meanOut = 0;
			for (int y = 40; y < H - 40; ++y)
				for (int x = 80; x < W - 80; ++x) { const size_t i = static_cast<size_t>(y) * W + x; meanIn += ramp[i]; meanOut += smoothed[i]; }
			const double n = static_cast<double>((H - 80) * (W - 160));
			// Texture: the same pass on noise keeps its variance.
			Assert::IsTrue(deband.Apply(gpu, dispatch, frameOf(texTex), params, out, 1));
			const std::vector<double> tex = readback(out.planes[0].texture);
			double varIn = 0, varOut = 0;
			for (size_t i = 0; i < tex.size(); ++i) { varIn += (texture[i] - 500.0) * (texture[i] - 500.0); varOut += (tex[i] - 500.0) * (tex[i] - 500.0); }
			char line[220];
			snprintf(line, sizeof(line), "deband: banding (RMS off a straight ramp) %.2f -> %.2f codes, mean %.3f -> %.3f; texture variance kept %.1f%%\n",
				stepIn, stepOut, meanIn / n, meanOut / n, 100.0 * varOut / varIn);
			Logger::WriteMessage(line);
			Assert::IsTrue(stepOut < 0.5 * stepIn, L"banding at least halved");
			Assert::AreEqual(meanIn / n, meanOut / n, 0.25, L"the level kept");
			Assert::IsTrue(varOut > 0.9 * varIn, L"texture kept");

			// Vectors: a field (one texel per 16 px block: vx, vy, trust, error)
			// moving the content 64 px right. Frame 1: left half banded (blurred,
			// w = 1), right half texture (w = 0). Frame 2: all texture. Held, the
			// columns the banded half moved into keep blurring partly; beyond the
			// motion, nothing is blurred.
			std::vector<float> fieldData(static_cast<size_t>(W / 16) * (H / 16) * 4);
			for (size_t i = 0; i < fieldData.size(); i += 4) { fieldData[i] = 64.0f; fieldData[i + 1] = 0.0f; fieldData[i + 2] = 1.0f; fieldData[i + 3] = 0.0f; }
			pl_tex_params fp{};
			fp.w = W / 16; fp.h = H / 16; fp.format = pl_find_named_fmt(gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = fieldData.data();
			pl_tex field = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(field, L"field texture");
			std::vector<int> half(ramp.size());
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					half[static_cast<size_t>(y) * W + x] = x < W / 2 ? ramp[static_cast<size_t>(y) * W + x] : texture[static_cast<size_t>(y) * W + x];
			pl_tex halfTex = upload(half);
			auto blurredShare = [&](const std::vector<double>& o, int x0, int x1) {
				double sum = 0; int count = 0;
				for (int y = 40; y < H - 40; ++y)
					for (int x = x0; x < x1; ++x) { const size_t i = static_cast<size_t>(y) * W + x; sum += std::fabs(o[i] - texture[i]); ++count; }
				return sum / count;
			};
			double heldMoved = 0, heldBeyond = 0, frameMoved = 0, frameBeyond = 0;
			for (int held = 0; held < 2; ++held)
			{
				VpDebander::Module m;
				VpDebander::Params p = params;
				p.field = held ? field : nullptr;
				p.sequence = 10;
				Assert::IsTrue(m.Apply(gpu, dispatch, frameOf(halfTex), p, out));
				p.sequence = 11;
				Assert::IsTrue(m.Apply(gpu, dispatch, frameOf(texTex), p, out));
				const std::vector<double> o = readback(out.planes[0].texture);
				// Columns whose previous position (64 px left) was the banded half's
				// interior (fully blurred there), and columns whose previous position
				// was texture.
				const double moved = blurredShare(o, 80, W / 2 - 80), beyond = blurredShare(o, W / 2 + 160, W - 16);
				std::string profile = held ? "held profile:" : "per-frame profile:";
				for (int x0 = 0; x0 < W; x0 += 32)
				{
					char cell[16];
					snprintf(cell, sizeof(cell), " %.2f", blurredShare(o, x0, x0 + 32));
					profile += cell;
				}
				Logger::WriteMessage((profile + "\n").c_str());
				snprintf(line, sizeof(line), "%s: change where the banded half was %.2f codes, where texture was %.2f; held frames %llu\n",
					held ? "held along the vectors" : "per frame (no vectors)", moved, beyond, m.HeldFrames());
				Logger::WriteMessage(line);
				if (held)
				{
					Assert::AreEqual(1ull, m.HeldFrames());
					heldMoved = moved;
					heldBeyond = beyond;
				}
				else
				{
					Assert::AreEqual(0ull, m.HeldFrames());
					Assert::IsTrue(moved < 1.0 && beyond < 1.0, L"without vectors: per-frame decisions (texture almost untouched)");
					frameMoved = moved;
					frameBeyond = beyond;
				}
				m.Release(gpu);
			}
			// Held, the weights follow the motion: more blur (inside this frame's
			// blur band) where the banded area was, less where texture was; never
			// more blur on texture than this frame's own decisions give.
			Assert::IsTrue(heldMoved > 1.5 * heldBeyond, L"the blur weight followed the motion");
			Assert::IsTrue(heldMoved > frameMoved, L"steadier blur where the banded area was");
			Assert::IsTrue(heldBeyond <= 1.05 * frameBeyond + 0.02, L"held never blurs texture more than this frame does");
			pl_tex_destroy(gpu, &rampTex); pl_tex_destroy(gpu, &texTex); pl_tex_destroy(gpu, &halfTex); pl_tex_destroy(gpu, &field);
			deband.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(VpDebanderKeepsGrainWithoutVectors)
		{
			// deband_vp_grain_guard: without vectors, grain of 0.5 8-bit code (2 10-bit codes) and
			// more keeps 0.95x or more at the 8-bit-in-10 threshold (6 codes) and at the low-bitrate
			// maximum (12 codes); a banded ramp is debanded as before (within a code); off is the
			// never-switched module's output byte for byte (0.5-0.6x of the grain).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 256, H = 128, margin = 24;
			auto upload = [&](const std::vector<int>& codes) {
				std::vector<uint16_t> data(codes.size());
				for (size_t i = 0; i < codes.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(1023, (std::max)(0, codes[i])) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readback = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &readable);
				return raw;
			};
			auto toCodes = [&](const std::vector<uint16_t>& raw) {
				GrainModel::Image g(W, H);
				for (size_t i = 0; i < raw.size(); ++i) g.v[i] = raw[i] / 64.0;
				return g;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			GrainModel::Image ramp(W, H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					ramp.at(x, y) = 400.0 + 0.28 * x + 0.12 * y;   // 10-bit codes
			for (double threshold : { VpDebander::Strength(8.0, 10, 1.0, true), VpDebander::Strength(8.0, 10, 0.0, true) })
				for (bool soft : { false, true })
					for (double sigma8 : { 0.5, 1.0, 2.0, 4.0 })
					{
						std::mt19937 rng(61u + static_cast<unsigned>(sigma8 * 4) + (soft ? 100u : 0u));
						const GrainModel::Image g = GrainModel::Grain(W, H, sigma8 * 4.0, soft, rng);   // 10-bit codes
						std::vector<int> codes(static_cast<size_t>(W) * H);
						for (size_t i = 0; i < codes.size(); ++i) codes[i] = static_cast<int>(std::lround(ramp.v[i] + g.v[i]));
						pl_tex tex = upload(codes);
						GrainModel::Image input(W, H);
						for (size_t i = 0; i < codes.size(); ++i) input.v[i] = codes[i];
						const double source = GrainModel::Rms(input - ramp, margin);
						VpDebander::Params params;
						params.thresholdCodes = threshold;
						params.colorDepth = 10;
						params.radiusScale = 1080.0 / H;
						params.grainGuard = false;   // "never": the 2026-10-16 debander (the guard is the struct default since the integration)
						VpDebander::Module never, guarded, legacy;
						pl_frame outNever{}, outGuard{}, outLegacy{};
						Assert::IsTrue(never.Apply(gpu, dispatch, frameOf(tex), params, outNever), ExtendedRangeGpuProbe::ToWide(never.LastError()).c_str());
						VpDebander::Params on = params;
						on.grainGuard = true;
						on.grainSplit = false;   // the spatial-share veto (VpDebanderGrainSplitDebandsNoisyBands: the split)
						Assert::IsTrue(guarded.Apply(gpu, dispatch, frameOf(tex), on, outGuard), ExtendedRangeGpuProbe::ToWide(guarded.LastError()).c_str());
						VpDebander::Params off = params;
						off.grainGuard = false;
						Assert::IsTrue(legacy.Apply(gpu, dispatch, frameOf(tex), off, outLegacy), ExtendedRangeGpuProbe::ToWide(legacy.LastError()).c_str());
						const std::vector<uint16_t> rawNever = readback(outNever.planes[0].texture);
						const std::vector<uint16_t> rawLegacy = readback(outLegacy.planes[0].texture);
						const double rg = GrainModel::Rms(toCodes(readback(outGuard.planes[0].texture)) - ramp, margin) / source;
						const double rl = GrainModel::Rms(toCodes(rawLegacy) - ramp, margin) / source;
						char text[160];
						snprintf(text, sizeof(text), "%s grain %.1f codes, threshold %.0f: guard %.3f legacy %.3f", soft ? "soft" : "white", sigma8, threshold, rg, rl);
						Logger::WriteMessage(text);
						const std::wstring wtext(text, text + strlen(text));
						Assert::IsTrue(rawLegacy == rawNever, (L"off: the established debander byte for byte; " + wtext).c_str());
						Assert::IsTrue(rg > 0.95 && rg < 1.05, wtext.c_str());
						if (sigma8 == 0.5) Assert::IsTrue(rl < 0.8, wtext.c_str());
						pl_tex_destroy(gpu, &tex);
						never.Release(gpu); guarded.Release(gpu); legacy.Release(gpu);
					}
			// A banded ramp (8-bit steps in the 10-bit signal): debanded with the guard as without.
			{
				std::vector<int> banded(static_cast<size_t>(W) * H);
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
						banded[static_cast<size_t>(y) * W + x] = 4 * (100 + x / 24);
				pl_tex tex = upload(banded);
				VpDebander::Params params;
				params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
				params.colorDepth = 10;
				params.radiusScale = 1080.0 / H;
				params.grainGuard = false;   // without the guard (the struct default since the integration is on)
				VpDebander::Params on = params;
				on.grainGuard = true;
				on.grainSplit = false;   // the spatial-share veto
				VpDebander::Module a, b;
				pl_frame oa{}, ob{};
				Assert::IsTrue(a.Apply(gpu, dispatch, frameOf(tex), params, oa));
				Assert::IsTrue(b.Apply(gpu, dispatch, frameOf(tex), on, ob));
				const std::vector<uint16_t> ra = readback(oa.planes[0].texture), rb = readback(ob.planes[0].texture);
				int worst = 0, changed = 0;
				for (size_t i = 0; i < ra.size(); ++i)
				{
					worst = (std::max)(worst, std::abs(int(ra[i]) - int(rb[i])) / 64);
					changed += ra[i] != static_cast<uint16_t>(banded[i] << 6);
				}
				Assert::IsTrue(worst <= 1, L"bands debanded with the guard as without (within a code)");
				Assert::IsTrue(changed > static_cast<int>(ra.size()) / 4, L"the bands are debanded");
				a.Release(gpu); b.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(VpDebanderGrainSplitDebandsNoisyBands)
		{
			// deband_vp_grain_guard on = the grain split (hunt 2 07-scaling H1): the shader follows
			// the CPU model (GrainModel::DebandSplit) within a code on 99 % of the pixels; grain of
			// 0.5-4 8-bit codes keeps 0.95-1.05x; 8-bit bands 16 px wide with 2 codes of noise
			// lose 70 % or more of their fundamental (the spatial-share veto: under 30 %) and
			// pass the noise at 0.95x or more. radiusScale 2 = a 2160-line source (grain scale 1,
			// radius 32 px).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 256, H = 128, margin = 24;
			const double radius = 32.0;
			auto upload = [&](const GrainModel::Image& codes) {
				std::vector<uint16_t> data(codes.v.size());
				for (size_t i = 0; i < codes.v.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(1023L, (std::max)(0L, std::lround(codes.v[i]))) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readCodes = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &readable);
				GrainModel::Image g(W, H);
				for (size_t i = 0; i < raw.size(); ++i) g.v[i] = raw[i] / 64.0;
				return g;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			auto run = [&](const GrainModel::Image& img, double threshold, bool split) {
				pl_tex tex = upload(img);
				VpDebander::Params params;
				params.thresholdCodes = threshold;
				params.colorDepth = 10;
				params.radiusScale = 2.0;
				params.grainGuard = true;
				params.grainSplit = split;
				VpDebander::Module module;
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dispatch, frameOf(tex), params, out), ExtendedRangeGpuProbe::ToWide(module.LastError()).c_str());
				const GrainModel::Image result = readCodes(out.planes[0].texture);
				module.Release(gpu);
				pl_tex_destroy(gpu, &tex);
				return result;
			};
			auto matchesModel = [&](const GrainModel::Image& gpuOut, const GrainModel::Image& img, double threshold, const char* label) {
				const GrainModel::Image cpu = GrainModel::DebandSplit(img, threshold, radius);
				int off = 0;
				for (size_t i = 0; i < cpu.v.size(); ++i)
					off += std::fabs(gpuOut.v[i] - cpu.v[i]) > 1.0 ? 1 : 0;
				char text[160];
				snprintf(text, sizeof(text), "%s: %d of %d pixels more than a code from the CPU model", label, off, static_cast<int>(cpu.v.size()));
				Logger::WriteMessage(text);
				Assert::IsTrue(off * 100 <= static_cast<int>(cpu.v.size()), std::wstring(text, text + strlen(text)).c_str());
			};
			GrainModel::Image ramp(W, H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					ramp.at(x, y) = 400.0 + 0.28 * x + 0.12 * y;
			for (double threshold : { VpDebander::Strength(8.0, 10, 1.0, true), VpDebander::Strength(8.0, 10, 0.0, true) })
				for (bool soft : { false, true })
					for (double sigma8 : { 0.5, 1.0, 2.0, 4.0 })
					{
						std::mt19937 rng(61u + static_cast<unsigned>(sigma8 * 4) + (soft ? 100u : 0u));
						const GrainModel::Image g = GrainModel::Grain(W, H, sigma8 * 4.0, soft, rng);
						GrainModel::Image img(W, H);
						for (size_t i = 0; i < img.v.size(); ++i) img.v[i] = static_cast<double>(std::lround(ramp.v[i] + g.v[i]));
						const GrainModel::Image out = run(img, threshold, true);
						const double kept = GrainModel::Rms(out - ramp, margin) / GrainModel::Rms(img - ramp, margin);
						char text[160];
						snprintf(text, sizeof(text), "%s grain %.1f codes, threshold %.0f: split %.3f", soft ? "soft" : "white", sigma8, threshold, kept);
						Logger::WriteMessage(text);
						Assert::IsTrue(kept > 0.95 && kept < 1.05, std::wstring(text, text + strlen(text)).c_str());
						if (sigma8 == 1.0) matchesModel(out, img, threshold, text);
					}
			// Noisy bands: 8-bit steps every 16 px, white noise of 2 10-bit codes.
			{
				const double threshold = VpDebander::Strength(8.0, 10, 0.0, true);
				GrainModel::Image bandRamp(W, H), bands(W, H), noise(W, H);
				std::mt19937 rng(4242u);
				const GrainModel::Image g = GrainModel::Grain(W, H, 2.0, false, rng);
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
					{
						bandRamp.at(x, y) = 400.0 + 4.0 * x / 16.0;
						bands.at(x, y) = 4.0 * std::round(bandRamp.at(x, y) / 4.0);
						noise.at(x, y) = std::round(g.at(x, y));
					}
				const GrainModel::Image img = bands + noise;
				auto fundamental = [&](const GrainModel::Image& err) {
					double re = 0.0, im = 0.0;
					int n = 0;
					for (int y = margin; y < H - margin; ++y)
						for (int x = margin; x < W - margin; ++x)
						{
							re += err.at(x, y) * std::cos(6.283185307179586 * x / 16.0);
							im -= err.at(x, y) * std::sin(6.283185307179586 * x / 16.0);
							++n;
						}
					return 2.0 * std::hypot(re, im) / n;
				};
				const GrainModel::Image split = run(img, threshold, true), veto = run(img, threshold, false), clean = run(bands, threshold, true);
				const double before = fundamental(img - bandRamp);
				const double kept = fundamental(split - bandRamp) / before, vetoKept = fundamental(veto - bandRamp) / before;
				const double pass = GrainModel::Rms(split - clean, margin) / GrainModel::Rms(noise, margin);
				char text[200];
				snprintf(text, sizeof(text), "noisy bands: fundamental kept split %.3f veto %.3f; noise passed %.3f", kept, vetoKept, pass);
				Logger::WriteMessage(text);
				const std::wstring wtext(text, text + strlen(text));
				Assert::IsTrue(kept < 0.3, wtext.c_str());
				Assert::IsTrue(vetoKept > 0.7, wtext.c_str());
				Assert::IsTrue(pass > 0.95 && pass < 1.05, wtext.c_str());
				matchesModel(split, img, threshold, "noisy bands");
			}
			pl_dispatch_destroy(&dispatch);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(VpDebanderGuardMatchesTheBusyShareOracle)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(VpDebanderGuardMatchesTheBusyShareOracle)
		{
			// C7-6 U5 G1 parity: the debander's grain guard (the spatial-share veto) is the regions owner's busy-share kernel
			// (RegionEvidence::BusyShareBodyGlsl) with bz_scale 1 and the thresholds in the plane's normalised units. Its texture
			// must carry the CPU oracle's (BusyEdge::BusyShare) decisions on the same float taps, pixel for pixel: per cell the
			// share is within a quarter of one pixel's weight (one flipped decision moves a share by 1 / the cell's pixels).
			// Planes: a 1-component luma and a 2-component chroma (bz_ncomp 2), partial cells on both axes, integer codes on a
			// ramp (the absolute value varies the float rounding) plus small noise. Threshold edges: at 12 bits with thresholds
			// 12 and 24 the extremum level is exactly 1 and 2 codes, so a neighbour step of that many codes sits ON the comparison.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			struct Case { int depth; double threshold; };
			const Case cases[3] = { { 12, 12.0 }, { 12, 24.0 }, { 10, 6.0 } };
			for (const Case& cs : cases)
			{
				const int shift = 16 - cs.depth, maxCode = (1 << cs.depth) - 1;
				const int lw = 72, lh = 40, cw = 36, ch = 20;   // 4.5 x 2.5 and 2.25 x 1.25 cells
				std::mt19937 rng(7u + static_cast<unsigned>(cs.depth * 100 + cs.threshold));
				std::uniform_int_distribution<int> noise(-1, 2);
				auto code = [&](int x, int y, int k, int noisyBelowX) {
					// Top: near-flat steps (extremum edges); bottom: a steep ramp (Laplacian edges at varied absolute codes). Noise of
					// -1..2 codes of the signal left of noisyBelowX (neighbour steps of 1, 2, 3 codes: on and around the levels); the
					// clean part right of it keeps both decisions in the picture.
					const int base = y < 20 ? (maxCode / 3 + 3 * (x / 9) + 5 * k) : (maxCode / 8 + 37 * x + 11 * y + 101 * k);
					return (std::min)(maxCode, (std::max)(0, base + (x < noisyBelowX ? noise(rng) : 0)));
				};
				std::vector<uint16_t> luma(static_cast<size_t>(lw) * lh), chroma(static_cast<size_t>(cw) * ch * 2);
				for (int y = 0; y < lh; ++y)
					for (int x = 0; x < lw; ++x) luma[static_cast<size_t>(y) * lw + x] = static_cast<uint16_t>(code(x, y, 0, 48) << shift);
				for (int y = 0; y < ch; ++y)
					for (int x = 0; x < cw; ++x)
						for (int k = 0; k < 2; ++k)
							chroma[(static_cast<size_t>(y) * cw + x) * 2 + k] = static_cast<uint16_t>(code(x, y * 2, k + 1, 24) << shift);
				auto upload = [&](int w, int h, const char* fmt, const std::vector<uint16_t>& data) {
					pl_tex_params p{};
					p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, fmt); p.sampleable = true; p.initial_data = data.data();
					Assert::IsNotNull(p.format, L"no 16-bit unorm upload format");
					return pl_tex_create(gpu, &p);
				};
				pl_tex lt = upload(lw, lh, "r16", luma), ct = upload(cw, ch, "rg16", chroma);
				Assert::IsTrue(lt && ct, L"upload");
				pl_frame f{};
				f.num_planes = 2;
				f.planes[0].texture = lt; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.planes[1].texture = ct; f.planes[1].components = 2;
				f.planes[1].component_mapping[0] = PL_CHANNEL_CB; f.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = cs.depth; f.repr.bits.bit_shift = shift;
				f.color = pl_color_space_bt709;
				VpDebander::Params params;
				params.thresholdCodes = cs.threshold;
				params.colorDepth = cs.depth;
				params.radiusScale = 1080.0 / lh;
				params.grainGuard = true;
				params.grainSplit = false;   // the spatial-share veto: the guard pass runs
				VpDebander::Module deband;
				pl_frame out{};
				Assert::IsTrue(deband.Apply(gpu, dispatch, f, params, out), ExtendedRangeGpuProbe::ToWide(deband.LastError()).c_str());
				pl_gpu_finish(gpu);
				// The thresholds exactly as the debander forms them (one code in normalised units, VpDebander.cpp).
				const double codeUnit = std::pow(2.0, shift) / 65535.0;
				const double extCodes = GrainPreserve::GuardExtremumCodes(cs.threshold, cs.depth);
				const float lapMin = static_cast<float>(GrainPreserve::GuardLaplacianCodes(cs.threshold, cs.depth) * codeUnit);
				const float extMin = static_cast<float>(extCodes * codeUnit);
				int flips = 0, edgeSteps = 0;
				for (int planeIndex = 0; planeIndex < 2; ++planeIndex)
				{
					const int w = planeIndex == 0 ? lw : cw, h = planeIndex == 0 ? lh : ch, n = planeIndex == 0 ? 1 : 2;
					const std::vector<uint16_t>& raw = planeIndex == 0 ? luma : chroma;
					// The oracle's taps: the GPU's unorm16 sample, raw / 65535 rounded to float.
					std::vector<float> comp[2];
					for (int k = 0; k < n; ++k)
					{
						comp[k].resize(static_cast<size_t>(w) * h);
						for (size_t i = 0; i < comp[k].size(); ++i) comp[k][i] = static_cast<float>(raw[i * n + k] / 65535.0);
					}
					RegionEvidence::BusyEdge::PlaneGroup g;
					g.w = w; g.h = h; g.n = n; g.comp[0] = comp[0].data(); g.comp[1] = n > 1 ? comp[1].data() : nullptr;
					const std::vector<float> want = RegionEvidence::BusyEdge::BusyShare(g, 16, lapMin, extMin);
					// Horizontal neighbour steps exactly on an integer extremum level: the threshold edges this case exercises.
					if (extCodes == std::floor(extCodes))
						for (int k = 0; k < n; ++k)
							for (int y = 0; y < h; ++y)
								for (int x = 1; x < w; ++x)
								{
									const int a = raw[(static_cast<size_t>(y) * w + x) * n + k] >> shift;
									const int b = raw[(static_cast<size_t>(y) * w + x - 1) * n + k] >> shift;
									edgeSteps += std::abs(a - b) == static_cast<int>(extCodes) ? 1 : 0;
								}
					pl_tex guard = deband.GuardTextureForTest(0, planeIndex);
					Assert::IsNotNull(guard, L"the guard texture exists (grainGuard on, grainSplit off)");
					const int gw = (w + 15) / 16, gh = (h + 15) / 16;
					Assert::AreEqual(gw, guard->params.w);
					Assert::AreEqual(gh, guard->params.h);
					Assert::IsTrue(guard->params.blit_src, L"the guard texture is blittable (its format lacks PL_FMT_CAP_BLITTABLE on this device)");
					const pl_fmt fmt = guard->params.format;
					pl_tex_params rp{};
					rp.w = gw; rp.h = gh; rp.format = fmt; rp.host_readable = true; rp.blit_dst = true;
					pl_tex readable = pl_tex_create(gpu, &rp);
					Assert::IsNotNull(readable, L"readback texture");
					pl_tex_blit_params bp{}; bp.src = guard; bp.dst = readable;
					pl_tex_blit(gpu, &bp);
					const size_t texel = fmt->texel_size;
					std::vector<uint8_t> bytes(static_cast<size_t>(gw) * gh * texel);
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = bytes.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					pl_tex_destroy(gpu, &readable);
					auto share = [&](size_t i) -> float {
						const uint8_t* q = bytes.data() + i * texel;
						if (fmt->type == PL_FMT_FLOAT && fmt->component_depth[0] == 32) { float v; std::memcpy(&v, q, 4); return v; }
						uint16_t h16; std::memcpy(&h16, q, 2);
						if (fmt->type == PL_FMT_UNORM) return h16 / 65535.0f;
						const int e = (h16 >> 10) & 31, m = h16 & 1023;   // IEEE half (a 16-bit float fallback format)
						const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m + 1024), e - 25);
						return (h16 & 0x8000) ? -v : v;
					};
					double worst = 0.0;
					bool anyBusy = false, anyClear = false;
					for (int by = 0; by < gh; ++by)
						for (int bx = 0; bx < gw; ++bx)
						{
							const size_t i = static_cast<size_t>(by) * gw + bx;
							const int cellPx = ((std::min)(w, (bx + 1) * 16) - bx * 16) * ((std::min)(h, (by + 1) * 16) - by * 16);
							const double delta = std::fabs(static_cast<double>(share(i)) - want[i]) * cellPx;   // in pixels
							worst = (std::max)(worst, delta);
							flips += static_cast<int>(std::lround(delta));
							anyBusy = anyBusy || want[i] > 0.0f;
							anyClear = anyClear || want[i] < 1.0f;
						}
					char text[220];
					snprintf(text, sizeof(text), "%d-bit threshold %.0f plane %d (%dx%d, %d comp): worst %.3f px of a cell, flips %d, edge steps so far %d",
						cs.depth, cs.threshold, planeIndex, w, h, n, worst, flips, edgeSteps);
					Logger::WriteMessage(text);
					const std::wstring wtext(text, text + strlen(text));
					Assert::IsTrue(anyBusy && anyClear, (L"the picture exercises both decisions; " + wtext).c_str());
					Assert::IsTrue(worst < 0.25, (L"guard = BusyShare oracle decision for decision (else register a FIX with this flip count); " + wtext).c_str());
				}
				if (cs.depth == 12)
					Assert::IsTrue(edgeSteps > 0, L"the 12-bit cases put neighbour steps exactly on the extremum level");
				deband.Release(gpu);
				pl_tex_destroy(gpu, &lt);
				pl_tex_destroy(gpu, &ct);
			}
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(LibplaceboDebandAddsNoGrainWhenOff)
		{
			// deband_grain off: libplacebo's deband (default strength) on a flat picture adds no
			// noise - the output is flat; on adds its grain (about 0.29 8-bit code RMS); on is
			// libplacebo's own default parameters byte for byte (a fresh renderer each).
			auto render = [](const pl_deband_params* deband) {
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				pl_gpu gpu = fixture.Gpu();
				const int w = 64, h = 64;
				std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4, 0);
				for (size_t i = 0; i < px.size(); i += 4) { px[i] = px[i + 1] = px[i + 2] = 30000; px[i + 3] = 65535; }
				pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
				pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params sp{}; sp.w = w; sp.h = h; sp.format = srcFmt; sp.sampleable = true; sp.initial_data = px.data();
				pl_tex src = pl_tex_create(gpu, &sp);
				pl_tex_params dp{}; dp.w = w; dp.h = h; dp.format = dstFmt; dp.renderable = true; dp.host_readable = true;
				pl_tex dst = pl_tex_create(gpu, &dp);
				Assert::IsTrue(src && dst);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame target = TargetLutGpuFixture::MakeRgbFrame(dst);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.deband_params = deband;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &target, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> out(static_cast<size_t>(w) * h * 4);
				pl_tex_transfer_params d{}; d.tex = dst; d.ptr = out.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &src);
				pl_tex_destroy(gpu, &dst);
				return out;
			};
			auto rms = [](const std::vector<uint16_t>& o) {
				double mean = 0.0, sq = 0.0; size_t n = 0;
				for (size_t i = 0; i < o.size(); i += 4) { mean += o[i]; ++n; }
				mean /= n;
				for (size_t i = 0; i < o.size(); i += 4) sq += (o[i] - mean) * (o[i] - mean);
				return std::sqrt(sq / n) / 257.0;   // 8-bit codes
			};
			LibplaceboRenderParameters::Settings on, off;
			on.debandStrength = off.debandStrength = "default";
			on.debandGrain = true;
			off.debandGrain = false;
			LibplaceboRenderParameters::Projection pOn, pOff;
			std::string error;
			Assert::IsTrue(LibplaceboRenderParameters::Build(on, false, pOn, error));
			Assert::IsTrue(LibplaceboRenderParameters::Build(off, false, pOff, error));
			const std::vector<uint16_t> withGrain = render(pOn.renderParams.deband_params);
			const std::vector<uint16_t> without = render(pOff.renderParams.deband_params);
			const std::vector<uint16_t> native = render(&pl_deband_default_params);
			char text[120];
			snprintf(text, sizeof(text), "flat picture through libplacebo's deband: grain on %.3f, off %.3f 8-bit codes RMS", rms(withGrain), rms(without));
			Logger::WriteMessage(text);
			Assert::AreEqual(0.0, rms(without), 1e-9, L"off: no added noise");
			Assert::IsTrue(rms(withGrain) > 0.1, L"on: libplacebo's grain");
			Assert::IsTrue(withGrain == native, L"on: libplacebo's own default parameters byte for byte");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(BandMemoOneComputePerKey)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BandMemoOneComputePerKey)
		{
			namespace GB = Vpv::GrainBand;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			struct DispatchGuard
			{
				pl_dispatch dispatch = nullptr;
				explicit DispatchGuard(pl_dispatch d) : dispatch(d) {}
				~DispatchGuard() { if (dispatch) pl_dispatch_destroy(&dispatch); }
				DispatchGuard(const DispatchGuard&) = delete;
				DispatchGuard& operator=(const DispatchGuard&) = delete;
			};
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct DebandGuard
			{
				pl_gpu gpu = nullptr;
				VpDebander::Module mod;
				explicit DebandGuard(pl_gpu g) : gpu(g) {}
				~DebandGuard() { mod.Release(gpu); }
				DebandGuard(const DebandGuard&) = delete;
				DebandGuard& operator=(const DebandGuard&) = delete;
			};
			struct MemoGpuGuard
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				explicit MemoGpuGuard(pl_gpu g) : ownerDev(g) {}
				~MemoGpuGuard()
				{
					memo.ReleaseGpuSpots();
					ownerDev.Release();
				}
				MemoGpuGuard(const MemoGpuGuard&) = delete;
				MemoGpuGuard& operator=(const MemoGpuGuard&) = delete;
			};

			auto hashU16 = [](const std::vector<uint16_t>& v) -> uint64_t {
				uint64_t h = 14695981039346656037ull;
				const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
				const size_t n = v.size() * sizeof(uint16_t);
				for (size_t i = 0; i < n; ++i)
				{
					h ^= p[i];
					h *= 1099511628211ull;
				}
				return h;
			};

			DispatchGuard dispGuard(pl_dispatch_create(fixture.Log(), gpu));
			pl_dispatch dispatch = dispGuard.dispatch;
			Assert::IsNotNull(dispatch, L"pl_dispatch_create succeeded");

			const int W = 128, H = 96;
			const int HW = (W + 1) / 2, HH = (H + 1) / 2;

			// Synthetic structured + banded staircase + deterministic grain (10-bit codes in P010 r16 UNORM).
			std::vector<uint16_t> srcData(static_cast<size_t>(W) * H);
			uint32_t rng = 0x51a7c0deu;
			for (int y = 0; y < H; ++y)
			{
				for (int x = 0; x < W; ++x)
				{
					rng = rng * 1664525u + 1013904223u;
					const int grain = (y >= H / 2) ? (static_cast<int>((rng >> 20) % 5u) - 2) : 0;
					const int band = 4 * (96 + x / 16);
					const int edge = (y >= 64 && y < 80 && x >= 40 && x < 88) ? 32 : 0;
					const int code10 = (std::min)(1023, (std::max)(0, band + edge + grain));
					srcData[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(code10 << 6);
				}
			}

			// Exact 2x2 float box mean matching HalfLuma / SPLIT_HALF_BODY in normalised 0..1 units.
			std::vector<float> halfData(static_cast<size_t>(HW) * HH, 0.0f);
			for (int qy = 0; qy < HH; ++qy)
			{
				for (int qx = 0; qx < HW; ++qx)
				{
					const int x0 = (std::min)(2 * qx, W - 1), x1 = (std::min)(2 * qx + 1, W - 1);
					const int y0 = (std::min)(2 * qy, H - 1), y1 = (std::min)(2 * qy + 1, H - 1);
					const float a = static_cast<float>(srcData[static_cast<size_t>(y0) * W + x0]) / 65535.0f;
					const float b = static_cast<float>(srcData[static_cast<size_t>(y0) * W + x1]) / 65535.0f;
					const float c = static_cast<float>(srcData[static_cast<size_t>(y1) * W + x0]) / 65535.0f;
					const float d = static_cast<float>(srcData[static_cast<size_t>(y1) * W + x1]) / 65535.0f;
					halfData[static_cast<size_t>(qy) * HW + qx] = 0.25f * (a + b + c + d);
				}
			}
			const std::vector<float> zeroHalfData(static_cast<size_t>(HW) * HH, 0.0f);

			pl_tex_params sp{};
			sp.w = W; sp.h = H; sp.format = pl_find_named_fmt(gpu, "r16"); sp.sampleable = true; sp.initial_data = srcData.data();
			TexGuard srcGuard(gpu, pl_tex_create(gpu, &sp));
			Assert::IsNotNull(srcGuard.tex, L"source r16 texture created");

			pl_tex_params hp{};
			hp.w = HW; hp.h = HH; hp.format = pl_find_named_fmt(gpu, "r32f"); hp.sampleable = true; hp.initial_data = halfData.data();
			TexGuard halfGuard(gpu, pl_tex_create(gpu, &hp));
			Assert::IsNotNull(halfGuard.tex, L"half-res r32f texture created");

			pl_tex_params zp = hp;
			zp.initial_data = zeroHalfData.data();
			TexGuard zeroGuard(gpu, pl_tex_create(gpu, &zp));
			Assert::IsNotNull(zeroGuard.tex, L"zero half-res r32f texture created");

			auto frameOf = [](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex;
				f.planes[0].components = 1;
				f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709;
				f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16;
				f.repr.bits.color_depth = 10;
				f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};

			auto downloadR16 = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				TexGuard readable(gpu, pl_tex_create(gpu, &rp));
				Assert::IsNotNull(readable.tex, L"readable blit target created");
				pl_tex_blit_params bp{};
				bp.src = tex;
				bp.dst = readable.tex;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H);
				pl_tex_transfer_params dl{};
				dl.tex = readable.tex;
				dl.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"pl_tex_download succeeded");
				return raw;
			};

			// Wire real SkeletonMemo + OwnerGpuDevice + kP1HalfLuma view + kBandMemo GPU owner fill with Counters.
			MemoGpuGuard gpuMemo(gpu);
			int hostTag = 0;
			gpuMemo.memo.SetHost(&hostTag);
			gpuMemo.memo.SetRenderThread(std::this_thread::get_id());
			gpuMemo.memo.SetGpuDispatcherPresent(true);
			gpuMemo.memo.SetGpuBudget(true, uint64_t(1) << 28);
			gpuMemo.memo.SetGpuDevice(&gpuMemo.ownerDev);

			const uint64_t kSeq = 101;
			pl_tex halfTexPtr = halfGuard.tex;
			VpSkeleton::OwnerFill p1Fill;
			p1Fill.view = true;
			p1Fill.frameScoped = true;
			p1Fill.fill = [halfTexPtr, HW, HH, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq)
					return false;
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(halfTexPtr));
				out.gpu.width = static_cast<uint32_t>(HW);
				out.gpu.height = static_cast<uint32_t>(HH);
				out.gpu.bytes = 0;
				return true;
			};
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(p1Fill)) == Reg::Registered,
				L"kP1HalfLuma GPU view fill registered");

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			const uint64_t reserve = GB::BandMemoReserveBytes(VpSkeleton::Dims{ static_cast<uint32_t>(W), static_cast<uint32_t>(H) });
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu,
				GB::kBandMemoUnit, GB::MakeBandMemoOwnerFill(reserve, counters)) == Reg::Registered,
				L"kBandMemo GPU fill registered");

			VpDebander::Params params;
			params.thresholdCodes = 6.0;
			params.colorDepth = 10;
			params.radiusScale = 1.0;
			params.grainGuard = true;
			params.grainSplit = true;
			params.sequence = kSeq;
			params.memo = &gpuMemo.memo;

			// 1. Two independent VpDebander readers acquiring the exact same (seq, sigma=6.0) key compute BandMemo ONCE.
			DebandGuard debandA(gpu), debandB(gpu);
			pl_frame outA{}, outB{};
			Assert::IsTrue(debandA.mod.Apply(gpu, dispatch, frameOf(srcGuard.tex), params, outA),
				ExtendedRangeGpuProbe::ToWide(debandA.mod.LastError()).c_str());
			Assert::IsTrue(debandB.mod.Apply(gpu, dispatch, frameOf(srcGuard.tex), params, outB),
				ExtendedRangeGpuProbe::ToWide(debandB.mod.LastError()).c_str());
			Assert::AreEqual(uint64_t(1), counters->fills.load(), L"kBandMemo computed ONCE for two readers of the same (seq, BandKey)");
			Assert::AreEqual(1ull, debandA.mod.BandMemoHits(), L"debandA hit kBandMemo");
			Assert::AreEqual(1ull, debandB.mod.BandMemoHits(), L"debandB hit kBandMemo");
			Assert::AreEqual(0ull, debandA.mod.BandMemoFallbacks(), L"debandA had zero fallbacks");
			Assert::AreEqual(0ull, debandB.mod.BandMemoFallbacks(), L"debandB had zero fallbacks");
			Assert::IsTrue(gpuMemo.memo.Counters(VpSkeleton::kBandMemo).reusedMemo >= 1u, L"memo counter recorded reuse");

			const std::vector<uint16_t> rawMemoA = downloadR16(outA.planes[0].texture);
			const std::vector<uint16_t> rawMemoB = downloadR16(outB.planes[0].texture);
			const uint64_t hashMemoA = hashU16(rawMemoA);
			Assert::AreEqual(hashMemoA, hashU16(rawMemoB), L"second reader capture hash matches first reader");
			Assert::IsTrue(rawMemoA == rawMemoB, L"second reader output pixels match first reader bit-for-bit");

			// Confirm debander actually modified the banded staircase (not a vacuous no-op).
			int rawDiffCount = 0;
			for (size_t i = 0; i < rawMemoA.size(); ++i)
				if (rawMemoA[i] != srcData[i]) ++rawDiffCount;
			Assert::IsTrue(rawDiffCount > 100, L"debander actively smoothed the banded staircase");

			// 2. Capture-hash identity vs memo=nullptr fallback + Rule 4 visible fallback accounting.
			VpDebander::Params fallbackParams = params;
			fallbackParams.memo = nullptr;
			DebandGuard debandFallback(gpu);
			pl_frame outFallback{};
			Assert::IsTrue(debandFallback.mod.Apply(gpu, dispatch, frameOf(srcGuard.tex), fallbackParams, outFallback),
				ExtendedRangeGpuProbe::ToWide(debandFallback.mod.LastError()).c_str());
			Assert::AreEqual(0ull, debandFallback.mod.BandMemoHits(), L"memo=nullptr has zero BandMemo hits");
			Assert::AreEqual(1ull, debandFallback.mod.BandMemoFallbacks(), L"memo=nullptr increments BandMemoFallbacks");
			Assert::IsFalse(debandFallback.mod.LastBandMemoFallback().empty(), L"memo=nullptr records visible fallback reason");
			const std::vector<uint16_t> rawFallback = downloadR16(outFallback.planes[0].texture);
			Assert::AreEqual(hashU16(rawFallback), hashMemoA, L"GPU capture-hash identity: kBandMemo hit == memo=nullptr fallback");
			Assert::IsTrue(rawMemoA == rawFallback, L"kBandMemo hit pixels match memo=nullptr fallback bit-for-bit");

			// 3. Negative control (a): acquiring a DIFFERENT BandKey (sigmaPx=3.0 via FrameInterpolator vs 6.0 via VpDebander)
			//    on the exact same seq MUST trigger a second fill (fills == 2), and a second interpolator reader reuses it (fills stays 2).
			Dejudder::Planes lumaOnly{};
			lumaOnly.luma = srcGuard.tex;
			Dejudder::Planes interpOutA{}, interpOutB{};
			Interpolator::FrameInterpolator interpA(fixture.Log(), gpu, true);
			Interpolator::FrameInterpolator interpB(fixture.Log(), gpu, true);
			interpA.SetGrain(InterpolationPolicy::Grain::Preserve);
			interpB.SetGrain(InterpolationPolicy::Grain::Preserve);
			Assert::IsTrue(interpA.Run(lumaOnly, lumaOnly, Interpolator::Motion(), 0.5f,
				InterpolationPolicy::Fallback::Blend, interpOutA, 0, 0, &gpuMemo.memo, kSeq, kSeq),
				ExtendedRangeGpuProbe::ToWide(interpA.LastError()).c_str());
			Assert::AreEqual(uint64_t(2), counters->fills.load(),
				L"negative control: different sigmaPx (3.0 vs 6.0) on same seq triggers a second BandMemo fill");
			Assert::AreEqual(1ull, interpA.BandMemoHits(), L"interpA hit kBandMemo for sigmaPx=3.0");
			Assert::AreEqual(0ull, interpA.BandMemoFallbacks(), L"interpA had zero fallbacks");
			Assert::IsTrue(interpB.Run(lumaOnly, lumaOnly, Interpolator::Motion(), 0.5f,
				InterpolationPolicy::Fallback::Blend, interpOutB, 0, 0, &gpuMemo.memo, kSeq, kSeq),
				ExtendedRangeGpuProbe::ToWide(interpB.LastError()).c_str());
			Assert::AreEqual(uint64_t(2), counters->fills.load(),
				L"second interpolator reader of same (seq, sigmaPx=3.0) reuses memo without a third fill");
			Assert::AreEqual(1ull, interpB.BandMemoHits(), L"interpB hit kBandMemo");

			// 4. Negative control (b): unregistered kBandMemo visibly falls back and preserves output parity.
			VpSkeleton::SkeletonMemo unregMemo;
			VpDebander::Params unregParams = params;
			unregParams.memo = &unregMemo;
			DebandGuard debandUnreg(gpu);
			pl_frame outUnreg{};
			Assert::IsTrue(debandUnreg.mod.Apply(gpu, dispatch, frameOf(srcGuard.tex), unregParams, outUnreg),
				ExtendedRangeGpuProbe::ToWide(debandUnreg.mod.LastError()).c_str());
			Assert::AreEqual(0ull, debandUnreg.mod.BandMemoHits(), L"unregistered memo has zero hits");
			Assert::AreEqual(1ull, debandUnreg.mod.BandMemoFallbacks(), L"unregistered memo increments fallbacks");
			Assert::IsFalse(debandUnreg.mod.LastBandMemoFallback().empty(), L"unregistered memo records visible fallback reason");
			Assert::AreEqual(hashMemoA, hashU16(downloadR16(outUnreg.planes[0].texture)),
				L"unregistered fallback preserves exact capture hash");

			// 5. Negative control (c): a tampered all-zero kBandMemo low band causes output to diverge substantially
			//    from the real kBandMemo output (proving VpDebander genuinely consumes the low band, not bypassing it).
			MemoGpuGuard zeroMemo(gpu);
			zeroMemo.memo.SetHost(&hostTag);
			zeroMemo.memo.SetRenderThread(std::this_thread::get_id());
			zeroMemo.memo.SetGpuDispatcherPresent(true);
			zeroMemo.memo.SetGpuBudget(true, uint64_t(1) << 28);
			zeroMemo.memo.SetGpuDevice(&zeroMemo.ownerDev);
			pl_tex zeroTexPtr = zeroGuard.tex;
			VpSkeleton::OwnerFill zeroFill;
			using VK = VpSkeleton::VariantKey;
			zeroFill.variantAxes = uint32_t(VK::kAxisPlane) | uint32_t(VK::kAxisSize) | uint32_t(VK::kAxisScale)
				| uint32_t(VK::kAxisSigma) | uint32_t(VK::kAxisStage) | uint32_t(VK::kAxisParam0) | uint32_t(VK::kAxisParam1);
			zeroFill.view = true;
			zeroFill.frameScoped = true;
			zeroFill.fill = [zeroTexPtr, HW, HH](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(zeroTexPtr));
				out.gpu.width = static_cast<uint32_t>(HW);
				out.gpu.height = static_cast<uint32_t>(HH);
				out.gpu.bytes = 0;
				return true;
			};
			Assert::IsTrue(zeroMemo.memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu,
				GB::kBandMemoUnit, std::move(zeroFill)) == Reg::Registered,
				L"tampered zero kBandMemo fill registered");

			VpDebander::Params zeroParams = params;
			zeroParams.memo = &zeroMemo.memo;
			DebandGuard debandZero(gpu);
			pl_frame outZero{};
			Assert::IsTrue(debandZero.mod.Apply(gpu, dispatch, frameOf(srcGuard.tex), zeroParams, outZero),
				ExtendedRangeGpuProbe::ToWide(debandZero.mod.LastError()).c_str());
			Assert::AreEqual(1ull, debandZero.mod.BandMemoHits(), L"debandZero consumed tampered kBandMemo");
			const std::vector<uint16_t> rawZero = downloadR16(outZero.planes[0].texture);
			Assert::AreNotEqual(hashMemoA, hashU16(rawZero), L"negative control: zeroed low band MUST alter capture hash");
			int zeroDiffPixels = 0;
			int maxZeroDiff = 0;
			for (size_t i = 0; i < rawZero.size(); ++i)
			{
				const int d = std::abs(static_cast<int>(rawZero[i]) - static_cast<int>(rawMemoA[i]));
				if (d > 0) ++zeroDiffPixels;
				if (d > maxZeroDiff) maxZeroDiff = d;
			}
			Assert::IsTrue(zeroDiffPixels > 100 && maxZeroDiff >= 64,
				L"negative control: zeroed low band diverges by >= 1 10-bit code across >100 pixels");
		}

		// W23-B3 M2#6 (15b A3): VpDebander grain guard acquires shared kP10GrainSplit (luma),
		// kP10cGrainSplitChroma (Cb/Cr), and kP11RegionBlockStats (re_s1 structure tensor); preserves
		// luma & chroma grain, smooths banding ramp, uses P11 directional contour tensor, preserves
		// unclamped BTB (< 0.0) and WTW (> 1.0) on float planes, and visibly reports fallbacks when
		// P10/P11 are absent.
		BEGIN_TEST_METHOD_ATTRIBUTE(VpDebanderGrainGuardReadsP10AndP11TensorAndPreservesBtbWtw)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(VpDebanderGrainGuardReadsP10AndP11TensorAndPreservesBtbWtw)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			namespace G = GrainSplit::Gpu;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			struct DispatchGuard
			{
				pl_dispatch dispatch = nullptr;
				explicit DispatchGuard(pl_dispatch d) : dispatch(d) {}
				~DispatchGuard() { if (dispatch) pl_dispatch_destroy(&dispatch); }
				DispatchGuard(const DispatchGuard&) = delete;
				DispatchGuard& operator=(const DispatchGuard&) = delete;
			};
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct DebandGuard
			{
				pl_gpu gpu = nullptr;
				VpDebander::Module mod;
				explicit DebandGuard(pl_gpu g) : gpu(g) {}
				~DebandGuard() { mod.Release(gpu); }
				DebandGuard(const DebandGuard&) = delete;
				DebandGuard& operator=(const DebandGuard&) = delete;
			};
			struct P10P11Rig
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				G::VariantOwner grainOwner;
				explicit P10P11Rig(pl_gpu g) : ownerDev(g)
				{
					memo.SetHost(this);
					memo.SetRenderThread(std::this_thread::get_id());
					memo.SetGpuDispatcherPresent(true);
					memo.SetGpuBudget(true, uint64_t(1) << 29);
					memo.SetGpuDevice(&ownerDev);
				}
				~P10P11Rig()
				{
					memo.ReleaseGpuSpots();
					Vpv::Capture::SourcePlanes::Forget(this);
					ownerDev.Release();
				}
				P10P11Rig(const P10P11Rig&) = delete;
				P10P11Rig& operator=(const P10P11Rig&) = delete;
			};

			DispatchGuard dispGuard(pl_dispatch_create(fixture.Log(), gpu));
			pl_dispatch dispatch = dispGuard.dispatch;
			Assert::IsNotNull(dispatch, L"pl_dispatch_create succeeded");

			const int W = 384, H = 128;
			const int CW = 192, CH = 64;
			const float kCodeUnit = 1.0f / 1023.0f;

			// Build independent synthetic 3-plane YCbCr float (r32f) picture with 4 distinct 96-px horizontal zones
			// (well beyond the 16-px luma / 8-px chroma deband radius so every interior ROI is cross-zone isolated):
			//   Zone 0 [0..96):     4-code banding ramp (every 16 px) -> must be smoothed by deband
			//   Zone 1 [96..192):   flat pedestal + sigma=3.0 codes grain -> must be preserved by P10/P10c grain guard
			//   Zone 2 [192..288):  sharp vertical contour step (+28 codes at x=240) -> protected by P11 re_s1 tensor
			//   Zone 3 [288..384):  BTB (-0.15f, top half) & WTW (+1.18f, bottom half) -> must stay unclamped
			std::vector<float> yData(static_cast<size_t>(W) * H, 0.0f);
			std::vector<float> cbData(static_cast<size_t>(CW) * CH, 0.0f);
			std::vector<float> crData(static_cast<size_t>(CW) * CH, 0.0f);

			std::mt19937 rngY(15031u), rngCb(15032u), rngCr(15033u);
			std::normal_distribution<float> grainDist(0.0f, 3.0f);

			for (int y = 0; y < H; ++y)
			{
				for (int x = 0; x < W; ++x)
				{
					float v = 0.0f;
					if (x < 96)
						v = (300.0f + 4.0f * static_cast<float>(x / 16)) * kCodeUnit;
					else if (x < 192)
						v = (500.0f + grainDist(rngY)) * kCodeUnit;
					else if (x < 288)
						v = (x < 240 ? 450.0f : 478.0f) * kCodeUnit;
					else
						v = (y < H / 2) ? -0.15f : 1.18f;
					yData[static_cast<size_t>(y) * W + x] = v;
				}
			}
			for (int y = 0; y < CH; ++y)
			{
				for (int x = 0; x < CW; ++x)
				{
					float cb = 0.0f, cr = 0.0f;
					if (x < 48)
					{
						cb = (480.0f + 4.0f * static_cast<float>(x / 8)) * kCodeUnit;
						cr = (520.0f + 4.0f * static_cast<float>(x / 8)) * kCodeUnit;
					}
					else if (x < 96)
					{
						cb = (512.0f + grainDist(rngCb)) * kCodeUnit;
						cr = (512.0f + grainDist(rngCr)) * kCodeUnit;
					}
					else if (x < 144)
					{
						cb = (x < 120 ? 480.0f : 508.0f) * kCodeUnit;
						cr = (x < 120 ? 500.0f : 528.0f) * kCodeUnit;
					}
					else
					{
						cb = (y < CH / 2) ? -0.12f : 1.15f;
						cr = (y < CH / 2) ? -0.11f : 1.14f;
					}
					cbData[static_cast<size_t>(y) * CW + x] = cb;
					crData[static_cast<size_t>(y) * CW + x] = cr;
				}
			}

			pl_fmt r32fFmt = pl_find_named_fmt(gpu, "r32f");
			pl_fmt rgba32fFmt = pl_find_named_fmt(gpu, "rgba32f");
			Assert::IsNotNull(r32fFmt, L"r32f format available");
			Assert::IsNotNull(rgba32fFmt, L"rgba32f format available");

			auto uploadR32f = [&](int w, int h, const std::vector<float>& data) {
				pl_tex_params p{};
				p.w = w; p.h = h; p.format = r32fFmt; p.sampleable = true; p.initial_data = data.data();
				pl_tex t = pl_tex_create(gpu, &p);
				Assert::IsNotNull(t, L"r32f upload texture created");
				return t;
			};
			auto downloadR32f = [&](pl_tex tex, int w, int h) {
				pl_tex_params rp{};
				rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				TexGuard readable(gpu, pl_tex_create(gpu, &rp));
				Assert::IsNotNull(readable.tex, L"readable float blit target created");
				pl_tex_blit_params bp{};
				bp.src = tex;
				bp.dst = readable.tex;
				pl_tex_blit(gpu, &bp);
				pl_gpu_finish(gpu);
				std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = readable.tex;
				dl.ptr = out.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"r32f download succeeded");
				return out;
			};

			TexGuard yTex(gpu, uploadR32f(W, H, yData));
			TexGuard cbTex(gpu, uploadR32f(CW, CH, cbData));
			TexGuard crTex(gpu, uploadR32f(CW, CH, crData));

			// Build two synthetic P11 re_s1 (jxx, jyy, jxy, lap_e) textures:
			//   (i)  strong vertical-contour tensor (jxx=1000, jyy=0, jxy=0, lap_e=100),
			//   (ii) zero tensor (0, 0, 0, 0) for same-input negative control proving P11 is genuinely consumed.
			const int p11Block = RegionEvidence::BlockSizeFor(H);
			const int p11W = (W + p11Block - 1) / p11Block;
			const int p11H = (H + p11Block - 1) / p11Block;
			std::vector<float> strongTensor(static_cast<size_t>(p11W) * p11H * 4u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(p11W) * p11H; ++i)
			{
				strongTensor[i * 4u + 0u] = 1000.0f;   // jxx: strong horizontal gradient -> vertical contour (along = +/-y)
				strongTensor[i * 4u + 1u] = 0.0f;      // jyy
				strongTensor[i * 4u + 2u] = 0.0f;      // jxy
				strongTensor[i * 4u + 3u] = 100.0f;    // lap_e
			}
			const std::vector<float> zeroTensor(static_cast<size_t>(p11W) * p11H * 4u, 0.0f);
			auto uploadTensor = [&](const std::vector<float>& tdata) {
				pl_tex_params tp{};
				tp.w = p11W; tp.h = p11H; tp.format = rgba32fFmt; tp.sampleable = true; tp.initial_data = tdata.data();
				pl_tex t = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(t, L"P11 re_s1 rgba32f texture created");
				return t;
			};
			TexGuard p11StrongTex(gpu, uploadTensor(strongTensor));
			TexGuard p11ZeroTex(gpu, uploadTensor(zeroTensor));

			pl_frame srcFrame{};
			srcFrame.num_planes = 3;
			srcFrame.planes[0].texture = yTex.tex;  srcFrame.planes[0].components = 1; srcFrame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			srcFrame.planes[1].texture = cbTex.tex; srcFrame.planes[1].components = 1; srcFrame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			srcFrame.planes[2].texture = crTex.tex; srcFrame.planes[2].components = 1; srcFrame.planes[2].component_mapping[0] = PL_CHANNEL_CR;
			srcFrame.repr.sys = PL_COLOR_SYSTEM_BT_709;
			srcFrame.repr.levels = PL_COLOR_LEVELS_LIMITED;
			srcFrame.repr.bits.sample_depth = 32;
			srcFrame.repr.bits.color_depth = 10;
			srcFrame.repr.bits.bit_shift = 0;
			srcFrame.color = pl_color_space_bt709;

			auto setupRig = [&](P10P11Rig& rig, pl_tex p11Tex, uint64_t seq) {
				Assert::IsTrue(rig.memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu,
					Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill()) == Reg::Registered,
					L"P13 registered");
				Assert::IsTrue(rig.grainOwner.Register(rig.memo, W, H) == Reg::Registered, L"P10 registered");
				Assert::IsTrue(rig.grainOwner.RegisterChroma(rig.memo, CW, CH) == Reg::Registered, L"P10c registered");
				VpSkeleton::OwnerFill p11Fill;
				p11Fill.view = true;
				p11Fill.frameScoped = true;
				p11Fill.fill = [p11Tex, p11W, p11H, seq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq != seq) return false;
					out.gpu.resource = const_cast<void*>(static_cast<const void*>(p11Tex));
					out.gpu.width = static_cast<uint32_t>(p11W);
					out.gpu.height = static_cast<uint32_t>(p11H);
					out.gpu.bytes = 0;
					return true;
				};
				Assert::IsTrue(rig.memo.RegisterOwnerFill(VpSkeleton::kP11RegionBlockStats, VpSkeleton::AcquireForm::Gpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP11RegionBlockStats), std::move(p11Fill)) == Reg::Registered,
					L"P11 registered");
				Vpv::Capture::SourcePlaneSet set;
				set.seq = seq;
				set.planes = 3;
				set.tex[0] = yTex.tex; set.components[0] = 1;
				set.tex[1] = cbTex.tex; set.components[1] = 1;
				set.tex[2] = crTex.tex; set.components[2] = 1;
				set.rgb = false;
				set.repr = srcFrame.repr;
				set.origin = "vpdebander p10/p10c/p11 test";
				Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(&rig, set), L"3-plane float P13 offered");
			};

			auto zoneRms = [](const std::vector<float>& img, int w, int x0, int x1, int y0, int y1, float pedestal) -> double {
				double sum = 0.0;
				int n = 0;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x)
					{
						const double d = static_cast<double>(img[static_cast<size_t>(y) * w + x] - pedestal);
						sum += d * d;
						++n;
					}
				return std::sqrt(sum / static_cast<double>(n));
			};
			auto rampStaircaseRms = [&](const std::vector<float>& img) -> double {
				std::vector<double> col;
				for (int x = 20; x < 76; ++x)
				{
					double m = 0.0;
					for (int y = 20; y < H - 20; ++y)
						m += static_cast<double>(img[static_cast<size_t>(y) * W + x]) / kCodeUnit;
					col.push_back(m / static_cast<double>(H - 40));
				}
				double sx = 0, sy = 0, sxx = 0, sxy = 0;
				const double k = static_cast<double>(col.size());
				for (size_t i = 0; i < col.size(); ++i) { sx += i; sy += col[i]; sxx += double(i) * i; sxy += i * col[i]; }
				const double b = (k * sxy - sx * sy) / (k * sxx - sx * sx), a = (sy - b * sx) / k;
				double e = 0.0;
				for (size_t i = 0; i < col.size(); ++i) e += (col[i] - a - b * i) * (col[i] - a - b * i);
				return std::sqrt(e / k);
			};

			VpDebander::Params params;
			params.thresholdCodes = 12.0;
			params.colorDepth = 10;
			params.radiusScale = 1.0;   // 16-px luma / 8-px chroma radius; splitScale == 1.0 for all planes
			params.grainGuard = true;
			params.grainSplit = true;
			params.sequence = 201u;

			// 1. Protected path: P10 (luma) + P10c (Cb/Cr) + strong P11 tensor.
			P10P11Rig strongRig(gpu);
			setupRig(strongRig, p11StrongTex.tex, params.sequence);
			params.memo = &strongRig.memo;

			DebandGuard debandStrong(gpu);
			pl_frame outStrong{};
			Assert::IsTrue(debandStrong.mod.Apply(gpu, dispatch, srcFrame, params, outStrong),
				ExtendedRangeGpuProbe::ToWide(debandStrong.mod.LastError()).c_str());
			Assert::AreEqual(3ull, debandStrong.mod.GrainGuardHits(), L"all 3 planes (Y, Cb, Cr) hit shared P10/P10c + P11 grain guard");
			Assert::AreEqual(0ull, debandStrong.mod.GrainGuardFallbacks(), L"zero grain-guard fallbacks when P10/P10c/P11 registered");
			Assert::AreEqual(uint64_t(1), strongRig.grainOwner.Computes(), L"P10 luma computed once");
			Assert::AreEqual(uint64_t(2), strongRig.grainOwner.ChromaComputes(), L"P10c chroma computed once per plane (Cb, Cr)");

			const std::vector<float> yStrong = downloadR32f(outStrong.planes[0].texture, W, H);
			const std::vector<float> cbStrong = downloadR32f(outStrong.planes[1].texture, CW, CH);
			const std::vector<float> crStrong = downloadR32f(outStrong.planes[2].texture, CW, CH);

			// 1a. Banding ramp in Zone 0 is smoothed.
			const double rampInRms = rampStaircaseRms(yData);
			const double rampOutRms = rampStaircaseRms(yStrong);
			Assert::IsTrue(rampInRms > 0.8 && rampOutRms < 0.55 * rampInRms,
				L"Zone 0 banding staircase is smoothed by >45% under P10+P11 grain guard");

			// 1b. Grain in Zone 1 is preserved on BOTH luma (P10) and chroma (P10c).
			const double yGrainIn = zoneRms(yData, W, 116, 172, 20, 108, 500.0f * kCodeUnit);
			const double yGrainStrong = zoneRms(yStrong, W, 116, 172, 20, 108, 500.0f * kCodeUnit);
			const double cbGrainIn = zoneRms(cbData, CW, 58, 86, 12, 52, 512.0f * kCodeUnit);
			const double cbGrainStrong = zoneRms(cbStrong, CW, 58, 86, 12, 52, 512.0f * kCodeUnit);
			const double crGrainIn = zoneRms(crData, CW, 58, 86, 12, 52, 512.0f * kCodeUnit);
			const double crGrainStrong = zoneRms(crStrong, CW, 58, 86, 12, 52, 512.0f * kCodeUnit);
			Assert::IsTrue(yGrainStrong > 0.90 * yGrainIn, L"P10 grain guard preserves >90% of luma grain RMS");
			Assert::IsTrue(cbGrainStrong > 0.88 * cbGrainIn, L"P10c grain guard preserves >88% of Cb grain RMS");
			Assert::IsTrue(crGrainStrong > 0.88 * crGrainIn, L"P10c grain guard preserves >88% of Cr grain RMS");

			// 1c. Extended-range BTB (< 0.0) and WTW (> 1.0) in Zone 3 stay strictly unclamped on luma and chroma!
			float maxBtbY = -1e9f, minWtwY = 1e9f;
			for (int y = 12; y < 44; ++y)
				for (int x = 308; x < 364; ++x)
					maxBtbY = (std::max)(maxBtbY, yStrong[static_cast<size_t>(y) * W + x]);
			for (int y = 84; y < 116; ++y)
				for (int x = 308; x < 364; ++x)
					minWtwY = (std::min)(minWtwY, yStrong[static_cast<size_t>(y) * W + x]);
			Assert::IsTrue(maxBtbY < -0.10f, L"luma BTB (-0.15) stays strictly below 0.0 (unclamped)");
			Assert::IsTrue(minWtwY > 1.12f, L"luma WTW (+1.18) stays strictly above 1.0 (unclamped)");

			float maxBtbCb = -1e9f, minWtwCb = 1e9f;
			for (int y = 6; y < 22; ++y)
				for (int x = 154; x < 182; ++x)
					maxBtbCb = (std::max)(maxBtbCb, cbStrong[static_cast<size_t>(y) * CW + x]);
			for (int y = 42; y < 58; ++y)
				for (int x = 154; x < 182; ++x)
					minWtwCb = (std::min)(minWtwCb, cbStrong[static_cast<size_t>(y) * CW + x]);
			Assert::IsTrue(maxBtbCb < -0.08f, L"chroma Cb BTB (-0.12) stays strictly below 0.0 (unclamped)");
			Assert::IsTrue(minWtwCb > 1.10f, L"chroma Cb WTW (+1.15) stays strictly above 1.0 (unclamped)");

			// 2. Negative control (a1): zero-tensor P11 vs strong-tensor P11 on the EXACT SAME input & P10/P10c split
			//    proves P11 re_s1 tensor genuinely drives directional contour protection in Zone 2 (x = 238..241).
			P10P11Rig zeroP11Rig(gpu);
			VpDebander::Params zeroP11Params = params;
			zeroP11Params.sequence = 202u;
			setupRig(zeroP11Rig, p11ZeroTex.tex, zeroP11Params.sequence);
			zeroP11Params.memo = &zeroP11Rig.memo;

			DebandGuard debandZeroP11(gpu);
			pl_frame outZeroP11{};
			Assert::IsTrue(debandZeroP11.mod.Apply(gpu, dispatch, srcFrame, zeroP11Params, outZeroP11),
				ExtendedRangeGpuProbe::ToWide(debandZeroP11.mod.LastError()).c_str());
			Assert::AreEqual(3ull, debandZeroP11.mod.GrainGuardHits(), L"zero-tensor P11 still hits P10+P11 path");
			Assert::AreEqual(0ull, debandZeroP11.mod.GrainGuardFallbacks(), L"zero-tensor P11 has zero fallbacks");

			const std::vector<float> yZeroP11 = downloadR32f(outZeroP11.planes[0].texture, W, H);
			double strongEdgeErr = 0.0, zeroEdgeErr = 0.0, maxP11Delta = 0.0;
			int edgeCount = 0;
			for (int y = 24; y < H - 24; ++y)
			{
				for (int x = 238; x <= 241; ++x)
				{
					const size_t idx = static_cast<size_t>(y) * W + x;
					const double orig = static_cast<double>(yData[idx]) / kCodeUnit;
					const double sVal = static_cast<double>(yStrong[idx]) / kCodeUnit;
					const double zVal = static_cast<double>(yZeroP11[idx]) / kCodeUnit;
					strongEdgeErr += std::fabs(sVal - orig);
					zeroEdgeErr += std::fabs(zVal - orig);
					maxP11Delta = (std::max)(maxP11Delta, std::fabs(sVal - zVal));
					++edgeCount;
				}
			}
			strongEdgeErr /= static_cast<double>(edgeCount);
			zeroEdgeErr /= static_cast<double>(edgeCount);
			Assert::IsTrue(maxP11Delta > 0.5,
				L"negative control: changing P11 re_s1 tensor from strong contour to zero measurably shifts contour pixels (>0.5 code)");
			Assert::IsTrue(strongEdgeErr < zeroEdgeErr,
				L"strong P11 directional tensor preserves the vertical contour strictly tighter than zero P11 tensor");

			// 3. Negative control (a2): absent products (memo=nullptr and unregistered memo) increment GrainGuardFallbacks()
			//    and populate non-empty LastGrainGuardFallback() (Rule 4 visible fallback).
			{
				VpDebander::Params nullParams = params;
				nullParams.memo = nullptr;
				DebandGuard debandNull(gpu);
				pl_frame outNull{};
				Assert::IsTrue(debandNull.mod.Apply(gpu, dispatch, srcFrame, nullParams, outNull),
					ExtendedRangeGpuProbe::ToWide(debandNull.mod.LastError()).c_str());
				Assert::AreEqual(0ull, debandNull.mod.GrainGuardHits(), L"memo=nullptr has 0 grain-guard hits");
				Assert::AreEqual(3ull, debandNull.mod.GrainGuardFallbacks(), L"memo=nullptr records 3 grain-guard fallbacks (Y, Cb, Cr)");
				Assert::IsFalse(debandNull.mod.LastGrainGuardFallback().empty(), L"memo=nullptr records visible fallback reason");

				VpSkeleton::SkeletonMemo emptyMemo;
				VpDebander::Params emptyParams = params;
				emptyParams.memo = &emptyMemo;
				DebandGuard debandEmpty(gpu);
				pl_frame outEmpty{};
				Assert::IsTrue(debandEmpty.mod.Apply(gpu, dispatch, srcFrame, emptyParams, outEmpty),
					ExtendedRangeGpuProbe::ToWide(debandEmpty.mod.LastError()).c_str());
				Assert::AreEqual(0ull, debandEmpty.mod.GrainGuardHits(), L"unregistered P10/P11 memo has 0 hits");
				Assert::AreEqual(3ull, debandEmpty.mod.GrainGuardFallbacks(), L"unregistered P10/P11 memo records 3 fallbacks");
				Assert::IsFalse(debandEmpty.mod.LastGrainGuardFallback().empty(), L"unregistered memo records visible fallback reason");
			}

			// 4. Negative control (b): unguarded deband (grainGuard=false) on the EXACT SAME grainy input
			//    loses significantly more luma & chroma grain RMS than P10/P10c+P11 grain-guarded deband.
			{
				VpDebander::Params unguardedParams = params;
				unguardedParams.grainGuard = false;
				unguardedParams.memo = nullptr;
				DebandGuard debandUnguarded(gpu);
				pl_frame outUnguarded{};
				Assert::IsTrue(debandUnguarded.mod.Apply(gpu, dispatch, srcFrame, unguardedParams, outUnguarded),
					ExtendedRangeGpuProbe::ToWide(debandUnguarded.mod.LastError()).c_str());
				const std::vector<float> yUnguarded = downloadR32f(outUnguarded.planes[0].texture, W, H);
				const std::vector<float> cbUnguarded = downloadR32f(outUnguarded.planes[1].texture, CW, CH);
				const double yGrainUnguarded = zoneRms(yUnguarded, W, 116, 172, 20, 108, 500.0f * kCodeUnit);
				const double cbGrainUnguarded = zoneRms(cbUnguarded, CW, 58, 86, 12, 52, 512.0f * kCodeUnit);
				Assert::IsTrue(yGrainUnguarded < 0.75 * yGrainIn && yGrainStrong > yGrainUnguarded * 1.25,
					L"negative control: unguarded deband crushes luma grain (<75%) while P10 grain guard preserves it");
				Assert::IsTrue(cbGrainUnguarded < 0.78 * cbGrainIn && cbGrainStrong > cbGrainUnguarded * 1.15,
					L"negative control: unguarded deband crushes chroma grain (<78%) while P10c grain guard preserves it");
			}

			strongRig.memo.OnSeal(params.sequence);
			zeroP11Rig.memo.OnSeal(zeroP11Params.sequence);
		}
	};
}
