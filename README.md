# Minecraft Sword Controller

A wooden toy sword with an ESP32-S3 inside that plays Minecraft. It pairs with a computer over Bluetooth Low Energy as a standard keyboard and mouse (BLE HID) named **"Minecraft Sword"**, so the game sees ordinary input and no mods are needed. The author's setup has been tested in game with all five buttons, swing and camera aiming. It was built and tested with TLauncher.

- **Swing** the sword to attack.
- **Hold the aim button** and turn the sword to steer the camera.
- **Push buttons** handle walk, jump, use and hotbar switching.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 DevKit | PlatformIO board `esp32-s3-devkitc-1` |
| MPU-6500 / MPU-9250 IMU module | I2C address `0x68`; `WHO_AM_I` reads `0x70` on an MPU-6500 |
| 5 push buttons | Wired to GND, internal pull-ups, no resistors needed |
| USB cable / power bank | Power |

## Pin map

| Function | GPIO | Behaviour |
|---|---|---|
| IMU SDA | 8 | I2C data |
| IMU SCL | 9 | I2C clock |
| Aim | 4 | Hold: gyro moves the camera, swing detection paused |
| Jump | 5 | Space (held) |
| Walk | 7 | `W` key (held) |
| Hotbar next | 10 | One mouse-wheel step per press |
| Use | 11 | Left mouse button (held) |
| BOOT (on board) | 0 | Also acts as an aim button |

Buttons are active-low: one leg to the GPIO, the other leg to GND. On 4-leg tactile buttons use diagonally opposite legs, otherwise the pin reads as permanently pressed.

IMU wiring: `VCC -> 3V3`, `GND -> GND`, `SDA -> GPIO8`, `SCL -> GPIO9`. Leave NCS, ECL, EDA, FSYNC, INT and AD0 unconnected. Mount the IMU firmly on the sword grip, since a loose mount causes drift and false swings.

## Controls

| Input | Output |
|---|---|
| Swing (gyro magnitude above threshold) | Right click, the attack in the author's game bindings |
| Use button | Left click, held |
| Walk button | `W`, held |
| Jump button | Space, held |
| Hotbar button | Scroll one step to the next slot |
| Aim button | Gyro yaw (Z) and pitch (X) move the mouse |

A, S, D and any other keys come from the regular keyboard. If your game uses the default bindings (left click attacks), swap `MOUSE_LEFT` and `MOUSE_RIGHT` in `src/main.cpp`.

## How it works

1. On startup the firmware reads the IMU `WHO_AM_I` register and calibrates the gyro for about 2 seconds. **Keep the sword still during this time**, or the camera will drift.
2. It then starts BLE and advertises as "Minecraft Sword".
3. The main loop runs at about 100 Hz. It reads the gyro (±500 dps) over raw I2C, debounces the buttons (30 ms) and sends the HID reports.
4. A swing is detected when the gyro magnitude exceeds `SWING_THRESHOLD_DPS`. The click is held for 150 ms, followed by a 350 ms cooldown.
5. A button that is already pressed at boot is ignored until it has been released once, so a wiring fault cannot spam keys.

## Build and upload

Requires [PlatformIO](https://platformio.org/).

```
pio run -t upload
pio device monitor
```

- Library: [blackketter/ESP32-BLE-Combo](https://github.com/blackketter/ESP32-BLE-Combo), fetched automatically from `platformio.ini`. It provides a relative-motion mouse, which the cursor and camera require.
- Serial monitor runs at 115200 baud. It prints the `WHO_AM_I` value, the gyro offsets, button state changes, `SWING (N dps)` on each detected swing and a periodic peak-gyro reading.
- Only one program can open the COM port at a time, so close the monitor before uploading.
- After changing the library or the HID descriptor, remove "Minecraft Sword" in the Bluetooth settings and pair it again.

## Tuning

Constants at the top of `src/main.cpp`:

| Constant | Default | Purpose |
|---|---|---|
| `SWING_THRESHOLD_DPS` | 380 | Raise for false swings, lower for missed swings |
| `SWING_COOLDOWN_MS` | 350 | Minimum time between swings |
| `DEADZONE_DPS` | 8 | Gyro noise ignored while aiming |
| `CAM_SENSITIVITY` | 3.5 | Mouse counts per degree; higher is faster |
| `YAW_SIGN` | -1 | Set to 1 to reverse left/right camera |
| `PITCH_SIGN` | -1 | Set to 1 to reverse up/down camera |
| `DEBOUNCE_MS` | 30 | Button debounce time |
| `TEST_ALWAYS_AIM` | false | Gyro always moves the cursor (testing only) |

## Troubleshooting

| Symptom | Check |
|---|---|
| Upload fails, no COM port | Install the CP210x driver if your board uses that chip, try another cable or the other USB port |
| `WHO_AM_I = 0xFF` | SDA/SCL/3V3/GND wiring and pins GPIO8 and GPIO9 |
| Camera drifts | Keep the sword still for 2 seconds after reset |
| Camera direction reversed | Flip `YAW_SIGN` or `PITCH_SIGN` |
| Button always pressed | Wrong legs on the button, or a short to GND |
| Cursor does not move | Re-pair the device after any library change |
| Bluetooth will not connect | Remove the device in Windows, reset the board, pair again |

## Known limitations and to-do

- The return stroke of a swing can trigger a second hit; a forward-stroke-only filter is planned.
- The swing click uses a blocking 150 ms delay.
- No joystick support in the S3 firmware.
- Wireless power (power bank or LiPo with charger) and mounting the electronics inside the sword are still to do.

## Project layout

```
platformio.ini   PlatformIO environment and library dependency
src/main.cpp     Firmware
README.md        This file
```
