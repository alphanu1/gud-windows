# gud-windows — WIP

**The wire is proved on hardware. The driver is not.** `gudprobe` enumerates a
real device, sets modes and puts pixels on a CRT, including timings the device
never advertised. The IddCx driver builds, installs, loads and runs its own code
in the UMDF host, and stops one call short of a picture. Read *Status* before
trusting any of it.

---

A Windows host driver for Generic USB Display devices.

## What it is for

**Switchres generates a modeline per game, and that modeline reaches the
hardware.** The device solves its own PLL for the pixel clock, reconfigures, and
scans out the timing it was given. Not a resolution picked off a list — the
actual porches, totals and clock the game wants.

That already works on Linux. Switchres adds the modeline to a DRM connector, the
in-tree `gud` driver puts it on the wire whole in `SET_STATE_CHECK`, and the
device retunes. Modes that were never advertised and never compiled into the
fabric come up on a CRT because the board synthesises the clock for them:
blitsCRT_Mister reaches PAL 640x576i50 at 12.500 MHz that way, and its solver is
within 6 ppm across real console and arcade timings.

Windows has no equivalent path, and that is the gap this closes. Same board, no
firmware change, per-game timings on a 15 kHz CRT from a Windows host.

## Why it needs writing at all

GUD is a wire protocol, not a Linux one. A device implementing it needs the host
to speak request codes, a mode structure and a buffer format, and nothing about
the device depends on what is at the other end. So a Windows host driver is a
peer of the Linux one, not a port of it, and it is a driver for *any* GUD
device — a Pi Zero adapter, the STM32 reference device, anything answering the
protocol.

Switchres is the motivating case and not the scope. Everything above the
transport asks the device instead of assuming: `GET_FORMATS` picks the pixel
format, `GET_CONNECTOR_MODES` fills the mode list, the descriptor bounds are
checked, and `FULL_UPDATE` and `STATUS_ON_SET` are both honoured. A device with a
fixed panel and a sensible advertised list needs no configuration and never
touches the modeline store — plug it in and its modes are the modes Windows
offers. The store exists because some devices synthesise their own timing, not
because this one does.

What is left that is device-specific is the VID:PID in both INFs and in
`gudprobe.c`, and a format preference tuned to a six-bit DAC. `docs/DESIGN.md`
has the list and keeps it short deliberately.

No EDID is sent, to any device, and that one is not going to move. An EDID
carries a mode list the store knows nothing about, and the commit path refuses
modes it has no modeline for — the safety net working. If some device turns out
to need one it is a runtime option, not a new default.

Nothing like it exists. The GUD ecosystem is entirely Linux: the in-tree driver
since 5.13, the gadget side, Raspberry Pi images. Every IddCx sample is a
*virtual* display — it enumerates a monitor and discards the frames — so the
frame loop is demonstrated and the transport is not.

Screen capture through the Desktop Duplication API would be a fraction of the
work and is not an option: it cannot switch resolution per game, which is the
whole point.

## How a modeline gets to the device

This is the one hard problem, and it is the reason the driver carries something
no other indirect display driver has.

IddCx has nowhere to put a modeline. Its vocabulary is a monitor mode list and
EDID: width, height, and a refresh rate as a rational. Porches do not appear
anywhere in the API, because a normal indirect display does not generate timing
and has no use for them. This one does.

So the driver keeps its own table, and every mode it reports to Windows is
backed by a full modeline. At commit time the lookup runs the other way: Windows
says "640x480 at 59.94", the table says which modeline that was, and that
modeline goes on the wire intact — clock, both sync starts and ends, both
totals, interlace. The device sees exactly what it would have seen from Linux.

Two sources fill the table:

1. **The device's own advertised list**, from `GET_CONNECTOR_MODES`. Every GUD
   device has one and every entry carries full timing, so the driver works with
   no configuration at all — plug the board in and the modes it advertises are
   the modes Windows offers.

2. **A modeline store**, `C:\ProgramData\gud-windows\modelines.ini`, in the
   X11 and Switchres spelling. Loaded second, so a hand-written entry overrides
   an advertised one with the same geometry and rate. This is where a Switchres
   backend deposits generated modelines.

The driver is the *sink*, not a scavenger, and that is deliberate. There is
nowhere a Windows Switchres leaves a modeline to be found: the existing backends
`adl` and `powerstrip` push timing into a graphics driver through a vendor
interface and leave nothing behind. Writing to a known destination is the shape
`drmkms` already has, so the backend is small.

The lookup key is exact — pixel clock, both totals, active size, interlace — not
a refresh-rate match. `DISPLAYCONFIG_VIDEO_SIGNAL_INFO` hands those numbers
straight back because the driver put them there. Matching on refresh alone
cannot separate 59.94 from 60.00 with any tolerance wide enough to survive
Windows' rounding, and returning the wrong modeline puts wrong timing on a
fixed-frequency deflection circuit.

---

## Status

Worst first.

### Blocked — one call, in the driver

`WdfUsbTargetDeviceCreate` and `WdfUsbTargetDeviceCreateWithParameters` both
return `STATUS_INVALID_PARAMETER` from `EvtDevicePrepareHardware`. Everything
before it succeeds. The driver's own log, which is what established this:

```
DriverEntry: WdfDriverCreate            -> 0x00000000
EvtDriverDeviceAdd: IddCxDeviceInitConfig -> 0x00000000
EvtDriverDeviceAdd: WdfDeviceCreate       -> 0x00000000
EvtDriverDeviceAdd: IddCxDeviceInitialize -> 0x00000000
EvtDevicePrepareHardware: entered
  WdfUsbTargetDeviceCreateWithParameters  -> 0xC000000D
  WdfUsbTargetDeviceCreate (plain)        -> 0xC000000D
```

Ruled out, each by a deploy: the callback it is called from, both API variants,
`UmdfDispatcher` as `WinUsb` and as `NativeUSB`, and winusb.sys present as a
lower filter (verified in the device's `LowerFilters`, and it changed nothing).

The remaining hypothesis is that `IddCxDeviceInitConfig` transforms the
`PWDFDEVICE_INIT` into something whose `WDFDEVICE` does not support USB
targets. Two experiments settle it and neither has been run:

1. Comment out `IddCxDeviceInitConfig`/`IddCxDeviceInitialize`, keep the rest.
   If USB then works, IddCx and USB cannot share one device object and the
   one-package design in `docs/DESIGN.md` needs revisiting.
2. Build `BRINGUP.md` step 4's minimal driver — root-enumerated, no USB, one
   hardcoded mode. A known-good IddCx baseline to add USB to.

Skipping step 4 to go straight at the full driver on a USB PDO is why these are
tangled, and it was a mistake.

### Fixed, and worth not repeating

All four originally-listed semantic bugs are fixed: the context lookup in every
callback now goes through `AdapterContext`/`MonitorContext` back-pointers
attached via `ObjectAttributes`, the device context has an `EvtCleanupCallback`
so the frame thread is joined, and adapter re-initialisation on resume is
refused. Alongside those, found by building and running rather than by reading:

- `EvtIddCxParseMonitorDescription` is **mandatory**. Without it
  `IddCxDeviceInitConfig` fails `STATUS_INVALID_PARAMETER` and UMDF reports only
  "failed to load the driver at level 0".
- **UMDF 2.33 is a Windows 11 framework.** On Windows 10 19045 it compiles,
  signs and installs and then cannot load. 2.31 is the ceiling there.
- `Include = WUDFRD.inf` / `Needs = WUDFRD.NT` is what the documentation shows
  and **WUDFRD.inf does not exist on Windows 10 19045**. Register the reflector
  explicitly with `AddService = WUDFRd,0x000001fa,...`.
- The UMDF log channel that names any of this,
  `Microsoft-Windows-DriverFrameworks-UserMode/Operational`, ships **disabled**.
  Turn it on first: `wevtutil sl <channel> /e:true`.

### Compiled against stubs — the driver

Both translation units build to objects under `-Wall -Wextra -Wshadow
-Wcast-qual -Wconversion`, no warnings. `IddCx.h` and `wdf.h` are stubbed in
`tests/wdkstub`, written from the documented API; the D3D11, DXGI and
DISPLAYCONFIG code is checked against Microsoft's real headers, which mingw
ships.

That pass found a returned dangling pointer in the swapchain setup, bare
`min`/`max` relying on the windows.h macros, an implementation-defined
multi-character pool tag, and a misspelled field. None would have been found by
reading it.

The stubs were guesses, and the driver has since had its first pass against the
real WDK. Five things differed, in ascending order of consequence: `wdfusb.h`
is a separate header, it needs `usbspec.h` and `usb.h` ahead of it, `NOMINMAX`
was never defined so windows.h's macros mangled `std::min`,
`IDARG_IN_COMMITMODES_PATH` does not exist (paths are `IDDCX_PATH`, and
`IDDCX_TARGET_MODE` wraps its signal info one level deeper than `IDDCX_PATH`
does), and **`IDDCX_METADATA` carries no rect arrays at all** — dirty rects and
move regions come from `IddCxSwapChainGetDirtyRects`/`GetMoveRegions` into
caller-allocated arrays. The frame loop was rebuilt for that.

Two of those passed `make windows` without complaint, which is the limit of
that check: mingw defines no `min`/`max` macros and the stubs invented the
metadata shape. A syntax pass proves the code is self-consistent, not correct.

`UmdfExtensions` and `UmdfLibraryVersion` no longer need copying from anywhere.
`IddCx0102` is right; `UmdfLibraryVersion` must be **2.31.0** on Windows 10 —
see *Blocked* above.

### Runs on hardware — `gudprobe.exe`

Builds with `build.cmd` (Windows SDK, no WDK) and cross-builds with mingw.
Enumerates a real blitsCRT_Mister, sets modes, and puts pictures on a 15 kHz
CRT.

**`gudprobe` is not a driver.** It talks to the device directly over WinUSB.
Nothing appears in Display Settings; Windows does not know a display exists. It
prints the mode list, sets a mode — including an arbitrary modeline off the
command line — and puts a picture on the CRT. That is the point: it proves the
wire, and the modeline path, without IddCx in the way.

```
gudprobe --modeline "7.560 384 400 432 480 224 227 230 262 -hsync -vsync"
```

That timing is in no advertised list. The device solves the PLL for 7.560 MHz
and scans it out. **This works.** It was the point of the tool and it is the
point of the project, and it ran before any of the driver did.

Measured on a real device at 648x480i60, RGB565:

| | |
| --- | --- |
| full surface, LZ4 | 1.59 ms |
| full surface, raw | 22.38 ms |
| full surface at 384x224 | 0.85 ms |
| one 64x48 damage rect | 0.83 ms, stable over 600 |

A 16.7 ms field makes the raw figure **134% of the budget**, so compression is
not an optimisation here, it is a requirement for full-frame updates. The
bandwidth arithmetic in `docs/DESIGN.md` argued it was worth having; the
measurement says it is load-bearing.

### Tested — everything above the transport

- `gud_host.c` — enumeration, modeset, flush, the `STATUS_ON_SET` handshake
- `modeline.c` — store, parser, both lookups, INI override
- `convert.c` — BGRA to the wire formats, checked byte-for-byte through the flush path
- `lz4enc.c` — LZ4 block compression

Roughly 1100 of 3800 lines, and it is the part that would otherwise be debugged
on a CRT.

---

## Tests

```
make test DEVICE=../blitsCRT_Mister/sw
```

Neither suite uses a mock.

`test_lz4` runs the compressor against blitsCRT_Mister's own `lz4dec.c` — the
pair that will actually meet on the wire, rather than a reference
implementation. Every length across the encoder's boundary cases, offset-1 runs,
long-range duplicates, and 4000 randomised blocks. Incompressible input must
expand rather than corrupt, so the caller can notice and send raw.

`test_loopback` links the device's own `device.c`, runs it headless, and routes
`gud_transport` straight into `blitscrt_handle_ctrl`. Both protocol
implementations are under test at once, so a disagreement about structure
layout, request codes or the status handshake shows up there rather than on a
CRT. Among other things it sets an unadvertised 384x224p60 modeline and checks
the device accepts it, and sets a 1080p60 one and checks the device refuses it.

It has earned its keep several times over: it caught a lookup key that could not
separate 59.94 from 60.00, a `vSyncFreq` missing its interlace doubling (which
would have reported 480i to Windows as 30 Hz), an out-of-band test modeline the
device correctly refused, and an INI key/value split that rejected every line in
the example file.

It also has a blind spot worth naming, because it cost a night. The harness
routes `gud_transport` straight into `blitscrt_handle_ctrl`, so **no
`bmRequestType` is ever constructed**. The byte was wrong — `0x40`, recipient
device, where GUD requires `0x41`, recipient interface — and 93 passing checks
said nothing, because the transport is exactly what the loopback replaces. On
Linux the gadget core routes control requests by recipient before the function
sees them, so every request went to the composite `setup()` and was stalled
there while the device enumerated perfectly. Only hardware could find it.
Anything below `struct gud_transport` needs a device.

```
make windows        # cross-compile gudprobe.exe, compile the driver
make golden         # regenerate docs/expected-gudprobe-output.txt
```

`make windows` needs mingw-w64. Worth keeping green as a pre-flight — it is fast
and it catches the mechanical class, including two that would have failed on
MSVC and nowhere else: `strncasecmp` (POSIX, 12 call sites, absent from MSVC)
and a static-assert fallback that declared the same typedef six times.

`docs/expected-gudprobe-output.txt` is what `gudprobe` should print against
blitsCRT_Mister, generated by the probe tool's own dump code run against the
device in the loopback harness. Diff against it on the night; the difference is
the finding.

---

## Layout

```
include/
  gud.h              the protocol. MIT, from Noralf Tronnes' include/drm/gud.h
  gud_host.h         host side, with no USB in it
  modeline.h         the store: how a modeline reaches the device
  convert.h          BGRA8888 to the wire formats
  lz4enc.h           LZ4 block compression
  gud_dump.h         render a probed device as text
src/common/          everything above the transport, shared by both binaries
src/probe/           gudprobe.exe: WinUSB, no IddCx. Build this first
src/driver/          the IddCx driver, and both INFs
tests/wdkstub/       fake IddCx.h and wdf.h. Build scaffolding, never ship
examples/            a modeline store, every entry checked against the device
docs/
  DESIGN.md          how it fits together and why it is shaped this way
  BRINGUP.md         the order to do it in, and what to prove at each step
  expected-gudprobe-output.txt
```

The transport appears twice on purpose. `gudprobe` opens the device with
`WinUsb_Initialize`; the driver reaches it through a `WDFUSBDEVICE`. Those have
different types and different error reporting and issue byte-identical
transfers, so everything above `struct gud_transport` is shared.

## Build on Windows

```
build.cmd                       gudprobe.exe. Windows SDK, no WDK needed
```

The driver needs the WDK and the two version lines in `GudDisplay.inf` fixed.

---

## Next, in order

`docs/BRINGUP.md` has the detail.

Steps 1 to 3 are **done**. The wire is proved: WinUSB bound from a signed INF,
enumeration byte-identical to `docs/expected-gudprobe-output.txt`, test card,
an unadvertised 384x224p60 modeline accepted and scanned out, and 600 damage
rects with no drift.

Step 4 is **half answered**. IddCx does attach to a USB PDO it did not create:
`IddCxDeviceInitConfig` and `IddCxDeviceInitialize` both return success on one,
and Microsoft's indirect-display overview lists a USB dongle with a monitor
attached as a scenario the model exists for. So the one-package design stands.
What is not settled is whether that same device object can also be a USB target
— see *Blocked*.

1. Settle the USB question, with the two experiments in *Blocked*. Do the
   minimal root-enumerated driver first; it is what step 4 asked for and
   skipping it is why the variables are tangled.
2. First frame. `kFullFrameOnly` in `Driver.h` sends the whole surface every
   frame and skips damage entirely — 1.6 ms of a 16.7 ms field on measured
   hardware, so it is affordable, and it takes every rect-arithmetic fault off
   the table while the IddCx side is still unproven. Turn it off afterwards.
3. Fix the modeline store's dedup: `modeline_store_add()` keys on geometry and
   refresh within 20 mHz, so two per-game modelines at one geometry less than
   about 3 kHz of pixel clock apart silently replace each other.
   `modeline_store_find_exact()` can already tell them apart; only the insert
   cannot. This bites a Switchres backend directly.
4. A Switchres backend writing into the modeline store.

Worth doing on the device side at some point, unchanged: Microsoft OS 2.0
descriptors on the gadget remove the WinUSB binding step entirely.

Worth doing on the device side at some point: Microsoft OS 2.0 descriptors on
the gadget remove step 1 entirely. Windows binds WinUSB from a WCID descriptor
with nothing to install, which is the difference between a board that works when
plugged in and one that needs a driver installed first.

## Licensing

MIT, deliberately.

The GUD host driver in Linux is MIT rather than GPL, in its author's words to
smooth the path for a BSD port. A Windows host is the same kind of thing and
should be usable on the same terms. It also has to be: this links against IddCx
and ships as a signed driver package.

`include/gud.h` carries Noralf Tronnes' copyright and permission notice, which
MIT requires be retained. Nothing here is derived from any GPL source —
blitsCRT_Mister is GPL-2 and is used only as a test fixture, linked by the
loopback harness and never shipped.
