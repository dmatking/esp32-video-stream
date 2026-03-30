# esp32-video-stream

MJPEG-over-TCP video stream receiver for ESP32-S3. A Python server transcodes any video file to MJPEG using ffmpeg and sends frames over TCP. The ESP32 firmware decodes each frame with the tjpgd ROM decoder and pushes it to the display at ~12 fps.

<video src="demo.mp4" autoplay loop muted playsinline width="640"></video>

## Hardware

- **Board:** Waveshare ESP32-S3 Touch LCD 2.0"
- **Display:** ST7789 240x320 SPI (driven in landscape, 320x240)
- **MCU:** ESP32-S3 with 8 MB octal PSRAM, 16 MB flash

## Protocol

Each frame is sent as a 4-byte little-endian length prefix followed by JPEG data. The ESP32 connects to the server as a TCP client.

## Setup

### Server (PC / Pi)

Requires `ffmpeg` installed.

```
python stream_video.py <video_file> [--port 5000] [--width 320] [--height 240] [--fps 20] [--quality 10]
```

The `--quality` flag maps to ffmpeg's `-q:v` (2-31, lower is better). The video loops indefinitely.

### Firmware

Requires ESP-IDF v5.5+. WiFi credentials are loaded from `~/.esp_creds` via `SDKCONFIG_DEFAULTS` so they are never committed. Create the file with your network details:

```
# ~/.esp_creds
CONFIG_WIFI_SSID="YourNetworkName"
CONFIG_WIFI_PASS="YourPassword"
```

Then build and flash:

```
idf.py set-target esp32s3
idf.py menuconfig   # set Video Stream -> server IP/port if defaults don't match
idf.py build flash monitor
```

The default server address is `192.168.68.65:5000`. Change it in `menuconfig` under **Video Stream** or edit `sdkconfig.defaults`.

## How it works

1. `stream_video.py` spawns ffmpeg to produce an MJPEG pipe, parses SOI/EOI markers to extract individual JPEG frames, and sends each one length-prefixed over TCP.
2. The ESP32 firmware connects over WiFi, reads the length prefix, receives the JPEG data into a 64 KB PSRAM buffer, decodes it with the tjpgd decoder from ROM (zero flash cost), and writes RGB565 pixels directly into the display framebuffer.
3. After decoding, the framebuffer is flushed to the ST7789 over SPI at 40 MHz.
