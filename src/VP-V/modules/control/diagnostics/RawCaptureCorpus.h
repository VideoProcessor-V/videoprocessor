#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Reads a recorded raw-capture corpus (2026-10-17) for replay through VP's
// capture path in the tests and the comparison tools:
//   - VP's own recordings (RawCaptureRecorder.h): segNN folders with
//     capture.txt + frames.csv + frame_NNNNNN.bin;
//   - VideoProcessorStreamRecorder --raw N folders: events.txt (mode line) +
//     frames.csv + raw_NNNNN.bin. Its 8-bit RGB input arrives as 8BitARGB;
//     LoadFrame() can re-contain that losslessly as r210 low8 (every RGB8 byte
//     kept), the form VP's DV tunnel formatter reads.
// No fixed rates or rasters: everything comes from the sidecars.
#include <VideoState.h>
#include <dv_loopback/DvCaptureContainer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct RawCorpusFrame
{
	uint64_t sequence = 0;
	std::string file;
	uint64_t counter = 0;
	int64_t timingTimestamp = 0, captureTimestamp = 0, arrivalQpc = 0;
};

struct RawCorpusSegment
{
	std::string folder;
	std::string source;          // "vp-raw-capture-1" or "stream-recorder"
	std::map<std::string, std::string> keys;
	uint32_t width = 0, height = 0;
	bool interlaced = false;
	unsigned rateNumerator = 0, rateDenominator = 1;
	VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;   // as recorded
	uint32_t rowBytes = 0;
	EOTF eotf = EOTF::SDR;
	ColorSpace colorspace = ColorSpace::REC_709;
	bool hdrInfoFrame = false;
	std::vector<RawCorpusFrame> frames;

	bool EightBitRgb() const
	{
		return encoding == VideoFrameEncoding::ARGB_8BIT || encoding == VideoFrameEncoding::BGRA_8BIT;
	}
	// The encoding replayed into VP: 8-bit RGB recordings replay as r210.
	VideoFrameEncoding ReplayEncoding(bool rgb8AsR210) const
	{
		return rgb8AsR210 && EightBitRgb() ? VideoFrameEncoding::R210 : encoding;
	}

	VideoStateComPtr MakeState(bool dolbyVisionTunnel, bool rgb8AsR210 = true) const
	{
		VideoStateComPtr state = new VideoState();
		state->valid = true;
		state->displayMode = std::make_shared<DisplayMode>(width, height, interlaced, rateNumerator, rateDenominator);
		state->videoFrameEncoding = ReplayEncoding(rgb8AsR210);
		state->eotf = eotf;
		state->colorspace = colorspace;
		if (hdrInfoFrame)
			state->hdrData = std::make_shared<HDRData>();
		state->dolbyVisionTunnel = dolbyVisionTunnel;
		return state;
	}

	// The frame bytes laid out for MakeState()'s encoding and row bytes.
	bool LoadFrame(size_t index, std::vector<uint8_t>& bytes, bool rgb8AsR210 = true) const
	{
		if (index >= frames.size())
			return false;
		std::ifstream in(folder + "\\" + frames[index].file, std::ios::binary);
		if (!in)
			return false;
		std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const uint32_t stride = rowBytes ? rowBytes : width * 4;
		if (raw.size() < static_cast<size_t>(stride) * height)
			return false;
		if (rgb8AsR210 && EightBitRgb())
		{
			std::vector<uint8_t> packed;
			DvLoopback::EightBitRgbToR210(raw.data(), width, height, stride, encoding, packed);
			// VP's r210 rows are padded to 64 pixels (VideoState::BytesPerRow).
			const uint32_t vpStride = ((width + 63) / 64) * 256;
			bytes.assign(static_cast<size_t>(vpStride) * height, 0);
			for (uint32_t y = 0; y < height; ++y)
				std::copy_n(packed.data() + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4,
					bytes.data() + static_cast<size_t>(y) * vpStride);
			return true;
		}
		bytes = std::move(raw);
		return true;
	}
};

namespace RawCorpusDetail
{
	inline std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
		return s.substr(a, b - a);
	}

	inline std::vector<std::string> Split(const std::string& line, char sep)
	{
		std::vector<std::string> out;
		std::stringstream ss(line);
		std::string item;
		while (std::getline(ss, item, sep))
			out.push_back(Trim(item));
		return out;
	}

	// 23.976 -> 24000/1001 etc.: the nearest N/1001 when it fits, else N/1000.
	inline void RateFromHz(double hz, unsigned& num, unsigned& den)
	{
		const double ntsc = hz * 1001.0 / 1000.0;
		if (std::fabs(ntsc - std::round(ntsc)) < 0.002 && std::fabs(hz - std::round(hz)) > 0.002)
		{
			num = static_cast<unsigned>(std::lround(ntsc)) * 1000U;
			den = 1001U;
			return;
		}
		num = static_cast<unsigned>(std::lround(hz * 1000.0));
		den = 1000U;
	}

	inline VideoFrameEncoding EncodingFromDeckLinkName(const std::string& name)
	{
		if (name == "8BitARGB") return VideoFrameEncoding::ARGB_8BIT;
		if (name == "8BitBGRA") return VideoFrameEncoding::BGRA_8BIT;
		if (name == "10BitRGB") return VideoFrameEncoding::R210;
		if (name == "10BitRGBX") return VideoFrameEncoding::R10b;
		if (name == "10BitRGBXLE") return VideoFrameEncoding::R10l;
		if (name == "12BitRGB") return VideoFrameEncoding::R12B;
		if (name == "12BitRGBLE") return VideoFrameEncoding::R12L;
		if (name == "10BitYUV") return VideoFrameEncoding::V210;
		if (name == "8BitYUV") return VideoFrameEncoding::UYVY;
		return VideoFrameEncoding::UNKNOWN;
	}
}

inline bool LoadRawCorpusSegment(const std::string& folder, RawCorpusSegment& seg, std::string& error)
{
	using namespace RawCorpusDetail;
	seg = RawCorpusSegment();
	seg.folder = folder;
	std::ifstream sidecar(folder + "\\capture.txt");
	if (sidecar)
	{
		seg.source = "vp-raw-capture-1";
		std::string line;
		while (std::getline(sidecar, line))
		{
			const size_t eq = line.find('=');
			if (eq != std::string::npos)
				seg.keys[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
		}
		auto num = [&seg](const char* key, double fallback) {
			const auto it = seg.keys.find(key);
			return it == seg.keys.end() || it->second.empty() ? fallback : std::atof(it->second.c_str()); };
		seg.width = static_cast<uint32_t>(num("width", 0));
		seg.height = static_cast<uint32_t>(num("height", 0));
		seg.interlaced = num("interlaced", 0) != 0;
		seg.rateNumerator = static_cast<unsigned>(num("rate_numerator", 0));
		seg.rateDenominator = static_cast<unsigned>(num("rate_denominator", 1));
		seg.encoding = static_cast<VideoFrameEncoding>(static_cast<int>(num("encoding_id", 0)));
		seg.rowBytes = static_cast<uint32_t>(num("bytes_per_row", 0));
		seg.eotf = static_cast<EOTF>(static_cast<int>(num("eotf_id", static_cast<int>(EOTF::SDR))));
		seg.colorspace = static_cast<ColorSpace>(static_cast<int>(num("colorspace_id", static_cast<int>(ColorSpace::REC_709))));
		seg.hdrInfoFrame = num("hdr_infoframe", 0) != 0;
		std::ifstream csv(folder + "\\frames.csv");
		bool header = true;
		while (std::getline(csv, line))
		{
			if (header) { header = false; continue; }
			const std::vector<std::string> f = Split(line, ',');
			if (f.size() < 6)
				continue;
			RawCorpusFrame frame;
			frame.sequence = std::strtoull(f[0].c_str(), nullptr, 10);
			frame.file = f[1];
			frame.counter = std::strtoull(f[2].c_str(), nullptr, 10);
			frame.timingTimestamp = std::strtoll(f[3].c_str(), nullptr, 10);
			frame.captureTimestamp = std::strtoll(f[4].c_str(), nullptr, 10);
			frame.arrivalQpc = std::strtoll(f[5].c_str(), nullptr, 10);
			seg.frames.push_back(frame);
		}
	}
	else
	{
		std::ifstream events(folder + "\\events.txt");
		if (!events)
		{
			error = "no capture.txt or events.txt in " + folder;
			return false;
		}
		seg.source = "stream-recorder";
		std::string line;
		while (std::getline(events, line))
		{
			// "frame N: mode <name> WxH R Hz <format>"
			const size_t mode = line.find(": mode ");
			if (mode != std::string::npos && seg.width == 0)
			{
				const std::vector<std::string> words = Split(line.substr(mode + 7), ' ');
				for (size_t k = 0; k + 2 < words.size(); ++k)
				{
					unsigned w = 0, h = 0;
					if (sscanf_s(words[k].c_str(), "%ux%u", &w, &h) == 2 && words[k + 2] == "Hz")
					{
						seg.width = w;
						seg.height = h;
						RateFromHz(std::atof(words[k + 1].c_str()), seg.rateNumerator, seg.rateDenominator);
						if (k + 3 < words.size())
							seg.encoding = EncodingFromDeckLinkName(words[k + 3]);
						break;
					}
				}
			}
			// "frame N: raw frame saved (WxH, R bytes per row, <format>)"
			const size_t saved = line.find(": raw frame saved (");
			if (saved != std::string::npos)
			{
				unsigned long long index = 0;
				long w = 0, h = 0, row = 0;
				char fmt[32] = {};
				if (sscanf_s(line.c_str(), "frame %llu: raw frame saved (%ldx%ld, %ld bytes per row, %31[^)])",
					&index, &w, &h, &row, fmt, static_cast<unsigned>(sizeof(fmt))) >= 4)
				{
					seg.rowBytes = static_cast<uint32_t>(row);
					RawCorpusFrame frame;
					frame.sequence = index;
					frame.counter = index + 1;
					char file[32];
					snprintf(file, sizeof(file), "raw_%05llu.bin", index);
					frame.file = file;
					if (fmt[0])
						seg.encoding = EncodingFromDeckLinkName(fmt);
					seg.frames.push_back(frame);
				}
			}
		}
		// Hardware timestamps / callback QPC from frames.csv (index first).
		std::ifstream csv(folder + "\\frames.csv");
		std::map<uint64_t, std::pair<int64_t, int64_t>> times;
		bool header = true;
		while (std::getline(csv, line))
		{
			if (header) { header = false; continue; }
			const std::vector<std::string> f = Split(line, ',');
			if (f.size() >= 6)
				times[std::strtoull(f[0].c_str(), nullptr, 10)] = {
					std::strtoll(f[1].c_str(), nullptr, 10), std::strtoll(f[5].c_str(), nullptr, 10) };
		}
		for (RawCorpusFrame& frame : seg.frames)
		{
			const auto it = times.find(frame.sequence);
			if (it != times.end())
			{
				frame.timingTimestamp = frame.captureTimestamp = it->second.first;
				frame.arrivalQpc = it->second.second;
			}
		}
	}
	if (seg.width == 0 || seg.height == 0 || seg.rateNumerator == 0 || seg.encoding == VideoFrameEncoding::UNKNOWN)
	{
		error = "incomplete corpus description in " + folder;
		return false;
	}
	return true;
}

namespace RawCorpusDetail
{
	inline bool FileExists(const std::string& path)
	{
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	inline bool IsDirectory(const std::string& path)
	{
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	inline void Collect(const std::string& folder, std::vector<std::string>& out, int depth)
	{
		if (FileExists(folder + "\\capture.txt") ||
			(FileExists(folder + "\\events.txt") && FileExists(folder + "\\frames.csv")))
			out.push_back(folder);
		if (depth > 8)
			return;
		WIN32_FIND_DATAA data{};
		HANDLE find = FindFirstFileA((folder + "\\*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			const std::string name = data.cFileName;
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != "." && name != "..")
				Collect(folder + "\\" + name, out, depth + 1);
		} while (FindNextFileA(find, &data));
		FindClose(find);
	}
}

// Every segment folder under root (root itself included), sorted by path.
inline std::vector<std::string> FindRawCorpusSegments(const std::string& root)
{
	std::vector<std::string> out;
	if (!RawCorpusDetail::IsDirectory(root))
		return out;
	RawCorpusDetail::Collect(root, out, 0);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}
