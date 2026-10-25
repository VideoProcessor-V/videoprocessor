#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// VPV packer (DV tunnel loopback, 2026-10-17): 12-bit IPT 4:2:2 + display-
// management (DM) metadata -> the 8-bit RGB 4:4:4 HDMI tunnel picture that a
// TV-led Dolby Vision source sends, and that NVIDIA's experimental
// NV_HDR_MODE_DOLBY_VISION expects from the application ("RGB8 Dolby Vision
// encoded: 12 bpc YCbCr422 packed into RGB8").
//
// Written from ETSI GS CCM 001 V1.1.1 (2017-02):
//   6.2   Table 3/4/5 - dm_metadata() byte syntax and extension blocks
//   6.3   Tables 8-13 - 128-byte transmission packets, header, CRC-32
//   6.4.1 - packet bit i -> LSB of the chroma sample of raster pixel i
//   6.4.2 - parity scrambling: parity of chroma bit[11:1] and of the
//           subsequent luma sample's bit[11:0]
//   6.4.3 - three consecutive copies (pixels 0..1023, 1024..2047, 2048..3071)
//   6.4.4 - packet P starts at pixel P * 3072
// and the HDMI 12-bit YCbCr 4:2:2 mapping onto three 8-bit channels
// (Cb/Cr bits 11:4 on R, Y bits 11:4 on G, Cb/Cr bits 3:0 | Y bits 3:0 on B;
// Cb on even pixels, Cr on odd pixels - ETSI 6.4.1 "The chroma sample
// represents the Cb component for even pixels and the Cr component for odd").
//
// Header-only and free of VP dependencies so the hardware loopback sender
// (src/VideoProcessor-DvLoopback) can use it. It deliberately shares no code
// with VP's decoder (CR210DolbyVisionTunnelFormatter) nor with the tests'
// independent reference encoder (VideoProcessor-Test/DvTunnelReferenceEncoder.h):
// the loop compares this packer against the decoder, and the tests compare
// it against the reference, so a format error shared by the packer and the
// decoder cannot pass unnoticed (the common-mode trap).
//
// Extension levels other than 1, 2 and 5 are not defined by ETSI V1.1.1. Their
// tunnel containers here (big-endian 16-bit fields, like L1/L2/L5) follow VP's
// decoder assumptions (L6/L8/L11) and are unverified until a real capture is
// checked (docs/VP-dv-tunnel-autotest.md, Tier 3).
#include <array>
#include <cstdint>
#include <vector>

namespace DvLoopback
{
	using Packet = std::array<uint8_t, 128>;

	constexpr size_t kPacketBytes = 128;
	constexpr size_t kPacketBits = 1024;
	constexpr size_t kCopies = 3;
	constexpr size_t kPixelsPerPacket = kPacketBits * kCopies;   // 3072
	constexpr size_t kFirstPacketCapacity = 119;
	constexpr size_t kNextPacketCapacity = 121;
	constexpr size_t kMaxMetadataBytes = 0x2F00;

	// A 12-bit IPT (ICtCp) 4:2:2 picture: I at every pixel, Ct and Cp at every
	// second pixel (width / 2 per row). Width must be even.
	struct IptPicture
	{
		uint32_t width = 0, height = 0;
		std::vector<uint16_t> i, ct, cp;

		void Resize(uint32_t w, uint32_t h)
		{
			width = w;
			height = h;
			i.assign(static_cast<size_t>(w) * h, 0);
			ct.assign(static_cast<size_t>(w / 2) * h, 2048);
			cp.assign(static_cast<size_t>(w / 2) * h, 2048);
		}
		size_t Pixels() const { return static_cast<size_t>(width) * height; }
		// The chroma sample carried by raster pixel p (Ct on even x, Cp on odd x).
		uint16_t& Chroma(size_t p)
		{
			const size_t x = p % width, y = p / width;
			std::vector<uint16_t>& plane = (x & 1) ? cp : ct;
			return plane[y * (width / 2) + x / 2];
		}
		uint16_t Chroma(size_t p) const { return const_cast<IptPicture*>(this)->Chroma(p); }
	};

	// One extension block: ext_block_level + payload (ext_block_length bytes).
	struct DmExtBlock
	{
		uint8_t level = 0;
		std::vector<uint8_t> payload;
	};

	// dm_metadata() (ETSI Table 3). Names follow libdovi's vdr_dm_data where
	// ETSI calls the byte reserved (bytes 62/63 = signal_chroma_format /
	// signal_full_range_flag, 68-69 = source_diagonal).
	struct DmFields
	{
		uint8_t reserved0 = 0;                 // byte 0
		uint8_t sceneRefresh = 1;              // byte 1
		int16_t yccToRgb[9] = {};              // bytes 2-19, Short()
		uint32_t yccToRgbOffset[3] = {};       // bytes 20-31
		int16_t rgbToLms[9] = {};              // bytes 32-49, Short()
		uint16_t signalEotf = 0xFFFF;          // bytes 50-51 (ETSI reserved 0xFF 0xFF)
		uint8_t eotfParams[8] = {};            // bytes 52-59 (ETSI reserved 0x00 x 8)
		uint8_t signalBitDepth = 12;           // byte 60
		uint8_t signalColorSpace = 2;          // byte 61 (ETSI Table 6: 0 YCbCr, 2 ICtCp)
		uint8_t signalChromaFormat = 1;        // byte 62 (ETSI reserved 0x01)
		uint8_t signalFullRange = 1;           // byte 63 (ETSI reserved 0x01)
		uint16_t sourceMinPq = 62;             // bytes 64-65 (ETSI default 0x003E)
		uint16_t sourceMaxPq = 3696;           // bytes 66-67 (ETSI default 0x0E70)
		uint16_t sourceDiagonal = 42;          // bytes 68-69 (ETSI reserved 0x00 0x2A)
		std::vector<DmExtBlock> ext;           // byte 70 = ext.size(), then the blocks

		// An IPTPQc2 (profile 5 style) representation: libdovi's profile-5
		// preset matrices. Any 12-bit opponent-colour representation works.
		static DmFields Ipt()
		{
			DmFields dm;
			const int16_t ycc[9] = { 8192, 799, 1681, 8192, -933, 1091, 8192, 267, -5545 };
			const int16_t lms[9] = { 17081, -349, -349, -349, 17081, -349, -349, -349, 17081 };
			for (int k = 0; k < 9; ++k)
			{
				dm.yccToRgb[k] = ycc[k];
				dm.rgbToLms[k] = lms[k];
			}
			dm.yccToRgbOffset[0] = 0;
			dm.yccToRgbOffset[1] = dm.yccToRgbOffset[2] = 1U << 27;
			return dm;
		}
	};

	inline void PutBe16(std::vector<uint8_t>& out, uint16_t v)
	{
		out.push_back(static_cast<uint8_t>(v >> 8));
		out.push_back(static_cast<uint8_t>(v & 0xFF));
	}

	inline void PutBe32(std::vector<uint8_t>& out, uint32_t v)
	{
		for (int shift = 24; shift >= 0; shift -= 8)
			out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
	}

	// Extension payloads (ETSI Table 5 for L1, L2, L5; VP assumptions otherwise).
	namespace Ext
	{
		inline DmExtBlock L1(uint16_t minPq, uint16_t maxPq, uint16_t avgPq)
		{
			DmExtBlock b; b.level = 1;
			PutBe16(b.payload, minPq); PutBe16(b.payload, maxPq); PutBe16(b.payload, avgPq);
			return b;
		}
		inline DmExtBlock L2(uint16_t targetMaxPq, uint16_t slope, uint16_t offset, uint16_t power,
			uint16_t chromaWeight, uint16_t saturationGain, int16_t msWeight)
		{
			DmExtBlock b; b.level = 2;
			for (uint16_t v : { targetMaxPq, slope, offset, power, chromaWeight, saturationGain,
				static_cast<uint16_t>(msWeight) })
				PutBe16(b.payload, v);
			return b;
		}
		inline DmExtBlock L5(uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
		{
			DmExtBlock b; b.level = 5;
			for (uint16_t v : { left, right, top, bottom })
				PutBe16(b.payload, v);
			return b;
		}
		inline DmExtBlock L6(uint16_t maxMastering, uint16_t minMastering, uint16_t maxCll, uint16_t maxFall)
		{
			DmExtBlock b; b.level = 6;
			for (uint16_t v : { maxMastering, minMastering, maxCll, maxFall })
				PutBe16(b.payload, v);
			return b;
		}
		// L8: target_display_index (u8) + slope, offset, power, chroma_weight,
		// saturation_gain, ms_weight (16 bit) [+ mid_contrast, clip_trim]
		// [+ 6 saturation bytes] [+ 6 hue bytes].
		inline DmExtBlock L8(uint8_t targetIndex, uint16_t slope, uint16_t offset, uint16_t power,
			uint16_t chromaWeight, uint16_t saturationGain, int16_t msWeight,
			int extras = 0, uint16_t midContrast = 2048, uint16_t clipTrim = 2048,
			const uint8_t* saturation = nullptr, const uint8_t* hue = nullptr)
		{
			DmExtBlock b; b.level = 8;
			b.payload.push_back(targetIndex);
			for (uint16_t v : { slope, offset, power, chromaWeight, saturationGain,
				static_cast<uint16_t>(msWeight) })
				PutBe16(b.payload, v);
			if (extras >= 1) { PutBe16(b.payload, midContrast); PutBe16(b.payload, clipTrim); }
			for (int k = 0; extras >= 2 && k < 6; ++k) b.payload.push_back(saturation ? saturation[k] : 0);
			for (int k = 0; extras >= 3 && k < 6; ++k) b.payload.push_back(hue ? hue[k] : 0);
			return b;
		}
		inline DmExtBlock L11(uint8_t contentType, uint8_t whitepoint, bool referenceMode,
			uint8_t byte2 = 0, uint8_t byte3 = 0)
		{
			DmExtBlock b; b.level = 11;
			b.payload = { contentType, static_cast<uint8_t>((whitepoint & 0x0F) | (referenceMode ? 0x10 : 0)),
				byte2, byte3 };
			return b;
		}
		inline DmExtBlock Raw(uint8_t level, std::vector<uint8_t> bytes)
		{
			DmExtBlock b; b.level = level; b.payload = std::move(bytes);
			return b;
		}
	}

	// ETSI Table 3 / Table 4 serialisation.
	inline std::vector<uint8_t> SerializeDm(const DmFields& dm)
	{
		std::vector<uint8_t> out;
		out.reserve(71 + dm.ext.size() * 16);
		out.push_back(dm.reserved0);
		out.push_back(dm.sceneRefresh);
		for (int16_t c : dm.yccToRgb) PutBe16(out, static_cast<uint16_t>(c));
		for (uint32_t o : dm.yccToRgbOffset) PutBe32(out, o);
		for (int16_t c : dm.rgbToLms) PutBe16(out, static_cast<uint16_t>(c));
		PutBe16(out, dm.signalEotf);
		for (uint8_t p : dm.eotfParams) out.push_back(p);
		out.push_back(dm.signalBitDepth);
		out.push_back(dm.signalColorSpace);
		out.push_back(dm.signalChromaFormat);
		out.push_back(dm.signalFullRange);
		PutBe16(out, dm.sourceMinPq);
		PutBe16(out, dm.sourceMaxPq);
		PutBe16(out, dm.sourceDiagonal);
		out.push_back(static_cast<uint8_t>(dm.ext.size()));
		for (const DmExtBlock& block : dm.ext)
		{
			PutBe32(out, static_cast<uint32_t>(block.payload.size()));
			out.push_back(block.level);
			out.insert(out.end(), block.payload.begin(), block.payload.end());
		}
		return out;
	}

	// ISO/IEC 13818-1 Annex A CRC-32 (MSB first, init all ones, no final XOR),
	// table driven.
	inline uint32_t Crc32Mpeg2(const uint8_t* bytes, size_t count)
	{
		static const std::array<uint32_t, 256> table = [] {
			std::array<uint32_t, 256> t{};
			for (uint32_t n = 0; n < 256; ++n)
			{
				uint32_t c = n << 24;
				for (int k = 0; k < 8; ++k)
					c = (c & 0x80000000U) ? (c << 1) ^ 0x04C11DB7U : (c << 1);
				t[n] = c;
			}
			return t;
		}();
		uint32_t crc = 0xFFFFFFFFU;
		for (size_t k = 0; k < count; ++k)
			crc = (crc << 8) ^ table[((crc >> 24) ^ bytes[k]) & 0xFF];
		return crc;
	}

	struct PacketHeader
	{
		uint8_t affectedId = 0;     // current or current + 1 (mod 16)
		uint8_t currentId = 0;
		bool noMetadata = false;
		bool eos = false;
	};

	// ETSI Tables 8-13: packet_type, metadata_type 0, metadata_version 0,
	// no_md; ids; EOS; metadata_length in the first packet; zero trailing
	// bytes; CRC-32 over the first 124 bytes in the tail.
	inline std::vector<Packet> Packetize(const std::vector<uint8_t>& body, const PacketHeader& header)
	{
		std::vector<Packet> packets;
		const size_t length = header.noMetadata ? 0 : body.size();
		const size_t count = (header.noMetadata || length <= kFirstPacketCapacity) ? 1 :
			1 + (length - kFirstPacketCapacity + kNextPacketCapacity - 1) / kNextPacketCapacity;
		size_t offset = 0;
		for (size_t index = 0; index < count; ++index)
		{
			Packet p{};
			const uint8_t type = count == 1 ? 0 : index == 0 ? 1 : index + 1 == count ? 3 : 2;
			p[0] = static_cast<uint8_t>((type << 6) | (header.noMetadata ? 1 : 0));
			p[1] = static_cast<uint8_t>(((header.affectedId & 15) << 4) | (header.currentId & 15));
			p[2] = header.eos ? 1 : 0;
			size_t at = 3;
			if (index == 0 && !header.noMetadata)
			{
				p[3] = static_cast<uint8_t>(length >> 8);
				p[4] = static_cast<uint8_t>(length & 0xFF);
				at = 5;
			}
			const size_t room = 124 - at;
			const size_t chunk = header.noMetadata ? 0 : (length - offset < room ? length - offset : room);
			for (size_t k = 0; k < chunk; ++k)
				p[at + k] = body[offset + k];
			offset += chunk;
			const uint32_t crc = Crc32Mpeg2(p.data(), 124);
			p[124] = static_cast<uint8_t>(crc >> 24);
			p[125] = static_cast<uint8_t>(crc >> 16);
			p[126] = static_cast<uint8_t>(crc >> 8);
			p[127] = static_cast<uint8_t>(crc);
			packets.push_back(p);
		}
		return packets;
	}

	inline unsigned Parity12(unsigned v)
	{
		v &= 0xFFFU;
		v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
		return v & 1U;
	}

	// Writes the scrambled packet bit into the chroma LSB of one pixel
	// (ETSI 6.4.2): LSB = bit ^ parity(C[11:1]) ^ parity(Y[11:0]).
	inline void EmbedBit(IptPicture& picture, size_t pixel, unsigned bit)
	{
		uint16_t& c = picture.Chroma(pixel);
		const unsigned scramble = Parity12(c & 0xFFEU) ^ Parity12(picture.i[pixel]);
		c = static_cast<uint16_t>((c & 0xFFEU) | ((bit ^ scramble) & 1U));
	}

	// Embeds every packet (three copies each, ETSI 6.4.3/6.4.4). Returns false,
	// changing nothing, when the raster has too few pixels.
	inline bool EmbedPackets(IptPicture& picture, const std::vector<Packet>& packets)
	{
		if (packets.size() * kPixelsPerPacket > picture.Pixels())
			return false;
		for (size_t p = 0; p < packets.size(); ++p)
			for (size_t copy = 0; copy < kCopies; ++copy)
				for (size_t n = 0; n < kPacketBytes; ++n)
					for (int m = 7; m >= 0; --m)
					{
						const size_t i = p * kPixelsPerPacket + copy * kPacketBits + n * 8 + (7 - m);
						EmbedBit(picture, i, (packets[p][n] >> m) & 1U);
					}
		return true;
	}

	// The HDMI RGB 4:4:4 8-bit picture (R, G, B per pixel, no padding).
	inline void PackRgb8(const IptPicture& picture, std::vector<uint8_t>& rgb)
	{
		rgb.resize(picture.Pixels() * 3);
		for (size_t p = 0; p < picture.Pixels(); ++p)
		{
			const unsigned y = picture.i[p] & 0xFFFU, c = picture.Chroma(p) & 0xFFFU;
			rgb[p * 3 + 0] = static_cast<uint8_t>(c >> 4);
			rgb[p * 3 + 1] = static_cast<uint8_t>(y >> 4);
			rgb[p * 3 + 2] = static_cast<uint8_t>(((c & 15U) << 4) | (y & 15U));
		}
	}

	// The two planes VP's tunnel formatter writes for this picture: the I plane
	// (code << 4) and the alternating chroma plane (code << 4), full width.
	inline void FormatterPlanes(const IptPicture& picture, std::vector<uint16_t>& planes)
	{
		const size_t n = picture.Pixels();
		planes.resize(n * 2);
		for (size_t p = 0; p < n; ++p)
		{
			planes[p] = static_cast<uint16_t>((picture.i[p] & 0xFFFU) << 4);
			planes[n + p] = static_cast<uint16_t>((picture.Chroma(p) & 0xFFFU) << 4);
		}
	}

	// One complete tunnel picture: DM -> packets -> embedded -> RGB8.
	inline bool BuildTunnelRgb8(IptPicture picture, const DmFields& dm, const PacketHeader& header,
		std::vector<uint8_t>& rgb, IptPicture* embedded = nullptr)
	{
		const std::vector<Packet> packets = Packetize(SerializeDm(dm), header);
		if (!EmbedPackets(picture, packets))
			return false;
		PackRgb8(picture, rgb);
		if (embedded)
			*embedded = std::move(picture);
		return true;
	}
}
