#pragma once

// Automatic calibration phase 2: the Dynamic Drive (LED) characterisation run
// on a projector with an RS-232 LED drive (the Barco F35 / FL35; design "FL35
// LED measurements"). With VP's own patches on the screen and VP's Dynamic
// Drive stopped (the renderer stops it while patterns show, so the port is
// free), the run opens its own connection to the projector with the LED
// module's driver and measures:
//
//  1. a short greyscale at full light -> compensation_eotf (the display's
//     effective gamma, which the compensation gain is encoded with);
//  2. white (and black at every fourth level) against LPW1, swept down from
//     full light to the lowest level VP drives and back up (hysteresis) ->
//     the flux table (drive -> relative light) and the black / contrast
//     against drive;
//  3. code 235 (white), the highest legal code (WTW) and black against CNTR,
//     from the CNTR default up to the sweep maximum -> dmd_clip_cntr (where
//     white stops gaining) and cntr_max_usable (where any code stops gaining);
//  4. with a fast light sensor only (led_sensor_port; a colorimeter is far
//     too slow): LPW1, CNTR and picture steps -> led_latency_ms /
//     led_settle_ms, cntr_latency_ms / cntr_settle_ms and how much later the
//     LED lands than the picture.
//
// Safety first:
//  - every LPW1 and CNTR value is clamped to the limits (the projector's /
//    profile's light range, never below the lowest level VP drives, CNTR from
//    the default 50 to the sweep maximum, never above the profile maximum);
//  - full light (the ceiling), CNTR 50 and the standard dim mode (LDMM 0) are
//    restored at the end, on cancel, on any failure, on a renderer restart,
//    on VP closing and in the destructor - always before the patterns end, so
//    VP's own Dynamic Drive gets a projector in its normal state;
//  - a marker file (luts\calibration\led-run-active.txt) exists only while
//    the run drives the projector; VP's next start restores full light and
//    CNTR 50 when it finds one (a crash mid-run).
//
// Results: led_characterisation.cfg (the [projector.default] /
// [led_drive.default] keys Apply writes), projector_profile_measured.txt (the
// same values as .vpprojector sections, for a device profile), led.json (the
// fits and every sample), raw.jsonl and report.html in the run folder.

#include "CalibrationRunBase.h"
#include "LedCharacterize.h"
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/projector_control/ProjectorDriver.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Calibration
{
	// ------------------------------------------------------------ projector

	class IProjectorControl
	{
	public:
		virtual ~IProjectorControl() = default;
		virtual bool Open(std::string& error) = 0;
		// Synchronous; sentAt = when the command was written (the session
		// clock's seconds), for the step-response fits.
		virtual bool SetLight(int level, double& sentAt) = 0;
		virtual bool SetContrast(int value, double& sentAt) = 0;
		// Full light (the ceiling), CNTR 50, standard dim mode; bounded.
		// True when every command went out (confirmed with read-back on).
		virtual bool Restore(int ceiling, std::string& report) = 0;
		virtual void Close() = 0;
		virtual bool IsOpen() const = 0;
		// The projector's own light range when it reports one (read-back on).
		virtual LedDrive::LightRange ReportedRange() { return LedDrive::LightRange(); }
	};

	// The LED module's own driver on a connection opened for the run (as the
	// RS-232 connection test). sentAt comes from the driver's write time on
	// the steady clock (the session's system clock uses the same epoch).
	class DriverProjectorControl final : public IProjectorControl
	{
	public:
		DriverProjectorControl(const LedDrive::ProjectorConfig& projector, LedDrive::SerialPortFactory portFactory);
		~DriverProjectorControl() override;
		bool Open(std::string& error) override;
		bool SetLight(int level, double& sentAt) override;
		bool SetContrast(int value, double& sentAt) override;
		bool Restore(int ceiling, std::string& report) override;
		void Close() override;
		bool IsOpen() const override { return driver != nullptr; }
		LedDrive::LightRange ReportedRange() override;

	private:
		LedDrive::ProjectorConfig projector;
		LedDrive::SerialPortFactory portFactory;
		std::unique_ptr<LedDrive::IProjectorDriver> driver;
	};

	// ------------------------------------------------------- fast sensor

	// A light sensor fast enough for the LED's response (a photodiode or a
	// VEML7700 in a fast mode on a microcontroller): Begin() starts
	// buffering, Take() returns what arrived since, time-stamped on the
	// session clock.
	class ITimedLightSensor
	{
	public:
		virtual ~ITimedLightSensor() = default;
		virtual bool Open(std::string& error) = 0;
		virtual void Begin() = 0;
		virtual std::vector<TimedSample> Take() = 0;
		virtual void Close() = 0;
	};

	// One reading per line: "<value>", or "<device microseconds> <value>"
	// (also "t=<us> v=<value>"). Device times are aligned to the host by the
	// least-delayed arrival; plain values are stamped on arrival.
	bool ParseTimedSensorLine(const std::string& line, bool& haveDeviceTime, double& deviceSeconds, double& value);
	// Device times -> the host clock: shifts by the smallest (arrival - device).
	void AlignDeviceTimes(std::vector<TimedSample>& samples, const std::vector<double>& arrivals);

	class SerialTimedLightSensor final : public ITimedLightSensor
	{
	public:
		using Clock = std::function<double()>;
		SerialTimedLightSensor(const std::string& port, int baud, LedDrive::SerialPortFactory factory, Clock clock);
		~SerialTimedLightSensor() override;
		bool Open(std::string& error) override;
		void Begin() override;
		std::vector<TimedSample> Take() override;
		void Close() override;

	private:
		void Run();
		std::string portName;
		int baud;
		LedDrive::SerialPortFactory factory;
		Clock clock;
		std::unique_ptr<LedDrive::ISerialPort> port;
		std::thread worker;
		std::mutex mutex;
		bool stopping = false;
		bool buffering = false;
		std::vector<TimedSample> samples;
		std::vector<double> arrivals;
		bool deviceTimes = false;
	};

	// ------------------------------------------------------------ limits

	struct LedLimits
	{
		int lightMin = 0;          // the device's LPW1 range
		int lightMax = 255;
		int sweepLow = 60;         // the lowest level swept (VP's lowest drive, led_floor_black)
		int ceiling = 255;         // full light (led_ceiling)
		int contrastNeutral = 50;  // VP's / the projector's normal contrast
		int contrastMax = 74;      // the highest CNTR swept
		int contrastDeviceMax = 100;
		int ClampLight(int level) const;
		int ClampContrast(int value) const;
	};

	// From the projector settings ([projector.default]: light_min / light_max,
	// the projector's own range when it reports one), the LED drive
	// ([led_drive.default] led_floor_black, led_ceiling) and the run's
	// led_cntr_sweep_max. With device profiles (feat/dd-projector-profiles)
	// the same fields come from the profile's [light] min / max / floor_black
	// and [contrast] min / max.
	LedLimits ResolveLedLimits(const LedDrive::ProjectorConfig& projector, const LedDrive::LedDriveConfig& led,
		const CalibrationSettings& settings, const LedDrive::LightRange& reported);

	// ----------------------------------------------------------- the plan

	enum class LedPhase { Greyscale, FluxDown, FluxUp, Cntr, Steps, Done };

	struct LedStepKind
	{
		enum Kind { LightDown, LightUp, ContrastUp, ContrastDown, PictureUp, PictureDown } kind = LightDown;
		int from = 0, to = 0;
	};

	struct LedPlan
	{
		std::vector<double> greys;          // signal, from white down (black last)
		std::vector<int> fluxDown;          // ceiling -> sweepLow
		std::vector<int> fluxUp;            // sweepLow -> ceiling
		std::vector<int> cntr;              // neutral -> contrastMax
		std::vector<LedStepKind> steps;     // empty without a fast sensor
		int Units() const;
	};

	LedPlan BuildLedPlan(const LedLimits& limits, const CalibrationSettings& settings, bool haveSensor);
	// Black is measured at every fourth level of the down sweep and at both ends.
	bool LedBlackLevel(const LedPlan& plan, size_t downIndex);
	// Estimated seconds for the plan (dwell + readings; projector timings).
	double EstimateLedSeconds(const LedPlan& plan, const CalibrationSettings& settings);

	// --------------------------------------------------------- the results

	struct LedResults
	{
		FluxFit flux;
		CntrFit cntr;
		std::vector<std::pair<int, double>> blackByLight;   // lpw, cd/m2
		std::vector<std::pair<int, double>> blackByContrast;
		std::vector<std::pair<double, double>> greys;       // signal, Y
		double compensationEotf = 0.0;                      // 0 = not measured
		std::vector<StepResponseFit> lightSteps, contrastSteps, pictureSteps;
		LatencySummary led, contrast;
		double ledAfterPictureSeconds = 0.0;
		bool havePictureOffset = false;
		bool restoreConfirmed = false;
		std::string restoreReport;
		std::vector<std::string> notes;
	};

	// Effective gamma from a greyscale (signal, Y), 10-90 %, above black.
	double FitGreyscaleGamma(const std::vector<std::pair<double, double>>& greys);
	// The [projector.default] / [led_drive.default] edits for what was measured.
	std::vector<ConfigEdit> LedResultEdits(const LedResults& results);
	// The same values as .vpprojector sections ([light] curve, [contrast]
	// max_usable / dmd_clip, [timing] ...), with how to use them.
	std::string FormatProjectorProfilePatch(const LedResults& results, const LedLimits& limits);
	std::string FormatLedCfg(const LedResults& results);
	std::string FormatLedJson(const LedResults& results, const LedLimits& limits, const std::vector<FluxStep>& steps,
		const std::vector<CntrSample>& cntrSamples);
	std::string FormatLedReview(const LedResults& results, const LedLimits& limits);

	// --------------------------------------------------------- crash marker

	std::string LedMarkerPath(const std::string& vpRoot);
	// VP's start: when a marker exists, restore full light / CNTR 50 / LDMM 0
	// on a new connection, then remove the marker. Returns a log line ("" when
	// there was no marker).
	std::string RecoverInterruptedLedRun(const std::string& vpRoot, const LedDrive::ProjectorConfig& projector,
		int ceiling, const LedDrive::SerialPortFactory& portFactory);

	// ------------------------------------------------------------- the run

	class LedCharacterizationRun final : public MeterRun
	{
	public:
		LedCharacterizationRun(const CalibrationSettings& settings, const SessionEnvironment& environment,
			std::unique_ptr<IMeter> meter, double meterFloorNits, IPatternSink& patterns, ISessionClock& clock,
			std::unique_ptr<IProjectorControl> projector, std::unique_ptr<ITimedLightSensor> sensor,
			const LedDrive::ProjectorConfig& projectorConfig, const LedDrive::LedDriveConfig& ledConfig);
		~LedCharacterizationRun() override;

		const LedResults& Results() const { return results; }
		const LedLimits& Limits() const { return limits; }
		const LedPlan& Plan() const { return plan; }
		bool ProjectorOpen() const { return projector && projector->IsOpen(); }
		void SetValidatorForTests(CandidateValidator v) { validator = std::move(v); }

	protected:
		const char* FolderSuffix() const override { return "led"; }
		bool OnStart(std::string& error) override;
		bool OnPatternsStarted(std::string& error) override;
		bool WantsWarmUp() const override { return true; }
		void BeginWork() override;
		void StepWork(SessionStep step, Command command) override;
		void OnFinishing(SessionStep step) override;
		std::string PlacementInstruction() const override;

	private:
		bool MeasureGrey();
		bool MeasureFlux(bool down);
		bool MeasureCntr();
		bool MeasureStep();
		bool RestoreProjector();
		void Fit();
		bool SetLight(int level, double& at);
		bool SetContrast(int value, double& at);
		bool MeasureBlack(double& y);
		void Advance();
		void Note(const std::string& line);

		std::unique_ptr<IProjectorControl> projector;
		std::unique_ptr<ITimedLightSensor> sensor;
		LedDrive::ProjectorConfig projectorConfig;
		LedDrive::LedDriveConfig ledConfig;
		LedLimits limits;
		LedPlan plan;
		LedPhase phase = LedPhase::Greyscale;
		size_t index = 0;
		int unitsDone = 0;
		bool sensorOpen = false;
		bool restored = false;
		bool markerWritten = false;
		int currentLight = -1;
		int currentContrast = -1;
		bool whiteShown = false;
		std::vector<FluxStep> fluxSteps;
		std::vector<CntrSample> cntrSamples;
		LedResults results;
	};
}
