#pragma once

#include <VP-V/modules/drive/projector_control/ProjectorDriver.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>

#include <memory>
#include <string>

// VP Renderer filter (2026-10-17): VideoProcessor.exe and the DirectShow
// filter (inside MPC-HC / MPC-BE) both carry the LED drive, and the projector
// must never be driven by two of them at once. The drive's serial port is
// wrapped: Open() first takes a machine-wide named mutex and Close() gives it
// back. A second owner's Open() fails with the reason, so its drive runs as a
// dry run (LedDriveModule logs "continuing as a dry run") and never touches
// the picture or the projector.
//
// Global\ rather than Local\: the COM port is a machine resource. Local\ is
// per logon session, so VP.exe in one session (or elevated in a different
// session after fast user switching) and a player in another would not see
// each other's mutex. Creating a named mutex in Global\ needs no privilege
// (only file mappings do). A mutex another account created may refuse access;
// that is reported as "held by another owner", which is what it means.
//
// The mutex is held on a thread of its own: a Win32 mutex belongs to the
// thread that took it and is recursive on that thread, while the drive opens
// and closes its port from different threads, and two owners in one process
// (the filter's next graph while the previous one is still retiring) must
// exclude each other just as two processes do. When nobody else holds it the
// wrapper is transparent: every call is forwarded unchanged.
namespace LedDrive
{
	static const wchar_t* const kLedDriveMutexName = L"Global\\VideoProcessorLedDrive";

	// One held ownership of a named drive mutex; released on destruction.
	class DriveOwnership
	{
	public:
		// Null (with `reason`) when another owner holds it or it cannot be made.
		static std::unique_ptr<DriveOwnership> TryAcquire(const std::wstring& name, std::string& reason);
		~DriveOwnership();
		DriveOwnership(const DriveOwnership&) = delete;
		DriveOwnership& operator=(const DriveOwnership&) = delete;
		// The previous owner ended without releasing it (a crash): logged once.
		bool Abandoned() const { return abandoned; }

	private:
		struct Holder;
		explicit DriveOwnership(std::unique_ptr<Holder> holder, bool abandoned);
		std::unique_ptr<Holder> holder;
		bool abandoned = false;
	};

	class ArbitratedSerialPort : public ISerialPort
	{
	public:
		ArbitratedSerialPort(std::unique_ptr<ISerialPort> inner, const std::wstring& mutexName);
		~ArbitratedSerialPort() override;
		bool Open(const std::string& port, int baud, std::string& error) override;
		void Close() override;
		bool IsOpen() const override;
		void DiscardInput() override;
		bool Write(const std::string& bytes, std::string& error) override;
		bool ReadLine(int timeoutMs, std::string& line) override;
		// Tests and diagnostics.
		bool OwnsDrive() const { return ownership != nullptr; }

	private:
		std::unique_ptr<ISerialPort> inner;
		std::wstring mutexName;
		std::unique_ptr<DriveOwnership> ownership;
		// The last refusal logged, so a reopen loop says it once per change.
		std::string lastRefusal;
	};

	// Wraps every port the factory makes (default: the Win32 port).
	SerialPortFactory ArbitratedSerialPortFactory(SerialPortFactory inner = SerialPortFactory(),
		const std::wstring& mutexName = kLedDriveMutexName);
}
