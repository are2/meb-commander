# Hardware Architecture and Vehicle Connection

This document describes the current MEB Commander hardware and the planned
multi-bus design for a Volkswagen MEB vehicle. The firmware presently uses one
CAN-FD interface for battery telemetry and preheating. Future body functions,
including central-locking research, require a second, electrically separate
interface to the vehicle's Convenience CAN.

Vehicle wiring information for the working EV-CAN connection is based on the
[original meb-preheat project](https://github.com/jagheterfredrik/meb-preheat)
and its [connection notes](https://github.com/jagheterfredrik/meb-preheat/blob/main/NOTES.md).
The Convenience CAN information is research material and has not yet been
validated by this project. Always verify connector pins against official,
VIN-specific wiring documentation for the exact model, market, and model year.

For the wider logical topology, module roles, and the distinction between CAN
and the 100/1000 Mbit/s automotive-Ethernet fabric, see
[MEB_NETWORKS_AND_MODULES.md](MEB_NETWORKS_AND_MODULES.md).

## ID.4 CAN networks

The ID.4 has several separate vehicle networks. The names and topology can
vary between model years, markets, battery versions, and wiring diagrams.

| Network | Typical role |
| --- | --- |
| CAN-EV / EV-CAN | High-voltage components, including the battery-management controller (J840), charging and thermal-management equipment. |
| Powertrain CAN | Electric drive and motor-control systems. |
| Running-gear CAN-FD | Chassis, braking, and steering systems. |
| Driver-assistance CAN-FD | Driver-assistance control units. |
| Convenience CAN | Body and convenience functions. |
| B-CAN | Battery-cell-control sub-bus, not the bus used by this controller. |

The Volkswagen service-training material describes CAN-EV as the data bus for
high-voltage components and B-CAN as the battery sub-bus. CAN-FD frames use a
500 kbit/s arbitration phase and, on the relevant networks, a 2 Mbit/s data
phase. See the
[Volkswagen ID.4 Self Study Program 891213](https://static.nhtsa.gov/odi/tsbs/2021/MC-10189712-0001.pdf).

These CAN networks are only part of the MEB communications architecture.
J533/ICAS1 and J794/ICAS3 also use a switched 100BASE-T1/1000BASE-T1 Ethernet
fabric for higher-bandwidth functions and diagnostics. That Ethernet network
is electrically and logically different from CAN and is not supported by the
current Commander hardware.

## Current and Planned CAN Interfaces

MEB Commander must treat each vehicle network as a distinct trust and fault
domain. Connecting to two buses does not make the device a general-purpose
gateway, and firmware must not forward arbitrary frames between them.

| Logical interface | Status | Vehicle network | Format and speed | Purpose |
| --- | --- | --- | --- | --- |
| CAN 0 | Implemented | CAN-EV / EV-CAN | CAN FD, 500 kbit/s arbitration, 2 Mbit/s data, extended IDs | Battery telemetry, SoC polling, and preheating diagnostics |
| CAN 1 | Planned | Convenience CAN | Classical CAN, 500 kbit/s, primarily standard IDs | Body-state observation and central-locking research |

The current controller is connected to **CAN-EV / EV-CAN** after the vehicle
gateway, where it can communicate with the high-voltage battery management
controller J840. It is not connected to Convenience CAN, Powertrain CAN,
B-CAN, LIN, or an arbitrary OBD diagnostic pair.

The ID.4 wiring documentation has an important naming trap: CAN-EV may be
labelled “powertrain CAN bus” in wiring diagrams, while the actual drivetrain
Powertrain CAN is also labelled “powertrain CAN bus”. Use the connected control
units and the gateway connector pinout, not the label alone, to identify the
bus.

The implemented EV-CAN node uses these settings:

| Setting | Value |
| --- | --- |
| Arbitration bitrate | 500 kbit/s |
| Data bitrate | 2 Mbit/s |
| Frame format | CAN FD with bit-rate switching and extended IDs |
| ESP32-C5 CAN TX | GPIO 4 |
| ESP32-C5 CAN RX | GPIO 5 |

The application filters and uses MEB frames such as `0x17FC007B`,
`0x17FE007B`, `0x12DD54D2`, `0x1A5555B2`, `0x12DD54D0`, and `0x16A954A6`.
Seeing these frames is a useful indication that the transceiver is connected
to the intended network, but it is not a substitute for checking the wiring.

Volkswagen training material shows Convenience CAN as a separate 500 kbit/s
network associated with J533/ICAS1, J519, and the access/start system J965.
Central-locking traffic observed on that bus is not expected to be reachable
by transmitting on EV-CAN because the gateway isolates and filters traffic by
source network. See [CAN_LOCK_UNLOCK_RESEARCH.md](CAN_LOCK_UNLOCK_RESEARCH.md).

## ESP32-C5 and Transceivers

The ESP32-C5 contains two TWAI controllers, so the intended architecture can
use one on-chip controller per vehicle bus. Each controller still requires its
own external physical-layer transceiver. See the
[ESP32-C5 TWAI documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/api-reference/peripherals/twai.html).

### Current EV-CAN wiring

The ESP32-C5 pins connect to a **3.3 V-compatible CAN-FD transceiver**, not
directly to the vehicle CAN wires:

| ESP32 controller | CAN-FD transceiver |
| --- | --- |
| GPIO 4 (`MEB_TWAI_TX_GPIO`) | TXD |
| GPIO 5 (`MEB_TWAI_RX_GPIO`) | RXD |
| 3.3 V / transceiver supply | VCC, according to the transceiver datasheet |
| GND | GND |
| Transceiver CANH | Vehicle CAN-H |
| Transceiver CANL | Vehicle CAN-L |

Use a transceiver that supports CAN FD and the required 500 kbit/s / 2 Mbit/s
timing. Confirm its logic-level and supply-voltage requirements before wiring
it to the ESP32-C5.

### Planned Convenience CAN wiring

Convenience CAN requires a second ISO 11898-2 transceiver and a second pair of
ESP32-C5 TX/RX GPIOs. Those GPIO assignments are deliberately **TBD** until the
development-board routing, boot strapping pins, LED, UART, and low-power wake
requirements have been checked together.

The second interface should support:

- Classical CAN at 500 kbit/s.
- A hardware standby/silent mode controlled by the ESP32.
- Low-current sleep and bus-wake behavior suitable for an installed device.
- A listen-only commissioning mode in firmware.
- 3.3 V-compatible logic levels.

Do not connect one transceiver to both vehicle buses and do not electrically
join EV-CAN and Convenience CAN.

The CAN bus connection is separate from the host interfaces:

- UART0 uses GPIO 11/12 at 115200 8N1 through the development board's UART
  USB connector.
- The native USB Serial/JTAG connector is used for flashing and debugging.
- BLE is an optional wireless command and telemetry interface.

## ID.4 Gateway Harness

The referenced ID.4 installation uses a pass-through harness at the original
40-pin gateway connector behind the glove compartment. The harness is placed
between the original gateway cable and the gateway connector, leaving the
vehicle wiring intact.

The currently validated preheating installation identifies these wires:

| Harness pin | Signal | Connect to |
| --- | --- | --- |
| 15 | CAN-L of CAN-EV | Transceiver CANL |
| 16 | CAN-H of CAN-EV | Transceiver CANH |
| 31 | Ground | Controller/transceiver GND |
| 11 | +12 V | A suitable protected power input, if used |

The original project reports the gateway and harness location as behind the
glove compartment. It powers its CAN interface from the vehicle's USB-C outlet
after installing the harness. For this ESP32 project, do **not** connect the
vehicle's +12 V directly to an ESP32 3.3 V rail or an unprotected development
board input; use an appropriate automotive-rated regulator or power the board
separately. A common ground is still required.

The pin numbers above come from the referenced harness notes and are not a
universal OBD-II pinout. They must be checked against the actual connector,
market, model year, and harness before applying power.

An independent ID.4 ICAS1 analysis labels gateway connector T40a pins 17 and
18 as Convenience CAN-L and CAN-H respectively. This is a useful research lead,
not a project-validated pinout. Confirm it in official wiring documentation
before designing or installing a two-bus harness. See the
[ICAS1 analysis](https://gorgias.me/posts/vw-id4-vehicle-control-analysis/).

## Installed Power and Sleep Behavior

The existing USB-C-powered development setup is not an always-on automotive
power design. A permanently installed MEB Commander needs, at minimum:

- A fused automotive-rated input stage and regulator.
- Reverse-polarity, transient, and load-dump protection appropriate to the
  selected vehicle supply connection.
- Defined behavior during brownout and vehicle cranking/service events.
- Low-quiescent-current regulators and CAN transceivers.
- A sleep/wake design that allows every vehicle bus to return to sleep.

Keeping Convenience CAN awake can discharge the vehicle's 12 V battery. The
device must stop periodic traffic, place transceivers into the appropriate
standby state, and verify that the vehicle network reaches sleep before the
hardware is considered suitable for unattended installation. Remote wake from
BLE, Wi-Fi, or cellular connectivity and vehicle-bus wake are separate design
problems and must both be validated.

## Command Transport Security

The current BLE GATT RX characteristic does not require encryption,
authentication, or bonding, and firmware OTA uses SHA-256 for integrity without
signed images or secure boot. This is insufficient for any feature that can
unlock the vehicle.

Before access-control commands are implemented, the hardware and firmware
design must provide authenticated and encrypted commands, replay protection,
rate limiting, auditable command results, secure key storage, signed firmware,
secure boot, and a fail-closed recovery path. Internet-range control also needs
a deliberately selected Wi-Fi or cellular transport; BLE alone is only a
short-range link.

## Connection and Commissioning Procedure

1. Build and test the ESP32/transceiver assembly away from the vehicle.
2. Keep `MEB_CAN_TEST_TX_ENABLED` set to `0`. Do not enable the temporary test
   transmitter on a vehicle bus.
3. Power the controller from a known, regulated supply and confirm that the
   transceiver is not driving the bus during initial wiring checks.
4. Install the pass-through harness at the gateway, with CAN-EV CAN-H and
   CAN-L going only to the EV-CAN transceiver.
5. Connect vehicle ground to controller/transceiver ground.
6. Leave the transceiver's 120 ohm termination disabled. The vehicle network
   already provides its termination; adding another resistor can distort the
   bus.
7. Check CAN-H/CAN-L polarity and continuity before powering the controller.
8. Power the controller, observe CAN diagnostics/telemetry, and only then
   enable heating deliberately.
9. Commission any future Convenience CAN interface separately in listen-only
   mode. Do not enable transmit while identifying wiring or traffic.

Do not connect to the orange high-voltage system. The CAN wires are low-voltage
communications wiring, but they are connected to safety-critical vehicle
controllers. Avoid shorting CAN-H or CAN-L to power or ground, and disconnect
the controller before changing vehicle wiring.

## Compatibility notes

The referenced installation was tested on pre-2024 MEB vehicles, including an
ID.4. Newer ID.4 software, battery revisions, markets, and other MEB vehicles
may change gateway filtering, connector details, frame availability, or
preheating behavior. Successful communication on one vehicle does not prove
compatibility with another.

The current firmware transmits diagnostic and heating requests on EV-CAN. It
is not a passive CAN logger. Door lock/unlock is not implemented. All CAN IDs,
payloads, bus-routing rules, and future access-control behavior are
safety-sensitive. Start with the vehicle stationary, keep a way to remove
power from the controller, and stop if CAN errors, unexpected vehicle behavior,
or loss of gateway communication occurs.
