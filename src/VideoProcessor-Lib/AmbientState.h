#pragma once

#include <cmath>
#include <cstdint>
#include <string>

// Stage 9a (docs/VP-stage9-ambient.md): whether the room is dark or lit,
// for the profile rules' `ambient` variable. Pure logic, so the switching
// rules are unit-tested with simulated readings; the detector's driver (9b)
// only feeds readings in.
namespace Ambient
{
	enum class Mode { Off, Manual, Sensor };
	enum class Room { Dark, Lit };

	struct Settings
	{
		Mode mode = Mode::Off;
		Room room = Room::Dark;        // manual state; the sensor's fallback
		double luxLit = 20.0;          // above: lit
		double luxDark = 8.0;          // below: dark; in between: unchanged
		double holdSeconds = 5.0;      // a new state must last this long
		double lostSeconds = 10.0;     // no reading this long: detector lost
		std::string port;              // 9b: the sensor's COM port (its own, never the projector's)
		int baud = 115200;
	};

	inline bool operator==(const Settings& a, const Settings& b)
	{
		return a.mode == b.mode && a.room == b.room && a.luxLit == b.luxLit && a.luxDark == b.luxDark &&
			a.holdSeconds == b.holdSeconds && a.lostSeconds == b.lostSeconds && a.port == b.port &&
			a.baud == b.baud;
	}

	inline const char* Name(Room room) { return room == Room::Lit ? "lit" : "dark"; }
	inline const char* Name(Mode mode)
	{
		return mode == Mode::Manual ? "manual" : mode == Mode::Sensor ? "sensor" : "off";
	}

	inline bool ParseMode(const std::string& value, Mode& mode)
	{
		if (value == "off") { mode = Mode::Off; return true; }
		if (value == "manual") { mode = Mode::Manual; return true; }
		if (value == "sensor") { mode = Mode::Sensor; return true; }
		return false;
	}

	inline bool ParseRoom(const std::string& value, Room& room)
	{
		if (value == "dark") { room = Room::Dark; return true; }
		if (value == "lit") { room = Room::Lit; return true; }
		return false;
	}

	// Times are seconds on one monotonic clock.
	class State
	{
	public:
		void Configure(const Settings& value)
		{
			if (value == settings)
				return;
			// A new configuration starts the sensor over from the saved room:
			// the next reading decides at once.
			settings = value;
			sensorRoom = settings.room;
			sensed = false;
			candidateSince = -1.0;
		}

		// A detector reading (lux). Returns true when the sensed room changed.
		bool AddReading(double lux, double now)
		{
			lastReading = now;
			haveReading = true;
			lastLux = lux;
			if (settings.mode != Mode::Sensor || !std::isfinite(lux))
				return false;
			Room wanted = sensorRoom;
			if (lux > settings.luxLit) wanted = Room::Lit;
			else if (lux < settings.luxDark) wanted = Room::Dark;
			if (!sensed)
			{
				// The first reading decides at once (no hold at start).
				sensed = true;
				const bool changed = wanted != sensorRoom;
				sensorRoom = wanted;
				candidateSince = -1.0;
				return changed;
			}
			if (wanted == sensorRoom)
			{
				candidateSince = -1.0;
				return false;
			}
			if (candidateSince < 0.0 || candidateRoom != wanted)
			{
				candidateRoom = wanted;
				candidateSince = now;
			}
			if (now - candidateSince >= settings.holdSeconds)
			{
				sensorRoom = wanted;
				candidateSince = -1.0;
				return true;
			}
			return false;
		}

		// The detector is lost after lostSeconds without a reading (the room
		// then falls back to the saved `room`). Call periodically.
		bool DetectorLost(double now) const
		{
			return !haveReading || now - lastReading > settings.lostSeconds;
		}

		// The room for the rules, or false when ambient switching is off
		// (the variable is then empty and no `ambient` rule matches).
		bool Current(double now, Room& room) const
		{
			switch (settings.mode)
			{
			case Mode::Off: return false;
			case Mode::Manual: room = settings.room; return true;
			case Mode::Sensor: room = sensed && !DetectorLost(now) ? sensorRoom : settings.room; return true;
			}
			return false;
		}

		// Called when the detector is lost, so the next reading decides at
		// once again.
		void ResetSensor()
		{
			sensed = false;
			candidateSince = -1.0;
		}

		bool Sensed() const { return sensed; }
		double LastLux() const { return lastLux; }
		const Settings& GetSettings() const { return settings; }

	private:
		Settings settings{};
		Room sensorRoom = Room::Dark;
		Room candidateRoom = Room::Dark;
		double candidateSince = -1.0;
		double lastReading = 0.0;
		double lastLux = 0.0;
		bool haveReading = false;
		bool sensed = false;
	};
}
