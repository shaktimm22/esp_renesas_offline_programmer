# RA Offline Programmer (ESP8266)

A standalone production programmer for **Renesas RA2E1** (Cortex-M23: R7FA2E1A9 / A7 / A5, 128 / 64 / 32 KB) built on an ESP8266 (NodeMCU). It does the same job as the E2 emulator Lite with Renesas Flash Programmer: erase, program, verify and run, over SWD.

- The firmware image lives in the ESP8266's flash (LittleFS), so no PC is needed while programming.
- Programming starts on a **button press** or **automatically when a board is connected**.
- A Python UI (USB) or the built-in web page (Wi-Fi) uploads the `.hex` / `.srec`, changes the settings and shows **PASS / FAIL / TOTAL**.

```
 PC (tools/flasher_ui.py) ──USB/UART 460800──> ESP8266 ──SWD + RES──> RA2E1 target
 Phone / laptop browser ──────Wi-Fi AP + HTTP─────────┘
```

It shares its design, UART protocol and UI with `../flasher_mm32` (MM32 / Fortior). The SWD driver is the same one; `src/ra.cpp` is new.

## 1. Hardware

| ESP8266 (NodeMCU) | GPIO | RA2E1 | |
|---|---|---|---|
| D5 | 14 | **SWCLK** (P300) | |
| D6 | 12 | **SWDIO** (P108) | |
| D7 | 13 | **RES** | Recommended. Open drain: the ESP8266 only pulls it low |
| GND | – | **VSS** | Required |
| 3V3 | – | VCC | Only if the programmer powers the target |
| D3 | 0 | – | Start button. This is the onboard **FLASH** button; an external button goes from D3 to GND |
| D1 | 5 | – | Green PASS LED (with 330 Ω to GND) |
| D2 | 4 | – | Red FAIL LED (with 330 Ω to GND) |
| D4 | 2 | – | Onboard blue LED = BUSY (blinks while programming) |
| D8 | 15 | – | Buzzer through an NPN transistor (optional) |

- These are the same pins the E2 Lite uses (its 10-pin connector: SWCLK, SWDIO, RES, VCC, GND).
- **MD (P201) must stay high** (single-chip mode), as it is on a normal board. If MD is low, the chip starts in its serial boot mode.
- **RES** is optional, but wire it if possible. With it, the programmer connects **under reset**, so firmware that turns the SWD pins into GPIO, or enters software standby, cannot lock it out. Without it, untick *Use RES* (or build with `-D PIN_NRST=-1`).
- **Voltage:** the ESP8266 is 3.3 V only. A 3.3 V board connects directly. A **5 V board needs a level shifter** on SWCLK/SWDIO (e.g. TXS0102, target side at the board's VCC). RES needs no shifter because it is only ever pulled low.
- Keep the wires short (under 15 cm) with GND next to them. With long pogo-pin fixtures, raise **SWD clock delay**.

To change the pins, edit [src/pins.h](src/pins.h) or override them with `-D PIN_xxx=` in `platformio.ini`.

## 2. Build and flash the ESP8266

PlatformIO project (VS Code → PlatformIO → *Upload*), or `pio run -t upload`.

- The filesystem is formatted automatically on first boot.
- The web page source is [web/index.html](web/index.html). Every build gzips it into `src/web_page.h`, so no `uploadfs` is needed. Do not run `uploadfs`: it would erase the stored image and settings.

## 3. PC application

```
pip install -r tools/requirements.txt
python tools/flasher_ui.py      (or double-click tools/run_ui.bat)
```

1. Pick the COM port at **460800** baud and press **Connect**.
2. **Browse** to the e2 studio output: `.hex` (Intel HEX) or `.srec` / `.mot` (S-record), found in the project's `Debug` / `Release` folder.
3. Press **Upload to programmer**. The image is CRC-checked and stored, and it survives power-off.
4. Choose the settings and press **Save to programmer**.
5. Press **FLASH** (F5) or the button on the programmer. Every result, with the chip's **unique ID**, is appended to `tools/flash_log.csv`.

### What gets programmed

| Region | Address | Handling |
|---|---|---|
| Code flash | `0x00000000` – up to 128 KB | Programmed. This includes the OFS0/OFS1 option settings at `0x400`, which FSP puts in the image |
| Data flash | `0x40100000` – 4 KB | Programmed if the file has data there |
| ID code / access window (extra area) | `0x01010000` – `0x0101003F` | FSP always emits 0xFF here (unlocked). That is accepted and left as is. A file that **sets** an ID code is refused (not supported yet) |

### Settings

| Setting | |
|---|---|
| Start flashing by | **Button press**, or **Auto-detect board**: polls SWD and flashes once per board, after *Auto-detect settle* ms |
| Erase | **Full chip**: all code flash and data flash blocks, like a fresh chip. **Only image blocks**: only the 2 KB code blocks (and 1 KB data blocks) the image covers. Data flash contents outside the image are kept |
| Verify | Read everything back and compare |
| Run firmware | Reset the target after programming (pulse RES, or a system reset without RES) |
| Use RES | Connect under reset and use RES for the final reset |
| SWD clock delay | 0 = fastest. The programmer also slows down by itself when a board does not answer |

### Web UI (Wi-Fi)

The programmer runs its own Wi-Fi network **`RA-Flasher-XXXX`** (open by default). Join it; the page opens by itself (captive portal), or browse to **http://ra-flasher.local** / **http://192.168.4.1**. It offers the same functions as the desktop app. A password, SSID or hostname can be set with the `WIFI_AP_*` / `MDNS_HOSTNAME` flags in `platformio.ini`.

## 4. How it programs the RA2E1

This all runs over SWD with the core halted, and follows the FSP `r_flash_lp` driver (FSP 6.3.1, MF4 flash) in [src/ra.cpp](src/ra.cpp):

1. **Connect:** optionally under RES. Then halt on the reset vector after a system reset, so no firmware runs. Read the part number (`PNR`, `0x01001C10`) and the 128-bit unique ID (`UIDR`, `0x01001C00`). The part number gives the code flash size (A5 = 32 KB, A7 = 64 KB, A9 = 128 KB).
2. **Clock:** after reset, ICLK is MOCO/16 = 0.5 MHz, below the flash sequencer's 1 MHz minimum. The programmer switches to MOCO/1 = 8 MHz and sets `FISR.PCKA` to match.
3. **Erase:** 2 KB code blocks in code flash P/E mode (`FENTRYR=0xAA01`), 1 KB data blocks in data flash P/E mode (`0xAA80`).
4. **Program:** 4 bytes per command (code flash) or 1 byte (data flash). Erased words (all 0xFF) are skipped.
5. **Verify:** block reads compared with the stored image.
6. **Run:** clear the debug state, release the debug power request and reset.

## 5. UART protocol

The link is 460800 8N1 and line-based. Replies are `@OK <CMD> [data]` / `@ERR <CMD> <message>`, and events are `@EVT ...`. Lines without `@` are ESP8266 boot noise.

| Command | Reply data |
|---|---|
| `PING` | `RAFLASHER <ver>` |
| `INFO` | `fw= img= size= crc= name= cf= df= pass= fail= free=` + all settings |
| `SET <key> <value>` | `mode button\|auto`, `erase chip\|blocks`, `verify 0\|1`, `run 0\|1`, `reset 0\|1`, `swdelay <n>`, `settle <ms>` |
| `BEGIN <size> <crc32> <cf> <df> <name>` | start an upload: `cf` code flash bytes from 0x0, then `df` data flash bytes from 0x40100000 |
| `DATA <offset> <hex>` / `END` / `ABORT` | upload data (≤256 bytes per line); `END` checks size and CRC32 (zlib) |
| `FLASH` | replies immediately, then sends events |
| `DETECT` | `dpidr= part= flash=<KB> uid= aws= awe= swdelay= reset=` |
| `ERASE` / `RESET` | erase the whole chip / reset and run the target |
| `READ <addr> [words]` | dump up to 32 words of target memory |
| `RESETCNT` | zero the counters |

Events: `STATE connect|erase|program|verify|idle`, `PROGRESS <stage> <pct>`, `CHIP <part> <uid>`, `RESULT PASS <ms> <part> <uid>`, `RESULT FAIL <counted 0|1> <message>`, `COUNT <pass> <fail>`, `TARGET present|absent`, `BOOT`, `WIFI`.

HTTP API: `POST /api/cmd c=<command>`, `GET /api/events?since=<seq>`, `POST /api/upload?size=&crc=&fields=<cf> <df>&name=` (multipart file).

A board that does not answer on SWD at all is reported as FAIL but **not counted**. A board that answers but fails is counted.

## 6. First bring-up checklist

This firmware was written from the RA2E1 SVD and the FSP flash driver. Check it on a known-good board before production:

1. **Detect chip.** Expect `part=R7FA2E1A…`, the right `flash=` size and a UID. A `part=UNKNOWN` with a valid DPIDR means the part-number register reads differently from what is expected. Please report the `READ 0x01001C10 4` output.
2. **FLASH** a known image with *Verify* on, and check that the board runs.
3. Check that the board's own settings in data flash are kept or erased, depending on the *Erase* setting you chose.
4. Compare the timing with the E2 Lite. Typical: roughly 0.1–0.2 s per KB of code (each 4-byte write is ~20 SWD transactions).

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| "No SWD response: SWDIO idles high" | Target unpowered, GND missing, or SWCLK/SWDIO swapped |
| "SWDIO is held LOW" | Firmware drives P108. Wire RES and tick *Use RES* |
| "Garbled SWD reply" | Signal levels (5 V board without a level shifter) or long wires: raise *SWD clock delay* |
| "Could not halt the core" | ID code protection is set on that chip; the programmer does not support ID authentication |
| "Erase failed … illegal command" | Blocks protected by an access window (see `aws`/`awe` in DETECT) |
| "Image needs N KB of code flash" | The image does not fit the detected part (e.g. 64 KB image on an A5) |
