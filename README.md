# ColorfulBlue

基于 [m5stack/M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo) 的派生固件 —— 面向"日常海报 / 词云"展示场景，瘦身了一批不用的子系统、重排了前面板按键、把 dither 工作挪到浏览器端做，并加了一个 HTTP 拉图按键。

> Fork of M5Stack's `M5PaperColor-UserDemo`. The behavior diffs from upstream are documented below; build / flash steps are unchanged.

## 与上游的主要差异

### 1. Slim firmware — 删掉 ezdata + USB-MSC

- **EzData 在线照片推送**子系统整体拿掉（`apps/ezdata_photo_push` + `hal/ezdata`，约 2.7 kLoC + 云端 SDK 依赖）。本固件只走 captive Wi-Fi web portal 上传 + SD 卡复制。
- **USB-MSC**：宿主机不再能挂载 SPI flash 上的照片分区写文件。写操作只在 SD 卡上做，SPI flash 照片分区运行期只读 —— 避免设备和宿主机同时写同一 FAT 镜像导致的损坏。

合计减掉 ~3.5 kLoC、引导更快、二进制更小，也不再依赖 EzData / TinyUSB-MSC。

### 2. 前面板按键重排 + 每日图按键

| 按键 | 行为 |
|---|---|
| **A 短按** | 上一张 |
| **A 长按 5 s** | 重新启用 captive AP（保留上游 AP-recovery） |
| **B 短按** | 下一张 |
| **C 短按** | HTTP 拉一张"每日图"贴到面板，按顺序轮转 web 上配置的 URL 列表（最多 10 条，开机后第一次按显示第 1 条）。<br>URL 列表在 Settings → "Daily images (C button)" 里编辑，无默认值，需要先配置。 |
| **C 长按 ≥ 800 ms** | 双声 + 关机（PMIC powerOff + 可选 RTC 周期唤醒）。USB 接着时只播提示音、不关机，方便开发。 |

附带的小改动：
- 每次按键都有蜂鸣 ack（不论有没有 Wi-Fi，C 短按都先响一声）。
- 切张后前面板 LED 保持 AP-idle 绿，不再熜灭 —— A/B 短按看起来不像把设备关了。

实现细节：
- BtnC 用 `M5.BtnC.isPressed()` 边沿 + 手动计时，没用 M5Unified 的 `wasClicked()`/`wasHold()` 状态机，因为 `setHoldThresh()` 会同时改 click-count decision 窗口，把短按事件吃掉。
- 每日图 URL 列表存在 NVS 的 `papercolor` 命名空间（key 名 `daily_n` + `daily_0..daily_9`），通过 `GET/POST /api/daily-images` 读写。后端 schema 见 `main/hal/hal.h::Settings::daily_image_urls`。

### 3. 电源 / UX 微调

- **USB 连接时禁用所有自动关机路径**（包括低功耗 idle、长按关机），开发期不用先唤醒再 flash。
- **AP 空闲时 LED 常绿**：开机后不再在电纸屏上画 QR 码，绿灯就是"设备在线 + softAP 可连"的唯一指示。
- **Web 上传页**的双指捏合手势去掉 twist 角度，纯 pinch-zoom，更顺手。

### 4. Web 端 dither（Smart / Mix）

把吃力的"映射到面板 6 色油墨"搬到浏览器做，传给设备的就是已经量化好的 PNG，设备走 M5GFX 的 `epd_fastest` 直接 push，不再做二次抖动。

- **Smart（Atkinson）**：每像素挑距离最近的 6 色油墨，邻居感知核把残差往右下 / 下一行扩散。边缘锋利，适合高对比照片。
- **Mix（Floyd-Steinberg）**：用 6 色按 N 份比例预生成固定调色板（5 份 = 246 色 / 6 份 = 456 色 / 7 份 = 786 色，可选附加 6 纯色）。像素就近捕色后 FS 核扩散残差，渐变柔和，适合人像 / 天空。
- **Anti-run penalty**：加了一个小权重，避免低对比区域塌成一长串同一种油墨（在天空和肤色上明显）。
- 文件名前缀 `imageA*`（Atkinson）/ `imageS*`（FS）的图，设备端跳过 M5GFX 自带 dither，直推 `epd_fastest`。

## Build / Flash

需要 [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/index.html)。

```bash
git submodule update --init --recursive
idf.py build

# 推荐：只烧 app 分区。保留 NVS（Wi-Fi 凭据 + 每日图 URL 列表）+ storage 分区（用户照片）。
idf.py -p /dev/ttyACM0 app-flash

# 全量烧（会覆盖 storage 分区里的内置图，并写一遍 partition-table）：
idf.py -p /dev/ttyACM0 flash
```

调试：

```bash
idf.py -p /dev/ttyACM0 monitor
```

### 本地 provisioning 脚本（gitignored）

`scripts/local/flash.sh` 是本地辅助脚本：build → app-flash → 写 NVS（Wi-Fi SSID/密码 + 当前每日图 URL 列表），让刚 flash 完的设备直接连上 Wi-Fi 并带好 URL 列表，省去走 captive portal 配一遍的步骤。

整个 `scripts/local/` 目录在 `.gitignore` 里 —— 脚本和里头的明文凭据都不进库。第一次用：自己 `mkdir -p scripts/local && touch scripts/local/flash.sh`，照着上面 `Build / Flash` 段拼自己的版本即可（参考 NVS key 名：`wifi_ssid` / `wifi_pass` / `daily_n` / `daily_0..daily_9`，namespace `papercolor`，partition size `0x6000`）。

## Acknowledgments

- 上游：[m5stack/M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo)
- [M5GFX](https://github.com/m5stack/M5GFX) · [M5Unified](https://github.com/m5stack/M5Unified) · [M5PM1](https://github.com/m5stack/M5PM1)
- [esp_tinyusb](https://components.espressif.com/components?q=esp_tinyusb) · [qrcode](https://components.espressif.com/components/espressif/qrcode) · [mdns](https://components.espressif.com/components/espressif/mdns)

## License

[MIT](LICENSE)
