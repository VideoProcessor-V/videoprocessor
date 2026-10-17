#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * VPV probe: packs a synthetic TV-led Dolby Vision RGB tunnel picture into a
 * B8G8R8A8 buffer, the source format NVAPI documents for
 * NV_HDR_MODE_DOLBY_VISION ("RGB8 Dolby Vision encoded (12 bpc YCbCr422
 * packed into RGB8)").
 *
 * This is the same packing as VP's independent test sender
 * (src/VideoProcessor-Test/DolbyVisionTunnelSender.h), minus the DeckLink
 * 10-bit container step, so the bytes are exactly the HDMI wire bytes:
 *   - HDMI 1.4a 6.5.4: R8 = C[11:4], G8 = I[11:4], B8 = C[3:0] << 4 | I[3:0]
 *   - 4:2:2: even pixels carry Cb (or P), odd pixels Cr (or T)
 *   - ETSI GS CCM 001 6.2-6.4: 128-byte packets (CRC-32/MPEG-2 over bytes
 *     0..123, big endian at 124), each sent as three 1024-bit copies in
 *     raster order from pixel 0, one bit per pixel in the chroma LSB,
 *     scrambled with the parity of the pixel's other 23 bits
 *     (all 12 I bits and C bits 1..11).
 *   - the DM body follows the layout VP's CR210DolbyVisionTunnelFormatter
 *     parses (ParseMetadata): ycc_to_rgb coef/offsets, rgb_to_lms, signal
 *     fields at 50..70, extension blocks (L1) after byte 71.
 * The application (not the driver) embeds the DM: the NVAPI header says the
 * application is to "embed DV dynamic metadata"; NV_HDR_COLOR_DATA carries
 * no DM fields.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vpv
{
	using Packet = std::array<uint8_t, 128>;

	inline uint32_t Crc32Mpeg2(const uint8_t* bytes, size_t count)
	{
		uint32_t crc = 0xFFFFFFFFU;
		for (size_t i = 0; i < count; ++i)
		{
			crc ^= static_cast<uint32_t>(bytes[i]) << 24;
			for (int bit = 0; bit < 8; ++bit)
				crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04C11DB7U : crc << 1;
		}
		return crc;
	}

	inline void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v)
	{
		b[at] = static_cast<uint8_t>(v >> 8);
		b[at + 1] = static_cast<uint8_t>(v);
	}

	inline void Put32(uint8_t* b, uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			b[i] = static_cast<uint8_t>(v >> (24 - i * 8));
	}

	// Signal-domain presets. The coefficient sets are the commonly published
	// Dolby Vision profile defaults (profile 8.x BT.2020 YCbCr, profile 5
	// IPTPQc2). Cross-check both against a DM captured from the Apple TV
	// tunnel (VP logs it) before trusting a TV's rendering; --dm-file sends a
	// captured body verbatim and bypasses these.
	enum class DmPreset { YCBCR_BT2020, IPT_PQ_C2 };

	struct DmParams
	{
		DmPreset preset = DmPreset::YCBCR_BT2020;
		bool fullRange = true;
		uint16_t sourceMinPq = 62;    // ~0.005 cd/m2
		uint16_t sourceMaxPq = 3696;  // ~4000 cd/m2 mastering display
		uint16_t l1MinPq = 62;
		uint16_t l1MaxPq = 3079;      // ~1000 cd/m2 scene peak
		uint16_t l1AvgPq = 1262;      // ~10 cd/m2 scene average
		uint8_t sourceDiagonal = 42;
	};

	inline std::vector<uint8_t> BuildDmBody(const DmParams& p)
	{
		std::vector<uint8_t> dm(71, 0);
		dm[1] = 1; // scene refresh
		int16_t ycc[9];
		uint32_t offsets[3];
		uint16_t lms[9];
		if (p.preset == DmPreset::YCBCR_BT2020)
		{
			// Full-range BT.2020 non-constant-luminance Y'CbCr -> R'G'B'
			// (x 8192), rows R,G,B over columns Y,Cb,Cr.
			const int16_t c[9] = { 8192, 0, 12080, 8192, -1348, -4681, 8192, 15413, 0 };
			// BT.2020 RGB -> LMS with the 2% crosstalk (x 16384).
			const uint16_t m[9] = { 7222, 8771, 390, 2654, 12430, 1300, 0, 422, 15962 };
			std::copy(c, c + 9, ycc);
			std::copy(m, m + 9, lms);
			offsets[0] = 0;
			offsets[1] = offsets[2] = 0x08000000U; // 0.5 in 2^-28 units
		}
		else
		{
			const int16_t c[9] = { 8192, 799, 1681, 8192, -933, 1091, 8192, 267, -5545 };
			const int16_t m[9] = { 17081, -349, -349, -349, 17081, -349, -349, -349, 17081 };
			std::copy(c, c + 9, ycc);
			for (int i = 0; i < 9; ++i)
				lms[i] = static_cast<uint16_t>(m[i]);
			offsets[0] = 0;
			offsets[1] = offsets[2] = 0x08000000U;
		}
		for (size_t i = 0; i < 9; ++i)
		{
			Put16(dm, 2 + i * 2, static_cast<uint16_t>(ycc[i]));
			Put16(dm, 32 + i * 2, lms[i]);
		}
		for (size_t i = 0; i < 3; ++i)
			Put32(dm.data() + 20 + i * 4, offsets[i]);
		dm[50] = dm[51] = 0xFF;                                  // signal_eotf = PQ
		dm[60] = 12;                                             // signal_bit_depth
		dm[61] = p.preset == DmPreset::YCBCR_BT2020 ? 0 : 2;     // YCbCr / IPT
		dm[62] = 1;                                              // 4:2:2
		dm[63] = p.fullRange ? 1 : 0;
		Put16(dm, 64, p.sourceMinPq);
		Put16(dm, 66, p.sourceMaxPq);
		dm[69] = p.sourceDiagonal;
		dm[70] = 1;                                              // one extension block
		const size_t at = dm.size();
		dm.resize(at + 11, 0);
		Put32(dm.data() + at, 6);                                // payload length
		dm[at + 4] = 1;                                          // level 1
		Put16(dm, at + 5, p.l1MinPq);
		Put16(dm, at + 7, p.l1MaxPq);
		Put16(dm, at + 9, p.l1AvgPq);
		return dm;
	}

	inline std::vector<Packet> Packetize(const std::vector<uint8_t>& body,
		uint8_t currentId = 3, uint8_t affectedId = 3)
	{
		std::vector<Packet> packets;
		size_t offset = 0;
		for (;;)
		{
			Packet packet{};
			const bool first = packets.empty();
			const size_t capacity = first ? 119 : 121;
			const size_t chunk = (std::min)(capacity, body.size() - offset);
			const bool last = offset + chunk == body.size();
			const unsigned kind = first ? (last ? 0 : 1) : (last ? 3 : 2);
			packet[0] = static_cast<uint8_t>(kind << 6);
			packet[1] = static_cast<uint8_t>((affectedId << 4) | (currentId & 15));
			packet[2] = 0; // not end of stream
			if (first)
			{
				packet[3] = static_cast<uint8_t>(body.size() >> 8);
				packet[4] = static_cast<uint8_t>(body.size());
			}
			std::copy_n(body.data() + offset, chunk, packet.data() + (first ? 5 : 3));
			Put32(packet.data() + 124, Crc32Mpeg2(packet.data(), 124));
			packets.push_back(packet);
			offset += chunk;
			if (last)
				break;
		}
		return packets;
	}

	enum class Pattern { RAMP, BARS, FLAT };

	// The 12-bit planes of one picture (I per pixel, C per pixel alternating
	// Cb/Cr). Neutral chroma = 2048 in full range.
	struct TunnelPicture
	{
		uint32_t width = 0, height = 0;
		std::vector<uint16_t> intensity, chroma;

		TunnelPicture(uint32_t w, uint32_t h, Pattern pattern) : width(w), height(h)
		{
			intensity.resize(static_cast<size_t>(w) * h);
			chroma.assign(intensity.size(), 2048);
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					uint16_t i = 2048;
					if (pattern == Pattern::RAMP)
						i = static_cast<uint16_t>(static_cast<uint64_t>(x) * 4095 / (w > 1 ? w - 1 : 1));
					else if (pattern == Pattern::BARS)
						i = static_cast<uint16_t>(((x * 8) / w) * 585);
					intensity[static_cast<size_t>(y) * w + x] = i;
				}
		}

		static unsigned ScramblingParity(uint16_t i, uint16_t c)
		{
			unsigned parity = 0;
			for (unsigned b = 0; b < 12; ++b)
			{
				parity ^= (i >> b) & 1U;
				if (b != 0)
					parity ^= (c >> b) & 1U;
			}
			return parity;
		}

		// Returns false when the picture is too small for the packets.
		bool Embed(const std::vector<Packet>& packets)
		{
			if (packets.size() * 3072 > intensity.size())
				return false;
			for (size_t p = 0; p < packets.size(); ++p)
				for (size_t copy = 0; copy < 3; ++copy)
					for (size_t bit = 0; bit < 1024; ++bit)
					{
						const size_t px = p * 3072 + copy * 1024 + bit;
						const unsigned md = (packets[p][bit / 8] >> (7 - bit % 8)) & 1U;
						const unsigned parity = ScramblingParity(intensity[px], chroma[px]);
						chroma[px] = static_cast<uint16_t>((chroma[px] & ~1U) | (md ^ parity));
					}
			return true;
		}

		// B8G8R8A8 rows (pitch in bytes): byte 0 = B, 1 = G, 2 = R, 3 = A.
		void WriteBgra(uint8_t* out, size_t pitch) const
		{
			for (uint32_t y = 0; y < height; ++y)
			{
				uint8_t* row = out + static_cast<size_t>(y) * pitch;
				for (uint32_t x = 0; x < width; ++x)
				{
					const size_t px = static_cast<size_t>(y) * width + x;
					const unsigned i = intensity[px], c = chroma[px];
					row[x * 4 + 0] = static_cast<uint8_t>(((c & 15) << 4) | (i & 15));
					row[x * 4 + 1] = static_cast<uint8_t>(i >> 4);
					row[x * 4 + 2] = static_cast<uint8_t>(c >> 4);
					row[x * 4 + 3] = 0xFF;
				}
			}
		}
	};
}
