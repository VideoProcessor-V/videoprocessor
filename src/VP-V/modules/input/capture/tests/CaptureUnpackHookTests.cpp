// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(CaptureUnpackHookTests)
	{
	public:


		TEST_METHOD(EarlyUploadMatchesTheRenderUploadAndNeverReusesBusySlots)
		{
			// Option A of the pinned-memory work (owner 2026-10-15): a frame
			// uploaded early on a second device (EarlyUpload) unpacks to exactly
			// the planes of today's upload (V210 and R12B); a held slot is never
			// handed out again; a slot comes back once its handle is gone and its
			// GPU uses are released; the byte cap is honoured.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			EarlyUpload early;
			if (!early.Open(fixture.Device(), gpu, 2))
			{
				Logger::WriteMessage(("EarlyUpload not available on this device: " + early.LastError() + "\n").c_str());
				return;   // the renderer then keeps the normal upload
			}
			uint32_t seed = 0xabcdef1u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h * tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			// V210: the early-uploaded frame against today's pointer upload.
			{
				GpuCaptureUnpack unpack(fixture.Log(), gpu);
				unpack.SetHostReadableForTest(true);
				const int width = 1920, height = 100;
				const size_t rowBytes = static_cast<size_t>((width + 47) / 48) * 128;
				std::vector<uint8_t> frame(rowBytes * height);
				for (uint8_t& b : frame) b = static_cast<uint8_t>(next());
				pl_tex normal[2] = { nullptr, nullptr }, viaEarly[2] = { nullptr, nullptr };
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::V210, frame.data(), rowBytes, width, height, normal));
				const std::vector<uint16_t> wantY = download(normal[0]), wantUV = download(normal[1]);
				EarlyUpload::SlotHandle slot = early.Upload(frame.data(), rowBytes, height);
				Assert::IsTrue(slot != nullptr, L"V210: a free slot");
				pl_tex uploaded = early.Acquire(*slot);
				Assert::IsNotNull(uploaded, ExtendedRangeGpuProbe::ToWide(early.LastError()).c_str());
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::V210, nullptr, rowBytes, width, height, viaEarly, uploaded),
					ExtendedRangeGpuProbe::ToWide(unpack.Error()).c_str());
				early.Used(*slot);
				Assert::IsTrue(download(viaEarly[0]) == wantY && download(viaEarly[1]) == wantUV, L"V210: early upload differs");
				for (pl_tex* t : { &normal[0], &normal[1], &viaEarly[0], &viaEarly[1] }) pl_tex_destroy(gpu, t);
			}
			// R12B through GpuRgbUnpack, and the slot rules.
			{
				GpuRgbUnpack unpack(fixture.Log(), gpu);
				unpack.SetHostReadableForTest(true);
				const int width = 1920, height = 100;
				const size_t rowBytes = static_cast<size_t>(width) * 36 / 8;
				std::vector<uint8_t> frames[3];
				for (auto& f : frames) { f.resize(rowBytes * height); for (uint8_t& b : f) b = static_cast<uint8_t>(next()); }
				auto expected = [&](const std::vector<uint8_t>& f)
				{
					pl_tex rgb = nullptr, luma = nullptr;
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R12B, false, f.data(), rowBytes, width, height, rgb, luma));
					std::pair<std::vector<uint16_t>, std::vector<uint16_t>> out(download(rgb), download(luma));
					pl_tex_destroy(gpu, &rgb);
					pl_tex_destroy(gpu, &luma);
					return out;
				};
				auto viaEarly = [&](EarlyUpload::SlotHandle& s)
				{
					pl_tex rgb = nullptr, luma = nullptr;
					pl_tex uploaded = early.Acquire(*s);
					Assert::IsNotNull(uploaded);
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R12B, false, nullptr, rowBytes, width, height, rgb, luma, false, uploaded));
					early.Used(*s);
					std::pair<std::vector<uint16_t>, std::vector<uint16_t>> out(download(rgb), download(luma));
					pl_tex_destroy(gpu, &rgb);
					pl_tex_destroy(gpu, &luma);
					return out;
				};
				EarlyUpload::SlotHandle a = early.Upload(frames[0].data(), rowBytes, height);
				EarlyUpload::SlotHandle b = early.Upload(frames[1].data(), rowBytes, height);
				Assert::IsTrue(a && b, L"two slots");
				Assert::IsTrue(early.Upload(frames[2].data(), rowBytes, height) == nullptr, L"both slots held: none handed out");
				Assert::IsTrue(viaEarly(a) == expected(frames[0]), L"R12B slot A differs");
				Assert::IsTrue(viaEarly(b) == expected(frames[1]), L"R12B slot B differs");
				a.reset();   // its payload gone; its GPU use released once the GPU passes it
				pl_gpu_finish(gpu);
				EarlyUpload::SlotHandle c = early.Upload(frames[2].data(), rowBytes, height);
				Assert::IsTrue(c != nullptr, L"a released slot comes back");
				Assert::IsTrue(viaEarly(c) == expected(frames[2]), L"R12B reused slot differs");
				Assert::IsTrue(viaEarly(b) == expected(frames[1]), L"R12B slot B still intact after the reuse");
			}
			const EarlyUpload::Stats stats = early.GetStats();
			Assert::IsTrue(stats.noFreeSlot >= 1 && stats.failures == 0, L"stats");
			early.Close();
			// The cap: a pool too small for one frame hands nothing out.
			EarlyUpload tiny;
			Assert::IsTrue(tiny.Open(fixture.Device(), gpu, 2, 1024));
			std::vector<uint8_t> f(1920 * 4 * 100);
			Assert::IsTrue(tiny.Upload(f.data(), 1920 * 4, 100) == nullptr, L"over the cap: nothing");
			tiny.Close();
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("EarlyUpload: " + std::to_string(stats.uploads) + " uploads, " +
				std::to_string(stats.noFreeSlot) + " with no free slot, " + std::to_string(stats.slotsMade) + " textures\n").c_str());
		}

		TEST_METHOD(PinnedUploadCapabilitiesAndTiming)
		{
			// Pinned memory (roadmap, owner 2026-10-15): what this GPU's libplacebo
			// D3D11 backend offers for host-mapped upload buffers, and a 4K raw
			// frame's upload timed three ways (report; nothing asserted beyond
			// the uploads succeeding and matching):
			//   pointer      pl_tex_upload from a plain pointer (today)
			//   mapped host  a host-mapped pl_buf (PL_BUF_MEM_HOST): the copy into
			//                it timed apart, then pl_tex_upload from the buffer
			//   mapped vram  the same from PL_BUF_MEM_DEVICE when offered
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const pl_gpu_limits& lim = gpu->limits;
			char line[512];
			std::snprintf(line, sizeof(line),
				"Pinned upload: thread_safe %d callbacks %d buf_transfer %d max_buf %zu MB max_mapped %zu MB max_mapped_vram %zu MB host_cached %d align_host_ptr %zu\n",
				lim.thread_safe, lim.callbacks, lim.buf_transfer, lim.max_buf_size >> 20, lim.max_mapped_size >> 20,
				lim.max_mapped_vram >> 20, lim.host_cached, lim.align_host_ptr);
			Logger::WriteMessage(line);
			const int width = 3840, height = 2160;
			const size_t rowBytes = static_cast<size_t>(width) * 36 / 8;   // R12B, the largest capture format
			const size_t bytes = rowBytes * height;
			std::vector<uint8_t> frame(bytes);
			uint32_t seed = 0x12345u;
			for (uint8_t& b : frame) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
			pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(rgba8);
			pl_tex_params tp{};
			tp.w = static_cast<int>(rowBytes / 4);
			tp.h = height;
			tp.format = rgba8;
			tp.sampleable = true;
			tp.host_writable = true;
			tp.host_readable = true;
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
			{ return std::chrono::duration<double, std::milli>(b - a).count(); };
			auto check = [&](const wchar_t* what)
			{
				std::vector<uint8_t> back(bytes);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = back.data();
				d.row_pitch = rowBytes;
				Assert::IsTrue(pl_tex_download(gpu, &d), what);
				Assert::IsTrue(back == frame, what);
			};
			double pointerMs = 1e30, pointerCallMs = 1e30;
			for (int run = 0; run < 6; ++run)
			{
				pl_gpu_finish(gpu);
				const auto t0 = std::chrono::steady_clock::now();
				pl_tex_transfer_params u{};
				u.tex = tex;
				u.ptr = frame.data();
				u.row_pitch = rowBytes;
				Assert::IsTrue(pl_tex_upload(gpu, &u));
				const auto tCall = std::chrono::steady_clock::now();
				pl_gpu_finish(gpu);
				pointerMs = std::min(pointerMs, ms(t0, std::chrono::steady_clock::now()));
				pointerCallMs = std::min(pointerCallMs, ms(t0, tCall));
			}
			check(L"pointer upload");
			std::snprintf(line, sizeof(line), "Pinned upload 4K R12B (%zu MB) pointer upload: the call blocks %.2f ms, with the GPU transfer %.2f ms (%.1f GB/s)\n",
				bytes >> 20, pointerCallMs, pointerMs, bytes / (pointerMs * 1e6));
			Logger::WriteMessage(line);
			for (const enum pl_buf_mem_type mem : { PL_BUF_MEM_HOST, PL_BUF_MEM_DEVICE })
			{
				const char* name = mem == PL_BUF_MEM_HOST ? "mapped host" : "mapped vram";
				pl_buf_params bp{};
				bp.size = bytes;
				bp.host_mapped = true;
				bp.memory_type = mem;
				pl_buf buf = (lim.max_mapped_size >= bytes && (mem == PL_BUF_MEM_HOST || lim.max_mapped_vram >= bytes)) ?
					pl_buf_create(gpu, &bp) : nullptr;
				if (!buf || !buf->data)
				{
					std::snprintf(line, sizeof(line), "Pinned upload: %s buffer of %zu MB not available\n", name, bytes >> 20);
					Logger::WriteMessage(line);
					pl_buf_destroy(gpu, &buf);
					continue;
				}
				double copyMs = 1e30, uploadMs = 1e30;
				for (int run = 0; run < 6; ++run)
				{
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					std::memcpy(buf->data, frame.data(), bytes);   // the card would write here directly
					const auto t1 = std::chrono::steady_clock::now();
					pl_tex_transfer_params u{};
					u.tex = tex;
					u.buf = buf;
					u.row_pitch = rowBytes;
					Assert::IsTrue(pl_tex_upload(gpu, &u));
					pl_gpu_finish(gpu);
					const auto t2 = std::chrono::steady_clock::now();
					copyMs = std::min(copyMs, ms(t0, t1));
					uploadMs = std::min(uploadMs, ms(t1, t2));
				}
				check(mem == PL_BUF_MEM_HOST ? L"mapped host upload" : L"mapped vram upload");
				// Non-blocking reuse: right after an upload is queued, is the
				// buffer reported busy, and free once the GPU is done?
				pl_tex_transfer_params u{};
				u.tex = tex;
				u.buf = buf;
				u.row_pitch = rowBytes;
				pl_tex_upload(gpu, &u);
				const bool busyAfterQueue = pl_buf_poll(gpu, buf, 0);
				pl_gpu_finish(gpu);
				const bool busyAfterFinish = pl_buf_poll(gpu, buf, 0);
				std::snprintf(line, sizeof(line),
					"Pinned upload 4K R12B (%zu MB): %s copy-in %.2f ms + upload from buffer %.2f ms (pointer upload %.2f ms); busy after queue %d, after finish %d\n",
					bytes >> 20, name, copyMs, uploadMs, pointerMs, busyAfterQueue, busyAfterFinish);
				Logger::WriteMessage(line);
				pl_buf_destroy(gpu, &buf);
			}
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(GpuRgbUnpackMatchesTheCpuDecode)
		{
			// Roadmap item 2 (owner 2026-10-15): native ingest of R12B, R12L and
			// r210. With random bytes (row padding included): the luma equals the
			// CPU formatter's P010 Y plane sample for sample (BT.709 and BT.2020;
			// the matcher sees what it saw on the converted path), and the RGB
			// texture holds each pixel decoded independently here (R12: the
			// 12-bit value << 4; r210: 64-960 mapped to 16-bit limited levels,
			// below-black and above-white kept). Widths at several packings;
			// geometry the pass cannot take (an R12 width not a multiple of 8)
			// is refused, so the renderer keeps the CPU conversion.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuRgbUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex rgb = nullptr, luma = nullptr;
			uint32_t seed = 0x9e3779b9u;
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
			// An independent decode of pixel x of a row (SMPTE 268M C4 for R12;
			// big-endian [00 R G B] for r210).
			auto decode = [](VideoFrameEncoding encoding, const BYTE* row, int x, int rgbOut[3])
			{
				if (encoding == VideoFrameEncoding::R210)
				{
					const uint32_t w = (uint32_t(row[4 * x]) << 24) | (uint32_t(row[4 * x + 1]) << 16) |
						(uint32_t(row[4 * x + 2]) << 8) | row[4 * x + 3];
					rgbOut[0] = int((w >> 20) & 1023u);
					rgbOut[1] = int((w >> 10) & 1023u);
					rgbOut[2] = int(w & 1023u);
					return;
				}
				if (encoding == VideoFrameEncoding::ARGB_8BIT || encoding == VideoFrameEncoding::BGRA_8BIT)
				{
					const BYTE* q = row + 4 * x;
					const bool argb = encoding == VideoFrameEncoding::ARGB_8BIT;
					rgbOut[0] = argb ? q[1] : q[2];
					rgbOut[1] = q[argb ? 2 : 1];
					rgbOut[2] = argb ? q[3] : q[0];
					return;
				}
				if (encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l)
				{
					const BYTE* q = row + 4 * x;
					const uint32_t w = encoding == VideoFrameEncoding::R10b ?
						(uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3] :
						(uint32_t(q[3]) << 24) | (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0];
					rgbOut[0] = int((w >> 22) & 1023u);
					rgbOut[1] = int((w >> 12) & 1023u);
					rgbOut[2] = int((w >> 2) & 1023u);
					return;
				}
				// The pair's nine bytes in stream order; R12B reverses every
				// 32-bit word of its 36-byte block (stream byte k of the block
				// is at k ^ 3).
				const int pair = x / 2;
				BYTE b[9];
				for (int k = 0; k < 9; ++k)
				{
					const int stream = pair * 9 + k;
					b[k] = encoding == VideoFrameEncoding::R12B ? row[(stream / 36) * 36 + ((stream % 36) ^ 3)] : row[stream];
				}
				const int v[6] = { b[0] | ((b[1] & 15) << 8), (b[1] >> 4) | (b[2] << 4), b[3] | ((b[4] & 15) << 8),
					(b[4] >> 4) | (b[5] << 4), b[6] | ((b[7] & 15) << 8), (b[7] >> 4) | (b[8] << 4) };
				for (int c = 0; c < 3; ++c)
					rgbOut[c] = v[(x & 1) * 3 + c];
			};
			int cases = 0;
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R12B, VideoFrameEncoding::R12L, VideoFrameEncoding::R210,
				VideoFrameEncoding::R10b, VideoFrameEncoding::R10l, VideoFrameEncoding::ARGB_8BIT, VideoFrameEncoding::BGRA_8BIT })
				for (const ColorSpace space : { ColorSpace::REC_709, ColorSpace::BT_2020 })
					for (const int width : encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ?
						std::vector<int>{ 104, 136, 1920 } : std::vector<int>{ 120, 122, 130, 1920 })
					{
						const int height = 100;
						VideoStateComPtr state = new VideoState();
						state->valid = true;
						state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
						state->videoFrameEncoding = encoding;
						state->colorspace = space;
						const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
						std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
						for (BYTE& b : source) b = static_cast<BYTE>(next());
						std::unique_ptr<IVideoFrameFormatter> cpu;
						if (GpuRgbUnpack::Is8Bit(encoding)) cpu = std::make_unique<CARGBtoP010VideoFrameFormatter>();
						else cpu = std::make_unique<CDeckLinkRGBToP010VideoFrameFormatter>();
						cpu->OnVideoState(state);
						std::vector<BYTE> expected(static_cast<size_t>(cpu->GetOutFrameSize()));
						const VideoFrame frame(source.data(), 1, 0, nullptr);
						Assert::IsTrue(cpu->FormatVideoFrame(frame, expected.data()), L"CPU conversion failed");
						const std::wstring name = std::wstring(encoding == VideoFrameEncoding::R12B ? L"R12B " :
							encoding == VideoFrameEncoding::R12L ? L"R12L " : encoding == VideoFrameEncoding::R210 ? L"r210 " :
							encoding == VideoFrameEncoding::R10b ? L"R10b " : encoding == VideoFrameEncoding::R10l ? L"R10l " :
							encoding == VideoFrameEncoding::ARGB_8BIT ? L"ARGB " : L"BGRA ") +
							(space == ColorSpace::BT_2020 ? L"2020 " : L"709 ") + std::to_wstring(width);
						Assert::IsTrue(unpack.Unpack(encoding, space == ColorSpace::BT_2020, source.data(), rowBytes, width, height, rgb, luma),
							(name + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
						const std::vector<uint16_t> gotLuma = download(luma), gotRgb = download(rgb);
						// The luma-only mode (the de-judder's next / ahead frames): the
						// same luma, the picture untouched.
						{
							pl_tex lumaOnly = nullptr, untouched = nullptr;
							Assert::IsTrue(unpack.Unpack(encoding, space == ColorSpace::BT_2020, source.data(), rowBytes, width, height,
								untouched, lumaOnly, true), (name + L": luma-only unpack").c_str());
							Assert::IsTrue(untouched == nullptr, (name + L": luma-only made no picture").c_str());
							Assert::IsTrue(download(lumaOnly) == gotLuma, (name + L": luma-only luma differs").c_str());
							pl_tex_destroy(gpu, &lumaOnly);
						}
						const uint16_t* wantLuma = reinterpret_cast<const uint16_t*>(expected.data());
						for (int y = 0; y < height; ++y)
							for (int x = 0; x < width; ++x)
							{
								const size_t i = static_cast<size_t>(y) * width + x;
								if (gotLuma[i] != wantLuma[i])
									Assert::Fail((name + L": luma differs from the CPU formatter at (" + std::to_wstring(x) + L", " +
										std::to_wstring(y) + L"): " + std::to_wstring(gotLuma[i]) + L" vs " + std::to_wstring(wantLuma[i])).c_str());
								int v[3];
								decode(encoding, source.data() + static_cast<size_t>(y) * rowBytes, x, v);
								for (int c = 0; c < 3; ++c)
								{
									const int want = encoding == VideoFrameEncoding::R210 ?
										(4096 * 7 + (v[c] - 64) * 438 + 3) / 7 :
										encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l ? v[c] << 6 :
										GpuRgbUnpack::Is8Bit(encoding) ? v[c] << 8 : v[c] << 4;
									if (gotRgb[i * 4 + c] != want)
										Assert::Fail((name + L": RGB differs at (" + std::to_wstring(x) + L", " + std::to_wstring(y) +
											L") component " + std::to_wstring(c) + L": " + std::to_wstring(gotRgb[i * 4 + c]) + L" vs " +
											std::to_wstring(want)).c_str());
								}
							}
						++cases;
					}
			// Refused geometry: the renderer keeps the CPU conversion.
			std::vector<BYTE> narrow(1366 * 36 / 8 * 4 + 64);
			Assert::IsFalse(unpack.Unpack(VideoFrameEncoding::R12L, false, narrow.data(), 6148, 1366, 4, rgb, luma),
				L"an R12 width not a multiple of 8 must be refused");
			Assert::IsFalse(GpuRgbUnpack::Supports(VideoFrameEncoding::V210) || GpuRgbUnpack::Supports(VideoFrameEncoding::UYVY),
				L"only the RGB formats take this pass");
			// Efficiency (report): 3840x2160, the CPU conversion it replaces
			// against the raw upload + GPU pass (wall, GPU drained) and the pass
			// alone (timer).
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R12B, VideoFrameEncoding::R210, VideoFrameEncoding::ARGB_8BIT })
			{
				const int width = 3840, height = 2160;
				VideoStateComPtr state = new VideoState();
				state->valid = true;
				state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
				state->videoFrameEncoding = encoding;
				state->colorspace = ColorSpace::REC_709;
				std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
				for (BYTE& b : source) b = static_cast<BYTE>(next());
				std::unique_ptr<IVideoFrameFormatter> cpu;
				if (GpuRgbUnpack::Is8Bit(encoding)) cpu = std::make_unique<CARGBtoP010VideoFrameFormatter>();
				else cpu = std::make_unique<CDeckLinkRGBToP010VideoFrameFormatter>();
				cpu->OnVideoState(state);
				std::vector<BYTE> out(static_cast<size_t>(cpu->GetOutFrameSize()));
				const VideoFrame frame(source.data(), 1, 0, nullptr);
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				double cpuMs = 1e30, gpuWallMs = 1e30, passMs = 1e30;
				for (int run = 0; run < 8; ++run)
				{
					auto t0 = std::chrono::steady_clock::now();
					cpu->FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(encoding, false, source.data(), static_cast<size_t>(state->BytesPerRow()), width, height, rgb, luma));
					pl_gpu_finish(gpu);
					gpuWallMs = std::min(gpuWallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					const uint64_t ns = pl_timer_query(gpu, timer);
					if (ns > 0)
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				Logger::WriteMessage(("GpuRgbUnpack 4K " + std::string(encoding == VideoFrameEncoding::R12B ? "R12B" :
					encoding == VideoFrameEncoding::R210 ? "r210" : "ARGB") + ": CPU conversion " + std::to_string(cpuMs) +
					" ms; raw upload + GPU pass " + std::to_string(gpuWallMs) + " ms (pass alone " + std::to_string(passMs) + " ms)\n").c_str());
			}
			pl_tex_destroy(gpu, &rgb);
			pl_tex_destroy(gpu, &luma);
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("GpuRgbUnpack: " + std::to_string(cases) + " cases matched the CPU decode\n").c_str());
		}

		TEST_METHOD(GpuCaptureUnpackMatchesTheCpuFormatters)
		{
			std::string where = "setup";
			try {
			// capture_path: gpu (owner 2026-10-13): the GPU unpack of raw
			// DeckLink V210 / UYVY / HDYC equals the CPU formatters' P210 sample
			// for sample, at every V210 packing phase (widths 96, 98, 100 end a
			// line on pack phase 0, 2, 4: widths 120, 122, 124) and at 1920, with random bytes
			// (padding included). Then the GPU pass and the CPU conversion it
			// replaces are timed at 3840x2160 (report).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			where = "unpack object";
			GpuCaptureUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			where = "loop";
			pl_tex planes[2] = { nullptr, nullptr };
			uint32_t seed = 0x2545f491u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto makeState = [](VideoFrameEncoding encoding, int width, int height)
			{
				VideoStateComPtr state = new VideoState();
				state->valid = true;
				state->displayMode = std::make_shared<DisplayMode>(width, height, false, 60000, 1001);
				state->videoFrameEncoding = encoding;
				state->colorspace = ColorSpace::REC_709;
				return state;
			};
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
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY, VideoFrameEncoding::HDYC })
				for (const int width : { 120, 122, 124, 1920 })
				{
					const int height = 100;
					std::wstring stage = L"state";
					try {
					VideoStateComPtr state = makeState(encoding, width, height);
					const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
					std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
					for (BYTE& b : source) b = static_cast<BYTE>(next());
					std::unique_ptr<IVideoFrameFormatter> cpu;
					if (encoding == VideoFrameEncoding::V210) cpu = std::make_unique<CV210toP210VideoFrameFormatter>();
					else cpu = std::make_unique<CUYVYtoP210VideoFrameFormatter>();
					cpu->OnVideoState(state);
					std::vector<BYTE> expected(static_cast<size_t>(cpu->GetOutFrameSize()));
					const VideoFrame frame(source.data(), 1, 0, nullptr);
					Assert::IsTrue(cpu->FormatVideoFrame(frame, expected.data()));
					const std::wstring name = (encoding == VideoFrameEncoding::V210 ? L"V210 " :
						encoding == VideoFrameEncoding::UYVY ? L"UYVY " : L"HDYC ") + std::to_wstring(width);
					Assert::IsTrue(unpack.Unpack(encoding, source.data(), rowBytes, width, height, planes),
						(name + L": " + std::wstring(unpack.Error().begin(), unpack.Error().end()) + L"; log: " +
							std::wstring(log.begin(), log.end())).c_str());
					const auto contract = cpu->GetOutputContract(), gpuContract = GpuCaptureUnpack::Contract(encoding);
					Assert::IsTrue(contract.sampleRange == gpuContract.sampleRange && contract.colorDepth == gpuContract.colorDepth &&
						contract.bitShift == gpuContract.bitShift, (name + L": the contract").c_str());
					const std::vector<uint16_t> y = download(planes[0]), uv = download(planes[1]);
					const uint16_t* cpuY = reinterpret_cast<const uint16_t*>(expected.data());
					const uint16_t* cpuUv = cpuY + static_cast<size_t>(width) * height;
					size_t yDiff = 0, uvDiff = 0, first = SIZE_MAX;
					for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
						if (y[i] != cpuY[i]) { ++yDiff; first = std::min(first, i); }
					for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
						if (uv[i] != cpuUv[i]) { ++uvDiff; first = std::min(first, i); }
					Assert::IsTrue(yDiff == 0 && uvDiff == 0, (name + L": " + std::to_wstring(yDiff) + L" luma and " +
						std::to_wstring(uvDiff) + L" chroma samples differ (first at " + std::to_wstring(first) + L")").c_str());
					} catch (const std::exception& e) {
						const std::string what = e.what();
						Assert::Fail((L"width " + std::to_wstring(width) + L" encoding " + std::to_wstring(static_cast<int>(encoding)) +
							L": " + std::wstring(what.begin(), what.end())).c_str());
					}
				}
			// Efficiency (report): 3840x2160, the GPU pass (timer) and the CPU
			// conversion it replaces.
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY })
			{
				const int width = 3840, height = 2160;
				VideoStateComPtr state = makeState(encoding, width, height);
				std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
				for (BYTE& b : source) b = static_cast<BYTE>(next());
				std::unique_ptr<IVideoFrameFormatter> cpu;
				if (encoding == VideoFrameEncoding::V210) cpu = std::make_unique<CV210toP210VideoFrameFormatter>();
				else cpu = std::make_unique<CUYVYtoP210VideoFrameFormatter>();
				cpu->OnVideoState(state);
				std::vector<BYTE> out(static_cast<size_t>(cpu->GetOutFrameSize()));
				const VideoFrame frame(source.data(), 1, 0, nullptr);
				double cpuMs = 1e30, uploadAndPassMs = 1e30, passMs = 1e30, standardUploadMs = 1e30;
				pl_tex standard[2] = { nullptr, nullptr };
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				for (int run = 0; run < 8; ++run)
				{
					auto t0 = std::chrono::steady_clock::now();
					cpu->FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					// Today's path after the conversion: the P210 planes' upload (as RenderLocked).
					{
						pl_plane_data planesData[2]{};
						planesData[0].type = PL_FMT_UNORM;
						planesData[0].width = width;
						planesData[0].height = height;
						planesData[0].component_size[0] = 16;
						planesData[0].component_map[0] = PL_CHANNEL_Y;
						planesData[0].pixel_stride = sizeof(uint16_t);
						planesData[0].row_stride = static_cast<size_t>(width) * 2;
						planesData[0].pixels = out.data();
						planesData[1] = planesData[0];
						planesData[1].width = width / 2;
						planesData[1].component_size[1] = 16;
						planesData[1].component_map[0] = PL_CHANNEL_CB;
						planesData[1].component_map[1] = PL_CHANNEL_CR;
						planesData[1].pixel_stride = sizeof(uint16_t) * 2;
						planesData[1].pixels = out.data() + static_cast<size_t>(width) * height * 2;
						pl_gpu_finish(gpu);
						const auto u0 = std::chrono::steady_clock::now();
						pl_plane uploaded[2]{};
						for (int i = 0; i < 2; ++i)
							Assert::IsTrue(pl_upload_plane(gpu, &uploaded[i], &standard[i], &planesData[i]));
						pl_gpu_finish(gpu);
						standardUploadMs = std::min(standardUploadMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count());
					}
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(encoding, source.data(), static_cast<size_t>(state->BytesPerRow()), width, height, planes));
					pl_gpu_finish(gpu);
					uploadAndPassMs = std::min(uploadAndPassMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					for (uint64_t ns = pl_timer_query(gpu, timer); ns; ns = pl_timer_query(gpu, timer))
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				pl_tex_destroy(gpu, &standard[0]);
				pl_tex_destroy(gpu, &standard[1]);
				char line[320];
				std::snprintf(line, sizeof(line), "GPU capture unpack %s 3840x2160: standard = CPU conversion %.2f + P210 upload %.2f = %.2f ms; "
					"gpu = raw upload + pass %.2f ms (pass alone %.3f ms)",
					encoding == VideoFrameEncoding::V210 ? "V210" : "UYVY", cpuMs, standardUploadMs, cpuMs + standardUploadMs,
					uploadAndPassMs, passMs < 1e29 ? passMs : -1.0);
				Logger::WriteMessage(line);
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			} catch (const std::exception& e) { const std::string w = where + ": " + e.what(); Assert::Fail(std::wstring(w.begin(), w.end()).c_str()); }
			catch (...) { Assert::Fail(L"non-std exception"); }
		}
	};
}
