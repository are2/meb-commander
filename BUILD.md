# Build

This project has two build profiles:

- Development build: default profile for normal coding, JTAG flashing, and debugging.
- Production build: low-power profile for installed firmware.

The development profile is intentionally JTAG-safe. The production profile enables automatic light sleep and BLE modem sleep, which can make USB-JTAG flashing unreliable after that firmware is running.

## Development Build

Use the normal ESP-IDF build directory:

```powershell
$env:IDF_PATH='C:\esp\v6.0\esp-idf'
cmake --build build
```

The development profile uses `sdkconfig` and outputs:

```text
build\meb-preheat.bin
```

Development power settings:

```text
# CONFIG_MEB_PRODUCTION_BUILD is not set
# CONFIG_PM_ENABLE is not set
# CONFIG_BT_LE_SLEEP_ENABLE is not set
# CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set
# CONFIG_ESP_MODEM_CLOCK_ENABLE_CHECKING is not set
```

Use this profile while flashing over USB-JTAG or debugging with OpenOCD.

## Production Build

Use the production script:

```powershell
.\scripts\build_production.ps1
```

The script uses the normal `sdkconfig` as the base, applies `sdkconfig.defaults.production` as overrides, writes the generated config to `sdkconfig.production`, and outputs:

```text
build-production\meb-preheat.bin
```

`build-production\` and `sdkconfig.production` are generated files and are ignored by git.

## Firmware Release Package

Use the interactive publish helper from a clean git tree:

```powershell
python .\scripts\publish_firmware_package.py
```

The helper asks for the next firmware version, updates `MEB_APP_VERSION`,
creates a release commit and annotated git tag, pushes the branch and tag,
runs the production build, and writes a package under `release\<version>\`.
The package includes the versioned app image for OTA, such as
`meb-preheat-0.7.0.bin`, full-flash artifacts, `flash_args`, SHA-256 checksums,
a JSON manifest, and release notes generated from git commit messages.

Production power settings:

```text
CONFIG_MEB_PRODUCTION_BUILD=y
CONFIG_MEB_AUTOMATIC_LIGHT_SLEEP=y
CONFIG_MEB_JTAG_BOOT_GRACE_MS=0
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_BT_LE_SLEEP_ENABLE=y
CONFIG_ESP_MODEM_CLOCK_ENABLE_CHECKING=y
CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y
CONFIG_PM_SLP_DISABLE_GPIO=y
```

Production intentionally keeps these more aggressive sleep options disabled:

```text
# CONFIG_ESP_SLEEP_POWER_DOWN_FLASH is not set
# CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP is not set
```

Those are left off because this firmware uses BLE, UART, TWAI/CAN, RMT LED output, flash-backed OTA state, and runtime logging. Automatic light sleep plus BLE modem sleep gives the main power saving while avoiding the riskier flash/peripheral power-down modes.

## Flashing

Development firmware can be flashed with JTAG from the ESP-IDF extension.

Production firmware should be flashed with UART/serial if JTAG becomes unreliable after the production image has run. From the production build directory, use the generated flash arguments:

```powershell
cd build-production
python -m esptool --chip esp32c5 -b 460800 --before default-reset --after hard-reset --port COM4 write_flash "@flash_args"
```

Adjust `COM4` if the board appears on a different serial port.
