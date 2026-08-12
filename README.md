# gud-windows — WIP

**Nothing here has run on hardware.** A scaffold pushed as a backup, not a
working driver. Read *Status* before trusting any of it.

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

Four tiers, worst first.

### Broken — known semantic bugs in the driver

No compiler will catch these. WDF handles are all `void*` typedefs, so passing
the wrong handle to the wrong function typechecks under any header, real or
stubbed.

1. **The context lookup is wrong in every callback.** They all do
   `DeviceGetContext(WdfObjectContextGetObject(adapter))`.
   `WdfObjectContextGetObject` takes a *context pointer* and returns the object
   that owns it — this feeds it an `IDDCX_ADAPTER` handle and expects a
   `WDFDEVICE` back. There is no such path. The correct pattern gives the IddCx
   adapter and monitor their own WDF contexts holding a back-pointer, set
   through the `ObjectAttributes` field in `IDARG_IN_ADAPTER_INIT` and
   `IDARG_IN_MONITORCREATE` — declared in the stub, never used. Every callback
   would dereference garbage. Microsoft's `IndirectDisplay` sample shows the
   real pattern; copy it rather than reasoning about it.

2. **Nothing destroys the device context.** `new (DeviceGetContext(device))
   DeviceContext()` is placement-new into WDF memory with no matching
   destructor. On device removal `~SwapChainProcessor` never runs and the frame
   thread is never joined — the exact wedged-thread-in-Session-0 failure
   `docs/BRINGUP.md` warns about. Needs an `EvtCleanupCallback`.

3. **Adapter init is in the wrong callback.** `IddCxAdapterInitAsync` runs from
   `EvtDeviceD0Entry`, which fires on every power transition. Resume from sleep
   would re-probe USB and initialise a second adapter.

Treat `src/driver/Driver.cpp` and `SwapChain.cpp` as a design document with
syntax, not as code.

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

The stubs are guesses. The three structures most likely to differ are named at
the top of `tests/wdkstub/IddCx.h`, and the one field access that depends on
them is isolated in `SignalInfo()` so there is a single line to correct. Same
for the two version-specific lines in `GudDisplay.inf` — `UmdfExtensions` and
`UmdfLibraryVersion` — which must be copied from the `IndirectDisplay` sample
that ships with the WDK.

### Compiled and linked — `gudprobe.exe`

Cross-compiles and links for x86-64 Windows against Microsoft's real
`winusb.lib` and `setupapi.lib`. Never run with a device attached.

**`gudprobe` is not a driver.** It talks to the device directly over WinUSB.
Nothing appears in Display Settings; Windows does not know a display exists. It
prints the mode list, sets a mode — including an arbitrary modeline off the
command line — and puts a picture on the CRT. That is the point: it proves the
wire, and the modeline path, without IddCx in the way.

```
gudprobe --modeline "7.560 384 400 432 480 224 227 230 262 -hsync -vsync"
```

That timing is in no advertised list. The device solves the PLL for 7.560 MHz
and scans it out. Proving that from the command line is worth doing before any
of the driver exists.

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

1. Bind WinUSB — `GudProbeWinUsb.inf` or Zadig. The gadget declares a
   vendor-class interface with no Microsoft OS descriptors, so Windows leaves it
   with no function driver.
2. `gudprobe` — enumerate, diff against the expected output.
3. `gudprobe --testcard`, then `--modeline`, then `--bounce`. **At the end of
   this the whole wire is proved**, including an arbitrary modeline reaching the
   device, and anything afterwards is IddCx. Worth a day.
4. Settle the structural question: will IddCx attach to a PDO it did not create,
   rather than the root-enumerated software device every sample uses? Microsoft
   names this exact case — a USB dongle with a monitor attached — but if it does
   not hold, the driver splits in two with every frame crossing a process
   boundary in Session 0, and that is a different project. Prove it with a
   driver that enumerates a monitor and throws frames away. No USB in it.
5. Fix the three bugs above against the real headers and sample.
6. First frame.
7. A Switchres backend writing into the modeline store.

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
