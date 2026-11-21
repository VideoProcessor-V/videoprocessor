// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// D5-QK K1: the P13 unpack kernel (SourceUnpackGpu) against the PRODUCTION CPU decoders, sample for sample:
// AnalysisLumaSource::Sample().luma << 6 for P010 / P210 / v210 / UYVY / HDYC, DvTunnelTapLaw::Tap16 for the DV tunnel.
// Runs on WARP by default (the fixture's device). Fed through a real early-upload style rgba8 texture and through
// the host-bytes route; padded strides, odd and mid-group widths.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/capture/SourceUnpackGpu.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <AnalysisLumaSource.h>

namespace VideoProcessorTest
{
	TEST_CLASS(SourceUnpackGpuTests)
	{
	public:
		struct Harness
		{
			TargetLutGpuFixture fixture;
			std::unique_ptr<SourceUnpack::SourceUnpackGpu> unpack;
			pl_tex plane = nullptr;
			pl_gpu gpu = nullptr;
			uint32_t seed = 0x9e3779b9u;

			bool Init()
			{
				if (!fixture.Create())
					return false;
				gpu = fixture.Gpu();
				unpack.reset(new SourceUnpack::SourceUnpackGpu(fixture.Log(), gpu));
				unpack->SetHostReadableForTest(true);
				return true;
			}
			~Harness() { if (gpu) pl_tex_destroy(gpu, &plane); }
			uint32_t Next() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

			std::vector<uint16_t> Download()
			{
				std::vector<uint16_t> samples(static_cast<size_t>(plane->params.w) * plane->params.h);
				pl_tex_transfer_params d{};
				d.tex = plane;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			}

			// Runs both routes (host bytes, early-upload texture) and returns the planes.
			void RunBoth(const SourceUnpack::Request& request, const std::vector<uint8_t>& raw,
				const std::function<void(const std::vector<uint16_t>&, const wchar_t*)>& check)
			{
				Assert::IsTrue(unpack->Unpack(request, raw.data(), nullptr, &plane),
					(L"host route: " + ExtendedRangeGpuProbe::ToWide(unpack->LastError())).c_str());
				Assert::AreEqual(request.width, plane->params.w);
				Assert::AreEqual(request.height, plane->params.h);
				check(Download(), L"host route");
				pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
				Assert::IsNotNull(rgba8);
				pl_tex_params rp{};
				rp.w = static_cast<int>(request.rowBytes / 4);
				rp.h = request.height;
				rp.format = rgba8;
				rp.sampleable = true;
				rp.host_writable = true;
				pl_tex early = pl_tex_create(gpu, &rp);
				Assert::IsNotNull(early);
				pl_tex_transfer_params up{};
				up.tex = early;
				up.ptr = const_cast<uint8_t*>(raw.data());
				up.row_pitch = request.rowBytes;
				Assert::IsTrue(pl_tex_upload(gpu, &up));
				Assert::IsTrue(unpack->Unpack(request, nullptr, early, &plane),
					(L"early route: " + ExtendedRangeGpuProbe::ToWide(unpack->LastError())).c_str());
				check(Download(), L"early route");
				pl_tex_destroy(gpu, &early);
			}
		};

		TEST_METHOD(YuvPackingsMatchTheCpuLumaDecode)
		{
			Harness h;
			Assert::IsTrue(h.Init(), L"Could not create the libplacebo test device");
			struct Case { VideoFrameEncoding enc; AnalysisLumaFormat fmt; SourceUnpack::Packing packing; };
			const Case cases[] = {
				{ VideoFrameEncoding::P010, AnalysisLumaFormat::P010, SourceUnpack::Packing::Planar16 },
				{ VideoFrameEncoding::P210, AnalysisLumaFormat::P210, SourceUnpack::Packing::Planar16 },
				{ VideoFrameEncoding::V210, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::V210 },
				{ VideoFrameEncoding::UYVY, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::Uyvy8 },
				{ VideoFrameEncoding::HDYC, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::Uyvy8 } };
			const int sizes[][2] = { { 64, 36 }, { 66, 10 }, { 70, 9 }, { 129, 7 }, { 1280, 720 }, { 1920, 1080 } };
			int ran = 0;
			for (const Case& c : cases)
				for (const auto& size : sizes)
				{
					const int w = size[0], hgt = size[1];
					if (c.packing != SourceUnpack::Packing::V210 && (w & 1) != 0 && c.packing == SourceUnpack::Packing::Uyvy8)
						continue;   // 4:2:2 packings carry an even width
					for (const size_t pad : { size_t(0), size_t(8) })
					{
						SourceUnpack::Request req;
						req.packing = c.packing;
						req.width = w;
						req.height = hgt;
						req.rowBytes = SourceUnpack::MinRowBytes(c.packing, w) + pad;
						const bool twoPlane = c.fmt == AnalysisLumaFormat::P010 || c.fmt == AnalysisLumaFormat::P210;
						const size_t chromaRows = c.fmt == AnalysisLumaFormat::P010 ? (hgt + 1) / 2 : hgt;
						std::vector<uint8_t> raw(req.rowBytes * (hgt + (twoPlane ? chromaRows : 0)));
						for (auto& b : raw)
							b = static_cast<uint8_t>(h.Next() >> 11);
						AnalysisLumaSource src;
						src.data = raw.data();
						src.dataBytes = raw.size();
						src.width = w;
						src.height = hgt;
						src.rowBytes = req.rowBytes;
						src.chromaRowBytes = req.rowBytes;
						src.format = c.fmt;
						src.encoding = c.enc;
						Assert::IsTrue(src.IsValid(), L"the CPU source rejected the case");
						std::wstring name = std::to_wstring(static_cast<int>(c.enc)) + L" " + std::to_wstring(w) + L"x" +
							std::to_wstring(hgt) + L" pad" + std::to_wstring(pad);
						h.RunBoth(req, raw, [&](const std::vector<uint16_t>& got, const wchar_t* route)
						{
							for (int y = 0; y < hgt; ++y)
								for (int x = 0; x < w; ++x)
								{
									AnalysisLumaSample s;
									Assert::IsTrue(src.Sample(x, y, s));
									const uint16_t want = static_cast<uint16_t>(s.luma << 6);
									if (got[static_cast<size_t>(y) * w + x] != want)
										Assert::Fail((name + L" " + route + L": differs at (" + std::to_wstring(x) + L", " +
											std::to_wstring(y) + L")").c_str());
								}
						});
						++ran;
					}
				}
			Assert::IsTrue(ran > 40);
		}

		TEST_METHOD(DvTunnelMatchesTheTapLawForEveryIntensity)
		{
			Harness h;
			Assert::IsTrue(h.Init(), L"Could not create the libplacebo test device");
			int ran = 0;
			for (const VideoFrameEncoding container : { VideoFrameEncoding::R210, VideoFrameEncoding::R10b, VideoFrameEncoding::R10l })
				for (const int alignment : { 1, 2 })
					for (const bool full : { false, true })
					{
						const int w = 128, hgt = 33;   // 4224 pixels: every 12-bit intensity appears
						SourceUnpack::Request req;
						req.packing = SourceUnpack::Packing::DvTunnel;
						req.width = w;
						req.height = hgt;
						req.rowBytes = static_cast<size_t>(w) * 4 + 8;
						req.container = container;
						req.rgbBitAlignment = alignment;
						req.fullRange = full;
						std::vector<uint8_t> raw(req.rowBytes * hgt);
						for (auto& b : raw)
							b = static_cast<uint8_t>(h.Next() >> 9);
						// Pixel n carries intensity code n (mod 4096) by building the word from the tap law's own shift.
						const bool big = DolbyVisionTunnelUnpackReference::ContainerBigEndian(container);
						const uint32_t shift = DolbyVisionTunnelUnpackReference::ContainerShift(container, alignment == 1);
						for (int y = 0; y < hgt; ++y)
							for (int x = 0; x < w; ++x)
							{
								const uint32_t code = static_cast<uint32_t>(y * w + x) % 4096u;
								uint8_t* p = raw.data() + req.rowBytes * y + static_cast<size_t>(x) * 4;
								uint32_t word = big ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] :
									(uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
								// Place the 12-bit code in the intensity fields (HDMI 1.4a 6.5.4: high byte at bit 10+s, low nibble at bit s).
								word &= ~((0xFFu << (10u + shift)) | (0xFu << shift));
								word |= ((code >> 4) << (10u + shift)) | ((code & 0xFu) << shift);
								if (big)
								{
									p[0] = uint8_t(word >> 24); p[1] = uint8_t(word >> 16); p[2] = uint8_t(word >> 8); p[3] = uint8_t(word);
								}
								else
								{
									p[3] = uint8_t(word >> 24); p[2] = uint8_t(word >> 16); p[1] = uint8_t(word >> 8); p[0] = uint8_t(word);
								}
								Assert::AreEqual(code, uint32_t(DvTunnelTapLaw::Code12(p, container, alignment)), L"the test word does not carry its code");
							}
						h.RunBoth(req, raw, [&](const std::vector<uint16_t>& got, const wchar_t* route)
						{
							for (int y = 0; y < hgt; ++y)
								for (int x = 0; x < w; ++x)
								{
									const uint8_t* p = raw.data() + req.rowBytes * y + static_cast<size_t>(x) * 4;
									const uint16_t want = DvTunnelTapLaw::Tap16(p, container, alignment, full);
									if (got[static_cast<size_t>(y) * w + x] != want)
										Assert::Fail((std::wstring(route) + L": DV tap differs at (" + std::to_wstring(x) + L", " +
											std::to_wstring(y) + L")").c_str());
								}
						});
						++ran;
					}
			Assert::AreEqual(12, ran);
		}

		// One yuv case against the CPU decode (the same compare as YuvPackingsMatchTheCpuLumaDecode, used by the 4K / 8K cases).
		static void RunYuvCase(Harness& h, VideoFrameEncoding enc, AnalysisLumaFormat fmt, SourceUnpack::Packing packing, int w, int hgt, size_t pad)
		{
			SourceUnpack::Request req;
			req.packing = packing;
			req.width = w;
			req.height = hgt;
			req.rowBytes = SourceUnpack::MinRowBytes(packing, w) + pad;
			const bool twoPlane = fmt == AnalysisLumaFormat::P010 || fmt == AnalysisLumaFormat::P210;
			const size_t chromaRows = fmt == AnalysisLumaFormat::P010 ? (hgt + 1) / 2 : hgt;
			std::vector<uint8_t> raw(req.rowBytes * (hgt + (twoPlane ? chromaRows : 0)));
			for (auto& b : raw)
				b = static_cast<uint8_t>(h.Next() >> 11);
			AnalysisLumaSource src;
			src.data = raw.data();
			src.dataBytes = raw.size();
			src.width = w;
			src.height = hgt;
			src.rowBytes = req.rowBytes;
			src.chromaRowBytes = req.rowBytes;
			src.format = fmt;
			src.encoding = enc;
			Assert::IsTrue(src.IsValid(), L"the CPU source rejected the case");
			const std::wstring name = std::to_wstring(static_cast<int>(enc)) + L" " + std::to_wstring(w) + L"x" +
				std::to_wstring(hgt) + L" pad" + std::to_wstring(pad);
			h.RunBoth(req, raw, [&](const std::vector<uint16_t>& got, const wchar_t* route)
			{
				for (int y = 0; y < hgt; ++y)
					for (int x = 0; x < w; ++x)
					{
						AnalysisLumaSample s;
						Assert::IsTrue(src.Sample(x, y, s));
						const uint16_t want = static_cast<uint16_t>(s.luma << 6);
						if (got[static_cast<size_t>(y) * w + x] != want)
							Assert::Fail((name + L" " + route + L": differs at (" + std::to_wstring(x) + L", " +
								std::to_wstring(y) + L")").c_str());
					}
			});
		}

		// D5-QK K1 SHOULD 1 (brief s4, rule 6): the production frame sizes. 4K for every yuv packing, padded stride.
		TEST_METHOD(YuvPackingsMatchTheCpuLumaDecodeAt4K)
		{
			Harness h;
			Assert::IsTrue(h.Init(), L"Could not create the libplacebo test device");
			RunYuvCase(h, VideoFrameEncoding::P010, AnalysisLumaFormat::P010, SourceUnpack::Packing::Planar16, 3840, 2160, 8);
			RunYuvCase(h, VideoFrameEncoding::P210, AnalysisLumaFormat::P210, SourceUnpack::Packing::Planar16, 3840, 2160, 8);
			RunYuvCase(h, VideoFrameEncoding::V210, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::V210, 3840, 2160, 8);
			RunYuvCase(h, VideoFrameEncoding::UYVY, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::Uyvy8, 3840, 2160, 8);
			RunYuvCase(h, VideoFrameEncoding::HDYC, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::Uyvy8, 3840, 2160, 8);
		}

		// One 8K case (v210: the most involved field mapping; 7680 is a whole number of six-pixel groups).
		TEST_METHOD(V210MatchesTheCpuLumaDecodeAt8K)
		{
			Harness h;
			Assert::IsTrue(h.Init(), L"Could not create the libplacebo test device");
			RunYuvCase(h, VideoFrameEncoding::V210, AnalysisLumaFormat::NativeYuv422, SourceUnpack::Packing::V210, 7680, 4320, 0);
		}

		// One 4K DV tunnel case per range: arbitrary frame bytes, every pixel against the tap law.
		TEST_METHOD(DvTunnelMatchesTheTapLawAt4K)
		{
			Harness h;
			Assert::IsTrue(h.Init(), L"Could not create the libplacebo test device");
			struct DvCase { VideoFrameEncoding container; int alignment; bool full; };
			const DvCase cases[] = { { VideoFrameEncoding::R210, 1, false }, { VideoFrameEncoding::R10l, 2, true } };
			for (const DvCase& c : cases)
			{
				const int w = 3840, hgt = 2160;
				SourceUnpack::Request req;
				req.packing = SourceUnpack::Packing::DvTunnel;
				req.width = w;
				req.height = hgt;
				req.rowBytes = static_cast<size_t>(w) * 4 + 8;
				req.container = c.container;
				req.rgbBitAlignment = c.alignment;
				req.fullRange = c.full;
				std::vector<uint8_t> raw(req.rowBytes * hgt);
				for (auto& b : raw)
					b = static_cast<uint8_t>(h.Next() >> 9);
				h.RunBoth(req, raw, [&](const std::vector<uint16_t>& got, const wchar_t* route)
				{
					for (int y = 0; y < hgt; ++y)
						for (int x = 0; x < w; ++x)
						{
							const uint8_t* p = raw.data() + req.rowBytes * y + static_cast<size_t>(x) * 4;
							const uint16_t want = DvTunnelTapLaw::Tap16(p, c.container, c.alignment, c.full);
							if (got[static_cast<size_t>(y) * w + x] != want)
								Assert::Fail((std::wstring(route) + L": 4K DV tap differs at (" + std::to_wstring(x) + L", " +
									std::to_wstring(y) + L")").c_str());
						}
				});
			}
		}

		TEST_METHOD(RefusalsNameTheirCauseAndKeepSourceBits)
		{
			using namespace SourceUnpack;
			Assert::AreEqual(10, SourceBits(Packing::Planar16));
			Assert::AreEqual(10, SourceBits(Packing::V210));
			Assert::AreEqual(8, SourceBits(Packing::Uyvy8));
			Assert::AreEqual(12, SourceBits(Packing::DvTunnel));
			Request r;
			r.packing = Packing::Planar16;
			r.width = 64;
			r.height = 8;
			r.rowBytes = 128;
			Assert::IsTrue(Validate(r) == Cause::None);
			r.rowBytes = 126;
			Assert::IsTrue(Validate(r) == Cause::BadGeometry);
			r.rowBytes = 64;
			Assert::IsTrue(Validate(r) == Cause::BadGeometry);
			r.packing = Packing::DvTunnel;
			r.rowBytes = 256;
			r.container = VideoFrameEncoding::NV12;
			Assert::IsTrue(Validate(r) == Cause::DvContainer);
			r.container = VideoFrameEncoding::R210;
			r.rgbBitAlignment = 3;
			Assert::IsTrue(Validate(r) == Cause::DvAlignment);
			r.rgbBitAlignment = 1;
			r.width = 63;
			Assert::IsTrue(Validate(r) == Cause::BadGeometry);
			Assert::AreEqual(std::string("dv container"), std::string(CauseName(Cause::DvContainer)));
		}
	};
}
