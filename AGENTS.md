# AGENTS.md

Guidance for coding agents working in this repository.

## Project Overview

This is ESP-IDF firmware for an ESP32-C5 based Volkswagen MEB battery preheat controller. The firmware listens to CAN FD/TWAI traffic, exposes newline-delimited JSON-RPC over UART0 and BLE, sends diagnostic preheat requests when enabled, reports telemetry, drives a WS2812 status LED, and supports chunked OTA updates.

The target is `esp32c5` with ESP-IDF `6.0.0` as recorded in `dependencies.lock`.

## Repository Layout

- `main/`: Firmware component sources and headers.
- `main/app_config.h`: Central firmware metadata, GPIO pins, timing constants, CAN IDs, and safety defaults.
- `main/app_state.*`: Shared runtime state protected for FreeRTOS task access.
- `main/can_bus.*`: TWAI/CAN FD setup, receive handling, diagnostic requests, and CAN diagnostics.
- `main/serial_console.*` and `main/ble_console.*`: JSON-RPC command and telemetry transports.
- `main/control.*`: Heating control loop, telemetry task, SoC polling, and auto-off handling.
- `main/ota_update.*`: JSON-RPC OTA protocol.
- `main/status_led.*`: LED state display.
- `main/Kconfig.projbuild`: Project configuration options.
- `scripts/`: Host helper scripts for production builds, BLE commands, and OTA updates.
- `BUILD.md`, `README.md`, and `HARDWARE.md`: User-facing build, runtime, protocol, architecture, and vehicle connection documentation.

## Build And Verification

Development builds use the normal ESP-IDF build directory:

```sh
cmake --build build
```

If the build directory is not configured yet, configure it with ESP-IDF available in the environment:

```sh
cmake -S . -B build -G Ninja -DIDF_TARGET=esp32c5
cmake --build build
```

Production builds use the PowerShell helper documented in `BUILD.md`:

```powershell
.\scripts\build_production.ps1
```

There is no dedicated unit test suite in this repository at the moment. For firmware changes, use a successful development build as the baseline smoke test. For protocol or host-tool changes, also exercise the relevant Python helper path when hardware is available.

## Coding Conventions

- Keep C code in the existing ESP-IDF style: 4-space indentation, file-local helpers marked `static`, public functions prefixed with `meb_`, and module log tags near the top of each `.c` file.
- Return `esp_err_t` from initialization and setter APIs where failure is meaningful. Use ESP-IDF error codes rather than inventing local status enums unless the code already has a domain enum.
- Keep shared state changes inside `app_state.*` or protected by the same FreeRTOS synchronization style already used in the touched module.
- Do not do slow work in TWAI callbacks or ISR paths. Queue work to tasks, matching the existing CAN receive pattern.
- Keep JSON-RPC and telemetry output compact and newline-delimited. Host tools rely on one complete JSON object per line and should be able to ignore non-JSON ESP-IDF log lines.
- When adding a new C source file under `main/`, add it to `main/CMakeLists.txt`.
- When adding project configuration, use `main/Kconfig.projbuild` and keep development-vs-production behavior explicit.

## Hardware And Safety Notes

- Treat CAN IDs and diagnostic payloads as safety-sensitive. Preserve the existing MEB IDs and request bytes unless the change is specifically about that protocol.
- Keep `MEB_CAN_TEST_TX_ENABLED` set to `0` before real vehicle use.
- Preserve the distinction between development and production power behavior. Development builds intentionally keep automatic light sleep, BLE modem sleep, and tickless idle disabled so USB-JTAG remains reliable. Production builds enable low-power behavior through `sdkconfig.defaults.production`.
- The OTA partition table gives each app slot `0xE0000` bytes. Do not grow firmware features without checking binary size against the documented OTA slot limit.
- Signed firmware and secure boot are not enabled. Do not describe SHA-256 OTA checks as authenticity guarantees; they are integrity checks only.

## Generated Files And Dependencies

Do not edit or commit generated output:

- `build/`
- `build-production/`
- `managed_components/`
- `sdkconfig.production`
- `sdkconfig.old`
- CMake cache/output files
- Python virtual environments and caches

Tracked configuration and lock files are intentional:

- `sdkconfig`
- `sdkconfig.defaults`
- `sdkconfig.defaults.production`
- `dependencies.lock`
- `partitions.csv`

## Documentation Expectations

Update `README.md` when changing user-visible JSON-RPC methods, telemetry fields, BLE/UART behavior, CAN behavior, OTA behavior, LED meanings, or runtime flow.

Update `BUILD.md` when changing build profiles, flashing instructions, power-management defaults, generated artifacts, or production build behavior.

Update `HARDWARE.md` when changing hardware pin mappings, CAN-bus connection guidance, vehicle compatibility, or safety-critical wiring information.

Keep examples in the documentation aligned with the compact JSON style used by the firmware and Python helpers.

## Handoff Checklist

Before handing work back:

- Run `cmake --build build` when ESP-IDF and the configured build directory are available.
- For production-specific changes, run or clearly defer `.\scripts\build_production.ps1`.
- Note any hardware-dependent verification that could not be performed locally.
- Check `git status --short` and make sure generated artifacts were not added.
