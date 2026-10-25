// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(SourceResolutionHookTests)
	{
	public:


		// Look-ahead verification (owner 2026-10-16): CPU only - frames ahead of display
		// decide the switch at the cut / black frame, or cancel it.
		TEST_METHOD(SourceResolutionMonitorIdentifiesThePlayerKernel)
		{
			// Kernel-matched re-base (owner 2026-10-16: optimise 1080p): frames upscaled 2x by
			// a player kernel are read as 1080 and the kernel identified across shots; native
			// frames never identify one. CPU only.
			const int T = SourceResolutionMonitor::Monitor::kTile, M = T / 2;
			auto frameTiles = [&](const KernelInverse::Candidate* c, unsigned seed, float level) {
				std::vector<float> tiles(static_cast<size_t>(8) * T * T);
				std::mt19937 rng(seed);
				std::normal_distribution<float> noise(0.0f, 0.06f);
				for (int t = 0; t < 8; ++t)
				{
					float* tile = &tiles[static_cast<size_t>(t) * T * T];
					if (!c)
					{
						for (int i = 0; i < T * T; ++i) tile[i] = (std::min)(1.0f, (std::max)(0.0f, level + noise(rng)));
					}
					else
					{
						std::vector<float> master(static_cast<size_t>(M) * M), rows(static_cast<size_t>(M) * T), col(static_cast<size_t>(M)), out(static_cast<size_t>(T));
						for (float& v : master) v = level + noise(rng);
						for (int y = 0; y < M; ++y) KernelInverse::UpscaleLine(*c, &master[static_cast<size_t>(y) * M], M, &rows[static_cast<size_t>(y) * T]);
						for (int x = 0; x < T; ++x)
						{
							for (int y = 0; y < M; ++y) col[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * T + x];
							KernelInverse::UpscaleLine(*c, col.data(), M, out.data());
							for (int y = 0; y < T; ++y) tile[static_cast<size_t>(y) * T + x] = (std::min)(1.0f, (std::max)(0.0f, out[static_cast<size_t>(y)]));
						}
					}
					for (int i = 0; i < T * T; ++i) tile[i] = std::round(tile[i] * 1023.0f) / 1023.0f;
				}
				return tiles;
			};
			const int k = KernelInverse::Find("lanczos3");
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			auto run = [&](const KernelInverse::Candidate* kernel) {
				SourceResolutionMonitor::Monitor m;
				// The behaviour before the latch fixes (its synthetic noise frames have no shape, so the
				// shape-aware cut test sees one long shot; SourceResolutionLatchTests cover the fixes).
				m.SetLatchFixes(false);
				double t = 0.0;
				for (int shot = 0; shot < 14; ++shot)
					for (int f = 0; f < 6; ++f, t += 0.5)
						m.AnalyseTilesForTest(frameTiles(kernel, 1000 + shot * 31 + f, shot % 2 ? 0.55f : 0.35f), 8, t, 3840, 2160);
				return m.Read();
			};
			const SourceResolutionMonitor::Snapshot up = run(&c);
			char line[200];
			snprintf(line, sizeof(line), "upscaled (lanczos3): declared %.3f, kernel %d (%s), votes %d, agree %d\n", up.declaredRatio, up.kernel,
				up.kernel >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(up.kernel)].name.c_str() : "none", up.kernelVotes, up.kernelAgree);
			Logger::WriteMessage(line);
			Assert::AreEqual(0.5, up.declaredRatio, 0.01, L"read as 1080");
			Assert::AreEqual(k, up.kernel, L"the player's kernel identified");
			const SourceResolutionMonitor::Snapshot native = run(nullptr);
			snprintf(line, sizeof(line), "native: declared %.3f, kernel %d, votes %d\n", native.declaredRatio, native.kernel, native.kernelVotes);
			Logger::WriteMessage(line);
			Assert::AreEqual(-1, native.kernel, L"native: none");
		}

		TEST_METHOD(SourceResolutionLookAheadVerifiesOrCancelsTheSwitch)
		{
			const int FW = 1024, FH = 512, T = SourceResolutionMonitor::Monitor::kTile;
			// A frame: an upscale (white noise at half resolution, bilinear x2) or native
			// (white noise at full resolution), around a mean level; or flat black.
			enum class Kind { Upscaled, Native, Black };
			auto makeFrame = [&](Kind kind, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::normal_distribution<double> noise(0.0, 0.08);
				std::vector<double> f(static_cast<size_t>(FW) * FH, 64.0 / 1023.0);
				if (kind == Kind::Black) return f;
				if (kind == Kind::Native)
				{
					for (double& v : f) v = level + noise(rng);
					return f;
				}
				const int lw = FW / 2, lh = FH / 2;
				std::vector<double> low(static_cast<size_t>(lw) * lh);
				for (double& v : low) v = level + noise(rng);
				for (int y = 0; y < FH; ++y)
					for (int x = 0; x < FW; ++x)
					{
						const double u = (x + 0.5) * 0.5 - 0.5, w = (y + 0.5) * 0.5 - 0.5;
						const int x0 = (std::max)(0, (std::min)(lw - 2, static_cast<int>(std::floor(u))));
						const int y0 = (std::max)(0, (std::min)(lh - 2, static_cast<int>(std::floor(w))));
						const double fx = (std::min)(1.0, (std::max)(0.0, u - x0)), fy = (std::min)(1.0, (std::max)(0.0, w - y0));
						auto at = [&](int xx, int yy) { return low[static_cast<size_t>(yy) * lw + xx]; };
						f[static_cast<size_t>(y) * FW + x] = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
					}
				return f;
			};
			auto code = [](double v) { return static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 1023.0)); };
			// The monitor's own analysis of a displayed frame (8 tiles).
			auto analyse = [&](SourceResolutionMonitor::Monitor& m, const std::vector<double>& f, double t) {
				std::vector<float> tiles(static_cast<size_t>(8) * T * T);
				for (int i = 0; i < 8; ++i)
				{
					const int x0 = (i % 4) * (FW - T) / 3, y0 = (i / 4) * (FH - T);
					for (int y = 0; y < T; ++y)
						for (int x = 0; x < T; ++x)
							tiles[(static_cast<size_t>(i) * T + y) * T + x] = static_cast<float>(code(f[static_cast<size_t>(y0 + y) * FW + x0 + x]) / 1023.0);
				}
				m.AnalyseTilesForTest(tiles, 8, t, FW, FH);
			};
			auto ahead = [&](SourceResolutionMonitor::Monitor& m, uint64_t seq, const std::vector<double>& f) {
				m.LookAheadFrame(seq, FW, FH, [&](int x, int y) { return code(f[static_cast<size_t>(y) * FW + x]); }, 0, 0, FW, FH);
			};
			auto waitReady = [&](SourceResolutionMonitor::Monitor& m) {
				for (int k = 0; k < 400 && !m.Ahead().ready; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				return m.Ahead().ready;
			};
			// Upscaled shots until auto waits for the next cut; returns the time reached.
			auto untilWaiting = [&](SourceResolutionMonitor::Monitor& m, uint64_t& seq) {
				double t = 0.0;
				std::string why;
				for (int shot = 0; shot < 12; ++shot)
					for (int i = 0; i < 8; ++i)
					{
						analyse(m, makeFrame(Kind::Upscaled, 50 + shot * 13 + i, shot % 2 ? 0.55 : 0.35), t);
						m.AutoFactor(FH, 0.8, why, t, ++seq);
						t += 0.5;
						if (why == "waiting for the next cut or black") return t;
					}
				return -1.0;
			};
			const double offShot = 0.35;   // the level of the shot showing when auto begins to wait (even shots)
			(void)offShot;

			// 1. The next shot agrees: engaged exactly on the cut frame.
			{
				SourceResolutionMonitor::Monitor m;
				m.SetLatchFixes(false);   // the behaviour before the latch fixes (256-px ahead tiles, mean-jump cuts)
				uint64_t seq = 0;
				const double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0, L"auto reached waiting");
				const double level = m.Read().frames % 2 ? 0.35 : 0.35;
				(void)level;
				// Ahead: one more frame of the current shot, then the cut at seq + 2.
				const std::vector<double> same = makeFrame(Kind::Upscaled, 999, 0.35);
				const std::vector<double> next = makeFrame(Kind::Upscaled, 1000, 0.75);
				ahead(m, seq + 1, same);
				ahead(m, seq + 2, next);
				Assert::IsTrue(waitReady(m), L"the frame after the cut was analysed ahead");
				std::string why;
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t, seq + 1), 1e-9, L"not before the cut frame");
				const double f = m.AutoFactor(FH, 0.8, why, t + 0.04, seq + 2);
				Logger::WriteMessage(("agree: " + why + "\n").c_str());
				Assert::AreEqual(0.5, f, 1e-9, L"engaged on the cut frame");
				Assert::AreEqual(std::string("engaged at the cut / black (look-ahead verified)"), why);
				Assert::AreEqual(1ull, m.Ahead().verified);
			}
			// 2. The next shot is native: the switch is cancelled, and the cut itself is not used.
			{
				SourceResolutionMonitor::Monitor m;
				m.SetLatchFixes(false);   // the behaviour before the latch fixes (256-px ahead tiles, mean-jump cuts)
				uint64_t seq = 0;
				double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0);
				const std::vector<double> native = makeFrame(Kind::Native, 2000, 0.75);
				ahead(m, seq + 1, makeFrame(Kind::Upscaled, 1999, 0.35));   // the current shot (the first frame ahead sets the baseline)
				ahead(m, seq + 2, native);
				Assert::IsTrue(waitReady(m));
				std::string why;
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t, seq + 1), 1e-9);
				const double f = m.AutoFactor(FH, 0.8, why, t, seq + 2);
				Logger::WriteMessage(("disagree: " + why + "\n").c_str());
				Assert::AreEqual(1.0, f, 1e-9, L"stays native");
				Assert::AreEqual(std::string("look-ahead: the next shot disagrees - staying"), why);
				Assert::AreEqual(1ull, m.Ahead().cancelled);
				// The monitor then sees that cut (the native frame): still no engage on it.
				analyse(m, native, t + 0.5);
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t + 0.5, seq + 3), 1e-9, L"the cancelled cut is not used");
			}
			// 3. A black frame ahead, then an agreeing shot: engaged on the black frame.
			{
				SourceResolutionMonitor::Monitor m;
				m.SetLatchFixes(false);   // the behaviour before the latch fixes (256-px ahead tiles, mean-jump cuts)
				uint64_t seq = 0;
				const double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0);
				const std::vector<double> black = makeFrame(Kind::Black, 0, 0.0);
				const std::vector<double> next = makeFrame(Kind::Upscaled, 3000, 0.45);
				ahead(m, seq + 1, black);
				ahead(m, seq + 2, black);
				ahead(m, seq + 3, next);
				Assert::IsTrue(waitReady(m));
				std::string why;
				const double f = m.AutoFactor(FH, 0.8, why, t, seq + 1);
				Logger::WriteMessage(("black: " + why + "\n").c_str());
				Assert::AreEqual(0.5, f, 1e-9, L"engaged on the black frame, verified by the shot after it");
			}
		}

		TEST_METHOD(SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce)
		{
			// Stage 12 step B + auto (owner 2026-10-15). Simulated time (the
			// monitor takes its clock from the caller): a 1080-line frame
			// upscaled from 540 lines over eight shots, then native detail.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			// libplacebo's own errors for this test (the fixture's log is silent).
			static std::string plErrors;
			pl_log_params lp{};
			lp.log_level = PL_LOG_DEBUG;
			lp.log_cb = [](void*, pl_log_level, const char* msg) { if (plErrors.size() < 4000) { plErrors += msg; plErrors += '\n'; } };
			pl_log errorLog = pl_log_create(PL_API_VER, &lp);
			pl_log_update(fixture.Log(), &lp);   // the device's own messages too
			pl_dispatch dispatch = pl_dispatch_create(errorLog, gpu);
			const int W = 1920, H = 1080, w = 960, h = 540;
			// Smooth random content at size (sw, sh): a sum of a few hundred waves.
			auto content = [](int sw, int sh, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(sw) * sh, level);
				for (int k = 0; k < 120; ++k)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng)), a = u(rng) * 3.14159265358979, px = u(rng) * 6.283, py = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a), amp = 0.012 / std::sqrt(f);
					std::vector<double> cx(sw), sx(sw);
					for (int x = 0; x < sw; ++x) { cx[x] = std::cos(6.283185307 * fx * x + px); sx[x] = std::sin(6.283185307 * fx * x + px); }
					for (int y = 0; y < sh; ++y)
					{
						const double cy = std::cos(6.283185307 * fy * y + py) * amp, sy = std::sin(6.283185307 * fy * y + py) * amp;
						double* row = &v[static_cast<size_t>(y) * sw];
						for (int x = 0; x < sw; ++x) row[x] += cx[x] * cy - sx[x] * sy;
					}
				}
				return v;
			};
			// Catmull-Rom 2x upscale (separable, centre-aligned).
			auto upscale = [&](const std::vector<double>& src) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				std::vector<double> rows(static_cast<size_t>(W) * h), out(static_cast<size_t>(W) * H);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W + x] = s / ws;
					}
				for (int y = 0; y < H; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W + x]; ws += wt; }
						out[static_cast<size_t>(y) * W + x] = s / ws;
					}
				}
				return out;
			};
			// 10-bit codes in a P010-style r16 plane (code << 6).
			auto upload = [&](const std::vector<double>& v, double offset = 0.0) {
				std::vector<uint16_t> data(v.size());
				for (size_t i = 0; i < v.size(); ++i)
					data[i] = static_cast<uint16_t>(static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v[i] + offset)) * 1023.0)) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.repr.sys = PL_COLOR_SYSTEM_BT_709;
			frame.repr.levels = PL_COLOR_LEVELS_LIMITED;
			frame.repr.bits.sample_depth = 16; frame.repr.bits.color_depth = 10; frame.repr.bits.bit_shift = 6;
			frame.color = pl_color_space_bt709;
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };

			SourceResolutionMonitor::Monitor monitor;
			// Gate fix 2026-10-17: the latch fixes on (the default) again. The integration had pinned
			// them off for the ~542 reading of a 540-line source at 128-px tiles; fix/latch-540-exact
			// (0ccfc561) declares the exact rendition, and both wait texts are accepted below.
			auto isWaiting = [](const std::string& text) {
				return text == "waiting for the next cut or black" || text.rfind("confirmed: waiting for a cut", 0) == 0;
			};
			// Owner 2026-10-16: the auto history, one file per session, the newest 5 kept.
			char tempPath[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, tempPath);
			const std::string historyFolder = std::string(tempPath) + "vp-source-resolution-history";
			CreateDirectoryA(historyFolder.c_str(), nullptr);
			for (int old = 0; old < 7; ++old)   // seven earlier sessions (old names sort first)
				std::ofstream(historyFolder + "\\source-resolution 2020010" + std::to_string(old) + "-000000.csv") << "old";
			monitor.SetHistoryFolder(historyFolder);
			const std::string historyPath = monitor.HistoryPath();
			{
				int kept = 0;
				WIN32_FIND_DATAA found{};
				HANDLE search = FindFirstFileA((historyFolder + "\\source-resolution *.csv").c_str(), &found);
				if (search != INVALID_HANDLE_VALUE) { do ++kept; while (FindNextFileA(search, &found)); FindClose(search); }
				Assert::AreEqual(4, kept, L"the newest 4 earlier sessions kept before this one is written (5 with it)");
			}
			double seconds = 0.0;
			uint64_t sequence = 0;
			// One analysed frame: submit, let the GPU finish, collect, wait for the worker.
			auto step = [&](pl_tex tex) {
				frame.planes[0].texture = tex;
				const unsigned long long before = monitor.Read().frames;
				monitor.Frame(gpu, dispatch, frame, ++sequence, seconds);
				pl_gpu_finish(gpu);
				monitor.Frame(gpu, dispatch, frame, sequence, seconds);   // collects (interval not passed)
				for (int i = 0; i < 400 && monitor.Read().frames == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				if (monitor.Read().frames == before)
					Assert::Fail(ExtendedRangeGpuProbe::ToWide("no frame analysed: " + SourceResolutionMonitor::Describe(monitor.Read(), 1.0, false) +
						" / " + monitor.Read().error + " / ssbo " + std::to_string(gpu->limits.max_ssbo_size) + " buf " + std::to_string(gpu->limits.max_buf_size) + " / libplacebo: " + plErrors).c_str());
				seconds += 0.5;
			};
			std::vector<std::string> log;
			std::string why;
			double factor = 1.0;
			int engagedAtShot = -1, waitingAtShot = -1;
			for (int shot = 0; shot < 9; ++shot)
			{
				const std::vector<double> picture = upscale(content(w, h, 100 + shot, shot % 2 ? 0.55 : 0.30));
				for (int i = 0; i < 20; ++i)
				{
					// Small changes within a shot (not a repeated frame): a slow brightness drift.
					pl_tex tex = upload(picture, 0.0008 * i);
					step(tex);
					pl_tex_destroy(gpu, &tex);
					factor = monitor.AutoFactor(H, 0.8, why);
					if (!why.empty())
					{
						log.push_back("shot " + std::to_string(shot) + ": " + why);
						if (isWaiting(why) && waitingAtShot < 0) waitingAtShot = shot;
						if (why == "engaged at a cut" && engagedAtShot < 0) engagedAtShot = shot;
					}
				}
			}
			const SourceResolutionMonitor::Snapshot s = monitor.Read();
			for (const std::string& l : log) Logger::WriteMessage((l + "\n").c_str());
			Logger::WriteMessage((SourceResolutionMonitor::Describe(s, monitor.AutoRatio(), true) + "\n").c_str());
			Assert::IsTrue(s.reading, L"the source is read");
			Assert::AreEqual(0.5, s.declaredRatio, 0.01, L"declared: the 540-line source");
			Assert::IsTrue(s.confidence >= 0.8, L"with confidence");
			Assert::IsTrue(waitingAtShot >= 0 && engagedAtShot > waitingAtShot, L"auto waits for the next cut, then engages");
			Assert::AreEqual(0.5, factor, 1e-9, L"re-based to 540 lines");
			Assert::AreEqual(1080, s.nativeHeight);

			// Owner 2026-10-16: engaging also happens during a black frame (a fade's dark end), where
			// the change cannot be seen - a second monitor waiting to engage, then one black frame.
			{
				SourceResolutionMonitor::Monitor faded;
				// The fade is under test, not the latch fixes' crossfade after the wait (20 s by default,
				// shorter than this 40-s fade): no crossfade here.
				faded.SetEngageTiming(600.0, 1.5, 0.25);
				double t = 0.0;
				std::string fadedWhy;
				double fadedFactor = 1.0;
				bool waiting = false;
				std::vector<double> held;   // the shot that was showing when auto began to wait
				for (int shot = 0; shot < 6 && !waiting; ++shot)
				{
					const std::vector<double> picture = upscale(content(w, h, 700 + shot, shot % 2 ? 0.55 : 0.30));
					held = picture;
					for (int i = 0; i < 20 && !waiting; ++i)
					{
						pl_tex tex = upload(picture, 0.0008 * i);
						frame.planes[0].texture = tex;
						const unsigned long long before = faded.Read().frames;
						faded.Frame(gpu, dispatch, frame, ++sequence, t);
						pl_gpu_finish(gpu);
						faded.Frame(gpu, dispatch, frame, sequence, t);
						for (int k = 0; k < 400 && faded.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
						t += 0.5;
						fadedFactor = faded.AutoFactor(H, 0.8, fadedWhy, t);
						waiting = isWaiting(fadedWhy);
						pl_tex_destroy(gpu, &tex);
					}
				}
				Assert::IsTrue(waiting, L"auto reached 'waiting' on the upscaled shots");
				// A fade to black (each step below the cut level, so no cut is seen): the change is
				// made once the picture holds no detail.
				const std::vector<double> last = held;
				bool sawCut = false;
				// h3-latch (cut fixes, integration round 5): a candidate from the held shot's own cut
				// can be confirmed at the first fade frame (deferred, keeping its own moment before the
				// fade). A cut by the fade is a switch moment, or a confirmed cut whose moment is in it.
				const double fadeStart = t;
				for (int step = 1; step <= 80 && fadedFactor != 0.5; ++step)
				{
					std::vector<double> dim(last);
					const double keep = std::pow(0.9, step);
					for (double& v : dim) v = 0.0625 + (v - 0.0625) * keep;
					pl_tex tex = upload(dim);
					frame.planes[0].texture = tex;
					const auto stateBefore = faded.Read();
					const unsigned long long cutsBefore = stateBefore.cuts, momentsBefore = stateBefore.moments;
					const unsigned long long before = stateBefore.frames;
					faded.Frame(gpu, dispatch, frame, ++sequence, t);
					pl_gpu_finish(gpu);
					faded.Frame(gpu, dispatch, frame, sequence, t);
					for (int k = 0; k < 400 && faded.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					const auto stateAfter = faded.Read();
					if (stateAfter.cuts != cutsBefore || stateAfter.moments != momentsBefore)
						Logger::WriteMessage(("fade step " + std::to_string(step) + ": cuts " + std::to_string(stateAfter.cuts) +
							" moments " + std::to_string(stateAfter.moments) + " last cut at " + std::to_string(stateAfter.lastCutSeconds) +
							" s (fade from " + std::to_string(fadeStart) + " s)\n").c_str());
					sawCut = sawCut || stateAfter.moments != momentsBefore ||
						(stateAfter.cuts != cutsBefore && stateAfter.lastCutSeconds >= fadeStart - 1e-9);
					t += 0.5;
					fadedFactor = faded.AutoFactor(H, 0.8, fadedWhy, t);
					pl_tex_destroy(gpu, &tex);
				}
				Logger::WriteMessage(("fade: " + fadedWhy + "\n").c_str());
				Assert::IsFalse(sawCut, L"the fade made no cut");
				Assert::AreEqual(0.5, fadedFactor, 1e-9, L"engaged in the fade");
				// Latch fixes on: a real black or a low-detail moment; off: the earlier text.
				Assert::IsTrue(fadedWhy == "engaged during a black or flat frame" || fadedWhy == "engaged during a real black" ||
					fadedWhy == "engaged during a low-detail moment", ExtendedRangeGpuProbe::ToWide("at the fade's dark end: " + fadedWhy).c_str());
				faded.Release(gpu);
			}

			// Planner Q2: only the confidence falls (here: the minimum raised above it) - native at the
			// next cut, at most 5 s later; not at once.
			{
				factor = monitor.AutoFactor(H, 1.01, why, seconds);
				Logger::WriteMessage(("confidence fell: " + why + "\n").c_str());
				Assert::AreEqual(0.5, factor, 1e-9, L"a confidence drop does not disengage mid-shot");
				factor = monitor.AutoFactor(H, 1.01, why, seconds + 2.0);
				Assert::AreEqual(0.5, factor, 1e-9, L"still waiting for a cut after 2 s");
				factor = monitor.AutoFactor(H, 1.01, why, seconds + 5.1);
				Logger::WriteMessage(("after 5 s: " + why + "\n").c_str());
				Assert::AreEqual(1.0, factor, L"native after 5 s without a cut");
			}

			// Native detail: back to native at once (same shot level, no cut needed).
			{
				pl_tex tex = upload(content(W, H, 999, 0.55));
				step(tex);
				factor = monitor.AutoFactor(H, 0.8, why);
				Logger::WriteMessage(("native detail: " + why + "\n").c_str());
				Assert::AreEqual(1.0, factor, L"native at once on real detail");
				Assert::AreEqual(1.0, monitor.Read().declaredRatio, L"declared native");
				pl_tex_destroy(gpu, &tex);
			}

			// A discontinuity: native at once and the evidence starts again.
			monitor.Discontinuity("test seek");
			factor = monitor.AutoFactor(H, 0.8, why);
			Assert::AreEqual(1.0, factor);
			{
				pl_tex tex = upload(upscale(content(w, h, 5, 0.4)));
				step(tex);
				pl_tex_destroy(gpu, &tex);
			}
			Assert::AreEqual(1.0, monitor.Read().declaredRatio, L"evidence starts again after a seek");
			Assert::AreEqual(std::string("test seek"), monitor.Read().lastDiscontinuity);

			// The history holds the readings and every auto decision with its reason.
			monitor.FlushHistoryNow();
			{
				std::ifstream in(historyPath);
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				Assert::IsTrue(text.find("time,event,frame_ratio") == 0, L"history header");
				Assert::IsTrue(text.find("session started") != std::string::npos, L"the session is marked");
				Assert::IsTrue(text.find(",reading,") != std::string::npos, L"reading rows");
				Assert::IsTrue(text.find("engaged at a cut") != std::string::npos, L"the engage decision and its reason");
				Assert::IsTrue(text.find("native after 5 s without a cut") != std::string::npos, L"the bounded disengage");
				Assert::IsTrue(text.find("discontinuity") != std::string::npos && text.find("test seek") != std::string::npos,
					L"the seek");
			}
			{
				WIN32_FIND_DATAA found{};
				HANDLE search = FindFirstFileA((historyFolder + "\\*").c_str(), &found);
				if (search != INVALID_HANDLE_VALUE)
				{
					do if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((historyFolder + "\\" + found.cFileName).c_str());
					while (FindNextFileA(search, &found));
					FindClose(search);
				}
				RemoveDirectoryA(historyFolder.c_str());
			}

			// Genuine full-resolution content over many shots never engages.
			SourceResolutionMonitor::Monitor native;
			seconds = 0.0;
			int engaged = 0;
			for (int shot = 0; shot < 8; ++shot)
			{
				const std::vector<double> picture = content(W, H, 300 + shot, shot % 2 ? 0.55 : 0.30);
				for (int i = 0; i < 20; ++i)
				{
					pl_tex tex = upload(picture, 0.0008 * i);
					frame.planes[0].texture = tex;
					const unsigned long long before = native.Read().frames;
					native.Frame(gpu, dispatch, frame, ++sequence, seconds);
					pl_gpu_finish(gpu);
					native.Frame(gpu, dispatch, frame, sequence, seconds);
					for (int k = 0; k < 400 && native.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					seconds += 0.5;
					engaged += native.AutoFactor(H, 0.8, why) < 1.0;
					pl_tex_destroy(gpu, &tex);
				}
			}
			Logger::WriteMessage((SourceResolutionMonitor::Describe(native.Read(), native.AutoRatio(), true) + "\n").c_str());
			Assert::AreEqual(0, engaged, L"native content never engages");
			Assert::AreEqual(1.0, native.Read().declaredRatio);
			monitor.Release(gpu);
			native.Release(gpu);
			pl_dispatch_destroy(&dispatch);
			pl_log_destroy(&errorLog);
		}

		TEST_METHOD(SourceResolutionReadsAndRebasesTheDolbyVisionTunnel)
		{
			// 2026-10-17: the DV tunnel is no longer skipped as a "packed signal".
			// Its decoded intensity plane (12-bit codes << 4, repr DOLBYVISION) is
			// measured like YCbCr luma: a 540-line picture upscaled to 1080 is
			// declared 0.5. The re-base resamples the IPT planes and keeps the
			// Dolby Vision representation (its DM matrices) for libplacebo's decode.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1920, H = 1080, w = 960, h = 540;
			auto content = [](int sw, int sh, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(sw) * sh, level);
				for (int k = 0; k < 120; ++k)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng)), a = u(rng) * 3.14159265358979, px = u(rng) * 6.283, py = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a), amp = 0.012 / std::sqrt(f);
					std::vector<double> cx(sw), sx(sw);
					for (int x = 0; x < sw; ++x) { cx[x] = std::cos(6.283185307 * fx * x + px); sx[x] = std::sin(6.283185307 * fx * x + px); }
					for (int y = 0; y < sh; ++y)
					{
						const double cy = std::cos(6.283185307 * fy * y + py) * amp, sy = std::sin(6.283185307 * fy * y + py) * amp;
						double* row = &v[static_cast<size_t>(y) * sw];
						for (int x = 0; x < sw; ++x) row[x] += cx[x] * cy - sx[x] * sy;
					}
				}
				return v;
			};
			auto upscale = [&](const std::vector<double>& src) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				std::vector<double> rows(static_cast<size_t>(W) * h), out(static_cast<size_t>(W) * H);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W + x] = s / ws;
					}
				for (int y = 0; y < H; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W + x]; ws += wt; }
						out[static_cast<size_t>(y) * W + x] = s / ws;
					}
				}
				return out;
			};
			// 12-bit intensity codes << 4 (the DV formatter's / GPU unpack's plane).
			auto upload = [&](const std::vector<double>& v, double offset) {
				std::vector<uint16_t> data(v.size());
				for (size_t i = 0; i < v.size(); ++i)
					data[i] = static_cast<uint16_t>(static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v[i] + offset)) * 4095.0)) << 4);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			std::vector<uint16_t> neutral(static_cast<size_t>(W / 2) * H * 2, static_cast<uint16_t>(2048 << 4));
			pl_tex_params cp{};
			cp.w = W / 2; cp.h = H; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.sampleable = true; cp.initial_data = neutral.data();
			pl_tex chroma = pl_tex_create(gpu, &cp);
			Assert::IsNotNull(chroma);
			pl_dovi_metadata dovi{};
			dovi.nonlinear.m[0][0] = dovi.nonlinear.m[1][1] = dovi.nonlinear.m[2][2] = 1.0f;
			dovi.linear.m[0][0] = dovi.linear.m[1][1] = dovi.linear.m[2][2] = 1.0f;
			pl_frame frame{};
			frame.num_planes = 2;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.planes[1].texture = chroma;
			frame.planes[1].components = 2;
			frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			frame.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			frame.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
			frame.repr.dovi = &dovi;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.repr.bits.sample_depth = 16; frame.repr.bits.color_depth = 12; frame.repr.bits.bit_shift = 4;
			frame.color.primaries = PL_COLOR_PRIM_BT_2020;
			frame.color.transfer = PL_COLOR_TRC_PQ;
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };

			SourceResolutionMonitor::Monitor monitor;
			double seconds = 0.0;
			uint64_t sequence = 0;
			pl_tex first = nullptr;
			for (int shot = 0; shot < 9; ++shot)
			{
				const std::vector<double> picture = upscale(content(w, h, 300 + shot, shot % 2 ? 0.55 : 0.30));
				for (int i = 0; i < 20; ++i)
				{
					pl_tex tex = upload(picture, 0.0008 * i);
					frame.planes[0].texture = tex;
					Assert::IsTrue(SourceResolutionMonitor::Monitor::NotReadReason(frame).empty(), L"DV is readable");
					const unsigned long long before = monitor.Read().frames;
					monitor.Frame(gpu, dispatch, frame, ++sequence, seconds);
					pl_gpu_finish(gpu);
					monitor.Frame(gpu, dispatch, frame, sequence, seconds);
					for (int t = 0; t < 400 && monitor.Read().frames == before; ++t) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					seconds += 0.5;
					if (!first && shot == 0 && i == 0) first = tex;
					else pl_tex_destroy(gpu, &tex);
				}
			}
			const SourceResolutionMonitor::Snapshot s = monitor.Read();
			Logger::WriteMessage(("DV: " + SourceResolutionMonitor::Describe(s, monitor.AutoRatio(), true) + "\n").c_str());
			Assert::IsTrue(s.notRead.empty(), ExtendedRangeGpuProbe::ToWide("not read: " + s.notRead).c_str());
			Assert::IsTrue(s.reading, L"the DV intensity plane is read");
			Assert::AreEqual(0.5, s.declaredRatio, 0.01, L"declared: the 540-line source");

			// Re-base to 540 lines: both IPT planes halved, the DV representation kept.
			frame.planes[0].texture = first;
			SourceRebase::Module rebase;
			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, frame, 0.5, rebased), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::IsTrue(rebased.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && rebased.repr.dovi == &dovi, L"DV decode kept");
			Assert::AreEqual(W / 2, rebased.planes[0].texture->params.w);
			Assert::AreEqual(H / 2, rebased.planes[0].texture->params.h);
			Assert::AreEqual(W / 4, rebased.planes[1].texture->params.w);
			Assert::AreEqual(H / 2, rebased.planes[1].texture->params.h);
			rebase.Release(gpu);
			monitor.Release(gpu);
			pl_tex_destroy(gpu, &first);
			pl_tex_destroy(gpu, &chroma);
			pl_dispatch_destroy(&dispatch);
		}
	};
}
