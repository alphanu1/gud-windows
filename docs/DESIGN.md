# Design

## One driver, any GUD device

GUD is a wire protocol, not a Linux one. A device implementing it needs a host
that speaks request codes, a mode structure and a buffer format, and nothing
about the device depends on what is at the other end. This is a peer of the
in-tree Linux driver, not a port of it, and it drives anything that answers the
protocol — a Pi Zero adapter, the STM32 reference device, an e-paper panel.

Switchres, 15 kHz CRTs and per-game modelines are why it got written. They are
not what it is limited to, and that is structural: everything above
`struct gud_transport` asks the device instead of assuming. `GET_FORMATS`
decides the pixel format. `GET_CONNECTOR_MODES` fills the mode list. The
descriptor's width and height bounds are checked before anything uses them, a
`FULL_UPDATE` device gets whole frames and no per-rect `SET_BUFFER`, and a
`STATUS_ON_SET` device has its errors read.

**The modeline store is additive.** For a device whose advertised list is the
whole truth it is a cache of `GET_CONNECTOR_MODES` and nothing else — INI absent,
no override ever taken. It exists because some devices synthesise their own pixel
clock and can be handed timing that was never advertised, and it cannot invent a
mode the device did not agree to: `SET_STATE_CHECK` still arbitrates.

### What is device-specific

**`VID_1D50&PID_614D`**, in both INFs and as `VID`/`PID` in `gudprobe.c`. The one
thing another device must change, and the probe tool wants a `--device vid:pid`
argument so it does not need a rebuild for the next one.

**RGB565 ahead of anything deeper**, in `Driver.h`. Right for a six-bit resistor
ladder, a pointless loss on a device offering `XRGB8888`.

**Negative sync polarity** when a hand-written modeline omits it, in
`modeline_parse()`. The 15 kHz RGB and SCART convention. Advertised modes carry
their own flags and never reach it.

**No EDID**, for every device and not only this one. `GUD_REQ_GET_CONNECTOR_EDID`
is never issued. A decision and not a gap: an EDID carries a mode list the store
knows nothing about and the commit path refuses those modes. See below.

**Connector type reported as VGA.** `MonitorType` in `IDDCX_MONITOR_INFO`
describes the link between adapter and monitor, and over USB there is no such
connector. `DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15` describes the only hop that has
one — the analog output into the CRT. Mapping GUD's `connector_type` across buys
nothing: the value is cosmetic and a device with an HDMI socket is still reached
over USB. `gud_probe()` reads the field anyway, because `gudprobe` prints it.

`R1` and `XRGB1111` are refused outright, which is a real limit and not a tuned
default. See *What is not here*.

## One package, not two

The driver binds directly to the USB device: a UMDF2 driver on
`USB\VID_1D50&PID_614D` with `UmdfDispatcher = WinUsb` in the INF, calling
`IddCxDeviceInitConfig` on top of that. WinUSB sits underneath and the driver
gets a `WDFUSBDEVICE`.

Every IddCx sample is shaped the other way — a root-enumerated software device,
because a virtual display has no hardware to attach to. Copying that shape here
would mean a second driver or a service holding the USB handle, and an IPC
channel between them carrying every frame. In Session 0. A 640x480 surface is
600 KB and there are sixty of them a second; that copy is not affordable and
would exist for no reason other than following the sample.

The cost of the chosen shape is one assumption, stated plainly: IddCx has to be
willing to attach to a PDO it did not create. Microsoft names this case
directly — a USB dongle with a monitor attached — so it should work.

**It does.** On hardware, against a real USB PDO, `IddCxDeviceInitConfig` and
`IddCxDeviceInitialize` both return `STATUS_SUCCESS`. Microsoft's indirect
display overview also lists the USB dongle case as a scenario the model exists
for, and states that an IDD is responsible for its own device communications.
The assumption this whole file rests on is confirmed on both documentation and
measurement, and the driver does not have to split in two.

**And the same device object hosts a USB target.** `WdfUsbTargetDeviceCreate`,
`WdfUsbTargetDeviceSelectConfig` and then `gud_probe()` over WDF-USB all
succeed, and the driver registers an IddCx adapter afterwards. One UMDF2 driver
is both an IddCx display driver and a USB client driver on one PDO, which is
the whole of what this section claimed and the whole of what it needed.

The one cost worth recording is that the INF has to be exactly right, and gets
no help when it is not: `UmdfDispatcher` lives in `[..._Install.NT.Wdf]`, and in
the service install section it is silently inert. `BRINGUP.md` step 4 has the
detail.

## Where the porches come from

This is the whole difficulty, and it is the reason a Windows GUD driver is worth
building rather than being an obvious port.

`SET_STATE_CHECK` carries a complete DRM modeline. A device that synthesises its
own pixel clock — reconfiguring a PLL per mode, which is what the reference
device does — needs all of it: clock, both sync starts and ends, both totals,
interlace.

IddCx has nowhere to put any of it. Its vocabulary is a monitor mode list and
EDID. Width, height, and refresh as a rational. A normal indirect display does
not generate timing and has no use for porches, so the API does not carry them,
and no amount of squinting at `IDDCX_TARGET_MODE` produces them.

On Linux the path is direct: an application adds a modeline to a DRM connector,
the kernel hands the mode to `gud`, and `gud` puts it on the wire unchanged. The
existing Windows arrangement for this problem is two-part — a tool installs a
resolution list into the driver ahead of time, and the emulator adjusts timings
at runtime through a driver-specific interface, ADL on AMD. The first half maps
onto IddCx directly. The second half has no IddCx equivalent.

**So the driver keeps the table.** Every mode reported to Windows is backed by a
full modeline held in `modeline_store`. At commit time the lookup runs the other
way: Windows hands back active size and a vSync rational,
`modeline_store_find()` turns that into the modeline it came from, and that
modeline goes on the wire intact.

Two sources fill the table:

1. **The device's own list**, from `GET_CONNECTOR_MODES`. Every GUD device
   advertises one and every entry carries full timing. This is why the driver
   works with no configuration at all — plug the board in and the modes it
   advertises are the modes Windows offers.

2. **An INI**, at `C:\ProgramData\gud-windows\modelines.ini`, in the X11 and
   Switchres spelling. Loaded second, so a hand-written entry overrides an
   advertised one with the same geometry and rate. Someone who wrote out a
   modeline meant it.

That is the *fallback* key: `(hdisplay, vdisplay, refresh within
MODELINE_REFRESH_TOL_MHZ, interlace)`, 20 mHz as it stands. The tolerance is not
slop — Windows carries refresh as a rational and rounds it in several places, so
a mode reported at 60000 mHz can come back as 60001 — and it has to stay narrow
enough that 59.94 and 60.00 remain distinct, since they are different modelines
on a CRT and conflating them is a visible fault.

The primary lookup does not use it. `pixelRate` and `totalSize` come back
through `EvtIddCxAdapterCommitModes` exactly as the driver supplied them, so
four of the five identifying numbers are available and the match is exact rather
than approximate. Only the sync starts and ends are missing, and recovering
those is the whole reason the store exists.

`interlace` is in the key because it cannot be derived from geometry: DRM
reports a 480i mode's `vdisplay` as 480 and so does Windows, so nothing
distinguishes 640x480p60 from 640x480i60 by size alone. It is nonetheless
passed as don't-care at commit time, because the OS does not give it back —
see *What Windows cannot express* below.

The same 20 mHz is `modeline_store_add()`'s idea of a duplicate, and there it is
too loose: two per-game modelines at one geometry less than about 3 kHz of pixel
clock apart replace each other silently. `modeline_store_find_exact()` can tell
them apart; only the insert cannot. It is on the list in the README.

### Why the driver is the sink and not a scavenger

The obvious alternative is to find where a Windows Switchres deposits a
generated modeline and read it. There is no such place. The existing Windows
backends (`adl`, `powerstrip`) push timing into a graphics driver through a
vendor interface and leave nothing behind.

Making the driver the destination turns the Switchres side into a small backend
that writes modelines somewhere known and asks the device to re-enumerate —
which is the shape every Switchres backend already has, `drmkms` included. It
also means the mechanism exists and is testable before any Switchres work
happens: today it is an INI a person edits.

### The re-enumeration constraint

IddCx reads a monitor's mode list once, at arrival. Growing it later means
monitor departure and arrival.

GUD has the identical constraint on the device side — `GET_CONNECTOR_MODES` is
asked once during USB enumeration and the device raises
`GUD_CONNECTOR_STATUS_CHANGED` to make a host re-probe rather than growing the
list in place. Both ends re-probe.

The driver watches `modelines.ini` on a WDF timer and rebuilds the store when it
changes, so a modeline written while the display is running reaches the CRT
without a replug. That takes two mechanisms, and the first of them looks
sufficient until it is tried:

- `IddCxMonitorUpdateModes()` replaces the monitor's **target** modes, the
  timings the driver can drive. It takes effect at once with nothing disturbed
  on screen, and it is enough on its own to correct the porches of a mode that
  already exists.
- It does not touch the monitor's **own** mode list, which is what Windows
  builds the resolution list from, and that list is read once, at arrival. A
  genuinely new geometry published this way still cannot be selected — verified,
  with `UpdateModes` returning `STATUS_SUCCESS` and `EnumDisplaySettings` going
  on offering only the modes present at arrival.

So a new geometry costs a departure and an arrival. That is disruptive — the
swap chain goes, the CRT blanks, windows on the display move — so it happens
only when the mode set actually changed.

**"Actually changed" is a signature over every timing field, not a mode count.**
Counting them is wrong in the case that matters most: a Switchres backend
replacing one generated modeline with another between two games leaves the count
alone while changing the geometry. The monitor is then never re-enumerated,
Windows keeps offering a mode that no longer exists, and the new one cannot be
selected. It fails silently and it fails in the normal case.

## What Windows cannot express, and what to do about it

Interlace does not survive IddCx. A target mode set as
`DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED` comes back through
`EvtIddCxAdapterCommitModes()` as `PROGRESSIVE`, and reaches applications the
same way — `EnumDisplaySettings` reports `dmDisplayFlags` of 0 for a 480i mode.
The desktop IddCx composites is progressive and the field order is normalised
away above this driver.

Nothing here can change that, so two things follow.

**Nothing in the driver may key on getting it back.** The commit-time lookup
passes interlace as don't-care, which costs nothing because pixel clock, both
totals and both actives already identify a modeline. Keying on it means an
interlaced modeline never matches, the commit is refused, and IddCx restarts the
driver in a loop.

**Anything outside the driver would draw the wrong conclusion.** 648x480i60
presents as 480p at 60 Hz, whose line rate computes as 31.5 kHz rather than the
15.75 it runs. A tool deciding whether a mode suits a 15 kHz CRT will rule out
the modes it most wants — Switchres answered a 640x480 request with 240 lines,
half of them discarded, before this was addressed.

So the driver writes down what it is really running:

```
C:\ProgramData\gud-windows\modes.active
```

Same spelling as the INI it reads, so one parser covers both. It is a generated
file, rewritten on every store rebuild, and it is a **published interface** —
`switchres`' GUD backend reads it — so the format is not free to churn:

```
; gud-windows active mode list -- generated, do not edit.
; The interlace flag here is authoritative. Windows reports
; every one of these modes as progressive.
; device = USB\VID_1D50&PID_614D&REV_0100
[modelines]
ModeLine "648x480i60" 12.600 648 670 730 800 480 486 492 525 interlace -hsync -vsync
```

The `device` line carries the hardware id, from
`WdfDeviceQueryProperty(DevicePropertyHardwareID)`. `EnumDisplayDevices` reports
the same string as the display adapter's `DeviceID`, so a reader can confirm the
list describes the display it is looking at rather than some other USB-attached
one on the same machine. A reader that skips that check can end up claiming a
DisplayLink adapter.

## The frame loop

One thread per assigned swapchain. Per frame:

```
acquire  ->  dirty rects  ->  staging copy  ->  convert  ->  LZ4  ->  bulk
```

**The staging copy has no way around it.** IddCx hands over a DXGI surface in
GPU memory on the rendering adapter and there is no CPU pointer to it. Reading
it means `CopySubresourceRegion` into a `D3D11_USAGE_STAGING` texture and then
`Map`, which is a GPU round trip per frame. Copying only the damaged region
rather than the whole surface is what keeps that affordable — and it is also
what keeps the wire cheap, so both motivations point the same way.

The staging texture is allocated once at swapchain assignment, not per frame.
Allocating per frame is a driver-level allocation and shows up as jitter, and
jitter costs a whole frame: the device side measured exactly that in the other
direction, 58.5 fps instead of 60 from about two milliseconds of variance.

**Move regions are damage at the destination only.** IddCx supplies dirty rects
and move regions separately; a move region is the compositor saying a block of
pixels slid. The source is already correct on the device and only the
destination needs sending. Treating both as damage doubles the cost of every
window drag.

**Past a threshold, one full surface is cheaper than the rects.** Each rect
costs a `SET_BUFFER` control transfer plus a status read behind it, and a
control transfer occupies a whole microframe whatever it carries. A full surface
of a mostly-unchanged screen compresses extremely well — measured at 253x on a
static screen — so the union is often cheaper than thirty small headers.
`kMaxRects = 32` is a starting number and not a measured one.

## Compression

LZ4 block format, carried here rather than linked. The same reasoning as the
device carrying its own decoder: it is the only compression either end will ever
need, a driver package has to be self-contained, and a driver that pulls in a
third-party library acquires that library's servicing problem.

Block format only. No frame header, no magic, no checksum — GUD says how many
bytes follow in `gud_set_buffer_req` and the device expands exactly that.
Emitting a frame-format stream would decode as garbage, because the decoder
reads the frame magic as a token.

Compression is declared per rect, so a rect that did not compress is sent raw
and costs only the attempt. `gud_flush_rect()` refuses to mark a rect compressed
unless the compressor actually won: the device has to expand anything marked
compressed, and paying it a couple of milliseconds of ARM time to expand data
that got *bigger* is the worst of both.

The greedy single-table algorithm rather than the high-compression variant. This
runs once per damage rect at 60 Hz, and the measured ratio on this content is
already well past what the link needs — 2.58x on real traffic against the 1.2x
the bandwidth arithmetic required. Spending host CPU to push that higher buys
nothing; the frame is already inside budget and the CRT sets the pace.

## Colour

RGB565 by default, RGB332 as a fallback, and both are chosen by reading
`GET_FORMATS` rather than assumed.

The reference device's DAC is a six-bit resistor ladder per channel, so RGB565
gives away one bit on red and blue. RGB888 would waste nothing, and is not
offered by that device yet because its fetch path cannot read three bytes per
pixel across a 64-bit beat boundary. `convert_bgra_to_rgb888()` is written and
sits unused; it costs nothing to have ready and other GUD devices do offer the
format.

Truncation rather than rounding on the way down. Rounding shifts the whole ramp
by half a step and shows up as a black level that is not quite black — check it
against the 16-step greyscale ramp on the test card.

## EDID

A minimal one, carrying no timing at all.

The device's own EDID is not passed on. The reference device implements one and
deliberately does not send it: tested on hardware, a host given that EDID picks a
mode wider than the raster and drops frames — with a bare name descriptor as much
as with sync range limits.

Sending none at all is not available on Windows. `IddCxMonitorCreate` refuses a
description of type `UNINITIALIZED`, and the header comment saying to pass NULL
is stale — the field is a struct by value, so there is no NULL to pass. A monitor
has to have a description, and a monitor with no modes cannot arrive.

So the driver builds a 128-byte block with a name descriptor, three unused
descriptors and a computed checksum: no detailed timing descriptors, no
established timings, no standard timings. Windows can derive no modes from it,
so it asks `EvtIddCxParseMonitorDescription` instead, and the answer is the
modeline store — the only thing that knows the porches.

That is the property worth protecting. An EDID with timings in it would put modes
in front of Windows that the store has never heard of, and
`EvtIddCxAdapterCommitModes` would rightly refuse them, which is a display
listing modes it cannot set.

The display shows as a generic PnP monitor as a result. A wrong mode on a
fixed-frequency deflection circuit is worse than an unhelpful name.

### Not a pass-through either

The obvious generalisation is to issue `GUD_REQ_GET_CONNECTOR_EDID`, pass on
whatever comes back and send none when there is none, which is what the in-tree
Linux driver does. Wrong default here, and not only because of the 15 kHz hazard.

An EDID is not a name, it is a mode list. Windows reads modes out of its timing
descriptors and those modes have no entry in the store, because nothing put them
there — so `EvtIddCxAdapterCommitModes()` refuses them, which is the safety net
working. The result is a display whose modes are listed and cannot be selected,
and that happens on any device whose EDID describes timing its own mode list does
not.

So the device's EDID is not passed on, for any device. If one turns out to need
it, that is a runtime option and not a new default, and it can go in the
`modelines.ini` the store already reads:

```
[options]
edid = passthrough        ; default: the driver's own, no timing
```

Reconciling the EDID's modes against the store is the work. The switch is not.

## What is not here

**Properties.** GUD carries a property mechanism — backlight, rotation, TV mode.
The reference device advertises none and this driver reads none. IddCx has no
natural place to surface them anyway.

**More than one connector.** `GET_CONNECTORS` returns an array and this takes
the first. A second would need a second IddCx monitor with its own swapchain.
Not difficult; no GUD device in existence reports more than one.

**More than one GUD device on a machine.** Assumed against throughout, and the
assumption has spread rather than stayed put. `EvtIddCxParseMonitorDescription`
is handed a description and a mode buffer and nothing else — no monitor, no
adapter, no device — so the store is reached through a file-scope pointer, and
the lock guarding it is file-scope for the same reason. `MonitorContainerId` is
a fixed constant because it has to be stable across reboots, so two devices
would report the same one. `modes.active` and `modelines.ini` are single files
at fixed paths. Undoing this means a device-keyed lookup for that callback, and
a container ID derived from the device's serial.

**Sub-byte formats.** `R1` and `XRGB1111` have no byte-per-pixel and every
buffer-size calculation here assumes one. They exist for e-paper and tiny
displays. `gud_format_bpp()` returns 0 for them and `gud_set_state()` refuses,
rather than computing a wrong length quietly. Refusing is the right failure and
the wrong end state — these are what e-paper and the tiny displays actually use,
and reaching them means bits per pixel rather than bytes per pixel through
`convert.c` and `gud_flush_rect()`.

**EDID pass-through.** Covered above, and absent on purpose, not pending. The
mode list an EDID brings with it is the problem, not the EDID.
