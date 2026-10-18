// VP Renderer filter (2026-10-17): the LED drive arbiter. Two owners of the
// drive mutex - here in one process, with a per-test mutex name injected so the
// tests never meet a running VideoProcessor - exclude each other; a refused
// owner's port is never opened; closing gives the drive back; distinct names
// are independent; with no other owner the wrapper is transparent.
#include "pch.h"
#include "CppUnitTest.h"
#include <led/LedDriveArbiter.h>
#include <led/LedDriveConfig.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>
#include "LedDriveTestSupport.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

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

	// Bug hunt 3 (C3, led_handover): the arbiter never handed over, so the side
	// that lost stayed a dry run and VideoProcessor.exe drove LPW1 / CNTR from
	// its capture picture while MPC played a file. Two LED drive modules - "VP"
	// (VideoProcessor.exe) and "MPC" (the filter) - share one stand-in FL35
	// behind the arbiter, each with its own port so every write is attributed.
	using namespace LedDrive;

	namespace
	{
		RawMeasurement HandoverFrame(double code)
		{
			RawMeasurement raw{};
			const uint32_t blocks = 10000;
			raw.histogram[static_cast<int>(code)] += blocks;
			raw.blocks = blocks;
			raw.sum = static_cast<uint32_t>(code * blocks * SUM_SCALE);
			for (int i = 0; i < GRID_W * GRID_H; ++i)
			{
				raw.cellCount[i] = 4;
				raw.cellSum[i] = static_cast<uint32_t>(4 * code * SUM_SCALE);
			}
			return raw;
		}

		bool HandoverWaitFor(const std::function<bool()>& condition, int timeoutMs)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (condition()) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return condition();
		}

		// A FakePort that counts its own writes.
		class OwnerPort : public FakePort
		{
		public:
			OwnerPort(std::shared_ptr<FakeProjector> projector, std::shared_ptr<std::atomic<int>> countIn)
				: FakePort(std::move(projector)), count(std::move(countIn)) {}
			bool Write(const std::string& bytes, std::string& error) override
			{
				++*count;
				return FakePort::Write(bytes, error);
			}
		private:
			std::shared_ptr<std::atomic<int>> count;
		};

		LedDrive::ProjectorConfig HandoverPort()
		{
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
			port.ledLatencyMs = 50;
			port.ledSettleMs = 0;
			return port;
		}

		// internal_script with the light held at the ceiling: only CNTR moves
		// (a mid-grey picture asks for CNTR 51).
		LedDrive::LedDriveConfig HandoverScript(bool handover = true)
		{
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 100;
			config.handover = handover;
			LedDrive::ParseCurve("0:255, 255:255", config.curve);
			return config;
		}

		// One side of the handover: a module, its port's write count and a
		// player thread feeding it frames while it "plays".
		struct Side
		{
			std::shared_ptr<std::atomic<int>> writes = std::make_shared<std::atomic<int>>(0);
			std::unique_ptr<LedDrive::LedDriveModule> module;
			std::thread player;
			std::atomic<bool> playing{ false };
			std::atomic<bool> quit{ false };
			std::atomic<int> code{ 50 };

			Side(const std::shared_ptr<FakeProjector>& projector, const std::wstring& name, bool handover = true)
			{
				auto count = writes;
				module.reset(new LedDrive::LedDriveModule(HandoverPort(), HandoverScript(handover),
					LedDrive::ArbitratedSerialPortFactory([projector, count]()
					{
						return std::unique_ptr<LedDrive::ISerialPort>(new OwnerPort(projector, count));
					}, name)));
				module->SetHandover(std::make_shared<LedDrive::DriveHandoverChannel>(name));
				player = std::thread([this]()
				{
					uint64_t sequence = 1;
					while (!quit)
					{
						if (playing)
						{
							module->BeginFrame(sequence);
							module->SubmitMeasurement(HandoverFrame(code), sequence);
							++sequence;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(16));
					}
				});
			}
			~Side()
			{
				quit = true;
				if (player.joinable())
					player.join();
				module->Stop();
			}
			void Play() { playing = true; }
			void Pause() { playing = false; }
			bool Driving() const { return module->DrivingProjector(); }
		};

		int Contrast(FakeProjector& projector)
		{
			std::lock_guard<std::mutex> guard(projector.mutex);
			return projector.contrast;
		}
		int Light(FakeProjector& projector)
		{
			std::lock_guard<std::mutex> guard(projector.mutex);
			return projector.light;
		}
	}

	TEST_CLASS(LedDriveHandoverTests)
	{
	public:
		TEST_METHOD(HandoverChannelRequestsAreTakenOnceAndProbeTheDrive)
		{
			const std::wstring name = UniqueDriveMutexName();
			LedDrive::DriveHandoverChannel a(name), b(name);
			Assert::IsTrue(a.Usable() && b.Usable());
			Assert::IsTrue(a.DriveFree(), L"nobody holds it");
			Assert::IsFalse(a.TakeRequest());
			b.Request();
			Assert::IsTrue(a.TakeRequest(), L"the holder sees the request");
			Assert::IsFalse(a.TakeRequest(), L"once");
			b.Request();
			a.ClearRequests();
			Assert::IsFalse(a.TakeRequest(), L"a new holder drops older requests");
			std::string reason;
			auto held = LedDrive::DriveOwnership::TryAcquire(name, reason);
			Assert::IsTrue(held != nullptr);
			Assert::IsFalse(a.DriveFree(), L"held by an owner on another thread");
			held->SimulateCrashForTest();
			Assert::IsTrue(a.DriveFree(), L"an owner that crashed leaves it free");
			auto refused = MakePort(std::make_shared<ArbiterFakePortState>(), name);
			auto holder = MakePort(std::make_shared<ArbiterFakePortState>(), name);
			std::string error;
			Assert::IsTrue(holder->Open("COM3", 115200, error));
			Assert::IsFalse(refused->Open("COM3", 115200, error));
			Assert::IsTrue(LedDrive::IsDriveHeldError(error), L"a refusal is marked as held by another owner");
		}

		// VP plays and drives (CNTR raised above 50); MPC starts playing: VP puts
		// CNTR back to 50 and full light, lets go and stays silent; MPC drives.
		// MPC stops: VP, still playing, takes the projector back from full light.
		TEST_METHOD(VpHandsOverToMpcAndTakesTheProjectorBack)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto projector = std::make_shared<FakeProjector>();
			Side vp(projector, name);
			vp.module->Start();
			vp.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving() && Contrast(*projector) == 51; }, 5000),
				L"VP drives and its script raised CNTR to 51");

			{
				Side mpc(projector, name);
				mpc.code = 50;
				mpc.module->Start();
				mpc.Play();
				Assert::IsTrue(HandoverWaitFor([&] { return mpc.Driving(); }, 6000), L"MPC got the projector");
				Assert::IsFalse(vp.Driving(), L"VP stopped driving");
				Assert::IsTrue(vp.module->HandoverStandby());
				Assert::AreEqual(1, vp.module->HandoversGiven());
				Assert::AreEqual(255, Light(*projector), L"full light after the handover");
				// VP never writes while MPC owns the projector, though it still plays.
				const int vpWrites = *vp.writes;
				std::this_thread::sleep_for(std::chrono::milliseconds(1200));
				Assert::AreEqual(vpWrites, vp.writes->load(), L"VP stayed silent while MPC had the projector");
				Assert::IsTrue(*mpc.writes > 0, L"MPC drives the port");
			}
			// MPC restored full light and CNTR 50 on its way out; VP takes it back.
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving(); }, 6000), L"VP took the projector back");
			Assert::AreEqual(1, vp.module->HandoversTaken());
			Assert::IsFalse(vp.module->HandoverStandby());
			Assert::AreEqual(255, Light(*projector));
			Assert::IsTrue(HandoverWaitFor([&] { return Contrast(*projector) == 51; }, 5000),
				L"VP drives again from its own picture");
		}

		// The other order: MPC already drives when VP starts playing.
		TEST_METHOD(MpcHandsOverWhenVpStartsPlaying)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto projector = std::make_shared<FakeProjector>();
			Side mpc(projector, name);
			mpc.module->Start();
			mpc.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return mpc.Driving() && Contrast(*projector) == 51; }, 5000));
			Side vp(projector, name);
			vp.module->Start();
			// Not playing yet: VP waits as a dry run and asks for nothing.
			Assert::IsTrue(HandoverWaitFor([&] { return vp.module->HandoverStandby(); }, 3000));
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
			Assert::IsTrue(mpc.Driving(), L"a side that is not playing never takes the projector");
			Assert::AreEqual(0, vp.writes->load(), L"VP never touched the port");
			vp.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving(); }, 6000), L"VP got the projector");
			Assert::IsFalse(mpc.Driving());
			Assert::AreEqual(1, mpc.module->HandoversGiven());
		}

		// The holder crashes (an old owner without handover, killed): the side
		// that waits while playing takes the abandoned drive and brings the
		// projector to full light and CNTR 50 first.
		TEST_METHOD(AHolderThatCrashesLeavesTheProjectorToThePlayer)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto projector = std::make_shared<FakeProjector>();
			auto crashed = MakePort(std::make_shared<ArbiterFakePortState>(), name);
			std::string error;
			Assert::IsTrue(crashed->Open("COM3", 115200, error));
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->contrast = 74;   // left above the need by the crashed owner
				projector->light = 120;
			}
			Side vp(projector, name);
			vp.module->Start();
			vp.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.module->HandoverStandby(); }, 3000));
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			Assert::IsFalse(vp.Driving(), L"the holder never answers (it has no handover)");
			crashed->SimulateCrashForTest();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving(); }, 4000), L"the abandoned drive is taken");
			Assert::AreEqual(255, Light(*projector));
			Assert::IsTrue(HandoverWaitFor([&] { return Contrast(*projector) == 50 || Contrast(*projector) == 51; }, 2000),
				L"CNTR no longer left at 74");
		}

		// A side asks and crashes before it takes the projector: the holder hands
		// over (CNTR 50, full light), then takes it back after the grace period.
		TEST_METHOD(ARequesterThatCrashesGetsTheProjectorReturned)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto projector = std::make_shared<FakeProjector>();
			Side vp(projector, name);
			vp.module->Start();
			vp.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving() && Contrast(*projector) == 51; }, 5000));
			{
				LedDrive::DriveHandoverChannel ghost(name);
				ghost.Request();
			}
			Assert::IsTrue(HandoverWaitFor([&] { return !vp.Driving(); }, 3000), L"VP handed over");
			Assert::AreEqual(50, Contrast(*projector), L"CNTR back to 50 at the handover");
			Assert::AreEqual(255, Light(*projector));
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving(); }, 6000), L"nobody took it: VP takes it back");
			Assert::AreEqual(1, vp.module->HandoversTaken());
		}

		// led_handover off: the old behaviour - the second side stays a dry run.
		TEST_METHOD(HandoverOffKeepsTheFirstOwner)
		{
			const std::wstring name = UniqueDriveMutexName();
			auto projector = std::make_shared<FakeProjector>();
			Side vp(projector, name, false);
			vp.module->Start();
			vp.Play();
			Assert::IsTrue(HandoverWaitFor([&] { return vp.Driving(); }, 5000));
			Side mpc(projector, name, false);
			mpc.module->Start();
			mpc.Play();
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			Assert::IsTrue(vp.Driving());
			Assert::IsFalse(mpc.Driving());
			Assert::IsFalse(mpc.module->HandoverStandby());
			Assert::AreEqual(0, vp.module->HandoversGiven());
		}
	};
}
