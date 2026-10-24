/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <pch.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <GeneralRateLog.h>
#include <CpuFeatures.h>
#include "CR210DolbyVisionTunnelFormatter.h"
#include <VP-V/modules/input/signal/DvSignalLevels.h>   // feat/dv-l8: L9 / L10

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace
{
	// HDMI 1.4a section 6.5.4: RGB8 tunnel -> 12-bit 4:2:2.
	// ETSI GS CCM 001 sections 6.3-6.4: parity-scrambled metadata packets.
	constexpr size_t kPacketBits = 1024;
	constexpr size_t kPacketBytes = 128;
	constexpr size_t kRepeats = 3;
	constexpr size_t kMaxMetadataBytes = 0x2F00;
	constexpr uint32_t kDiagnosticInterval = 600;
	// audit 2026-10-16: time-based limits, converted to pictures from the
	// display mode's measured rate (never a fixed frame count). The loss
	// limit equals the original 24 pictures at 23.976 Hz.
	constexpr double kPacketLossSeconds = 24.0 * 1001.0 / 24000.0;
	// Black is shown for this long before a header-less, non-tunnel-looking
	// signal is accepted as SDR (same-mode SDR switch without EOS).
	constexpr double kSdrFallbackSeconds = 2.0;
	// ETSI metadata IDs are 4-bit and wrap after 16 values. The sender can
	// advance the current ID by at most one per picture (affected is current
	// or current+1), so an ID equality across 16 or more pictures without a
	// header cannot prove continuity: the ID may have wrapped back.
	constexpr uint32_t kMetadataIdModulus = 16;

	// Modular (mod-16) forward distance from one metadata ID to another.
	uint8_t MetadataIdDistance(uint8_t from, uint8_t to)
	{
		return static_cast<uint8_t>((to - from) & (kMetadataIdModulus - 1U));
	}

	uint32_t ReadLittleEndian32(const uint8_t* bytes)
	{
		return (static_cast<uint32_t>(bytes[3]) << 24) |
			(static_cast<uint32_t>(bytes[2]) << 16) |
			(static_cast<uint32_t>(bytes[1]) << 8) |
			static_cast<uint32_t>(bytes[0]);
	}

	uint32_t ReadBigEndian32(const uint8_t* bytes)
	{
		return (static_cast<uint32_t>(bytes[0]) << 24) |
			(static_cast<uint32_t>(bytes[1]) << 16) |
			(static_cast<uint32_t>(bytes[2]) << 8) |
			static_cast<uint32_t>(bytes[3]);
	}

	int16_t ReadSigned16(const uint8_t* bytes)
	{
		return static_cast<int16_t>(
			(static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
	}

	uint16_t ReadUnsigned16(const uint8_t* bytes)
	{
		return static_cast<uint16_t>(
			(static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
	}

	uint8_t Parity(uint32_t bits)
	{
		bits ^= bits >> 16;
		bits ^= bits >> 8;
		bits ^= bits >> 4;
		bits ^= bits >> 2;
		bits ^= bits >> 1;
		return static_cast<uint8_t>(bits & 1U);
	}

	uint32_t PacketCrc(const uint8_t* bytes)
	{
		// ISO/IEC 13818-1 CRC-32, specified by ETSI for the 128-byte packet.
		uint32_t crc = 0xFFFFFFFFU;
		for (size_t i = 0; i < kPacketBytes; ++i)
		{
			crc ^= static_cast<uint32_t>(bytes[i]) << 24;
			for (int bit = 0; bit < 8; ++bit)
				crc = (crc & 0x80000000U) ?
					(crc << 1) ^ 0x04C11DB7U : crc << 1;
		}
		return crc;
	}

	struct Rgb8 { uint8_t r, g, b; };

	Rgb8 DecodeRgb(uint32_t packed, bool leftAligned)
	{
		const unsigned shift = leftAligned ? 2U : 0U;
		return {
			static_cast<uint8_t>(((packed >> 20) & 0x3FFU) >> shift),
			static_cast<uint8_t>(((packed >> 10) & 0x3FFU) >> shift),
			static_cast<uint8_t>((packed & 0x3FFU) >> shift)
		};
	}

	// Equivalent to the original RGB8 unpack and 12-bit I/C reconstruction.
	// Keep each shift constant inside the hot loop for the scalar fallback.
	// audit 2026-10-16: Shift 0/2 = r210 low8/high8; 2/4 = R10b/R10l
	// low8/high8 (R10 word = r210 word << 2). LittleEndian selects R10l.
	template<unsigned Shift, bool LittleEndian = false>
	void ConvertRowsScalar(const uint8_t* source, uint16_t* output,
		uint32_t width, uint32_t height, uint32_t stride,
		uint32_t firstRow, uint32_t rowCount)
	{
		const size_t planePixels = static_cast<size_t>(width) * height;
		for (uint32_t row = firstRow; row < firstRow + rowCount; ++row)
		{
			const uint8_t* input = source + static_cast<size_t>(row) * stride;
			uint16_t* y = output + static_cast<size_t>(row) * width;
			uint16_t* uv = output + planePixels + static_cast<size_t>(row) * width;
			for (uint32_t x = 0; x < width; ++x)
			{
				const uint32_t p = LittleEndian ?
					ReadLittleEndian32(input + static_cast<size_t>(x) * 4) :
					ReadBigEndian32(input + static_cast<size_t>(x) * 4);
				y[x] = static_cast<uint16_t>(
					((p >> (2 + Shift)) & 0xFF00U) |
					((p << (4 - Shift)) & 0xF0U));
				uv[x] = static_cast<uint16_t>(
					((p >> (12 + Shift)) & 0xFF00U) |
					((p >> Shift) & 0xF0U));
			}
		}
	}

	struct ParsedPacket
	{
		DolbyVisionTunnelFrameMetadata metadata;
		uint8_t affectedId = 0;
		uint8_t currentId = 0;
		bool noMetadata = false;
		bool endOfStream = false;
	};

	// Header validation is separate from metadata decoding: a CRC-valid header
	// proves a DV transport even when a later packet/body is damaged.
	bool ParseHeader(const uint8_t* packet, ParsedPacket& result)
	{
		if ((packet[0] & 0x3EU) != 0 || (packet[2] & 0xFEU) != 0)
			return false;
		result.affectedId = packet[1] >> 4;
		result.currentId = packet[1] & 15U;
		result.endOfStream = (packet[2] & 1U) != 0;
		result.noMetadata = (packet[0] & 1U) != 0;
		// audit 2026-10-16: modular comparison (15 -> 0 is the next ID).
		return MetadataIdDistance(result.currentId, result.affectedId) <= 1U;
	}

	// audit 2026-10-16: first L2/L8 payload bytes, for one log line per title
	// so a live run can verify the assumed container layout.
	struct RawTrimPayload
	{
		bool present = false;
		size_t length = 0;
		uint8_t bytes[32] = {};
	};

	// audit 2026-10-16: generic L2/L8 trim parser (not applied). Containers
	// follow the existing L1 convention: big-endian 16-bit fields.
	//   L2: target_max_PQ, slope, offset, power, chroma_weight,
	//       saturation_gain, ms_weight(signed)                     = 14 bytes
	//   L8: target_display_index(u8), slope, offset, power, chroma_weight,
	//       saturation_gain, ms_weight(signed)                     = 13 bytes
	//       [+ target_mid_contrast, clip_trim] [+ 6 saturation] [+ 6 hue]
	// A short/odd trim block is skipped; it never rejects the DM.
	bool ParseTrim(const uint8_t* payload, size_t length, uint8_t level,
		DolbyVisionTunnelFrameMetadata::Trim& trim)
	{
		trim = DolbyVisionTunnelFrameMetadata::Trim();
		trim.level = level;
		size_t at = 0;
		if (level == 2)
		{
			if (length < 14)
				return false;
			trim.targetMaxPq = ReadUnsigned16(payload);
			at = 2;
		}
		else
		{
			if (length < 13)
				return false;
			trim.targetDisplayIndex = payload[0];
			at = 1;
		}
		trim.slope = ReadUnsigned16(payload + at);
		trim.offset = ReadUnsigned16(payload + at + 2);
		trim.power = ReadUnsigned16(payload + at + 4);
		trim.chromaWeight = ReadUnsigned16(payload + at + 6);
		trim.saturationGain = ReadUnsigned16(payload + at + 8);
		trim.msWeight = ReadSigned16(payload + at + 10);
		at += 12;
		if (level == 8)
		{
			if (length >= at + 4)
			{
				trim.hasMidContrastAndClip = true;
				trim.midContrast = ReadUnsigned16(payload + at);
				trim.clipTrim = ReadUnsigned16(payload + at + 2);
				at += 4;
			}
			if (length >= at + 6)
			{
				trim.hasSaturationVector = true;
				memcpy(trim.saturationVector, payload + at, 6);
				at += 6;
			}
			if (length >= at + 6)
			{
				trim.hasHueVector = true;
				memcpy(trim.hueVector, payload + at, 6);
			}
		}
		return true;
	}

	// Content intent (intent interpreter item 1, 2026-10-17): L5 / L6 / L11.
	// Field order and widths follow libdovi's public parser (quietvoid/dovi_tool,
	// dolby_vision/src/rpu/extension/):
	//   level5.rs:  active_area_left/right/top/bottom_offset, 13 bits each
	//               (RPU ext_block_length 7);
	//   level6.rs:  max_display_mastering_luminance, min_display_mastering_
	//               luminance, max_content_light_level, max_frame_average_
	//               light_level, 16 bits each (length 8);
	//   level11.rs: content_type (byte 0), whitepoint (byte 1, bit 4 =
	//               reference_mode_flag), reserved_byte2, reserved_byte3
	//               (length 4).
	// The tunnel's DM carries L1 in 16-bit containers (6 bytes, where the RPU
	// packs 3 x 12 bits in 5), so L5 is read from four big-endian 16-bit
	// containers (8 bytes) and, failing that, from the RPU's packed 7-byte
	// form. Bytes 2-3 of L11 are decoded as the intended picture-mode hints
	// (sharpness, noise reduction, MPEG noise reduction, frame-rate conversion;
	// brightness, colour; 2 bits each, MSB first) - an assumption until a live
	// packet is checked (the raw bytes are logged once per title). Each block
	// is bounds-checked; a short or out-of-range block is skipped and never
	// rejects the DM (the trim convention). The first block of a level wins.
	bool ParseActiveArea(const uint8_t* payload, size_t length,
		DolbyVisionTunnelFrameMetadata& parsed)
	{
		uint16_t offsets[4] = {};
		if (length >= 8)
		{
			for (size_t i = 0; i < 4; ++i)
				offsets[i] = ReadUnsigned16(payload + i * 2);
		}
		else if (length == 7)
		{
			// 52 bits, MSB first: four 13-bit fields, then 4 padding bits.
			uint64_t bits = 0;
			for (size_t i = 0; i < 7; ++i)
				bits = (bits << 8) | payload[i];
			bits >>= 4;
			for (size_t i = 0; i < 4; ++i)
				offsets[i] = static_cast<uint16_t>((bits >> (13 * (3 - i))) & 0x1FFFU);
		}
		else
			return false;
		for (const uint16_t offset : offsets)
			if (offset > 0x1FFFU)
				return false;
		parsed.hasActiveArea = true;
		parsed.activeAreaLeft = offsets[0];
		parsed.activeAreaRight = offsets[1];
		parsed.activeAreaTop = offsets[2];
		parsed.activeAreaBottom = offsets[3];
		return true;
	}

	bool ParseL6(const uint8_t* payload, size_t length,
		DolbyVisionTunnelFrameMetadata& parsed)
	{
		if (length < 8)
			return false;
		const uint16_t maxMastering = ReadUnsigned16(payload);
		const uint16_t minMastering = ReadUnsigned16(payload + 2);
		const uint16_t maxCll = ReadUnsigned16(payload + 4);
		const uint16_t maxFall = ReadUnsigned16(payload + 6);
		// ST 2084 tops out at 10000 nits; larger values are corrupt.
		if (maxMastering > 10000 || maxCll > 10000 || maxFall > 10000)
			return false;
		parsed.hasL6 = true;
		parsed.l6MaxDisplayMasteringLuminance = maxMastering;
		parsed.l6MinDisplayMasteringLuminance = minMastering;
		// 0 = unknown (CTA-861.3). MaxFALL above MaxCLL cannot be: both dropped.
		parsed.hasContentLight = maxCll > 0 && maxFall <= maxCll;
		parsed.maxCll = parsed.hasContentLight ? maxCll : 0;
		parsed.maxFall = parsed.hasContentLight ? maxFall : 0;
		return true;
	}

	bool ParseL11(const uint8_t* payload, size_t length,
		DolbyVisionTunnelFrameMetadata& parsed)
	{
		if (length < 4)
			return false;
		parsed.hasL11 = true;
		memcpy(parsed.l11Raw, payload, 4);
		parsed.l11ContentType = payload[0] & 0x0FU;
		parsed.l11Whitepoint = payload[1] & 0x0FU;
		parsed.l11ReferenceMode = (payload[1] & 0x10U) != 0;
		parsed.l11Sharpness = (payload[2] >> 6) & 3U;
		parsed.l11NoiseReduction = (payload[2] >> 4) & 3U;
		parsed.l11MpegNoiseReduction = (payload[2] >> 2) & 3U;
		parsed.l11FrameRateConversion = payload[2] & 3U;
		parsed.l11Brightness = (payload[3] >> 6) & 3U;
		parsed.l11Color = (payload[3] >> 4) & 3U;
		return true;
	}

	const char* L11ContentTypeName(uint8_t type)
	{
		switch (type)
		{
		case 0: return "default";
		case 1: return "movies";
		case 2: return "game";
		case 3: return "sport";
		case 4: return "user-generated";
		default: return "reserved";
		}
	}

	bool ParseMetadata(const uint8_t* dm, size_t length, ParsedPacket& result,
		std::array<uint16_t, 256>& extensionLevels, RawTrimPayload* rawTrims, bool l2First)
	{
		if (length < 71)
			return false;
		if (dm[60] != 12 || (dm[61] != 0 && dm[61] != 2))
			return false; // This unpacker accepts 12-bit opponent color only.
		DolbyVisionTunnelFrameMetadata parsed;
		parsed.valid = true;
		parsed.fullRange = dm[63] != 0;
		parsed.sourceMinPq = ReadUnsigned16(dm + 64);
		parsed.sourceMaxPq = ReadUnsigned16(dm + 66);
		for (size_t row = 0; row < 3; ++row)
		{
			for (size_t col = 0; col < 3; ++col)
			{
				const size_t index = row * 3 + col;
				parsed.nonlinear[row][col] =
					static_cast<float>(ReadSigned16(dm + 2 + index * 2)) /
					8192.0f;
				parsed.linear[row][col] =
					static_cast<float>(ReadSigned16(dm + 32 + index * 2)) /
					16384.0f;
			}
			parsed.nonlinearOffset[row] =
				static_cast<float>(ReadBigEndian32(dm + 20 + row * 4)) /
				268435456.0f;
		}
		// Reassembly has already validated the complete declared body. Unknown
		// extension levels are bounded/skipped; they are not display trims.
		size_t extension = 71;
		DolbyVisionTunnelFrameMetadata::Trim l8Trims[DolbyVisionTunnelFrameMetadata::kMaxTrims];
		uint8_t l8Count = 0; // Bug hunt 2026-10-17: L8 after L2, see below
		for (unsigned block = 0; block < dm[70]; ++block)
		{
			if (length - extension < 5)
				return false;
			const uint32_t payloadLength = ReadBigEndian32(dm + extension);
			const uint8_t level = dm[extension + 4];
			if (payloadLength > length - extension - 5)
				return false;
			const size_t end = extension + 5 + payloadLength;
			if (extensionLevels[level] != (std::numeric_limits<uint16_t>::max)())
				++extensionLevels[level]; // audit 2026-10-16: block count per level
			if (level == 2 || level == 8)
			{
				const uint8_t* payload = dm + extension + 5;
				RawTrimPayload& raw = rawTrims[level == 2 ? 0 : 1];
				if (!raw.present)
				{
					raw.present = true;
					raw.length = payloadLength;
					memcpy(raw.bytes, payload, (std::min)(static_cast<size_t>(payloadLength),
						sizeof(raw.bytes)));
				}
				// Bug hunt 2026-10-17: L2 trims (the ones dv_trims applies)
				// take the slots first; L8 blocks are held aside and fill only
				// the slots left after the last block, so a DM carrying many
				// L8 blocks before its L2 blocks can no longer crowd L2 out.
				if (!l2First)
				{
					// dv_tunnel_fixes off: the slots in block order.
					if (parsed.trimCount >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
						parsed.trimOverflow = true;
					else if (ParseTrim(payload, payloadLength, level,
						parsed.trims[parsed.trimCount]))
						++parsed.trimCount;
				}
				else if (level == 2)
				{
					if (parsed.trimCount >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
						parsed.trimOverflow = true;
					else if (ParseTrim(payload, payloadLength, level,
						parsed.trims[parsed.trimCount]))
						++parsed.trimCount;
				}
				else if (l8Count >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
					parsed.trimOverflow = true;
				else if (ParseTrim(payload, payloadLength, level, l8Trims[l8Count]))
					++l8Count;
			}
			if (level == 1)
			{
				if (payloadLength != 6)
					return false;
				const uint16_t sceneMin = ReadUnsigned16(dm + extension + 5);
				const uint16_t sceneMax = ReadUnsigned16(dm + extension + 7);
				const uint16_t sceneAvg = ReadUnsigned16(dm + extension + 9);
				if (sceneMin > 4095 || sceneMax > 4095 || sceneAvg > 4095)
					return false;
				parsed.hasSceneMetadata = true;
				parsed.sceneMinPq = sceneMin; // audit 2026-10-16: no longer dropped
				parsed.sceneMaxPq = sceneMax;
				parsed.sceneAvgPq = sceneAvg;
			}
			// Content intent 2026-10-17: L5 / L6 / L11 (the first block of each;
			// the payload is inside the validated body: payloadLength bytes).
			if (level == 5 && !parsed.hasActiveArea)
				ParseActiveArea(dm + extension + 5, payloadLength, parsed);
			else if (level == 6 && !parsed.hasL6)
				ParseL6(dm + extension + 5, payloadLength, parsed);
			else if (level == 11 && !parsed.hasL11)
				ParseL11(dm + extension + 5, payloadLength, parsed);
			else if (level == 10)   // feat/dv-l8: the targets L8 refers to
				DvSignalLevels::ParseL10(dm + extension + 5, payloadLength, parsed);
			else if (level == 9)
				DvSignalLevels::ParseL9(dm + extension + 5, payloadLength, parsed);
			extension = end;
		}
		for (uint8_t i = 0; i < l8Count; ++i)
		{
			if (parsed.trimCount >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
			{
				parsed.trimOverflow = true;
				break;
			}
			parsed.trims[parsed.trimCount++] = l8Trims[i];
		}
		result.metadata = parsed;
		return true;
	}

	// audit 2026-10-16: SMPTE ST 2084 code -> nits, for log lines only.
	double PqCodeToNits(uint16_t code)
	{
		const double m1 = 0.1593017578125, m2 = 78.84375;
		const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
		const double p = std::pow(static_cast<double>(code) / 4095.0, 1.0 / m2);
		const double numerator = (std::max)(p - c1, 0.0);
		return 10000.0 * std::pow(numerator / (c2 - c3 * p), 1.0 / m1);
	}

	std::string FormatLevelCount(uint16_t count)
	{
		if (count == 0)
			return "no";
		if (count == 1)
			return "yes";
		char text[16];
		snprintf(text, sizeof(text), "x%u", static_cast<unsigned>(count));
		return text;
	}

	std::string HexBytes(const uint8_t* bytes, size_t count)
	{
		std::string text;
		char item[4];
		for (size_t i = 0; i < count; ++i)
		{
			snprintf(item, sizeof(item), "%02X", bytes[i]);
			text += item;
		}
		return text;
	}

	// DV input fixes 2026-10-18: the word reader, the packet reader, the
	// placement votes and the tunnel-content cue moved out of the members
	// unchanged, so the static probes (HasTunnelHeader, ClassifyRgbContent,
	// LooksLikeTunnelContent, LastRowBlank; the GUI's tunnel hand-off) apply
	// the formatter's own rules to a raster without formatter state.
	enum class TunnelWordLayout { R210, R10_BIG_ENDIAN, R10_LITTLE_ENDIAN };

	TunnelWordLayout LayoutOf(VideoFrameEncoding encoding)
	{
		return encoding == VideoFrameEncoding::R10b ? TunnelWordLayout::R10_BIG_ENDIAN :
			encoding == VideoFrameEncoding::R10l ? TunnelWordLayout::R10_LITTLE_ENDIAN :
			TunnelWordLayout::R210;
	}

	struct TunnelWordReader
	{
		const uint8_t* source = nullptr;
		uint32_t width = 0;
		uint32_t stride = 0;
		TunnelWordLayout layout = TunnelWordLayout::R210;

		// audit 2026-10-16: returns the r210 layout [00 R10 G10 B10] for every
		// supported container (R10 words carry two padding bits at the bottom).
		uint32_t operator()(size_t pixel) const
		{
			const size_t y = pixel / width;
			const size_t x = pixel % width;
			const uint8_t* bytes = source + y * stride + x * 4;
			if (layout == TunnelWordLayout::R10_BIG_ENDIAN)
				return ReadBigEndian32(bytes) >> 2;
			if (layout == TunnelWordLayout::R10_LITTLE_ENDIAN)
				return ReadLittleEndian32(bytes) >> 2;
			return ReadBigEndian32(bytes);
		}
	};

	struct PacketReadCounters
	{
		uint32_t crcFailedCopies = 0;
		uint32_t recovered = 0;
		uint32_t majority = 0;
	};

	bool ReadPacketCopiesWith(const TunnelWordReader& readWord, size_t index,
		bool leftAligned, uint8_t* packet, PacketReadCounters& counters)
	{
		uint8_t copies[kRepeats][kPacketBytes];
		for (size_t copy = 0; copy < kRepeats; ++copy)
		{
			uint8_t* bytes = copies[copy];
			memset(bytes, 0, kPacketBytes);
			for (size_t bit = 0; bit < kPacketBits; ++bit)
			{
				const Rgb8 rgb = DecodeRgb(readWord(
					(index * kRepeats + copy) * kPacketBits + bit), leftAligned);
				bytes[bit / 8] |= static_cast<uint8_t>(Parity(
					(static_cast<uint32_t>(rgb.r) << 16) |
					(static_cast<uint32_t>(rgb.g) << 8) | rgb.b) << (7 - bit % 8));
			}
			if (PacketCrc(bytes) == 0)
			{
				if (copy != 0)
					++counters.recovered;
				memcpy(packet, bytes, kPacketBytes);
				return true;
			}
			++counters.crcFailedCopies;
		}
		// audit 2026-10-16: when every copy has (different) bit errors, a
		// bitwise 2-of-3 vote can still rebuild the packet. The CRC remains the
		// only authority, so this never accepts an unverified packet.
		for (size_t i = 0; i < kPacketBytes; ++i)
		{
			const uint8_t a = copies[0][i], b = copies[1][i], c = copies[2][i];
			packet[i] = static_cast<uint8_t>((a & b) | (a & c) | (b & c));
		}
		if (PacketCrc(packet) == 0)
		{
			++counters.majority;
			return true;
		}
		return false;
	}

	// Bug hunt 2026-10-17 placement grid (dv_tunnel_fixes): a grid over the
	// whole raster; high8 never sets the low two bits of a 10-bit field, low8
	// never the high two; dark pixels vote for neither, and a pixel using
	// both is contradictory (10-bit content).
	struct PlacementVotes
	{
		size_t left = 0, right = 0, contradictory = 0;
		size_t NonDark() const { return left + right + contradictory; }
	};

	PlacementVotes CountPlacementVotes(const TunnelWordReader& readWord,
		uint32_t width, uint32_t height)
	{
		PlacementVotes votes;
		const uint32_t rows = (std::min)(height, 64U);
		const uint32_t columns = (std::min)(width, 256U);
		for (uint32_t r = 0; r < rows; ++r)
		{
			const size_t y = (static_cast<size_t>(r) * 2 + 1) * height / (rows * 2);
			for (uint32_t c = 0; c < columns; ++c)
			{
				const size_t x = (static_cast<size_t>(c) * 2 + 1) * width / (columns * 2);
				const uint32_t word = readWord(y * width + x);
				const uint32_t bits = ((word >> 20) | (word >> 10) | word) & 0x3FFU;
				const bool lowBits = (bits & 3U) != 0;     // impossible for high8
				const bool highBits = (bits & 0x300U) != 0; // impossible for low8
				if (highBits && !lowBits)
					++votes.left;
				else if (lowBits && !highBits)
					++votes.right;
				else if (lowBits && highBits)
					++votes.contradictory;
			}
		}
		return votes;
	}

	constexpr size_t kMinPlacementVotes = 16;

	bool LooksLikeTunnelWith(const TunnelWordReader& readWord, uint32_t width,
		uint32_t height, bool bugHuntFixes)
	{
		// audit 2026-10-16: cheap content check, evaluated only while a locked
		// tunnel has no CRC-valid header. In the RGB8 tunnel G = I[11:4] (smooth,
		// like luma), R = C[11:4] (near neutral 128 for most content) and
		// B = C[3:0]<<4 | I[3:0] (low nibbles: close to random between
		// neighbours; Cb/Cr alternate per pixel). Ordinary RGB has B about as
		// smooth as G, and R follows brightness. Black (all-zero) RGB fails both.
		// Conservative by design: a false "still tunnel" only prolongs black,
		// a false "not tunnel" would show packed pixels. Both byte placements are
		// evaluated (flat tunnel content can CRC-validate in either placement),
		// and either cue in either placement counts as tunnel content.
		if (width < 2 || height == 0)
			return true;
		const uint32_t rows = (std::min)(height, 64U);
		const uint32_t pairsPerRow = (std::min)(width / 2, 128U);
		uint64_t roughB[2] = {}, roughG[2] = {}, neutral[2] = {}, pairs = 0;
		uint64_t rDistanceFromG[2] = {}; // Bug hunt 2026-10-17: sum of |R - G|
		for (uint32_t r = 0; r < rows; ++r)
		{
			const size_t y = (static_cast<size_t>(r) * 2 + 1) * height / (rows * 2);
			for (uint32_t c = 0; c < pairsPerRow; ++c)
			{
				const size_t x = ((static_cast<size_t>(c) * 2 + 1) * (width / 2) /
					(pairsPerRow * 2)) * 2;
				const uint32_t words[2] = {
					readWord(y * width + x),
					readWord(y * width + x + 1) };
				++pairs;
				for (size_t placement = 0; placement < 2; ++placement)
				{
					const bool leftAligned = placement == 1;
					const Rgb8 first = DecodeRgb(words[0], leftAligned);
					const Rgb8 second = DecodeRgb(words[1], leftAligned);
					roughB[placement] += static_cast<uint64_t>(std::abs(
						static_cast<int>(first.b) - static_cast<int>(second.b)));
					roughG[placement] += static_cast<uint64_t>(std::abs(
						static_cast<int>(first.g) - static_cast<int>(second.g)));
					neutral[placement] +=
						(std::abs(static_cast<int>(first.r) - 128) <= 32 ? 1U : 0U) +
						(std::abs(static_cast<int>(second.r) - 128) <= 32 ? 1U : 0U);
					rDistanceFromG[placement] += static_cast<uint64_t>(
						std::abs(static_cast<int>(first.r) - static_cast<int>(first.g)) +
						std::abs(static_cast<int>(second.r) - static_cast<int>(second.g)));
				}
			}
		}
		if (pairs == 0)
			return true;
		for (size_t placement = 0; placement < 2; ++placement)
		{
			// Noisy low nibbles in B while G stays comparatively smooth.
			if (roughB[placement] >= 16 * pairs &&
				roughB[placement] >= 3 * roughG[placement] + pairs)
				return true;
			// Near-neutral R (tunnel chroma high byte) on most samples, at any
			// brightness. Bug hunt 2026-10-17: only when R does not follow G
			// (mean |R - G| > 32). Grey SDR (a menu after the tunnel stops) has
			// R = G and used to hold the SDR exit off indefinitely; tunnel black
			// has G near 0 with R near 128. Mid-grey tunnel content relies on the
			// noisy-B cue above.
			if (neutral[placement] * 10 >= pairs * 2 * 8 &&
				(!bugHuntFixes || rDistanceFromG[placement] > 32 * pairs * 2))
				return true;
		}
		return false;
	}

	// DV input fixes 2026-10-18 (dv_tunnel_fixes): a frame cut short (the
	// tail of the buffer never written: a truncated transfer) leaves the last
	// row all zero. Valid tunnel content cannot: an all-zero word is I = 0
	// with a chroma sample of 0 (the most negative opponent value), outside
	// every real picture - tunnel black is I near 0 with chroma near 2048
	// (R near 128). The whole last row is checked, so one plausible pixel
	// keeps the frame.
	bool LastRowAllZero(const TunnelWordReader& readWord, uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return false;
		const size_t first = static_cast<size_t>(height - 1) * width;
		for (size_t x = 0; x < width; ++x)
			if ((readWord(first + x) & 0x3FFFFFFFU) != 0)
				return false;
		return true;
	}

	TunnelWordLayout ToLayout(int wordLayout)
	{
		return static_cast<TunnelWordLayout>(wordLayout);
	}
}

CR210DolbyVisionTunnelFormatter::CR210DolbyVisionTunnelFormatter()
{
	m_hasAVX2 = CpuFeatures::SupportsAvx2Kernels();
	for (uint32_t i = 0; i < MAX_WORKERS; ++i)
	{
		m_conversionWork[i] = CreateThreadpoolWork(ConversionWorkCallback, this, nullptr);
		if (!m_conversionWork[i])
			break;
		++m_workerCount;
	}
}

CR210DolbyVisionTunnelFormatter::~CR210DolbyVisionTunnelFormatter()
{
	for (uint32_t i = 0; i < m_workerCount; ++i)
	{
		WaitForThreadpoolWorkCallbacks(m_conversionWork[i], TRUE);
		CloseThreadpoolWork(m_conversionWork[i]);
	}
}

void CR210DolbyVisionTunnelFormatter::OnVideoState(VideoStateComPtr& state)
{
	// audit 2026-10-16: R10b/R10l hold the same 10-bit RGB fields as r210,
	// two bits higher. Only a CRC-valid packet proves that the card stored
	// the tunnel's RGB8 bytes exactly; otherwise DV never locks.
	if (!state || !state->displayMode ||
		(state->videoFrameEncoding != VideoFrameEncoding::R210 &&
			state->videoFrameEncoding != VideoFrameEncoding::R10b &&
			state->videoFrameEncoding != VideoFrameEncoding::R10l) ||
		!state->dolbyVisionTunnel)
		throw std::runtime_error("Dolby Vision tunnel requires enabled r210, R10b or R10l capture");
	const auto width = state->displayMode->FrameWidth();
	const auto height = state->displayMode->FrameHeight();
	if (width <= 0 || height <= 0 || (width & 1) != 0)
		throw std::runtime_error("Dolby Vision tunnel requires positive even width");
	const uint64_t outputSize =
		static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4U;
	if (outputSize > static_cast<uint64_t>(std::numeric_limits<LONG>::max()))
		throw std::runtime_error("Dolby Vision tunnel output frame too large");
	m_width = static_cast<uint32_t>(width);
	m_height = static_cast<uint32_t>(height);
	m_stride = state->BytesPerRow();
	if (m_stride < m_width * 4U)
		throw std::runtime_error("R210 stride is shorter than the active width");
	m_outputSize = static_cast<LONG>(outputSize);
	m_alignment = ByteAlignment::UNKNOWN;
	m_placementProven = false;
	m_ambiguousPlacementPictures = 0;
	m_missingPacketFrames = 0;
	m_missingPacketReported = false;
	m_hasPacketLoss = false; // Bug hunt 2026-10-17
	m_packetLossStartCounter = 0;
	m_tunnelConfirmed = false;
	m_currentPictureHasValidHeader = false;
	m_diagnosticFrames = 0;
	m_crcFailedCopies = 0;
	m_recoveredPackets = 0;
	m_rejectedFrames = 0;
	m_noMetadataFrames = 0;
	m_lastPacketCount = 0;
	m_lastMetadataLength = 0;
	m_metadataIdTransitions = 0;
	m_lastCurrentId = m_lastAffectedId = 0;
	m_haveDiagnosticIds = false;
	m_seenExtensionLevels.fill(false);
	m_metadata = {};
	m_pendingMetadata = {};
	m_hasPendingMetadata = false;
	m_pendingAffectedId = 0;
	m_pendingCurrentId = 0;
	m_pendingAfterCounter = 0;
	m_activeMetadataId = 0;
	m_hasActiveMetadataId = false;
	m_endOfStreamAfterCounter = 0;
	m_hasEndOfStream = false;
	// audit 2026-10-16: container, timing and time-based limits come from
	// the display mode (any rate/raster the tunnel carries; no fixed 4K23.976).
	m_wordLayout = state->videoFrameEncoding == VideoFrameEncoding::R10b ?
		WordLayout::R10_BIG_ENDIAN :
		state->videoFrameEncoding == VideoFrameEncoding::R10l ?
		WordLayout::R10_LITTLE_ENDIAN : WordLayout::R210;
	// The tunnel is a progressive raster; a woven interlaced frame would
	// scramble the packet/raster order. Never lock DV on it (SDR only).
	m_tunnelCapable = !state->displayMode->IsInterlaced();
	const double rate = state->displayMode->RefreshRateHz();
	static GeneralRate::FallbackNotice fallback;   // fixed-constant batch B7 2026-10-18: logged
	m_frameRate = GeneralRate::NoteFallback(fallback, rate > 0, "CR210 DV tunnel packet-loss limits", 24000.0 / 1001.0) ?
		rate : 24000.0 / 1001.0;
	m_packetLossLimit = static_cast<uint32_t>((std::max)(1.0,
		std::floor(m_frameRate * kPacketLossSeconds + 0.5)));
	m_sdrFallbackFrames = static_cast<uint64_t>((std::max)(2.0,
		std::ceil(m_frameRate * kSdrFallbackSeconds - 1e-9)));
	m_dmHold.SetRate(m_frameRate);   // feat/dv-l8: the hold is a time at the measured rate
	m_dmHold.Reset();
	m_sdrSuspect = false;
	m_sdrSuspectStartCounter = 0;
	m_lastHeaderCounter = 0;
	m_hasLastHeaderCounter = false;
	m_idAmbiguityResets = 0;
	m_majorityRecoveries = 0;
	BeginTitle();
	DebugLog::Log("Dolby Vision RGB tunnel unpack: %s, row workers=%u",
		m_hasAVX2 ? "AVX2" : "scalar", m_workerCount);
	DebugLog::Log("Dolby Vision RGB tunnel: container=%s %ux%u%s %.3f Hz; loss limit=%u pictures; SDR exit without EOS after %llu header-less non-tunnel pictures%s",
		m_wordLayout == WordLayout::R210 ? "r210" :
		m_wordLayout == WordLayout::R10_BIG_ENDIAN ? "R10b" : "R10l",
		m_width, m_height, m_tunnelCapable ? "p" : "i", m_frameRate,
		m_packetLossLimit, static_cast<unsigned long long>(m_sdrFallbackFrames),
		m_tunnelCapable ? "" : "; interlaced: DV lock disabled, SDR passthrough only");
}

uint32_t CR210DolbyVisionTunnelFormatter::ReadTunnelWord(
	const uint8_t* source, size_t pixel) const
{
	TunnelWordReader reader;
	reader.source = source;
	reader.width = m_width;
	reader.stride = m_stride;
	reader.layout = ToLayout(static_cast<int>(m_wordLayout));
	return reader(pixel);
}

bool CR210DolbyVisionTunnelFormatter::ReadPacketCopies(const uint8_t* source,
	size_t index, bool leftAligned, uint8_t* packet)
{
	TunnelWordReader reader;
	reader.source = source;
	reader.width = m_width;
	reader.stride = m_stride;
	reader.layout = ToLayout(static_cast<int>(m_wordLayout));
	PacketReadCounters counters;
	const bool ok = ReadPacketCopiesWith(reader, index, leftAligned, packet, counters);
	m_crcFailedCopies += counters.crcFailedCopies;
	m_recoveredPackets += counters.recovered;
	m_majorityRecoveries += counters.majority;
	return ok;
}

bool CR210DolbyVisionTunnelFormatter::LooksLikeTunnel(const uint8_t* source) const
{
	TunnelWordReader reader;
	reader.source = source;
	reader.width = m_width;
	reader.stride = m_stride;
	reader.layout = ToLayout(static_cast<int>(m_wordLayout));
	return LooksLikeTunnelWith(reader, m_width, m_height, m_bugHuntFixes);
}

namespace
{
	bool ReaderFor(const CR210DolbyVisionTunnelFormatter::RasterView& raster, TunnelWordReader& reader)
	{
		if (!raster.data || raster.width == 0 || raster.height == 0 ||
			raster.stride < raster.width * 4U ||
			(raster.encoding != VideoFrameEncoding::R210 &&
				raster.encoding != VideoFrameEncoding::R10b &&
				raster.encoding != VideoFrameEncoding::R10l))
			return false;
		reader.source = static_cast<const uint8_t*>(raster.data);
		reader.width = raster.width;
		reader.stride = raster.stride;
		reader.layout = LayoutOf(raster.encoding);
		return true;
	}
}

bool CR210DolbyVisionTunnelFormatter::HasTunnelHeader(const RasterView& raster, bool* highAligned)
{
	TunnelWordReader reader;
	if (!ReaderFor(raster, reader) ||
		static_cast<size_t>(raster.width) * raster.height < kPacketBits * kRepeats)
		return false;
	for (const bool left : { true, false })
	{
		std::array<uint8_t, kPacketBytes> packet{};
		PacketReadCounters counters;
		ParsedPacket parsed;
		if (ReadPacketCopiesWith(reader, 0, left, packet.data(), counters) &&
			(packet[0] >> 6) <= 1 && ParseHeader(packet.data(), parsed))
		{
			if (highAligned)
				*highAligned = left;
			return true;
		}
	}
	return false;
}

CR210DolbyVisionTunnelFormatter::RgbContent
CR210DolbyVisionTunnelFormatter::ClassifyRgbContent(const RasterView& raster)
{
	TunnelWordReader reader;
	if (!ReaderFor(raster, reader))
		return RgbContent::UNKNOWN;
	const PlacementVotes votes = CountPlacementVotes(reader, raster.width, raster.height);
	const size_t nonDark = votes.NonDark();
	// The formatter's own placement decision (AnalyzeFrame) first.
	if (votes.left >= kMinPlacementVotes &&
		(votes.right + votes.contradictory) * 100 <= nonDark * 2)
		return RgbContent::HIGH8;
	if (votes.right >= kMinPlacementVotes &&
		(votes.left + votes.contradictory) * 100 <= nonDark * 2)
		return RgbContent::LOW8;
	// 10-bit content: the low and the high two bits of the same fields in
	// use on at least a quarter of the non-dark samples. RGB8 in either
	// placement never does that (an 8-bit tunnel padded to 10 bits on the
	// link is "high8": its low two bits are zero), so this is not a tunnel
	// stored untouched.
	if (votes.contradictory >= kMinPlacementVotes && votes.contradictory * 4 >= nonDark)
		return RgbContent::TEN_BIT;
	return RgbContent::UNKNOWN;
}

bool CR210DolbyVisionTunnelFormatter::LooksLikeTunnelContent(const RasterView& raster)
{
	TunnelWordReader reader;
	if (!ReaderFor(raster, reader))
		return false;
	return LooksLikeTunnelWith(reader, raster.width, raster.height, true);
}

bool CR210DolbyVisionTunnelFormatter::LastRowBlank(const RasterView& raster)
{
	TunnelWordReader reader;
	if (!ReaderFor(raster, reader))
		return false;
	return LastRowAllZero(reader, raster.width, raster.height);
}

void CR210DolbyVisionTunnelFormatter::BeginTitle()
{
	// audit 2026-10-16: a "title" is one DV lock (lock .. EOS / SDR exit /
	// video-state reset). Extension levels are summarised once per title.
	m_titleLevelCounts.fill(0);
	m_titleLevelsLogged = false;
	m_titleTrimLogged[0] = m_titleTrimLogged[1] = false;
	m_titleIntentLogged = false;   // content intent 2026-10-17
}

void CR210DolbyVisionTunnelFormatter::LogTitleLevels(const char* reason)
{
	std::string other;
	for (size_t level = 0; level < m_titleLevelCounts.size(); ++level)
	{
		const bool named = (level >= 1 && level <= 6) ||
			(level >= 8 && level <= 11) || level == 254;
		if (named || m_titleLevelCounts[level] == 0)
			continue;
		char item[32];
		snprintf(item, sizeof(item), "%sL%u:%s", other.empty() ? "" : ",",
			static_cast<unsigned>(level), FormatLevelCount(m_titleLevelCounts[level]).c_str());
		other += item;
	}
	DebugLog::Log("Dolby Vision RGB tunnel: title DM extension levels (%s): L1=%s L2=%s L3=%s L4=%s L5=%s L6=%s L8=%s L9=%s L10=%s L11=%s L254=%s other=%s (L1 applied; L2/L8 parsed, not applied; xN = blocks per DM)",
		reason,
		FormatLevelCount(m_titleLevelCounts[1]).c_str(), FormatLevelCount(m_titleLevelCounts[2]).c_str(),
		FormatLevelCount(m_titleLevelCounts[3]).c_str(), FormatLevelCount(m_titleLevelCounts[4]).c_str(),
		FormatLevelCount(m_titleLevelCounts[5]).c_str(), FormatLevelCount(m_titleLevelCounts[6]).c_str(),
		FormatLevelCount(m_titleLevelCounts[8]).c_str(), FormatLevelCount(m_titleLevelCounts[9]).c_str(),
		FormatLevelCount(m_titleLevelCounts[10]).c_str(), FormatLevelCount(m_titleLevelCounts[11]).c_str(),
		FormatLevelCount(m_titleLevelCounts[254]).c_str(), other.empty() ? "none" : other.c_str());
}

CR210DolbyVisionTunnelFormatter::FrameResult
CR210DolbyVisionTunnelFormatter::AnalyzeFrame(const VideoFrame& frame)
{
	if (!frame.GetData() || m_outputSize <= 0)
		return FrameResult::REJECTED;
	const auto* source = static_cast<const uint8_t*>(frame.GetData());
	const uint64_t counter = frame.GetCounter(); // audit 2026-10-16
	// EOS changes the immediate next picture to SDR even when its packet
	// has no metadata body. A source counter gap can skip that picture, but
	// every later captured picture is still beyond the HDR stream boundary.
	if (m_hasEndOfStream &&
		frame.GetCounter() > m_endOfStreamAfterCounter)
	{
		m_metadata = {};
		m_hasActiveMetadataId = false;
		m_hasPendingMetadata = false;
		m_hasEndOfStream = false;
		m_tunnelConfirmed = false;
		m_sdrSuspect = false; // audit 2026-10-16
		BeginTitle();         // audit 2026-10-16: EOS ends the title
		DebugLog::Log("Dolby Vision RGB tunnel: EOS boundary; allowing SDR RGB passthrough");
	}
	// A packet may announce new DM metadata for the immediate next picture.
	// A contiguous source counter proves that this is that picture. If VP
	// skipped it, wait for a CRC-valid packet naming the affected metadata ID.
	if (m_hasPendingMetadata &&
		frame.GetCounter() == m_pendingAfterCounter + 1U)
	{
		m_metadata = m_pendingMetadata;
		m_activeMetadataId = m_pendingAffectedId;
		m_hasActiveMetadataId = true;
		m_hasPendingMetadata = false;
	}
	// audit 2026-10-16: container-independent word read (r210 layout).
	auto readWord = [this, source](size_t pixel)
	{
		return ReadTunnelWord(source, pixel);
	};

	bool acceptedMetadata = false;
	bool supportedHeader = false;
	const char* failureReason = "missing/CRC-invalid first packet";
	// Each packet occupies 3072 pixels: three independent CRC-protected
	// copies. Never span pictures or include row padding in the raster.
	const size_t availablePackets = static_cast<size_t>(m_width) * m_height /
		(kPacketBits * kRepeats);
	auto readPacket = [&](size_t index, ByteAlignment candidate,
		std::array<uint8_t, kPacketBytes>& packet)
	{
		if (index >= availablePackets)
			return false;
		// audit 2026-10-16: copies + 2-of-3 majority, see ReadPacketCopies.
		return ReadPacketCopies(source, index,
			candidate == ByteAlignment::LEFT, packet.data());
	};

	// Prefer the current alignment so normal low8 capture does not probe a
	// known-wrong layout on every picture. Only CRC can confirm the tunnel.
	ByteAlignment firstCandidate = m_alignment == ByteAlignment::RIGHT ?
		ByteAlignment::RIGHT : ByteAlignment::LEFT;
	// test/dv-synthetic 2026-10-24 (dv_tunnel_fixes): the packet bit is the parity of the
	// whole RGB8 pixel, so where the header rows are flat (black bars, a flat top) the
	// placement's two dropped bits are constant and the header's CRC is valid under BOTH
	// placements. Trying high8 first then locked a low8 capture as high8: the DM decoded,
	// the picture unpacked as garbage, for as long as the top rows stayed flat. Until the
	// placement is proven, both are checked; when both pass, the placement votes over the
	// raster (low8 never sets the top two bits of a 10-bit field, high8 never the bottom two)
	// decide, and an undecided picture keeps the current placement (high8 at first).
	if (m_bugHuntFixes && !m_placementProven && m_tunnelCapable)
	{
		auto headerValid = [&](ByteAlignment a)
		{
			std::array<uint8_t, kPacketBytes> packet{};
			ParsedPacket parsed;
			return readPacket(0, a, packet) && (packet[0] >> 6) <= 1 && ParseHeader(packet.data(), parsed);
		};
		const bool leftValid = headerValid(ByteAlignment::LEFT);
		const bool rightValid = headerValid(ByteAlignment::RIGHT);
		if (leftValid != rightValid)
		{
			firstCandidate = leftValid ? ByteAlignment::LEFT : ByteAlignment::RIGHT;
			m_placementProven = true;
		}
		else if (leftValid)
		{
			TunnelWordReader reader;
			reader.source = source;
			reader.width = m_width;
			reader.stride = m_stride;
			reader.layout = ToLayout(static_cast<int>(m_wordLayout));
			const PlacementVotes votes = CountPlacementVotes(reader, m_width, m_height);
			const size_t nonDark = votes.NonDark();
			const bool left = votes.left >= kMinPlacementVotes && (votes.right + votes.contradictory) * 100 <= nonDark * 2;
			const bool right = votes.right >= kMinPlacementVotes && (votes.left + votes.contradictory) * 100 <= nonDark * 2;
			++m_ambiguousPlacementPictures;
			if (left || right)
			{
				firstCandidate = right ? ByteAlignment::RIGHT : ByteAlignment::LEFT;
				m_placementProven = true;
				DebugLog::Log("Dolby Vision RGB tunnel: the header is CRC-valid under both RGB8 placements (flat header rows); placement votes (high8 %zu, low8 %zu, both %zu) chose %s",
					votes.left, votes.right, votes.contradictory, right ? "low8" : "high8");
			}
			else if (m_ambiguousPlacementPictures == 1)
				DebugLog::Log("Dolby Vision RGB tunnel: the header is CRC-valid under both RGB8 placements and the picture does not decide (votes high8 %zu, low8 %zu, both %zu); keeping %s until a picture decides",
					votes.left, votes.right, votes.contradictory, firstCandidate == ByteAlignment::LEFT ? "high8" : "low8");
		}
	}
	for (const ByteAlignment candidate : { firstCandidate,
		firstCandidate == ByteAlignment::LEFT ? ByteAlignment::RIGHT : ByteAlignment::LEFT })
	{
		if (!m_tunnelCapable)
			break; // audit 2026-10-16: interlaced raster never locks DV
		std::array<uint8_t, kPacketBytes> packet{};
		ParsedPacket parsed;
		if (!readPacket(0, candidate, packet) ||
			(packet[0] >> 6) > 1 || !ParseHeader(packet.data(), parsed))
			continue;
		supportedHeader = true;
		// audit 2026-10-16: metadata-ID wrap guard. IDs are mod 16 and move
		// at most one step per picture, so after 16+ pictures without a
		// header an equal ID may have wrapped all the way round. The cached
		// active/pending DM then cannot be associated by ID: require fresh DM.
		if (m_hasLastHeaderCounter &&
			(counter < m_lastHeaderCounter ||
				counter - m_lastHeaderCounter >= kMetadataIdModulus) &&
			(m_hasActiveMetadataId || m_hasPendingMetadata))
		{
			DebugLog::Log("Dolby Vision RGB tunnel: %llu pictures since the last valid header; 4-bit metadata IDs may have wrapped, cached DM discarded until fresh DM",
				static_cast<unsigned long long>(counter - m_lastHeaderCounter));
			m_metadata = {};
			m_hasActiveMetadataId = false;
			m_hasPendingMetadata = false;
			++m_idAmbiguityResets;
		}
		m_lastHeaderCounter = counter;
		m_hasLastHeaderCounter = true;
		if (m_haveDiagnosticIds && (parsed.currentId != m_lastCurrentId ||
			parsed.affectedId != m_lastAffectedId))
			++m_metadataIdTransitions;
		m_lastCurrentId = parsed.currentId;
		m_lastAffectedId = parsed.affectedId;
		m_haveDiagnosticIds = true;
		if (!m_tunnelConfirmed || m_alignment != candidate)
			DebugLog::Log("Dolby Vision RGB tunnel: CRC-validated r210 RGB8 alignment=%s",
				candidate == ByteAlignment::LEFT ? "high8" : "low8");
		if (!m_tunnelConfirmed)
			BeginTitle(); // audit 2026-10-16: a new DV lock starts a title
		m_tunnelConfirmed = true;
		m_sdrSuspect = false;
		m_alignment = candidate;
		// Header information remains usable even if a later packet fails.
		if (m_hasPendingMetadata)
		{
			if (parsed.currentId == m_pendingAffectedId)
			{
				m_metadata = m_pendingMetadata;
				m_activeMetadataId = m_pendingAffectedId;
				m_hasActiveMetadataId = true;
				m_hasPendingMetadata = false;
			}
			else if (parsed.currentId != m_pendingCurrentId)
				m_hasPendingMetadata = false;
		}
		if (m_hasActiveMetadataId && parsed.currentId != m_activeMetadataId)
		{
			m_metadata = {};
			m_hasActiveMetadataId = false;
		}
		if (parsed.endOfStream)
		{
			m_hasPendingMetadata = false;
			m_endOfStreamAfterCounter = frame.GetCounter();
			m_hasEndOfStream = true;
		}

		size_t length = 0;
		size_t packetCount = 1;
		std::array<uint16_t, 256> extensionLevels{}; // audit 2026-10-16: block counts
		RawTrimPayload rawTrims[2];
		if (!parsed.noMetadata)
		{
			length = ReadUnsigned16(packet.data() + 3);
			if (length < 71 || length > kMaxMetadataBytes ||
				((packet[0] >> 6) == 0 ? length > 119 : length <= 119))
			{
				failureReason = "invalid declared metadata length/type";
				break;
			}
			packetCount += length > 119 ? (length - 119 + 120) / 121 : 0;
			if (packetCount > availablePackets)
			{
				failureReason = "declared packets exceed picture raster";
				break;
			}
			// Bounded stack storage avoids a per-picture heap allocation.
			std::array<uint8_t, kMaxMetadataBytes> body;
			size_t copied = (std::min)(length, size_t(119));
			memcpy(body.data(), packet.data() + 5, copied);
			const uint8_t headerFlags = packet[0] & 0x3FU;
			const uint8_t headerIds = packet[1];
			const uint8_t headerEos = packet[2];
			bool complete = true;
			for (size_t index = 1; index < packetCount; ++index)
			{
				const unsigned expectedKind = index + 1 == packetCount ? 3 : 2;
				if (!readPacket(index, candidate, packet) ||
					(packet[0] >> 6) != expectedKind ||
					(packet[0] & 0x3FU) != headerFlags ||
					packet[1] != headerIds || packet[2] != headerEos)
				{
					complete = false;
					failureReason = "missing/invalid continuation packet";
					break;
				}
				const size_t chunk = (std::min)(length - copied, size_t(121));
				memcpy(body.data() + copied, packet.data() + 3, chunk);
				copied += chunk;
			}
			if (!complete || copied != length)
				break;
			if (!ParseMetadata(body.data(), length, parsed, extensionLevels, rawTrims, m_bugHuntFixes))
			{
				failureReason = "unsupported/malformed metadata body";
				break;
			}
		}

		if (m_missingPacketReported && m_missingPacketFrames >= m_packetLossLimit)
			DebugLog::Log("Dolby Vision RGB tunnel: metadata transport recovered after %u rejected/missing pictures; fresh_DM=%s",
				m_missingPacketFrames, parsed.noMetadata ? "no" : "yes");
		if (!parsed.noMetadata && (packetCount != m_lastPacketCount ||
			length != m_lastMetadataLength))
		{
			DebugLog::Log("Dolby Vision RGB tunnel: complete DM bytes=%zu packets=%zu current_id=%u affected_id=%u",
				length, packetCount, parsed.currentId, parsed.affectedId);
			m_lastPacketCount = packetCount;
			m_lastMetadataLength = length;
		}
		for (size_t level = 0; level < extensionLevels.size(); ++level)
		{
			if (extensionLevels[level] && !m_seenExtensionLevels[level])
			{
				DebugLog::Log("Dolby Vision RGB tunnel: extension L%zu %s",
					level, level == 1 ? "scene PQ values applied" :
					(level == 2 || level == 8) ? "trims parsed; not applied" :
					(level == 5 || level == 6 || level == 11) ? "parsed (content intent); not applied" :
					"recognized in transport; not applied");
				m_seenExtensionLevels[level] = true;
			}
		}
		// audit 2026-10-16: one compact per-title line answering "which levels
		// arrive", repeated only if a new level appears later in the title,
		// plus the first L2 / L8 block (decoded + raw) to verify the layout.
		if (!parsed.noMetadata)
		{
			bool newTitleLevel = false;
			for (size_t level = 0; level < extensionLevels.size(); ++level)
			{
				if (extensionLevels[level] == 0)
					continue;
				if (m_titleLevelCounts[level] == 0)
					newTitleLevel = true;
				m_titleLevelCounts[level] = (std::max)(m_titleLevelCounts[level],
					extensionLevels[level]);
			}
			if (!m_titleLevelsLogged)
			{
				LogTitleLevels("first complete DM");
				m_titleLevelsLogged = true;
			}
			else if (newTitleLevel)
				LogTitleLevels("new level in title");
			for (unsigned slot = 0; slot < 2; ++slot)
			{
				const uint8_t level = slot == 0 ? 2 : 8;
				if (!rawTrims[slot].present || m_titleTrimLogged[slot])
					continue;
				m_titleTrimLogged[slot] = true;
				const DolbyVisionTunnelFrameMetadata::Trim* trim = nullptr;
				for (uint8_t t = 0; t < parsed.metadata.trimCount; ++t)
				{
					if (parsed.metadata.trims[t].level == level)
					{
						trim = &parsed.metadata.trims[t];
						break;
					}
				}
				const std::string raw = HexBytes(rawTrims[slot].bytes,
					(std::min)(rawTrims[slot].length, sizeof(rawTrims[slot].bytes)));
				if (trim)
					DebugLog::Log("Dolby Vision RGB tunnel: first L%u trim (not applied): target_max_pq=%u (%.0f nits) target_index=%u slope=%u offset=%u power=%u chroma_weight=%u saturation_gain=%u ms_weight=%d%s payload_bytes=%zu raw=%s",
						static_cast<unsigned>(level), trim->targetMaxPq,
						level == 2 ? PqCodeToNits(static_cast<uint16_t>((std::min)(trim->targetMaxPq, uint16_t(4095)))) : 0.0,
						trim->targetDisplayIndex, trim->slope, trim->offset, trim->power,
						trim->chromaWeight, trim->saturationGain, static_cast<int>(trim->msWeight),
						trim->hasMidContrastAndClip ? " (+mid_contrast/clip)" : "",
						rawTrims[slot].length, raw.c_str());
				else
					DebugLog::Log("Dolby Vision RGB tunnel: first L%u block too short for the assumed trim layout: payload_bytes=%zu raw=%s",
						static_cast<unsigned>(level), rawTrims[slot].length, raw.c_str());
			}
			// Content intent 2026-10-17: one line per title naming the L11
			// content type (with its raw bytes, to verify the layout), plus the
			// L5 active area and L6 values when present.
			if (!m_titleIntentLogged)
			{
				m_titleIntentLogged = true;
				const DolbyVisionTunnelFrameMetadata& md = parsed.metadata;
				char l5[96] = "absent";
				if (md.hasActiveArea)
					snprintf(l5, sizeof(l5), "left=%u right=%u top=%u bottom=%u",
						md.activeAreaLeft, md.activeAreaRight, md.activeAreaTop, md.activeAreaBottom);
				char l6[128] = "absent";
				if (md.hasL6)
					snprintf(l6, sizeof(l6), "mastering_max=%u mastering_min=%.4f MaxCLL=%u MaxFALL=%u%s",
						md.l6MaxDisplayMasteringLuminance, md.l6MinDisplayMasteringLuminance / 10000.0,
						md.maxCll, md.maxFall, md.hasContentLight ? "" : " (content light unknown)");
				if (md.hasL11)
					DebugLog::Log("Dolby Vision RGB tunnel: title L11 content type=%u (%s) whitepoint=%u reference_mode=%d "
						"hints sharpness=%u nr=%u mpeg_nr=%u frc=%u brightness=%u color=%u raw=%s; L5 %s; L6 %s",
						md.l11ContentType, L11ContentTypeName(md.l11ContentType), md.l11Whitepoint,
						md.l11ReferenceMode ? 1 : 0, md.l11Sharpness, md.l11NoiseReduction,
						md.l11MpegNoiseReduction, md.l11FrameRateConversion, md.l11Brightness, md.l11Color,
						HexBytes(md.l11Raw, 4).c_str(), l5, l6);
				else
					DebugLog::Log("Dolby Vision RGB tunnel: title L11 content type=absent; L5 %s; L6 %s", l5, l6);
			}
		}
		parsed.metadata.rgbBitAlignment = candidate == ByteAlignment::LEFT ? 1 : 2;
		if (parsed.noMetadata)
			++m_noMetadataFrames; // Healthy transport; active DM remains valid.
		else if (parsed.affectedId == parsed.currentId)
		{
			m_metadata = parsed.metadata;
			m_activeMetadataId = parsed.currentId;
			m_hasActiveMetadataId = true;
		}
		else if (!parsed.endOfStream)
		{
			m_pendingMetadata = parsed.metadata;
			m_pendingAffectedId = parsed.affectedId;
			m_pendingCurrentId = parsed.currentId;
			m_pendingAfterCounter = frame.GetCounter();
			m_hasPendingMetadata = true;
		}
		acceptedMetadata = true;
		break;
	}
	// Metadata retention is separate from permission to display this picture.
	// On source exit, ordinary RGB/blank pixels must never use cached DV matrices.
	m_currentPictureHasValidHeader = supportedHeader;
	m_currentPictureDmAccepted = acceptedMetadata;
	if (acceptedMetadata)
	{
		m_missingPacketFrames = 0;
		m_missingPacketReported = false;
		m_hasPacketLoss = false;
	}
	else
	{
		++m_rejectedFrames;
		// Bug hunt 2026-10-17: the loss limit is time-based, so count source
		// pictures (counter span since the first loss, drops included) rather
		// than analysed pictures, as the SDR exit and the ID-wrap guard do. A
		// counter that goes backwards (reset/wrap) restarts the span.
		if (!m_bugHuntFixes)
		{
			// dv_tunnel_fixes off: analysed pictures.
			if (m_missingPacketFrames != (std::numeric_limits<uint32_t>::max)())
				++m_missingPacketFrames;
		}
		else
		{
			if (!m_hasPacketLoss || counter < m_packetLossStartCounter)
			{
				m_hasPacketLoss = true;
				m_packetLossStartCounter = counter;
			}
			const uint64_t lostPictures = counter - m_packetLossStartCounter + 1;
			m_missingPacketFrames = static_cast<uint32_t>((std::min)(lostPictures,
				static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
		}
		if (!m_missingPacketReported && m_missingPacketFrames >= m_packetLossLimit)
		{
			DebugLog::Log("Dolby Vision RGB tunnel: %u pictures without usable metadata (last_header=%s reason=%s); %s",
				m_missingPacketFrames, supportedHeader ? "CRC-valid" : "missing/CRC-invalid", failureReason,
				!m_tunnelConfirmed ? "no DV lock; allowing SDR RGB8 when alignment is known" :
				m_bugHuntFixes && supportedHeader && m_metadata.valid ? "header CRC-valid: pictures kept with static metadata until fresh DM" :
				m_tunnelConfirmed ? "suspending DV until fresh DM; SDR fallback blocked" :
				"no DV lock; allowing SDR RGB8 when alignment is known");
			m_missingPacketReported = true;
		}
		// feat/dv-l8 (owner 2026-10-24): a picture with a CRC-valid header is still a tunnel
		// picture VP can decode with the retained matrices, so past the loss limit it is shown
		// with static metadata (DvMetadataHold; "Fallbacks engaged: DV metadata" + the counter);
		// only a picture without a valid header (not decodable as the tunnel) ends the DM.
		if (m_tunnelConfirmed && m_missingPacketFrames >= m_packetLossLimit &&
			!(m_bugHuntFixes && supportedHeader && m_metadata.valid))
		{
			m_metadata = {};
			m_hasActiveMetadataId = false;
			m_hasPendingMetadata = false;
		}
	}
	// feat/dv-l8 (no silent fallbacks): a rejected DM is held only briefly, then reduced to
	// its static part (DvMetadataHold.h); every rejected picture after DV lock is counted.
	// The retained DM itself is kept whole (a later no-metadata packet or a black picture still
	// refers to it); only a SHOWN picture past the hold gets the static copy (getter).
	if (m_bugHuntFixes && (m_tunnelConfirmed || acceptedMetadata) &&
		m_dmHold.OnPicture(acceptedMetadata, m_metadata.valid, counter) == DvMetadataHold::Phase::Static &&
		m_dmHold.EnteredStatic())
		DebugLog::Log("Dolby Vision RGB tunnel: DM rejected for longer than the %u-picture hold; shown pictures use static metadata only (scene L1 and trims dropped) until fresh DM; %u rejected pictures so far",
			m_dmHold.HoldPictures(), m_dmHold.RejectedPictures());
	// audit 2026-10-16: safe exit for a same-mode SDR switch without EOS.
	// CRC loss alone still never authorizes SDR (a damaged tunnel looks the
	// same). Only when, for kSdrFallbackSeconds of capture time (counted in
	// source pictures, so drops count too), no picture has had a CRC-valid
	// header AND no picture has looked like tunnel content, is the signal
	// accepted as SDR. Black is presented throughout the uncertainty window.
	if (m_tunnelConfirmed && !supportedHeader)
	{
		if (LooksLikeTunnel(source))
			m_sdrSuspect = false;
		else if (!m_sdrSuspect || counter < m_sdrSuspectStartCounter)
		{
			m_sdrSuspect = true;
			m_sdrSuspectStartCounter = counter;
		}
		if (m_sdrSuspect &&
			counter - m_sdrSuspectStartCounter + 1 >= m_sdrFallbackFrames)
		{
			DebugLog::Log("Dolby Vision RGB tunnel: no CRC-valid header and no tunnel-like content for %llu pictures (%.2f s at %.3f Hz) without EOS; treating source as SDR",
				static_cast<unsigned long long>(counter - m_sdrSuspectStartCounter + 1),
				static_cast<double>(counter - m_sdrSuspectStartCounter + 1) / m_frameRate,
				m_frameRate);
			m_metadata = {};
			m_hasActiveMetadataId = false;
			m_hasPendingMetadata = false;
			m_hasEndOfStream = false;
			m_tunnelConfirmed = false;
			m_sdrSuspect = false;
			m_currentPictureHasValidHeader = false;
			BeginTitle();
		}
	}
	else
		m_sdrSuspect = false;
	if (++m_diagnosticFrames >= kDiagnosticInterval)
	{
		DebugLog::Log("Dolby Vision RGB tunnel: last %u pictures: rejected=%u no_md=%u CRC_failed_copies=%u repeat_recoveries=%u majority_recoveries=%u id_transitions=%u id_wrap_resets=%u current_id=%u affected_id=%u active_DM=%s",
			m_diagnosticFrames, m_rejectedFrames, m_noMetadataFrames,
			m_crcFailedCopies, m_recoveredPackets, m_majorityRecoveries,
			m_metadataIdTransitions, m_idAmbiguityResets,
			m_lastCurrentId, m_lastAffectedId, m_metadata.valid ? "yes" : "no");
		m_diagnosticFrames = m_rejectedFrames = m_noMetadataFrames = 0;
		m_crcFailedCopies = m_recoveredPackets = m_metadataIdTransitions = 0;
		m_majorityRecoveries = m_idAmbiguityResets = 0;
	}
	if (m_alignment == ByteAlignment::UNKNOWN && !m_bugHuntFixes)
	{
		// dv_tunnel_fixes off: the first 4096 pixels, dark pixels voting for both.
		const size_t sampleCount = (std::min)(
			static_cast<size_t>(m_width) * m_height, size_t(4096));
		size_t high8 = 0, low8 = 0;
		for (size_t pixel = 0; pixel < sampleCount; ++pixel)
		{
			const uint32_t word = readWord(pixel);
			const uint32_t r = (word >> 20) & 0x3FFU;
			const uint32_t g = (word >> 10) & 0x3FFU;
			const uint32_t b = word & 0x3FFU;
			high8 += ((r | g | b) & 3U) == 0;
			low8 += ((r | g | b) & 0x300U) == 0;
		}
		if (sampleCount > 0 && high8 * 100 >= sampleCount * 98 &&
			high8 > low8 * 2)
			m_alignment = ByteAlignment::LEFT;
		else if (sampleCount > 0 && low8 * 100 >= sampleCount * 98 &&
			low8 > high8 * 2)
			m_alignment = ByteAlignment::RIGHT;
	}
	else if (m_alignment == ByteAlignment::UNKNOWN)
	{
		// Alignment alone does not identify Dolby Vision. It does let
		// ordinary 8-bit RGB menus use the SDR passthrough path.
		// Bug hunt 2026-10-17: the probe used to read only the first 4096
		// pixels, and a dark pixel (no bits in either the low two or the high
		// two of each 10-bit field) counted for both placements, so a
		// letterboxed or dark top of the picture left the alignment UNKNOWN
		// (black SDR for the whole title). Now a grid over the whole raster
		// (as LooksLikeTunnel) is sampled and only discriminating pixels vote:
		// high8 never sets the low two bits, low8 never sets the high two.
		// Dark pixels count for neither; a decision needs kMinVotes votes and
		// at most 2% of the non-dark samples contradicting it.
		constexpr size_t kMinVotes = kMinPlacementVotes;
		TunnelWordReader reader;
		reader.source = source;
		reader.width = m_width;
		reader.stride = m_stride;
		reader.layout = ToLayout(static_cast<int>(m_wordLayout));
		const PlacementVotes votes = CountPlacementVotes(reader, m_width, m_height);
		const size_t leftVotes = votes.left, rightVotes = votes.right, contradictory = votes.contradictory;
		const size_t nonDark = leftVotes + rightVotes + contradictory;
		if (leftVotes >= kMinVotes &&
			(rightVotes + contradictory) * 100 <= nonDark * 2)
			m_alignment = ByteAlignment::LEFT;
		else if (rightVotes >= kMinVotes &&
			(leftVotes + contradictory) * 100 <= nonDark * 2)
			m_alignment = ByteAlignment::RIGHT;
	}
	if (!m_metadata.valid)
		m_metadata.rgbBitAlignment =
			m_alignment == ByteAlignment::LEFT ? 1 :
			m_alignment == ByteAlignment::RIGHT ? 2 : 0;
	// The renderer may use SDR only before DV lock or after explicit EOS.
	// Invalid metadata after DV lock must not expose the packed RGB pixels.
	const FrameResult result = m_alignment == ByteAlignment::UNKNOWN || !m_metadata.valid ?
		FrameResult::BLACK : FrameResult::PICTURE;
	// DV input fixes 2026-10-18 (dv_tunnel_fixes): a picture whose metadata
	// area arrived intact but whose tail was never written (a truncated
	// transfer: the last row is all zero, which valid tunnel content never
	// is - see LastRowAllZero) is rejected rather than decoded; zero IPT is
	// extreme chroma, not black. The metadata it carried is CRC-verified and
	// kept; only this picture is dropped (the renderer keeps the previous one).
	// Only a picture that would be shown is checked: one without a CRC-valid
	// header of its own is presented black anyway (requiresBlackFrame). The
	// check reads the last row only, besides the packet pixels.
	if (result == FrameResult::PICTURE && m_currentPictureHasValidHeader && m_bugHuntFixes)
	{
		TunnelWordReader reader;
		reader.source = source;
		reader.width = m_width;
		reader.stride = m_stride;
		reader.layout = ToLayout(static_cast<int>(m_wordLayout));
		if (LastRowAllZero(reader, m_width, m_height))
		{
			++m_truncatedFrames;
			if ((m_truncatedFrames & (m_truncatedFrames - 1)) == 0)
				DebugLog::Log("Dolby Vision RGB tunnel: picture %llu rejected: incomplete frame (last row blank, truncated transfer); metadata kept; %llu truncated so far",
					static_cast<unsigned long long>(counter),
					static_cast<unsigned long long>(m_truncatedFrames));
			return FrameResult::REJECTED;
		}
	}
	return result;
}

bool CR210DolbyVisionTunnelFormatter::FormatVideoFrame(
	const VideoFrame& frame, BYTE* output)
{
	if (!output)
		return false;
	const FrameResult result = AnalyzeFrame(frame);
	if (result == FrameResult::REJECTED)
		return false;
	if (result == FrameResult::BLACK)
	{
		memset(output, 0, static_cast<size_t>(m_outputSize));
		return true;
	}

	// P216-style semiplanar output, with exact 12-bit I/C codes.
	ConvertPicture(static_cast<const uint8_t*>(frame.GetData()),
		reinterpret_cast<uint16_t*>(output), m_alignment == ByteAlignment::LEFT);
	return true;
}

void CR210DolbyVisionTunnelFormatter::ConvertPicture(
	const uint8_t* source, uint16_t* outputWords, bool leftAligned)
{
	if (m_workerCount > 0 &&
		static_cast<uint64_t>(m_width) * m_height >= 1920ULL * 1080ULL)
	{
		m_workerSource = source;
		m_workerOutput = outputWords;
		m_workerLeftAligned = leftAligned;
		const uint32_t rowsPerSegment = m_height / (m_workerCount + 1);
		uint32_t firstRow = 0;
		for (uint32_t i = 0; i < m_workerCount; ++i)
		{
			m_workerFirstRow[i] = firstRow;
			m_workerRowCount[i] = rowsPerSegment;
			firstRow += rowsPerSegment;
			SubmitThreadpoolWork(m_conversionWork[i]);
		}
		ConvertRows(source, outputWords, firstRow, m_height - firstRow, leftAligned);
		for (uint32_t i = 0; i < m_workerCount; ++i)
			WaitForThreadpoolWorkCallbacks(m_conversionWork[i], FALSE);
	}
	else
		ConvertRows(source, outputWords, 0, m_height, leftAligned);
}

bool CR210DolbyVisionTunnelFormatter::UnpackPicture(
	const void* source, BYTE* output, bool highAligned)
{
	if (!source || !output || m_outputSize <= 0)
		return false;
	ConvertPicture(static_cast<const uint8_t*>(source), reinterpret_cast<uint16_t*>(output), highAligned);
	return true;
}

void CR210DolbyVisionTunnelFormatter::ConvertRows(
	const uint8_t* source, uint16_t* output, uint32_t firstRow,
	uint32_t rowCount, bool leftAligned) const
{
	if (m_hasAVX2 && m_conversionMethod != ConversionMethod::SCALAR)
	{
		ConvertRowsAVX2(source, output, firstRow, rowCount, leftAligned);
		return;
	}
	// audit 2026-10-16: R10b/R10l = r210 masks with Shift + 2 (R10l also
	// little-endian). The r210 branch below is unchanged.
	if (m_wordLayout == WordLayout::R10_BIG_ENDIAN)
	{
		if (leftAligned)
			ConvertRowsScalar<4>(source, output, m_width, m_height, m_stride,
				firstRow, rowCount);
		else
			ConvertRowsScalar<2>(source, output, m_width, m_height, m_stride,
				firstRow, rowCount);
		return;
	}
	if (m_wordLayout == WordLayout::R10_LITTLE_ENDIAN)
	{
		if (leftAligned)
			ConvertRowsScalar<4, true>(source, output, m_width, m_height, m_stride,
				firstRow, rowCount);
		else
			ConvertRowsScalar<2, true>(source, output, m_width, m_height, m_stride,
				firstRow, rowCount);
		return;
	}
	if (leftAligned)
		ConvertRowsScalar<2>(source, output, m_width, m_height, m_stride,
			firstRow, rowCount);
	else
		ConvertRowsScalar<0>(source, output, m_width, m_height, m_stride,
			firstRow, rowCount);
}

void CALLBACK CR210DolbyVisionTunnelFormatter::ConversionWorkCallback(
	PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK work)
{
	auto* formatter = static_cast<CR210DolbyVisionTunnelFormatter*>(context);
	uint32_t index = 0;
	while (index < formatter->m_workerCount &&
		formatter->m_conversionWork[index] != work)
		++index;
	if (index == formatter->m_workerCount)
		return;
	formatter->ConvertRows(formatter->m_workerSource, formatter->m_workerOutput,
		formatter->m_workerFirstRow[index], formatter->m_workerRowCount[index],
		formatter->m_workerLeftAligned);
}
