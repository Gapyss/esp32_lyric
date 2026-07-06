# Waveshare ESP32-S3-RLCD-4.2 — Board Spec

> AIoT development board built around an ESP32-S3-WROOM-1 (N16R8) driving a
> 4.2" reflective monochrome LCD, with a dual-mic audio front-end, RTC,
> environmental sensor, and microSD.

- **Connected as:** `/dev/cu.usbmodem11201` (USB-Serial/JTAG, native — no external UART chip)
- **Verified:** chip + flash read live with `esptool` v5.3.1 on 2026-07-03
- **Product page:** https://www.waveshare.com/esp32-s3-rlcd-4.2.htm
- **Docs:** https://docs.waveshare.com/ESP32-S3-RLCD-4.2
- **Official code + firmware:** https://github.com/waveshareteam/ESP32-S3-RLCD-4.2

---

## SoC / Module

| Property | Value |
|---|---|
| Module | ESP32-S3-WROOM-1 **N16R8** |
| Chip | ESP32-S3 (QFN56), revision **v0.2** |
| Cores | Dual-core Xtensa LX7 @ 240 MHz + LP core |
| Wireless | Wi-Fi 4 (2.4 GHz b/g/n), Bluetooth 5 LE |
| Crystal | 40 MHz |
| USB | Native USB-Serial/JTAG (VID:PID `303A:1001`) |
| Base MAC | `94:A9:90:CD:53:6C` |

### Memory (read via esptool)

| | Value |
|---|---|
| Flash | **16 MB** quad SPI, 3.3 V (GigaDevice `0x46:0x4018`, GD25Q128) |
| PSRAM | **8 MB** octal (OPI), embedded, 3.3 V |

---

## Display

| Property | Value |
|---|---|
| Type | 4.2" **Reflective LCD (RLCD)** — memory-in-pixel, no backlight, ambient-reflective, e-paper-like |
| Driver IC | **ST7305** |
| Resolution | **400 × 300**, 1-bit black & white |
| Interface | SPI (SPI3_HOST), 10 MHz, mode 0, 8-bit cmd/param |

The ST7305 is a monochrome memory-in-pixel controller (NOT e-paper): far faster
refresh than e-ink at very low power. Framebuffer is only ~15 KB (400×300×1bit).

---

## Pin Map

> GPIO assignments extracted from Waveshare's official Arduino demos
> (`02_Example/Arduino/*`): `user_config.h`, `07_Audio_Test.ino`,
> `06_SD_Card`, `03_ADC_Test`.

### Display — ST7305 (SPI)

| Signal | GPIO |
|---|---|
| MOSI / SDA | **GPIO12** |
| SCK / SCL  | **GPIO11** |
| DC         | **GPIO5**  |
| CS         | **GPIO40** |
| RST        | **GPIO41** |
| TE         | **GPIO6**  |

### I2C bus (port 0, 400 kHz, internal pull-ups)

| Signal | GPIO |
|---|---|
| SDA | **GPIO13** |
| SCL | **GPIO14** |

Devices on this bus:
- **PCF85063** — RTC
- **SHTC3** — temperature / humidity sensor
- **ES8311** — audio codec (control)
- **ES7210** — mic-array ADC (control)

### microSD / TF card (SDMMC, 1-bit mode)

| Signal | GPIO |
|---|---|
| CLK | **GPIO38** |
| CMD | **GPIO21** |
| D0  | **GPIO39** |

### ADC — battery voltage

| Signal | GPIO |
|---|---|
| VBAT sense | **GPIO4** (ADC1_CH3, 12-bit, 12 dB atten, ~÷3 divider) |

### Audio

- **ES8311** codec (speaker/DAC) + **ES7210** dual-mic array ADC
- Codec control over the shared I2C bus (GPIO13/14)
- I2S mode: **TDM**, 16 kHz / 16-bit in the reference firmware
- I2S pins from Waveshare `codec_board` `S3_RLCD_4_2` config:
  - **MCLK GPIO16**
  - **BCLK GPIO9**
  - **WS/LRCK GPIO45**
  - **DIN to codec GPIO10**
  - **DOUT from codec GPIO8**
  - **Speaker PA enable GPIO46**

### Buttons

| Button | GPIO | Active Level |
|---|---:|---:|
| BOOT / application button | **GPIO0** | Low |
| Secondary demo button | **GPIO18** | Low |

---

## Onboard Peripherals

- 4.2" ST7305 reflective LCD (400×300)
- ES8311 codec + ES7210 dual-microphone array + onboard speaker (AI voice)
- PCF85063 RTC (I2C)
- SHTC3 temp/humidity sensor (I2C)
- microSD (TF) card slot
- USB Type-C
- 18650 battery holder + battery voltage monitoring (GPIO4)

---

## Toolchain / Build Config

Frameworks supported: **Arduino IDE** and **ESP-IDF** (official demos for both).
Also runs as a documented **ESPHome** device (community ST7305 component), and a
community MicroPython driver exists.

### Arduino IDE (board = "ESP32S3 Dev Module")

| Setting | Value |
|---|---|
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM** |
| Flash Mode | QIO / DIO (flash is quad) |
| Partition | a 16MB scheme (e.g. "16M Flash (3MB APP/9.9MB FATFS)") |
| USB CDC On Boot | Enabled (native USB-Serial/JTAG) |

### ESP-IDF (sdkconfig)

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

### Flashing

```bash
# no permanent install needed — run esptool ephemerally via uvx
uvx esptool --port /dev/cu.usbmodem11201 chip-id
uvx esptool --port /dev/cu.usbmodem11201 flash-id
uvx esptool --port /dev/cu.usbmodem11201 write-flash 0x0 firmware.bin
```

### Related repos

- Official: https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
- MicroPython driver (community): https://github.com/micheleaiello/ESP32-S3-4.2inch-RLCD-Driver
- ESPHome device: https://devices.esphome.io/devices/waveshare-esp32-s3-rlcd-42/
