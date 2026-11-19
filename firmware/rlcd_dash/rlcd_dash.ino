// SPDX-License-Identifier: Apache-2.0
// rlcd_dash - host load dashboard for the Waveshare ESP32-S3-RLCD-4.2
//
// The host samples CPU / memory / network and pushes a fixed size binary frame
// over the native USB serial (USB-Serial-JTAG) at 30 Hz.  This sketch parses the
// frames and repaints the 300x400 reflective LCD at 30 Hz.
//
// Build: see scripts/build.sh      Flash: see scripts/flash.sh
#include <Preferences.h>
#include <string.h>

#include "audio_level.h"
#include "dashboard.h"
#include "protocol.h"
#include "shtc3.h"
#include "st7305_u8g2.h"

// Bumped for tagged releases; printed in the boot banner so a user can tell
// which build is actually on the board.
#define RLCD_DASH_VERSION "1.0.0"

// The board's KEY button: pulled up, shorts to GND when pressed.
#define KEY_PIN 18
#define KEY_DEBOUNCE_MS 30

static ST7305_U8g2 lcd;
static Dashboard dash;
static Shtc3 board_env;
static AudioLevel sound;
static U8G2 *g = nullptr;

static dash_payload_t payload;
static bool have_payload = false;
static uint32_t last_rx_ms = 0;
static uint32_t frames_rx = 0;

// ------------------------------------------------------------------ theme ---
// true  = dark ink on a light panel (white background) -- the default, because a
//         reflective panel does not emit light and a light background reflects
//         far more of the ambient light.
// false = light ink on a dark panel.
// Persisted in NVS so a KEY press survives a power cycle.
static Preferences prefs;
static bool invert_mode = true;
static bool force_repaint = false;

static bool key_raw = false;
static bool key_stable = false;
static uint32_t key_changed_ms = 0;

static void toggleTheme() {
  invert_mode = !invert_mode;
  lcd.setInvert(invert_mode);
  force_repaint = true;

  prefs.begin("rlcd", false);
  prefs.putBool("invert", invert_mode);
  prefs.end();

  Serial.printf("KEY: theme=%s\r\n", invert_mode ? "light (black on white)" : "dark (white on black)");
}

static void pollKey() {
  const bool raw = digitalRead(KEY_PIN) == LOW;
  const uint32_t now = millis();

  if (raw != key_raw) {
    key_raw = raw;
    key_changed_ms = now;
    return;  // still bouncing
  }
  if (now - key_changed_ms < KEY_DEBOUNCE_MS) return;
  if (raw == key_stable) return;

  key_stable = raw;
  if (key_stable) toggleTheme();  // debounced press edge
}

// ------------------------------------------------------------------ rx parser
static uint8_t rxbuf[800];
static size_t rxlen = 0;

// Returns the number of complete, CRC-valid frames consumed.
static uint32_t pollSerial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (rxlen < sizeof(rxbuf)) {
      rxbuf[rxlen++] = (uint8_t)c;
    } else {
      // buffer full without a valid frame: drop the oldest byte
      memmove(rxbuf, rxbuf + 1, sizeof(rxbuf) - 1);
      rxbuf[sizeof(rxbuf) - 1] = (uint8_t)c;
    }
  }

  uint32_t got = 0;
  size_t i = 0;
  while (rxlen - i >= 5) {
    if (rxbuf[i] != DASH_MAGIC0 || rxbuf[i + 1] != DASH_MAGIC1) {
      i++;
      continue;
    }
    uint8_t len = rxbuf[i + 2];
    if (len != DASH_PAYLOAD_LEN) {
      i++;
      continue;
    }
    size_t need = (size_t)3 + len + 2;
    if (rxlen - i < need) break;

    uint16_t want = (uint16_t)rxbuf[i + 3 + len] | ((uint16_t)rxbuf[i + 4 + len] << 8);
    if (dash_crc16(&rxbuf[i + 3], len) == want) {
      memcpy(&payload, &rxbuf[i + 3], len);
      have_payload = true;
      last_rx_ms = millis();
      frames_rx++;
      got++;
      i += need;
    } else {
      i++;
    }
  }
  if (i > 0) {
    memmove(rxbuf, rxbuf + i, rxlen - i);
    rxlen -= i;
  }
  return got;
}

// ------------------------------------------------------------------ rendering
// Idle repaint cadence, used only when the host link goes quiet.  The live
// path repaints as soon as frames arrive, so this does not cap the host rate.
static const uint32_t FRAME_PERIOD_US = 100000;  // 10 Hz
static const uint32_t FRAME_PERIOD_MS = FRAME_PERIOD_US / 1000;
static uint32_t next_frame_us = 0;

static uint32_t fps_window_ms = 0;
static uint32_t fps_frames = 0;
static float fps_value = 0.0f;
static uint32_t rx_per_sec = 0;

// Status reporting is a slow heartbeat plus an immediate line whenever
// something actually changes. One line per second is not worth the journal
// space when the answer is always the same.
static const uint32_t STAT_PERIOD_MS = 60000;
static uint32_t stat_last_ms = 0;
static bool stat_link_prev = false;
static uint32_t stat_err_env_prev = 0;
static uint32_t stat_err_snd_prev = 0;
static bool stat_first = true;

static void reportStatus(bool link_ok, uint32_t now_ms) {
  const uint32_t err_env = board_env.errors();
  const uint32_t err_snd = sound.errors();
  const bool link_changed = stat_first || link_ok != stat_link_prev;
  const bool err_changed = err_env != stat_err_env_prev || err_snd != stat_err_snd_prev;
  const bool heartbeat = stat_first || (now_ms - stat_last_ms) >= STAT_PERIOD_MS;
  if (!link_changed && !err_changed && !heartbeat) return;

  Serial.printf("STAT fps=%.1f rx=%lu/s heap=%u link=%d env=%.1fC/%.0f%% snd=%.1fdB/%.0f%% err=%lu/%lu%s%s\r\n",
                fps_value, (unsigned long)rx_per_sec, (unsigned)ESP.getFreeHeap(),
                link_ok ? 1 : 0, board_env.tempC(), board_env.rh(), sound.dbfs(),
                sound.level() * 100.0f, (unsigned long)err_env, (unsigned long)err_snd,
                link_changed && !stat_first ? " link-change" : "",
                err_changed ? " err-change" : "");

  stat_last_ms = now_ms;
  stat_link_prev = link_ok;
  stat_err_env_prev = err_env;
  stat_err_snd_prev = err_snd;
  stat_first = false;
}

static void splash() {
  g->clearBuffer();
  g->setDrawColor(1);
  g->setFont(u8g2_font_7x14B_tf);
  const char *t = "ESP32-S3 RLCD dashboard";
  g->drawStr((DASH_W - g->getStrWidth(t)) / 2, 120, t);
  g->setFont(u8g2_font_6x12_tf);
  const char *s = "waiting for host link on USB serial...";
  g->drawStr((DASH_W - g->getStrWidth(s)) / 2, 150, s);
  g->drawFrame(20, 170, 360, 60);
  g->sendBuffer();
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Never let a missing reader stall the render loop.
  Serial.setTxTimeoutMs(0);
#endif

  bool ok = lcd.begin(0, U8G2_R1);
  g = lcd.getU8g2();
  dash.begin(g);

  pinMode(KEY_PIN, INPUT_PULLUP);
  key_raw = digitalRead(KEY_PIN) == LOW;
  key_stable = key_raw;
  key_changed_ms = millis();

  prefs.begin("rlcd", true);
  invert_mode = prefs.getBool("invert", true);
  prefs.end();
  lcd.setInvert(invert_mode);

  const bool have_env = board_env.begin();
  dash.setBoardEnv(0.0f, 0.0f, false);

  // The ES7210 needs I2C (shared with the SHTC3) plus the I2S clocks, so it has
  // to come after board_env.begin() brought Wire up.
  const bool have_sound = sound.begin();
  dash.setSoundLevel(0.0f, 0.0f, -70.0f, false);

  splash();

  next_frame_us = micros();
  fps_window_ms = millis();

  Serial.println();
  Serial.printf("rlcd_dash %s ready  panel=%dx%d  payload=%u bytes  frame=%u bytes  theme=%s\r\n",
                RLCD_DASH_VERSION, RLCD_PANEL_W, RLCD_PANEL_H, (unsigned)DASH_PAYLOAD_LEN,
                (unsigned)DASH_FRAME_LEN, invert_mode ? "light" : "dark");
  Serial.printf("KEY (GPIO%d) toggles black-on-white / white-on-black\r\n", KEY_PIN);
  Serial.printf("SHTC3 %s  id=0x%04X  i2c sda=%d scl=%d\r\n", have_env ? "ok" : "NOT FOUND",
                board_env.id(), SHTC3_SDA_PIN, SHTC3_SCL_PIN);
  Serial.printf("ES7210 %s  i2s mclk=%d bclk=%d ws=%d din=%d @%dHz gain_reg43=0x%02X%s%s\r\n",
                have_sound ? "ok" : "NOT FOUND", AUDIO_I2S_MCLK_PIN, AUDIO_I2S_BCLK_PIN,
                AUDIO_I2S_WS_PIN, AUDIO_I2S_DIN_PIN, AUDIO_SAMPLE_RATE, sound.gainReg(),
                have_sound ? "" : " -- ", have_sound ? "" : sound.errorText());
  if (!ok) Serial.println("!! display buffer allocation failed");
  delay(20);
}

void loop() {
  pollKey();
  board_env.poll();
  sound.poll();

  uint32_t got = pollSerial();
  uint32_t now_us = micros();

  bool repaint = false;
  if (got) {
    // Repaint as soon as a batch of host frames lands: this is what keeps the
    // link latency low.
    dash.onFrame(payload);
    repaint = true;
  } else if (force_repaint) {
    repaint = true;  // key press: show the new theme immediately
  } else if ((int32_t)(now_us - next_frame_us) >= 0) {
    // Idle repaint -- but while a stream is live the frame path already keeps
    // the panel fresh, so do not double up on it.
    if (!(have_payload && (millis() - last_rx_ms) < FRAME_PERIOD_MS * 3 / 2)) repaint = true;
  }
  if (!repaint) return;

  force_repaint = false;
  next_frame_us = now_us + FRAME_PERIOD_US;

  uint32_t now_ms = millis();
  bool link_ok = have_payload && (now_ms - last_rx_ms) < 1500;

  dash.setBoardEnv(board_env.tempC(), board_env.rh(), board_env.valid());
  dash.setSoundLevel(sound.level(), sound.peak(), sound.smoothedDb(), sound.valid());
  dash.render(link_ok);

  fps_frames++;
  if (now_ms - fps_window_ms >= 1000) {
    const uint32_t dt = now_ms - fps_window_ms;
    fps_value = fps_frames * 1000.0f / (float)dt;
    rx_per_sec = frames_rx;
    frames_rx = 0;
    fps_frames = 0;
    fps_window_ms = now_ms;
  }

  reportStatus(link_ok, now_ms);
}
