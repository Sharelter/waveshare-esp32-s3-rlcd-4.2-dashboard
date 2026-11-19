// SPDX-License-Identifier: Apache-2.0
#include "shtc3.h"

#include <Wire.h>

namespace {

const uint8_t kAddr = 0x70;

const uint16_t kReadId = 0xEFC8;
const uint16_t kSoftReset = 0x805D;
const uint16_t kSleep = 0xB098;
const uint16_t kWakeup = 0x3517;
// Measure, temperature first, clock stretching disabled (we poll instead).
const uint16_t kMeasTRh = 0x7866;

const uint32_t kWakeupMs = 2;    // datasheet: 240 us max
const uint32_t kMeasureMs = 20;  // datasheet: 12.1 ms max for T-first normal mode

// Sensirion CRC-8: polynomial 0x131, init 0xFF, no reflection.
uint8_t crc8(const uint8_t *data, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x131) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

}  // namespace

bool Shtc3::begin(int sda, int scl) {
  Wire.begin(sda, scl, SHTC3_I2C_HZ);
  // Never let a stuck bus hang the display for ever.
  Wire.setTimeOut(50);

  _cmd(kWakeup);
  delay(kWakeupMs);
  _cmd(kSoftReset);
  delay(20);

  uint8_t buf[3] = {0};
  _cmd(kReadId);
  if (_read(buf, 3) && crc8(buf, 2) == buf[2]) {
    _id = ((uint16_t)buf[0] << 8) | buf[1];
    _present = true;
  }
  _cmd(kSleep);

  _state = IDLE;
  _next_ms = millis();
  return _present;
}

void Shtc3::poll() {
  if (!_present) return;
  const uint32_t now = millis();

  switch (_state) {
    case IDLE:
      if ((int32_t)(now - _next_ms) < 0) return;
      if (!_cmd(kWakeup)) {
        _abort(now);
        return;
      }
      _state = WAKING;
      _state_ms = now;
      break;

    case WAKING:
      if (now - _state_ms < kWakeupMs) return;
      if (!_cmd(kMeasTRh)) {
        _abort(now);
        return;
      }
      _state = MEASURING;
      _state_ms = now;
      break;

    case MEASURING:
      if (now - _state_ms < kMeasureMs) return;
      _collect();
      break;
  }
}

void Shtc3::_collect() {
  const uint32_t now = millis();

  uint8_t b[6] = {0};
  const bool ok = _read(b, 6) && crc8(b, 2) == b[2] && crc8(b + 3, 2) == b[5];
  _cmd(kSleep);

  _state = IDLE;
  _next_ms = now + SHTC3_PERIOD_MS;

  if (!ok) {
    _errors++;
    return;
  }

  const uint16_t raw_t = ((uint16_t)b[0] << 8) | b[1];
  const uint16_t raw_rh = ((uint16_t)b[3] << 8) | b[4];
  _temp = 175.0f * (float)raw_t / 65536.0f - 45.0f + SHTC3_TEMP_OFFSET_C;
  _rh = 100.0f * (float)raw_rh / 65536.0f;
  _valid = true;
}

void Shtc3::_abort(uint32_t now) {
  _errors++;
  _cmd(kSleep);
  _state = IDLE;
  _next_ms = now + SHTC3_PERIOD_MS;
}

bool Shtc3::_cmd(uint16_t command) {
  Wire.beginTransmission(kAddr);
  Wire.write((uint8_t)(command >> 8));
  Wire.write((uint8_t)(command & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Shtc3::_read(uint8_t *buf, size_t n) {
  if (Wire.requestFrom((uint8_t)kAddr, (size_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}
