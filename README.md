# g7motion

Expose the GameSir G7 Pro's **actual gyroscope and accelerometer** as an
SDL3 gamepad sensor on Linux.

The controller's own firmware normally converts motion into a right stick or
mouse.  SDL3, Eden and ScarletNX never see a gyro that way.  `g7motion`
instead:

1. claims the G7 Pro's vendor USB interface (detaching `xpad`),
2. keeps the vendor heartbeat session alive,
3. reads report `0x10` at 500–1000 Hz,
4. creates a virtual **DualShock 4 v1** via `/dev/uhid`,
5. feeds it the raw sticks/buttons/triggers plus the raw IMU,
6. forwards the virtual pad's rumble output back to the physical grips.

The kernel's `hid-playstation` driver and SDL3's PS4 HIDAPI driver then expose
a real `SDL_SENSOR_GYRO` / `SDL_SENSOR_ACCEL` source.  No SDL patch, no Nexus,
no dongle.

## Quick start

```sh
git clone https://github.com/VIGO-VI/g7motion.git
cd g7motion
```

### 1. Dependencies

| Distribution | Command |
| --- | --- |
| Arch | `sudo pacman -S --needed base-devel cmake pkgconf libusb sdl3` |
| Debian/Ubuntu | `sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev libsdl3-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake pkgconf-pkg-config libusb1-devel SDL3-devel` |

Only `libusb-1.0` and CMake are required.  `SDL3` is optional; without it the
`sdl3_probe` verification tool is not built.

### 2. Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

### 3. Grant access to the controller and `/dev/uhid`

```sh
sudo install -Dm644 udev/61-g7motion.rules /etc/udev/rules.d/61-g7motion.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Replug the controller once.  After this the bridge runs as your user; `sudo`
is not needed.

### 4. Check the controller read path

```sh
./build/g7motion --probe
```

This prints sticks, buttons, gyro, accel and battery for five seconds.  It
proves USB access and the vendor protocol before any virtual device exists.

### 5. Start the bridge

```sh
./build/g7motion
```

While it runs, the physical `xpad` device is detached and the virtual
`GameSir G7 Pro (Virtual DualShock 4)` is the only gamepad.  Stop it with
Ctrl+C to get the physical controller back.

For daily use, install the user service instead (section below).

### 6. Verify the IMU

With the bridge running:

```sh
./build/sdl3_probe
```

Expected output:

```text
SDL3 3.x, 1 gamepad(s)
  [0] PS4 Controller  gyro=1 accel=1
gyro ok   0.0000   0.0000   0.0000 rad/s   accel ok  -0.0156   9.9300   0.0694 m/s^2
```

Rotate the controller: gyro changes.  Hold it still: accelerometer magnitude
stays near 9.8 m/s².  If you only see `gyro=0 accel=0`, the bridge is not
running or `/dev/uhid` is not writable.

### 7. Verify rumble

Build the small force-feedback test and point it at the virtual pad's event
node:

```sh
cc -O2 -Wall -Wextra -o build/rumble_test tests/rumble_test.c

# find the node: the virtual pad has PROP=0 and ABS=3003f
grep -B4 -A6 'GameSir G7 Pro (Virtual DualShock 4)' /proc/bus/input/devices

./build/rumble_test /dev/input/eventN
```

Both grips should buzz for one second.  This exercises the full path:
evdev FF → UHID output report → 13-byte GIP packet → physical motors.

### 8. Persistent user services (recommended)

```sh
./scripts/install-user-service.sh
```

It installs `g7motion` and `g7bt` into `~/.local/bin` plus their systemd user
units, and enables both.  `g7motion` handles USB (IMU, battery, rumble);
`g7bt` idles until the pad appears over Bluetooth.  Both start at login, wait
for the controller, and recreate their virtual device after replug or reboot.

```sh
systemctl --user status g7motion
journalctl --user -u g7motion -f
```

To start it before login, enable lingering once:
`sudo loginctl enable-linger "$USER"`.

## Why a virtual DS4?

The G7 Pro answers the vendor protocol only on interface 0, which `xpad`
owns.  Claiming it for IMU access necessarily removes the physical gamepad.
The virtual DS4 replaces it with one device that carries both input and
motion, so emulators see a single, normal, motion-capable controller.

This reuses the UHID DualShock 4 implementation from
[ds4linux](https://github.com/PalashDalsaniya/ds4linux) (MIT), including its
Sony calibration feature report.  The rest of ds4linux is not used.

The Sony VID/PID is required for SDL3 and Steam to select the PS4 HIDAPI
driver; only the product string is changed so desktop battery/power UIs do
not present the pad as a Sony device.

## Steam Input

Steam sees the virtual DualShock 4 through its HIDAPI driver and uses the
normal PS4 mapping, including gyro origins.  Steam's log should contain:

```text
Controller using HIDAPI driver, vid=0x054c, pid=0x05c4
```

Verify:

1. `systemctl --user status g7motion` is active.
2. Steam → Settings → Controller → Test/Calibration.
3. Select the **PS4 Controller** and rotate the pad.
4. For a game: Controller Layout → Gyro, or use Steam's
   `controller_ps4_gamepad_mouse_gyro.vdf` template.

Steam's gyro graph is in °/s and deliberately sensitive: a slow rotation can
show hundreds of °/s.  At rest the G7 Pro reads a few °/s of bias; click
**Calibrate** once while the pad lies flat, or enable auto-calibrate.

Steam's controller-test **Rumble** button only emitted LED/zero-motor output
reports for this virtual pad, so there is nothing to forward from that button.
The forwarding path itself is verified with `rumble_test`, and in-game rumble
works.  `LED` has no physical target: the G7 Pro's Xbox button ring is not a
DualShock lightbar and has no arbitrary-colour vendor command.

## Bluetooth

Bluetooth uses a different identity (`3537:1022`): a generic HID gamepad with
no IMU, no battery telemetry, and no force feedback in its descriptor.  Gyro
and battery therefore remain wired-only.

Rumble does work.  The firmware accepts the standard Xbox One output report on
the hidraw node even though it never advertises it:

```text
03 0F LT RT LM RM
```

`g7bt` grabs the kernel's gamepad node, re-exports it as a virtual
**Xbox 360** pad (`045e:028e`) with `FF_RUMBLE`, forwards input 1:1, and turns
play/stop requests into that report.  Because the physical node is grabbed,
games and Steam see only the virtual pad.

```sh
./build/g7bt            # or: systemctl --user start g7bt
```

Verify:

```sh
cc -O2 -Wall -Wextra -o build/rumble_test tests/rumble_test.c
grep -B4 -A6 'GameSir G7 Pro (Bluetooth)' /proc/bus/input/devices   # note FF=
./build/rumble_test /dev/input/eventN
```

Both grips buzz.  The trigger channels (`LT`/`RT`) are accepted by the pad but
produced no perceptible vibration on the tested unit.

Notes:

- `g7bt` needs write access to the Bluetooth hidraw node; the udev rule above
  covers it.  Reinstall the rule after updating this repository.
- No IMU is exposed in this mode, so Steam's gyro origins will be inactive.
- The installer adds `~/.config/environment.d/90-g7bt.conf`, which sets
  `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x3537/0x1022`.  The grabbed original is
  still enumerated by SDL and would otherwise show up as a second, dead pad.
  Log out and back in (or restart Steam) for it to take effect.  If `g7bt`
  is not running, that also hides the pad from SDL; remove the file to undo.

For a native solution (FF on the real device, no virtual pad), see
`upstream/BT-RUMBLE.md` in
[g7pro-kernel](https://github.com/VIGO-VI/g7pro-kernel).

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

- **Wired only for IMU and battery.**  Bluetooth exposes a generic HID
  gamepad: no vendor interface and no battery telemetry.  Rumble over
  Bluetooth is provided by `g7bt` through an undeclared output report.
- **Four paddles:** only L4 and Share map to the DS4 touchpad click.
- **Native/GIP identity `3537:1022` is not supported.**  Hold Share+Menu to
  return to the configurable identity.
- **Gyro axis orientation** is passed straight through.  If your unit needs
  yaw/roll swapped or inverted, patch `to_ds4()` (one line per axis).
- The G7 Pro is unavailable to games while the bridge runs; that is the
  cost of exclusive ownership of its vendor interface.

## Troubleshooting

- `--probe` says permission denied: reinstall the udev rule, then replug.
- `sdl3_probe` shows no gamepad: check `systemctl --user status g7motion`
  and `ls -l /dev/uhid`.
- The controller stops being a gamepad after a crash: the physical device
  returns as soon as the bridge exits; replug if the interface stays claimed.
- Kernel `xpad` reclaims the device: stop the bridge, unplug/replug, then
  start it again.

## Protocol credit

The G7 Pro report layout was documented by
[g7ctl](https://github.com/questionablesyntax/g7ctl) (`PROTOCOL.md`):
report `0x10`, marker `0x3C`/`0xE0`, sticks 5–8, triggers 12–13, gyro
17–22, accel 23–28, battery 32–33, raw buttons 55–58.

## License

GPL-3.0-or-later for this project.  The vendored UHID DS4 code from
ds4linux remains MIT (GPL-compatible); see `third_party/ds4linux/LICENSE`.
Reverse-engineering and interoperability notes are in `LEGAL.md`.
