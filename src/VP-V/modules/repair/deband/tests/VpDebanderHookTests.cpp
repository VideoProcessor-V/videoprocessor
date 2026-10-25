// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

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
	};
}
