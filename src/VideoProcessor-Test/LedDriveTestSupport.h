#pragma once

// Test doubles shared by the LED drive tests: a stand-in FL35 behind a fake
// serial port.

#include <led/SerialPort.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	// A stand-in FL35 behind a fake serial port. It answers ":MNEM..." commands
	// framed with acceptedSuffix with "%001 MNEM 000nnn" replies.
	struct FakeProjector
	{
		std::mutex mutex;
		std::condition_variable changed;
		std::vector<std::string> writes;
		std::vector<std::chrono::steady_clock::time_point> writeTimes;
		std::deque<std::string> replies;
		std::string acceptedSuffix = "\r";
		bool silent = false;
		bool holdReplies = false;
		bool openFails = false;
		int opens = 0;
		int light = 120;
		int contrast = 74;
		int minimum = 60;
		int maximum = 255;

		static std::string Reply(const char* mnemonic, int value)
		{
			char text[32];
			sprintf_s(text, "%%001 %s %06d", mnemonic, value);
			return text;
		}

		void Receive(const std::string& bytes)
		{
			std::lock_guard<std::mutex> guard(mutex);
			writes.push_back(bytes);
			writeTimes.push_back(std::chrono::steady_clock::now());
			changed.notify_all();
			if (silent || bytes.size() < acceptedSuffix.size() ||
				bytes.compare(bytes.size() - acceptedSuffix.size(), acceptedSuffix.size(), acceptedSuffix) != 0)
				return;
			const std::string body = bytes.substr(0, bytes.size() - acceptedSuffix.size());
			// "\r" framing must not also accept the literal "cr" variant.
			if (acceptedSuffix == "\r" && body.size() >= 2 && body.compare(body.size() - 2, 2, "cr") == 0)
				return;
			if (body == ":ECHO1") replies.push_back(Reply("ECHO", 1));
			else if (body == ":LPW1?") replies.push_back(Reply("LPW1", light));
			else if (body == ":LPW1 ?N") replies.push_back(Reply("LPW1", minimum));
			else if (body == ":LPW1 ?M") replies.push_back(Reply("LPW1", maximum));
			else if (body == ":CNTR?") replies.push_back(Reply("CNTR", contrast));
			else if (body.rfind(":LPW1", 0) == 0) { light = std::stoi(body.substr(5)); replies.push_back(Reply("LPW1", light)); }
			else if (body.rfind(":CNTR", 0) == 0) { contrast = std::stoi(body.substr(5)); replies.push_back(Reply("CNTR", contrast)); }
			// Any other ":MNEMvalue" (LDMM, LDCR, ...) is acknowledged with its value.
			else if (body.size() > 5 && body[0] == ':' && body.find('?') == std::string::npos &&
				std::isdigit(static_cast<unsigned char>(body[5])))
				replies.push_back(Reply(body.substr(1, 4).c_str(), std::stoi(body.substr(5))));
			changed.notify_all();
		}

		std::vector<std::string> Writes()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return writes;
		}

		std::vector<std::chrono::steady_clock::time_point> WriteTimes()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return writeTimes;
		}

		bool WaitForWrites(size_t count, int timeoutMs = 2000)
		{
			std::unique_lock<std::mutex> lock(mutex);
			return changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return writes.size() >= count; });
		}

		void Release()
		{
			std::lock_guard<std::mutex> guard(mutex);
			holdReplies = false;
			changed.notify_all();
		}
	};

	class FakePort : public LedDrive::ISerialPort
	{
	public:
		explicit FakePort(std::shared_ptr<FakeProjector> projectorIn) : projector(std::move(projectorIn)) {}
		bool Open(const std::string&, int, std::string& error) override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			++projector->opens;
			if (projector->openFails) { error = "cannot open COM9"; return false; }
			open = true;
			return true;
		}
		void Close() override { open = false; }
		bool IsOpen() const override { return open; }
		void DiscardInput() override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			projector->replies.clear();
		}
		bool Write(const std::string& bytes, std::string&) override
		{
			projector->Receive(bytes);
			return true;
		}
		bool ReadLine(int timeoutMs, std::string& line) override
		{
			std::unique_lock<std::mutex> lock(projector->mutex);
			if (!projector->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return !projector->holdReplies && !projector->replies.empty(); }))
				return false;
			line = projector->replies.front();
			projector->replies.pop_front();
			return true;
		}

	private:
		std::shared_ptr<FakeProjector> projector;
		bool open = false;
	};

}
