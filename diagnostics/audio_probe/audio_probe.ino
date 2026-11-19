// Diagnostic only: drive the ES7210 through the vendor's own stack
// (codec_bsp -> codec_board -> esp_codec_dev -> i2c_bsp), bypassing the
// dashboard's own audio code entirely
// in the path.  If this also reads digital silence, the fault is not in our
// ES7210 integration.
#include "codec_bsp.h"

static I2cMasterBus I2cbus(14, 13, 0);  // (scl, sda, port)
static CodecPort *codec = nullptr;
static int16_t buf[480];  // 10 ms of 24 kHz stereo 16-bit

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== vendor codec stack probe ===");

  codec = new CodecPort(I2cbus, "S3_RLCD_4_2");
  codec->CodecPort_SetSpeakerVol(80);
  codec->CodecPort_SetMicGain(25);
  codec->CodecPort_SetInfo("es8311 & es7210", 1, 24000, 2, 16);

  Serial.printf("es7210 reg12=0x%02X reg01=0x%02X reg40=0x%02X reg47=0x%02X\n",
                codec->Codec_GetCodecReg("es7210", 0x12), codec->Codec_GetCodecReg("es7210", 0x01),
                codec->Codec_GetCodecReg("es7210", 0x40), codec->Codec_GetCodecReg("es7210", 0x47));
  Serial.println("codec stack initialised");
}

void loop() {
  static uint32_t next = 0;
  if (millis() - next < 500) {
    delay(5);
    return;
  }
  next = millis();

  const int ret = codec->CodecPort_EchoRead(buf, sizeof(buf));
  int32_t peak = 0;
  for (size_t i = 0; i < sizeof(buf) / sizeof(buf[0]); i++) {
    const int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
    if (a > peak) peak = a;
  }
  Serial.printf("ret=%d peak=%ld first=[%d %d %d %d %d %d]\n", ret, (long)peak, buf[0], buf[1],
                buf[2], buf[3], buf[4], buf[5]);
}
