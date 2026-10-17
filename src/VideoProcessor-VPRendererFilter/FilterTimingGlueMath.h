#pragma once

// VP Renderer DirectShow filter: pure timing/colorimetry arithmetic behind
// FilterTimingGlue (header-only so the CPU tests include it directly).
//
// TODO(VP Renderer integration, agent C): replaced by
//   src/VideoProcessor-Lib/filesource/FileClockMapper.h (reference clock ->
//     QPC correlation, smoothing, file_av_offset_ms),
//   src/VideoProcessor-Lib/filesource/FileMediaTypeAdapter.h (colorimetry ->
//     ColorSpace/EOTF and range),
// which supersede ReferenceTimeToQpc / ColorSpaceFromColorimetry /
// EotfFromColorimetry below.

#include <Windows.h>

#include <cstdint>

#include <ColorSpace.h>
#include <EOTF.h>

#include "VPRendererFilterMediaTypes.h"


namespace VPRendererFilter
{
	// The QPC tick at which a stream time is due: dueReference is the reference
	// clock time the sample is due (tStart + rtStart, 100 ns units), sampled
	// together with nowReference / nowQpc. Exact integer split so neither the
	// product nor a long delta overflows. Returns 0 when qpcFrequency <= 0.
	inline int64_t ReferenceTimeToQpc(int64_t dueReference, int64_t nowReference,
		int64_t nowQpc, int64_t qpcFrequency)
	{
		if (qpcFrequency <= 0)
			return 0;
		const int64_t delta = dueReference - nowReference;
		const int64_t seconds = delta / 10000000;
		const int64_t remainder = delta % 10000000;
		const int64_t qpc = nowQpc + seconds * qpcFrequency +
			(remainder * qpcFrequency) / 10000000;
		// 0 means "unknown" in FileFrameTiming; a real tick of 0 is not reachable
		// on a running system, but keep the value distinct anyway.
		return qpc == 0 ? 1 : qpc;
	}

	// MFVideoPrimaries values (DXVA_ExtendedFormat::VideoPrimaries).
	inline ColorSpace ColorSpaceFromColorimetry(const Colorimetry& c)
	{
		if (!c.present)
			return ColorSpace::UNKNOWN;
		switch (c.primaries)
		{
		case 2: return ColorSpace::REC_709;       // MFVideoPrimaries_BT709
		case 4: return ColorSpace::REC_601_625;   // MFVideoPrimaries_BT470_2_SysBG
		case 5: return ColorSpace::REC_601_525;   // MFVideoPrimaries_SMPTE170M
		case 6: return ColorSpace::REC_601_525;   // MFVideoPrimaries_SMPTE240M
		case 9: return ColorSpace::BT_2020;       // MFVideoPrimaries_BT2020
		case 11: return ColorSpace::P3_DCI;       // MFVideoPrimaries_DCI_P3
		default: return ColorSpace::UNKNOWN;
		}
	}

	// MFVideoTransferFunction values, including the 2084 (15) and HLG (16)
	// extensions LAV Video and MPC use.
	inline EOTF EotfFromColorimetry(const Colorimetry& c)
	{
		if (!c.present)
			return EOTF::UNKNOWN;
		switch (c.transferFunction)
		{
		case 15: return EOTF::PQ;
		case 16: return EOTF::HLG;
		case 0: return EOTF::UNKNOWN;
		default: return EOTF::SDR;
		}
	}
}
