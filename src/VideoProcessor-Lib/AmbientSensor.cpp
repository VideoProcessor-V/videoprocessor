#include <pch.h>
#include "AmbientSensor.h"

#include "DebugLog.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace Ambient
{
	namespace
	{
		std::string Trim(const std::string& value)
		{
			size_t begin = 0, end = value.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
			return value.substr(begin, end - begin);
		}

		bool StartsWithNoCase(const std::string& value, const char* prefix, size_t& length)
		{
			length = 0;
			while (prefix[length])
			{
				if (length >= value.size() ||
					std::tolower(static_cast<unsigned char>(value[length])) != prefix[length])
					return false;
				++length;
			}
			return true;
		}
	}

	bool ParseLuxLine(const std::string& raw, double& lux)
	{
		std::string line = Trim(raw);
		size_t length = 0;
		if (StartsWithNoCase(line, "lux", length))
		{
			line = Trim(line.substr(length));
			if (line.empty() || (line[0] != '=' && line[0] != ':'))
				return false;
			line = Trim(line.substr(1));
		}
		if (line.empty() || !(std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '.'))
			return false;
		char* end = nullptr;
		const double value = std::strtod(line.c_str(), &end);
		if (end == line.c_str() || !std::isfinite(value) || value < 0.0)
			return false;
		std::string rest = Trim(std::string(end));
		if (!rest.empty())
		{
			std::string lower;
			for (char c : rest)
				lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (lower != "lux" && lower != "lx")
				return false;
		}
		lux = value;
		return true;
	}

	void SensorReader::Start(const std::string& port, int baud, PortFactory portFactory, Timing timingIn)
	{
		Stop();
		std::lock_guard<std::mutex> guard(mutex);
		portName = port;
		baudRate = baud;
		factory = std::move(portFactory);
		timing = timingIn;
		stopping = false;
		status = "starting";
		worker = std::thread(&SensorReader::Run, this);
	}

	void SensorReader::Stop()
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable())
			worker.join();
	}

	bool SensorReader::Newest(uint64_t& sequence, double& lux) const
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (readingSequence == sequence)
			return false;
		sequence = readingSequence;
		lux = readingLux;
		return true;
	}

	std::string SensorReader::Status() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return status;
	}

	void SensorReader::SetStatus(const std::string& value)
	{
		bool changed = false;
		{
			std::lock_guard<std::mutex> guard(mutex);
			changed = value != status;
			status = value;
		}
		if (changed)
			DebugLog::Log("Room light sensor: %s", value.c_str());
	}

	bool SensorReader::WaitStop(int ms)
	{
		std::unique_lock<std::mutex> guard(mutex);
		return wake.wait_for(guard, std::chrono::milliseconds(ms), [this] { return stopping; });
	}

	void SensorReader::Run()
	{
		for (;;)
		{
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (stopping)
					return;
			}
			std::unique_ptr<LedDrive::ISerialPort> port = factory ? factory() : nullptr;
			std::string error;
			if (!port || !port->Open(portName, baudRate, error))
			{
				SetStatus(portName + ": cannot open" + (error.empty() ? std::string() : " (" + error + ")"));
				if (WaitStop(timing.retryMs))
					return;
				continue;
			}
			SetStatus(portName + " open, waiting for readings");
			port->DiscardInput();
			auto lastLine = std::chrono::steady_clock::now();
			bool reading = false;
			for (;;)
			{
				{
					std::lock_guard<std::mutex> guard(mutex);
					if (stopping)
					{
						port->Close();
						return;
					}
				}
				std::string line;
				const bool complete = port->ReadLine(timing.readTimeoutMs, line);
				const auto now = std::chrono::steady_clock::now();
				double lux = 0.0;
				if (complete && ParseLuxLine(line, lux))
				{
					lastLine = now;
					{
						std::lock_guard<std::mutex> guard(mutex);
						++readingSequence;
						readingLux = lux;
					}
					if (!reading)
						SetStatus(portName + " reading");
					reading = true;
				}
				else if (complete)
					lastLine = now;   // a banner or comment: the device is there
				if (!port->IsOpen() ||
					std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLine).count() > timing.silenceReopenMs)
				{
					SetStatus(portName + ": no readings, reopening");
					port->Close();
					break;
				}
			}
			if (WaitStop(timing.retryMs))
				return;
		}
	}
}
