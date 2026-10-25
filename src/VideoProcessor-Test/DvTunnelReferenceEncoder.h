#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel autotest, Tier 1 (2026-10-17): an INDEPENDENT reference encoder.
//
// Written directly from ETSI GS CCM 001 V1.1.1 (sections 6.2-6.4, Tables 3-14)
// and the HDMI 12-bit YCbCr 4:2:2 channel mapping, without calling or copying
// VP's decoder (CR210DolbyVisionTunnelFormatter), VP's VPV packer
// (dv_loopback/DvTunnelPacker.h) or the older test sender
// (DolbyVisionTunnelSender.h). It is deliberately built differently:
//   - the DM body is assembled from a field table (name, byte width, value)
//     in Table 3 order, not from a struct serialiser;
//   - packets are built as a bit string; the CRC is the bit-serial LFSR model
//     of ISO/IEC 13818-1 Annex A (polynomial taps listed explicitly);
//   - the picture is processed as the 4:2:2 sample stream of ETSI Table 14
//     (Cb0 Y0 Cr0 Y1 Cb1 Y2 ...), scrambling each chroma sample with the
//     parity of its own bits 11..1 and of the luma sample that follows it;
//   - the RGB8 mapping is an explicit bit-position table.
// The loop harness compares the wire bytes VP's packer produced with this
// encoder's output, so a format error shared by VP's packer and VP's decoder
// (the common-mode trap) cannot pass.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Tests
{
	namespace DvReference
	{
		// A DM field in Table 3 order: width in bytes (1, 2 or 4), big-endian.
		struct Field
		{
			std::string name;
			unsigned bytes;
			uint32_t value;
		};

		// One extension block: level and its fields (Table 5 for L1/L2/L5).
		struct Block
		{
			unsigned level;
			std::vector<Field> fields;
		};

		struct Dm
		{
			unsigned sceneRefresh = 1;
			std::array<int, 9> yccToRgb{};
			std::array<uint32_t, 3> offsets{};
			std::array<int, 9> rgbToLms{};
			unsigned signalBitDepth = 12, signalColorSpace = 2;
			unsigned chromaFormat = 1, fullRange = 1;   // ETSI reserved 0x01 0x01
			unsigned sourceMinPq = 62, sourceMaxPq = 3696;
			std::vector<Block> blocks;

			static Dm Ipt()
			{
				Dm dm;
				dm.yccToRgb = { 8192, 799, 1681, 8192, -933, 1091, 8192, 267, -5545 };
				dm.rgbToLms = { 17081, -349, -349, -349, 17081, -349, -349, -349, 17081 };
				dm.offsets = { 0, 134217728u, 134217728u };
				return dm;
			}
			void AddL1(unsigned mn, unsigned mx, unsigned avg)
			{
				blocks.push_back({ 1, { { "min_PQ", 2, mn }, { "max_PQ", 2, mx }, { "avg_PQ", 2, avg } } });
			}
			void AddL2(unsigned target, unsigned slope, unsigned offset, unsigned power, unsigned chroma,
				unsigned saturation, int ms)
			{
				blocks.push_back({ 2, { { "target_max_PQ", 2, target }, { "trim_slope", 2, slope },
					{ "trim_offset", 2, offset }, { "trim_power", 2, power }, { "trim_chroma_weight", 2, chroma },
					{ "trim_saturation_gain", 2, saturation }, { "ms_weight", 2, static_cast<uint32_t>(ms) & 0xFFFFu } } });
			}
			void AddL5(unsigned l, unsigned r, unsigned t, unsigned b)
			{
				blocks.push_back({ 5, { { "left", 2, l }, { "right", 2, r }, { "top", 2, t }, { "bottom", 2, b } } });
			}
			// Levels ETSI V1.1.1 does not define: VP's assumed 16-bit containers.
			void AddL6(unsigned maxM, unsigned minM, unsigned cll, unsigned fall)
			{
				blocks.push_back({ 6, { { "max_mastering", 2, maxM }, { "min_mastering", 2, minM },
					{ "max_cll", 2, cll }, { "max_fall", 2, fall } } });
			}
			void AddL8(unsigned index, unsigned slope, unsigned offset, unsigned power, unsigned chroma,
				unsigned saturation, int ms, bool midClip, unsigned mid = 2048, unsigned clip = 2048)
			{
				Block b{ 8, { { "target_display_index", 1, index }, { "slope", 2, slope }, { "offset", 2, offset },
					{ "power", 2, power }, { "chroma_weight", 2, chroma }, { "saturation_gain", 2, saturation },
					{ "ms_weight", 2, static_cast<uint32_t>(ms) & 0xFFFFu } } };
				if (midClip)
				{
					b.fields.push_back({ "target_mid_contrast", 2, mid });
					b.fields.push_back({ "clip_trim", 2, clip });
				}
				blocks.push_back(b);
			}
			void AddL11(unsigned type, unsigned whitepoint, bool reference)
			{
				blocks.push_back({ 11, { { "content_type", 1, type },
					{ "whitepoint_ref", 1, (whitepoint & 15u) | (reference ? 16u : 0u) },
					{ "byte2", 1, 0 }, { "byte3", 1, 0 } } });
			}
		};

		inline std::vector<Field> Table3(const Dm& dm)
		{
			std::vector<Field> f;
			f.push_back({ "reserved_0x00", 1, 0 });
			f.push_back({ "scene_refresh_flag", 1, dm.sceneRefresh });
			for (int k = 0; k < 9; ++k)
				f.push_back({ "YCCtoRGB_coef", 2, static_cast<uint32_t>(dm.yccToRgb[k]) & 0xFFFFu });
			for (int k = 0; k < 3; ++k)
				f.push_back({ "YCCtoRGB_offset", 4, dm.offsets[k] });
			for (int k = 0; k < 9; ++k)
				f.push_back({ "RGBtoLMS_coef", 2, static_cast<uint32_t>(dm.rgbToLms[k]) & 0xFFFFu });
			f.push_back({ "reserved_0xff", 1, 0xFF });
			f.push_back({ "reserved_0xff", 1, 0xFF });
			for (int k = 0; k < 8; ++k)
				f.push_back({ "reserved_0x00", 1, 0 });
			f.push_back({ "signal_bit_depth", 1, dm.signalBitDepth });
			f.push_back({ "signal_color_space", 1, dm.signalColorSpace });
			f.push_back({ "reserved_0x01 (chroma format)", 1, dm.chromaFormat });
			f.push_back({ "reserved_0x01 (full range)", 1, dm.fullRange });
			f.push_back({ "source_min_PQ", 2, dm.sourceMinPq });
			f.push_back({ "source_max_PQ", 2, dm.sourceMaxPq });
			f.push_back({ "reserved_0x00", 1, 0 });
			f.push_back({ "reserved_0x2a", 1, 0x2A });
			f.push_back({ "num_ext_blocks", 1, static_cast<uint32_t>(dm.blocks.size()) });
			for (const Block& b : dm.blocks)
			{
				uint32_t length = 0;
				for (const Field& x : b.fields)
					length += x.bytes;
				f.push_back({ "ext_block_length", 4, length });
				f.push_back({ "ext_block_level", 1, b.level });
				for (const Field& x : b.fields)
					f.push_back(x);
			}
			return f;
		}

		inline std::vector<uint8_t> Body(const Dm& dm)
		{
			std::vector<uint8_t> out;
			for (const Field& x : Table3(dm))
				for (int k = static_cast<int>(x.bytes) - 1; k >= 0; --k)
					out.push_back(static_cast<uint8_t>((x.value >> (8 * k)) & 0xFF));
			return out;
		}

		// Bit-serial CRC-32 per ISO/IEC 13818-1 Annex A: register initialised
		// to all ones; for each input bit, feedback = MSB ^ bit; shift; XOR
		// taps x^26 x^23 x^22 x^16 x^12 x^11 x^10 x^8 x^7 x^5 x^4 x^2 x^1 x^0.
		inline uint32_t Crc(const std::vector<int>& bits)
		{
			static const int taps[] = { 26, 23, 22, 16, 12, 11, 10, 8, 7, 5, 4, 2, 1, 0 };
			uint32_t reg = 0xFFFFFFFFu;
			for (int bit : bits)
			{
				const uint32_t feedback = ((reg >> 31) & 1u) ^ static_cast<uint32_t>(bit & 1);
				reg <<= 1;
				if (feedback)
					for (int t : taps)
						reg ^= 1u << t;
			}
			return reg;
		}

		struct Header
		{
			unsigned affected = 0, current = 0;
			bool noMd = false, eos = false;
		};

		// The packets as bit strings (1024 bits each), Tables 8-13.
		inline std::vector<std::vector<int>> PacketBits(const std::vector<uint8_t>& body, const Header& h)
		{
			std::vector<std::vector<uint8_t>> packets;
			const size_t length = h.noMd ? 0 : body.size();
			size_t used = 0;
			do
			{
				const bool first = packets.empty();
				const size_t room = first ? 119 : 121;
				const size_t take = h.noMd ? 0 : (length - used < room ? length - used : room);
				const bool last = h.noMd || used + take == length;
				const unsigned type = first ? (last ? 0u : 1u) : (last ? 3u : 2u);
				std::vector<uint8_t> p;
				p.push_back(static_cast<uint8_t>((type << 6) | (0u << 4) | (0u << 1) | (h.noMd ? 1u : 0u)));
				p.push_back(static_cast<uint8_t>(((h.affected & 15u) << 4) | (h.current & 15u)));
				p.push_back(static_cast<uint8_t>(h.eos ? 1u : 0u));
				if (first && !h.noMd)
				{
					p.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
					p.push_back(static_cast<uint8_t>(length & 0xFF));
				}
				for (size_t k = 0; k < take; ++k)
					p.push_back(body[used + k]);
				used += take;
				while (p.size() < 124)
					p.push_back(0x00);   // trailing_byte
				packets.push_back(p);
				if (last)
					break;
			} while (true);
			std::vector<std::vector<int>> out;
			for (std::vector<uint8_t>& p : packets)
			{
				std::vector<int> bits;
				for (uint8_t byte : p)
					for (int m = 7; m >= 0; --m)
						bits.push_back((byte >> m) & 1);
				const uint32_t crc = Crc(bits);   // over the first 992 bits
				for (int m = 31; m >= 0; --m)
					bits.push_back(static_cast<int>((crc >> m) & 1u));
				out.push_back(bits);
			}
			return out;
		}

		inline int ParityOf(uint32_t v, int fromBit, int toBit)
		{
			int p = 0;
			for (int b = fromBit; b <= toBit; ++b)
				p ^= static_cast<int>((v >> b) & 1u);
			return p;
		}

		// The reference wire picture: i[W*H], ct/cp[(W/2)*H] (12-bit codes),
		// optional DM -> RGB8 (R, G, B per pixel). False if the raster is too
		// small for the packets.
		inline bool Encode(uint32_t width, uint32_t height, const std::vector<uint16_t>& i,
			std::vector<uint16_t> ct, std::vector<uint16_t> cp, const Dm* dm, const Header& header,
			std::vector<uint8_t>& rgb)
		{
			const size_t pixels = static_cast<size_t>(width) * height;
			// The 4:2:2 sample stream, per row: Cb0 Y0 Cr0 Y1 Cb1 Y2 Cr1 Y3 ...
			// chroma sample of pixel x: Cb[x/2] for even x, Cr[x/2] for odd x.
			auto chromaRef = [&](size_t p) -> uint16_t& {
				const size_t y = p / width, x = p % width;
				return (x % 2 == 0 ? ct : cp)[y * (width / 2) + x / 2];
			};
			if (dm || header.noMd)
			{
				const std::vector<uint8_t> body = dm ? Body(*dm) : std::vector<uint8_t>();
				const std::vector<std::vector<int>> packets = PacketBits(body, header);
				if (packets.size() * 3072 > pixels)
					return false;
				for (size_t P = 0; P < packets.size(); ++P)
					for (size_t copy = 0; copy < 3; ++copy)
						for (size_t k = 0; k < 1024; ++k)
						{
							// ETSI 6.4.4: pixel i = P*3072 + (n*8 + (7-m)), copies 1024 apart.
							const size_t p = P * 3072 + copy * 1024 + k;
							uint16_t& c = chromaRef(p);
							const int scramble = ParityOf(c, 1, 11) ^ ParityOf(i[p], 0, 11);
							c = static_cast<uint16_t>((c & ~1u) | static_cast<unsigned>(packets[P][k] ^ scramble));
						}
			}
			// HDMI RGB8 channel bit positions for 12-bit YCbCr 4:2:2:
			//   R[7:0] = C[11:4]; G[7:0] = Y[11:4]; B[7:4] = C[3:0]; B[3:0] = Y[3:0]
			rgb.assign(pixels * 3, 0);
			for (size_t p = 0; p < pixels; ++p)
			{
				const uint32_t y = i[p], c = chromaRef(p);
				uint32_t r = 0, g = 0, b = 0;
				for (int bit = 0; bit < 8; ++bit)
				{
					r |= ((c >> (4 + bit)) & 1u) << bit;
					g |= ((y >> (4 + bit)) & 1u) << bit;
				}
				for (int bit = 0; bit < 4; ++bit)
				{
					b |= ((c >> bit) & 1u) << (4 + bit);
					b |= ((y >> bit) & 1u) << bit;
				}
				rgb[p * 3] = static_cast<uint8_t>(r);
				rgb[p * 3 + 1] = static_cast<uint8_t>(g);
				rgb[p * 3 + 2] = static_cast<uint8_t>(b);
			}
			return true;
		}
	}
}
