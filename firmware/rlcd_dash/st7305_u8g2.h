// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) Waveshare -- original example
// Modifications Copyright 2026 the rlcd-dashboard contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// See NOTICE at the repository root for what changed relative to upstream.

// Minimal ST7305 (300x400 reflective LCD) driver for the Waveshare
// ESP32-S3-RLCD-4.2, exposing a full u8g2 canvas in landscape (400x300).
//
// Derived from the vendor example
//   waveshareteam/ESP32-S3-RLCD-4.2 -> 02_Example/Arduino/10_U8G2_Test
// (ST7305_U8g2.{h,cpp}).  The panel RAM packs a 4(column) x 2(row) pixel block
// into one byte, MSB pair = left most column, upper bit of each pair = the upper
// row, so the u8g2 page buffer has to be repacked before it is pushed with the
// 0x2C memory write.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

// Default wiring of the ESP32-S3-RLCD-4.2.
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

#define RLCD_PANEL_W 300
#define RLCD_PANEL_H 400
#define RLCD_SCREEN_W 400  // landscape view width
#define RLCD_SCREEN_H 300  // landscape view height

class ST7305_U8g2 {
 public:
  ST7305_U8g2(int sck = RLCD_SCK_PIN, int mosi = RLCD_MOSI_PIN, int dc = RLCD_DC_PIN,
              int cs = RLCD_CS_PIN, int rst = RLCD_RST_PIN);
  ~ST7305_U8g2();

  // tile_buf_height == 0 -> one full screen buffer (~15 kB).
  bool begin(uint8_t tile_buf_height = 0, const u8g2_cb_t *rotation = U8G2_R1);
  void reset();
  void fullInit();

  // Swap the panel's black and white.  Implemented by inverting the packed byte
  // stream on the way out -- every bit is exactly one pixel -- so it costs one
  // XOR pass over 15 kB per frame, roughly 60 us on a 240 MHz core.
  void setInvert(bool invert) { _invert = invert; }
  bool invert() const { return _invert; }

  U8G2 *getU8g2() { return &u8g2_wrapper; }

 private:
  int _sck, _mosi, _dc, _cs, _rst;
  bool _invert = false;
  SPIClass *_spi = nullptr;
  U8G2 u8g2_wrapper;
  uint8_t *_my_buf = nullptr;

  void _cmd(uint8_t cmd);
  void _cmd_data(uint8_t cmd, const uint8_t *data, size_t len);

  static uint8_t u8x8_d_st7305_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
  static uint8_t u8x8_byte_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
};
