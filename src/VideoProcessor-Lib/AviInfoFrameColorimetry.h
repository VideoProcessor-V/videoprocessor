#pragma once

// CTA-861 AVI InfoFrame colorimetry, decoded from the payload bytes PB1..PB3.
// Used where a capture SDK reports RGB without colorimetry (Magewell reports
// RGB BT.2020 as plain RGB, which VP read as BT.709): newlldv needs the AVI
// BT.2020 flag to recognise HDR10 whose DRM InfoFrame was dropped.

#include <cstddef>
#include <cstdint>

struct AviColorimetry
{
	bool valid = false;
	int pixelEncoding = -1;  // Y: 0 RGB, 1 YCbCr 4:2:2, 2 YCbCr 4:4:4, 3 YCbCr 4:2:0
	int colorimetry = 0;     // C1C0: 0 none, 1 SMPTE 170M, 2 BT.709, 3 extended
	int extended = 0;        // EC2..EC0 (meaningful when C1C0 == 3)
	bool bt2020 = false;     // EC 5 (BT.2020 constant luminance) or 6 (BT.2020 RGB/YCC)
};

// payload: PB1 first (the checksum byte PB0 excluded), at least 3 bytes.
inline AviColorimetry DecodeAviColorimetry(const uint8_t* payload, size_t bytes)
{
	AviColorimetry result;
	if (!payload || bytes < 3)
		return result;
	result.valid = true;
	result.pixelEncoding = (payload[0] >> 5) & 0x3;
	result.colorimetry = (payload[1] >> 6) & 0x3;
	result.extended = (payload[2] >> 4) & 0x7;
	result.bt2020 = result.colorimetry == 3 &&
		(result.extended == 5 || result.extended == 6);
	return result;
}
