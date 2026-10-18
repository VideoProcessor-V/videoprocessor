/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// External anchors for the DV harness (2026-10-17): real Dolby Vision RPU
// metadata read by a third party (libdovi / dovi_tool, MIT; the files are
// dovi_tool's own test assets, test-fixtures/dv-rpu/README.md).
//
//  - Their parse -> our tunnel serialisation -> the loop -> VP's decode: every
//    decoded field equals dovi_tool's value (field semantics anchored outside
//    VP's own reading), for real trim sets, L5/L6/L11, multi-packet DMs.
//  - The RPU CRC-32 checks with the standard ISO/IEC 13818-1 algorithm over
//    the RPU payload (emulation prevention removed, 0x19 prefix and trailing
//    0x80 excluded): an independent check that the bytes dovi_tool read are
//    the bytes in the file.
// The opposite direction (our serialisation -> their parse) does not apply to
// the tunnel: dovi_tool parses RPUs, VP neither parses nor writes RPUs, and the
// HDMI tunnel carries the DM structure, not the RPU (docs/VP-dv-tunnel-autotest.md).
#include "pch.h"
#include "CppUnitTest.h"

#include "DvTunnelReferenceEncoder.h"
#include <dv_loopback/DvLoopbackHarness.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace
	{
		std::string FixtureFolder()
		{
			std::string path = __FILE__;
			std::replace(path.begin(), path.end(), '/', '\\');
			const size_t at = path.rfind("\\src\\VideoProcessor-Test\\");
			return path.substr(0, at) + "\\test-fixtures\\dv-rpu";
		}

		std::string Env(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string out;
			if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
			std::free(v);
			return out;
		}

		// The external RPU corpus (opt-in): VP_TEST_DV_RPU_DIR, else D:\testclips\dv\rpu.
		std::string CorpusFolder()
		{
			std::string dir = Env("VP_TEST_DV_RPU_DIR");
			if (dir.empty())
				dir = "D:\\testclips\\dv\\rpu";
			const DWORD a = GetFileAttributesA(dir.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? dir : std::string();
		}

		std::vector<std::string> Files(const std::string& folder, const char* pattern)
		{
			std::vector<std::string> out;
			WIN32_FIND_DATAA data{};
			HANDLE find = FindFirstFileA((folder + "\\" + pattern).c_str(), &data);
			if (find == INVALID_HANDLE_VALUE)
				return out;
			do out.push_back(folder + "\\" + data.cFileName); while (FindNextFileA(find, &data));
			FindClose(find);
			std::sort(out.begin(), out.end());
			return out;
		}

		std::vector<long long> Numbers(const std::string& text)
		{
			std::vector<long long> v;
			std::stringstream ss(text);
			std::string item;
			while (std::getline(ss, item, ','))
				v.push_back(std::strtoll(item.c_str(), nullptr, 10));
			return v;
		}

		// One frame of tools/dv_rpu_to_dm.py output (dovi_tool's values).
		struct RpuFrame
		{
			std::vector<std::pair<std::string, std::vector<long long>>> fields;
			std::vector<long long> Get(const std::string& key) const
			{
				for (const auto& f : fields) if (f.first == key) return f.second;
				return {};
			}
		};

		std::vector<RpuFrame> ReadDmLines(const std::string& path)
		{
			std::vector<RpuFrame> frames;
			std::ifstream in(path);
			std::string line;
			while (std::getline(in, line))
			{
				RpuFrame f;
				std::stringstream ss(line);
				std::string token;
				while (ss >> token)
				{
					const size_t eq = token.find('=');
					if (eq != std::string::npos)
						f.fields.push_back({ token.substr(0, eq), Numbers(token.substr(eq + 1)) });
				}
				if (!f.fields.empty())
					frames.push_back(f);
			}
			return frames;
		}

		std::vector<uint8_t> Be16s(const std::vector<long long>& v, size_t from = 0)
		{
			std::vector<uint8_t> out;
			for (size_t k = from; k < v.size(); ++k)
				PutBe16(out, static_cast<uint16_t>(v[k]));
			return out;
		}

		// dovi_tool's vdr_dm_data -> the tunnel's dm_metadata() (ETSI GS CCM 001
		// Table 3/4/5; levels ETSI does not define use VP's 16-bit containers).
		DmFields ToDm(const RpuFrame& f)
		{
			DmFields dm;
			const auto ycc = f.Get("ycc"), off = f.Get("off"), lms = f.Get("lms"), params = f.Get("params");
			for (int k = 0; k < 9; ++k)
			{
				dm.yccToRgb[k] = static_cast<int16_t>(ycc.at(k));
				dm.rgbToLms[k] = static_cast<int16_t>(lms.at(k));
			}
			for (int k = 0; k < 3; ++k)
				dm.yccToRgbOffset[k] = static_cast<uint32_t>(off.at(k));
			dm.sceneRefresh = static_cast<uint8_t>(f.Get("refresh").at(0));
			dm.signalEotf = static_cast<uint16_t>(f.Get("eotf").at(0));
			// libdovi: signal_eotf_param0/1 (16 bit), param2 (32 bit) - ETSI bytes 52..59.
			const uint32_t p2 = static_cast<uint32_t>(params.at(2));
			const uint8_t eotfParams[8] = { static_cast<uint8_t>(params.at(0) >> 8), static_cast<uint8_t>(params.at(0)),
				static_cast<uint8_t>(params.at(1) >> 8), static_cast<uint8_t>(params.at(1)),
				static_cast<uint8_t>(p2 >> 24), static_cast<uint8_t>(p2 >> 16), static_cast<uint8_t>(p2 >> 8), static_cast<uint8_t>(p2) };
			std::copy(eotfParams, eotfParams + 8, dm.eotfParams);
			dm.signalBitDepth = static_cast<uint8_t>(f.Get("depth").at(0));
			dm.signalColorSpace = static_cast<uint8_t>(f.Get("cs").at(0));
			dm.signalChromaFormat = static_cast<uint8_t>(f.Get("chroma").at(0));
			dm.signalFullRange = static_cast<uint8_t>(f.Get("full").at(0));
			dm.sourceMinPq = static_cast<uint16_t>(f.Get("minpq").at(0));
			dm.sourceMaxPq = static_cast<uint16_t>(f.Get("maxpq").at(0));
			dm.sourceDiagonal = static_cast<uint16_t>(f.Get("diag").at(0));
			for (const auto& field : f.fields)
			{
				const std::string& k = field.first;
				const std::vector<long long>& v = field.second;
				if (k == "L1") dm.ext.push_back(Ext::L1(uint16_t(v[0]), uint16_t(v[1]), uint16_t(v[2])));
				else if (k == "L2") dm.ext.push_back(Ext::L2(uint16_t(v[0]), uint16_t(v[1]), uint16_t(v[2]), uint16_t(v[3]),
					uint16_t(v[4]), uint16_t(v[5]), int16_t(v[6])));
				else if (k == "L3") dm.ext.push_back(Ext::Raw(3, Be16s(v)));
				else if (k == "L4") dm.ext.push_back(Ext::Raw(4, Be16s(v)));
				else if (k == "L5") dm.ext.push_back(Ext::L5(uint16_t(v[0]), uint16_t(v[1]), uint16_t(v[2]), uint16_t(v[3])));
				else if (k == "L6") dm.ext.push_back(Ext::L6(uint16_t(v[0]), uint16_t(v[1]), uint16_t(v[2]), uint16_t(v[3])));
				else if (k == "L8")
				{
					uint8_t sat[6] = {}, hue[6] = {};
					int extras = v.size() >= 9 ? 1 : 0;
					if (v.size() >= 15) { extras = 2; for (int s = 0; s < 6; ++s) sat[s] = uint8_t(v[9 + s]); }
					if (v.size() >= 21) { extras = 3; for (int s = 0; s < 6; ++s) hue[s] = uint8_t(v[15 + s]); }
					dm.ext.push_back(Ext::L8(uint8_t(v[0]), uint16_t(v[1]), uint16_t(v[2]), uint16_t(v[3]), uint16_t(v[4]),
						uint16_t(v[5]), int16_t(v[6]), extras, extras ? uint16_t(v[7]) : 2048, extras ? uint16_t(v[8]) : 2048, sat, hue));
				}
				else if (k == "L9")
				{
					std::vector<uint8_t> p = { uint8_t(v[0]) };
					const std::vector<uint8_t> rest = Be16s(v, 1);
					p.insert(p.end(), rest.begin(), rest.end());
					dm.ext.push_back(Ext::Raw(9, p));
				}
				else if (k == "L10")
				{
					std::vector<uint8_t> p = { uint8_t(v[0]) };
					PutBe16(p, uint16_t(v[1]));
					PutBe16(p, uint16_t(v[2]));
					p.push_back(uint8_t(v[3]));
					const std::vector<uint8_t> rest = Be16s(v, 4);
					p.insert(p.end(), rest.begin(), rest.end());
					dm.ext.push_back(Ext::Raw(10, p));
				}
				else if (k == "L11") dm.ext.push_back(Ext::L11(uint8_t(v[0]), uint8_t(v[1]), v[2] != 0, uint8_t(v[3]), uint8_t(v[4])));
				else if (k == "L254") dm.ext.push_back(Ext::Raw(254, { uint8_t(v[0]), uint8_t(v[1]) }));
			}
			return dm;
		}

		// dovi_tool's values checked against VP's decode directly (not through
		// DvMetadataModel): the external anchor for the field semantics.
		std::vector<std::string> AnchorCheck(const RpuFrame& f, const DolbyVisionTunnelFrameMetadata& md)
		{
			std::vector<std::string> bad;
			auto want = [&bad](bool ok, const std::string& what) { if (!ok) bad.push_back(what); };
			const auto ycc = f.Get("ycc"), lms = f.Get("lms"), off = f.Get("off");
			for (int k = 0; k < 9; ++k)
			{
				want(md.nonlinear[k / 3][k % 3] == static_cast<float>(ycc[k]) / 8192.0f, "ycc_to_rgb_coef" + std::to_string(k));
				want(md.linear[k / 3][k % 3] == static_cast<float>(lms[k]) / 16384.0f, "rgb_to_lms_coef" + std::to_string(k));
			}
			for (int k = 0; k < 3; ++k)
				want(md.nonlinearOffset[k] == static_cast<float>(static_cast<uint32_t>(off[k])) / 268435456.0f, "ycc_to_rgb_offset" + std::to_string(k));
			want(md.fullRange == (f.Get("full")[0] != 0), "signal_full_range_flag");
			want(md.sourceMinPq == f.Get("minpq")[0], "source_min_pq");
			want(md.sourceMaxPq == f.Get("maxpq")[0], "source_max_pq");
			const auto l1 = f.Get("L1");
			if (!l1.empty())
				want(md.hasSceneMetadata && md.sceneMinPq == l1[0] && md.sceneMaxPq == l1[1] && md.sceneAvgPq == l1[2], "Level1");
			const auto l5 = f.Get("L5");
			if (!l5.empty())
				want(md.hasActiveArea && md.activeAreaLeft == l5[0] && md.activeAreaRight == l5[1] &&
					md.activeAreaTop == l5[2] && md.activeAreaBottom == l5[3], "Level5");
			const auto l6 = f.Get("L6");
			if (!l6.empty())
			{
				want(md.hasL6 && md.l6MaxDisplayMasteringLuminance == l6[0] && md.l6MinDisplayMasteringLuminance == l6[1], "Level6 mastering");
				const bool light = l6[2] > 0 && l6[3] <= l6[2];
				want(md.hasContentLight == light && (!light || (md.maxCll == l6[2] && md.maxFall == l6[3])), "Level6 MaxCLL/MaxFALL");
			}
			const auto l11 = f.Get("L11");
			if (!l11.empty())
				want(md.hasL11 && md.l11ContentType == (l11[0] & 15) && md.l11Whitepoint == (l11[1] & 15) &&
					md.l11ReferenceMode == (l11[2] != 0), "Level11");
			// Trims: every L2 first (in order), then every L8, up to the slots.
			std::vector<std::vector<long long>> l2, l8;
			for (const auto& field : f.fields)
			{
				if (field.first == "L2") l2.push_back(field.second);
				if (field.first == "L8") l8.push_back(field.second);
			}
			size_t slot = 0;
			for (const auto& t : l2)
			{
				if (slot >= DolbyVisionTunnelFrameMetadata::kMaxTrims) break;
				const auto& m = md.trims[slot++];
				want(slot <= md.trimCount && m.level == 2 && m.targetMaxPq == t[0] && m.slope == t[1] && m.offset == t[2] &&
					m.power == t[3] && m.chromaWeight == t[4] && m.saturationGain == t[5] && m.msWeight == static_cast<int16_t>(t[6]),
					"Level2 #" + std::to_string(slot));
			}
			for (const auto& t : l8)
			{
				if (slot >= DolbyVisionTunnelFrameMetadata::kMaxTrims) break;
				const auto& m = md.trims[slot++];
				want(slot <= md.trimCount && m.level == 8 && m.targetDisplayIndex == t[0] && m.slope == t[1] && m.offset == t[2] &&
					m.power == t[3] && m.chromaWeight == t[4] && m.saturationGain == t[5] && m.msWeight == static_cast<int16_t>(t[6]) &&
					(t.size() < 9 || (m.hasMidContrastAndClip && m.midContrast == t[7] && m.clipTrim == t[8])),
					"Level8 #" + std::to_string(slot));
			}
			want(md.trimCount == slot, "trim count");
			want(md.trimOverflow == (l2.size() + l8.size() > DolbyVisionTunnelFrameMetadata::kMaxTrims), "trim overflow flag");
			return bad;
		}

		struct AnchorResult { size_t frames = 0, rejectedAsVpDocuments = 0, mismatches = 0, multiPacket = 0; std::string first; };

		AnchorResult RunAnchor(const std::string& path)
		{
			AnchorResult out;
			const std::vector<RpuFrame> frames = ReadDmLines(path);
			const uint32_t w = 640;
			uint32_t h = 108;
			for (const RpuFrame& f : frames)
			{
				const size_t packets = Packetize(SerializeDm(ToDm(f)), PacketHeader()).size();
				h = (std::max)(h, static_cast<uint32_t>((packets * kPixelsPerPacket + w - 1) / w + 16));
			}
			AnalyzerOptions options;
			options.collectLog = false;
			options.keepFrames = false;
			DvLoopbackAnalyzer analyzer(options);
			DvSimulatedLoop loop(analyzer, LoopFaults());
			loop.Begin(w, h, 24000, 1001);
			for (size_t k = 0; k < frames.size(); ++k)
			{
				const DmFields dm = ToDm(frames[k]);
				IptPicture p;
				p.Resize(w, h);
				PacketHeader hdr;
				hdr.currentId = hdr.affectedId = static_cast<uint8_t>(k & 15);
				LoopFrame lf;
				lf.index = static_cast<uint32_t>(k);
				Assert::IsTrue(BuildTunnelRgb8(p, dm, hdr, lf.rgb8));
				if (Packetize(SerializeDm(dm), hdr).size() > 1) ++out.multiPacket;
				loop.Send(lf, 0);
				++out.frames;
				bool accepted = false;
				ExpectedDecode(dm, accepted);
				const DolbyVisionTunnelFrameMetadata& md = analyzer.LastMetadata();
				if (!accepted)
				{
					// VP documents 12-bit opponent colour only (e.g. profile 4's 14-bit).
					if (md.valid && k == 0) { ++out.mismatches; if (out.first.empty()) out.first = "accepted a DM VP must reject"; }
					++out.rejectedAsVpDocuments;
					continue;
				}
				const std::vector<std::string> bad = AnchorCheck(frames[k], md);
				if (!md.valid || !bad.empty())
				{
					++out.mismatches;
					if (out.first.empty())
						out.first = "frame " + std::to_string(k) + ": " + (md.valid ? bad.front() : std::string("not decoded"));
				}
			}
			loop.End();
			return out;
		}

		// ISO/IEC 13818-1 CRC-32 (bit-serial, independent of the packer's table).
		uint32_t Crc32(const std::vector<uint8_t>& bytes, size_t from, size_t to)
		{
			uint32_t crc = 0xFFFFFFFFu;
			for (size_t k = from; k < to; ++k)
			{
				crc ^= static_cast<uint32_t>(bytes[k]) << 24;
				for (int b = 0; b < 8; ++b)
					crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
			}
			return crc;
		}

		// Every RPU in a dovi_tool .bin (Annex B start codes, NAL header stripped
		// by extract-rpu), emulation prevention removed.
		std::vector<std::vector<uint8_t>> RpuPayloads(const std::vector<uint8_t>& file)
		{
			std::vector<size_t> starts;
			for (size_t k = 0; k + 3 <= file.size(); ++k)
				if (file[k] == 0 && file[k + 1] == 0 && file[k + 2] == 1) { starts.push_back(k + 3); k += 2; }
			std::vector<std::vector<uint8_t>> out;
			for (size_t s = 0; s < starts.size(); ++s)
			{
				size_t end = s + 1 < starts.size() ? starts[s + 1] - 3 : file.size();
				while (end > starts[s] && file[end - 1] == 0) --end;
				std::vector<uint8_t> p;
				int zeros = 0;
				for (size_t k = starts[s]; k < end; ++k)
				{
					if (zeros >= 2 && file[k] == 3) { zeros = 0; continue; }
					p.push_back(file[k]);
					zeros = file[k] == 0 ? zeros + 1 : 0;
				}
				out.push_back(p);
			}
			return out;
		}

		// DV input fixes 2026-10-18: the same frame as the independent reference
		// encoder's DM (values only; L1 / L2 / L5 / L6 - the levels the
		// reference writes the way VP reads them).
		DvReference::Dm ToReferenceDm(const RpuFrame& f)
		{
			DvReference::Dm dm;
			const auto ycc = f.Get("ycc"), off = f.Get("off"), lms = f.Get("lms");
			for (int k = 0; k < 9; ++k)
			{
				dm.yccToRgb[k] = static_cast<int>(ycc.at(k));
				dm.rgbToLms[k] = static_cast<int>(lms.at(k));
			}
			for (int k = 0; k < 3; ++k)
				dm.offsets[k] = static_cast<uint32_t>(off.at(k));
			dm.sceneRefresh = static_cast<unsigned>(f.Get("refresh").at(0));
			dm.signalBitDepth = static_cast<unsigned>(f.Get("depth").at(0));
			dm.signalColorSpace = static_cast<unsigned>(f.Get("cs").at(0));
			dm.fullRange = static_cast<unsigned>(f.Get("full").at(0));
			dm.sourceMinPq = static_cast<unsigned>(f.Get("minpq").at(0));
			dm.sourceMaxPq = static_cast<unsigned>(f.Get("maxpq").at(0));
			for (const auto& field : f.fields)
			{
				const std::vector<long long>& v = field.second;
				if (field.first == "L1") dm.AddL1(unsigned(v[0]), unsigned(v[1]), unsigned(v[2]));
				else if (field.first == "L2") dm.AddL2(unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]),
					unsigned(v[4]), unsigned(v[5]), int(v[6]));
				else if (field.first == "L5") dm.AddL5(unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]));
				else if (field.first == "L6") dm.AddL6(unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]));
			}
			return dm;
		}

		// The 12-bit codes of PQ black (zero light) under a DM: the code whose
		// normalised value equals the DM's YCCtoRGB offset (VP's 2^28 scale),
		// full or limited range as the DM signals.
		struct BlackCodes { uint16_t i, ct, cp; };
		BlackCodes BlackOf(const DvReference::Dm& dm)
		{
			auto code = [&dm](uint32_t offset, bool luma) {
				const double o = static_cast<double>(offset) / 268435456.0;
				const double c = dm.fullRange ? o * 4095.0 : (luma ? 256.0 + o * 3504.0 : 256.0 + o * 3584.0);
				return static_cast<uint16_t>((std::min)(4095.0, (std::max)(0.0, std::floor(c + 0.5))));
			};
			return { code(dm.offsets[0], true), code(dm.offsets[1], false), code(dm.offsets[2], false) };
		}

		enum class LegitPicture { AllBlack, DeepestBlack, Letterboxed, FadeToBlack };

		// A legitimate picture under the DM, as planes for DvReference::Encode.
		void LegitPlanes(LegitPicture kind, const BlackCodes& black, uint32_t w, uint32_t h, uint32_t seed,
			std::vector<uint16_t>& i, std::vector<uint16_t>& ct, std::vector<uint16_t>& cp)
		{
			i.assign(static_cast<size_t>(w) * h, black.i);
			ct.assign(static_cast<size_t>(w / 2) * h, black.ct);
			cp.assign(static_cast<size_t>(w / 2) * h, black.cp);
			if (kind == LegitPicture::AllBlack)
				return;
			if (kind == LegitPicture::DeepestBlack)
			{
				// The lowest intensity code anywhere (below the DM's black for a
				// limited-range or offset DM), neutral chroma.
				std::fill(i.begin(), i.end(), uint16_t(0));
				return;
			}
			const uint32_t bar = kind == LegitPicture::Letterboxed ? h / 8 : 0;
			for (uint32_t y = bar; y < h - bar; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const int content = 300 + static_cast<int>((x * 2800) / w) + static_cast<int>(y * 3) + static_cast<int>(seed % 7);
					const int chroma = static_cast<int>((x + y + seed) % 400) - 200;
					// Fade: 2% of the way from black to the picture.
					const int scale = kind == LegitPicture::FadeToBlack ? 2 : 100;
					i[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>((std::min)(4095, black.i + (content - black.i) * scale / 100));
					if (x % 2 == 0)
						ct[static_cast<size_t>(y) * (w / 2) + x / 2] = static_cast<uint16_t>((std::max)(0, (std::min)(4095, black.ct + chroma * scale / 100)));
					else
						cp[static_cast<size_t>(y) * (w / 2) + x / 2] = static_cast<uint16_t>((std::max)(0, (std::min)(4095, black.cp - chroma * scale / 100)));
				}
		}

		// The non-linear (PQ-domain) values an all-zero tunnel word would decode
		// to under the DM, full range: M * (0 - offset). Real light is never
		// negative, so a zero word is not a picture any source produces.
		double ZeroWordMinimum(const DvReference::Dm& dm)
		{
			double lowest = 1e9;
			for (int row = 0; row < 3; ++row)
			{
				double v = 0;
				for (int col = 0; col < 3; ++col)
					v -= (dm.yccToRgb[row * 3 + col] / 8192.0) * (dm.offsets[col] / 268435456.0);
				lowest = (std::min)(lowest, v);
			}
			return lowest;
		}

		struct LegitResult { size_t frames = 0, accepted = 0, failures = 0; std::string first; };

		// Every legitimate picture kind under every DM in `dms`, packed by the
		// independent reference encoder, through VP's capture container and
		// tunnel formatter: never rejected as truncated, always decoded.
		LegitResult RunLegitimate(const std::vector<std::pair<DvReference::Dm, bool>>& dms, const std::string& name)
		{
			LegitResult out;
			const uint32_t w = 640;
			uint32_t h = 108;
			for (const auto& d : dms)
			{
				DvReference::Header hdr;
				const size_t packets = DvReference::PacketBits(DvReference::Body(d.first), hdr).size();
				h = (std::max)(h, static_cast<uint32_t>((packets * kPixelsPerPacket + w - 1) / w + 32));
			}
			for (const bool high8 : { false, true })
			{
				AnalyzerOptions options;
				options.collectLog = false;
				options.keepFrames = false;
				DvLoopbackAnalyzer analyzer(options);
				LoopFaults faults;
				faults.container.high8 = high8;
				DvSimulatedLoop loop(analyzer, faults);
				loop.Begin(w, h, 24000, 1001);
				uint32_t index = 0;
				for (size_t k = 0; k < dms.size(); ++k)
				{
					const BlackCodes black = BlackOf(dms[k].first);
					for (const LegitPicture kind : { LegitPicture::AllBlack, LegitPicture::DeepestBlack,
						LegitPicture::Letterboxed, LegitPicture::FadeToBlack })
					{
						std::vector<uint16_t> i, ct, cp;
						LegitPlanes(kind, black, w, h, static_cast<uint32_t>(k), i, ct, cp);
						DvReference::Header hdr;
						hdr.current = hdr.affected = index & 15;
						LoopFrame lf;
						lf.index = index++;
						Assert::IsTrue(DvReference::Encode(w, h, i, ct, cp, &dms[k].first, hdr, lf.rgb8), L"raster too small");
						loop.Send(lf, 0);
						++out.frames;
						if (!dms[k].second)
							continue;   // a DM VP rejects as documented (e.g. 14-bit): not a decode case
						const bool decoded = analyzer.LastShown() == Shown::Picture;
						out.accepted += decoded ? 1 : 0;
						if (!decoded)
						{
							++out.failures;
							if (out.first.empty())
								out.first = name + " DM " + std::to_string(k) + " picture kind " + std::to_string(static_cast<int>(kind)) +
									(high8 ? " high8" : " low8") + ": shown " + ToString(analyzer.LastShown());
						}
					}
				}
				loop.End();
			}
			return out;
		}
	}

	TEST_CLASS(DvRpuAnchorTests)
	{
	public:
		// DV input fixes 2026-10-18: proof that the truncated-frame check (an
		// all-zero last row) never rejects a legitimate frame.
		//  1. Algebra: an all-zero tunnel word is I = 0 with a chroma sample of
		//     0. Under every DM (synthetic and all real RPUs, full range) it
		//     decodes to a negative non-linear value, M * (0 - offset) < 0: no
		//     non-negative light produces it. Under a limited-range DM, code 0
		//     is below the black code (256) besides.
		//  2. Packing: PQ black (the DM's own black codes), the deepest
		//     intensity code with neutral chroma, letterboxed pictures (black
		//     bars at the bottom) and a fade 2% from black, packed by the
		//     independent reference encoder under every DM, in both RGB8
		//     placements, are all decoded - none rejected.
		TEST_METHOD(TruncationCheckNeverRejectsALegitimateFrame)
		{
			// Synthetic DMs: plain IPT, rich metadata, and a profile-8-style
			// YCbCr DM with luma offset and limited range.
			std::vector<std::pair<DvReference::Dm, bool>> synthetic;
			DvReference::Dm plain = DvReference::Dm::Ipt();
			plain.AddL1(0, 2081, 819);
			synthetic.push_back({ plain, true });
			DvReference::Dm rich = plain;
			rich.AddL2(2851, 2100, 2000, 2048, 2048, 2048, 2048);
			rich.AddL5(0, 0, 280, 280);
			rich.AddL6(1000, 1, 1000, 400);
			rich.AddL8(1, 2048, 2048, 2048, 2048, 2048, 2048, true);
			rich.AddL11(1, 0, true);
			synthetic.push_back({ rich, true });
			DvReference::Dm ycc = plain;
			ycc.offsets = { 16777216u, 134217728u, 134217728u };
			ycc.signalColorSpace = 0;
			ycc.fullRange = 0;
			synthetic.push_back({ ycc, true });
			for (const auto& d : synthetic)
				Assert::IsTrue(ZeroWordMinimum(d.first) < -0.05, L"synthetic DM: a zero word decodes negative");
			const LegitResult s = RunLegitimate(synthetic, "synthetic");
			Logger::WriteMessage(("synthetic DMs: " + std::to_string(s.frames) + " frames, " + std::to_string(s.accepted) +
				" decoded, " + std::to_string(s.failures) + " failures " + s.first + "\n").c_str());
			Assert::AreEqual<size_t>(0, s.failures, std::wstring(s.first.begin(), s.first.end()).c_str());

			// Every real RPU (committed fixtures and, when present, the corpus).
			std::vector<std::string> files = Files(FixtureFolder(), "*.dm.txt");
			const std::string corpus = CorpusFolder();
			if (!corpus.empty())
			{
				const std::vector<std::string> more = Files(corpus, "*.dm.txt");
				files.insert(files.end(), more.begin(), more.end());
			}
			size_t frames = 0, decoded = 0, dms = 0, limited = 0;
			double worstZero = -1e9;
			for (const std::string& file : files)
			{
				std::vector<std::pair<DvReference::Dm, bool>> list;
				for (const RpuFrame& f : ReadDmLines(file))
				{
					bool accepted = false;
					ExpectedDecode(ToDm(f), accepted);
					const DvReference::Dm dm = ToReferenceDm(f);
					list.push_back({ dm, accepted });
					++dms;
					if (!dm.fullRange)
					{
						++limited;
						Assert::IsTrue(BlackOf(dm).i > 0, L"limited range: code 0 is below black");
					}
					const double zero = ZeroWordMinimum(dm);
					worstZero = (std::max)(worstZero, zero);
					Assert::IsTrue(zero < -0.01, std::wstring(file.begin(), file.end()).c_str());
				}
				const LegitResult r = RunLegitimate(list, file);
				Assert::AreEqual<size_t>(0, r.failures, std::wstring(r.first.begin(), r.first.end()).c_str());
				frames += r.frames;
				decoded += r.accepted;
			}
			Logger::WriteMessage(("real RPU DMs: " + std::to_string(dms) + " (" + std::to_string(limited) + " limited range) in " +
				std::to_string(files.size()) + " files; zero word decodes to at most " + std::to_string(worstZero) +
				" (negative = impossible light); " + std::to_string(frames) + " legitimate frames packed, " +
				std::to_string(decoded) + " decoded, 0 rejected\n").c_str());
			Assert::IsTrue(decoded > 0);
		}

		// The committed real-RPU fixtures (and, when present, the whole RPU
		// corpus): dovi_tool's reading survives the tunnel and VP's decode.
		TEST_METHOD(RealRpuMetadataDecodesAsDoviToolReadsIt)
		{
			std::vector<std::string> files = Files(FixtureFolder(), "*.dm.txt");
			Assert::IsFalse(files.empty(), L"test-fixtures/dv-rpu has no .dm.txt files");
			const std::string corpus = CorpusFolder();
			if (!corpus.empty())
			{
				const std::vector<std::string> more = Files(corpus, "*.dm.txt");
				files.insert(files.end(), more.begin(), more.end());
			}
			size_t frames = 0, multi = 0, rejected = 0;
			for (const std::string& file : files)
			{
				const AnchorResult r = RunAnchor(file);
				Logger::WriteMessage((file + ": " + std::to_string(r.frames) + " frames, " + std::to_string(r.multiPacket) +
					" multi-packet, " + std::to_string(r.rejectedAsVpDocuments) + " rejected as VP documents, " +
					std::to_string(r.mismatches) + " mismatches " + r.first + "\n").c_str());
				Assert::AreEqual<size_t>(0, r.mismatches, std::wstring(file.begin(), file.end()).c_str());
				frames += r.frames;
				multi += r.multiPacket;
				rejected += r.rejectedAsVpDocuments;
			}
			Logger::WriteMessage(("real RPU frames through the loop: " + std::to_string(frames) + " (" + std::to_string(multi) +
				" multi-packet, " + std::to_string(rejected) + " rejected as documented)\n").c_str());
			Assert::IsTrue(multi > 0, L"real metadata exercised multi-packet DMs");
		}

		// The RPU CRC-32 with the standard algorithm, over the committed RPU
		// fixtures and (opt-in) the corpus. dovi_tool's malformed regression
		// fixtures named *end_crc32* are reported, not required to pass.
		TEST_METHOD(RpuCrc32MatchesTheStandardAlgorithm)
		{
			std::vector<std::string> files = Files(FixtureFolder(), "*.bin");
			const std::string corpus = CorpusFolder();
			if (!corpus.empty())
			{
				const std::vector<std::string> more = Files(corpus, "*.bin");
				files.insert(files.end(), more.begin(), more.end());
			}
			Assert::IsFalse(files.empty());
			size_t checked = 0, failed = 0;
			for (const std::string& file : files)
			{
				std::ifstream in(file, std::ios::binary);
				const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				const bool odd = file.find("end_crc32") != std::string::npos;
				for (const std::vector<uint8_t>& rpu : RpuPayloads(bytes))
				{
					if (rpu.size() < 6 || rpu[0] != 0x19)
						continue;
					const size_t end = rpu.back() == 0x80 ? rpu.size() - 1 : rpu.size();
					const uint32_t stored = (uint32_t(rpu[end - 4]) << 24) | (uint32_t(rpu[end - 3]) << 16) |
						(uint32_t(rpu[end - 2]) << 8) | rpu[end - 1];
					const bool ok = Crc32(rpu, 1, end - 4) == stored;
					++checked;
					if (!ok)
					{
						++failed;
						Logger::WriteMessage((file + ": CRC mismatch" + (odd ? " (malformed regression fixture)" : "") + "\n").c_str());
						Assert::IsTrue(odd, std::wstring(file.begin(), file.end()).c_str());
					}
				}
			}
			Logger::WriteMessage(("RPU CRC-32 checked: " + std::to_string(checked) + ", mismatches in malformed fixtures: " +
				std::to_string(failed) + "\n").c_str());
			Assert::IsTrue(checked > 0);
		}
	};
}
