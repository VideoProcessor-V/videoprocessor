#pragma once

#include <memory>
#include <string>

// Byte transport for the LED drive module's projector protocols (and, later,
// the Stage 9 light sensor). Synchronous with timeouts; each driver owns the
// thread that uses its port. Tests substitute a fake implementation.
namespace LedDrive
{
	class ISerialPort
	{
	public:
		virtual ~ISerialPort() = default;
		// port: "COM1".."COM256"; 8 data bits, no parity, one stop bit, no flow
		// control.
		virtual bool Open(const std::string& port, int baud, std::string& error) = 0;
		virtual void Close() = 0;
		virtual bool IsOpen() const = 0;
		// Discards anything received but not yet read.
		virtual void DiscardInput() = 0;
		virtual bool Write(const std::string& bytes, std::string& error) = 0;
		// Reads up to and excluding '\r'; '\n' is dropped. False on timeout,
		// with line empty: a partial line is kept for the next call (audit
		// 2026-10-16). A failed read closes the port (IsOpen() false).
		virtual bool ReadLine(int timeoutMs, std::string& line) = 0;
	};

	// Win32 implementation: CreateFile on \\.\COMn, SetCommState,
	// SetCommTimeouts.
	class Win32SerialPort : public ISerialPort
	{
	public:
		Win32SerialPort();
		~Win32SerialPort() override;
		bool Open(const std::string& port, int baud, std::string& error) override;
		void Close() override;
		bool IsOpen() const override;
		void DiscardInput() override;
		bool Write(const std::string& bytes, std::string& error) override;
		bool ReadLine(int timeoutMs, std::string& line) override;

	private:
		void* handle;
		std::string pending;
	};

	std::unique_ptr<ISerialPort> CreateWin32SerialPort();
}
