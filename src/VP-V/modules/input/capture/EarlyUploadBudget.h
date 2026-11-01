// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Bug hunt 38 M3: the early-upload pool's byte budget as a pure rule (no GPU, no Windows).
// After a frame-size change, textures of the old size can still be held or in GPU use while
// new-size ones are made, so live bytes could reach twice the cap. A new texture is made only
// when the live bytes plus the new frame still fit the cap.
#pragma once

#include <cstddef>

namespace EarlyUploadBudget
{
	inline bool MayMakeSlot(size_t liveBytes, size_t frameBytes, size_t capBytes)
	{
		if (frameBytes > capBytes)
			return false;
		return liveBytes <= capBytes - frameBytes;
	}
}
