# audio_probe — 厂商原版 codec 链路探针

这个 sketch 用 **Waveshare 自己的**音频栈驱动 ES7210
（`codec_bsp` → `codec_board` → `esp_codec_dev` → `i2c_bsp`，从
`02_Example/Arduino/07_Audio_Test` 原样拷来，只删掉了音乐和 LVGL UI 部分），
每 500 ms 打印一次采样峰值。

它只回答一个问题：当仪表盘的 SND 条看起来是死的时，**是仪表盘的 ES7210 集成写错了，
还是硬件的问题？**

```bash
source scripts/env.sh                      # 提供 $FQBN
tools/bin/arduino-cli compile --fqbn "$FQBN" --output-dir build/probe diagnostics/audio_probe
tools/bin/arduino-cli upload -p /dev/ttyACM0 --fqbn "$FQBN" --input-dir build/probe diagnostics/audio_probe

python3 host/serial_monitor.py -p /dev/ttyACM0
```

正常（安静房间）应该看到 `ret=0` 且 `peak` 在几十到几百（16-bit 计数）：

```
ret=0 peak=89  first=[29 39 28 20 28 20]
ret=0 peak=256 first=[-151 -146 -173 -118 -167 -109]
```

如果 `peak` 一直是 0–7，说明 mic 的信号没有进到 ADC。

**它就是定位 `firmware/rlcd_dash/audio_level.cpp` 那个 bug 的关键**：这个探针能读到信号、
而仪表盘读不到，直接指向了仪表盘漏掉了 `enable()` 之后的麦克风增益设置
（原因见 `audio_level.cpp` 里的注释）。以后音频再出问题，先跑它做二分。
