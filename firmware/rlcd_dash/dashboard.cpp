// SPDX-License-Identifier: Apache-2.0
#include "dashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const uint8_t *FONT_LABEL = u8g2_font_7x14B_tf;
const uint8_t *FONT_VALUE = u8g2_font_10x20_tf;
const uint8_t *FONT_SMALL = u8g2_font_6x12_tf;
const uint8_t *FONT_TINY = u8g2_font_5x7_tf;

const int HIST_BINS = Dashboard::HIST_BINS;

void humanRate(uint32_t bps, char *out, size_t n) {
  if (bps >= 1024u * 1024u) {
    snprintf(out, n, "%.2f MB/s", bps / (1024.0 * 1024.0));
  } else if (bps >= 1024u) {
    snprintf(out, n, "%.0f KB/s", bps / 1024.0);
  } else {
    snprintf(out, n, "%u B/s", (unsigned)bps);
  }
}

void humanRateShort(uint32_t bps, char *out, size_t n) {
  if (bps >= 1024u * 1024u) {
    snprintf(out, n, "%.1fM", bps / (1024.0 * 1024.0));
  } else if (bps >= 1024u) {
    snprintf(out, n, "%.0fK", bps / 1024.0);
  } else {
    snprintf(out, n, "%uB", (unsigned)bps);
  }
}

// Round up to the next 1/2/5 * 10^k.
uint32_t niceCeil(uint32_t v) {
  if (v < 1024) return 1024;
  uint32_t p = 1;
  while (p * 10u <= v) p *= 10u;
  if (p >= v) return p;
  if (2u * p >= v) return 2u * p;
  if (5u * p >= v) return 5u * p;
  return 10u * p;
}

void fmtUptime(uint32_t s, char *out, size_t n) {
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d)
    snprintf(out, n, "up %ud %02uh", (unsigned)d, (unsigned)h);
  else if (h)
    snprintf(out, n, "up %uh %02um", (unsigned)h, (unsigned)m);
  else
    snprintf(out, n, "up %um", (unsigned)m);
}

}  // namespace

void Dashboard::begin(U8G2 *g) {
  _g = g;
  memset(&_d, 0, sizeof(_d));
  _rx_scale = _tx_scale = 64u * 1024u;
  _head = 0;
  _last_bin_ms = millis();
}

int Dashboard::_textWidth(const char *s) { return (int)_g->getStrWidth(s); }

void Dashboard::_drawTextRight(int x_right, int y, const char *s) {
  _g->drawStr(x_right - _textWidth(s), y, s);
}

void Dashboard::onFrame(const dash_payload_t &p) {
  _d = p;
  _have_data = true;
  _last_frame_ms = millis();

  if (p.niface) {
    if (p.iface[0].rx_bps > _rx_bin_max) _rx_bin_max = p.iface[0].rx_bps;
    if (p.iface[0].tx_bps > _tx_bin_max) _tx_bin_max = p.iface[0].tx_bps;
  }

  // Push one graph column every BIN_PERIOD_MS of wall clock, so the 96 column
  // window always spans ~48 s no matter what rate the host streams at.
  uint32_t now = millis();
  if (now - _last_bin_ms >= BIN_PERIOD_MS) {
    _last_bin_ms = now;
    _pushHistory();
  }
}

void Dashboard::_pushHistory() {
  _rx_hist[_head] = _rx_bin_max;
  _tx_hist[_head] = _tx_bin_max;
  _head = (_head + 1) % HIST_BINS;
  _rx_bin_max = _tx_bin_max = 0;

  uint32_t rmax = 0, tmax = 0;
  for (int i = 0; i < HIST_BINS; i++) {
    if (_rx_hist[i] > rmax) rmax = _rx_hist[i];
    if (_tx_hist[i] > tmax) tmax = _tx_hist[i];
  }
  uint32_t rneed = niceCeil((uint32_t)(rmax * 1.10f) + 1);
  uint32_t tneed = niceCeil((uint32_t)(tmax * 1.10f) + 1);

  // Grow at once, shrink gently, so the graph does not breathe.
  _rx_scale = (rneed > _rx_scale) ? rneed : _rx_scale - (_rx_scale - rneed) / 6;
  _tx_scale = (tneed > _tx_scale) ? tneed : _tx_scale - (_tx_scale - tneed) / 6;
  if (_rx_scale < 1024) _rx_scale = 1024;
  if (_tx_scale < 1024) _tx_scale = 1024;
}

void Dashboard::_gauge(int top, const char *label, const char *value, const char *detail,
                       float pct, bool warn) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  _g->setFont(FONT_LABEL);
  _g->drawStr(8, top + 14, label);

  _g->setFont(FONT_SMALL);
  _g->drawStr(64, top + 13, detail);

  _g->setFont(FONT_VALUE);
  _drawTextRight(394, top + 18, value);

  const int bx = 6, by = top + 22, bw = 388, bh = 16;
  _g->drawFrame(bx, by, bw, bh);
  int fill = (int)((bw - 4) * pct / 100.0f + 0.5f);
  if (fill > 0) {
    if (warn) {
      for (int x = 0; x < fill; x += 3) _g->drawBox(bx + 2 + x, by + 2, 2, bh - 4);
    } else {
      _g->drawBox(bx + 2, by + 2, fill, bh - 4);
    }
  }
}

void Dashboard::_graphBand(int y, int h, const uint32_t *hist, uint32_t scale, const char *tag) {
  const int x0 = 6, pitch = 4, barw = 3;

  for (int i = 0; i < HIST_BINS; i++) {
    uint32_t v = hist[(_head + i) % HIST_BINS];
    if (!v) continue;
    int bh = (int)((uint64_t)v * h / scale);
    if (bh < 1) bh = 1;
    if (bh > h) bh = h;
    _g->drawBox(x0 + i * pitch, y + h - bh, barw, bh);
  }
  _g->drawHLine(x0, y + h, HIST_BINS * pitch - 1);

  // tag on a white patch so it stays readable on top of the bars
  _g->setFont(FONT_TINY);
  _g->setDrawColor(0);
  _g->drawBox(x0 + 2, y + 1, 16, 8);
  _g->setDrawColor(1);
  _g->drawStr(x0 + 3, y + 8, tag);

  char buf[24];
  humanRateShort(scale, buf, sizeof(buf));
  _drawTextRight(x0 + HIST_BINS * pitch - 2, y + 8, buf);
}

void Dashboard::setBoardEnv(float temp_c, float rh, bool valid) {
  _env_valid = valid;
  _env_temp = temp_c;
  _env_rh = rh;
}

void Dashboard::setSoundLevel(float level, float peak, float db, bool valid) {
  _snd_valid = valid;
  _snd_level = level;
  _snd_peak = peak;
  _snd_db = db;
}

void Dashboard::render(bool link_ok) {
  U8G2 *g = _g;
  char buf[64];

  g->clearBuffer();
  g->setDrawColor(1);

  // ---------------------------------------------------------------- header
  g->setFont(FONT_LABEL);
  g->drawStr(8, 18, _have_data ? _d.host : "rlcd-dash");

  if (_have_data) {
    char up[24];
    fmtUptime(_d.uptime_s, up, sizeof(up));
    g->setFont(FONT_SMALL);
    g->drawStr(246, 15, up);
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)_d.hour, (unsigned)_d.minute,
             (unsigned)_d.second);
  } else {
    snprintf(buf, sizeof(buf), "--:--:--");
  }
  g->setFont(FONT_LABEL);
  _drawTextRight(394, 18, buf);

  g->drawHLine(6, 23, 388);

  // ---------------------------------------------------------------- gauges
  float cpu = _have_data ? _d.cpu_total / 10.0f : 0.0f;
  float mem = (_have_data && _d.mem_total_mb) ? 100.0f * _d.mem_used_mb / _d.mem_total_mb : 0.0f;
  float swp =
      (_have_data && _d.swap_total_mb) ? 100.0f * _d.swap_used_mb / _d.swap_total_mb : 0.0f;

  snprintf(buf, sizeof(buf), "%.1f%%", cpu);
  char detail[40];
  snprintf(detail, sizeof(detail), "%u threads", (unsigned)(_have_data ? _d.ncpu : 0));
  _gauge(28, "CPU", buf, detail, cpu, cpu >= 90.0f);

  snprintf(buf, sizeof(buf), "%.1f%%", mem);
  snprintf(detail, sizeof(detail), "%.1f/%.1f GB", _d.mem_used_mb / 1024.0,
           _d.mem_total_mb / 1024.0);
  _gauge(70, "MEM", buf, detail, mem, mem >= 90.0f);

  snprintf(buf, sizeof(buf), "%.1f%%", swp);
  if (_d.swap_total_mb)
    snprintf(detail, sizeof(detail), "%.1f/%.1f GB", _d.swap_used_mb / 1024.0,
             _d.swap_total_mb / 1024.0);
  else
    snprintf(detail, sizeof(detail), "not configured");
  _gauge(112, "SWAP", buf, detail, swp, swp >= 50.0f);

  g->drawHLine(6, 152, 388);

  // ---------------------------------------------------------------- network
  g->setFont(FONT_LABEL);
  g->drawStr(8, 170, "NET");

  g->setFont(FONT_SMALL);
  if (_have_data && _d.niface) {
    char nm[DASH_IFNAME_LEN + 1];
    memcpy(nm, _d.iface[0].name, DASH_IFNAME_LEN);
    nm[DASH_IFNAME_LEN] = 0;
    g->drawStr(48, 169, nm);

    char rx[24], tx[24];
    humanRate(_d.iface[0].rx_bps, rx, sizeof(rx));
    humanRate(_d.iface[0].tx_bps, tx, sizeof(tx));
    snprintf(buf, sizeof(buf), "down %s   up %s", rx, tx);
    _drawTextRight(394, 169, buf);
  } else {
    g->drawStr(48, 169, "no interface");
  }

  g->drawFrame(5, 174, 390, 72);
  if (!link_ok) {
    g->setFont(FONT_VALUE);
    const char *msg = "NO HOST SIGNAL";
    g->drawStr((DASH_W - _textWidth(msg)) / 2, 220, msg);
  } else {
    _graphBand(176, 32, _rx_hist, _rx_scale, "RX");
    _graphBand(212, 32, _tx_hist, _tx_scale, "TX");
  }

  g->drawHLine(6, 250, 388);

  // ------------------------------------------------------------- cpu cores
  const int nx = 6, ny = 256, nw = 256, nh = 36;
  const int slots = 16;
  uint16_t ncpu = _have_data ? _d.ncpu : 0;
  for (int i = 0; i < slots; i++) {
    int pitch = nw / slots;
    int x = nx + i * pitch;
    g->drawFrame(x, ny, pitch - 2, nh);
    if (!ncpu) continue;
    int shown = ncpu < slots ? ncpu : slots;
    if (i >= shown) continue;
    int v;
    if (ncpu <= slots) {
      v = _d.cpu_load[i];
    } else {
      int lo = (int)((long)i * ncpu / slots);
      int hi = (int)((long)(i + 1) * ncpu / slots);
      v = 0;
      for (int k = lo; k < hi && k < DASH_MAX_CPUS; k++)
        if (_d.cpu_load[k] > v) v = _d.cpu_load[k];
    }
    if (v > 100) v = 100;
    int bh = (nh - 4) * v / 100;
    if (bh > 0) g->drawBox(x + 2, ny + nh - 2 - bh, pitch - 6 > 0 ? pitch - 6 : 1, bh);
  }

  // No loadavg here: the instantaneous total and the per-core bars already
  // cover CPU pressure, so the space goes to the two thermometers plus a
  // vertical sound-level bar (a meter wants height, so it reads better standing
  // up next to the core bars than as another line of text).
  // Three short lines stacked, top to bottom, so the text column stays narrow
  // enough to leave the whole right-hand side to the sound bar.
  g->setFont(FONT_SMALL);
  if (_have_data && _d.temp_x10 != DASH_NO_TEMP)
    snprintf(buf, sizeof(buf), "CPU %.1fC", _d.temp_x10 / 10.0f);
  else
    snprintf(buf, sizeof(buf), "CPU --");
  g->drawStr(266, 266, buf);

  // The board's own SHTC3 sits right next to the ESP32, so it reads a few
  // degrees above ambient unless SHTC3_TEMP_OFFSET_C compensates.
  if (_env_valid) {
    snprintf(buf, sizeof(buf), "ENV %.1fC", _env_temp);
    g->drawStr(266, 278, buf);
    snprintf(buf, sizeof(buf), "RH  %.0f%%", _env_rh);
  } else {
    g->drawStr(266, 278, "ENV --");
    snprintf(buf, sizeof(buf), "RH  --");
  }
  g->drawStr(266, 290, buf);

  {
    // Bar sits on the CPU line; its label is centred directly underneath.
    const int bx = 328, by = 256, bw = 66, bh = 14;
    g->drawFrame(bx, by, bw, bh);
    if (_snd_valid) {
      const int inner = bw - 4;
      const int fill = (int)(inner * _snd_level + 0.5f);
      if (fill > 0) g->drawBox(bx + 2, by + 2, fill, bh - 4);
      // Peak hold: notch the fill in the background colour so it stays visible
      // in both themes.
      int pk = (int)(inner * _snd_peak + 0.5f);
      if (pk > inner - 2) pk = inner - 2;
      if (pk > 1) {
        g->setDrawColor(0);
        g->drawBox(bx + 2 + pk, by + 2, 2, bh - 4);
        g->setDrawColor(1);
      }
      // dB rather than a percentage: the bar is already a level, and dB is what
      // a sound level actually is (dBFS here -- 0 dB = digital full scale).
      int db = (int)lroundf(_snd_db);
      if (db < -70) db = -70;
      if (db > 0) db = 0;
      snprintf(buf, sizeof(buf), "SND %d dB", db);
    } else {
      snprintf(buf, sizeof(buf), "SND  --");
    }
    g->drawStr(bx + (bw - _textWidth(buf)) / 2, 282, buf);
  }

  g->sendBuffer();
  _frames++;
}
