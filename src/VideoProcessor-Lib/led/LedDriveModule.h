#pragma once

#include "LedController.h"
#include "LedDriveConfig.h"
#include "LedMeasurement.h"
#include "ProjectorDriver.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

// The LED drive module at runtime: owns the projector driver and the
// controller and runs them on its own thread every tick_ms. The renderer
// touches it at exactly two points: SubmitMeasurement() after a frame (stats
// out) and CurrentGain() before the next one (gain in). It never stalls the
// renderer: both calls only take a short lock.
namespace LedDrive
{
	struct CompensationGain
	{
		// False: the compensation hook leaves the picture untouched.
		bool active = false;
		double gain = 1.0;
		GainDomain domain = GainDomain::Encoded;
		double knee = 0.08;
		double eotf = 2.2;
	};

	class LedDriveModule
	{
	public:
		LedDriveModule(const ProjectorConfig& projector, const LedDriveConfig& led,
			SerialPortFactory portFactory = [] { return CreateWin32SerialPort(); });
		~LedDriveModule();
		LedDriveModule(const LedDriveModule&) = delete;
		LedDriveModule& operator=(const LedDriveModule&) = delete;

		void Start();
		// Restores full light (when it was driving the projector) and stops the
		// thread. Called by the destructor.
		void Stop();

		// Render thread, after a frame: the GPU pass's counts for that frame.
		void SubmitMeasurement(const RawMeasurement& raw);
		// Render thread, before a frame.
		CompensationGain CurrentGain() const;

		// Diagnostics and tests.
		bool DrivingProjector() const;
		DriverStatus ProjectorStatus() const;
		int Ticks() const;
		const LedDriveConfig& Config() const { return led; }

	private:
		void Run();
		bool WaitUntil(std::chrono::steady_clock::time_point when);

		ProjectorConfig projector;
		LedDriveConfig led;
		SerialPortFactory portFactory;

		mutable std::mutex mutex;
		std::condition_variable wake;
		std::thread thread;
		bool stopping = false;
		bool started = false;

		StatsBuilder statsBuilder;
		bool haveStats = false;
		FrameStats latest;
		bool pendingCut = false;
		bool pendingFade = false;

		std::unique_ptr<IProjectorDriver> driver;
		bool live = false;
		CompensationGain gain;
		int ticks = 0;
	};
}
