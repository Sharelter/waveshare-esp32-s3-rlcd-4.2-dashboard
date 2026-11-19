// SPDX-License-Identifier: Apache-2.0
// Ambient sound level from the board's ES7210 microphone array.
//
// Signal path (from the vendor's board config, `board_cfg.h` in codec_board):
//   ES7210 ADC (I2C 0x40) -> I2S, MCLK=16 BCLK=9 WS=45 DIN=10 -> ESP32-S3
// The ES7210 is the I2S *slave*; the ESP32 is the master and supplies the clocks.
//
// We drive the codec through Espressif's own `esp_codec_dev` ES7210 driver
// (vendored, see firmware/libraries/esp_codec_dev) so the register sequence is
// exactly the one the vendor demo uses.  Only the I2C transport is ours: a
// small `audio_codec_ctrl_if_t` over Arduino `Wire`, so the ES7210 shares the
// bus with the SHTC3 instead of opening a second driver on pins 13/14.
#pragma once

#include <Arduino.h>

#define AUDIO_I2S_MCLK_PIN 16
#define AUDIO_I2S_BCLK_PIN 9
#define AUDIO_I2S_WS_PIN 45
#define AUDIO_I2S_DIN_PIN 10  // ES7210 -> ESP32 (the ES8311 output is unused here)

#define AUDIO_SAMPLE_RATE 24000
// The vendor's I2S config uses 32-bit slots (I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(32, ...)),
// so capture 32-bit words and pick the meaningful bits out afterwards.
#define AUDIO_SLOT_BITS 32
#define AUDIO_BLOCK_MS 10       // length of one measured window
#define AUDIO_PERIOD_MS 200     // how often to measure
#define AUDIO_FLOOR_DB (-70.0f) // bottom of the displayed range, dBFS
                                 // (a quiet room measures about -59 dBFS here)
// Applied after enable(); the driver's own open-time gain is clobbered (see .cpp).
#define AUDIO_MIC_GAIN_DB 30.0f

class AudioLevel {
 public:
  bool begin();
  void poll();  // non-blocking-ish; call from loop()

  bool present() const { return _present; }
  bool valid() const { return _valid; }
  /// Smoothed level, 0..1 over AUDIO_FLOOR_DB..0 dBFS.
  float level() const { return _level; }
  /// Peak hold, decays between measurements.
  float peak() const { return _peak; }
  float dbfs() const { return _dbfs; }
  /// The smoothed level expressed back in dBFS, so the number on screen always
  /// agrees with the length of the bar next to it.
  float smoothedDb() const { return AUDIO_FLOOR_DB + _level * (0.0f - AUDIO_FLOOR_DB); }
  /// Raw peak of the last block (16-bit counts, i.e. high half of the 32-bit
  /// slot).  Handy when the meter looks dead: 0-7 means no analogue signal.
  int32_t rawPeak() const { return _raw_peak; }
  /// MIC1 gain register after init; 0x1A means gain-enabled + 30 dB.
  uint8_t gainReg() const { return _gain_reg; }
  uint32_t errors() const { return _errors; }
  const char *errorText() const { return _error; }

 private:
  static const int BLOCK_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_BLOCK_MS / 1000;  // frames
  static const int BLOCK_INTS = BLOCK_SAMPLES * 2;                             // stereo

  int32_t _buf[BLOCK_INTS] = {};
  bool _present = false;
  bool _valid = false;
  float _level = 0.0f;
  float _peak = 0.0f;
  float _dbfs = AUDIO_FLOOR_DB;
  uint32_t _errors = 0;
  uint32_t _last_ms = 0;
  int32_t _raw_peak = 0;
  uint8_t _gain_reg = 0;
  const char *_error = "not started";
};
