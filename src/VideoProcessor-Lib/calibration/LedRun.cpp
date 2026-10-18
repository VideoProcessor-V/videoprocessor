#include <pch.h>

#include "LedRun.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Calibration
{
	namespace
	{
		double SteadySeconds(std::chrono::steady_clock::time_point t)
		{
			return std::chrono::duration<double>(t.time_since_epoch()).count();
		}

		double SteadyNow()
		{
			return SteadySeconds(std::chrono::steady_clock::now());
		}

		double MedianOf(std::vector<double> v)
		{
			if (v.empty()) return 0.0;
			std::sort(v.begin(), v.end());
			const size_t n = v.size();
			return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
		}

		std::wstring WidePath(const std::string& text)
		{
			if (text.empty()) return std::wstring();
			int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
			const UINT page = n > 0 ? CP_UTF8 : CP_ACP;
			n = MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
			if (n > 0) MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
			return out;
		}

		const char* StepKindName(LedStepKind::Kind kind)
		{
			switch (kind)
			{
			case LedStepKind::LightDown: return "light_down";
			case LedStepKind::LightUp: return "light_up";
			case LedStepKind::ContrastUp: return "contrast_up";
			case LedStepKind::ContrastDown: return "contrast_down";
			case LedStepKind::PictureUp: return "picture_up";
			default: return "picture_down";
			}
		}
	}

	// ------------------------------------------------------------ projector

	DriverProjectorControl::DriverProjectorControl(const LedDrive::ProjectorConfig& p, LedDrive::SerialPortFactory f)
		: projector(p), portFactory(std::move(f))
	{
		// The run speaks the module's barco_ascii protocol on its own connection.
		projector.protocol = "barco_ascii";
	}

	DriverProjectorControl::~DriverProjectorControl()
	{
		Close();
	}

	bool DriverProjectorControl::Open(std::string& error)
	{
		if (driver) return true;
		driver = LedDrive::CreateProjectorDriver("barco_ascii", projector, portFactory);
		if (!driver)
		{
			error = "no driver for the projector protocol";
			return false;
		}
		std::string why;
		if (!driver->Open(why))
		{
			driver.reset();
			error = "could not open " + projector.port + (why.empty() ? std::string() : ": " + why) +
				" (is another program using the projector's port?)";
			return false;
		}
		return true;
	}

	bool DriverProjectorControl::SetLight(int level, double& sentAt)
	{
		if (!driver) return false;
		const double before = SteadyNow();
		const bool ok = driver->SetLightNow(level, 0);
		const LedDrive::DriverStatus s = driver->Status();
		sentAt = s.lastLightSentAt.time_since_epoch().count() != 0 ? SteadySeconds(s.lastLightSentAt) : before;
		return ok;
	}

	bool DriverProjectorControl::SetContrast(int value, double& sentAt)
	{
		if (!driver) return false;
		const double before = SteadyNow();
		const bool ok = driver->SetContrast(value);
		const LedDrive::DriverStatus s = driver->Status();
		sentAt = s.lastContrastSentAt.time_since_epoch().count() != 0 ? SteadySeconds(s.lastContrastSentAt) : before;
		return ok;
	}

	bool DriverProjectorControl::Restore(int ceiling, std::string& report)
	{
		if (!driver) { report = "the projector connection is closed"; return false; }
		LedDrive::RestoreOutcome outcome;
		const bool light = LedDrive::RestoreFullLight(*driver, ceiling, true, true, &outcome);
		report = std::string("LPW1 ") + std::to_string(ceiling) + (outcome.light ? " sent" : " NOT confirmed") +
			", LDMM 0 " + (outcome.dimMode ? "sent" : "NOT confirmed") + ", CNTR 50 " +
			(outcome.contrast ? "sent" : "NOT confirmed");
		return light && outcome.Confirmed();
	}

	void DriverProjectorControl::Close()
	{
		if (!driver) return;
		driver->Close();
		driver.reset();
	}

	LedDrive::LightRange DriverProjectorControl::ReportedRange()
	{
		return driver && projector.readBack ? driver->QueryLightRange() : LedDrive::LightRange();
	}

	// ---------------------------------------------------------- fast sensor

	bool ParseTimedSensorLine(const std::string& line, bool& haveDeviceTime, double& deviceSeconds, double& value)
	{
		std::vector<double> numbers;
		size_t i = 0;
		while (i < line.size() && numbers.size() < 3)
		{
			const char c = line[i];
			if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')
			{
				char* end = nullptr;
				const double v = std::strtod(line.c_str() + i, &end);
				const size_t used = static_cast<size_t>(end - (line.c_str() + i));
				if (used == 0) { ++i; continue; }
				if (std::isfinite(v)) numbers.push_back(v);
				i += used;
			}
			else
				++i;
		}
		if (numbers.empty() || numbers.size() > 2) return false;
		haveDeviceTime = numbers.size() == 2;
		deviceSeconds = haveDeviceTime ? numbers[0] / 1e6 : 0.0;
		value = numbers.back();
		return true;
	}

	void AlignDeviceTimes(std::vector<TimedSample>& samples, const std::vector<double>& arrivals)
	{
		if (samples.empty() || arrivals.size() != samples.size()) return;
		double offset = 1e300;
		for (size_t i = 0; i < samples.size(); ++i)
			offset = (std::min)(offset, arrivals[i] - samples[i].seconds);
		for (TimedSample& s : samples) s.seconds += offset;
	}

	SerialTimedLightSensor::SerialTimedLightSensor(const std::string& p, int b, LedDrive::SerialPortFactory f, Clock c)
		: portName(p), baud(b), factory(std::move(f)), clock(c ? std::move(c) : Clock(SteadyNow))
	{
	}

	SerialTimedLightSensor::~SerialTimedLightSensor()
	{
		Close();
	}

	bool SerialTimedLightSensor::Open(std::string& error)
	{
		if (worker.joinable()) return true;
		port = factory ? factory() : nullptr;
		std::string why;
		if (!port || !port->Open(portName, baud, why))
		{
			error = "the fast light sensor on " + portName + " could not be opened" + (why.empty() ? std::string() : ": " + why);
			port.reset();
			return false;
		}
		port->DiscardInput();
		stopping = false;
		worker = std::thread(&SerialTimedLightSensor::Run, this);
		return true;
	}

	void SerialTimedLightSensor::Run()
	{
		for (;;)
		{
			{
				std::lock_guard<std::mutex> g(mutex);
				if (stopping) return;
			}
			std::string line;
			if (!port || !port->IsOpen()) return;
			if (!port->ReadLine(100, line)) continue;
			const double arrival = clock();
			bool device = false;
			double deviceSeconds = 0.0, value = 0.0;
			if (!ParseTimedSensorLine(line, device, deviceSeconds, value)) continue;
			std::lock_guard<std::mutex> g(mutex);
			if (!buffering) continue;
			if (samples.empty()) deviceTimes = device;
			if (device != deviceTimes) continue;
			TimedSample s;
			s.seconds = device ? deviceSeconds : arrival;
			s.value = value;
			samples.push_back(s);
			arrivals.push_back(arrival);
		}
	}

	void SerialTimedLightSensor::Begin()
	{
		std::lock_guard<std::mutex> g(mutex);
		samples.clear();
		arrivals.clear();
		buffering = true;
	}

	std::vector<TimedSample> SerialTimedLightSensor::Take()
	{
		std::vector<TimedSample> out;
		std::vector<double> times;
		bool device = false;
		{
			std::lock_guard<std::mutex> g(mutex);
			out.swap(samples);
			times.swap(arrivals);
			device = deviceTimes;
			buffering = false;
		}
		if (device) AlignDeviceTimes(out, times);
		return out;
	}

	void SerialTimedLightSensor::Close()
	{
		{
			std::lock_guard<std::mutex> g(mutex);
			stopping = true;
		}
		if (worker.joinable()) worker.join();
		if (port) port->Close();
		port.reset();
	}

	// --------------------------------------------------------------- limits

	int LedLimits::ClampLight(int level) const
	{
		// Never below the lowest level VP drives, never above full light, and
		// always inside the device's range.
		const int hi = (std::min)(ceiling, lightMax);
		const int lo = (std::min)((std::max)(lightMin, sweepLow), hi);
		return (std::min)((std::max)(level, lo), hi);
	}

	int LedLimits::ClampContrast(int value) const
	{
		return (std::min)((std::max)(value, contrastNeutral), (std::min)(contrastMax, contrastDeviceMax));
	}

	LedLimits ResolveLedLimits(const LedDrive::ProjectorConfig& projector, const LedDrive::LedDriveConfig& led,
		const CalibrationSettings& settings, const LedDrive::LightRange& reported)
	{
		LedLimits l;
		l.lightMin = projector.lightMin >= 0 ? projector.lightMin : 0;
		l.lightMax = projector.lightMax >= 0 ? projector.lightMax : 255;
		if (reported.known)
		{
			l.lightMin = (std::max)(l.lightMin, reported.minimum);
			l.lightMax = (std::min)(l.lightMax, reported.maximum);
		}
		if (l.lightMax < l.lightMin) l.lightMax = l.lightMin;
		l.ceiling = (std::min)((std::max)(led.ledCeiling, l.lightMin), l.lightMax);
		l.sweepLow = (std::min)((std::max)(led.ledFloorBlack, l.lightMin), l.ceiling);
		l.contrastNeutral = 50;
		l.contrastDeviceMax = 100;
		l.contrastMax = (std::min)((std::max)(settings.ledCntrSweepMax, l.contrastNeutral), l.contrastDeviceMax);
		return l;
	}

	// ----------------------------------------------------------------- plan

	int LedPlan::Units() const
	{
		return static_cast<int>(greys.size() + fluxDown.size() + fluxUp.size() + cntr.size() + steps.size());
	}

	LedPlan BuildLedPlan(const LedLimits& limits, const CalibrationSettings& settings, bool haveSensor)
	{
		LedPlan p;
		for (int i = 10; i >= 0; --i) p.greys.push_back(i / 10.0);
		const int step = (std::max)(1, settings.ledFluxStep);
		for (int level = limits.ceiling; level > limits.sweepLow; level -= step) p.fluxDown.push_back(level);
		p.fluxDown.push_back(limits.sweepLow);
		for (int level = limits.sweepLow + step; level < limits.ceiling; level += step) p.fluxUp.push_back(level);
		if (limits.ceiling > limits.sweepLow) p.fluxUp.push_back(limits.ceiling);
		for (int c = limits.contrastNeutral; c <= limits.contrastMax; ++c) p.cntr.push_back(c);
		if (haveSensor)
		{
			const int mid = (limits.ceiling + limits.sweepLow) / 2;
			const int contrastHigh = (std::min)(limits.contrastMax, limits.contrastNeutral + 10);
			for (int r = 0; r < (std::max)(1, settings.ledStepRepeats); ++r)
			{
				if (mid < limits.ceiling)
				{
					p.steps.push_back({ LedStepKind::LightDown, limits.ceiling, mid });
					p.steps.push_back({ LedStepKind::LightUp, mid, limits.ceiling });
				}
				if (contrastHigh > limits.contrastNeutral)
				{
					p.steps.push_back({ LedStepKind::ContrastUp, limits.contrastNeutral, contrastHigh });
					p.steps.push_back({ LedStepKind::ContrastDown, contrastHigh, limits.contrastNeutral });
				}
				p.steps.push_back({ LedStepKind::PictureUp, 0, 1 });
				p.steps.push_back({ LedStepKind::PictureDown, 1, 0 });
			}
		}
		return p;
	}

	bool LedBlackLevel(const LedPlan& plan, size_t i)
	{
		return i == 0 || i + 1 == plan.fluxDown.size() || i % 4 == 0;
	}

	double EstimateLedSeconds(const LedPlan& plan, const CalibrationSettings& s)
	{
		const double read = 1.2;   // a colorimeter on a projector's white, roughly
		double t = plan.greys.size() * (0.5 + 1.5 * read);
		t += (plan.fluxDown.size() + plan.fluxUp.size()) * (s.ledDwellMs / 1000.0 + s.ledReadsPerLevel * read);
		size_t blacks = 0;
		for (size_t i = 0; i < plan.fluxDown.size(); ++i) if (LedBlackLevel(plan, i)) ++blacks;
		t += blacks * (2 * 0.5 + 4 * read);
		t += plan.cntr.size() * (s.ledCntrDwellMs / 1000.0 + 2 * (0.5 + read)) + plan.cntr.size() / 4.0 * (0.5 + 4 * read);
		t += plan.steps.size() * 3.5;
		return t;
	}

	// -------------------------------------------------------------- results

	double FitGreyscaleGamma(const std::vector<std::pair<double, double>>& greys)
	{
		double black = 0.0, white = 0.0;
		bool haveBlack = false, haveWhite = false;
		for (const auto& g : greys)
		{
			if (g.first <= 1e-9) { black = g.second; haveBlack = true; }
			if (g.first >= 1.0 - 1e-9) { white = g.second; haveWhite = true; }
		}
		if (!haveWhite || !(white > black)) return 0.0;
		if (!haveBlack) black = 0.0;
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		int n = 0;
		for (const auto& g : greys)
		{
			if (g.first < 0.095 || g.first > 0.905) continue;
			const double rel = (g.second - black) / (white - black);
			if (!(rel > 0.0)) continue;
			const double x = std::log(g.first), y = std::log(rel);
			sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
		}
		if (n < 3) return 0.0;
		const double den = n * sxx - sx * sx;
		return den != 0.0 ? (n * sxy - sx * sy) / den : 0.0;
	}

	std::vector<ConfigEdit> LedResultEdits(const LedResults& r)
	{
		LedCalibrationResults in;
		in.flux = r.flux.ok ? &r.flux : nullptr;
		in.led = r.led.ok ? &r.led : nullptr;
		in.cntr = r.contrast.ok ? &r.contrast : nullptr;
		in.cntrCurve = r.cntr.ok ? &r.cntr : nullptr;
		in.compensationEotf = r.compensationEotf >= 1.0 && r.compensationEotf <= 3.0 ? r.compensationEotf : 0.0;
		return LedEdits(in, LedDrive::PROJECTOR_SECTION, LedDrive::LED_DRIVE_SECTION);
	}

	std::string FormatProjectorProfilePatch(const LedResults& r, const LedLimits& limits)
	{
		std::ostringstream out;
		out << "# Dynamic Drive characterisation (VP automatic calibration, phase 2)\n"
			<< "#\n"
			<< "# The measured values in .vpprojector form (feat/dd-projector-profiles).\n"
			<< "# To use them in a device profile: copy profiles\\dynamic-drive\\barco-fl35.vpprojector,\n"
			<< "# replace these keys in the matching sections, raise [profile] version, and\n"
			<< "# point [projector.default] device_profile at the copy. The same values are\n"
			<< "# also what Apply writes to [projector.default] (led_characterisation.cfg);\n"
			<< "# those keys override a profile's values, so either route gives the same result.\n"
			<< "# Drive values are in device units (VP's 0-255 LPW1 = device units on the FL35).\n"
			<< "# Measured between LPW1 " << limits.sweepLow << " and " << limits.ceiling << ", CNTR "
			<< limits.contrastNeutral << " to " << limits.contrastMax << ".\n\n";
		out << "[light]\n";
		if (r.flux.ok) out << "curve = " << r.flux.ConfigValue() << "\n";
		else out << "# curve: not measured\n";
		out << "\n[contrast]\n";
		if (r.cntr.ok)
		{
			out << "max_usable = " << r.cntr.cntrMaxUsable << "\n";
			out << "dmd_clip = " << r.cntr.dmdClipCntr << "\n";
		}
		else out << "# max_usable / dmd_clip: not measured\n";
		out << "\n[timing]\n";
		if (r.led.ok) out << "latency_ms = " << r.led.latencyMs << "\nsettle_ms = " << r.led.settleMs << "\n";
		else out << "# latency_ms / settle_ms: not measured (needs led_sensor_port)\n";
		if (r.contrast.ok)
			out << "contrast_latency_ms = " << r.contrast.latencyMs << "\ncontrast_settle_ms = " << r.contrast.settleMs << "\n";
		else out << "# contrast_latency_ms / contrast_settle_ms: not measured (needs led_sensor_port)\n";
		if (r.compensationEotf >= 1.0)
			out << "\n# [led_drive.default] compensation_eotf = " << FixedText(r.compensationEotf, 2)
				<< " (a VideoProcessor.cfg key, not a profile key)\n";
		return out.str();
	}

	std::string FormatLedCfg(const LedResults& r)
	{
		std::ostringstream out;
		out << "# Dynamic Drive characterisation: the keys Apply writes (VP automatic calibration, phase 2).\n";
		std::string section;
		for (const ConfigEdit& e : LedResultEdits(r))
		{
			if (e.section != section)
			{
				out << (section.empty() ? "" : "\n") << "[" << e.section << "]\n";
				section = e.section;
			}
			out << e.key << ": " << e.value << "\n";
		}
		return out.str();
	}

	std::string FormatLedJson(const LedResults& r, const LedLimits& l, const std::vector<FluxStep>& steps,
		const std::vector<CntrSample>& cntr)
	{
		std::ostringstream out;
		out << "{\n  \"limits\": {\"light_min\":" << l.lightMin << ",\"light_max\":" << l.lightMax << ",\"sweep_low\":"
			<< l.sweepLow << ",\"ceiling\":" << l.ceiling << ",\"contrast_neutral\":" << l.contrastNeutral
			<< ",\"contrast_max\":" << l.contrastMax << "},\n";
		out << "  \"flux\": {\"ok\":" << (r.flux.ok ? "true" : "false") << ",\"table\":" << JsonText(r.flux.ConfigValue())
			<< ",\"max_hysteresis\":" << JsonNumber(r.flux.maxHysteresis, 5) << ",\"points\":[";
		for (size_t i = 0; i < r.flux.points.size(); ++i)
		{
			const FluxPoint& p = r.flux.points[i];
			out << (i ? "," : "") << "{\"lpw\":" << p.lpw << ",\"up\":" << (p.haveUp ? JsonNumber(p.up) : "null")
				<< ",\"down\":" << (p.haveDown ? JsonNumber(p.down) : "null") << ",\"flux\":" << JsonNumber(p.flux)
				<< ",\"hysteresis\":" << JsonNumber(p.hysteresis, 5) << "}";
		}
		out << "]},\n  \"samples\": [";
		for (size_t i = 0; i < steps.size(); ++i)
		{
			out << (i ? "," : "") << "{\"lpw\":" << steps[i].lpw << ",\"dir\":\""
				<< (steps[i].direction == StepDirection::Up ? "up" : "down") << "\",\"y\":[";
			for (size_t k = 0; k < steps[i].samples.size(); ++k)
				out << (k ? "," : "") << "[" << JsonNumber(steps[i].samples[k].seconds, 10) << ","
					<< JsonNumber(steps[i].samples[k].value, 7) << "]";
			out << "]}";
		}
		out << "],\n  \"black_by_light\": [";
		for (size_t i = 0; i < r.blackByLight.size(); ++i)
			out << (i ? "," : "") << "[" << r.blackByLight[i].first << "," << JsonNumber(r.blackByLight[i].second, 7) << "]";
		out << "],\n  \"cntr\": {\"ok\":" << (r.cntr.ok ? "true" : "false") << ",\"clip_reached\":"
			<< (r.cntr.clipReached ? "true" : "false") << ",\"clip\":" << JsonNumber(r.cntr.clipExact, 5)
			<< ",\"dmd_clip_cntr\":" << r.cntr.dmdClipCntr << ",\"usable\":" << JsonNumber(r.cntr.usableExact, 5)
			<< ",\"cntr_max_usable\":" << r.cntr.cntrMaxUsable << ",\"plateau\":" << JsonNumber(r.cntr.plateau, 6)
			<< ",\"samples\":[";
		for (size_t i = 0; i < cntr.size(); ++i)
			out << (i ? "," : "") << "[" << cntr[i].cntr << "," << cntr[i].code << "," << JsonNumber(cntr[i].luminance, 7) << "]";
		out << "]},\n  \"black_by_contrast\": [";
		for (size_t i = 0; i < r.blackByContrast.size(); ++i)
			out << (i ? "," : "") << "[" << r.blackByContrast[i].first << "," << JsonNumber(r.blackByContrast[i].second, 7) << "]";
		out << "],\n  \"greys\": [";
		for (size_t i = 0; i < r.greys.size(); ++i)
			out << (i ? "," : "") << "[" << JsonNumber(r.greys[i].first, 4) << "," << JsonNumber(r.greys[i].second, 7) << "]";
		out << "],\n  \"compensation_eotf\": " << JsonNumber(r.compensationEotf, 4) << ",\n";
		auto steps_ = [&](const char* name, const std::vector<StepResponseFit>& fits)
		{
			out << "  \"" << name << "\": [";
			for (size_t i = 0; i < fits.size(); ++i)
				out << (i ? "," : "") << "{\"ok\":" << (fits[i].ok ? "true" : "false") << ",\"dead_ms\":"
					<< JsonNumber(fits[i].deadTimeSeconds * 1000.0, 5) << ",\"tau_ms\":" << JsonNumber(fits[i].tauSeconds * 1000.0, 5)
					<< ",\"ramp_ms\":" << JsonNumber(fits[i].rampSeconds * 1000.0, 5) << ",\"rms\":" << JsonNumber(fits[i].rmsResidual, 4)
					<< "}";
			out << "],\n";
		};
		steps_("light_steps", r.lightSteps);
		steps_("contrast_steps", r.contrastSteps);
		steps_("picture_steps", r.pictureSteps);
		out << "  \"led_latency_ms\": " << (r.led.ok ? std::to_string(r.led.latencyMs) : "null")
			<< ",\n  \"led_settle_ms\": " << (r.led.ok ? std::to_string(r.led.settleMs) : "null")
			<< ",\n  \"cntr_latency_ms\": " << (r.contrast.ok ? std::to_string(r.contrast.latencyMs) : "null")
			<< ",\n  \"cntr_settle_ms\": " << (r.contrast.ok ? std::to_string(r.contrast.settleMs) : "null")
			<< ",\n  \"led_after_picture_ms\": " << (r.havePictureOffset ? JsonNumber(r.ledAfterPictureSeconds * 1000.0, 5) : "null")
			<< ",\n  \"restore_confirmed\": " << (r.restoreConfirmed ? "true" : "false")
			<< ",\n  \"restore\": " << JsonText(r.restoreReport) << "\n}\n";
		return out.str();
	}

	std::string FormatLedReview(const LedResults& r, const LedLimits& l)
	{
		std::ostringstream out;
		out << "Dynamic Drive characterisation (LPW1 " << l.sweepLow << "-" << l.ceiling << ", CNTR " << l.contrastNeutral
			<< "-" << l.contrastMax << ").";
		if (r.flux.ok)
		{
			out << "\nFlux table (" << r.flux.table.size() << " levels): light at LPW1 " << l.sweepLow << " is "
				<< FixedText(r.flux.table.front().second * 100.0, 1) << "% of full; largest up/down hysteresis "
				<< FixedText(r.flux.maxHysteresis * 100.0, 2) << "%.";
		}
		else out << "\nFlux table: not measured.";
		if (!r.blackByLight.empty())
		{
			out << "\nBlack: " << FixedText(r.blackByLight.front().second, 4) << " cd/m2 at LPW1 " << r.blackByLight.front().first
				<< ", " << FixedText(r.blackByLight.back().second, 4) << " cd/m2 at LPW1 " << r.blackByLight.back().first << ".";
		}
		if (r.cntr.ok)
		{
			out << "\nCNTR: code 235 " << (r.cntr.clipReached ? "reaches the DMD's maximum at CNTR " +
				std::to_string(r.cntr.dmdClipCntr) : std::string("did not reach a plateau in the sweep (dmd_clip_cntr -1)"))
				<< "; light stops rising at CNTR " << r.cntr.cntrMaxUsable << " (cntr_max_usable).";
			if (r.cntr.cntrMaxUsable >= l.contrastMax)
				out << " Still rising at the sweep maximum: raise led_cntr_sweep_max to find the knee.";
		}
		else out << "\nCNTR curve: not measured.";
		if (!r.blackByContrast.empty())
			out << "\nBlack against CNTR: " << FixedText(r.blackByContrast.front().second, 4) << " cd/m2 at CNTR "
				<< r.blackByContrast.front().first << ", " << FixedText(r.blackByContrast.back().second, 4) << " cd/m2 at CNTR "
				<< r.blackByContrast.back().first << ".";
		if (r.compensationEotf >= 1.0)
			out << "\nGreyscale effective gamma (compensation_eotf): " << FixedText(r.compensationEotf, 2) << ".";
		if (r.led.ok)
			out << "\nLED response: " << r.led.latencyMs << " ms dead time, " << r.led.settleMs << " ms settle"
				<< (r.led.rampDetected ? " (an internal ramp was detected)" : "") << ".";
		else out << "\nLED latency / settle: not measured (set led_sensor_port to a fast light sensor).";
		if (r.contrast.ok)
			out << "\nCNTR response: " << r.contrast.latencyMs << " ms dead time, " << r.contrast.settleMs << " ms settle.";
		if (r.havePictureOffset)
			out << "\nThe LED lands " << FixedText(r.ledAfterPictureSeconds * 1000.0, 0) << " ms after a picture change sent at the same time.";
		out << "\nProjector restored: " << (r.restoreConfirmed ? "yes" : "NOT CONFIRMED") << " (" << r.restoreReport << ").";
		for (const std::string& n : r.notes) out << "\n" << n;
		return out.str();
	}

	// --------------------------------------------------------- crash marker

	std::string LedMarkerPath(const std::string& vpRoot)
	{
		return (vpRoot.empty() ? std::string(".") : vpRoot) + "\\luts\\calibration\\led-run-active.txt";
	}

	std::string RecoverInterruptedLedRun(const std::string& vpRoot, const LedDrive::ProjectorConfig& projectorIn,
		int ceiling, const LedDrive::SerialPortFactory& portFactory)
	{
		const std::string marker = LedMarkerPath(vpRoot);
		if (GetFileAttributesW(WidePath(marker).c_str()) == INVALID_FILE_ATTRIBUTES) return std::string();
		DriverProjectorControl control(projectorIn, portFactory);
		std::string error, report;
		if (!control.Open(error))
			return "Calibration: an interrupted Dynamic Drive characterisation left a marker, but " + error +
				"; the marker stays for the next start";
		const bool ok = control.Restore(ceiling, report);
		control.Close();
		if (ok) DeleteFileW(WidePath(marker).c_str());
		return std::string("Calibration: an interrupted Dynamic Drive characterisation was found; projector restored (") +
			report + ")" + (ok ? "" : "; not confirmed, the marker stays for the next start");
	}

	// -------------------------------------------------------------- the run

	LedCharacterizationRun::LedCharacterizationRun(const CalibrationSettings& s, const SessionEnvironment& e,
		std::unique_ptr<IMeter> m, double floor, IPatternSink& p, ISessionClock& c,
		std::unique_ptr<IProjectorControl> pc, std::unique_ptr<ITimedLightSensor> fs,
		const LedDrive::ProjectorConfig& projectorIn, const LedDrive::LedDriveConfig& ledIn)
		: MeterRun("led", s, e, std::move(m), floor, p, c), projector(std::move(pc)), sensor(std::move(fs)),
		projectorConfig(projectorIn), ledConfig(ledIn)
	{
	}

	LedCharacterizationRun::~LedCharacterizationRun()
	{
		Shutdown();
	}

	std::string LedCharacterizationRun::PlacementInstruction() const
	{
		return MeterRun::PlacementInstruction() +
			" VP drives the projector's LED (LPW1) and contrast (CNTR) during this run and restores full light and CNTR " +
			std::to_string(limits.contrastNeutral) + " at the end or on Cancel.";
	}

	bool LedCharacterizationRun::OnStart(std::string& error)
	{
		if (!projector)
		{
			error = "The Dynamic Drive characterisation needs the projector's RS-232 connection: set [projector.default] "
				"protocol to barco_ascii and its port.";
			return false;
		}
		limits = ResolveLedLimits(projectorConfig, ledConfig, settings, LedDrive::LightRange());
		plan = BuildLedPlan(limits, settings, sensor != nullptr);
		std::lock_guard<std::mutex> g(mutex);
		status.patchCount = plan.Units();
		status.secondsRemaining = EstimateLedSeconds(plan, settings);
		return true;
	}

	void LedCharacterizationRun::Note(const std::string& line)
	{
		results.notes.push_back(line);
		WriteRaw("{\"note\":" + JsonText(line) + "}");
	}

	bool LedCharacterizationRun::OnPatternsStarted(std::string& error)
	{
		// The renderer stopped VP's Dynamic Drive for the patterns: the port is free.
		if (!projector->Open(error))
		{
			error = "The projector could not be opened: " + error;
			return false;
		}
		std::string markerError;
		WriteTextFile(LedMarkerPath(environment.vpRoot), "Dynamic Drive characterisation in progress\nport: " +
			projectorConfig.port + "\nceiling: " + std::to_string(limits.ceiling) + "\nstarted: " + stamp.Format() + "\n",
			markerError);
		markerWritten = markerError.empty();
		const LedDrive::LightRange reported = projector->ReportedRange();
		if (reported.known)
		{
			limits = ResolveLedLimits(projectorConfig, ledConfig, settings, reported);
			plan = BuildLedPlan(limits, settings, sensor != nullptr);
			std::lock_guard<std::mutex> g(mutex);
			status.patchCount = plan.Units();
		}
		std::string report;
		if (!projector->Restore(limits.ceiling, report))
			Note("Start: bringing the projector to full light / CNTR 50 was not confirmed (" + report + ").");
		currentLight = limits.ceiling;
		currentContrast = limits.contrastNeutral;
		WriteRaw("{\"event\":\"projector_open\",\"limits\":{\"light_min\":" + std::to_string(limits.lightMin) +
			",\"light_max\":" + std::to_string(limits.lightMax) + ",\"sweep_low\":" + std::to_string(limits.sweepLow) +
			",\"ceiling\":" + std::to_string(limits.ceiling) + ",\"contrast_max\":" + std::to_string(limits.contrastMax) + "}}");
		return true;
	}

	void LedCharacterizationRun::BeginWork()
	{
		phase = LedPhase::Greyscale;
		index = 0;
		phaseStart = clock.Now();
		if (sensor)
		{
			std::string error;
			sensorOpen = sensor->Open(error);
			if (!sensorOpen)
			{
				Note("The fast light sensor could not be opened (" + error + "); latency and settle are not measured.");
				plan.steps.clear();
			}
		}
		SetProgress(0, plan.Units());
		SetStep(SessionStep::Measuring, "Measuring the Dynamic Drive: VP steps the LED and CNTR. Keep the room dark and "
			"the meter still.");
	}

	void LedCharacterizationRun::Advance()
	{
		++index;
		++unitsDone;
		SetProgress(unitsDone, plan.Units());
	}

	bool LedCharacterizationRun::SetLight(int level, double& at)
	{
		const int clamped = limits.ClampLight(level);
		if (!projector->SetLight(clamped, at))
		{
			Fail("The projector did not take LPW1 " + std::to_string(clamped) + ". Full light and CNTR " +
				std::to_string(limits.contrastNeutral) + " were restored.");
			return false;
		}
		currentLight = clamped;
		return true;
	}

	bool LedCharacterizationRun::SetContrast(int value, double& at)
	{
		const int clamped = limits.ClampContrast(value);
		if (!projector->SetContrast(clamped, at))
		{
			Fail("The projector did not take CNTR " + std::to_string(clamped) + ". Full light and CNTR " +
				std::to_string(limits.contrastNeutral) + " were restored.");
			return false;
		}
		currentContrast = clamped;
		return true;
	}

	bool LedCharacterizationRun::MeasureBlack(double& y)
	{
		std::string error;
		if (!ShowPatch(Signal(0.0, 0.0, 0.0), 0.0, error)) { Fail(error); return false; }
		whiteShown = false;
		double elapsed = 0.0;
		Vec3 xyz;
		int reads = 0;
		bool floorLimited = false;
		std::string note;
		const ReadOutcome o = MeasureCodes(elapsed, xyz, reads, floorLimited, note);
		if (o == ReadOutcome::Stop) return false;
		y = o == ReadOutcome::Ok ? xyz[1] : -1.0;
		return true;
	}

	bool LedCharacterizationRun::MeasureGrey()
	{
		const double g = plan.greys[index];
		std::string error;
		if (!ShowPatch(Signal(g, g, g), whiteY * std::pow(g, 2.2), error)) { Fail(error); return false; }
		whiteShown = g >= 1.0;
		double elapsed = 0.0;
		Vec3 xyz;
		int reads = 0;
		bool floorLimited = false;
		std::string note;
		const ReadOutcome o = MeasureCodes(elapsed, xyz, reads, floorLimited, note);
		if (o == ReadOutcome::Stop) return false;
		if (o == ReadOutcome::Ok)
		{
			results.greys.push_back(std::make_pair(g, xyz[1]));
			if (g >= 1.0) whiteY = xyz[1];
		}
		WriteRaw("{\"phase\":\"grey\",\"signal\":" + JsonNumber(g, 4) + ",\"xyz\":[" + JsonNumber(xyz[0], 8) + "," +
			JsonNumber(xyz[1], 8) + "," + JsonNumber(xyz[2], 8) + "],\"reads\":" + std::to_string(reads) + ",\"status\":" +
			(o == ReadOutcome::Ok ? "\"ok\"" : "\"unreadable\"") + "}");
		return true;
	}

	bool LedCharacterizationRun::MeasureFlux(bool down)
	{
		const int level = down ? plan.fluxDown[index] : plan.fluxUp[index];
		std::string error;
		if (!whiteShown)
		{
			if (!ShowPatch(Signal(1.0, 1.0, 1.0), whiteY, error)) { Fail(error); return false; }
			whiteShown = true;
		}
		double at = 0.0;
		if (!SetLight(level, at)) return false;
		if (!Wait(settings.ledDwellMs / 1000.0)) return false;
		FluxStep step;
		step.lpw = limits.ClampLight(level);
		step.direction = down ? StepDirection::Down : StepDirection::Up;
		std::string note;
		for (int k = 0; k < (std::max)(1, settings.ledReadsPerLevel); ++k)
		{
			Vec3 xyz;
			double t = 0.0;
			const ReadOutcome o = ReadOnce(xyz, t, note);
			if (o == ReadOutcome::Stop) return false;
			if (o != ReadOutcome::Ok) continue;
			TimedSample s;
			s.seconds = t;
			s.value = xyz[1];
			step.samples.push_back(s);
		}
		std::string ys;
		for (const TimedSample& s : step.samples) ys += (ys.empty() ? "" : ",") + JsonNumber(s.value, 8);
		WriteRaw("{\"phase\":\"flux\",\"lpw\":" + std::to_string(step.lpw) + ",\"dir\":\"" + (down ? "down" : "up") +
			"\",\"y\":[" + ys + "]" + (note.empty() ? std::string() : ",\"note\":" + JsonText(note)) + "}");
		if (!step.samples.empty()) fluxSteps.push_back(step);
		if (down && LedBlackLevel(plan, index))
		{
			double y = 0.0;
			if (!MeasureBlack(y)) return false;
			if (y >= 0.0) results.blackByLight.push_back(std::make_pair(step.lpw, y));
			WriteRaw("{\"phase\":\"black_by_light\",\"lpw\":" + std::to_string(step.lpw) + ",\"y\":" + JsonNumber(y, 8) + "}");
		}
		return true;
	}

	bool LedCharacterizationRun::MeasureCntr()
	{
		if (index == 0 && currentLight != limits.ceiling)
		{
			double at = 0.0;
			if (!SetLight(limits.ceiling, at)) return false;
			if (!Wait(settings.ledDwellMs / 1000.0)) return false;
		}
		const int c = plan.cntr[index];
		double at = 0.0;
		if (!SetContrast(c, at)) return false;
		if (!Wait(settings.ledCntrDwellMs / 1000.0)) return false;
		struct Probe { double signal; int code8; };
		std::vector<Probe> probes = { { 1.0, 235 } };
		if (format.limitedRange)
			probes.push_back({ CodeToSignal(format, HighestLegalCode(format)), 254 });
		for (const Probe& probe : probes)
		{
			std::string error;
			if (!ShowPatch(Signal(probe.signal, probe.signal, probe.signal), whiteY, error)) { Fail(error); return false; }
			whiteShown = probe.code8 == 235;
			double elapsed = 0.0;
			Vec3 xyz;
			int reads = 0;
			bool floorLimited = false;
			std::string note;
			const ReadOutcome o = MeasureCodes(elapsed, xyz, reads, floorLimited, note);
			if (o == ReadOutcome::Stop) return false;
			if (o == ReadOutcome::Ok)
			{
				CntrSample s;
				s.cntr = limits.ClampContrast(c);
				s.code = probe.code8;
				s.luminance = xyz[1];
				cntrSamples.push_back(s);
			}
			WriteRaw("{\"phase\":\"cntr\",\"cntr\":" + std::to_string(c) + ",\"code8\":" + std::to_string(probe.code8) +
				",\"y\":" + JsonNumber(xyz[1], 8) + ",\"status\":" + (o == ReadOutcome::Ok ? "\"ok\"" : "\"unreadable\"") + "}");
		}
		if (index == 0 || index + 1 == plan.cntr.size() || index % 4 == 0)
		{
			double y = 0.0;
			if (!MeasureBlack(y)) return false;
			if (y >= 0.0) results.blackByContrast.push_back(std::make_pair(c, y));
			WriteRaw("{\"phase\":\"black_by_contrast\",\"cntr\":" + std::to_string(c) + ",\"y\":" + JsonNumber(y, 8) + "}");
		}
		if (index + 1 == plan.cntr.size())
		{
			// Back to the default before anything else.
			if (!SetContrast(limits.contrastNeutral, at)) return false;
			if (!Wait(settings.ledCntrDwellMs / 1000.0)) return false;
		}
		return true;
	}

	bool LedCharacterizationRun::MeasureStep()
	{
		const LedStepKind s = plan.steps[index];
		std::string error;
		double at = 0.0;
		const bool light = s.kind == LedStepKind::LightDown || s.kind == LedStepKind::LightUp;
		const bool contrast = s.kind == LedStepKind::ContrastUp || s.kind == LedStepKind::ContrastDown;
		// The state before the step.
		const int wantLight = light ? s.from : limits.ceiling;
		const int wantContrast = contrast ? s.from : limits.contrastNeutral;
		const double patch = light ? 1.0 : contrast ? 0.5 : static_cast<double>(s.from);
		if (!ShowPatch(Signal(patch, patch, patch), whiteY, error)) { Fail(error); return false; }
		whiteShown = patch >= 1.0;
		bool waitSettle = false;
		if (currentLight != wantLight) { if (!SetLight(wantLight, at)) return false; waitSettle = true; }
		if (currentContrast != wantContrast) { if (!SetContrast(wantContrast, at)) return false; waitSettle = true; }
		if (waitSettle && !Wait(1.0)) return false;
		sensor->Begin();
		if (!Wait(0.4)) return false;
		if (light) { if (!SetLight(s.to, at)) return false; }
		else if (contrast) { if (!SetContrast(s.to, at)) return false; }
		else
		{
			const PatchCodes codes = Signal(s.to, s.to, s.to);
			PatternRequest r;
			r.active = true;
			r.generation = ++generation;
			for (int k = 0; k < 3; ++k) r.patch[k] = codes.container[k];
			const double bg = (std::min)((std::max)(settings.backgroundPercent, 0.0), 100.0) / 100.0;
			const PatchCodes background = CodesForSignal(format, { bg, bg, bg });
			for (int k = 0; k < 3; ++k) r.background[k] = background.container[k];
			r.windowAreaPercent = static_cast<float>(EffectiveWindowPercent(settings));
			if (!patterns.Show(r, error)) { Fail(error); return false; }
			at = clock.Now();
			patternsOn = true;
			whiteShown = s.to >= 1;
		}
		if (!Wait(2.0)) return false;
		const std::vector<TimedSample> samples = sensor->Take();
		const StepResponseFit fit = FitStepResponse(samples, at);
		(light ? results.lightSteps : contrast ? results.contrastSteps : results.pictureSteps).push_back(fit);
		WriteRaw(std::string("{\"phase\":\"step\",\"kind\":\"") + StepKindName(s.kind) + "\",\"from\":" +
			std::to_string(s.from) + ",\"to\":" + std::to_string(s.to) + ",\"samples\":" + std::to_string(samples.size()) +
			",\"ok\":" + (fit.ok ? "true" : "false") + ",\"dead_ms\":" + JsonNumber(fit.deadTimeSeconds * 1000.0, 5) +
			",\"tau_ms\":" + JsonNumber(fit.tauSeconds * 1000.0, 5) + (fit.ok ? std::string() : ",\"error\":" + JsonText(fit.error)) + "}");
		return true;
	}

	bool LedCharacterizationRun::RestoreProjector()
	{
		if (restored) return results.restoreConfirmed;
		restored = true;
		if (sensor && sensorOpen) { sensor->Close(); sensorOpen = false; }
		if (!projector || !projector->IsOpen())
		{
			results.restoreConfirmed = true;
			results.restoreReport = "the projector was not opened";
			return true;
		}
		std::string report;
		bool ok = projector->Restore(limits.ceiling, report);
		if (!ok)
		{
			clock.Sleep(0.5);
			std::string again;
			ok = projector->Restore(limits.ceiling, again);
			report += "; retried: " + again;
		}
		projector->Close();
		results.restoreConfirmed = ok;
		results.restoreReport = report;
		currentLight = limits.ceiling;
		currentContrast = limits.contrastNeutral;
		if (ok && markerWritten) DeleteFileW(WidePath(LedMarkerPath(environment.vpRoot)).c_str());
		if (!ok)
			results.notes.push_back("WARNING: restoring full light and CNTR " + std::to_string(limits.contrastNeutral) +
				" was not confirmed (" + report + "). VP's Dynamic Drive sets both again when it restarts; VP's next start "
				"also restores them (the marker file stays).");
		WriteRaw("{\"event\":\"restore\",\"confirmed\":" + std::string(ok ? "true" : "false") + ",\"report\":" +
			JsonText(report) + "}");
		DebugLog::Log("Calibration (Dynamic Drive): projector restored to LPW1 %d / CNTR %d: %s (%s)", limits.ceiling,
			limits.contrastNeutral, ok ? "confirmed" : "NOT confirmed", report.c_str());
		return ok;
	}

	void LedCharacterizationRun::OnFinishing(SessionStep)
	{
		RestoreProjector();
	}

	void LedCharacterizationRun::Fit()
	{
		FluxFitOptions fluxOptions;
		fluxOptions.settledFraction = 1.0;   // every reading is after the dwell
		fluxOptions.referenceLpw = limits.ceiling;
		results.flux = FitFluxTable(fluxSteps, fluxOptions);
		if (!results.flux.ok && !fluxSteps.empty()) Note("Flux table: " + results.flux.error + ".");
		if (plan.cntr.size() >= 4)
		{
			results.cntr = FitCntrCurve(cntrSamples, 235);
			if (!results.cntr.ok) Note("CNTR curve: " + results.cntr.error + ".");
		}
		const double gamma = FitGreyscaleGamma(results.greys);
		if (gamma >= 1.0 && gamma <= 3.0) results.compensationEotf = gamma;
		results.led = SummarizeSteps(results.lightSteps);
		results.contrast = SummarizeSteps(results.contrastSteps);
		const LatencySummary picture = SummarizeSteps(results.pictureSteps);
		if (picture.ok && results.led.ok)
		{
			std::vector<double> led, pic;
			for (const StepResponseFit& f : results.lightSteps) if (f.ok) led.push_back(f.deadTimeSeconds);
			for (const StepResponseFit& f : results.pictureSteps) if (f.ok) pic.push_back(f.deadTimeSeconds);
			results.ledAfterPictureSeconds = MedianOf(led) - MedianOf(pic);
			results.havePictureOffset = true;
		}
		std::string error;
		WriteTextFile(folder.runDir + "\\led_characterisation.cfg", FormatLedCfg(results), error);
		WriteTextFile(folder.runDir + "\\projector_profile_measured.txt", FormatProjectorProfilePatch(results, limits), error);
		WriteTextFile(folder.runDir + "\\led.json", FormatLedJson(results, limits, fluxSteps, cntrSamples), error);
		const std::string review = FormatLedReview(results, limits);
		std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>VP Dynamic Drive characterisation</title>"
			"</head><body style=\"font-family:Segoe UI,Arial,sans-serif\"><h1>Dynamic Drive characterisation</h1><pre>";
		for (const char ch : review)
			html += ch == '<' ? std::string("&lt;") : ch == '&' ? std::string("&amp;") : std::string(1, ch);
		html += "</pre><p>Files: led_characterisation.cfg, projector_profile_measured.txt, led.json, raw.jsonl.</p></body></html>\n";
		WriteTextFile(folder.reportHtml, html, error);
		SetReview(review, LedResultEdits(results), "[projector.default] / [led_drive.default]");
	}

	void LedCharacterizationRun::StepWork(SessionStep step, Command)
	{
		if (step == SessionStep::Measuring)
		{
			// Phases with nothing to measure (no CNTR range, no fast sensor) are skipped.
			for (;;)
			{
				const size_t count = phase == LedPhase::Greyscale ? plan.greys.size() : phase == LedPhase::FluxDown ?
					plan.fluxDown.size() : phase == LedPhase::FluxUp ? plan.fluxUp.size() : phase == LedPhase::Cntr ?
					plan.cntr.size() : phase == LedPhase::Steps ? (sensorOpen ? plan.steps.size() : 0) : 1;
				if (phase == LedPhase::Done || index < count) break;
				phase = static_cast<LedPhase>(static_cast<int>(phase) + 1);
				index = 0;
				whiteShown = whiteShown && phase != LedPhase::Cntr;
			}
			bool ok = true;
			switch (phase)
			{
			case LedPhase::Greyscale: ok = MeasureGrey(); break;
			case LedPhase::FluxDown: ok = MeasureFlux(true); break;
			case LedPhase::FluxUp: ok = MeasureFlux(false); break;
			case LedPhase::Cntr: ok = MeasureCntr(); break;
			case LedPhase::Steps: ok = MeasureStep(); break;
			case LedPhase::Done:
				RestoreProjector();
				EndPatterns();
				SetStep(SessionStep::Fitting, "Fitting the flux table, the CNTR curve and the response times...");
				return;
			}
			if (!ok) return;   // cancel (handled by the next Step), a meter prompt or a failure
			Advance();
			return;
		}
		if (step == SessionStep::Fitting)
		{
			Fit();
			return;
		}
		clock.Sleep(0.05);
	}
}
