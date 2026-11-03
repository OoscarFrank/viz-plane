# Radar firmware (Waveshare ESP32-S3-Touch-LCD-7)

ESP-IDF 5.4 + LVGL 8.4. The app (`components/radar_app`) is the same on the screen and in the Mac simulator. Its on-screen text is in French.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor     # board plugged in over USB-C
```

If flashing fails on one USB-C port, try the other one. On first boot the Wi-Fi screen opens; the network is then remembered.

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

## Not verified on the board yet

Everything builds and runs in the simulator, but nothing has run on the real screen yet: display, touch, Wi-Fi, HTTPS, smoothness and orientation still need testing once the board arrives.
