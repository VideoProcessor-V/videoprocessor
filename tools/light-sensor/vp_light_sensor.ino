// VideoProcessor room light sensor (Stage 9b, docs/VP-stage9-ambient.md).
//
// An ESP32 or Arduino with a VEML7700 (default) or TSL2591 light sensor on
// I2C, connected to the HTPC by USB. It prints one reading per line,
// "lux=12.34", twice a second at 115200 baud. In VideoProcessor's Config:
// General > Room lights > Light sensor, Sensor port = the board's COM port
// (Device Manager > Ports), Sensor speed 115200. The projector keeps its own
// RS-232 port.
//
// Libraries (Arduino IDE > Library Manager): "Adafruit VEML7700" or
// "Adafruit TSL2591", each with "Adafruit Unified Sensor" / "Adafruit
// BusIO" as it asks.
//
// Wiring (I2C): sensor VIN to 3.3 V, GND to GND, SDA and SCL to the board's
// SDA / SCL pins (ESP32: GPIO 21 / 22; Arduino Uno: A4 / A5).
//
// Mounting: behind the seating, facing the ceiling, so it measures the room
// light and never sees the screen. Then read the lux values in VP's overlay
// with the room lights off and on, and set "Lit above" / "Dark below" in
// between (e.g. off 1 lux, on 60 lux: lit above 20, dark below 8).

#define VP_SENSOR_VEML7700 1   // 0: TSL2591

#include <Wire.h>
#if VP_SENSOR_VEML7700
#include <Adafruit_VEML7700.h>
Adafruit_VEML7700 sensor;
#else
#include <Adafruit_TSL2591.h>
Adafruit_TSL2591 sensor(2591);
#endif

const unsigned long PERIOD_MS = 500;
bool ready = false;

void setup()
{
  Serial.begin(115200);
  Wire.begin();
#if VP_SENSOR_VEML7700
  ready = sensor.begin();
#else
  ready = sensor.begin();
  if (ready)
  {
    sensor.setGain(TSL2591_GAIN_MED);
    sensor.setTiming(TSL2591_INTEGRATIONTIME_200MS);
  }
#endif
  // A banner line: VideoProcessor ignores it (not a reading).
  Serial.println(ready ? "# VP light sensor ready" : "# VP light sensor: sensor not found, check the wiring");
}

void loop()
{
  static unsigned long last = 0;
  const unsigned long now = millis();
  if (now - last < PERIOD_MS)
    return;
  last = now;
  if (!ready)
  {
    Serial.println("# sensor not found");
    return;
  }
#if VP_SENSOR_VEML7700
  const float lux = sensor.readLux(VEML_LUX_AUTO);   // automatic gain / integration
#else
  const uint32_t full = sensor.getFullLuminosity();
  const float lux = sensor.calculateLux(full & 0xFFFF, full >> 16);
#endif
  if (lux >= 0.0f && lux == lux)   // skip overflow / NaN readings
  {
    Serial.print("lux=");
    Serial.println(lux, 2);
  }
}
