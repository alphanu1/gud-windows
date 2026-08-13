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

One thing is still open and it is narrower: whether that same device object can
also host a USB target. `WdfUsbTargetDeviceCreate` fails on it with
`STATUS_INVALID_PARAMETER` and the cause is not yet known — see `BRINGUP.md`
step 4, which now carries the eliminations and the remaining hypothesis. If it
turns out IddCx and USB cannot share one WDFDEVICE then this section is wrong
after all, and the answer is a root-enumerated IddCx driver plus a separate
transport. Nothing found so far says that, and the documentation says the
opposite.

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

The key is `(hdisplay, vdisplay, refresh within 100 mHz, interlace)`. The
tolerance is not slop: Windows carries refresh as a rational and rounds it in
several places, and a mode reported at 60000 mHz can come back as 60001. 100 mHz
is wide enough to survive that and narrow enough that 59.94 and 60.00 stay
distinct — they are different modelines on a CRT and conflating them is a
visible fault.

`interlace` is part of the key because it cannot be derived. DRM reports a 480i
mode's `vdisplay` as 480 and so does Windows, so geometry alone cannot
distinguish 640x480p60 from 640x480i60.

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
list in place. Both ends re-probe. `ReloadModes()` does the IddCx half.

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

None sent.

The reference device implements one and deliberately does not send it: tested on
hardware, a host given that EDID picks a mode wider than the raster and drops
frames — with a bare name descriptor as much as with sync range limits. Both
variants lack any timing descriptor, which is the likely cause and is not yet
settled.

Windows is less tolerant of a monitor with no EDID than Linux is; the display
shows as a generic PnP monitor. A wrong mode on a fixed-frequency deflection
circuit is worse than an unhelpful name.

If a name turns out to be needed, build a block carrying a *timing* descriptor
for the preferred mode rather than range limits. That is the untried variant and
the one the evidence points at.

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

So no EDID, for every device. If one turns out not to work without an EDID that
is a runtime option and not a new default, and it can go in the `modelines.ini`
the store already reads:

```
[options]
edid = passthrough        ; default: none
```

Reconciling the EDID's modes against the store is the work. The switch is not.

## What is not here

**Properties.** GUD carries a property mechanism — backlight, rotation, TV mode.
The reference device advertises none and this driver reads none. IddCx has no
natural place to surface them anyway.

**More than one connector.** `GET_CONNECTORS` returns an array and this takes
the first. A second would need a second IddCx monitor with its own swapchain.
Not difficult; no GUD device in existence reports more than one.

**Sub-byte formats.** `R1` and `XRGB1111` have no byte-per-pixel and every
buffer-size calculation here assumes one. They exist for e-paper and tiny
displays. `gud_format_bpp()` returns 0 for them and `gud_set_state()` refuses,
rather than computing a wrong length quietly. Refusing is the right failure and
the wrong end state — these are what e-paper and the tiny displays actually use,
and reaching them means bits per pixel rather than bytes per pixel through
`convert.c` and `gud_flush_rect()`.

**EDID pass-through.** Covered above, and absent on purpose, not pending. The
mode list an EDID brings with it is the problem, not the EDID.
