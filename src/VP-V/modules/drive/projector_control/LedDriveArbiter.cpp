#include <pch.h>
#include "LedDriveArbiter.h"

#include <DebugLog.h>

#include <Windows.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace LedDrive
{
	namespace
	{
		std::string Narrow(const std::wstring& text)
		{
			std::string result;
			result.reserve(text.size());
			for (wchar_t c : text)
				result.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
			return result;
		}
	}

	// Takes the mutex on its own thread and keeps it until released.
	struct DriveOwnership::Holder
	{
		enum class State { Pending, Held, Abandoned, Refused };

		std::mutex mutex;
		std::condition_variable changed;
		State state = State::Pending;
		bool release = false;
		std::string reason;
		std::thread thread;

		void Run(std::wstring name)
		{
			HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
			if (!handle)
			{
				const DWORD error = GetLastError();
				Finish(State::Refused, error == ERROR_ACCESS_DENIED ?
					"the LED drive is held by another owner (" + Narrow(name) + ": access denied)" :
					"cannot create " + Narrow(name) + " (error " + std::to_string(error) + ")");
				return;
			}
			const DWORD wait = WaitForSingleObject(handle, 0);
			if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
			{
				CloseHandle(handle);
				Finish(State::Refused, wait == WAIT_TIMEOUT ?
					"another VideoProcessor (VideoProcessor.exe or the VP Renderer filter) is driving the projector (" +
						Narrow(name) + " is held)" :
					"cannot take " + Narrow(name) + " (error " + std::to_string(GetLastError()) + ")");
				return;
			}
			Finish(wait == WAIT_ABANDONED ? State::Abandoned : State::Held, std::string());
			{
				std::unique_lock<std::mutex> guard(mutex);
				changed.wait(guard, [this] { return release; });
			}
			ReleaseMutex(handle);
			CloseHandle(handle);
		}

		void Finish(State result, const std::string& why)
		{
			std::lock_guard<std::mutex> guard(mutex);
			state = result;
			reason = why;
			changed.notify_all();
		}

		~Holder()
		{
			{
				std::lock_guard<std::mutex> guard(mutex);
				release = true;
				changed.notify_all();
			}
			if (thread.joinable())
				thread.join();
		}
	};

	std::unique_ptr<DriveOwnership> DriveOwnership::TryAcquire(const std::wstring& name, std::string& reason)
	{
		reason.clear();
		std::unique_ptr<Holder> holder(new Holder());
		Holder* raw = holder.get();
		try
		{
			holder->thread = std::thread([raw, name] { raw->Run(name); });
		}
		catch (const std::exception& e)
		{
			reason = std::string("cannot start the LED drive ownership thread: ") + e.what();
			return nullptr;
		}
		Holder::State state = Holder::State::Pending;
		{
			std::unique_lock<std::mutex> guard(holder->mutex);
			holder->changed.wait(guard, [raw] { return raw->state != Holder::State::Pending; });
			state = holder->state;
			reason = holder->reason;
		}
		if (state == Holder::State::Refused)
			return nullptr;   // the thread has ended; ~Holder joins it
		const bool abandoned = state == Holder::State::Abandoned;
		if (abandoned)
			DebugLog::Log("LED drive: %s was abandoned by an owner that ended without releasing it; taking it over",
				Narrow(name).c_str());
		return std::unique_ptr<DriveOwnership>(new DriveOwnership(std::move(holder), abandoned));
	}

	DriveOwnership::DriveOwnership(std::unique_ptr<Holder> holderIn, bool abandonedIn)
		: holder(std::move(holderIn)), abandoned(abandonedIn)
	{
	}

	DriveOwnership::~DriveOwnership() = default;


	ArbitratedSerialPort::ArbitratedSerialPort(std::unique_ptr<ISerialPort> innerIn, const std::wstring& name)
		: inner(std::move(innerIn)), mutexName(name)
	{
	}

	ArbitratedSerialPort::~ArbitratedSerialPort()
	{
		Close();
	}

	bool ArbitratedSerialPort::Open(const std::string& port, int baud, std::string& error)
	{
		if (!inner)
		{
			error = "no serial port";
			return false;
		}
		if (!ownership)
		{
			std::string reason;
			ownership = DriveOwnership::TryAcquire(mutexName, reason);
			if (!ownership)
			{
				error = reason + "; this one does not drive " + port;
				if (reason != lastRefusal)
					DebugLog::Log("LED drive: not driving %s: %s", port.c_str(), reason.c_str());
				lastRefusal = reason;
				return false;
			}
			lastRefusal.clear();
		}
		if (!inner->Open(port, baud, error))
		{
			ownership.reset();
			return false;
		}
		return true;
	}

	void ArbitratedSerialPort::Close()
	{
		if (inner)
			inner->Close();
		ownership.reset();
	}

	bool ArbitratedSerialPort::IsOpen() const
	{
		return inner && inner->IsOpen();
	}

	void ArbitratedSerialPort::DiscardInput()
	{
		if (inner)
			inner->DiscardInput();
	}

	bool ArbitratedSerialPort::Write(const std::string& bytes, std::string& error)
	{
		if (!inner)
		{
			error = "no serial port";
			return false;
		}
		return inner->Write(bytes, error);
	}

	bool ArbitratedSerialPort::ReadLine(int timeoutMs, std::string& line)
	{
		if (!inner)
		{
			line.clear();
			return false;
		}
		return inner->ReadLine(timeoutMs, line);
	}

	SerialPortFactory ArbitratedSerialPortFactory(SerialPortFactory innerFactory, const std::wstring& mutexName)
	{
		return [innerFactory, mutexName]() -> std::unique_ptr<ISerialPort>
		{
			std::unique_ptr<ISerialPort> port = innerFactory ? innerFactory() : CreateWin32SerialPort();
			return std::unique_ptr<ISerialPort>(new ArbitratedSerialPort(std::move(port), mutexName));
		};
	}
}
