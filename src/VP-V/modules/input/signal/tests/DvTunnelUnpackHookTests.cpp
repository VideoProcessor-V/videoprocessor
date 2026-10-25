// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(DvTunnelUnpackHookTests)
	{
	public:


		TEST_METHOD(GpuDolbyVisionTunnelUnpackMatchesTheCpuUnpack)
		{
			// dv_tunnel_gpu_unpack (owner 2026-10-16): the Dolby Vision tunnel's
			// 12-bit IPT 4:2:2 planes unpacked on the GPU from the raw r210 frame
			// equal CR210DolbyVisionTunnelFormatter's planes sample for sample,
			// for both RGB8 placements, several widths (row padding filled, every
			// pixel after the DM packet an arbitrary 32-bit word), in the r210,
			// R10b and R10l containers (each against the CPU's own decode of that
			// container, R10 padding bits set), from the frame
			// and from an already-uploaded texture (EarlyUpload). Refused geometry
			// keeps the CPU unpack. Runs on WARP by default (no GPU needed; the
			// fixture's default device), on the GPU with VP_TEST_GPU=hardware (adds
			// 3840 and the 4K timing report). VP_TEST_DV_WARP=1 is accepted (now the
			// default).
			auto envIs = [](const char* name, const char* want)
			{
				char* v = nullptr;
				size_t n = 0;
				const bool on = _dupenv_s(&v, &n, name) == 0 && v && std::string(v) == want;
				std::free(v);
				return on;
			};
			const bool hardware = envIs("VP_TEST_GPU", "hardware");
			Logger::WriteMessage(hardware ? L"DV tunnel GPU unpack: on the hardware GPU\n" : L"DV tunnel GPU unpack: on WARP\n");
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuDolbyVisionTunnelUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex planes[2] = { nullptr, nullptr };
			uint32_t seed = 0x6a09e667u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(rgba8);
			int cases = 0;
			std::vector<unsigned> widths = { 128U, 130U, 134U, 1922U };
			if (hardware)
				widths.push_back(3840U);
			for (const VideoFrameEncoding container : { VideoFrameEncoding::R210, VideoFrameEncoding::R10b,
				VideoFrameEncoding::R10l })
			for (const bool high8 : { false, true })
				for (const unsigned width : widths)
				{
					const unsigned height = width >= 3840 ? 2160U : width > 1000 ? 1081U : 100U;
					Tests::DvSender::SenderPicture picture(high8, width, height, container, 24000, 1001, false, 2);
					picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
					const size_t rowBytes = picture.state->BytesPerRow();
					for (size_t row = 0; row < height; ++row)
						for (size_t byte = 0; byte + 4 <= rowBytes; byte += 4)
						{
							if (byte / 4 < width && row * width + byte / 4 < 3072)
								continue;
							const uint32_t word = next();
							std::memcpy(picture.input.data() + row * rowBytes + byte, &word, 4);
						}
					const VideoFrame frame(picture.input.data(), 1, 0, nullptr);
					CR210DolbyVisionTunnelFormatter cpu;
					cpu.OnVideoState(picture.state);
					std::vector<BYTE> expected(static_cast<size_t>(cpu.GetOutFrameSize()));
					Assert::IsTrue(cpu.FormatVideoFrame(frame, expected.data()), L"CPU unpack failed");
					Assert::IsTrue(cpu.GetDolbyVisionTunnelMetadata().valid, L"the sender picture did not lock");
					CR210DolbyVisionTunnelFormatter analyzed;
					analyzed.OnVideoState(picture.state);
					Assert::IsTrue(analyzed.AnalyzeFrame(frame) == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE);
					Assert::AreEqual(high8, analyzed.PlanesHighAligned());
					const std::wstring name = std::wstring(container == VideoFrameEncoding::R210 ? L"r210 " :
						container == VideoFrameEncoding::R10b ? L"R10b " : L"R10l ") +
						(high8 ? L"high8 " : L"low8 ") + std::to_wstring(width);
					const uint16_t* want = reinterpret_cast<const uint16_t*>(expected.data());
					const size_t planePixels = static_cast<size_t>(width) * height;
					auto compare = [&](const wchar_t* route)
					{
						const std::vector<uint16_t> gotY = download(planes[0]), gotC = download(planes[1]);
						Assert::AreEqual(planePixels, gotY.size(), (name + L": intensity plane size").c_str());
						Assert::AreEqual(planePixels, gotC.size(), (name + L": chroma plane size").c_str());
						for (size_t i = 0; i < planePixels; ++i)
						{
							if (gotY[i] != want[i])
								Assert::Fail((name + L" " + route + L": intensity differs at (" + std::to_wstring(i % width) + L", " +
									std::to_wstring(i / width) + L"): " + std::to_wstring(gotY[i]) + L" vs " + std::to_wstring(want[i])).c_str());
							if (gotC[i] != want[planePixels + i])
								Assert::Fail((name + L" " + route + L": chroma differs at (" + std::to_wstring(i % width) + L", " +
									std::to_wstring(i / width) + L"): " + std::to_wstring(gotC[i]) + L" vs " +
									std::to_wstring(want[planePixels + i])).c_str());
						}
					};
					Assert::IsTrue(unpack.Unpack(container, picture.input.data(), rowBytes, static_cast<int>(width),
						static_cast<int>(height), analyzed.PlanesHighAligned(), planes),
						(name + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					compare(L"frame");
					// The early-upload route: the raw frame already on the GPU as rgba8.
					pl_tex_params rawParams{};
					rawParams.w = static_cast<int>(rowBytes / 4);
					rawParams.h = static_cast<int>(height);
					rawParams.format = rgba8;
					rawParams.sampleable = true;
					rawParams.host_writable = true;
					pl_tex early = pl_tex_create(gpu, &rawParams);
					Assert::IsNotNull(early);
					pl_tex_transfer_params up{};
					up.tex = early;
					up.ptr = picture.input.data();
					up.row_pitch = rowBytes;
					Assert::IsTrue(pl_tex_upload(gpu, &up));
					Assert::IsTrue(unpack.Unpack(container, nullptr, rowBytes, static_cast<int>(width), static_cast<int>(height),
						analyzed.PlanesHighAligned(), planes, early), (name + L" early: " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					compare(L"early");
					pl_tex_destroy(gpu, &early);
					++cases;
				}
			// Refused geometry: the renderer keeps the CPU unpack.
			std::vector<BYTE> tiny(1024 * 8);
			const VideoFrameEncoding r210 = VideoFrameEncoding::R210;
			Assert::IsFalse(unpack.Unpack(r210, tiny.data(), 1024, 129, 4, false, planes), L"an odd width must be refused");
			Assert::IsFalse(unpack.Unpack(r210, tiny.data(), 508, 128, 4, false, planes), L"a row shorter than the frame must be refused");
			Assert::IsFalse(unpack.Unpack(r210, nullptr, 1024, 128, 4, false, planes), L"no frame must be refused");
			Assert::IsFalse(unpack.Unpack(VideoFrameEncoding::V210, tiny.data(), 1024, 128, 4, false, planes),
				L"only the tunnel containers take this pass");
			// Efficiency (report, hardware): 3840x2160, the CPU unpack it replaces
			// against AnalyzeFrame (the CPU's part now) and the raw upload + GPU
			// pass (wall, GPU drained) and the pass alone (timer).
			if (hardware)
			{
				Tests::DvSender::SenderPicture picture(true, 3840, 2160);
				picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
				CR210DolbyVisionTunnelFormatter cpu, analyzed;
				cpu.OnVideoState(picture.state);
				analyzed.OnVideoState(picture.state);
				std::vector<BYTE> out(static_cast<size_t>(cpu.GetOutFrameSize()));
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				double cpuMs = 1e30, analyzeMs = 1e30, gpuWallMs = 1e30, passMs = 1e30;
				for (int run = 0; run < 8; ++run)
				{
					const VideoFrame frame(picture.input.data(), static_cast<uint64_t>(run + 1), 0, nullptr);
					auto t0 = std::chrono::steady_clock::now();
					cpu.FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					t0 = std::chrono::steady_clock::now();
					analyzed.AnalyzeFrame(frame);
					analyzeMs = std::min(analyzeMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, picture.input.data(), static_cast<size_t>(picture.state->BytesPerRow()), 3840, 2160,
						true, planes));
					pl_gpu_finish(gpu);
					gpuWallMs = std::min(gpuWallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					const uint64_t ns = pl_timer_query(gpu, timer);
					if (ns > 0)
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				Logger::WriteMessage(("GpuDolbyVisionTunnelUnpack 4K: CPU unpack " + std::to_string(cpuMs) + " ms; AnalyzeFrame " +
					std::to_string(analyzeMs) + " ms; raw upload + GPU pass " + std::to_string(gpuWallMs) + " ms (pass alone " +
					std::to_string(passMs) + " ms)\n").c_str());
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("GpuDolbyVisionTunnelUnpack: " + std::to_string(cases) + " cases matched the CPU unpack\n").c_str());
		}

		// Synthetic DV tunnel validation (test/dv-synthetic, 2026-10-24): the GPU unpack on the
		// GPU (not its transcription) against the planes an independent Python implementation
		// of the tunnel wrote (tools/dv_synth/make_corpus.py, VP_TEST_DV_SYNTH_CORPUS): every
		// accuracy segment (r210 / R10b / R10l, low8 / high8 / high8 with replicated bits,
		// 1080p and 2160p), bit-exact; the 4K pass timed (hardware). WARP by default.
		TEST_METHOD(GpuDolbyVisionTunnelUnpackMatchesTheSyntheticCorpus)
		{
			char* rootValue = nullptr;
			size_t rootLength = 0;
			const std::string root = _dupenv_s(&rootValue, &rootLength, "VP_TEST_DV_SYNTH_CORPUS") == 0 && rootValue ? rootValue : "";
			std::free(rootValue);
			if (root.empty())
			{
				Logger::WriteMessage("SKIP-NO-CORPUS GpuDolbyVisionTunnelUnpackMatchesTheSyntheticCorpus (VP_TEST_DV_SYNTH_CORPUS)\n");
				return;
			}
			char* gpuValue = nullptr;
			size_t gpuLength = 0;
			const bool hardware = _dupenv_s(&gpuValue, &gpuLength, "VP_TEST_GPU") == 0 && gpuValue && std::string(gpuValue) == "hardware";
			std::free(gpuValue);
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuDolbyVisionTunnelUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex planes[2] = { nullptr, nullptr };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			size_t frames = 0;
			for (const std::string& folder : FindRawCorpusSegments(root))
			{
				const std::string name = folder.substr(folder.find_last_of("\\/") + 1);
				if (name.compare(0, 4, "acc-") != 0)
					continue;
				RawCorpusSegment seg;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), ExtendedRangeGpuProbe::ToWide(error).c_str());
				if (!hardware && seg.width > 2000)
					continue;   // WARP: 1080p only
				std::ifstream truth(folder + "\\truth.txt");
				std::string line, placement;
				std::map<std::string, std::string> planesOf;
				std::vector<std::pair<size_t, std::string>> framePicture;
				while (std::getline(truth, line))
				{
					auto value = [&line](const char* key) {
						const std::string k = std::string(" ") + key + "=";
						const size_t at = line.find(k);
						if (at == std::string::npos) return std::string();
						const size_t end = line.find(' ', at + k.size());
						return line.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size()); };
					if (line.compare(0, 8, "segment ") == 0) placement = value("placement");
					else if (line.compare(0, 8, "picture ") == 0) planesOf[value("id")] = value("planes");
					else if (line.compare(0, 6, "frame ") == 0)
						framePicture.push_back({ static_cast<size_t>(std::atoi(value("index").c_str())), value("picture") });
				}
				const bool highAligned = placement != "low8";
				const size_t n = static_cast<size_t>(seg.width) * seg.height;
				uint64_t bad = 0;
				double passMs = 1e30, wallMs = 1e30;
				pl_timer timer = hardware ? pl_timer_create(gpu) : nullptr;
				unpack.SetTimerForTest(timer);
				for (const auto& fp : framePicture)
				{
					const std::string planesFile = planesOf[fp.second];
					if (planesFile.empty() || planesFile == "-")
						continue;
					std::ifstream in(folder + "\\" + planesFile, std::ios::binary);
					std::vector<uint16_t> want(n * 2);
					in.read(reinterpret_cast<char*>(want.data()), static_cast<std::streamsize>(want.size() * 2));
					Assert::IsTrue(static_cast<bool>(in), L"planes file");
					std::vector<uint8_t> bytes;
					Assert::IsTrue(seg.LoadFrame(fp.first, bytes));
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(seg.encoding, bytes.data(), seg.rowBytes, static_cast<int>(seg.width), static_cast<int>(seg.height),
						highAligned, planes), ExtendedRangeGpuProbe::ToWide(name + ": " + unpack.Error()).c_str());
					pl_gpu_finish(gpu);
					wallMs = std::min(wallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					if (timer)
					{
						const uint64_t ns = pl_timer_query(gpu, timer);
						if (ns > 0) passMs = std::min(passMs, ns / 1e6);
					}
					const std::vector<uint16_t> gotY = download(planes[0]), gotC = download(planes[1]);
					Assert::AreEqual(n, gotY.size());
					for (size_t k = 0; k < n; ++k)
						bad += (gotY[k] != want[k] ? 1 : 0) + (gotC[k] != want[n + k] ? 1 : 0);
					++frames;
				}
				unpack.SetTimerForTest(nullptr);
				if (timer) pl_timer_destroy(gpu, &timer);
				char text[512];
				snprintf(text, sizeof(text), "GPU UNPACK %s (%ux%u %s): mismatching samples %llu; raw upload + pass %.2f ms, pass alone %.3f ms (%s)\n",
					name.c_str(), seg.width, seg.height, placement.c_str(), (unsigned long long)bad, wallMs, passMs < 1e29 ? passMs : -1.0,
					hardware ? "hardware" : "WARP");
				Logger::WriteMessage(text);
				Assert::AreEqual<uint64_t>(0, bad, ExtendedRangeGpuProbe::ToWide(name + ": the GPU unpack differs from the generator's planes").c_str());
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			AssertCleanLibplaceboLog(log);
			Assert::IsTrue(frames > 0, L"no corpus frame checked");
		}

		// Rule test (2026-10-17): Dolby Vision GPU unpack with a lost / corrupt DM packet
		// presents black, never garbage. A tunnel stream through the renderer's route
		// (LibplaceboVideoRenderer's dvRaw path: AnalyzeFrame decides, PICTURE ->
		// GpuDolbyVisionTunnelUnpack, otherwise zero planes) on WARP: a locked picture
		// unpacks on the GPU equal to the CPU; with every copy of the DM header damaged
		// the route reaches BLACK within the formatter's loss limit and the planes the
		// renderer receives are all zero (the CPU formatter agrees); the packed pixels
		// would not be black (the gate matters); a fresh DM brings the GPU picture
		// back. A stream that starts with a damaged DM is black from its first frame.
		TEST_METHOD(GpuDolbyVisionDmLossPresentsBlackNotGarbage)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuDolbyVisionTunnelUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex planes[2] = { nullptr, nullptr };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			const unsigned width = 130, height = 128;
			Tests::DvSender::SenderPicture picture(true, width, height);
			picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
			Tests::DvSender::SenderPicture damaged = picture;
			damaged.CorruptAllCopies(0);
			const size_t rowBytes = picture.state->BytesPerRow();
			const size_t planePixels = static_cast<size_t>(width) * height;
			// The renderer's route for one frame; returns the planes it would render.
			struct Routed { CR210DolbyVisionTunnelFormatter::FrameResult result; std::vector<uint16_t> intensity, chroma; };
			auto route = [&](CR210DolbyVisionTunnelFormatter& analyzed, CR210DolbyVisionTunnelFormatter& cpu,
				const std::vector<BYTE>& input, uint64_t counter, const wchar_t* name)
			{
				Routed out;
				const VideoFrame frame(const_cast<BYTE*>(input.data()), counter, 0, nullptr);
				out.result = analyzed.AnalyzeFrame(frame);
				Assert::IsTrue(out.result != CR210DolbyVisionTunnelFormatter::FrameResult::REJECTED,
					(std::wstring(name) + L": frame rejected").c_str());
				std::vector<BYTE> expected(static_cast<size_t>(cpu.GetOutFrameSize()));
				Assert::IsTrue(cpu.FormatVideoFrame(frame, expected.data()), (std::wstring(name) + L": CPU unpack failed").c_str());
				const uint16_t* want = reinterpret_cast<const uint16_t*>(expected.data());
				if (out.result == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE)
				{
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, input.data(), rowBytes, static_cast<int>(width),
						static_cast<int>(height), analyzed.PlanesHighAligned(), planes),
						(std::wstring(name) + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					out.intensity = download(planes[0]);
					out.chroma = download(planes[1]);
					Assert::AreEqual(planePixels, out.intensity.size());
					for (size_t i = 0; i < planePixels; ++i)
						Assert::IsTrue(out.intensity[i] == want[i] && out.chroma[i] == want[planePixels + i],
							(std::wstring(name) + L": the GPU picture differs from the CPU at " + std::to_wstring(i)).c_str());
				}
				else
				{
					// No trusted DM: the renderer hands on zero planes (never the packed bytes).
					out.intensity.assign(planePixels, 0);
					out.chroma.assign(planePixels, 0);
					for (size_t i = 0; i < 2 * planePixels; ++i)
						Assert::AreEqual(0, static_cast<int>(want[i]), (std::wstring(name) + L": the CPU formatter is not black").c_str());
				}
				return out;
			};
			auto black = [](const Routed& r)
			{
				for (uint16_t v : r.intensity) if (v != 0) return false;
				for (uint16_t v : r.chroma) if (v != 0) return false;
				return true;
			};
			// The packed pixels of the damaged frame, unpacked anyway: not black (what the route
			// must never show).
			{
				CR210DolbyVisionTunnelFormatter probe;
				probe.OnVideoState(picture.state);
				const VideoFrame frame(picture.input.data(), 1, 0, nullptr);
				Assert::IsTrue(probe.AnalyzeFrame(frame) == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE);
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, damaged.input.data(), rowBytes, static_cast<int>(width),
					static_cast<int>(height), probe.PlanesHighAligned(), planes));
				const std::vector<uint16_t> garbage = download(planes[0]);
				size_t nonZero = 0;
				for (uint16_t v : garbage) nonZero += v != 0 ? 1 : 0;
				Assert::IsTrue(nonZero > planePixels / 2, L"precondition: the damaged frame's packed pixels are not black");
			}
			// Locked, then the DM lost, then a fresh DM.
			CR210DolbyVisionTunnelFormatter analyzed, cpu;
			analyzed.OnVideoState(picture.state);
			cpu.OnVideoState(picture.state);
			uint64_t counter = 1;
			Assert::IsTrue(route(analyzed, cpu, picture.input, counter++, L"locked").result ==
				CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE, L"the sender picture locks");
			int blackAt = -1;
			for (int lost = 0; lost < 60 && blackAt < 0; ++lost)
			{
				const Routed r = route(analyzed, cpu, damaged.input, counter++, L"DM lost");
				if (r.result == CR210DolbyVisionTunnelFormatter::FrameResult::BLACK)
				{
					Assert::IsTrue(black(r), L"DM lost: black planes");
					blackAt = lost;
				}
			}
			Logger::WriteMessage(("DM loss: black after " + std::to_string(blackAt + 1) + " damaged frame(s)\n").c_str());
			Assert::IsTrue(blackAt >= 0, L"a lasting DM loss reaches black within the loss limit");
			Assert::IsFalse(analyzed.GetDolbyVisionTunnelMetadata().valid || analyzed.GetDolbyVisionTunnelMetadata().allowSdrFallback,
				L"after DV lock a lost DM is neither trusted nor allowed the SDR passthrough (the packed pixels)");
			// Still lost: stays black.
			for (int more = 0; more < 5; ++more)
				Assert::IsTrue(black(route(analyzed, cpu, damaged.input, counter++, L"still lost")), L"stays black while the DM is lost");
			// A fresh, complete DM (a new metadata ID, as a sender restarts): the GPU picture again.
			Tests::DvSender::SenderPicture fresh = picture;
			fresh.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody(2048)));
			bool restored = false;
			for (int k = 0; k < 10 && !restored; ++k)
				restored = route(analyzed, cpu, fresh.input, counter++, L"fresh DM").result ==
					CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE;
			Assert::IsTrue(restored, L"a fresh DM brings the GPU picture back");
			// A stream whose first frame already has a damaged DM: zero DV planes from the start (before
			// any lock the renderer may still choose the SDR passthrough - allowSdrFallback, reported).
			{
				CR210DolbyVisionTunnelFormatter coldAnalyzed, coldCpu;
				coldAnalyzed.OnVideoState(picture.state);
				coldCpu.OnVideoState(picture.state);
				for (uint64_t n = 1; n <= 3; ++n)
				{
					const Routed r = route(coldAnalyzed, coldCpu, damaged.input, n, L"never locked");
					Assert::IsTrue(r.result == CR210DolbyVisionTunnelFormatter::FrameResult::BLACK && black(r),
						L"never locked with a damaged DM: zero planes, not the packed pixels");
				}
				Logger::WriteMessage(coldAnalyzed.GetDolbyVisionTunnelMetadata().allowSdrFallback ?
					"never locked: SDR passthrough allowed (no DV lock yet)\n" : "never locked: SDR passthrough not allowed\n");
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
