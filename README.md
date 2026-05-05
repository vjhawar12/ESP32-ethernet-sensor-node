# ESP32 Ethernet Sensor Node

ESP32-S3 firmware for an Ethernet-connected sensor node intended for long-lived remote deployment in a **3000+ unit seniors village**. The system is being built as part of an in-progress large-scale sensing and maintenance workflow, where nodes need stable addressing, remote observability, and reliable firmware updates without requiring physical access.

The node brings up a W5500 SPI Ethernet interface, assigns a persistent static IP from NVS, exposes a TCP control console, periodically measures onboard/attached sensors, streams telemetry over UDP, and supports HTTPS OTA updates with rollback validation.

This project is written in **C using ESP-IDF** and is aimed at real embedded deployment rather than a one-off demo. The focus is on firmware architecture, networking, remote maintainability, sensor integration, and the hardware/software boundary.


## Why this project matters

This repo demonstrates:

- **Low-level embedded firmware development** on ESP32-S3 using ESP-IDF
- **Hardware bring-up** of an SPI Ethernet controller (W5500)
- **Driver and peripheral integration** across SPI, GPIO, Ethernet, timers, sockets, NVS, OTA, I2C, GPIO expansion, and ADC
- **Networked firmware design** using both UDP and TCP
- **Remote update workflows** via HTTPS OTA
- **Periodic sensor acquisition and shared telemetry packaging**
- **Fault-conscious deployment features** such as persistent configuration and OTA validation

## Deployment context

This firmware is part of an ongoing system for a **3000+ unit seniors village**, where many sensor nodes are expected to operate continuously and be manageable remotely. That environment shapes the design choices in this repo:

- persistent per-node identity and static IP assignment
- lightweight remote operator console
- telemetry streaming to backend infrastructure
- OTA support for field updates
- event-driven task structure suitable for unattended deployment

The project is still in progress, but it is already structured around real deployment constraints rather than a classroom-only prototype.

## Current feature set

- Brings up the ESP32-S3 + W5500 Ethernet path over SPI
- Uses ESP-IDF Ethernet stack and TCP/IP integration
- Loads a persistent node IP address from NVS (or writes a default on first boot)
- Loads persistent/basic node identity information from NVS
- Assigns a static IPv4 address to the node
- Creates a TCP command console for remote interaction
- Periodically sends heartbeat logs
- Periodically measures connected sensors
- Periodically formats and streams JSON telemetry over UDP
- Fetches an OTA manifest over HTTPS
- Compares semantic firmware versions and flashes newer firmware when available
- Marks the currently running OTA image valid to prevent rollback after successful boots

## Repository structure

```text
.
├── components/
│   └── w5500/                 # custom/packaged W5500 support
├── main/
│   ├── main.c                 # app_main entry point
│   ├── s3.c                   # firmware implementation
│   ├── s3.h                   # public interface
│   └── cert.pem               # server certificate for HTTPS OTA
├── partitions.csv             # OTA-capable partition layout
├── sdkconfig                  # ESP-IDF configuration
└── .github/workflows/         # CI / release automation
```

## Firmware architecture

At startup, the firmware performs the following sequence:

1. Create FreeRTOS event groups
2. Allocate shared telemetry structures
3. Initialize NVS and load persistent node identity/configuration
4. Validate the currently running OTA image
5. Bring up the network stack and attach the W5500 Ethernet driver
6. Wait for link/IP acquisition
7. Initialize sensor-facing peripherals (I2C, ADC, GPIO interrupt path)
8. Fetch and parse the OTA manifest in non-flashing/status mode
9. Create synchronization primitives and timer-driven events
10. Spawn runtime tasks:
  - sensor measurement task
  - heartbeat task
  - UDP streaming task
  - TCP console task
11. Block in the main loop waiting for OTA trigger events

# Core runtime model

The firmware uses a small event-driven architecture built around:

- Event groups for coarse task signaling
- GPTimer callbacks for periodic scheduling
- FreeRTOS tasks for blocking network and application work
- A mutex to protect shared telemetry payload formatting/transmission

## Event groups

### `main_group`
- `ETH_CONNECTED_BIT`: network is ready
- `OTA_REQUESTED_BIT`: begin OTA check/update flow

### `log_group`
- `HEARTBEAT_BIT`: periodic heartbeat log
- `STREAM_BIT`: periodic telemetry stream trigger
- `STREAM_BIT_MANUAL`: operator-enabled streaming flag

### `collect_group`
- `MEASURE_ALL_BIT`: periodic sensor collection trigger

---

# Networking model

## Ethernet

The board uses a W5500 SPI Ethernet controller. Firmware initializes:

- SPI bus
- W5500 MAC/PHY wrappers through ESP-IDF
- Ethernet driver installation
- netif attachment to the TCP/IP stack
- static IP assignment

## UDP telemetry path

A UDP socket is created and bound locally. A periodic task formats telemetry into a JSON payload and sends it to the backend host.

Current payload fields include:

- chip
- firmware version
- IP
- temperature
- humidity
- air quality
- motion detection

The UDP path is intentionally simple and lightweight so that nodes can continuously export measurements to backend infrastructure.

## TCP node console

A lightweight TCP console listens on port 4000 and currently supports commands such as:

- `help`
- `reboot`
- `stream on`
- `stream off`
- `ota status`
- `ota flash`
- `exit`

This makes the node remotely inspectable and controllable without requiring physical access.

---

# Sensor integration

This repo now includes active sensor measurement code rather than only placeholder payload fields.

## Current sensor paths

### AHT20 (I2C)
Used for temperature and humidity measurement. The firmware performs device setup/checks and periodic reads, then converts raw values into floating-point engineering units for telemetry output.

### MQ135-style analog air quality input (ADC)
An ADC oneshot + calibration path is configured and sampled periodically. The calibrated voltage is currently used as the exported air-quality-related measurement.

### HC-SR505 / GPIO-expander-backed motion input
Motion state is read periodically and also wired into a GPIO interrupt path for immediate detection signaling/logging.

## Sensor-side peripherals used

- I2C master bus for digital sensors / expander-backed reads
- ADC oneshot + calibration for analog sensing
- GPIO interrupt handling for motion-related events
- Mutex-protected shared telemetry struct for packaging measurements into outgoing UDP JSON

---

# Telemetry update flow

The measurement task wakes on a periodic timer event, reads all configured sensors, converts raw values into human-readable fields, and updates a shared telemetry structure. The UDP stream task then serializes the latest values into JSON and transmits them to the backend.

This separation keeps acquisition and transport loosely coupled.

---

# OTA update flow

The node can be instructed to begin an OTA cycle through the TCP console.

OTA flow:

1. Fetch manifest over HTTPS
2. Parse manifest JSON
3. Extract version, target, flash size, commit, and firmware URL
4. Compare current firmware version vs. latest available version
5. Download and flash new firmware if newer
6. Reboot into the new slot
7. Mark the new image valid after successful boot

This project uses:

- HTTPS manifest fetch
- certificate-pinned TLS server trust via `cert.pem`
- ESP-IDF OTA APIs
- OTA validity / rollback handling

---

# Hardware assumptions

This code targets an ESP32-S3-ETH board using the W5500 and assumes the following pin mapping:

| Signal     | GPIO |
|------------|------|
| Reset      | 9    |
| Interrupt  | 10   |
| MOSI       | 11   |
| MISO       | 12   |
| SCLK       | 13   |
| CS         | 14   |

The implementation currently references the Waveshare ESP32-S3-ETH schematic conventions in code comments.

Additional sensor/peripheral assumptions in the current firmware include:

- I2C on GPIO 0 / GPIO 1
- AHT20 at address `0x38`
- PCF8575-style expander path at address `0x20`
- GPIO-based motion signaling on GPIO 10
- ADC-based analog input sampling on ADC unit 1 / channel 1

---

# Build notes

This is an ESP-IDF project.

Typical workflow:

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

# What is still in progress

This repo reflects an in-progress real deployment effort, so some hardening work is intentionally still ongoing.

Planned / ongoing work includes:

- socket recreation / recovery logic after repeated UDP send failures
- deeper sensor-driver modularization
- improved telemetry freshness and ownership boundaries
- richer remote diagnostics through the console
- stronger network fault recovery behavior
- clearer separation between hardware abstraction, transport, and application layers
- more complete backend/OTA pipeline documentation

# Debugging and systems challenges tackled

This project required working across several embedded problem areas:

- integrating Ethernet on a microcontroller over SPI instead of using only Wi-Fi
- coordinating multiple periodic behaviors without blocking the main control path
- designing a simple remote console for field interaction
- handling OTA safely enough for unattended devices
- persisting node identity/network configuration in non-volatile storage
- integrating sensor acquisition with concurrent network transport
- structuring the system so networking, telemetry, sensing, and updates can evolve independently

# Future hardening ideas

If this were pushed further toward production, the next steps would be:

- watchdog and task health monitoring
- reconnect/retry logic for repeated socket failures
- socket recreation after transport errors
- stronger payload serialization boundaries
- sensor-specific driver modules and calibration handling
- persistent diagnostic counters in NVS
- remote log/metrics export
- clearer HAL / transport / application separation

# Portfolio note

This project is part of a broader embedded systems portfolio centered on firmware, hardware bring-up, networking, OTA infrastructure, sensor integration, and debug-heavy development on real hardware.