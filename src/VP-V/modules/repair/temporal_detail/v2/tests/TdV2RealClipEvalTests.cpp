// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-U0 Real-Clip Diagnostic & Modular Ablation Runner
// (2026-11-25_td-v2-upgrade-plan-FABLE52-draft.md Phase 0 / BUILD.READY D1-D7):
//
// Links the exact production C++ headers unchanged (ZERO Python ports):
//   - TdV2ReferenceEngine (luma recovery + 4-layer grain guard)
//   - TdV2ChromaEngine (coded-grid chroma recovery + W4 colored-edge evaluation)
//   - TdV2FrameRoleDetector + AlignedSharpnessTracker + TdV2NonBlockStepEstimator + TdV2CodingGridStandIn
//   - GrainSplit::Compute (authoritative CPU v3 grain separator)
//   - TdV2Metrics (W1-W4 worst-area masks, P2 worst-5% cells, s13a speed bins)
//
// Reads real encoded H.264, HEVC (8-bit & 10-bit + x265 per-frame CSV), and SVT-AV1 streams
// from D:\testclips\ strictly READ-ONLY via ffmpeg rawvideo pipe (ZERO writes to D:\, ZERO committed pixels).
// Opt-in via Category=Footage (VP_TD_EVAL=1 or VP_TEST_REQUIRE_FOOTAGE=1); never in default CPU unit count.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2FrameRole.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Settings.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2RealClipEvalNs
{
	using namespace TdV2;

	constexpr double kEvalPi = 3.14159265358979323846;

	static bool FileExistsReadOnly(const std::string& path)
	{
		std::ifstream ifs(path, std::ios::binary);
		return ifs.good();
	}

	static int ReadEnvIntOrDefault(const char* name, int defVal, int minVal, int maxVal)
	{
		char* buf = nullptr;
		size_t len = 0;
		int out = defVal;
		if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr && buf[0] != '\0')
		{
			char* endPtr = nullptr;
			const long v = std::strtol(buf, &endPtr, 10);
			if (endPtr != buf)
				out = static_cast<int>(v);
		}
		std::free(buf);
		return (std::max)(minVal, (std::min)(maxVal, out));
	}

	struct DecodedYuv420Frame10
	{
		TdV2Plane y;
		TdV2Plane cb;
		TdV2Plane cr;
	};

	// Decode an exact CTU-aligned (64-px boundary) native-pixel crop of `numFrames` frames from `videoPath`
	// as 10-bit YUV420p10le strictly in memory via ffmpeg pipe (zero temporary files, zero resampling).
	static bool DecodeYuv420p10CropViaFfmpeg(
		const std::string& ffmpegExe,
		const std::string& videoPath,
		int fullW,
		int fullH,
		int cropX,
		int cropY,
		int cropW,
		int cropH,
		int numFrames,
		std::vector<DecodedYuv420Frame10>& outFrames)
	{
		outFrames.clear();
		if (!FileExistsReadOnly(videoPath) || cropW <= 0 || cropH <= 0 || (cropW % 2) != 0 || (cropH % 2) != 0)
			return false;

		const std::string vfArg =
			(cropX == 0 && cropY == 0 && cropW == fullW && cropH == fullH)
				? ""
				: (" -vf \"crop=" + std::to_string(cropW) + ":" + std::to_string(cropH) + ":" +
				   std::to_string(cropX) + ":" + std::to_string(cropY) + "\"");

		// In cmd.exe (`_popen`), wrap the entire command in outer quotes when executable path contains spaces/quotes.
		// `-fps_mode passthrough -map 0:v:0` guarantees 1-to-1 presentation-order raw frame delivery without muxer DTS drops.
		const std::string cmd =
			"\"\"" + ffmpegExe + "\" -hide_banner -loglevel error -nostdin -i \"" + videoPath +
			"\" -map 0:v:0 -fps_mode passthrough" +
			vfArg + " -frames:v " + std::to_string(numFrames) + " -pix_fmt yuv420p10le -f rawvideo -\"";

		FILE* pipe = _popen(cmd.c_str(), "rb");
		if (pipe == nullptr)
			return false;

		const size_t ySamples = static_cast<size_t>(cropW) * static_cast<size_t>(cropH);
		const int cW = cropW / 2;
		const int cH = cropH / 2;
		const size_t cSamples = static_cast<size_t>(cW) * static_cast<size_t>(cH);
		std::vector<uint16_t> yBuf(ySamples);
		std::vector<uint16_t> uBuf(cSamples);
		std::vector<uint16_t> vBuf(cSamples);

		outFrames.reserve(static_cast<size_t>(numFrames));
		for (int f = 0; f < numFrames; ++f)
		{
			if (std::fread(yBuf.data(), sizeof(uint16_t), ySamples, pipe) != ySamples ||
			    std::fread(uBuf.data(), sizeof(uint16_t), cSamples, pipe) != cSamples ||
			    std::fread(vBuf.data(), sizeof(uint16_t), cSamples, pipe) != cSamples)
			{
				break;
			}
			DecodedYuv420Frame10 fr;
			fr.y = TdV2Plane(cropW, cropH, 0.0);
			fr.cb = TdV2Plane(cW, cH, 0.0);
			fr.cr = TdV2Plane(cW, cH, 0.0);
			for (size_t i = 0; i < ySamples; ++i)
				fr.y.v[i] = static_cast<double>(yBuf[i] & 0x03FFu);
			for (size_t i = 0; i < cSamples; ++i)
			{
				fr.cb.v[i] = static_cast<double>(uBuf[i] & 0x03FFu);
				fr.cr.v[i] = static_cast<double>(vBuf[i] & 0x03FFu);
			}
			outFrames.push_back(std::move(fr));
		}
		_pclose(pipe);
		return static_cast<int>(outFrames.size()) == numFrames;
	}

	struct EncoderFrameCsvEntry
	{
		int poc = -1;
		char sliceType = '?'; // 'I', 'P', 'B', 'b'
		double qp = 0.0;
	};

	// Parse x265 per-frame CSV log (Encode Order, Type, POC, QP, ...) into display-order (POC-indexed) entries.
	static std::vector<EncoderFrameCsvEntry> ParseX265CsvDisplayOrder(const std::string& csvPath, int maxFrames)
	{
		std::vector<EncoderFrameCsvEntry> byPoc;
		if (csvPath.empty() || !FileExistsReadOnly(csvPath))
			return byPoc;

		std::ifstream ifs(csvPath);
		std::string line;
		bool headerSeen = false;
		while (std::getline(ifs, line))
		{
			if (line.empty())
				continue;
			if (!headerSeen)
			{
				if (line.find("Encode Order") != std::string::npos && line.find("POC") != std::string::npos)
					headerSeen = true;
				continue;
			}
			std::vector<std::string> cols;
			std::stringstream ss(line);
			std::string tok;
			while (std::getline(ss, tok, ','))
				cols.push_back(tok);
			if (cols.size() < 5u)
				continue;

			// x265 CSV columns: 0=Layer, 1=Encode Order, 2=Type ("I-SLICE"/"P-SLICE"/"B-SLICE"/"b-SLICE"), 3=POC, 4=QP
			const size_t typeCol = (cols[0].find('.') == std::string::npos && cols.size() >= 5u &&
				(cols[2].find("SLICE") != std::string::npos || cols[2].find('I') != std::string::npos ||
				 cols[2].find('P') != std::string::npos || cols[2].find('B') != std::string::npos ||
				 cols[2].find('b') != std::string::npos)) ? 2u : 1u;
			char sType = '?';
			for (char c : cols[typeCol])
			{
				if (c == 'I' || c == 'P' || c == 'B' || c == 'b' || c == 'i')
				{
					sType = (c == 'i') ? 'I' : c;
					break;
				}
			}
			const int poc = std::atoi(cols[typeCol + 1u].c_str());
			const double qp = std::atof(cols[typeCol + 2u].c_str());
			if (poc >= 0 && poc < maxFrames && sType != '?')
			{
				if (static_cast<int>(byPoc.size()) <= poc)
					byPoc.resize(static_cast<size_t>(poc + 1));
				byPoc[static_cast<size_t>(poc)] = EncoderFrameCsvEntry{ poc, sType, qp };
			}
		}
		return byPoc;
	}

	// Deterministic multi-scale 16x16 block + quarter-pel bilinear subpixel motion estimator with
	// forward/backward consistency check (`fbErrorPx`) and median camera velocity extraction.
	// Computed ONCE per clip pair and shared identically across all blind & oracle ablation arms.
	static double SampleBilinearClamped(const TdV2Plane& p, double x, double y)
	{
		const double cx = (std::max)(0.0, (std::min)(static_cast<double>(p.w - 1), x));
		const double cy = (std::max)(0.0, (std::min)(static_cast<double>(p.h - 1), y));
		const int x0 = static_cast<int>(std::floor(cx));
		const int y0 = static_cast<int>(std::floor(cy));
		const int x1 = (std::min)(p.w - 1, x0 + 1);
		const int y1 = (std::min)(p.h - 1, y0 + 1);
		const double fx = cx - static_cast<double>(x0);
		const double fy = cy - static_cast<double>(y0);
		const double v00 = p.at(x0, y0);
		const double v10 = p.at(x1, y0);
		const double v01 = p.at(x0, y1);
		const double v11 = p.at(x1, y1);
		return (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
	}

	static void SearchSingleDirectionBlockMotion(
		const TdV2Plane& src,
		const TdV2Plane& ref,
		int cellPx,
		int searchRangePx,
		std::vector<double>& outVx,
		std::vector<double>& outVy)
	{
		const int cw = (src.w + cellPx - 1) / cellPx;
		const int ch = (src.h + cellPx - 1) / cellPx;
		const size_t nCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);
		outVx.assign(nCells, 0.0);
		outVy.assign(nCells, 0.0);

		auto blockSadInt = [&](int x0, int y0, int x1, int y1, int dx, int dy) -> double
		{
			double sad = 0.0;
			for (int y = y0; y < y1; y += 2)
			{
				const int ry = (std::max)(0, (std::min)(ref.h - 1, y + dy));
				for (int x = x0; x < x1; x += 2)
				{
					const int rx = (std::max)(0, (std::min)(ref.w - 1, x + dx));
					sad += std::fabs(src.at(x, y) - ref.at(rx, ry));
				}
			}
			return sad;
		};

		auto blockSadSubpel = [&](int x0, int y0, int x1, int y1, double vx, double vy) -> double
		{
			double sad = 0.0;
			for (int y = y0; y < y1; y += 2)
			{
				for (int x = x0; x < x1; x += 2)
				{
					sad += std::fabs(src.at(x, y) - SampleBilinearClamped(ref, static_cast<double>(x) + vx, static_cast<double>(y) + vy));
				}
			}
			return sad;
		};

		for (int cy = 0; cy < ch; ++cy)
		{
			const int y0 = cy * cellPx;
			const int y1 = (std::min)(src.h, y0 + cellPx);
			for (int cx = 0; cx < cw; ++cx)
			{
				const int x0 = cx * cellPx;
				const int x1 = (std::min)(src.w, x0 + cellPx);
				int bestDx = 0;
				int bestDy = 0;
				double bestSad = blockSadInt(x0, y0, x1, y1, 0, 0);

				// Coarse 4-px grid + local 1-px refinement around best coarse match and spatial neighbor predictor.
				for (int dy = -searchRangePx; dy <= searchRangePx; dy += 4)
				{
					for (int dx = -searchRangePx; dx <= searchRangePx; dx += 4)
					{
						const double s = blockSadInt(x0, y0, x1, y1, dx, dy);
						if (s + 1e-6 < bestSad)
						{
							bestSad = s;
							bestDx = dx;
							bestDy = dy;
						}
					}
				}
				if (cx > 0)
				{
					const int px = static_cast<int>(std::lround(outVx[static_cast<size_t>(cy) * cw + (cx - 1)]));
					const int py = static_cast<int>(std::lround(outVy[static_cast<size_t>(cy) * cw + (cx - 1)]));
					const double s = blockSadInt(x0, y0, x1, y1, px, py);
					if (s + 1e-6 < bestSad)
					{
						bestSad = s;
						bestDx = px;
						bestDy = py;
					}
				}
				const int cDx = bestDx;
				const int cDy = bestDy;
				for (int dy = cDy - 3; dy <= cDy + 3; ++dy)
				{
					if (std::abs(dy) > searchRangePx) continue;
					for (int dx = cDx - 3; dx <= cDx + 3; ++dx)
					{
						if (std::abs(dx) > searchRangePx) continue;
						const double s = blockSadInt(x0, y0, x1, y1, dx, dy);
						if (s + 1e-6 < bestSad)
						{
							bestSad = s;
							bestDx = dx;
							bestDy = dy;
						}
					}
				}

				// Half-pel + quarter-pel refinement.
				double bestVx = static_cast<double>(bestDx);
				double bestVy = static_cast<double>(bestDy);
				double bestSubSad = blockSadSubpel(x0, y0, x1, y1, bestVx, bestVy);
				for (double step : { 0.5, 0.25 })
				{
					const double curX = bestVx;
					const double curY = bestVy;
					for (int oy = -1; oy <= 1; ++oy)
					{
						for (int ox = -1; ox <= 1; ++ox)
						{
							if (ox == 0 && oy == 0) continue;
							const double candX = curX + static_cast<double>(ox) * step;
							const double candY = curY + static_cast<double>(oy) * step;
							const double s = blockSadSubpel(x0, y0, x1, y1, candX, candY);
							if (s + 1e-6 < bestSubSad)
							{
								bestSubSad = s;
								bestVx = candX;
								bestVy = candY;
							}
						}
					}
				}
				const size_t idx = static_cast<size_t>(cy) * cw + cx;
				outVx[idx] = bestVx;
				outVy[idx] = bestVy;
			}
		}
	}

	static CellMotionField EstimateForwardBackwardMotionField(
		const TdV2Plane& curClean,
		const TdV2Plane& refClean,
		int cellPx,
		int searchRangePx)
	{
		const int cw = (curClean.w + cellPx - 1) / cellPx;
		const int ch = (curClean.h + cellPx - 1) / cellPx;
		const size_t nCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);

		std::vector<double> fwdVx, fwdVy, bwdVx, bwdVy;
		SearchSingleDirectionBlockMotion(curClean, refClean, cellPx, searchRangePx, fwdVx, fwdVy);
		SearchSingleDirectionBlockMotion(refClean, curClean, cellPx, searchRangePx, bwdVx, bwdVy);

		std::vector<double> sortedVx = fwdVx;
		std::vector<double> sortedVy = fwdVy;
		const double medCamVx = RobustStats::Median(sortedVx, RobustStats::MedianRule::MeanOfMiddle);
		const double medCamVy = RobustStats::Median(sortedVy, RobustStats::MedianRule::MeanOfMiddle);
		const double camMag = std::hypot(medCamVx, medCamVy);

		CellMotionField mf;
		mf.cellsW = cw;
		mf.cellsH = ch;
		mf.vx = std::move(fwdVx);
		mf.vy = std::move(fwdVy);
		mf.camVx.assign(nCells, medCamVx);
		mf.camVy.assign(nCells, medCamVy);
		mf.cls.assign(nCells, RegionMotionClass::Still);
		mf.valid.assign(nCells, 1u);
		mf.confidence.assign(nCells, 1.0);
		mf.fbErrorPx.assign(nCells, 0.0);

		for (int cy = 0; cy < ch; ++cy)
		{
			for (int cx = 0; cx < cw; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * cw + cx;
				const double rcx = (std::max)(0.0, (std::min)(static_cast<double>(cw - 1),
					static_cast<double>(cx) + mf.vx[idx] / static_cast<double>(cellPx)));
				const double rcy = (std::max)(0.0, (std::min)(static_cast<double>(ch - 1),
					static_cast<double>(cy) + mf.vy[idx] / static_cast<double>(cellPx)));
				const int rx = static_cast<int>(std::lround(rcx));
				const int ry = static_cast<int>(std::lround(rcy));
				const size_t rIdx = static_cast<size_t>(ry) * cw + rx;
				const double fbErr = std::hypot(mf.vx[idx] + bwdVx[rIdx], mf.vy[idx] + bwdVy[rIdx]);
				mf.fbErrorPx[idx] = fbErr;
				mf.confidence[idx] = RampMath::Clamp01(1.0 - 0.5 * fbErr);

				const double objSpeed = std::hypot(mf.vx[idx] - medCamVx, mf.vy[idx] - medCamVy);
				if (objSpeed >= 0.5)
					mf.cls[idx] = RegionMotionClass::Object;
				else if (camMag >= 0.25)
					mf.cls[idx] = RegionMotionClass::Camera;
				else
					mf.cls[idx] = RegionMotionClass::Still;
			}
		}
		return mf;
	}

	// Build pixel-domain W1..W4 & trackable masks strictly from master frame geometry and temporal motion trust.
	static std::vector<uint8_t> BuildClipWorstAreaMask(
		const DecodedYuv420Frame10& master,
		const CellMotionField* mf,
		bool isValleyBetweenPeaks,
		int cellPx)
	{
		const int w = master.y.w;
		const int h = master.y.h;
		const size_t nPx = static_cast<size_t>(w) * static_cast<size_t>(h);
		std::vector<uint8_t> mask(nPx, 0u);
		const int cw = (w + cellPx - 1) / cellPx;
		const int ch = (h + cellPx - 1) / cellPx;

		for (int cy = 0; cy < ch; ++cy)
		{
			const int y0 = cy * cellPx;
			const int y1 = (std::min)(h, y0 + cellPx);
			for (int cx = 0; cx < cw; ++cx)
			{
				const int x0 = cx * cellPx;
				const int x1 = (std::min)(w, x0 + cellPx);
				const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
				const bool trackable = (mf != nullptr && cIdx < mf->fbErrorPx.size() && mf->fbErrorPx[cIdx] <= 1.0);
				const bool stillCell = (mf == nullptr) ||
					(cIdx < mf->vx.size() && std::hypot(mf->vx[cIdx], mf->vy[cIdx]) < 0.5);

				double sumY = 0.0;
				double sumGrad = 0.0;
				size_t cnt = 0;
				for (int y = y0; y < y1; ++y)
				{
					for (int x = x0; x < x1; ++x)
					{
						const double v = master.y.at(x, y);
						sumY += v;
						if (x + 1 < x1 && y + 1 < y1)
						{
							sumGrad += std::fabs(master.y.at(x + 1, y) - v) + std::fabs(master.y.at(x, y + 1) - v);
						}
						++cnt;
					}
				}
				const double meanY = cnt > 0 ? (sumY / static_cast<double>(cnt)) : 0.0;
				const double meanGrad = cnt > 0 ? (sumGrad / static_cast<double>(cnt)) : 0.0;

				// W1 smooth gradient/sky: bright/mid-tone low local gradient; W2 dark still: meanY < 160 codes & still.
				const bool isW1 = (meanY >= 160.0 && meanGrad <= 3.0);
				const bool isW2 = (meanY < 160.0 && stillCell);

				for (int y = y0; y < y1; ++y)
				{
					for (int x = x0; x < x1; ++x)
					{
						uint8_t bits = 0u;
						if (trackable) bits |= kMaskTrackable;
						if (isW1) bits |= kMaskW1BandedSky;
						if (isW2) bits |= kMaskW2DarkStill;
						if (isValleyBetweenPeaks) bits |= kMaskW3BetweenKeyframes;

						const int ux = (std::min)(master.cb.w - 1, x / 2);
						const int uy = (std::min)(master.cb.h - 1, y / 2);
						const int ux1 = (std::min)(master.cb.w - 1, ux + 1);
						const int uy1 = (std::min)(master.cb.h - 1, uy + 1);
						const double cGrad =
							std::fabs(master.cb.at(ux1, uy) - master.cb.at(ux, uy)) +
							std::fabs(master.cb.at(ux, uy1) - master.cb.at(ux, uy)) +
							std::fabs(master.cr.at(ux1, uy) - master.cr.at(ux, uy)) +
							std::fabs(master.cr.at(ux, uy1) - master.cr.at(ux, uy));
						if (cGrad >= 6.0)
							bits |= kMaskW4ChromaEdges;
						mask[static_cast<size_t>(y) * w + x] = bits;
					}
				}
			}
		}
		return mask;
	}

	// Compute P5 0.5-4.0 Hz temporal DFT energy of 16x16 cell-mean luminance residuals relative to master.
	static double ComputeFlickerBandEnergy05To4Hz(
		const std::vector<TdV2Plane>& planes,
		const std::vector<DecodedYuv420Frame10>& masters,
		double fps,
		int cellPx)
	{
		const size_t T = planes.size();
		if (T < 8u || masters.size() < T || fps <= 0.0)
			return 0.0;
		const int w = planes[0].w;
		const int h = planes[0].h;
		const int cw = (w + cellPx - 1) / cellPx;
		const int ch = (h + cellPx - 1) / cellPx;
		const size_t nCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);

		std::vector<double> cellSeries(T * nCells, 0.0);
		for (size_t t = 0; t < T; ++t)
		{
			for (int cy = 0; cy < ch; ++cy)
			{
				const int y0 = cy * cellPx;
				const int y1 = (std::min)(h, y0 + cellPx);
				for (int cx = 0; cx < cw; ++cx)
				{
					const int x0 = cx * cellPx;
					const int x1 = (std::min)(w, x0 + cellPx);
					double sumDiff = 0.0;
					size_t cnt = 0;
					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							sumDiff += (planes[t].at(x, y) - masters[t].y.at(x, y));
							++cnt;
						}
					}
					cellSeries[t * nCells + static_cast<size_t>(cy) * cw + cx] =
						cnt > 0 ? (sumDiff / static_cast<double>(cnt)) : 0.0;
				}
			}
		}

		double totalPower = 0.0;
		size_t activeBins = 0;
		for (size_t k = 1; k <= T / 2; ++k)
		{
			const double freqHz = (static_cast<double>(k) * fps) / static_cast<double>(T);
			if (freqHz < 0.5 - 1e-6 || freqHz > 4.0 + 1e-6)
				continue;
			++activeBins;
			for (size_t c = 0; c < nCells; ++c)
			{
				double re = 0.0;
				double im = 0.0;
				for (size_t t = 0; t < T; ++t)
				{
					const double ang = (2.0 * kEvalPi * static_cast<double>(k * t)) / static_cast<double>(T);
					const double v = cellSeries[t * nCells + c];
					re += v * std::cos(ang);
					im -= v * std::sin(ang);
				}
				totalPower += (re * re + im * im) / static_cast<double>(T * T);
			}
		}
		return (activeBins > 0 && nCells > 0) ? (totalPower / static_cast<double>(nCells)) : 0.0;
	}

	// Oracle A4 upper-bound reference arm: uses the exact production BandFromBase split, bicubic warp,
	// motion/focus gates, SolveConvexWeights(ActiveSetExact), 3-sigma Huber pixel taper, statistical bound,
	// and Layer-1/Layer-4 grain separation, replacing ONLY blind c_j with true clean-master cross-covariance
	// c_oracle = E[(curClean - masterClean)(curClean - warpedRefClean)] inside each pooled cell window!
	static TdV2Plane RunOracleA4Frame(
		const Config& cfg,
		const TdV2Plane& receivedY,
		const TdV2Plane& curClean,
		const TdV2Plane& masterClean,
		const TdV2Plane* prevPeakClean,
		const CellMotionField* prevMv,
		const TdV2Plane* nextPeakClean,
		const CellMotionField* nextMv)
	{
		const int w = receivedY.w;
		const int h = receivedY.h;
		const int cellPx = cfg.cellPx > 0 ? cfg.cellPx : kCanonicalBlockPx;
		const int cw = (w + cellPx - 1) / cellPx;
		const int ch = (h + cellPx - 1) / cellPx;
		const size_t nCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);
		const ActiveRect active = ActiveRect::Full(w, h);

		struct OracleRef
		{
			TdV2Plane warped;
			ThreeBandPlanes bands;
			std::vector<uint8_t> validPx;
			MotionGateResult gate;
		};
		std::vector<OracleRef> refs;
		auto addRef = [&](const TdV2Plane* refP, const CellMotionField* mf)
		{
			if (refP == nullptr || mf == nullptr || !mf->IsWellFormed(cw, ch))
				return;
			OracleRef r;
			WarpCleanReference(*refP, curClean, active, mf->vx, mf->vy, cw, ch, cellPx,
			                   r.warped, r.validPx, nullptr, &mf->valid, &mf->cls, nullptr, nullptr, cfg.enableSmoothWarpVectors);
			r.bands = DecomposeActiveBands(r.warped, active, nullptr);
			r.gate = EvaluateCameraRelativeMotionGate(*mf, nullptr, cfg.enableHardFbOcclusionGate);
			refs.push_back(std::move(r));
		};
		addRef(prevPeakClean, prevMv);
		addRef(nextPeakClean, nextMv);
		const int K = static_cast<int>(refs.size());
		if (K == 0)
			return receivedY;

		const ThreeBandPlanes curBands = DecomposeActiveBands(curClean, active, nullptr);
		const ThreeBandPlanes mstBands = DecomposeActiveBands(masterClean, active, nullptr);
		std::vector<ThreeBandPlanes> refBandVec;
		refBandVec.reserve(refs.size());
		for (const auto& r : refs) refBandVec.push_back(r.bands);
		const std::vector<FocusCellInfo> focus = EvaluateFocusGuardStandIn(
			curBands, refBandVec, active, cw, ch, cellPx, cfg.sourceBits, 1.0 / 12.0, nullptr, nullptr);

		auto bandPlane = [](const ThreeBandPlanes& b, int bi) -> const TdV2Plane&
		{
			return (bi == 0) ? b.L : ((bi == 1) ? b.M : b.H);
		};

		std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellW;
		std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellDiagC;
		std::array<std::vector<double>, 3> cellOwnU;
		for (int b = 0; b < 3; ++b)
		{
			cellOwnU[b].assign(nCells, 0.0);
			for (int k = 0; k < K; ++k)
			{
				cellW[b][k].assign(nCells, 0.0);
				cellDiagC[b][k].assign(nCells, 0.0);
			}
		}

		for (int b = 0; b < 3; ++b)
		{
			const int poolR = cfg.PoolRadiusForBand(b);
			const TdV2Plane& curB = bandPlane(curBands, b);
			const TdV2Plane& mstB = bandPlane(mstBands, b);
			for (int cy = 0; cy < ch; ++cy)
			{
				for (int cx = 0; cx < cw; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
					if (focus[cIdx].focusChange)
						continue;

					double C[kMaxBlendRefs][kMaxBlendRefs] = {};
					double cVec[kMaxBlendRefs] = {};
					double uTrue = 0.0;
					size_t nSamp = 0;

					for (int py = (std::max)(0, cy - poolR); py <= (std::min)(ch - 1, cy + poolR); ++py)
					{
						for (int px = (std::max)(0, cx - poolR); px <= (std::min)(cw - 1, cx + poolR); ++px)
						{
							const size_t pIdx = static_cast<size_t>(py) * cw + px;
							if (focus[pIdx].focusChange) continue;
							for (int y = py * cellPx; y < (std::min)(h, (py + 1) * cellPx); ++y)
							{
								for (int x = px * cellPx; x < (std::min)(w, (px + 1) * cellPx); ++x)
								{
									const size_t pix = static_cast<size_t>(y) * w + x;
									bool allValid = true;
									for (int k = 0; k < K; ++k)
										if (!refs[k].validPx[pix]) { allValid = false; break; }
									if (!allValid) continue;

									const double xCur = curB.v[pix];
									const double eOwn = xCur - mstB.v[pix];
									uTrue += eOwn * eOwn;
									double d[kMaxBlendRefs] = {};
									for (int k = 0; k < K; ++k)
										d[k] = xCur - bandPlane(refs[k].bands, b).v[pix];
									for (int i = 0; i < K; ++i)
									{
										cVec[i] += eOwn * d[i];
										for (int j = 0; j < K; ++j)
											C[i][j] += d[i] * d[j];
									}
									++nSamp;
								}
							}
						}
					}
					if (nSamp < 16u)
						continue;
					const double invN = 1.0 / static_cast<double>(nSamp);
					for (int i = 0; i < K; ++i)
					{
						cVec[i] *= invN;
						for (int j = 0; j < K; ++j)
							C[i][j] *= invN;
						cellDiagC[b][i][cIdx] = C[i][i];
					}
					cellOwnU[b][cIdx] = uTrue * invN;

					const BlendSolveResult sol = SolveConvexWeights(
						C, cVec, K, kDefaultTraceRegScale, nullptr, SolverMode::ActiveSetExact);
					for (int k = 0; k < K; ++k)
					{
						const double g = (b == 0) ? refs[k].gate.gateL[cIdx] : refs[k].gate.gateMH[cIdx];
						cellW[b][k][cIdx] = sol.weights[k] * g;
					}
				}
			}
		}

		TdV2Plane out = receivedY;
		for (int y = 0; y < h; ++y)
		{
			const int cy = (std::min)(ch - 1, y / cellPx);
			for (int x = 0; x < w; ++x)
			{
				const int cx = (std::min)(cw - 1, x / cellPx);
				const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
				const size_t pix = static_cast<size_t>(y) * w + x;
				double sumDelta = 0.0;
				double sumU = 0.0;
				for (int b = 0; b < 3; ++b)
				{
					const TdV2Plane& curB = bandPlane(curBands, b);
					double dBand = 0.0;
					for (int k = 0; k < K; ++k)
					{
						if (!refs[k].validPx[pix]) continue;
						const double diff = bandPlane(refs[k].bands, b).v[pix] - curB.v[pix];
						const double wEff = SpatialBilinearHuberWeight(
							cellW[b][k].data(), cellDiagC[b][k].data(), cw, ch,
							static_cast<double>(x), static_cast<double>(y), cellPx, diff);
						dBand += wEff * diff;
					}
					const double uB = (std::max)(0.0, cellOwnU[b][cIdx]);
					sumU += uB;
					const double capB = cfg.pixelBoundKappa * std::sqrt(uB);
					if (std::fabs(dBand) > capB)
						dBand = (dBand >= 0.0) ? capB : -capB;
					sumDelta += dBand;
				}
				const double jointCap = cfg.pixelBoundKappa * std::sqrt(sumU);
				if (std::fabs(sumDelta) > jointCap)
					sumDelta = (sumDelta >= 0.0) ? jointCap : -jointCap;
				out.v[pix] = receivedY.v[pix] + sumDelta;
			}
		}
		return out;
	}

	struct RealClipSpec
	{
		const char* id;
		const char* codecLabel;
		const char* masterPath;
		const char* encodedPath;
		const char* x265CsvPath;
		int fullW;
		int fullH;
		double fps;
		int defaultFrames;
	};

	struct AblationArmSpec
	{
		const char* name;
		bool isOff = false;
		bool isOracleA4 = false;
		Config cfg;
		bool feedNonBlockStepPrior = false;
		bool feedLegacy8pxStepPrior = false;
	};

	struct ClipArmMetrics
	{
		std::string clipId;
		std::string armName;
		double globalDeltaDb = 0.0;
		double valleyW3DeltaDb = 0.0;
		double worst5DeltaDb = 0.0;
		double worsenedFracPct = 0.0;
		double w1SkyDeltaDb = 0.0;
		double w1StepRedPct = 0.0;
		double w2DarkDeltaDb = 0.0;
		double p5FlickerRatio = 1.0;
		double panSharpRet = 1.0;
		double worstObjRet = 1.0;
		double grainHighFreqRatio = 1.0;
		double chromaW4DeltaDb = 0.0;
	};

	static double SpearmanRankCorr(const std::vector<double>& a, const std::vector<double>& b)
	{
		const size_t n = a.size();
		if (n < 3u || b.size() != n)
			return 0.0;
		auto toRanks = [n](const std::vector<double>& v)
		{
			std::vector<size_t> idx(n);
			std::iota(idx.begin(), idx.end(), 0u);
			std::sort(idx.begin(), idx.end(), [&](size_t i, size_t j) { return v[i] < v[j]; });
			std::vector<double> r(n, 0.0);
			size_t i = 0;
			while (i < n)
			{
				size_t j = i + 1;
				while (j < n && std::fabs(v[idx[j]] - v[idx[i]]) <= 1e-12) ++j;
				const double avgRank = 0.5 * static_cast<double>(i + (j - 1));
				for (size_t k = i; k < j; ++k) r[idx[k]] = avgRank;
				i = j;
			}
			return r;
		};
		const std::vector<double> ra = toRanks(a);
		const std::vector<double> rb = toRanks(b);
		double ma = 0.0, mb = 0.0;
		for (size_t i = 0; i < n; ++i) { ma += ra[i]; mb += rb[i]; }
		ma /= static_cast<double>(n);
		mb /= static_cast<double>(n);
		double cov = 0.0, va = 0.0, vb = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const double da = ra[i] - ma;
			const double db = rb[i] - mb;
			cov += da * db;
			va += da * da;
			vb += db * db;
		}
		return (va > 1e-12 && vb > 1e-12) ? (cov / std::sqrt(va * vb)) : 0.0;
	}
} // namespace TdV2RealClipEvalNs

TEST_CLASS(TdV2RealClipEvalTests)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(RealClipDiagnosticAndModularAblations)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(RealClipDiagnosticAndModularAblations)
	{
		using namespace TdV2RealClipEvalNs;
		const std::string ffmpegExe = "D:\\tools\\ffmpeg\\bin\\ffmpeg.exe";
		const bool runRequested =
			HardwarePrerequisites::EnvIsOne("VP_TD_EVAL") ||
			HardwarePrerequisites::EnvIsOne("VP_TEST_FOOTAGE") ||
			HardwarePrerequisites::Required(HardwarePrerequisites::Kind::Footage);
		const bool footagePresent =
			FileExistsReadOnly("D:\\testclips\\td-oracle2\\masters\\sol150.mkv") &&
			FileExistsReadOnly("D:\\testclips\\td-oracle2\\enc\\sol150__x265_low.mp4") &&
			FileExistsReadOnly(ffmpegExe);

		if (!HardwarePrerequisites::RequireOrSkip(
				HardwarePrerequisites::Kind::Footage,
				"TdV2RealClipEvalTests::RealClipDiagnosticAndModularAblations",
				runRequested && footagePresent,
				!footagePresent
					? "missing D:\\testclips\\td-oracle2 or D:\\tools\\ffmpeg\\bin\\ffmpeg.exe"
					: "opt-in TD-U0 real-clip diagnostic harness (set VP_TD_EVAL=1)"))
		{
			return;
		}

		// CTU-aligned (64-px boundary) native 1:1 crop (zero resampling, preserves exact codec transform/CTU grid).
		// Set VP_TD_EVAL_FULL=1 to evaluate full 1920x1080 frames.
		const bool fullFrame = HardwarePrerequisites::EnvIsOne("VP_TD_EVAL_FULL");
		const int maxFramesOverride = ReadEnvIntOrDefault("VP_TD_EVAL_FRAMES", 0, 0, 120);

		const std::vector<RealClipSpec> clips = {
			{
				"sol150_x264_low", "H.264 8b 2Mbps",
				"D:\\testclips\\td-oracle2\\masters\\sol150.mkv",
				"D:\\testclips\\td-oracle2\\enc\\sol150__x264_low.mp4",
				"",
				1920, 1080, 24.0, 24
			},
			{
				"sol150_x265_low", "HEVC 8b 1.4Mbps",
				"D:\\testclips\\td-oracle2\\masters\\sol150.mkv",
				"D:\\testclips\\td-oracle2\\enc\\sol150__x265_low.mp4",
				"D:\\testclips\\td-oracle2\\enc\\sol150__x265_low.csv",
				1920, 1080, 24.0, 24
			},
			{
				"noct120_x265_low", "HEVC 8b Grain 2.5Mbps",
				"D:\\testclips\\td-oracle2\\masters\\noct120.mkv",
				"D:\\testclips\\td-oracle2\\enc\\noct120__x265_low.mp4",
				"D:\\testclips\\td-oracle2\\enc\\noct120__x265_low.csv",
				1920, 1080, 60.0, 24
			},
			{
				"mer_dark_x265_low", "HEVC 8b Dark 2.5Mbps",
				"D:\\testclips\\td-oracle2\\masters\\mer_dark.mkv",
				"D:\\testclips\\td-oracle2\\enc\\mer_dark__x265_low.mp4",
				"D:\\testclips\\td-oracle2\\enc\\mer_dark__x265_low.csv",
				1920, 1080, 59.94, 24
			},
			{
				"r3_merdark_x265_10b", "HEVC 10b 4K OpenGOP",
				"D:\\testclips\\round3\\masters\\r3_merdark.mkv",
				"D:\\testclips\\round3\\enc\\r3_merdark__x265_10bit_open.mp4",
				"D:\\testclips\\round3\\enc\\r3_merdark__x265_10bit_open.csv",
				3840, 2160, 59.94, 24
			},
			{
				"r3_merdark_av1_low", "SVT-AV1 10b 4K CRF-Low",
				"D:\\testclips\\round3\\masters\\r3_merdark.mkv",
				"D:\\testclips\\round3\\enc\\r3_merdark__svtav1_crf_low.mkv",
				"",
				3840, 2160, 59.94, 24
			}
		};

		auto makeBaseCfg = []() -> Config
		{
			Config c = MakePreset(Preset::Reference, Mode::On);
			c.sourceBits = 10;
			c.pixelBoundKappa = 1.0;
			c.solverMode = SolverMode::ActiveSetExact;
			c.qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e;
			c.enableA2eEmpiricalPrior = true;
			c.enableBandDependentPool = true;
			c.enableEmpiricalBayesShrinkage = true;
			c.enableTwoFrameSameRoleStatsPool = false;
			c.enableCarryForward = false;
			c.enableAnchoredCarry = false;
			c.enableSmoothWarpVectors = true;
			c.enableHardFbOcclusionGate = true;
			return c;
		};

		std::vector<AblationArmSpec> arms;
		{
			AblationArmSpec offArm{ "0_Off", true, false, MakePreset(Preset::Reference, Mode::Off), false, false };
			arms.push_back(offArm);

			AblationArmSpec defK1{ "1_D1_D7_Default_k1", false, false, makeBaseCfg(), false, false };
			arms.push_back(defK1);

			Config k2 = makeBaseCfg();
			k2.pixelBoundKappa = 2.0;
			arms.push_back(AblationArmSpec{ "2_AlignedA2e_A3_k2", false, false, k2, false, false });

			Config k3 = makeBaseCfg();
			k3.pixelBoundKappa = 3.0;
			arms.push_back(AblationArmSpec{ "3_AlignedA2e_A3_k3", false, false, k3, false, false });

			Config a1Only = k2;
			a1Only.estimator = VarianceEstimator::A1_ThreeCorneredHat;
			arms.push_back(AblationArmSpec{ "4_Blind_A1_k2", false, false, a1Only, false, false });

			Config a2eOnly = k2;
			a2eOnly.estimator = VarianceEstimator::A2_RoundingCells;
			arms.push_back(AblationArmSpec{ "5_Blind_A2e_k2", false, false, a2eOnly, false, false });

			Config nonBlock = k2;
			nonBlock.qualitySignalMode = QualitySignalMode::NonBlockPictureStep;
			nonBlock.enableA2eEmpiricalPrior = false;
			arms.push_back(AblationArmSpec{ "6_NonBlockStep_k2", false, false, nonBlock, true, false });

			Config leg8 = k2;
			leg8.qualitySignalMode = QualitySignalMode::LegacyBlockEdge8px;
			leg8.enableA2eEmpiricalPrior = false;
			arms.push_back(AblationArmSpec{ "7_Legacy8px_k2", false, false, leg8, false, true });

			Config legSolver = k2;
			legSolver.solverMode = SolverMode::LegacyClipRescale;
			arms.push_back(AblationArmSpec{ "8_LegacySolver_k2", false, false, legSolver, false, false });

			Config leg1x1 = k2;
			leg1x1.enableBandDependentPool = false;
			leg1x1.enableEmpiricalBayesShrinkage = false;
			leg1x1.momentPoolRadiusCells = 0;
			arms.push_back(AblationArmSpec{ "9_Legacy1x1Stats_k2", false, false, leg1x1, false, false });

			Config twoFrame = k2;
			twoFrame.enableTwoFrameSameRoleStatsPool = true;
			arms.push_back(AblationArmSpec{ "10_TwoFrameRole_k2", false, false, twoFrame, false, false });

			Config ancCarry = k2;
			ancCarry.enableAnchoredCarry = true;
			arms.push_back(AblationArmSpec{ "11_AnchoredCarry_k2", false, false, ancCarry, false, false });

			Config reuseChroma = k2;
			reuseChroma.chromaReuseLumaWeights = true;
			arms.push_back(AblationArmSpec{ "12_ChromaReuseLuma_k2", false, false, reuseChroma, false, false });

			Config oracleCfg = k3;
			arms.push_back(AblationArmSpec{ "13_Oracle_A4_Ceiling", false, true, oracleCfg, false, false });

			Config a41k1 = makeBaseCfg();
			a41k1.enableRegionTailoring = true;
			a41k1.bokehMatchDelta = kBokehMatchDeltaNormal;
			a41k1.enableRegionDeband = true;
			arms.push_back(AblationArmSpec{ "14_Blind_A4_1_k1", false, false, a41k1, false, false });

			Config a41k2 = k2;
			a41k2.enableRegionTailoring = true;
			a41k2.bokehMatchDelta = kBokehMatchDeltaNormal;
			a41k2.enableRegionDeband = true;
			arms.push_back(AblationArmSpec{ "15_Blind_A4_1_k2", false, false, a41k2, false, false });

			Config a41k3 = k3;
			a41k3.enableRegionTailoring = true;
			a41k3.bokehMatchDelta = kBokehMatchDeltaNormal;
			a41k3.enableRegionDeband = true;
			arms.push_back(AblationArmSpec{ "16_Blind_A4_1_k3", false, false, a41k3, false, false });
		}

		std::ostringstream report;
		report << "\n=== [TD-U0] REAL-CLIP DIAGNOSTIC & MODULAR ABLATION REPORT (Read-Only D:\\testclips) ===\n";

		std::ostringstream roleReport;
		roleReport << "\n### [TD-U0 Part A] Picture-Only Frame-Role Detector vs Real x265 Encoder CSV Ground Truth\n"
		           << "| Clip | Mode | Enc Peaks (I/P/B) | Det Peaks | Peak Precision | Valley Recall (b) | Spearman(Score, -QP) |\n"
		           << "|---|---|---:|---:|---:|---:|---:|\n";

		std::ostringstream ablationTable;
		ablationTable << "\n### [TD-U0 Part B] Modular Ablation Matrix Across Real H.264 / HEVC / AV1 Streams\n"
		              << "| Clip | Arm | Global dB | Valley W3 dB | Worst-5% dB | Worsened>0.1dB | W1 Sky dB | W1 StepRed | W2 Dark dB | P5 0.5-4Hz | Pan Ret | HF Ratio | W4 Chroma dB |\n"
		              << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";

		size_t evaluatedClips = 0;
		for (const RealClipSpec& cs : clips)
		{
			if (!FileExistsReadOnly(cs.masterPath) || !FileExistsReadOnly(cs.encodedPath))
				continue;

			const int nFrames = (maxFramesOverride > 0) ? maxFramesOverride : cs.defaultFrames;
			// Exact 64-px CTU-aligned native 1:1 center crop (384x256 = 6x4 CTUs; zero resampling, preserves codec CTU/TU grid).
			const int cropW = fullFrame ? cs.fullW : 384;
			const int cropH = fullFrame ? cs.fullH : 256;
			const int cropX = fullFrame ? 0 : (((cs.fullW - cropW) / 2) & ~63);
			const int cropY = fullFrame ? 0 : (((cs.fullH - cropH) / 2) & ~63);

			std::vector<DecodedYuv420Frame10> masterFrames;
			std::vector<DecodedYuv420Frame10> recFrames;
			Assert::IsTrue(
				DecodeYuv420p10CropViaFfmpeg(ffmpegExe, cs.masterPath, cs.fullW, cs.fullH, cropX, cropY, cropW, cropH, nFrames, masterFrames),
				(std::wstring(L"Failed to decode master clip: ") + std::wstring(cs.id, cs.id + std::strlen(cs.id))).c_str());
			Assert::IsTrue(
				DecodeYuv420p10CropViaFfmpeg(ffmpegExe, cs.encodedPath, cs.fullW, cs.fullH, cropX, cropY, cropW, cropH, nFrames, recFrames),
				(std::wstring(L"Failed to decode encoded clip: ") + std::wstring(cs.id, cs.id + std::strlen(cs.id))).c_str());
			++evaluatedClips;

			const int cellPx = kCanonicalBlockPx;
			const int cw = (cropW + cellPx - 1) / cellPx;
			const int ch = (cropH + cellPx - 1) / cellPx;
			const size_t nCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);

			// Run authoritative CPU GrainSplit::Compute once per received frame.
			GrainSplit::Params gsParams = GrainSplit::DefaultParams();
			gsParams.sourceBits = 10;
			std::vector<TdV2Plane> recCleanY(static_cast<size_t>(nFrames));
			std::vector<std::vector<double>> recRetainedGrainVar(static_cast<size_t>(nFrames));
			std::vector<ThreeBandPlanes> recCleanBands(static_cast<size_t>(nFrames));
			for (int t = 0; t < nFrames; ++t)
			{
				GrainSplit::Plane gp(cropW, cropH);
				gp.v = recFrames[t].y.v;
				const GrainSplit::Split sp = GrainSplit::Compute(gp, 1.0, cellPx, gsParams);
				recCleanY[t] = TdV2Plane(cropW, cropH);
				recCleanY[t].v = sp.clean.v;
				recRetainedGrainVar[t].assign(nCells, 0.0);
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						const int gx = (std::min)(sp.bw - 1, MapCellToGrainGrid(cx, cellPx, sp.B));
						const int gy = (std::min)(sp.bh - 1, MapCellToGrainGrid(cy, cellPx, sp.B));
						// Leaked grain variance proxy in clean = N_block * mean(f^2) over cell.
						double sumF2 = 0.0;
						size_t cnt = 0;
						for (int y = cy * cellPx; y < (std::min)(cropH, (cy + 1) * cellPx); ++y)
						{
							for (int x = cx * cellPx; x < (std::min)(cropW, (cx + 1) * cellPx); ++x)
							{
								const double fv = sp.f.at(x, y);
								sumF2 += fv * fv;
								++cnt;
							}
						}
						const double meanF2 = cnt > 0 ? (sumF2 / static_cast<double>(cnt)) : 0.0;
						recRetainedGrainVar[t][static_cast<size_t>(cy) * cw + cx] =
							sp.N[static_cast<size_t>(gy) * sp.bw + gx] * meanF2;
					}
				}
				recCleanBands[t] = DecomposeActiveBands(recCleanY[t], ActiveRect::Full(cropW, cropH), nullptr);
			}

			// Compute adjacent motion (t -> t-1) for TD-U1 AlignedSharpnessA2e + grid-free & 8-px step summaries.
			std::vector<CellMotionField> adjMv(static_cast<size_t>(nFrames));
			std::vector<CodingGridSummary> nonBlockSummaries(static_cast<size_t>(nFrames));
			std::vector<CodingGridSummary> leg8Summaries(static_cast<size_t>(nFrames));
			std::vector<QualityPattern::Record> rolesAligned(static_cast<size_t>(nFrames));
			std::vector<QualityPattern::Record> rolesNonBlock(static_cast<size_t>(nFrames));
			std::vector<QualityPattern::Record> rolesLeg8(static_cast<size_t>(nFrames));

			TdV2NonBlockStepEstimator nonBlockEst;
			TdV2CodingGridStandIn leg8Est;
			AlignedSharpnessTracker sharpTracker;
			TdV2FrameRoleDetector detAligned(2), detNonBlock(2), detLeg8(2);

			auto recordDecision = [](std::vector<QualityPattern::Record>& dst, const FrameRoleDecision& d)
			{
				for (const auto& fl : d.flushedBeforeCut)
					if (fl.produced && fl.record.seq < dst.size())
						dst[static_cast<size_t>(fl.record.seq)] = fl.record;
				if (d.produced && d.record.seq < dst.size())
					dst[static_cast<size_t>(d.record.seq)] = d.record;
			};

			for (int t = 0; t < nFrames; ++t)
			{
				const double* p1 = (t >= 1) ? recFrames[t - 1].y.v.data() : nullptr;
				const double* p2 = (t >= 2) ? recFrames[t - 2].y.v.data() : nullptr;
				nonBlockSummaries[t] = nonBlockEst.MeasureWithOverlay(
					recFrames[t].y.v.data(), p1, p2, cropW, cropH, 10, nullptr, cellPx, nullptr);
				leg8Summaries[t] = leg8Est.Measure(
					recFrames[t].y.v.data(), p1, p2, cropW, cropH, 10, nullptr);

				AlignedSharpnessSummary asSum;
				asSum.exactCopyFracPrev1 = nonBlockSummaries[t].exactCopyFracPrev1;
				asSum.exactCopyFracPrev2 = nonBlockSummaries[t].exactCopyFracPrev2;
				if (t >= 1)
				{
					adjMv[t] = EstimateForwardBackwardMotionField(recCleanY[t], recCleanY[t - 1], cellPx, 16);
					TdV2Plane warpedPrev;
					std::vector<uint8_t> validPx;
					WarpCleanReference(recCleanY[t - 1], recCleanY[t], ActiveRect::Full(cropW, cropH),
					                   adjMv[t].vx, adjMv[t].vy, cw, ch, cellPx, warpedPrev, validPx,
					                   nullptr, &adjMv[t].valid, &adjMv[t].cls, nullptr, nullptr, true);
					const ThreeBandPlanes prevB = DecomposeActiveBands(warpedPrev, ActiveRect::Full(cropW, cropH), nullptr);

					std::vector<double> curM(nCells, 0.0), curH(nCells, 0.0), prvM(nCells, 0.0), prvH(nCells, 0.0);
					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
							double cm = 0.0, chE = 0.0, pm = 0.0, ph = 0.0;
							size_t cnt = 0;
							for (int y = cy * cellPx; y < (std::min)(cropH, (cy + 1) * cellPx); ++y)
							{
								for (int x = cx * cellPx; x < (std::min)(cropW, (cx + 1) * cellPx); ++x)
								{
									const size_t pix = static_cast<size_t>(y) * cropW + x;
									if (!validPx[pix]) continue;
									cm += recCleanBands[t].M.v[pix] * recCleanBands[t].M.v[pix];
									chE += recCleanBands[t].H.v[pix] * recCleanBands[t].H.v[pix];
									pm += prevB.M.v[pix] * prevB.M.v[pix];
									ph += prevB.H.v[pix] * prevB.H.v[pix];
									++cnt;
								}
							}
							if (cnt > 0)
							{
								const double inv = 1.0 / static_cast<double>(cnt);
								curM[cIdx] = cm * inv; curH[cIdx] = chE * inv;
								prvM[cIdx] = pm * inv; prvH[cIdx] = ph * inv;
							}
						}
					}
					std::vector<uint8_t> motTrust = adjMv[t].valid;
					for (size_t i = 0; i < nCells; ++i)
						if (adjMv[t].fbErrorPx[i] > 1.0) motTrust[i] = 0u;
					asSum = MeasureAlignedSharpnessFromCellEnergies(
						curM, curH, prvM, prvH, adjMv[t].vx, adjMv[t].vy, motTrust,
						nullptr, &recRetainedGrainVar[t], 10,
						nonBlockSummaries[t].exactCopyFracPrev1, nonBlockSummaries[t].exactCopyFracPrev2);
				}
				const FrameRoleEvidence evAligned = sharpTracker.PushPairAndBuildEvidence(
					static_cast<uint64_t>(t), asSum, nullptr, false, false, 10);
				recordDecision(rolesAligned, detAligned.PushAndClassify(evAligned));
				recordDecision(rolesNonBlock, detNonBlock.PushAndClassify(
					FrameRoleEvidence::FromCodingGrid(static_cast<uint64_t>(t), nonBlockSummaries[t], false, false, 10)));
				recordDecision(rolesLeg8, detLeg8.PushAndClassify(
					FrameRoleEvidence::FromCodingGrid(static_cast<uint64_t>(t), leg8Summaries[t], false, false, 10)));
			}
			for (const auto& fd : detAligned.FlushEndOfStream())
				if (fd.produced && fd.record.seq < rolesAligned.size()) rolesAligned[static_cast<size_t>(fd.record.seq)] = fd.record;
			for (const auto& fd : detNonBlock.FlushEndOfStream())
				if (fd.produced && fd.record.seq < rolesNonBlock.size()) rolesNonBlock[static_cast<size_t>(fd.record.seq)] = fd.record;
			for (const auto& fd : detLeg8.FlushEndOfStream())
				if (fd.produced && fd.record.seq < rolesLeg8.size()) rolesLeg8[static_cast<size_t>(fd.record.seq)] = fd.record;

			// Part A: Evaluate picture-only role detector accuracy against x265 CSV ground truth when available.
			const std::vector<EncoderFrameCsvEntry> csv = ParseX265CsvDisplayOrder(cs.x265CsvPath, nFrames);
			if (static_cast<int>(csv.size()) >= nFrames)
			{
				auto evalRoleMode = [&](const char* modeLabel, const std::vector<QualityPattern::Record>& recs)
				{
					size_t encPeaks = 0, detPeaks = 0, truePosPeaks = 0, encValleys = 0, detTrueValleys = 0;
					std::vector<double> scores, negQp;
					for (int t = 1; t < nFrames; ++t)
					{
						const bool gtPeak = (csv[t].sliceType == 'I' || csv[t].sliceType == 'P' || csv[t].sliceType == 'B');
						const bool gtValley = (csv[t].sliceType == 'b');
						if (gtPeak) ++encPeaks;
						if (gtValley) ++encValleys;
						const auto role = static_cast<QualityPattern::FrameRole>(recs[t].role);
						if (role == QualityPattern::FrameRole::Peak)
						{
							++detPeaks;
							if (gtPeak) ++truePosPeaks;
						}
						if (gtValley && role != QualityPattern::FrameRole::Peak)
							++detTrueValleys;
						scores.push_back(static_cast<double>(recs[t].score));
						negQp.push_back(-csv[t].qp);
					}
					const double prec = detPeaks > 0 ? (100.0 * static_cast<double>(truePosPeaks) / static_cast<double>(detPeaks)) : 0.0;
					const double vRec = encValleys > 0 ? (100.0 * static_cast<double>(detTrueValleys) / static_cast<double>(encValleys)) : 0.0;
					const double rho = SpearmanRankCorr(scores, negQp);
					char row[320];
					std::snprintf(row, sizeof(row), "| %s | %s | %zu | %zu | %.1f%% | %.1f%% | %+.3f |\n",
						cs.id, modeLabel, encPeaks, detPeaks, prec, vRec, rho);
					roleReport << row;
				};
				evalRoleMode("AlignedSharpnessA2e", rolesAligned);
				evalRoleMode("NonBlockPictureStep", rolesNonBlock);
				evalRoleMode("LegacyBlockEdge8px", rolesLeg8);
			}

			// Select detected peak indices from AlignedSharpnessA2e (always seeding frame 0 as initial anchor peak).
			std::vector<bool> isPeakFrame(static_cast<size_t>(nFrames), false);
			isPeakFrame[0] = true;
			for (int t = 1; t < nFrames; ++t)
			{
				if (static_cast<QualityPattern::FrameRole>(rolesAligned[t].role) == QualityPattern::FrameRole::Peak)
					isPeakFrame[t] = true;
			}
			// Ensure bounded span (<= kContractBackRefs = 8) by promoting best-scoring frame if any gap > 8 occurs.
			int lastP = 0;
			for (int t = 1; t < nFrames; ++t)
			{
				if (isPeakFrame[t]) { lastP = t; continue; }
				if (t - lastP >= kContractBackRefs)
				{
					isPeakFrame[t] = true;
					lastP = t;
				}
			}

			// Precompute motion fields from every non-peak frame to its bracketing prevPeak & nextPeak ONCE per clip!
			std::vector<int> prevPeakIdx(static_cast<size_t>(nFrames), -1);
			std::vector<int> nextPeakIdx(static_cast<size_t>(nFrames), -1);
			std::vector<CellMotionField> prevPeakMv(static_cast<size_t>(nFrames));
			std::vector<CellMotionField> nextPeakMv(static_cast<size_t>(nFrames));
			int curPrev = 0;
			for (int t = 0; t < nFrames; ++t)
			{
				if (isPeakFrame[t])
				{
					if (t > 0) prevPeakIdx[t] = curPrev;
					curPrev = t;
				}
				else
				{
					prevPeakIdx[t] = curPrev;
				}
			}
			int curNext = -1;
			for (int t = nFrames - 1; t >= 0; --t)
			{
				if (isPeakFrame[t])
				{
					curNext = t;
				}
				else if (curNext > t && (curNext - t) <= kContractAheadCeiling)
				{
					nextPeakIdx[t] = curNext;
				}
			}
			for (int t = 1; t < nFrames; ++t)
			{
				if (prevPeakIdx[t] >= 0)
					prevPeakMv[t] = EstimateForwardBackwardMotionField(recCleanY[t], recCleanY[prevPeakIdx[t]], cellPx, 20);
				if (nextPeakIdx[t] >= 0)
					nextPeakMv[t] = EstimateForwardBackwardMotionField(recCleanY[t], recCleanY[nextPeakIdx[t]], cellPx, 20);
			}

			// Baseline received P5 0.5-4 Hz energy & master high-frequency energy.
			std::vector<TdV2Plane> recYVec(static_cast<size_t>(nFrames));
			for (int t = 0; t < nFrames; ++t) recYVec[t] = recFrames[t].y;
			const double recFlickerE = ComputeFlickerBandEnergy05To4Hz(recYVec, masterFrames, cs.fps, cellPx);

			// Run all modular ablation arms on the exact same decoded frames & motion fields!
			for (const AblationArmSpec& arm : arms)
			{
				Config cfg = arm.cfg;
				cfg.sourceHz = cs.fps;
				TdV2ReferenceEngine lumaEng(cfg);
				TdV2ChromaEngine chromaEng(cfg);

				std::vector<TdV2Plane> outYFrames(static_cast<size_t>(nFrames));
				double sumGlobalDeltaDb = 0.0, sumW3DeltaDb = 0.0, sumWorst5Db = 0.0, sumWorsened = 0.0;
				double sumW1Db = 0.0, sumW1Step = 0.0, sumW2Db = 0.0, sumPanRet = 0.0, sumHfRatio = 0.0, sumW4ChromaDb = 0.0;
				size_t cntGlobal = 0, cntW3 = 0, cntW1 = 0, cntW2 = 0, cntPan = 0, cntHf = 0, cntW4 = 0;

				for (int t = 0; t < nFrames; ++t)
				{
					TdV2Plane outY = recFrames[t].y;
					TdV2Plane outCb = recFrames[t].cb;
					TdV2Plane outCr = recFrames[t].cr;

					if (!arm.isOff && arm.isOracleA4)
					{
						if (!isPeakFrame[t])
						{
							const TdV2Plane* pp = (prevPeakIdx[t] >= 0) ? &recCleanY[prevPeakIdx[t]] : nullptr;
							const CellMotionField* pm = (prevPeakIdx[t] >= 0) ? &prevPeakMv[t] : nullptr;
							const TdV2Plane* np = (nextPeakIdx[t] >= 0) ? &recCleanY[nextPeakIdx[t]] : nullptr;
							const CellMotionField* nm = (nextPeakIdx[t] >= 0) ? &nextPeakMv[t] : nullptr;
							outY = RunOracleA4Frame(cfg, recFrames[t].y, recCleanY[t], masterFrames[t].y, pp, pm, np, nm);
						}
					}
					else if (!arm.isOff)
					{
						FrameProcessInput fIn;
						fIn.seq = static_cast<uint64_t>(t);
						fIn.receivedY = recFrames[t].y;
						fIn.hasCleanSplit = true;
						fIn.cleanY = recCleanY[t];
						fIn.retainedGrainVar = recRetainedGrainVar[t];
						fIn.frameRole = static_cast<QualityPattern::FrameRole>(rolesAligned[t].role);
						fIn.isConfirmedPeak = isPeakFrame[t];

						if (!isPeakFrame[t] && prevPeakIdx[t] >= 0)
						{
							fIn.prevPeak.cleanPlane = &recCleanY[prevPeakIdx[t]];
							fIn.prevPeak.motion = &prevPeakMv[t];
							if (cfg.enableAnchoredCarry)
							{
								fIn.hasAnchorComposedMotion = true;
								fIn.anchorComposedMotion = prevPeakMv[t];
							}
						}
						else if (isPeakFrame[t] && t > 0 && prevPeakIdx[t] >= 0 && cfg.enableAnchoredCarry)
						{
							fIn.hasAnchorComposedMotion = true;
							fIn.anchorComposedMotion = prevPeakMv[t];
							fIn.hasDirectPeakToPeakMotion = true;
							fIn.directPeakToPeakMotion = prevPeakMv[t];
						}
						if (!isPeakFrame[t] && nextPeakIdx[t] >= 0)
						{
							fIn.nextPeak.cleanPlane = &recCleanY[nextPeakIdx[t]];
							fIn.nextPeak.motion = &nextPeakMv[t];
						}

						if (arm.feedNonBlockStepPrior && nonBlockSummaries[t].stepMean > 0.0)
						{
							fIn.hasStepPrior = true;
							fIn.estimatedStepDelta = nonBlockSummaries[t].stepMean;
						}
						else if (arm.feedLegacy8pxStepPrior && leg8Summaries[t].stepMean > 0.0)
						{
							fIn.hasStepPrior = true;
							fIn.estimatedStepDelta = leg8Summaries[t].stepMean;
						}

						const FrameProcessResult lRes = lumaEng.ProcessFrame(fIn);
						if (!isPeakFrame[t])
							outY = lRes.outputY;

						// Run production coded-grid chroma recovery on non-peak frames.
						if (!isPeakFrame[t])
						{
							ChromaProcessInput cIn;
							cIn.seq = static_cast<uint64_t>(t);
							cIn.lumaW = cropW;
							cIn.lumaH = cropH;
							cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
							cIn.codedSubsampling = ChromaSubsampling::Yuv420;
							cIn.gridStatus = ChromaGridStatus::Identified;
							cIn.receivedY = recFrames[t].y;
							cIn.receivedCb = recFrames[t].cb;
							cIn.receivedCr = recFrames[t].cr;
							cIn.frameRole = fIn.frameRole;
							if (prevPeakIdx[t] >= 0)
							{
								cIn.prevPeak.cleanCb = &recFrames[prevPeakIdx[t]].cb;
								cIn.prevPeak.cleanCr = &recFrames[prevPeakIdx[t]].cr;
								cIn.prevPeak.lumaMotion = &prevPeakMv[t];
							}
							if (nextPeakIdx[t] >= 0)
							{
								cIn.nextPeak.cleanCb = &recFrames[nextPeakIdx[t]].cb;
								cIn.nextPeak.cleanCr = &recFrames[nextPeakIdx[t]].cr;
								cIn.nextPeak.lumaMotion = &nextPeakMv[t];
							}
							std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> packedLw;
							if (cfg.chromaReuseLumaWeights && lRes.lumaActiveRefCount > 0)
							{
								for (int b = 0; b < 3; ++b)
									for (int k = 0; k < kMaxBlendRefs; ++k)
										packedLw[b][k] = lRes.lumaCellWeights[b][k];
								cIn.lumaCellWeights = &packedLw;
								cIn.lumaRefRoles = lRes.lumaRefRoles;
								cIn.lumaActiveRefCount = lRes.lumaActiveRefCount;
							}
							const ChromaProcessResult cRes = chromaEng.ProcessFrame(cIn);
							outCb = cRes.outputCb;
							outCr = cRes.outputCr;
						}
					}

					outYFrames[t] = outY;

					// Evaluate W1-W4, worst-5%, speed-bin sharpness, HF grain ratio, and W4 chroma edge PSNR on valley frames.
					const bool isValley = !isPeakFrame[t];
					const CellMotionField* evalMv = (prevPeakIdx[t] >= 0) ? &prevPeakMv[t] : (t >= 1 ? &adjMv[t] : nullptr);
					const std::vector<uint8_t> mask = BuildClipWorstAreaMask(masterFrames[t], evalMv, isValley, cellPx);
					const WorstAreaReport wa = EvaluateWorstAreaMasks(
						masterFrames[t].y.v.data(), recFrames[t].y.v.data(), outY.v.data(),
						mask.data(), cropW, cropH, cellPx, 1023.0);
					if (wa.valid)
					{
						sumGlobalDeltaDb += wa.global.deltaDb;
						sumWorst5Db += wa.worst5PctMeanDeltaDb;
						sumWorsened += 100.0 * wa.worst5PctWorsenedOver01DbFrac;
						++cntGlobal;
						if (wa.w3BetweenKeyframes.valid) { sumW3DeltaDb += wa.w3BetweenKeyframes.deltaDb; ++cntW3; }
						if (wa.w1BandedSky.valid)
						{
							sumW1Db += wa.w1BandedSky.deltaDb;
							sumW1Step += 100.0 * wa.w1BandingStepReduction;
							++cntW1;
						}
						if (wa.w2DarkStill.valid) { sumW2Db += wa.w2DarkStill.deltaDb; ++cntW2; }
					}

					if (evalMv != nullptr && evalMv->IsWellFormed(cw, ch))
					{
						std::vector<double> objSpd(static_cast<size_t>(cropW) * cropH, 0.0);
						std::vector<double> camSpd(static_cast<size_t>(cropW) * cropH, 0.0);
						std::vector<uint8_t> camBg(static_cast<size_t>(cropW) * cropH, 0u);
						for (int y = 0; y < cropH; ++y)
						{
							const int cy = (std::min)(ch - 1, y / cellPx);
							for (int x = 0; x < cropW; ++x)
							{
								const int cx = (std::min)(cw - 1, x / cellPx);
								const size_t ci = static_cast<size_t>(cy) * cw + cx;
								const size_t pi = static_cast<size_t>(y) * cropW + x;
								objSpd[pi] = std::hypot(evalMv->vx[ci] - evalMv->camVx[ci], evalMv->vy[ci] - evalMv->camVy[ci]);
								camSpd[pi] = std::hypot(evalMv->camVx[ci], evalMv->camVy[ci]);
								camBg[pi] = (evalMv->cls[ci] == RegionMotionClass::Camera) ? 1u : 0u;
							}
						}
						const SpeedBinReport sb = EvaluateSpeedBins(
							masterFrames[t].y.v.data(), recFrames[t].y.v.data(), outY.v.data(),
							objSpd.data(), camSpd.data(), camBg.data(), cropW, cropH, 1023.0);
						if (sb.cameraPan.valid) { sumPanRet += sb.cameraPan.sharpnessRetention; ++cntPan; }
					}

					// Fine-band RMS retention relative to received frame (checks zero plastic grain wipe).
					double recHfE = 0.0, outHfE = 0.0;
					for (int y = 1; y < cropH - 1; ++y)
					{
						for (int x = 1; x < cropW - 1; ++x)
						{
							const double lapRec = 4.0 * recFrames[t].y.at(x, y) - recFrames[t].y.at(x - 1, y) -
								recFrames[t].y.at(x + 1, y) - recFrames[t].y.at(x, y - 1) - recFrames[t].y.at(x, y + 1);
							const double lapOut = 4.0 * outY.at(x, y) - outY.at(x - 1, y) -
								outY.at(x + 1, y) - outY.at(x, y - 1) - outY.at(x, y + 1);
							recHfE += lapRec * lapRec;
							outHfE += lapOut * lapOut;
						}
					}
					if (recHfE > 1e-9) { sumHfRatio += std::sqrt(outHfE / recHfE); ++cntHf; }

					// W4 colored chroma edges PSNR delta (Cb+Cr combined MSE on W4 chroma-edge pixels).
					if (isValley)
					{
						double baseCbCrErr = 0.0, outCbCrErr = 0.0;
						size_t w4Cnt = 0;
						for (int uy = 0; uy < outCb.h; ++uy)
						{
							for (int ux = 0; ux < outCb.w; ++ux)
							{
								const size_t lumaIdx = static_cast<size_t>(uy * 2) * cropW + (ux * 2);
								if ((mask[lumaIdx] & kMaskW4ChromaEdges) == 0u)
									continue;
								const double eb = (recFrames[t].cb.at(ux, uy) - masterFrames[t].cb.at(ux, uy));
								const double er = (recFrames[t].cr.at(ux, uy) - masterFrames[t].cr.at(ux, uy));
								const double ob = (outCb.at(ux, uy) - masterFrames[t].cb.at(ux, uy));
								const double orr = (outCr.at(ux, uy) - masterFrames[t].cr.at(ux, uy));
								baseCbCrErr += 0.5 * (eb * eb + er * er);
								outCbCrErr += 0.5 * (ob * ob + orr * orr);
								++w4Cnt;
							}
						}
						if (w4Cnt > 0)
						{
							double dbB = 0.0, dbO = 0.0;
							if (ComputePsnrDb(baseCbCrErr / static_cast<double>(w4Cnt), 1023.0, dbB) &&
							    ComputePsnrDb(outCbCrErr / static_cast<double>(w4Cnt), 1023.0, dbO))
							{
								sumW4ChromaDb += (dbO - dbB);
								++cntW4;
							}
						}
					}
				}

				const double outFlickerE = ComputeFlickerBandEnergy05To4Hz(outYFrames, masterFrames, cs.fps, cellPx);
				const double p5Ratio = (recFlickerE > 1e-12) ? (outFlickerE / recFlickerE) : 1.0;

				char line[512];
				std::snprintf(
					line, sizeof(line),
					"| %s | %s | %+.3f | %+.3f | %+.3f | %.1f%% | %+.3f | %+.1f%% | %+.3f | %.3fx | %.3f | %.3f | %+.3f |\n",
					cs.id,
					arm.name,
					cntGlobal ? (sumGlobalDeltaDb / cntGlobal) : 0.0,
					cntW3 ? (sumW3DeltaDb / cntW3) : 0.0,
					cntGlobal ? (sumWorst5Db / cntGlobal) : 0.0,
					cntGlobal ? (sumWorsened / cntGlobal) : 0.0,
					cntW1 ? (sumW1Db / cntW1) : 0.0,
					cntW1 ? (sumW1Step / cntW1) : 0.0,
					cntW2 ? (sumW2Db / cntW2) : 0.0,
					p5Ratio,
					cntPan ? (sumPanRet / cntPan) : 1.0,
					cntHf ? (sumHfRatio / cntHf) : 1.0,
					cntW4 ? (sumW4ChromaDb / cntW4) : 0.0);
				ablationTable << line;
			}
		}

		Assert::IsTrue(evaluatedClips >= 4u, L"Expected at least 4 real encoded clips evaluated in TD-U0");
		report << roleReport.str() << ablationTable.str();
		Logger::WriteMessage(report.str().c_str());
	}
};

} // namespace Tests
