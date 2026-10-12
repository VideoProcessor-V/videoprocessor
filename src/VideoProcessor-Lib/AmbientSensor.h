#pragma once

#include "led/SerialPort.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Stage 9b (docs/VP-stage9-ambient.md): the room light sensor over a serial
// port (USB). The device prints one reading per line, in lux: a plain number
// ("12.3"), or "lux=12.3" / "lux: 12.3" / "12.3 lux". Any other line (a
// banner, a comment) is ignored. The bundled sketch
// tools/light-sensor/vp_light_sensor.ino does this for an ESP32 or Arduino
// with a VEML7700 or TSL2591.
//
// The reader owns its thread and port (the LED module's pattern): it opens
// the port, reads lines, keeps the newest reading, and reopens the port after
// an error or 10 s of silence (unplugged), every 5 s, until stopped. The GUI
// polls the newest reading once a second and feeds Ambient::State.
namespace Ambient
{
	bool ParseLuxLine(const std::string& line, double& lux);

	class SensorReader
	{
	public:
		using PortFactory = std::function<std::unique_ptr<LedDrive::ISerialPort>()>;

		struct Timing
		{
			int readTimeoutMs = 1000;
			int silenceReopenMs = 10000;   // no line this long: reopen
			int retryMs = 5000;            // after a failed open
		};

		SensorReader() = default;
		~SensorReader() { Stop(); }
		SensorReader(const SensorReader&) = delete;
		SensorReader& operator=(const SensorReader&) = delete;

		void Start(const std::string& port, int baud, PortFactory factory, Timing timing = Timing());
		void Stop();
		bool Running() const { return worker.joinable(); }
		const std::string& Port() const { return portName; }
		int Baud() const { return baudRate; }
		// The newest reading, when newer than `sequence` (updated).
		bool Newest(uint64_t& sequence, double& lux) const;
		// What the reader is doing, for the log and the overlay.
		std::string Status() const;

	private:
		void Run();
		void SetStatus(const std::string& value);
		bool WaitStop(int ms);

		mutable std::mutex mutex;
		std::condition_variable wake;
		std::thread worker;
		bool stopping = false;
		std::string portName;
		int baudRate = 115200;
		PortFactory factory;
		Timing timing;
		uint64_t readingSequence = 0;
		double readingLux = 0.0;
		std::string status;
	};
}
