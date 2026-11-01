// VP Renderer filter (2026-10-17): the LED drive arbiter. Two owners of the
// drive mutex - here in one process, with a per-test mutex name injected so the
// tests never meet a running VideoProcessor - exclude each other; a refused
// owner's port is never opened; closing gives the drive back; distinct names
// are independent; with no other owner the wrapper is transparent.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/drive/projector_control/LedDriveArbiter.h>
#include <Windows.h>
#include <atomic>
#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		std::wstring UniqueDriveMutexName()
		{
			static std::atomic<int> counter{ 0 };
			return L"Local\\VideoProcessorLedDriveTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
				std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++counter);
		}

		struct ArbiterFakePortState
		{
			int opens = 0;
			int closes = 0;
			int writes = 0;
			bool open = false;
			bool openFails = false;
		};

		class ArbiterFakePort : public LedDrive::ISerialPort
		{
		public:
			explicit ArbiterFakePort(std::shared_ptr<ArbiterFakePortState> stateIn) : state(std::move(stateIn)) {}
			bool Open(const std::string&, int, std::string& error) override
			{
				++state->opens;
				if (state->openFails)
				{
					error = "COM9 busy";
					return false;
				}
				state->open = true;
				return true;
			}
			void Close() override
			{
				if (state->open)
					++state->closes;
				state->open = false;
			}
			bool IsOpen() const override { return state->open; }
			void DiscardInput() override {}
			bool Write(const std::string&, std::string&) override { ++state->writes; return state->open; }
			bool ReadLine(int, std::string& line) override { line = "%001 LPW1 000120"; return state->open; }

		private:
			std::shared_ptr<ArbiterFakePortState> state;
		};

		std::unique_ptr<LedDrive::ArbitratedSerialPort> MakePort(const std::shared_ptr<ArbiterFakePortState>& state,
			const std::wstring& name)
		{
			return std::unique_ptr<LedDrive::ArbitratedSerialPort>(new LedDrive::ArbitratedSerialPort(
				std::unique_ptr<LedDrive::ISerialPort>(new ArbiterFakePort(state)), name));
		}
	}

	TEST_CLASS(LedDriveArbiterTests)
	{
	public:
		TEST_METHOD(TwoOwnersExcludeEachOther)
		{
			const std::wstring name = UniqueDriveMutexName();
			std::string reason;
			auto first = LedDrive::DriveOwnership::TryAcquire(name, reason);
			Assert::IsTrue(first != nullptr, L"the first owner takes the drive");
			Assert::IsTrue(reason.empty());
			auto second = LedDrive::DriveOwnership::TryAcquire(name, reason);
			Assert::IsTrue(second == nullptr, L"a second owner in the same process is refused");
			Assert::IsTrue(reason.find("another VideoProcessor") != std::string::npos,
				std::wstring(reason.begin(), reason.end()).c_str());
			first.reset();
			second = LedDrive::DriveOwnership::TryAcquire(name, reason);
			Assert::IsTrue(second != nullptr, L"released: the next owner takes it");
		}

		TEST_METHOD(DistinctNamesAreIndependent)
		{
			std::string reason;
			auto a = LedDrive::DriveOwnership::TryAcquire(UniqueDriveMutexName(), reason);
			auto b = LedDrive::DriveOwnership::TryAcquire(UniqueDriveMutexName(), reason);
			Assert::IsTrue(a != nullptr && b != nullptr);
		}

		TEST_METHOD(RefusedOwnerNeverOpensItsPort)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto stateA = std::make_shared<ArbiterFakePortState>();
			auto stateB = std::make_shared<ArbiterFakePortState>();
			auto portA = MakePort(stateA, name);
			auto portB = MakePort(stateB, name);
			std::string error;
			Assert::IsTrue(portA->Open("COM3", 115200, error), L"no other owner: opens");
			Assert::IsTrue(portA->OwnsDrive() && stateA->opens == 1 && portA->IsOpen());
			Assert::IsFalse(portB->Open("COM3", 115200, error), L"second owner refused");
			Assert::AreEqual(0, stateB->opens, L"the refused owner never touched its port");
			Assert::IsFalse(portB->OwnsDrive() || portB->IsOpen());
			Assert::IsTrue(error.find("does not drive COM3") != std::string::npos,
				std::wstring(error.begin(), error.end()).c_str());
			// A second refusal is the same (said once in the log, still refused).
			Assert::IsFalse(portB->Open("COM3", 115200, error));
			portA->Close();
			Assert::IsFalse(portA->OwnsDrive());
			Assert::AreEqual(1, stateA->closes);
			Assert::IsTrue(portB->Open("COM3", 115200, error), L"after the close the other owner may drive");
			Assert::AreEqual(1, stateB->opens);
		}

		TEST_METHOD(TransparentWhileOwned)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto state = std::make_shared<ArbiterFakePortState>();
			auto port = MakePort(state, name);
			std::string error;
			Assert::IsTrue(port->Open("COM3", 9600, error));
			Assert::IsTrue(port->Write(":LPW1 120\r", error));
			std::string line;
			Assert::IsTrue(port->ReadLine(100, line));
			Assert::AreEqual(std::string("%001 LPW1 000120"), line);
			Assert::AreEqual(1, state->writes);
			// A reopen while still owning (after a failed read closed the port) keeps ownership.
			state->open = false;
			Assert::IsTrue(port->Open("COM3", 9600, error));
			Assert::IsTrue(port->OwnsDrive());
			Assert::AreEqual(2, state->opens);
		}

		TEST_METHOD(FailedOpenReleasesTheDrive)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto failing = std::make_shared<ArbiterFakePortState>();
			failing->openFails = true;
			auto portA = MakePort(failing, name);
			std::string error;
			Assert::IsFalse(portA->Open("COM3", 115200, error));
			Assert::AreEqual(std::string("COM9 busy"), error, L"the port's own error is kept");
			Assert::IsFalse(portA->OwnsDrive());
			auto stateB = std::make_shared<ArbiterFakePortState>();
			auto portB = MakePort(stateB, name);
			Assert::IsTrue(portB->Open("COM3", 115200, error), L"a failed open does not keep the drive");
		}

		TEST_METHOD(DestroyingThePortReleasesTheDrive)
		{
			const std::wstring name = UniqueDriveMutexName();
			std::string error;
			{
				auto portA = MakePort(std::make_shared<ArbiterFakePortState>(), name);
				Assert::IsTrue(portA->Open("COM3", 115200, error));
			}
			auto portB = MakePort(std::make_shared<ArbiterFakePortState>(), name);
			Assert::IsTrue(portB->Open("COM3", 115200, error));
		}

		TEST_METHOD(FactoryWrapsEveryPort)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto state = std::make_shared<ArbiterFakePortState>();
			const LedDrive::SerialPortFactory factory = LedDrive::ArbitratedSerialPortFactory(
				[state]() { return std::unique_ptr<LedDrive::ISerialPort>(new ArbiterFakePort(state)); }, name);
			auto first = factory();
			auto second = factory();
			std::string error;
			Assert::IsTrue(first->Open("COM3", 115200, error));
			Assert::IsFalse(second->Open("COM3", 115200, error), L"two drives from the factory exclude each other");
			Assert::AreEqual(1, state->opens);
			Assert::AreEqual(std::wstring(L"Global\\VideoProcessorLedDrive"), std::wstring(LedDrive::kLedDriveMutexName),
				L"the production name is machine-wide");
		}
	};
}
