// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <U8g2lib.h>

#include "protocol.h"

// Drawing surface is 400x300 landscape (u8g2 rotation R1).
#define DASH_W 400
#define DASH_H 300

class Dashboard {
 public:
  static const int HIST_BINS = 96;
  static const uint32_t BIN_PERIOD_MS = 500;  // one graph column per 0.5 s

  void begin(U8G2 *g);
  void onFrame(const dash_payload_t &p);                 // a fresh host frame arrived
  void setBoardEnv(float temp_c, float rh, bool valid);   // on-board SHTC3 reading
  void setSoundLevel(float level, float peak, float db, bool valid);  // ES7210 mic array
  void render(bool link_ok);                             // repaint + push to the panel

  uint32_t frames() const { return _frames; }

 private:
  U8G2 *_g = nullptr;
  dash_payload_t _d = {};
  bool _have_data = false;

  bool _env_valid = false;
  float _env_temp = 0.0f;
  float _env_rh = 0.0f;

  bool _snd_valid = false;
  float _snd_level = 0.0f;
  float _snd_peak = 0.0f;
  float _snd_db = -70.0f;

  uint32_t _rx_hist[HIST_BINS] = {};
  uint32_t _tx_hist[HIST_BINS] = {};
  uint32_t _rx_scale = 64u * 1024u, _tx_scale = 64u * 1024u;
  uint32_t _rx_bin_max = 0, _tx_bin_max = 0;
  uint32_t _last_bin_ms = 0;
  int _head = 0;

  uint32_t _frames = 0;
  uint32_t _last_frame_ms = 0;

  void _pushHistory();
  void _gauge(int top, const char *label, const char *value, const char *detail, float pct,
              bool warn);
  void _graphBand(int y, int h, const uint32_t *hist, uint32_t scale, const char *tag);
  void _drawTextRight(int x_right, int y, const char *s);
  int _textWidth(const char *s);
};
