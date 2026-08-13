# gud-windows — WIP

**It works, and it is early.** A Generic USB Display appears in Display
Settings, the desktop extends onto a 15 kHz CRT, and Switchres generates a
per-game modeline that reaches the board with its own porches — the thing the
project exists to do. What it has not had is mileage: one device, one machine,
one pair of eyes. Read *Status* before trusting any of it.

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

The file is watched, so a modeline added while the display is running takes
effect without a replug — the store is rebuilt and the monitor re-enumerated
when the set of modes changes. IddCx reads a monitor's mode list once, at
arrival, so a new geometry costs a departure and an arrival; there is no way
around that and `docs/DESIGN.md` explains what was tried.

The driver also writes back. `C:\ProgramData\gud-windows\modes.active` holds
what it is currently running, in the same spelling, because nothing outside the
driver can otherwise learn what these modes really are — Windows reports every
one of them as progressive, so a 480i timing reads as 31.5 kHz rather than
15.75 and a 15 kHz tool discards exactly the modes it wants. It is a generated
file and a published interface; the Switchres backend reads it.

The lookup key is exact — pixel clock, both totals, active size, interlace — not
a refresh-rate match. `DISPLAYCONFIG_VIDEO_SIGNAL_INFO` hands those numbers
straight back because the driver put them there. Matching on refresh alone
cannot separate 59.94 from 60.00 with any tolerance wide enough to survive
Windows' rounding, and returning the wrong modeline puts wrong timing on a
fixed-frequency deflection circuit.

---

## Status

Most proved first, and the caveats are in each section rather than collected
here.

### Works — the desktop is on the CRT, at per-game modelines

The whole path runs end to end on hardware. Windows names a mode, the driver
finds the modeline behind it, and the real porches go on the wire:

```
CommitModes: entered, 1 paths
  path active 648x480 pixelRate=12600000 total 800x525
  modeline 648x480i60: 648x480i clk=12600 h 648/670/730/800 v 480/486/492/525
  gud_set_state -> 0
AssignSwapChain: streaming 648x480
```

A monitor appears in Display Settings, is selectable, and the desktop extends
onto it. Both of the reference device's modes work — 648x480i60 and 632x240p60
— each committed from the store with its own timing.

**Modelines can be added while the display is running.** The driver watches
`modelines.ini` and picks up a new one within about two seconds, then
re-enumerates its monitor so Windows offers the new geometry. No replug, no
device restart. That is what makes a per-game modeline workable rather than a
configuration step.

**A Switchres backend exists**, on the `gud-backend` branch of
[alphanu1/switchres](https://github.com/alphanu1/switchres). It generates a
timing for the game, writes it here, waits for the re-enumeration and selects
it. Verified against a 15 kHz CRT with `384x224@59.185` and an interlaced
`640x480@60`, neither of which the device advertises:

```
Switchres: normal (384x224@59.185001)->(384x224@59.185001)
GUD: mode 384x224 appeared after 3750 ms
set_desktop_mode: \\.\DISPLAY68 (384x224@59)
    -> modeline sr_0: 384x224p clk=7842 h 384/400/437/500 v 224/236/239/265
```

That is the thing this project was written to do.

**One surprise worth reading before touching any of it:** interlace does not
survive IddCx. A mode set as interlaced comes back progressive, both at commit
time and to every application — `EnumDisplaySettings` reports no `DM_INTERLACED`
for a 480i mode. It has caught two different pieces of code already. The driver
publishes `C:\ProgramData\gud-windows\modes.active` with the real timings for
anything that needs to reason about line rate; `docs/DESIGN.md` has the detail.

### Fixed, and worth not repeating

Eight blockers, none of which a compiler can see and none of which the failure
names. The last two were the expensive ones. In the order they were hit:

1. **`EvtIddCxParseMonitorDescription` is mandatory.** Without it
   `IddCxDeviceInitConfig` fails `STATUS_INVALID_PARAMETER`. A driver with no
   EDID still has to supply it — return zero modes and succeed.
2. **UMDF 2.33 is a Windows 11 framework.** On Windows 10 19045 it compiles,
   signs and installs and then cannot load. 2.31 is the ceiling there, and the
   build has to move with the INF: headers from `Include\wdf\umdf\2.31`, stub
   from `Lib\wdf\umdf\x64\2.31`.
3. **`Include = WUDFRD.inf` / `Needs = WUDFRD.NT` is what the documentation
   shows and WUDFRD.inf does not exist on Windows 10 19045.** Register the
   reflector explicitly: `AddService = WUDFRd,0x000001fa,...`.
4. **`UmdfDispatcher` belongs in the `.NT.Wdf` section**, not the service
   install section. In the wrong section nothing reads it, and
   `WdfUsbTargetDeviceCreate` then fails `STATUS_INVALID_PARAMETER` — which is
   exactly what its documentation warns about. This is what cost the most: with
   the directive inert, changing its *value* between `WinUsb` and `NativeUSB`
   naturally changed nothing, which made the dispatcher look innocent.
5. **USB targets are created in `EvtDevicePrepareHardware`**, not
   `EvtDriverDeviceAdd`. Correct per WDF regardless of the above.
6. **`pFirmwareVersion` and `pHardwareVersion` in `IDDCX_ENDPOINT_DIAGNOSTIC_INFO`
   are not optional.** Null, and `IddCxAdapterInitAsync` refuses the caps
   without saying which field. Point both at one `IDDCX_ENDPOINT_VERSION` with
   `Size` and `MajorVer` set, as the `IndirectDisplay` sample does.
7. **An IddCx driver needs the `IndirectKmd` upper filter in its INF.**
   `HKR,,"UpperFilters",0x00010000,"IndirectKmd"` in an `.NT.HW` section.
   `IndirectKmd.sys` is the kernel half of the indirect display model, the piece
   that joins the device to `dxgkrnl`. Without it `IddCxAdapterInitAsync`
   returns success and the OS has no path to call back on:
   `EvtIddCxAdapterInitFinished` is never invoked, the device sits at
   `status=OK` `problem=0`, and no monitor appears. It is declared in
   `rdpidd.inf` and **absent from the `IndirectDisplay` sample's INF**, which is
   why building from the sample cannot get you there. Seven hypotheses were
   eliminated ahead of it — dispatcher, caps, callback ordering, `ClassVer`,
   host process sharing, test signing, the callback it is called from — and none
   of them were wrong; the plumbing they depended on was absent.
8. **`AdditionalSignalInfo.videoStandard` must not be zero.** Zero is what
   zero-initialising the signal info leaves, and it is enough for
   `IddCxMonitorArrival` to refuse the mode list with `STATUS_INVALID_PARAMETER`
   and name no field — while `IddCxMonitorCreate` accepts the same monitor
   happily. 255, "other", is the honest value: there is no D3DKMDT video
   standard for a 15 kHz arcade raster. `vSyncFreqDivider` goes with it, 1 on
   target modes and 0 on monitor modes; the header describes both structures in
   one comment and so appears to contradict itself.

**And one that is not a driver bug but cost more than several of them.**
`DriverVer` has to increase or Windows keeps the installed binary and does not
say it has declined. Several rounds of testing here measured an older build,
including the first run of a fix that worked. Any driver iteration loop needs a
check that the installed binary is the one just built — without it a negative
result means nothing.

Also fixed: all four originally-listed semantic bugs. The context lookup in
every callback goes through `AdapterContext`/`MonitorContext` back-pointers
attached via `ObjectAttributes`, the device context has an `EvtCleanupCallback`
so the frame thread is joined, and adapter re-initialisation on resume is
refused.

**Two things that made all six findable**, and both are worth doing before
touching a UMDF driver again:

- The UMDF log channel that describes a failed load,
  `Microsoft-Windows-DriverFrameworks-UserMode/Operational`, ships **disabled**.
  `wevtutil sl <channel> /e:true`. Even then it says only "failed to load the
  driver at level 0" and an NTSTATUS — "level 0" covers `EvtDriverDeviceAdd`,
  not just `DriverEntry`, and three cycles went into the wrong function on the
  strength of that phrase.
- **The driver logs to a file.** `GudLog()` appends to
  `C:\ProgramData\gud-windows\driver.log`, opening and closing per line so a
  killed process still leaves the last one. One deploy with it in place found
  what three without it had not, and every blocker after that came out in one
  or two cycles rather than six.

### The stub build, which is now a lint rather than the only check

The driver builds against the real WDK and runs, so this is no longer what
stands between the code and reality. It is kept because it is fast, needs no
WDK, and catches a class of fault the WDK build does not.

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
blocker 2 above.

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

Steps 1 to 6 are **done**. The wire is proved: WinUSB bound from a signed INF,
enumeration byte-identical to `docs/expected-gudprobe-output.txt`, test card,
an unadvertised 384x224p60 modeline accepted and scanned out, and 600 damage
rects with no drift. The driver loads on a USB PDO, brings up a monitor, puts
the desktop on the CRT, takes modelines from outside while running, and a
Switchres backend drives it.

What is left, in order:

1. **Turn `kFullFrameOnly` off.** It is in `Driver.h` and sends the whole
   surface every frame, skipping damage entirely — 1.6 ms of a 16.7 ms field on
   measured hardware, so it is affordable, and it took every rect-arithmetic
   fault off the table while the IddCx side was unproven. That justification has
   expired: there is a stable picture to compare against now, and the damage
   path measured 0.83 ms for 600 rects on this hardware and is not exercised.
2. **Fix the modeline store's dedup.** `modeline_store_add()` keys on geometry
   and refresh within 20 mHz, so two per-game modelines at one geometry less
   than about 3 kHz of pixel clock apart silently replace each other.
   `modeline_store_find_exact()` can already tell them apart; only the insert
   cannot. This bites the Switchres backend directly, and now that the backend
   exists it is reachable rather than theoretical.
3. **Replace the deferred monitor arrival.** It is a `std::thread` with a
   two-second sleep, left over from a hypothesis that proved wrong. `StartModeWatch`
   demonstrates a working WDF timer in the same driver, so there is no longer an
   excuse for it.
4. **Revisit `MonitorType` and the security descriptor.** `MonitorType` is
   `HDMI`, which was a diagnostic value; `INDIRECT_WIRED` is probably the honest
   one. `D:P(A;;GA;;;WD)` is `rdpidd.inf`'s verbatim and grants generic-all to
   Everyone — it is there because it is the known-working configuration, not
   because it has been reasoned about.

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
