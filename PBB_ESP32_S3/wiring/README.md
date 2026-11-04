# ESP32-S3 Wiring Reference

This directory contains the wiring reference for the ESP32-S3 version of the **Planetary Bean Bouncer Coffee Roaster**.

The diagram documents the connections between the 44-pin ESP32-S3 development board, controller peripherals, stepper motor drivers, temperature sensors, and touchscreen display.

## Included Files

- `PBB_ESP32_S3.fzz` — Editable Fritzing project file.
- `PBB_ESP32_S3_bb.png` — PNG export of the wiring diagram for viewing without Fritzing.

## Wiring Diagram

The wiring diagram covers the following components:

- ESP32-S3 44-pin development board
- 4-inch TFT touchscreen with SPI interface
- Capacitive touch controller
- SD card interface
- X and Y TMC2209 stepper motor drivers
- MAX6675 thermocouple modules
- SHT31 temperature and humidity sensor
- DS3231 real-time clock
- EC11 rotary encoder
- Buzzer
- Optional limit switch and Y-axis motor connections

## GPIO Assignments

The diagram includes the ESP32-S3 GPIO assignments for the connected peripherals, along with notes identifying optional components, reserved pins, and alternate functions.

These assignments reflect the current Planetary Bean Bouncer hardware configuration. Changing or removing optional components may make some GPIO pins available for other uses.

Pay particular attention to the reserved and boot/strapping pin notes before making wiring changes.

## Notes

- The diagram represents the current ESP32-S3 controller configuration.
- Optional components are identified in the diagram.
- Stepper motor power and controller power connections should be made according to the build guide.
- Verify all connections before applying power.
- Use the editable Fritzing project if you need to inspect or modify the wiring.

For complete assembly instructions, component details, and controller setup, refer to the ESP32-S3 build guide and bill of materials in the `docs/` directory.