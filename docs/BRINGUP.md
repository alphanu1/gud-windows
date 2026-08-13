# Bring-up

The order matters more than usual here. A display driver that produces no
picture has half a dozen layers that could be at fault and no way to tell them
apart from outside, and IddCx adds three more: the UMDF host process, Session 0,
and the DXGI surface. Debugging a driver in Session 0 means WinDbg and a second
machine.

So each step ends in something observable, and nothing is built on top of an
unproven layer.

---

## 1. Get a handle on the device

The gadget declares a vendor-class interface with no Microsoft OS descriptors,
so Windows has nothing to match and leaves the device with no function driver.
It appears in Device Manager with a warning triangle. That is expected and not
a fault.

**Done.** The INF route works and needs no test signing, which was not obvious
and is worth writing down because it turns this from a manual Zadig step into a
reproducible one:

```
inf2cat /driver:<staging dir> /os:10_X64
signtool sign /fd sha256 /s My /sha1 <thumbprint> GudProbeWinUsb.cat
pnputil /add-driver <staging dir>\GudProbeWinUsb.inf /install
```

with a self-signed code-signing certificate imported into **both**
`LocalMachine\Root` and `LocalMachine\TrustedPublisher`. No `bcdedit /set
testsigning on`, no reboot, no watermark. The package loads no new kernel
binary — winusb.sys is Microsoft's and already signed — so trusting the
publisher is enough for PnP to accept it.

`DriverVer` must be present and must not be postdated **in UTC**: inf2cat
compares against UTC, so a date set after local midnight in a positive offset
fails signability with `22.9.7` while looking perfectly reasonable in the file.

Zadig does the same job interactively if you would rather.

**Proved when:** the device shows under "Universal Serial Bus devices" with no
warning, and `gudprobe` gets past `winusb_open`.

Add Microsoft OS 2.0 descriptors to the gadget at some point and this step
disappears — Windows binds WinUSB on its own from a WCID descriptor, with
nothing to install. That is a device-side change (`sw/gadget.c` and the configfs
setup), it is genuinely small, and it is the difference between a board that
works when plugged in and one that needs a driver installed first. Worth doing
once the rest works.

## 2. Enumerate

```
gudprobe
```

Should print the descriptor, the format list, the connector and every
advertised mode with its full timing.

**What to check, not just that it printed:**

- `magic` is `0x1d50614d`. Anything else and this is not a GUD interface —
  on a MiSTer board the most likely cause is the USB hub add-on still fitted,
  which reaches the same `dwc2` controller.
- `STATUS_ON_SET` is set. If it is not, every failed modeset from here on will
  silently look like a success.
- The mode list matches what the daemon says it advertises. If the count is
  right and the numbers are wrong, `struct gud_display_mode_req` is being
  padded — the static asserts in `gud.h` should have caught that at compile
  time, so check they were not compiled out.

This step proves the control endpoint end to end and costs nothing to repeat.

## 3. Pixels, without IddCx

```
gudprobe --testcard
gudprobe --testcard --raw
gudprobe --bounce
```

`--testcard` does a full modeset and pushes one full surface through exactly
the conversion and compression path the driver's frame loop uses. The card is
drawn in BGRA first and converted, rather than written straight into RGB565, so
that the conversion is under test rather than bypassed.

**Proved when:** the CRT shows colour bars, a half-amplitude row, a 16-step
ramp, a one-pixel border and a centre crosshair.

Read it rather than glancing at it:

- Border missing on one edge → the rect is off by one, or the raster's active
  window does not start where the porches say. Compare against the device's own
  built-in test card, which is generated in the fabric and does not go through
  any of this.
- Colour bars right but the ramp banded oddly → the RGB565 truncation. Six-bit
  ladder, five bits used on red and blue.
- Picture right with `--raw` and wrong without → the compressor. That would be
  surprising: it round-trips against the device's own `lz4dec.c` across four
  thousand random blocks, including the run-length and long-range-duplicate
  cases. Check `grep -c 'LZ4 block bad'` in the daemon's trace first.
- Stride drifting across every line, worse toward the right → the format is
  RGB888 and the device cannot read three bytes per pixel. It should not be
  offering it; check `GET_FORMATS`.

`--bounce` is the damage case, which is what GUD is actually for and where the
numbers diverge most from a full surface. If a full frame works and this does
not, the fault is in the rect arithmetic and nowhere near the transport.

**At the end of step 3 the entire wire is proved.** Transport, protocol,
conversion, compression, modeset. Anything that goes wrong afterwards is IddCx.
That is worth a day and it is why this tool exists.

**Done.** All of it, on a real device: enumeration byte-identical to
`expected-gudprobe-output.txt`, the greyscale and colour test cards, an
unadvertised 384x224p60 modeline accepted and scanned out, and `--bounce`
holding 0.83 ms per rect across 600 with no drift.

It also earned the day it was allocated. `gud_probe` failed on the first real
device with every control request stalled — `ERROR_GEN_FAILURE`, the daemon
running and never seeing a request — because `bmRequestType` was `0x40`,
recipient *device*, where GUD requires `0x41`, recipient *interface*. The Linux
gadget core routes by recipient before the function is reached. The loopback
harness cannot catch that class of fault at all: it replaces the transport, so
no setup packet is ever built. Had this been attempted first through IddCx, the
symptom would have been a driver that loads and shows nothing.

`--grey` is worth using over the colour card. A monotonic staircase makes a
step out of order obviously a geometry fault, and any tint at all means the
BGRA to RGB565 conversion has the channels crossed.

## 4. Settle the structural question

Everything in `src/driver/` rests on one assumption: that IddCx will attach to
a device node it did not create — a real USB PDO rather than the root-enumerated
software device every sample uses.

**Half answered, and the half that is answered is the good half.** Against a
real USB PDO, on hardware:

```
EvtDriverDeviceAdd: IddCxDeviceInitConfig -> 0x00000000
EvtDriverDeviceAdd: WdfDeviceCreate       -> 0x00000000
EvtDriverDeviceAdd: IddCxDeviceInitialize -> 0x00000000
```

IddCx accepts it. Microsoft's indirect display overview also lists "connecting
a dongle to a PC via USB that has a regular monitor connected to it" as a
scenario the model exists for, and says an IDD is responsible for its own
device communications. The one-package design in `DESIGN.md` stands.

What is **not** settled is whether that same device object can also be a USB
target. `WdfUsbTargetDeviceCreate` and `WdfUsbTargetDeviceCreateWithParameters`
both return `STATUS_INVALID_PARAMETER` from `EvtDevicePrepareHardware`, and the
following have each been eliminated by a deploy: the callback it is called from,
both API variants, `UmdfDispatcher` as `WinUsb` and as `NativeUSB`, and
winusb.sys installed as a lower filter (confirmed present in the device's
`LowerFilters`, and it changed nothing).

The live hypothesis is that `IddCxDeviceInitConfig` transforms the
`PWDFDEVICE_INIT` into something whose `WDFDEVICE` cannot host a USB target.

**Do this step as originally written, and do it first.** Build a driver that
does nothing but the plumbing: `EvtDriverDeviceAdd`, `IddCxDeviceInitConfig`,
`IddCxDeviceInitialize`, `IddCxAdapterInitAsync`, one monitor with one hardcoded
mode, and a swapchain processor that acquires frames and throws them away. No
USB in it at all.

**Proved when:** a monitor appears in Display Settings, is selectable, and can
be extended onto — with nothing on the CRT, because nothing is being sent.

Then add USB to that known-good baseline. Going straight at the full driver on
a USB PDO is what tangled the variables above; the minimal driver separates them
in one run.

### Debugging a driver that will not load

Three things cost hours and none of them are discoverable from the failure.

**Turn the UMDF log on before anything else.** It ships disabled and it is the
only place a load failure is described:

```
wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true
```

Even then it says "failed to load the driver at level 0" with an NTSTATUS and
no indication of which call produced it. `0xD000000D` is `STATUS_INVALID_PARAMETER`
with the customer bit set, and "level 0" covers `EvtDriverDeviceAdd` as well as
`DriverEntry` — three cycles went into `DriverEntry`, which was never at fault.

**Make the driver say where it got to.** `GudLog()` in `Driver.cpp` appends a
line to `C:\ProgramData\gud-windows\driver.log`, opening and closing per line so
a killed process still leaves the last one. WUDFHost runs as LocalSystem so the
path is writable. One deploy with it in place produced the answer that three
without it had not.

**A reinstall leaves the device on problem 14 until the stack is rebuilt.**
`pnputil /add-driver /install` reports "System reboot is needed", the device
reads `CM_PROB_NEED_RESTART`, and any driver log from that moment is describing
the *old* binary. For a USB device an unplug/replug rebuilds the stack and is
much cheaper than a reboot. A reading taken while problem 14 is set is not
evidence.

## 5. First frame

Turn on the real frame loop. Test-sign and enable test signing:

```
bcdedit /set testsigning on
pnputil /add-driver GudDisplay.inf /install
```

**Proved when:** the desktop is on the CRT.

Expect the first version to be slow and to have the wrong idea about damage.
Measure before changing anything:

- Time the staging copy and `Map` separately from everything else. That is the
  unknown in the budget — the compression and the wire cost are both already
  measured, from the device side, and neither is the problem.
- Count rects per frame. If a mouse move is producing thirty of them, the
  per-rect control transfer overhead is the cost, not the pixels. `kMaxRects`
  in `SwapChain.cpp` is the lever and 32 is a guess.
- Watch the daemon's own once-a-second line on the serial console. `critical
  path` well under `available` means the device is idle waiting for frames and
  the shortfall is on this end.

## 6. Modelines from outside

Write `C:\ProgramData\gud-windows\modelines.ini`:

```
[modelines]
arcade_224p = ModeLine "384x224@60" 6.700 384 400 432 480 224 227 230 262 -hsync -vsync
```

Restart the device (`pnputil /restart-device`, or unplug it) and the mode should
appear in Display Settings.

**Proved when:** a mode that was never advertised by the device is selectable
and produces the right line rate on the CRT.

That is also the point at which a Switchres backend becomes a small piece of
work rather than a design question: it writes modelines to a known destination
and asks the device to re-enumerate. The existing Windows backends push timing
into a graphics driver through a vendor interface and leave nothing behind to
read, so making this driver the sink rather than a scavenger is the shape that
already fits.

---

## Things that will waste time if you do not know them

**Session 0.** The driver runs in the UMDF host process in Session 0. `printf`
goes nowhere, a message box hangs the process, and a debugger has to be
attached to `WUDFHost.exe`. Use `TraceLoggingWrite` or ETW from the start rather
than adding it after the first thing goes wrong.

**The frame thread must be woken to be stopped.** `~SwapChainProcessor` sets the
terminate flag and signals the frame event. Without the signal the thread stays
blocked and unplugging the cable leaves a wedged thread in Session 0 every time.
The device side hit exactly this shape of fault on host detach — a hang that
only appears when the cable comes out, found by reading the code rather than by
running it.

**A zero-length packet desynchronises the bulk stream.** WinUSB's
`SHORT_PACKET_TERMINATE` must stay off. With it on, a transfer that is an exact
multiple of the endpoint packet size gets a ZLP appended, the device reads it as
a rect of nothing, and every rect after that decodes against the wrong length
with no framing to recover against. A 512-aligned rect is not rare; it is most
of them.

**Do not synthesise a modeline.** When the store has no entry for what Windows
asked for, `EvtIddCxAdapterCommitModes` refuses. The temptation is to generate a
CVT or GTF timing from width, height and refresh. CVT is designed for multisync
panels and produces line rates well outside what a fixed-frequency deflection
circuit will take. The device's own `mode_check` would reject it, which is the
safety net working — but a driver whose normal path depends on the far end
refusing it is waiting for a device with a looser check.
