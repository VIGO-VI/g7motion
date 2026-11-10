# g7motion

Expose the GameSir G7 Pro's **actual gyroscope and accelerometer** as an
SDL3 gamepad sensor.

The controller's own firmware normally converts motion into a right stick or
mouse.  SDL3, Eden and ScarletNX never see a gyro that way.  `g7motion`
instead:

1. claims the G7 Pro's vendor USB interface (detaching `xpad`),
2. keeps the vendor heartbeat session alive,
3. reads report `0x10` at 500–1000 Hz,
4. creates a virtual **DualShock 4 v1** via `/dev/uhid`,
5. feeds it the raw sticks/buttons/triggers plus the raw IMU.

The kernel's `hid-playstation` driver and SDL3's PS4 HIDAPI driver then expose
a real `SDL_SENSOR_GYRO` / `SDL_SENSOR_ACCEL` source.  No SDL patch, no Nexus,
no dongle, no Bluetooth.

## Why a virtual DS4?

The G7 Pro answers the vendor protocol only on interface 0, which `xpad`
owns.  Claiming it for IMU access necessarily removes the physical gamepad.
The virtual DS4 replaces it with one device that carries both input and
motion, so emulators see a single, normal, motion-capable controller.

This reuses the UHID DualShock 4 implementation from
[ds4linux](https://github.com/PalashDalsaniya/ds4linux) (MIT), including its
Sony calibration feature report.  The rest of ds4linux is not used.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Dependencies: `libusb-1.0`, CMake, a C++20 compiler.  `sdl3` is optional and
only builds the verification probe.

## Test the controller read path

```sh
./build/g7motion --probe
```

This needs read/write access to the GameSir USB device.  Install the udev rule
once (see below), replug, and run it as your user.  It prints sticks, buttons,
gyro, accel and battery for five seconds.

## Run

`/dev/uhid` is normally root-only, so either run with `sudo`:

```sh
sudo ./build/g7motion
```

or install the udev rule and run as your user:

```sh
sudo install -Dm644 udev/61-g7motion.rules /etc/udev/rules.d/61-g7motion.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then:

```sh
./build/g7motion
```

While it runs, the physical `xpad` device is detached and the virtual
"DualShock 4" is the only gamepad.  Stop it with Ctrl+C to get the physical
controller back.

### systemd

```sh
sudo install -Dm755 build/g7motion /usr/local/bin/g7motion
sudo install -Dm644 systemd/g7motion.service /etc/systemd/system/g7motion.service
sudo systemctl daemon-reload
sudo systemctl enable --now g7motion
```

Logs: `journalctl -u g7motion -f`.

## Verify it is real SDL3 gyro

With `g7motion` running:

```sh
./build/sdl3_probe
```

It should print the virtual pad with `gyro=1 accel=1`, then live values in
`rad/s` and `m/s²`.  Rotate the controller; gyro changes.  Keep it still;
accel stays near 9.8 m/s².

Verified on the reference G7 Pro (3537:109b, wired): SDL3 3.4.18 reports
`gyro=1 accel=1`, stationary accelerometer ≈ 9.9 m/s², and live gyro values.

In Eden/ScarletNX the controller appears as a **DualShock 4** with motion.
Select SDL as the motion source (not mouse-as-motion), then map the gyro in
the emulator's controller settings.

## Mapping

| G7 Pro | Virtual DS4 |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 (analog + digital) |
| View / Menu | Share / Options |
| L3 / R3 | L3 / R3 |
| Guide | PS |
| Share (Capture) | Touchpad click |
| L4 | Touchpad click |
| L5 / R5 / R4 | not mapped |
| D-pad | D-pad |

Sticks are the controller's processed/calibrated values.  Buttons come from
the raw pre-remap group, so on-board remaps are not applied twice.

IMU values are passed through raw.  The G7 Pro uses the same scales as a
DS4: gyro 16 counts per °/s, accelerometer 8192 counts per g.  The virtual
DS4 calibration report matches those scales.

## Limitations

- **Wired only.**  Bluetooth mode does not expose the vendor interface.
- **No rumble/LED forwarding yet.**  The virtual DS4's output reports are
  drained but not sent to the G7 Pro.
- **Four paddles:** only L4 and Share map to the DS4 touchpad click.
- **Native/GIP identity `3537:1022` is not supported.**  Hold Share+Menu to
  return to a configurable identity.
- **Gyro axis orientation** is passed straight through.  If your unit needs
  yaw/roll swapped or inverted, patch `to_ds4()` (one line per axis).
- The G7 Pro is unavailable to games while the bridge runs; that is the
  cost of exclusive ownership of its vendor interface.

## Protocol credit

The G7 Pro report layout was documented by
[g7ctl](https://github.com/questionablesyntax/g7ctl) (`PROTOCOL.md`):
report `0x10`, marker `0x3C`/`0xE0`, sticks 5–8, triggers 12–13, gyro
17–22, accel 23–28, battery 32–33, raw buttons 55–58.

## License

GPL-3.0-or-later for this project.  The vendored UHID DS4 code from
ds4linux remains MIT (GPL-compatible); see `third_party/ds4linux/LICENSE`.
Reverse-engineering and interoperability notes are in `LEGAL.md`.

## Persistent user service

The bridge can run as a systemd user service; it starts at login, waits for the
controller and recreates the virtual DS4 after replug/reboot.

```sh
./scripts/install-user-service.sh
```

Or manually:

```sh
install -Dm755 build/g7motion ~/.local/bin/g7motion
install -Dm644 systemd/g7motion.user.service ~/.config/systemd/user/g7motion.service
systemctl --user daemon-reload
systemctl --user enable --now g7motion
```

Logs: `journalctl --user -u g7motion -f`.  If you need it before login,
`sudo loginctl enable-linger "$USER"`.

## Steam Input

Steam sees the virtual DualShock 4 through its HIDAPI driver and uses the
normal PS4 controller mapping.  The gyro/accelerometer are the DS4 sensor
stream, so Steam Input's PS4 gyro origins work without extra software.

Verify:

1. Start the bridge (`systemctl --user status g7motion` should be active).
2. Open Steam → Settings → Controller → Test/Calibration.
3. Select the **PS4 Controller** and rotate the pad; gyro should respond.
4. For a game: Controller Layout → Gyro, or use Steam's
   `controller_ps4_gamepad_mouse_gyro.vdf` template.

Steam's controller log should contain `Controller using HIDAPI driver,
vid=0x054c, pid=0x05c4`.
