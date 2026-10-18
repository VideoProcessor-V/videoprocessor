#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): the known picture + metadata sequence the
// loop sends. Deterministic: frame n is the same on every run, so a hardware
// run (sender and capture in different processes) can be analysed later by
// regenerating frame n from its stamped index.
//
// Every frame carries a 64-bit stamp (magic, sequence id, frame index,
// CRC-16) in four rows near the bottom of the picture, as I-plane levels
// for DV frames (G = I[11:4] on the wire) and as grey G levels for SDR
// frames, so the analyzer can identify any captured frame from the raw RGB8
// bytes alone, before and independently of decoding.
//
// Metadata sequence (ETSI GS CCM 001 6.3.2 semantics):
//   - a scene lasts sceneFrames pictures; each scene has its own DM (new L1,
//     trims, L5/L6/L11 values) and the next metadata id (mod 16);
//   - with advanceMetadata, the last picture of a scene announces the next
//     scene's DM (affected_metadata_id = current + 1);
//   - every noMetadataEvery-th picture inside a scene sends no_md;
//   - an optional SDR segment, entered with or without EOS on the last DV
//     picture before it, then DV again with fresh DM.
// No rates or resolutions are fixed here: the caller gives the raster.
#include "DvTunnelPacker.h"
#include <cstdint>
#include <vector>

namespace DvLoopback
{
	constexpr uint8_t kStampMagic = 0xD7;
	constexpr uint32_t kStampBits = 64;
	constexpr uint32_t kStampRows = 4;

	enum class PatternKind { AllCodes, Ramp, Noise, Bars, Black, Count };

	struct SequenceConfig
	{
		uint32_t width = 256, height = 108;
		uint8_t sequenceId = 0x5A;
		uint32_t sceneFrames = 12;
		bool advanceMetadata = true;
		uint32_t noMetadataEvery = 5;     // 0 = never
		bool richMetadata = true;         // L2, L5, L6, L8, L11 besides L1
		uint32_t sdrStart = 0, sdrLength = 0;
		bool sdrEos = true;
		uint64_t seed = 0x1234567ULL;
		bool movingPicture = true;
		// > 0: the content (picture and metadata) repeats every `period`
		// frames while the stamp keeps the real index, so a real-time sender
		// can precompute `period` pictures and only rewrite the stamp.
		uint32_t period = 0;
	};

	// What one sent frame is, and what a correct receiver must recover.
	struct LoopFrame
	{
		uint32_t index = 0;
		bool sdr = false;
		std::vector<uint8_t> rgb8;     // the wire picture (R, G, B per pixel)
		IptPicture embedded;           // DV: the picture after metadata embedding
		bool hasDm = false;            // DV: metadata in force for this picture
		DmFields effectiveDm;
		PacketHeader header;
		bool sentBody = false;         // DV: a DM body was transmitted with this picture
	};

	inline uint16_t StampCrc16(uint64_t bits48)
	{
		// CRC-16/CCITT-FALSE over the six bytes, MSB first.
		uint16_t crc = 0xFFFF;
		for (int byte = 5; byte >= 0; --byte)
		{
			crc ^= static_cast<uint16_t>(((bits48 >> (byte * 8)) & 0xFF) << 8);
			for (int k = 0; k < 8; ++k)
				crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
		}
		return crc;
	}

	inline uint64_t StampWord(uint8_t sequenceId, uint32_t index)
	{
		const uint64_t head = (static_cast<uint64_t>(kStampMagic) << 40) |
			(static_cast<uint64_t>(sequenceId) << 32) | index;
		return (head << 16) | StampCrc16(head);
	}

	inline uint32_t StampFirstRow(uint32_t height) { return height - kStampRows; }

	// Reads the stamp from the G channel of an RGB8 picture (G = I[11:4]).
	// True when magic, sequence id and CRC agree (bitwise majority of rows).
	inline bool ReadStampFromRgb8(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height,
		uint8_t sequenceId, uint32_t& index)
	{
		if (width < kStampBits || height < kStampRows * 2 || rgb.size() < static_cast<size_t>(width) * height * 3)
			return false;
		uint64_t word = 0;
		for (uint32_t bit = 0; bit < kStampBits; ++bit)
		{
			unsigned votes = 0;
			for (uint32_t r = 0; r < kStampRows; ++r)
			{
				const size_t p = static_cast<size_t>(StampFirstRow(height) + r) * width + bit;
				votes += rgb[p * 3 + 1] >= 128 ? 1U : 0U;
			}
			word = (word << 1) | (votes * 2 > kStampRows ? 1ULL : 0ULL);
		}
		const uint64_t head = word >> 16;
		if (((head >> 40) & 0xFF) != kStampMagic || ((head >> 32) & 0xFF) != sequenceId ||
			StampCrc16(head) != static_cast<uint16_t>(word & 0xFFFF))
			return false;
		index = static_cast<uint32_t>(head & 0xFFFFFFFFULL);
		return true;
	}

	// Reads the stamp from VP's decoded I plane (code << 4).
	inline bool ReadStampFromPlanes(const std::vector<uint16_t>& planes, uint32_t width, uint32_t height,
		uint8_t sequenceId, uint32_t& index)
	{
		if (width < kStampBits || height < kStampRows * 2 || planes.size() < static_cast<size_t>(width) * height)
			return false;
		std::vector<uint8_t> g(static_cast<size_t>(width) * height * 3, 0);
		for (uint32_t r = 0; r < kStampRows; ++r)
			for (uint32_t bit = 0; bit < kStampBits; ++bit)
			{
				const size_t p = static_cast<size_t>(StampFirstRow(height) + r) * width + bit;
				g[p * 3 + 1] = static_cast<uint8_t>((planes[p] >> 4) >> 4);
			}
		return ReadStampFromRgb8(g, width, height, sequenceId, index);
	}

	// Writes the frame stamp: into the I plane (DV) and/or as grey RGB8 (SDR).
	inline void WriteStamp(uint32_t w, uint32_t h, uint8_t sequenceId, uint32_t index,
		std::vector<uint16_t>* iPlane, std::vector<uint8_t>* rgb)
	{
		if (w < kStampBits || h < kStampRows * 2)
			return;
		const uint64_t word = StampWord(sequenceId, index);
		for (uint32_t r = 0; r < kStampRows; ++r)
			for (uint32_t bit = 0; bit < kStampBits; ++bit)
			{
				const bool one = ((word >> (kStampBits - 1 - bit)) & 1ULL) != 0;
				const size_t p = static_cast<size_t>(StampFirstRow(h) + r) * w + bit;
				if (iPlane)
					(*iPlane)[p] = one ? 3072 : 1024;
				if (rgb)
				{
					const uint8_t v = one ? 192 : 64;
					(*rgb)[p * 3] = (*rgb)[p * 3 + 1] = (*rgb)[p * 3 + 2] = v;
				}
			}
	}

	class PatternSequence
	{
	public:
		explicit PatternSequence(const SequenceConfig& config) : m_config(config) {}
		const SequenceConfig& Config() const { return m_config; }

		bool IsSdr(uint32_t index) const
		{
			return m_config.sdrLength > 0 && index >= m_config.sdrStart &&
				index < m_config.sdrStart + m_config.sdrLength;
		}

		// Scene number of a DV frame: scenes restart after the SDR segment.
		uint32_t Scene(uint32_t index) const
		{
			if (m_config.sdrLength > 0 && index >= m_config.sdrStart + m_config.sdrLength)
			{
				const uint32_t before = m_config.sdrStart == 0 ? 0 :
					(m_config.sdrStart - 1) / m_config.sceneFrames + 1;
				return before + (index - m_config.sdrStart - m_config.sdrLength) / m_config.sceneFrames;
			}
			return index / m_config.sceneFrames;
		}

		bool FirstOfScene(uint32_t index) const
		{
			if (index == 0 || IsSdr(index - 1))
				return true;
			return Scene(index) != Scene(index - 1);
		}

		bool LastOfScene(uint32_t index) const
		{
			return !IsSdr(index + 1) && Scene(index + 1) != Scene(index);
		}

		// The scene's DM: L1 and the trims/intent values vary per scene.
		DmFields SceneDm(uint32_t scene) const
		{
			DmFields dm = DmFields::Ipt();
			dm.sceneRefresh = 1;
			const uint16_t s = static_cast<uint16_t>(scene);
			dm.sourceMinPq = static_cast<uint16_t>(7 + (s % 5));
			dm.sourceMaxPq = static_cast<uint16_t>(3079 - (s % 3) * 300);
			dm.ext.push_back(Ext::L1(static_cast<uint16_t>(s % 7), static_cast<uint16_t>(1500 + (s * 211) % 2000),
				static_cast<uint16_t>(600 + (s * 97) % 800)));
			if (m_config.richMetadata)
			{
				dm.ext.push_back(Ext::L2(2851, static_cast<uint16_t>(2048 + (s % 9) * 16), 2048,
					static_cast<uint16_t>(2048 - (s % 4) * 32), 2048, 2048, 2048));
				dm.ext.push_back(Ext::L2(3079, 2048, static_cast<uint16_t>(2040 + s % 16), 2048, 2048, 2048, 2048));
				dm.ext.push_back(Ext::L5(0, 0, static_cast<uint16_t>((s % 3) * 140), static_cast<uint16_t>((s % 3) * 140)));
				dm.ext.push_back(Ext::L6(1000, 1, static_cast<uint16_t>(1000 - (s % 4) * 100), 400));
				dm.ext.push_back(Ext::L8(static_cast<uint8_t>(1 + s % 3), 2048, 2048, 2048, 2048,
					static_cast<uint16_t>(2048 + s % 32), 2048, 1));
				dm.ext.push_back(Ext::L11(static_cast<uint8_t>(1 + s % 4), 0, (s & 1) != 0));
			}
			return dm;
		}

		uint8_t SceneId(uint32_t scene) const { return static_cast<uint8_t>((scene + 1) & 15); }

		uint32_t ContentIndex(uint32_t index) const { return m_config.period ? index % m_config.period : index; }

		LoopFrame Make(uint32_t index) const
		{
			LoopFrame f;
			f.index = index;
			const uint32_t ci = ContentIndex(index);
			f.sdr = IsSdr(ci);
			if (f.sdr)
			{
				MakeSdr(ci, index, f.rgb8);
				return f;
			}
			const uint32_t scene = Scene(ci);
			IptPicture picture;
			MakePicture(ci, index, picture);
			f.hasDm = true;
			f.effectiveDm = SceneDm(scene);
			f.header.currentId = SceneId(scene);
			f.header.affectedId = f.header.currentId;
			DmFields body = f.effectiveDm;
			const bool lastInPeriod = m_config.period && ci + 1 == m_config.period;
			const bool beforeSdr = IsSdr(ci + 1) && !lastInPeriod;
			if (beforeSdr && m_config.sdrEos)
				f.header.eos = true;
			else if (m_config.advanceMetadata && LastOfScene(ci) && !beforeSdr && !lastInPeriod)
			{
				f.header.affectedId = SceneId(Scene(ci + 1));
				body = SceneDm(Scene(ci + 1));
			}
			else if (!FirstOfScene(ci) && m_config.noMetadataEvery &&
				(ci % m_config.noMetadataEvery) == 0)
				f.header.noMetadata = true;
			f.sentBody = !f.header.noMetadata;
			BuildTunnelRgb8(picture, body, f.header, f.rgb8, &f.embedded);
			return f;
		}

	private:
		static uint32_t Lcg(uint64_t& state)
		{
			state = state * 6364136223846793005ULL + 1442695040888963407ULL;
			return static_cast<uint32_t>(state >> 33);
		}

		void Stamp(uint32_t index, std::vector<uint16_t>* iPlane, std::vector<uint8_t>* rgb) const
		{
			WriteStamp(m_config.width, m_config.height, m_config.sequenceId, index, iPlane, rgb);
		}

		void MakePicture(uint32_t index, uint32_t stampIndex, IptPicture& picture) const
		{
			const uint32_t w = m_config.width, h = m_config.height;
			picture.Resize(w, h);
			const PatternKind kind = static_cast<PatternKind>(Scene(index) % static_cast<uint32_t>(PatternKind::Count));
			const uint32_t shift = m_config.movingPicture ? index * 3 : 0;
			uint64_t rng = m_config.seed ^ (0x9E3779B97F4A7C15ULL * (index + 1));
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const size_t p = static_cast<size_t>(y) * w + x;
					const uint32_t xs = (x + shift) % w;
					uint16_t i = 0, c = 2048;
					switch (kind)
					{
					case PatternKind::AllCodes:
						i = static_cast<uint16_t>((p + index * 977U) & 4095U);
						c = static_cast<uint16_t>((p * 73U + index * 31U + 7U) & 4095U);
						break;
					case PatternKind::Ramp:
						i = static_cast<uint16_t>(64 + (static_cast<uint64_t>(xs) * 3696) / (w > 1 ? w - 1 : 1));
						c = static_cast<uint16_t>(2048 + static_cast<int>((y * 512) / (h ? h : 1)) - 256 + ((x & 1) ? 100 : -100));
						break;
					case PatternKind::Noise:
						i = static_cast<uint16_t>(Lcg(rng) & 4095U);
						c = static_cast<uint16_t>(Lcg(rng) & 4095U);
						break;
					case PatternKind::Bars:
					{
						static const uint16_t bars[8][3] = {
							{ 3200, 2048, 2048 }, { 2900, 1700, 2300 }, { 2600, 2400, 1600 }, { 2300, 1800, 1700 },
							{ 2000, 2300, 2500 }, { 1700, 1600, 2600 }, { 1400, 2500, 1500 }, { 300, 2048, 2048 } };
						const uint32_t b = (xs * 8) / w;
						i = bars[b][0];
						c = (x & 1) ? bars[b][2] : bars[b][1];
						break;
					}
					default:
						i = 0;
						c = 2048;
						break;
					}
					picture.i[p] = i;
					picture.Chroma(p) = c;
				}
			Stamp(stampIndex, &picture.i, nullptr);
		}

		void MakeSdr(uint32_t index, uint32_t stampIndex, std::vector<uint8_t>& rgb) const
		{
			const uint32_t w = m_config.width, h = m_config.height;
			rgb.assign(static_cast<size_t>(w) * h * 3, 0);
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const size_t p = static_cast<size_t>(y) * w + x;
					// A grey menu-like ramp with a coloured block: ordinary RGB.
					const uint8_t v = static_cast<uint8_t>(16 + (static_cast<uint64_t>((x + index) % w) * 219) / (w > 1 ? w - 1 : 1));
					rgb[p * 3] = rgb[p * 3 + 1] = rgb[p * 3 + 2] = v;
					if (y > h / 4 && y < h / 2 && x > w / 4 && x < w / 2)
					{
						rgb[p * 3] = 200; rgb[p * 3 + 1] = 60; rgb[p * 3 + 2] = 40;
					}
				}
			Stamp(stampIndex, nullptr, &rgb);
		}

		SequenceConfig m_config;
	};
}
