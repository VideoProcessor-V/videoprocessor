// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC series A: the de-judder extras' compiled home (DejudderExtrasPresent.h). A1 brings the state only; the extras'
// body (Vpv::Dejudder::PresentExtras, moved from the renderer's PresentDejudderExtras) follows in series A3.
#include <pch.h>
#include "DejudderExtrasPresent.h"

#include <type_traits>

namespace Vpv
{
	namespace Dejudder
	{
		static_assert(std::is_default_constructible<ExtrasState>::value, "the renderer's Impl default-constructs its member");
	}
}
