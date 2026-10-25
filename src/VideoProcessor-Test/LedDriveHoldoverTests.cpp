// VP Renderer filter (2026-10-17): the LED drive hold-over between graphs,
// with a fake clock and a fake drive. A parked drive is adopted by the next
// renderer with the same LED settings inside file_led_holdover_ms; otherwise it
// is stopped (restoring full light) at the deadline, at once when the settings
// differ, when another drive is parked, on EndNow and when the holder ends.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/drive/led_compensation/LedDriveHoldover.h>
#include <Windows.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		struct HoldoverFakeDrive
		{
			int id = 0;
			int stops = 0;
		};

		struct HoldoverFixture
		{
			uint64_t now = 1000;
			std::vector<int> stopped;
			LedDrive::Holdover<HoldoverFakeDrive> holder;

			HoldoverFixture()
				: holder([this] { return now; },
					[this](const std::shared_ptr<HoldoverFakeDrive>& drive)
					{
						++drive->stops;
						stopped.push_back(drive->id);
					},
					false)
			{
			}

			static std::shared_ptr<HoldoverFakeDrive> Drive(int id)
			{
				auto drive = std::make_shared<HoldoverFakeDrive>();
				drive->id = id;
				return drive;
			}
		};
	}

	TEST_CLASS(LedDriveHoldoverTests)
	{
	public:
		TEST_METHOD(NextRendererAdoptsWithinTheHoldover)
		{
			HoldoverFixture f;
			auto drive = HoldoverFixture::Drive(1);
			f.holder.Park(drive, "led=a", 3000);
			Assert::IsTrue(f.holder.Parked());
			f.now += 2999;
			Assert::IsFalse(f.holder.Poll(), L"not yet expired");
			auto adopted = f.holder.Adopt("led=a");
			Assert::IsTrue(adopted == drive, L"the same drive carries on");
			Assert::AreEqual(0, drive->stops, L"never stopped: no return to full light");
			Assert::IsFalse(f.holder.Parked());
			f.now += 100000;
			Assert::IsFalse(f.holder.Poll(), L"an adopted drive is not stopped later");
			Assert::AreEqual(0, drive->stops);
		}

		TEST_METHOD(NoNewRendererStopsAtTheDeadline)
		{
			HoldoverFixture f;
			auto drive = HoldoverFixture::Drive(2);
			f.holder.Park(drive, "led=a", 3000);
			f.now += 2999;
			Assert::IsFalse(f.holder.Poll());
			Assert::AreEqual(0, drive->stops);
			f.now += 1;
			Assert::IsTrue(f.holder.Poll(), L"stopped exactly at the deadline");
			Assert::AreEqual(1, drive->stops, L"stopped once (full light restored)");
			Assert::IsFalse(f.holder.Parked());
			Assert::IsTrue(f.holder.Adopt("led=a") == nullptr, L"nothing left to adopt");
			Assert::AreEqual(1, drive->stops);
		}

		TEST_METHOD(DifferentSettingsStopTheHeldDrive)
		{
			HoldoverFixture f;
			auto drive = HoldoverFixture::Drive(3);
			f.holder.Park(drive, "led=a", 3000);
			f.now += 10;
			Assert::IsTrue(f.holder.Adopt("led=b") == nullptr, L"a drive with other settings is not adopted");
			Assert::AreEqual(1, drive->stops, L"and is stopped at once, freeing the port for the new one");
			Assert::IsFalse(f.holder.Parked());
		}

		TEST_METHOD(LateAdoptionAfterTheDeadlineStops)
		{
			HoldoverFixture f;
			auto drive = HoldoverFixture::Drive(4);
			f.holder.Park(drive, "led=a", 3000);
			f.now += 3500;   // the thread has not run (tests): the adoption sees the expiry
			Assert::IsTrue(f.holder.Adopt("led=a") == nullptr);
			Assert::AreEqual(1, drive->stops);
		}

		TEST_METHOD(ZeroHoldoverStopsAtOnce)
		{
			HoldoverFixture f;
			auto drive = HoldoverFixture::Drive(5);
			f.holder.Park(drive, "led=a", 0);
			Assert::AreEqual(1, drive->stops);
			Assert::IsFalse(f.holder.Parked());
		}

		TEST_METHOD(ParkingAnotherStopsThePrevious)
		{
			HoldoverFixture f;
			auto first = HoldoverFixture::Drive(6);
			auto second = HoldoverFixture::Drive(7);
			f.holder.Park(first, "led=a", 3000);
			f.now += 500;
			f.holder.Park(second, "led=a", 3000);
			Assert::AreEqual(1, first->stops, L"only one drive is ever held");
			Assert::AreEqual(0, second->stops);
			f.now += 2999;   // the second's deadline counts from its own parking
			Assert::IsFalse(f.holder.Poll());
			Assert::IsTrue(f.holder.Adopt("led=a") == second);
		}

		TEST_METHOD(EndNowAndHolderEndStopTheDrive)
		{
			auto drive = HoldoverFixture::Drive(8);
			{
				HoldoverFixture f;
				f.holder.Park(drive, "led=a", 3000);
				Assert::IsTrue(f.holder.EndNow("test"));
				Assert::AreEqual(1, drive->stops);
				Assert::IsFalse(f.holder.EndNow("test"), L"nothing held any more");
				auto other = HoldoverFixture::Drive(9);
				f.holder.Park(other, "led=a", 3000);
				drive = other;
			}
			Assert::AreEqual(1, drive->stops, L"the holder's end stops a held drive");
		}

		TEST_METHOD(NullDriveIsIgnored)
		{
			HoldoverFixture f;
			f.holder.Park(std::shared_ptr<HoldoverFakeDrive>(), "led=a", 3000);
			Assert::IsFalse(f.holder.Parked());
			Assert::IsTrue(f.stopped.empty());
		}

		TEST_METHOD(SlotKeepsItsDeadline)
		{
			LedDrive::HoldoverSlot<HoldoverFakeDrive> slot;
			std::shared_ptr<HoldoverFakeDrive> stale;
			Assert::IsTrue(slot.Adopt("x", 0, stale) == nullptr && stale == nullptr);
			auto drive = HoldoverFixture::Drive(10);
			Assert::IsTrue(slot.Park(drive, "x", 100, 250) == nullptr);
			Assert::AreEqual(static_cast<uint64_t>(350), slot.DeadlineMs());
			Assert::IsTrue(slot.Expire(349) == nullptr);
			Assert::IsTrue(slot.Expire(350) == drive);
			Assert::IsFalse(slot.Parked());
		}

		TEST_METHOD(ThreadStopsAnUnadoptedDrive)
		{
			// The real thread with a real clock and a short hold: the drive is stopped by itself.
			std::mutex mutex;
			std::condition_variable changed;
			int stops = 0;
			{
				LedDrive::Holdover<HoldoverFakeDrive> holder(
					[] { return static_cast<uint64_t>(GetTickCount64()); },
					[&](const std::shared_ptr<HoldoverFakeDrive>&)
					{
						std::lock_guard<std::mutex> guard(mutex);
						++stops;
						changed.notify_all();
					});
				holder.Park(HoldoverFixture::Drive(11), "led=a", 50);
				std::unique_lock<std::mutex> guard(mutex);
				Assert::IsTrue(changed.wait_for(guard, std::chrono::seconds(5), [&] { return stops > 0; }),
					L"the hold-over thread stopped the drive");
			}
			Assert::AreEqual(1, stops, L"stopped exactly once");
		}
	};
}
