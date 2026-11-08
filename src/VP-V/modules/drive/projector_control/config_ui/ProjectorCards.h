// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The projector's cards on the Dynamic Drive page of the Config editor (drive.projector_control's own
// settings: the [projector.default] serial link, its response timings and the advanced serial details)
// and the RS-232 connection test button. They were part of the editor's createLedDrivePage; the page
// (led_compensation/config_ui/LedDrivePage) places them in its card grid at their old positions and
// reads the few controls its show/hide rules depend on. UI thread only.

#include "DriveConfigBase.h"

#include <QPointer>
#include <QThread>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QThread;

namespace Vpv
{
	class ProjectorCards final : public DriveConfig::DriveConfigBase
	{
	public:
		explicit ProjectorCards(ConfigPageHost::EditorKit& kit) : DriveConfig::DriveConfigBase(kit) {}

		// The "Projector connection" content: protocol, port, response times, read-back, the RS-232 test.
		struct Connection { QWidget* content = nullptr; QComboBox* protocolChoice = nullptr; };
		Connection BuildConnection();

		// The "Projector response" content: settle times and the contrast limits.
		struct Response { QWidget* content = nullptr; QFormLayout* form = nullptr; QDoubleSpinBox* maxUsable = nullptr; QDoubleSpinBox* whiteClips = nullptr; };
		Response BuildResponse();

		// The advanced serial settings (collapsed behind a toggle): the raw form, then the holder with the toggle.
		QWidget* BuildAdvanced();
		QWidget* WrapAdvanced(QWidget* advancedProjector);

		// The RS-232 test runs on a helper thread that outlives page rebuilds; the editor closing joins it.
		void JoinTest();
		bool TestRunning() const { return !projectorTestThread_.isNull(); }
		// Test seam: stands in for a running RS-232 test (a thread the caller controls).
		void AdoptTestThreadForTesting(QThread* thread) { projectorTestThread_ = thread; }

	private:
		QPointer<QThread> projectorTestThread_;
	};
}
