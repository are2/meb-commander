# Hardware and Vehicle Connection

This document describes the CAN hardware used by the ESP32 controller and the
intended connection to a Volkswagen ID.4. The vehicle wiring information is
based on the [meb-preheat project](https://github.com/jagheterfredrik/meb-preheat)
and its [connection notes](https://github.com/jagheterfredrik/meb-preheat/blob/main/NOTES.md).
Verify the connector and pinout against the wiring documentation for the exact
vehicle before connecting anything.

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
phase. See the [ID.4 service-training reference](https://esperformance.net/ssp/vw/SSP_718_EN.pdf).

### Which bus this project uses

This controller is intended for **CAN-EV / EV-CAN**, after the vehicle gateway,
where it can communicate with J840. It is not intended for the ordinary
Powertrain CAN, B-CAN, LIN, or an arbitrary OBD diagnostic pair.

The ID.4 wiring documentation has an important naming trap: CAN-EV may be
labelled “powertrain CAN bus” in wiring diagrams, while the actual drivetrain
Powertrain CAN is also labelled “powertrain CAN bus”. Use the connected control
units and the gateway connector pinout, not the label alone, to identify the
bus.

The firmware uses one CAN-FD controller with these settings:

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

## ESP32 controller wiring

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

The CAN bus connection is separate from the host interfaces:

- UART0 uses GPIO 11/12 at 115200 8N1 through the development board's UART
  USB connector.
- The native USB Serial/JTAG connector is used for flashing and debugging.
- BLE is an optional wireless command and telemetry interface.

## ID.4 gateway harness

The referenced ID.4 installation uses a pass-through harness at the original
40-pin gateway connector behind the glove compartment. The harness is placed
between the original gateway cable and the gateway connector, leaving the
vehicle wiring intact.

The referenced harness identifies these wires:

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

## Termination and connection procedure

1. Build and test the ESP32/transceiver assembly away from the vehicle.
2. Keep `MEB_CAN_TEST_TX_ENABLED` set to `0`. Do not enable the temporary test
   transmitter on a vehicle bus.
3. Power the controller from a known, regulated supply and confirm that the
   transceiver is not driving the bus during initial wiring checks.
4. Install the pass-through harness at the gateway, with CAN-EV CAN-H and
   CAN-L going to the transceiver.
5. Connect vehicle ground to controller/transceiver ground.
6. Leave the transceiver's 120 ohm termination disabled. The vehicle network
   already provides its termination; adding another resistor can distort the
   bus.
7. Check CAN-H/CAN-L polarity and continuity before powering the controller.
8. Power the controller, observe CAN diagnostics/telemetry, and only then
   enable heating deliberately.

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

This project transmits diagnostic and heating requests. It is not a passive
CAN logger, and the documented CAN IDs and payloads should be treated as
safety-sensitive. Start with the vehicle stationary, keep a way to remove
power from the controller, and stop if CAN errors, unexpected vehicle behavior,
or loss of gateway communication occurs.
