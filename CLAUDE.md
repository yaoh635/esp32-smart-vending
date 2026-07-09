# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
idf.py set-target esp32p4             # Set target chip (required first time)
idf.py build                          # Full build
idf.py flash monitor                  # Flash and monitor serial output
idf.py menuconfig                     # Interactive configuration
idf.py fullclean                      # Clean all build artifacts including managed components
```

Build requires `IDF_PATH` environment variable pointing to ESP-IDF v5.5.4.

### Build Configuration

- `sdkconfig.defaults` — Base config: PSRAM, 16MB flash, custom partition table, OV5647 800×800 RAW8, LVGL fonts, WDT, ESP-SR models
- `sdkconfig.defaults.esp32p4` — ESP32-P4 specific: cache config, I2S/I2C
- `partitions.csv` — `nvs` (24KB) + `phy_init` (4KB) + `factory` (6MB app) + `model` (5MB ESP-SR models)
- Root `CMakeLists.txt` sets `MINIMAL_BUILD ON` and registers `components/components` and `components/managed_components` as `EXTRA_COMPONENT_DIRS`

## Architecture

This is an **ESP32-P4 Smart Vending Machine** with face recognition and voice control. The pipeline:

```
OV5647 Camera (800×800 RAW8 MIPI-CSI)
  → WhoFrameCap (frame capture node graph)
    → WhoDetect (face detection, MSRMNP_S8_V1 model @ 5 FPS)
      → HumanFaceRecognizer (MFN feature extraction, embedding DB on SD)
        → Vending UI (LVGL on ST7789 320×480 SPI LCD)
          → Voice Control (ESP-SR MultiNet7, Chinese commands)
            → SD Card purchase logging (CSV)
```

### Application Flow (`main/main.cpp` → `app_main`)

1. **Boot/Wake** — Check wakeup causes (capacitive touch, timer fallback, or cold boot)
2. **SD + Face DB** — Mount SD at `/sdcard`, load `face_db.csv`
3. **Camera pipeline** — `WhoFrameCap` node graph with `WhoOV5647Cam` (MIPI-CSI)
4. **Detection** — `WhoDetect` task at 5 FPS, callback triggers semaphore
5. **Monitor** — `face_monitor_task` waits on semaphore (5s timeout); 30s cumulative idle → deep sleep
6. **Vending** — On face detected: pause camera, init voice control + SPI LCD+touch, run LVGL UI with voice commands, wait for purchase completion via FreeRTOS task notification, stop voice, deinit LCD, resume detection
7. **Sleep** — `enter_deep_sleep()` stops camera, deinits ISP, configures capacitive touch wakeup

### Voice Control

- **ESP-SR** with MultiNet7 Chinese model for offline speech command recognition
- **I2S audio** via ES8311 (speaker) + ES7210 (microphone) on ESP32-P4-Function-EV-Board
- **Shared I2C bus** with camera (GPIO7/8) — `camera_get_i2c_bus_handle()` exposes the bus
- **Voice commands**: "可乐" (Cola), "雪碧" (Sprite), "水/矿泉水" (Water), "薯片" (Chips)
- **Integration**: Voice control starts with vending UI, stops when UI ends

### Servo Control

- **5 servos** for product delivery, one per product slot
- **GPIO pins**: 47 (Cola), 27 (Water), 53 (Chips), 46 (Candy), 48 (reserved)
- **PWM**: 50Hz, 0.5ms (0°) to 2.5ms (180°), using LEDC peripheral
- **Dispensing**: Servo rotates to 90° for 1.5s, then returns to 0°

### WiFi & Web Server

- **ESP-Hosted** WiFi via ESP32-C6 co-processor (SDIO interface)
- **HTTP server** on port 80 with 4 endpoints:
  - `GET /` — HTML status dashboard
  - `GET /status` — JSON system status (uptime, memory, face count, vending state)
  - `GET /stream` — MJPEG live camera stream (~8 FPS)
  - `GET /snapshot` — Single JPEG frame
- **Hardware JPEG encoder** on ESP32-P4 (RGB565 → JPEG, quality 75, zero CPU load)
- WiFi config via Kconfig: `CONFIG_APP_WIFI_SSID`, `CONFIG_APP_WIFI_PASSWORD`

### Mini-Program Frontend

- **WeChat mini-program** in `mini-program-vending/` for remote management
- **Dual-role UI**: User view (recommendations, orders) / Admin view (dashboard, inventory)
- **REST API** communication with ESP32 (14 endpoints: `/api/system`, `/api/products`, `/api/order`, `/api/sales`, `/api/inventory`, `/api/face/register`, etc.)
- **Features**: QR code binding, personalized recommendations, inventory restock, sales analytics

### Key Files

- `main/main.cpp` — Entry point, orchestrates entire system
- `components/cam_mipi_ov56457/camera.c` — Camera HAL (ISP, CSI, sensor init; file-scope static `s_isp_proc`)
- `components/spi_lcd_touch/` — SPI bus + ST7789 LCD + XPT2046 touch + LVGL init/deinit
- `components/vending_machine_ui/` — LVGL vending UI (product select, confirm, pay, dispense)
- `components/face_id_manager/` — Face registration, lookup, purchase logging on SD
- `components/voice_control/` — ESP-SR voice recognition (I2S + ES8311/ES7210, MultiNet7 Chinese)
- `main/web_server.cpp` — HTTP server with MJPEG streaming and REST API
- `main/wifi_init.c` — ESP-Hosted WiFi STA initialization
- `main/jpeg_encoder.c` — ESP32-P4 hardware JPEG encoder wrapper
- `mini-program-vending/` — WeChat mini-program frontend (user + admin views)

### Component Layout

- **`components/components/`** — Espressif WHO framework (customized). Registered via `EXTRA_COMPONENT_DIRS` in root CMakeLists.txt. Key: `cam_mipi_ov56457`, `who_frame_cap`, `who_detect`.
- **`components/` (top-level)** — Custom components: `spi_lcd_touch`, `vending_machine_ui`, `face_id_manager`, `sd_card`.
- **`components/managed_components/`** — IDF Component Manager deps (esp-dl, LVGL, face models, touch drivers).

⚠️ There are **duplicate** `spi_lcd_touch` components (one in `components/`, one in `components/components/`). The root CMakeLists.txt registers `components/components/` via `EXTRA_COMPONENT_DIRS`, so that version wins unless `main/CMakeLists.txt` explicitly depends on the other.

### Deep Sleep & Wakeup

- Idle 30s → `enter_deep_sleep()` stops camera pipeline, deinits ISP hardware, configures EXT1 wakeup on GPIO 2 (touch XPT2046 interrupt, active low).
- ISP handle (`s_isp_proc`) is file-scope static in `camera.c` — must be released via `camera_deinit()` before deep sleep or wakeup fails on next cycle.
- Wakeup cause checked via `esp_sleep_get_wakeup_cause()` (returns `esp_sleep_source_t`, ESP-IDF v5.x API).

### Synchronization

- Face detection callback (`on_face_detected`) triggers a FreeRTOS semaphore.
- `face_monitor_task` waits on that semaphore with 5s timeout; 30s cumulative idle → deep sleep.
- Purchase completion signaled via FreeRTOS task notifications (not polling).

## Key Technical Constraints

- **PSRAM required**: Camera frame buffers (~2.4MB) must be in PSRAM.
- **Factory partition is 6MB** (`partitions.csv`): needed for face detection/recognition models.
- **Model partition is 5MB**: needed for ESP-SR speech recognition models.
- **Camera outputs RAW8 Bayer** (GBRG native, mirror+flip → GRBG). Code may label frames as RGB565 — this is a known inconsistency. ISP demosaicing is configured but its output is not connected to frame buffers.
- **GPIO 2** is used for touch wakeup (EXT1). It's in the LP GPIO domain (GPIO 0-13) for deep sleep.
- **I2C bus shared** between camera (OV5647) and audio codecs (ES8311/ES7210) on GPIO7/8.
- **Servo control** uses LEDC PWM on GPIO 47, 27, 53, 46, 48 (50Hz, 0.5ms~2.5ms pulse width).

## Common Pitfalls

- `replace_all` edits can corrupt type names (e.g., `isp_proc_handle_t` → `s_isp_proc_handle_t`). Verify after bulk edits.
- SPI bus must be freed before re-initialization. `spi_lcd_touch_deinit()` handles cleanup order: display → panel → panel_io → touch_io → semaphore → spi_bus_free.
- Touch wakeup: GPIO 2 stays low after wakeup → add pull-up config + wait-for-release loop before re-entering sleep, plus 500ms debounce.
