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
// Cut column: tip = the verdict the module used, read from the SCENE PRODUCT (C5, audit x5dup MUST 1/2: FrameAnalysis::Scene
// groups produced by the shared U7 law VpUnit::CutFadeJudge::CutTracker over this clip's P4 records, sealed in frame order and
// delivered as SkeletonModule::FillScene delivers them for contiguous frames: a confirmed cut goes out once, with the group of the
// frame after it, cutLate; the module reads them through AdaptiveDownscale::VerdictFromScene, the production rule); base = the old
// rule re-derived from Stats.meanLuma (|step| > 20 codes). The harness models only the skeleton's delivery (no lane, no store; the
// real stage with the memo and store bound is gate B-S3) and no fade law (x5dup never snaps on a fade).
// Every row is one source-frame ADVANCE, so cut=1 on a row is one grain-term snap (the per-consumer reset impact of a cut verdict).
// cut_state (owner-intent fix 2026-11-15: false cuts are counted, not only misses): tip = first (no previous advanced frame: a cut
// by definition, never scored as a true or a false cut), known (the frame's scene group was read), unavailable (no verdict, no
// snap) or unasked (the module did not ask: a harness fault); base = derived. cut_here / confirmed = the frame's OWN U7 verdict in
// its group after the run (confirmed by the next frame; the last frame stays provisional), cut_late = the snap delivered a cut of an
// earlier frame; base: '-'. Rows are written when a clip ends (cut_here is final then). A short clip fails the case.
// The before side must run THIS text too (carry this file's commit onto the before branch). Scoring is not done here:
// tools/x5ab-score.ps1 reads both sides' CSVs, the list file and a hand-marked truth list (tests/README.md, "X5 cut A/B").
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

#ifdef X5AB_TIP_CUT
		// The scene product of one clip: U7's CutTracker (the shared law, unchanged) pushed once per frame in order, and the groups the
		// skeleton's seal writes for contiguous frames with no look-ahead (SkeletonModule::FillScene): the frame's provisional verdict
		// (cutHere, confirmed), rewritten when the next frame confirms it; the delivered `cut` once per confirmed cut: now when the
		// verdict is final at once (the no-grid pair-law fallback), else with the next frame's group (cutLate). No classification here.
		struct SceneProductModel
		{
			VpUnit::CutFadeJudge::CutTracker tracker;
			std::map<uint64_t, FrameAnalysis::Scene> groups;
			std::map<uint64_t, bool> delivered;   // cut frames already delivered (exactly once per cut)

			void Seal(uint64_t seq, const VpLaneProducts::SourceStatsRecord& rec)
			{
				FrameAnalysis::Scene s;
				s.origin = FrameAnalysis::SceneOrigin::Vp;
				const auto done = tracker.Push(seq, rec);   // the CONFIRMED verdict of the previous frame (known = false: none)
				if (done.known)
				{
					const auto it = groups.find(done.seq);
					if (it != groups.end() && !it->second.confirmed)
					{
						it->second.cutHere = done.cut;
						it->second.flash = done.flash;
						it->second.cutPossible = done.candidate;
						it->second.cutStrength = done.strength;
						it->second.confirmed = true;
					}
					if (done.cut && !delivered[done.seq])
					{
						delivered[done.seq] = true;
						s.cut = true;
						s.cutLate = true;
					}
				}
				const auto own = tracker.Provisional();
				s.cutHere = own.cut;
				s.flash = own.flash;
				s.cutPossible = own.candidate;
				s.cutStrength = own.strength;
				s.confirmed = own.confirmed;
				if (own.known && own.confirmed && own.cut && !delivered[seq])
				{
					delivered[seq] = true;
					s.cut = true;   // final at once: delivered with its own group (cut = now || late, one snap)
				}
				groups[seq] = s;
			}

			bool Read(uint64_t seq, FrameAnalysis::Scene& scene) const
			{
				const auto it = groups.find(seq);
				if (it == groups.end())
					return false;
				scene = it->second;
				return true;
			}
		};
#endif
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
			csv << "case,side,frame,seq,valid,grain_scale,mean_w,mean_luma,cut,cut_rule,cut_state,cut_here,confirmed,cut_late,hash64,gpu_ms\n";

			for (const auto& c : cases)
			{
				std::ifstream raw(c.raw, std::ios::binary);
				Assert::IsTrue(raw.good(), L"cannot open a raw clip");
				std::ofstream dump(outDir + "\\" + c.name + "-" + side + ".y10", std::ios::binary);

				AdaptiveDownscale::Module module;
				uint64_t sequence = 0;
				std::map<uint64_t, int> tipCut;   // sequence -> cut flag the module was given (tip only)
				std::map<uint64_t, const char*> tipState;   // sequence -> first / known / unavailable (tip only)
				std::map<uint64_t, int> tipLate;   // sequence -> the snap delivered an earlier frame's cut (tip only)
				std::vector<std::string> rows;   // written when the clip ends (cut_here / confirmed are final then)
				std::vector<uint64_t> rowSeqs;
#ifdef X5AB_TIP_CUT
				X5AbSupport::SceneProductModel scene;
				module.SetCutSource([&](uint64_t prev, uint64_t seq)
				{
					// The production read rule (AdaptiveDownscaleCut.h) over the modelled groups; reach = a default store ring.
					AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFromScene(prev, seq,
						[&scene](uint64_t s, FrameAnalysis::Scene& g) { return scene.Read(s, g); },
						FrameAnalysis::RingDepth(4, 4), "harness: no scene group for frame " + std::to_string(seq));
					tipCut[seq] = v.cut ? 1 : 0;
					tipLate[seq] = v.cutLate ? 1 : 0;
					tipState[seq] = v.state != AdaptiveDownscale::CutVerdict::State::Known ? "unavailable" : (v.first ? "first" : "known");
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
					// A short clip fails the case (audit x5dup MUST 2: rows must equal the list's frame count; no silent truncation).
					Assert::IsTrue(raw.gcount() == static_cast<std::streamsize>(rgb.size()), L"short clip: fewer frames than the list says");
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
						scene.Seal(sequence + 1, rec);   // the frame's seal (before it renders): its scene group is published
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
					const char* state;
					std::string late = "-";
#ifdef X5AB_TIP_CUT
					cut = tipCut.count(sequence) ? tipCut[sequence] : 0;
					rule = "u7_scene";
					state = tipState.count(sequence) ? tipState[sequence] : "unasked";
					late = tipLate.count(sequence) && tipLate[sequence] ? "1" : "0";
#else
					cut = (prevMean >= 0.0f && std::fabs(stats.meanLuma - prevMean) > 20.0f) ? 1 : 0;
					rule = "meanstep20_derived";
					state = "derived";
#endif
					prevMean = stats.meanLuma;
					char row[320];
					// cut_here / confirmed: filled in when the clip ends (marker @HERE@).
					std::snprintf(row, sizeof(row), "%s,%s,%d,%llu,%d,%.6f,%.6f,%.3f,%d,%s,%s,@HERE@,%s,%016llx,%.3f\n", c.name.c_str(),
						side.c_str(), f, static_cast<unsigned long long>(sequence), stats.valid ? 1 : 0, stats.grainScale, stats.meanW,
						stats.meanLuma, cut, rule, state, late.c_str(), static_cast<unsigned long long>(hash), ms);
					rows.push_back(row);
					rowSeqs.push_back(sequence);
				}
				for (size_t i = 0; i < rows.size(); ++i)
				{
					std::string here = "-,-";
#ifdef X5AB_TIP_CUT
					FrameAnalysis::Scene g;
					if (scene.Read(rowSeqs[i], g))
						here = std::string(g.cutHere ? "1" : "0") + "," + (g.confirmed ? "1" : "0");
#endif
					std::string& text = rows[i];
					text.replace(text.find("@HERE@"), 6, here);
					csv << text;
				}
			}
			csv.flush();
		}
	};
