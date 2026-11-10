# Vendored from ds4linux

Minimal subset of [PalashDalsaniya/ds4linux](https://github.com/PalashDalsaniya/ds4linux)
(MIT) used only for its UHID DualShock 4 virtual device:

- `daemon/src/virtual_device.cpp`
- `daemon/include/ds4linux/virtual_device.h`
- `daemon/include/ds4linux/input_device.h` (types only)
- `common/include/ds4linux/constants.h`

Upstream commit: 2026-10-31 (`main`).
The rest of ds4linux (libevdev input, profiles, GUI, IPC) is not used.
