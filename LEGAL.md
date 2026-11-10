# Legal and reverse-engineering notes

Not legal advice.  Laws differ by country; this is a summary of why this
project is structured the way it is.

## What this project does

`g7motion` is an **interoperability implementation**.  It:

- runs on a GameSir G7 Pro that the user owns,
- reads the controller's USB vendor reports (the same reports the official
  Windows app uses),
- creates a standard virtual DualShock 4 through Linux `uhid`.

It does **not**:

- distribute GameSir firmware, Windows binaries, or SDK files,
- decompile or copy the GameSir Nexus application,
- bypass a DRM/TPM/access-control system,
- modify the controller's firmware.

The controller is a device the user lawfully owns.  The project implements a
documented USB interface on the host side; it does not reproduce the
controller's internal code.

## Copyright

Protocol facts (report IDs, byte offsets, register addresses) are functional
interface information, not creative expression.  The wire format was
documented by the GPL-3 `g7ctl` project (`PROTOCOL.md`) and independently
confirmed on real hardware for this project.  No `g7ctl` source code or prose
is copied here.

The UHID DualShock 4 implementation is vendored from `ds4linux` under the MIT
license.  MIT is compatible with this project's GPL-3.0-or-later license; the
original MIT notice and copyright are preserved in
`third_party/ds4linux/LICENSE`.

## US

17 U.S.C. § 1201(f) contains an interoperability exception for circumventing
technological measures to identify and analyze the elements necessary to
achieve interoperability.  This project does not circumvent a technological
protection measure at all: it speaks the controller's normal USB protocol.
Reverse engineering for interoperability and the functional facts learned
from it have a long history of protection in US copyright law (e.g. the
idea/expression distinction and fair-use analysis).  No patent search has
been performed.

## EU

Directive 2009/24/EC Article 6 permits decompilation for the purpose of
achieving interoperability, subject to conditions.  This project did not
decompile GameSir software; it observed USB traffic and used published
protocol notes.  The same interoperability rationale applies.

## Trademarks

"GameSir", "G7 Pro", "DualShock", "PlayStation", "SDL", "Eden" and
"ScarletNX" are used descriptively to identify compatible hardware and
software.  This project is not affiliated with or endorsed by GameSir, Sony,
or any emulator project.  The virtual device reports Sony's DualShock 4
VID/PID because that is the identity SDL3 and the kernel's
`hid-playstation` driver require for a motion-capable gamepad; no physical
Sony product is copied or sold.

## Practical cautions

- Distributing this project is not the same as distributing GameSir's
  firmware; never include firmware images or Nexus installers.
- Do not use GameSir logos or imply endorsement.
- If you plan commercial distribution, have counsel review the applicable
  jurisdiction, patent landscape, and trademark use.
