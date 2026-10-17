#pragma once

#include <ACaptureDevice.h>

// Bug hunt 2 (2026-10-17, M3): when the GUI must keep "Restart capture"
// available after a device state message. Pure, so it is unit tested.
namespace CaptureStatePolicy
{
	// newState: what the device reported (FAILED included).
	// previousState: the GUI's tracked state before the message.
	// Devices report READY after STARTING only when the start failed (setup
	// error or an early capture-thread failure: Magewell posts Error() then
	// READY). The GUI's input-change path does not retry an unchanged input,
	// so without Restart the capture stayed stuck at "Ready" with no picture.
	inline bool StartFailed(CaptureDeviceState newState, CaptureDeviceState previousState)
	{
		return (newState == CaptureDeviceState::CAPTUREDEVICESTATE_READY ||
				newState == CaptureDeviceState::CAPTUREDEVICESTATE_FAILED) &&
			previousState == CaptureDeviceState::CAPTUREDEVICESTATE_STARTING;
	}

	inline bool RestartAvailable(CaptureDeviceState newState, CaptureDeviceState previousState)
	{
		switch (newState)
		{
		case CaptureDeviceState::CAPTUREDEVICESTATE_CAPTURING:
		case CaptureDeviceState::CAPTUREDEVICESTATE_FAILED:
			return true;
		case CaptureDeviceState::CAPTUREDEVICESTATE_READY:
			// Unexpected stop while capturing, or a failed start.
			return previousState == CaptureDeviceState::CAPTUREDEVICESTATE_CAPTURING ||
				StartFailed(newState, previousState);
		default:
			return false;
		}
	}
}
