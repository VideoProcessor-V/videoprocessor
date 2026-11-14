// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// X5AbCaptureTests: the before/after capture of FIX-X5-CUT-P4 (owner: the A/B must exist before the fix lands).
// Included inside namespace VideoProcessorTest by AdaptiveScalingOneAxisMotionTests.cpp. The SAME text is committed on the base
// (integ 2e77256c7, private mean-step cut) and on the tip (P4 + U7 cut event); only `VP_X5AB_SIDE` differs at run time.
//
// Runs only when VP_X5AB_LIST names a list file; otherwise it passes without doing anything (no GPU needed for the CPU run).
// One list line per case:  case|raw rgb24 file|w|h|out w|out h|transfer gamma22/pq|light gamma/linear|frames
// Output (all under VP_X5AB_OUT): <side>.csv (one row per frame) and <case>-<side>.y10 (sampled 10-bit luma of the output, every
// 2nd pixel in x and y, uint16 little endian, frame after frame). Frames are 8-bit RGB from ffmpeg (tools/x5ab-extract.ps1).
// Cut column: tip = the P4/U7 verdict the module used; base = the old rule re-derived from Stats.meanLuma (|step| > 20 codes).
// gpu_ms is wall time of one render plus pl_gpu_finish (includes the upload); VRAM is not measured by this harness.
// Hardware GPU only: set VP_TEST_GPU=hardware.

	namespace X5AbSupport
	{
		struct CaseLine
		{
			std::string name, raw, transfer, light;
			int w = 0, h = 0, ow = 0, oh = 0, frames = 0;
		};

		inline std::vector<CaseLine> ParseList(const std::string& path)
		{
			std::vector<CaseLine> cases;
			std::ifstream in(path);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#') continue;
				std::vector<std::string> f;
				size_t b = 0;
				for (;;)
				{
					const size_t e = line.find('|', b);
					f.push_back(line.substr(b, e == std::string::npos ? std::string::npos : e - b));
					if (e == std::string::npos) break;
					b = e + 1;
				}
				if (f.size() < 9) continue;
				CaseLine c;
				c.name = f[0]; c.raw = f[1];
				c.w = std::atoi(f[2].c_str()); c.h = std::atoi(f[3].c_str());
				c.ow = std::atoi(f[4].c_str()); c.oh = std::atoi(f[5].c_str());
				c.transfer = f[6]; c.light = f[7]; c.frames = std::atoi(f[8].c_str());
				cases.push_back(c);
			}
			return cases;
		}

		inline uint64_t Fnv1a(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
			return h;
		}
	}

	TEST_CLASS(X5AbCaptureTests)
	{
	public:
		TEST_METHOD(X5AbCaptureClips)
		{
			const char* listEnv = std::getenv("VP_X5AB_LIST");
			if (listEnv == nullptr || *listEnv == 0)
				return;   // not requested: nothing to capture
			const char* outEnv = std::getenv("VP_X5AB_OUT");
			const char* sideEnv = std::getenv("VP_X5AB_SIDE");
			Assert::IsTrue(outEnv != nullptr && sideEnv != nullptr, L"VP_X5AB_OUT and VP_X5AB_SIDE are required with VP_X5AB_LIST");
			const std::string outDir = outEnv, side = sideEnv;
			const auto cases = X5AbSupport::ParseList(listEnv);
			Assert::IsFalse(cases.empty(), L"the list has no cases");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const std::string source = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(source.empty());
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
			Assert::IsNotNull(ssim);
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt, L"no 8-bit source or 16-bit target format");

			std::ofstream csv(outDir + "\\" + side + ".csv", std::ios::app);
			Assert::IsTrue(csv.good(), L"cannot open the CSV");
			csv << "case,side,frame,seq,valid,grain_scale,mean_w,mean_luma,cut,cut_rule,hash64,gpu_ms\n";

			for (const auto& c : cases)
			{
				std::ifstream raw(c.raw, std::ios::binary);
				Assert::IsTrue(raw.good(), L"cannot open a raw clip");
				std::ofstream dump(outDir + "\\" + c.name + "-" + side + ".y10", std::ios::binary);

				AdaptiveDownscale::Module module;
				uint64_t sequence = 0;
				std::map<uint64_t, int> tipCut;   // sequence -> cut flag the module was given (tip only)
#ifdef X5AB_TIP_CUT
				std::map<uint64_t, VpLaneProducts::SourceStatsRecord> records;
				module.SetCutSource([&](uint64_t prev, uint64_t seq)
				{
					AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFrom(prev, seq,
						[&](uint64_t s, VpLaneProducts::SourceStatsRecord& rec, std::string& cause)
						{
							const auto it = records.find(s);
							if (it == records.end()) { cause = "harness: no record"; return false; }
							rec = it->second;
							return true;
						});
					tipCut[seq] = v.cut ? 1 : 0;
					return v;
				});
#endif
				AdaptiveDownscale::Settings st;
				const bool pq = c.transfer == "pq";
				std::vector<uint8_t> rgb(static_cast<size_t>(c.w) * c.h * 3), rgba(static_cast<size_t>(c.w) * c.h * 4, 255);
				std::vector<uint16_t> half(static_cast<size_t>(c.w / 2) * (c.h / 2));
				float prevMean = -1.0f;
				for (int f = 0; f < c.frames; ++f)
				{
					raw.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
					if (raw.gcount() != static_cast<std::streamsize>(rgb.size())) break;   // short clip: fewer frames
					for (size_t i = 0, n = static_cast<size_t>(c.w) * c.h; i < n; ++i)
					{
						rgba[i * 4 + 0] = rgb[i * 3 + 0];
						rgba[i * 4 + 1] = rgb[i * 3 + 1];
						rgba[i * 4 + 2] = rgb[i * 3 + 2];
					}
#ifdef X5AB_TIP_CUT
					{
						const int hw = c.w / 2, hh = c.h / 2;
						for (int y = 0; y < hh; ++y)
							for (int x = 0; x < hw; ++x)
							{
								double acc = 0.0;
								for (int dy = 0; dy < 2; ++dy)
									for (int dx = 0; dx < 2; ++dx)
									{
										const uint8_t* p = &rgb[(static_cast<size_t>(2 * y + dy) * c.w + 2 * x + dx) * 3];
										acc += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
									}
								half[static_cast<size_t>(y) * hw + x] = static_cast<uint16_t>(std::lround(acc / 4.0 * 257.0));
							}
						uint8_t buf[VpUnit::SourceStats::kRecordBytes] = {};
						VpLaneProducts::SourceStatsRecord rec;
						Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), hw, hh, buf, sizeof(buf)), L"P4 record compute");
						Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf, sizeof(buf), &rec), L"P4 record read");
						records[sequence + 1] = rec;
					}
#endif
					pl_tex_params sp{};
					sp.w = c.w; sp.h = c.h; sp.format = srcFmt; sp.sampleable = true; sp.initial_data = rgba.data();
					pl_tex src = pl_tex_create(gpu, &sp);
					Assert::IsNotNull(src);
					auto render = [&](bool advance, std::vector<uint16_t>* pixels) -> double
					{
						pl_tex_params tp{};
						tp.w = c.ow; tp.h = c.oh; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
						pl_tex target = pl_tex_create(gpu, &tp);
						pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
						pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
						image.color.transfer = out.color.transfer = pq ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_GAMMA22;
						image.color.primaries = out.color.primaries = pq ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709;
						pl_render_params params = pl_render_default_params;
						params.dither_params = nullptr;
						params.error_diffusion = nullptr;
						params.downscaler = &pl_filter_mitchell;
						params.disable_linear_scaling = c.light != "linear";
						if (advance) ++sequence;
						module.SetFrame(st, sequence, 1.0f / 24.0f);
						const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
						params.hooks = hooks;
						params.num_hooks = 3;
						const auto t0 = std::chrono::steady_clock::now();
						Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
						pl_gpu_finish(gpu);
						const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
						if (pixels != nullptr)
						{
							pixels->assign(static_cast<size_t>(c.ow) * c.oh * 4, 0);
							pl_tex_transfer_params d{};
							d.tex = target; d.ptr = pixels->data();
							Assert::IsTrue(pl_tex_download(gpu, &d));
						}
						pl_tex_destroy(gpu, &target);
						return ms;
					};
					std::vector<uint16_t> out;
					const double ms = render(true, &out);
					render(false, nullptr);   // the next render polls the stats of the advanced frame
					const AdaptiveDownscale::Stats stats = module.LastStats();
					pl_tex_destroy(gpu, &src);
					const uint64_t hash = X5AbSupport::Fnv1a(out.data(), out.size() * sizeof(uint16_t));
					for (int y = 0; y < c.oh; y += 2)
						for (int x = 0; x < c.ow; x += 2)
						{
							const uint16_t* p = &out[(static_cast<size_t>(y) * c.ow + x) * 4];
							const uint16_t y10 = static_cast<uint16_t>(std::lround((0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 64.0));
							dump.write(reinterpret_cast<const char*>(&y10), sizeof(y10));
						}
					int cut;
					const char* rule;
#ifdef X5AB_TIP_CUT
					cut = tipCut.count(sequence) ? tipCut[sequence] : 0;
					rule = "p4_u7";
#else
					cut = (prevMean >= 0.0f && std::fabs(stats.meanLuma - prevMean) > 20.0f) ? 1 : 0;
					rule = "meanstep20_derived";
#endif
					prevMean = stats.meanLuma;
					char row[320];
					std::snprintf(row, sizeof(row), "%s,%s,%d,%llu,%d,%.6f,%.6f,%.3f,%d,%s,%016llx,%.3f\n", c.name.c_str(), side.c_str(), f,
						static_cast<unsigned long long>(sequence), stats.valid ? 1 : 0, stats.grainScale, stats.meanW, stats.meanLuma, cut, rule,
						static_cast<unsigned long long>(hash), ms);
					csv << row;
				}
			}
			csv.flush();
		}
	};
