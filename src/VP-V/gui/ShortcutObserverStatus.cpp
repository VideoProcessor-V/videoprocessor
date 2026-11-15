// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3f b (refs/112 pattern P4, static registration, as ExeLogRing.cpp): compiled into the
// VideoProcessor-GUI executable through src/VP-V/build/VideoProcessor-GUI.items.props, so the dialog
// needs no call line of its own. DialogExtension::Register only appends to a function-local static
// registry, so running it during static initialization (before the dialog creates its timers) keeps
// the registration contract. The pathway is read with the shared reader (HdrInferenceChoice::Load,
// [vpv] then [vprenderer] pipeline_original_pathway through ConfigFile::NormalizeName) on the first tick.
#include "ShortcutObserverStatus.h"

#include <VP-V/modules/input/signal/HdrInferenceChoice.h>

namespace
{
	struct ShortcutObserverStatusRegistration
	{
		ShortcutObserverStatus::Extension extension{
			[] { return HdrInferenceChoice::Load().originalPathway; },
			ShortcutObserverStatus::CurrentNote() };
		ShortcutObserverStatusRegistration() { DialogExtension::Register(&extension); }
	};
	ShortcutObserverStatusRegistration g_shortcutObserverStatusRegistration;
}
