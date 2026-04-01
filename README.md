# esp-rfid-bridge

Firmware for the ESP32-C3 (Super Mini) that reads RFID cards via an RC522 module and relays card identifiers to a host system over USB Serial using a JSON protocol. The host is responsible for all access-control logic; the firmware handles only hardware I/O and LED feedback.

---

## Overview

```
┌─────────────┐    SPI     ┌───────────────┐   USB CDC   ┌─────────────┐
│  RFID Card  │ ────────► │  ESP32-C3 +   │ ──────────► │  Host/      │
│  (ISO 14443)│           │  RC522 Module │ ◄────────── │  Server     │
└─────────────┘           └───────────────┘  JSON/UART  └─────────────┘
                                  │
                               PIN_LED
                          (visual feedback)
```

The device exposes a REST-style API over USB Serial. Every message in both directions is a single-line JSON object terminated by `\n`. The host can read device state, update configuration, and control the LED at any time — not only when a card event occurs.

---

## Hardware

| Signal | GPIO |
|--------|------|
| SCK    | 4    |
| MISO   | 5    |
| MOSI   | 6    |
| SS     | 7    |
| RST    | 10   |
| LED    | 8    |

Target board: **ESP32-C3 Super Mini** (compatible with `esp32-c3-devkitm-1`).

---

## Serial Protocol

Communication runs at **115200 baud** over USB CDC.

### Request format (Host → Device)

```json
{"method": "<METHOD>", "resource": "<RESOURCE>", "id": <N>, "data": {...}}
```

| Field      | Required | Description                                                    |
|------------|----------|----------------------------------------------------------------|
| `method`   | yes      | `GET`, `PUT`, or `POST`                                        |
| `resource` | yes      | Target resource name (see table below)                         |
| `id`       | no       | Arbitrary integer echoed back in the response for correlation  |
| `data`     | depends  | Required for `PUT` and `POST /access`                          |

### Response format (Device → Host)

```json
{"status": <CODE>, "resource": "<RESOURCE>", "id": <N>, "data": {...}}
```

| Field      | Description                                                           |
|------------|-----------------------------------------------------------------------|
| `status`   | HTTP-style status code (`200`, `400`, `404`, `405`)                  |
| `resource` | Echoed resource name                                                  |
| `id`       | Echoed request `id`, omitted if the request had none                 |
| `data`     | Response payload, present on successful reads                         |
| `error`    | Human-readable error string, present on non-200 responses             |

### Event format (Device → Host, unsolicited)

Events are emitted by the device without a corresponding request.

```json
{"event": "<name>", ...}
```

---

## API Reference

### GET /status

Returns device uptime and last scanned card.

**Request**
```json
{"method": "GET", "resource": "status", "id": 1}
```

**Response**
```json
{"status": 200, "resource": "status", "id": 1, "data": {"uptime_ms": 45230, "last_uid": "AB:CD:EF:12", "version": "2.0.0"}}
```

---

### GET /config

Returns current timing configuration.

**Request**
```json
{"method": "GET", "resource": "config", "id": 2}
```

**Response**
```json
{"status": 200, "resource": "config", "id": 2, "data": {"debounce_ms": 3000, "response_timeout": 2000, "poll_interval_ms": 80, "post_read_delay": 300}}
```

---

### PUT /config

Updates one or more timing parameters at runtime. Only supplied fields are changed; omitted fields retain their current values.

**Request**
```json
{"method": "PUT", "resource": "config", "id": 3, "data": {"debounce_ms": 5000, "response_timeout": 3000}}
```

**Response** — full config after applying the update:
```json
{"status": 200, "resource": "config", "id": 3, "data": {"debounce_ms": 5000, "response_timeout": 3000, "poll_interval_ms": 80, "post_read_delay": 300}}
```

| Field              | Default | Description                                         |
|--------------------|---------|-----------------------------------------------------|
| `debounce_ms`      | 3000    | Minimum interval before the same UID re-fires       |
| `response_timeout` | 2000    | How long the firmware waits for `POST /access`      |
| `poll_interval_ms` | 80      | Card-presence polling cadence                       |
| `post_read_delay`  | 300     | Cooldown after processing a card event              |

---

### GET /uid

Returns the last scanned card UID without waiting for a new scan.

**Request**
```json
{"method": "GET", "resource": "uid", "id": 4}
```

**Response**
```json
{"status": 200, "resource": "uid", "id": 4, "data": {"uid": "AB:CD:EF:12"}}
```

UIDs are formatted as uppercase colon-separated hexadecimal of the first four serial bytes.

---

### PUT /led

Controls the indicator LED directly.

**Request**
```json
{"method": "PUT", "resource": "led", "id": 5, "data": {"state": "blink", "count": 2, "duration": 150}}
```

**Response**
```json
{"status": 200, "resource": "led", "id": 5}
```

| Field      | Values               | Default | Description                             |
|------------|----------------------|---------|-----------------------------------------|
| `state`    | `on`, `off`, `blink` | `blink` | LED state                               |
| `count`    | integer              | `1`     | Number of blinks (only for `blink`)     |
| `duration` | integer (ms)         | `100`   | Half-cycle duration (only for `blink`)  |

---

### POST /access

Sends an access decision in response to a `card` event. Must arrive within `response_timeout` milliseconds of the card event; otherwise the firmware times out and blinks the error pattern.

**Request**
```json
{"method": "POST", "resource": "access", "id": 6, "data": {"access": true}}
```

**Response**
```json
{"status": 200, "resource": "access", "id": 6, "data": {"access": true}}
```

Other host commands (e.g. `GET /status`) are still processed while the firmware is waiting for this response.

---

### POST /reset

Reboots the device. The firmware sends a `200` response before restarting.

**Request**
```json
{"method": "POST", "resource": "reset", "id": 7}
```

**Response**
```json
{"status": 200, "resource": "reset", "id": 7}
```

---

## Events (Device → Host)

### boot

Emitted once on startup.

```json
{"event": "boot", "msg": "ESP32-C3 RFID ready", "version": "2.0.0"}
```

### card

Emitted each time a new card is detected (subject to debounce). The host should reply with `POST /access` within `response_timeout` ms.

```json
{"event": "card", "uid": "AB:CD:EF:12"}
```

---

## LED Feedback

| Condition        | Pattern            |
|------------------|--------------------|
| Boot complete    | 2 blinks × 150 ms  |
| Access granted   | 1 blink  × 80 ms   |
| Access denied    | 3 blinks × 80 ms   |
| No host response | 5 blinks × 50 ms   |

---

## Error Responses

| Status | Meaning                                    |
|--------|--------------------------------------------|
| 400    | Request is missing a required `data` field |
| 404    | Resource name not recognised               |
| 405    | HTTP method not supported                  |

**Example**
```json
{"status": 404, "resource": "foo", "error": "resource not found", "id": 9}
```

---

## Building

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code
- Python 3.11 or later

### Build

```bash
pio run
```

### Flash

```bash
pio run --target upload
```

### Monitor

```bash
pio device monitor
```

The monitor filter includes `esp32_exception_decoder` and `time` for easier debugging.

---

## CI/CD

GitHub Actions is configured in `.github/workflows/build.yml`.

**Triggers:**
- Push to `main`, `master`, or `develop`
- Pull requests targeting `main` or `master`
- Any tag matching `v*`
- Manual dispatch

**Pipeline:**
1. Restore PlatformIO cache keyed on `platformio.ini`
2. Build firmware with `platformio run`
3. Collect `firmware.bin` and `firmware.elf` from `.pio/build/`
4. Upload as a build artifact retained for 30 days

**Releases:**

When a `v*` tag is pushed, an additional `release` job creates a GitHub Release and attaches the compiled `.bin` file. Tags containing `-rc` or `-beta` are marked as pre-releases automatically.

### Flashing a Released Binary

Via esptool:
```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 \
  write_flash 0x0 esp32c3-supermini-firmware.bin
```

Via PlatformIO:
```bash
pio run --target upload
```

Via browser: [esptool-js](https://espressif.github.io/esptool-js/)

---

## Dependencies

| Library     | Version | Source                |
|-------------|---------|-----------------------|
| MFRC522     | ^1.4.11 | miguelbalboa/MFRC522  |
| ArduinoJson | ^7.3.1  | bblanchon/ArduinoJson |

---

## License

Licensed under the [MIT License](LICENSE).