// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S4A A5 (refs/112 pattern P4, static registration): compiled into the VideoProcessor-GUI executable through
// src/VP-V/build/VideoProcessor-GUI.items.props, so the executable needs no call line of its own.
// C15: the one recent-lines ring (CrashState) listens to DebugLog from before the first line of
// InitInstance, as the call at the top of the startup path did. InstallLogRing only stores one observer
// pointer, uses no other static of this translation unit and is idempotent, so running it during static
// initialization is safe.
#include "CrashState.h"

namespace
{
	struct ExeLogRingRegistration
	{
		ExeLogRingRegistration() { CrashState::InstallLogRing(); }
	};
	const ExeLogRingRegistration g_exeLogRingRegistration;
}
