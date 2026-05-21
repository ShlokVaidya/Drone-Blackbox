# Blackbox firmware

This firmware targets the Seeed XIAO SAMD21 carrier in this workspace.

## Wiring assumed from the schematic

- MPU6886 on I2C: `D4` / `D5`
- GPS on UART: `D6` / `D7`
- SD card on SPI: `D8` / `D9` / `D10`
- SD chip select: `D1`

## What it logs

The firmware writes one CSV row per IMU sample with:

- millisecond timestamp
- accelerometer in g
- gyroscope in deg/s
- temperature in C
- GPS fix flag
- latitude / longitude / altitude / speed / course / satellites / HDOP
- GPS UTC timestamp when available

## Build notes

This is a PlatformIO project. The `TinyGPSPlus` library is declared in `platformio.ini`.

If your SD socket CS net is routed differently on a hardware revision, change `kSdChipSelectPin` in `src/main.cpp`.

## Output

The firmware appends to `/flight.csv` on the SD card.
