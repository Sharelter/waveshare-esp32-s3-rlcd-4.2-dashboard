// SPDX-License-Identifier: Apache-2.0
// Minimal SHTC3 temperature/humidity driver for the ESP32-S3-RLCD-4.2.
//
// The sensor sits on the board's I2C bus (SDA = GPIO13, SCL = GPIO14) at
// address 0x70.  The command values below are the same ones the vendor's
// 05_I2C_SHTC3 example uses and match the Sensirion datasheet.
//
// Reading is a non-blocking state machine so a measurement never stalls the
// 10 Hz render loop: wake up, issue the "T first, no clock stretching"
// measurement, come back after the conversion time, then read 6 bytes.
#pragma once

#include <Arduino.h>

#define SHTC3_SDA_PIN 13
#define SHTC3_SCL_PIN 14
#define SHTC3_I2C_HZ 100000

// How often to sample.  Also keeps the part from self-heating.
#define SHTC3_PERIOD_MS 2000

// The vendor example subtracts 4 C from the reading to compensate for the
// ESP32's self-heating on this small board.  That is a board specific fudge
// factor we cannot verify here, so the plain datasheet formula is used and the
// knob is left visible for anyone who wants to match a reference thermometer.
#define SHTC3_TEMP_OFFSET_C 0.0f

class Shtc3 {
 public:
  /// Wakes the part and reads its ID.  Returns false if it does not answer.
  bool begin(int sda = SHTC3_SDA_PIN, int scl = SHTC3_SCL_PIN);

  /// Non-blocking; call from loop().
  void poll();

  bool present() const { return _present; }
  bool valid() const { return _valid; }
  float tempC() const { return _temp; }
  float rh() const { return _rh; }
  uint16_t id() const { return _id; }
  uint32_t errors() const { return _errors; }

 private:
  enum State : uint8_t { IDLE, WAKING, MEASURING };

  State _state = IDLE;
  uint32_t _state_ms = 0;
  uint32_t _next_ms = 0;
  bool _present = false;
  bool _valid = false;
  float _temp = 0.0f;
  float _rh = 0.0f;
  uint16_t _id = 0;
  uint32_t _errors = 0;

  bool _cmd(uint16_t command);
  bool _read(uint8_t *buf, size_t n);
  void _collect();
  void _abort(uint32_t now);
};
