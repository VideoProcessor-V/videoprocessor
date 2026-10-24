#pragma once

// Chroma siting through VP's own resampling stages (chroma investigation research/chroma
// c7564afa, 2026-10-24).
//
// libplacebo positions a subsampled chroma plane by its shift (pl_plane::shift_x / shift_y,
// in the plane's luma-relative units: -0.5 for LEFT-sited 4:2:x). A VP stage that resamples the
// whole picture by a factor with an edge-aligned mapping (the re-base, later the 4K route or a
// chroma slot) scales that offset by the same factor: keeping the old shift would move the colour
// by a fraction of a resampled luma pixel (a quarter of one at 2:1 for the re-base).
//
// Header-only, C++14.
namespace ChromaSiting
{
	// The plane shift after an edge-aligned resample of the picture by `factor` on that axis.
	inline float ShiftAfterResample(float shift, double factor)
	{
		return static_cast<float>(static_cast<double>(shift) * factor);
	}
}
