# CanuckWeb v1.00
**History made on 320 kilobytes.**

The first web browser running on an ESP32-S3 microcontroller. Searches DuckDuckGo, fetches live pages, follows links, keeps history. Real browser. $30 device.

---

## Hardware
- **LilyGo T-Deck** (ESP32-S3FN16R8)
- 512KB internal SRAM + 8MB PSRAM + 16MB Flash
- 2.8" ST7789 320×240 IPS display
- QWERTY keyboard (I2C 0x55) + optical trackball

## How It Works
Searches go to `lite.duckduckgo.com` over a raw TLS POST. Pages are fetched through `r.jina.ai` which strips them to plain text — a 2MB webpage becomes a few KB. If a page is blocked, it falls back to the Wayback Machine automatically. All page content lives in PSRAM.

## Controls
| Key | Action |
|-----|--------|
| Type + ENTER | Search |
| Trackball UP/DN | Scroll results / page |
| Trackball CLICK or ENTER | Open result |
| B | Back |
| N | Enter URL directly |
| R | Reload |
| S | New search |
| Q | Restart |

## Building
1. Install [PlatformIO](https://platformio.org/)
2. Clone this repo
3. Copy your TFT_eSPI `User_Setup.h` for the T-Deck into the lib folder
4. Run `pio run --target upload`

## Pre-built Binary
See [Releases](../../releases) — flash with esptool:
```
esptool.py --chip esp32s3 --port COMX write_flash 0x0 CanuckWeb_v1.00.bin
```

![IMG_5327](https://github.com/user-attachments/assets/9fcbea37-ee5e-419b-bd98-24a5a3c3450d)
![IMG_5290](https://github.com/user-attachments/assets/1ba1d90b-5069-4058-83e1-4cd4d8c9d0ab)
![IMG_5315](https://github.com/user-attachments/assets/5ddf1e81-a356-477f-afae-fb97e71668b0)
![IMG_5299](https://github.com/user-attachments/assets/d2ed809d-e0db-4df8-8354-88fa9d396453)
