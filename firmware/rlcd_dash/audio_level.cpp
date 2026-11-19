// SPDX-License-Identifier: Apache-2.0
#include "audio_level.h"

#include <Wire.h>
#include <math.h>

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"

extern "C" {
#include "src/device/include/es7210_adc.h"
}

namespace {

// 7-bit I2C address of the ES7210 (es7210_adc.h carries the 8-bit form, 0x80).
const uint8_t kEs7210Addr = 0x40;

audio_codec_ctrl_if_t s_ctrl = {};
bool s_ctrl_open = false;

int ctrl_open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size) {
  (void)ctrl;
  (void)cfg;
  (void)cfg_size;
  // The bus itself is already up: the SHTC3 driver owns Wire on the same pins.
  s_ctrl_open = true;
  return ESP_CODEC_DEV_OK;
}

bool ctrl_is_open(const audio_codec_ctrl_if_t *ctrl) {
  (void)ctrl;
  return s_ctrl_open;
}

int ctrl_close(const audio_codec_ctrl_if_t *ctrl) {
  (void)ctrl;
  s_ctrl_open = false;
  return ESP_CODEC_DEV_OK;
}

int ctrl_write_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len) {
  (void)ctrl;
  Wire.beginTransmission(kEs7210Addr);
  for (int i = 0; i < reg_len; i++) Wire.write((uint8_t)(reg >> (8 * (reg_len - 1 - i))));
  for (int i = 0; i < data_len; i++) Wire.write(((const uint8_t *)data)[i]);
  return Wire.endTransmission() == 0 ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

int ctrl_read_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len) {
  (void)ctrl;
  Wire.beginTransmission(kEs7210Addr);
  for (int i = 0; i < reg_len; i++) Wire.write((uint8_t)(reg >> (8 * (reg_len - 1 - i))));
  if (Wire.endTransmission(false) != 0) return ESP_CODEC_DEV_READ_FAIL;
  if (Wire.requestFrom((uint8_t)kEs7210Addr, (size_t)data_len) != (size_t)data_len) {
    return ESP_CODEC_DEV_READ_FAIL;
  }
  for (int i = 0; i < data_len; i++) ((uint8_t *)data)[i] = (uint8_t)Wire.read();
  return ESP_CODEC_DEV_OK;
}

i2s_chan_handle_t s_rx = nullptr;
const audio_codec_if_t *s_codec = nullptr;

}  // namespace

bool AudioLevel::begin() {
  // ---- I2S master first: the ES7210 is a slave and needs the clocks running.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  if (i2s_new_channel(&chan_cfg, nullptr, &s_rx) != ESP_OK) {
    _error = "i2s_new_channel failed";
    return false;
  }

  // TDM with four 32-bit slots, which is what the vendor's codec_board sets up
  // for this board (I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(32, ..., slot_mask) with
  // total_slot = 4).  With four mics enabled the ES7210 drives TDM framing, so
  // the ESP32 side has to match or nothing lands on the data line.
  i2s_tdm_config_t tdm_cfg = {
      .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO,
          (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)),
      .gpio_cfg =
          {
              .mclk = (gpio_num_t)AUDIO_I2S_MCLK_PIN,
              .bclk = (gpio_num_t)AUDIO_I2S_BCLK_PIN,
              .ws = (gpio_num_t)AUDIO_I2S_WS_PIN,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)AUDIO_I2S_DIN_PIN,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  tdm_cfg.slot_cfg.total_slot = 4;
  // 24 kHz * 256 = 6.144 MHz MCLK, which is what the ES7210 expects in slave mode.
  tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_channel_init_tdm_mode(s_rx, &tdm_cfg) != ESP_OK) {
    _error = "i2s_channel_init_tdm_mode failed";
    return false;
  }
  if (i2s_channel_enable(s_rx) != ESP_OK) {
    _error = "i2s_channel_enable failed";
    return false;
  }

  // ---- ES7210 over the shared I2C bus.
  s_ctrl.open = ctrl_open;
  s_ctrl.is_open = ctrl_is_open;
  s_ctrl.read_reg = ctrl_read_reg;
  s_ctrl.write_reg = ctrl_write_reg;
  s_ctrl.close = ctrl_close;
  // es7210_codec_new() refuses to build unless the control interface is open.
  if (s_ctrl.open(&s_ctrl, nullptr, 0) != ESP_CODEC_DEV_OK) {
    _error = "ctrl_if open failed";
    return false;
  }

  es7210_codec_cfg_t cfg = {};
  cfg.ctrl_if = &s_ctrl;
  cfg.master_mode = false;  // ESP32 drives MCLK/BCLK/WS
  // All four inputs, exactly like the vendor's codec_init.c does when the board
  // is set up in TDM mode.  That matters: the ES7210 driver only switches the
  // chip into TDM output when 3+ mics are selected (ENABLE_TDM_MAX_NUM == 3),
  // and with just MIC1|MIC3 it stays in plain 2-channel mode and delivers
  // nothing but the ADC noise floor.
  cfg.mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4;
  cfg.mclk_src = ES7210_MCLK_FROM_PAD;
  cfg.mclk_div = 0;

  // es7210_codec_new() runs the whole open/init sequence itself, so do NOT call
  // open() again here -- a second pass just re-runs the register table.
  s_codec = es7210_codec_new(&cfg);
  if (s_codec == nullptr) {
    _error = "es7210_codec_new failed (no ACK on I2C 0x40?)";
    return false;
  }

  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 2;
  fs.channel_mask = 0;
  fs.sample_rate = AUDIO_SAMPLE_RATE;
  fs.mclk_multiple = 0;
  if (s_codec->set_fs(s_codec, &fs) != ESP_CODEC_DEV_OK) {
    _error = "es7210 set_fs failed";
    return false;
  }
  if (s_codec->enable(s_codec, true) != ESP_CODEC_DEV_OK) {
    _error = "es7210 enable failed";
    return false;
  }

  // Must come AFTER enable().  es7210_enable() -> es7210_start() ->
  // es7210_mic_select() rewrites the gain registers from `codec->gain`, and
  // that field is never assigned by the driver (calloc leaves it 0), so the
  // 30 dB set during open is silently replaced with 0 dB.  The vendor's demo
  // works for exactly one reason: it calls CodecPort_SetMicGain() after
  // opening, which lands here.
  if (s_codec->set_mic_gain(s_codec, AUDIO_MIC_GAIN_DB) != ESP_CODEC_DEV_OK) {
    _error = "es7210 set_mic_gain failed";
    return false;
  }

  // Read back MIC1's gain register to prove the set_mic_gain() above actually
  // landed (bits [3:0] hold the 0..13 gain code, bit 4 is the gain enable).
  {
    int v = 0;
    if (s_codec->get_reg(s_codec, 0x43, &v) == ESP_CODEC_DEV_OK) _gain_reg = (uint8_t)v;
  }

  _present = true;
  _error = "ok";
  _last_ms = millis();
  return true;
}

void AudioLevel::poll() {
  if (!_present) return;
  const uint32_t now = millis();
  if (now - _last_ms < AUDIO_PERIOD_MS) return;
  _last_ms = now;

  size_t got = 0;
  if (i2s_channel_read(s_rx, _buf, sizeof(_buf), &got, 30) != ESP_OK || got < sizeof(int32_t)) {
    _errors++;
    _error = "i2s read failed";
    return;
  }

  const size_t n = got / sizeof(int32_t);
  double acc = 0.0;
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    const double s = (double)_buf[i] / 2147483648.0;
    acc += s * s;
    const int32_t a = _buf[i] < 0 ? -_buf[i] : _buf[i];
    if (a > peak) peak = a;
  }
  _raw_peak = peak;
  const double rms = sqrt(acc / (double)n);
  const float db = (float)(20.0 * log10(rms + 1e-9));

  float norm = (db - AUDIO_FLOOR_DB) / (0.0f - AUDIO_FLOOR_DB);
  if (norm < 0.0f) norm = 0.0f;
  if (norm > 1.0f) norm = 1.0f;

  _level += 0.4f * (norm - _level);
  _peak = (norm > _peak) ? norm : _peak * 0.90f;
  _dbfs = db;
  _valid = true;
}
