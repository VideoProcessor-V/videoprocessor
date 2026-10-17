#include <pch.h>
#include "LedDriveHoldover.h"
#include "LedDriveModule.h"

#include <Windows.h>

namespace LedDrive
{
	Holdover<LedDriveModule>& ProcessLedDriveHoldover()
	{
		// Deliberately leaked: see the header.
		static Holdover<LedDriveModule>* holder = new Holdover<LedDriveModule>(
			[] { return static_cast<uint64_t>(GetTickCount64()); },
			[](const std::shared_ptr<LedDriveModule>& drive) { if (drive) drive->Stop(); });
		return *holder;
	}

	bool EndHeldLedDrive(const char* reason)
	{
		return ProcessLedDriveHoldover().EndNow(reason);
	}
}
