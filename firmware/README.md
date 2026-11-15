# Radar firmware (Waveshare ESP32-S3-Touch-LCD-7)

ESP-IDF 5.4 + LVGL 8.4. The app (`components/radar_app`) is the same on the screen and in the Mac simulator. Its on-screen text is in French.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor     # USB-C port marked "USB"
```

Use the port marked **USB** (native USB of the ESP32-S3); the **UART** port is not wired to the console. The first time, the factory demo keeps that port off: unplug, hold **BOOT**, plug in, release. Our firmware keeps the USB/CAN switch on USB, so later flashes need no button.

If the board drops back into download mode after flashing (black screen), flash with a watchdog reset instead:

```sh
cd build && esptool.py --chip esp32s3 -p /dev/cu.usbmodem* -b 460800 --after watchdog_reset write_flash @flash_args
```

On first boot the Wi-Fi screen opens; the network is then remembered.

## Releases and over-the-air updates

Push a version tag and GitHub Actions builds the firmware and publishes it as a release:

```sh
git tag v1.2.0 && git push origin v1.2.0
```

The release holds `radar.bin` (the app, used for updates) and `radar-full.bin` (bootloader, partitions and app, to flash a new board with `esptool.py --chip esp32s3 write_flash 0x0 radar-full.bin`).

Every screen running a release checks the latest one at boot (after 3 min) and every night at 4:00, and on demand from the settings screen. A newer version is downloaded into the other OTA slot, then the screen restarts on it. The new firmware is kept once it has been online for a minute; if it crashes before that, the bootloader goes back to the previous one. Local builds (version from `git describe`, not `vX.Y.Z`) never update themselves.

## Mac simulator

```sh
cd sim && cmake -S . -B build -G Ninja && ninja -C build
./build/radar_sim            # live aircraft (adsb.lol), fake Wi-Fi
./build/radar_sim --demo     # frozen traffic, handy in the evening when the sky is empty
./build/radar_sim --wifi     # start with no saved network
```

The mouse acts as a finger. In the simulator, any 8-character password "connects".

## Layout

| Folder | Contents |
|---|---|
| `main/` | Startup, Waveshare board (RGB display, GT911 touch, CH422G), Wi-Fi, HTTPS, clock |
| `components/radar_app/` | LVGL UI, ADS-B and route data, base map, icons, fonts |
| `sim/` | SDL2 simulator: Mac platform (pthreads, libcurl, fake Wi-Fi) |
| `tools/` | `gen_assets.py` (map + icons), `gen_fonts.sh` (fonts with French accents) |

## Useful settings

- Screen upside down in the case: set `CONFIG_EXAMPLE_LVGL_PORT_ROTATION_180=y` in `sdkconfig.defaults`, then run `idf.py fullclean build`.
- Polling interval: `POLL_MS` in `components/radar_app/adsb.c` (10 s).

## Notes from the board bring-up

- Display, touch, Wi-Fi, HTTPS and live aircraft work on the real board.
- The CH422G pin EXIO5 selects USB (low) or CAN (high) on GPIO19/20: keep it low or the USB port disappears.
- The factory demo leaves a Wi-Fi network ("WSCSBZY") in NVS; scans pause auto-reconnect so it does not block them.
- Wi-Fi reception is weak with the module's PCB antenna: keep metal away from it in the enclosure.
- Routes come from adsbdb by callsign and are often stale: a route is shown only if the aircraft lies on it and flies toward the destination. Routes of every aircraft in view are prefetched over kept-alive HTTPS connections.
