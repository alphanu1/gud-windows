# Vertical blanking

Design notes for synchronising the host to the device's raster. **Nothing here
is implemented.** It is written before the work rather than after it, so the
shape of the problem is on paper while the device side is still being designed
and can still change.

Named `VBLANK.md` to match `DESIGN.md` and `BRINGUP.md`.

## Why this matters here and not on an ordinary display

The device is not double buffered. From `blitscrt_regs.h`:

> Not a framebuffer. Damage rectangles are written straight into the region the
> raster is already reading; nothing is presented and nothing is flipped.

That is a deliberate design and it is what makes damage rectangles cheap — there
is no copy, no flip, and no second buffer's worth of DDR3 bandwidth. It also
means **tearing is structural rather than incidental**. A write that lands while
the beam is over the region it touches is displayed half-old and half-new, and
on a 15 kHz CRT running 60 fields a second that is visible.

Today the host writes whenever DWM hands it a frame. Nothing prevents a write
landing mid-scanout, and that it usually looks acceptable is luck plus the fact
that most desktop damage is small and static. It will not survive a game
redrawing the whole screen every frame, which is the case this project exists
for.

## The budget, which is the constraint everything else works around

Vertical blanking is `(vtotal - vdisplay)` lines, and a line is
`htotal / clock`:

| mode | line | blanking | window | full surface costs |
| --- | --- | --- | --- | --- |
| 632x240p60 | 63.49 us | 22 lines | **1.40 ms** | 0.85 ms at 384x224 |
| 648x480i60 | 63.49 us | 22.5 lines/field | **1.43 ms** | 1.59 ms |
| 384x224p60 | 63.76 us | 41 lines | **2.61 ms** | 0.85 ms |

Measured on this hardware, one 64x48 damage rectangle costs **0.83 ms**, stable
over 600 of them. That cost is almost entirely the `SET_BUFFER` control transfer
and the status read behind it rather than the pixels: a control transfer
occupies a whole microframe whatever it carries.

Three things follow, and they are the whole design:

1. **One rect per vblank, near enough.** 0.83 ms into a 1.40 ms window leaves no
   room for a second. This happens to suit Windows, which coalesces damage to
   exactly one rectangle per frame before an indirect display driver sees it
   (see `DESIGN.md`), but it is a hard ceiling rather than a comfortable fit.
2. **A full 480i surface does not fit in one field's blanking.** 1.59 ms against
   1.43 ms. It has to be split, or written while the beam is somewhere harmless,
   or not written every field.
3. **The status read is now load-bearing twice over.** It is the thing that makes
   a rect expensive, and — see below — it is also the cheapest place to carry the
   vblank information back.

## What Windows gives us, and what it does not

Checked against `IddCx.h` 1.2 rather than assumed, and against 1.10 to see
whether later versions help. They do not.

**There is no vblank API in IddCx.** No wait-for-vblank, no way to tell the OS
when the device's blanking occurs, nothing. The complete function list is
twenty entries and none of them concern raster timing.

What does exist is one field and one call:

- **`IDARG_OUT_RELEASEANDACQUIREBUFFER.PresentDisplayQPCTime`** — "System QPC
  time of when this surface should be displayed on the indirect display
  monitor". The OS decides this, from the `vSyncFreq` the driver declared and
  its own clock. It is a target handed to us, not a question asked of us.
- **`IddCxSwapChainReportFrameStatistics()`** — the driver reports back what
  happened: `FrameAcquireQpcTime`, `SendStartQpcTime`, `SendStopQpcTime`,
  `SendCompleteQpcTime`, `ProcessedPixelCount`, `FrameSizeInBytes`. This is
  telemetry flowing to the OS, and it is the only channel we have for saying
  anything about timing at all.

So the synchronisation cannot be delegated. The OS believes the display refreshes
at whatever rational we declared, paces DWM to that, and hands us a target time
derived from its own clock. **Everything that reconciles that belief with the
device's actual raster has to happen inside this driver.**

## What the device already has

From `blitscrt_regs.h`, before any new work:

| | |
| --- | --- |
| `BLITSCRT_STAT_VBLANK` | status bit, currently in blanking |
| `BLITSCRT_STAT_FIELD` | which field, for interlaced modes |
| `BLITSCRT_REG_FRAME_COUNT` | read-only, increments per **field** |
| `BLITSCRT_STAT_UNDERRUN` | the line fetcher has fallen behind |

That is most of what a host needs. A status bit alone is not enough — polling it
over USB tells you "in blanking" with a round trip's worth of uncertainty, by
which time it may be false. The frame counter is the more useful of the two,
because a counter plus a timestamp is a *phase*, and a phase can be extrapolated.

`UNDERRUN` is worth wiring into the host's telemetry regardless. It is the
device saying the host is writing faster than the fetcher can absorb, which is
the one failure this design can cause that the host cannot otherwise see.

## Getting the vblank to the host

Four options, in increasing order of how much new device work they need.

### 1. Extend the status read (recommended first step)

GUD already reads status after every `SET_BUFFER` when
`GUD_DISPLAY_FLAG_STATUS_ON_SET` is set, and this device sets it. That read is
already paid for. Extending its payload to carry the field counter and a device
timestamp costs **nothing on the wire**.

```
struct gud_status_ext {
    uint8_t  status;          /* as now */
    uint8_t  flags;           /* FIELD, VBLANK, UNDERRUN */
    uint16_t reserved;
    uint32_t field_count;     /* BLITSCRT_REG_FRAME_COUNT */
    uint64_t device_qpc_ns;   /* free running, nanoseconds */
};
```

Every frame the host sends, it learns exactly where the raster was when the
device processed it. The limitation is that samples only arrive when frames are
being sent, so the estimate goes stale on an idle desktop — which is precisely
when it does not matter, and one frame of catch-up fixes it.

This is the cheapest possible experiment and it is enough to build and validate
the whole clock model before committing to an endpoint.

### 2. An interrupt IN endpoint

The device NAKs until vblank, then returns one packet carrying the same
structure. The host keeps a thread permanently waiting on it.

At high speed a `bInterval` of 1 gives a 125 us service interval, so the host
learns of a vblank within 125 us of it happening, plus scheduling jitter. That
is a twentieth of the 1.4 ms window and comfortably good enough.

This is the right long-term answer: it gives a steady phase reference whether or
not frames are flowing, it costs no bandwidth when idle, and it is what the
hardware's own event most naturally maps onto. It needs an endpoint added to the
gadget and a `WdfUsbTargetPipeReadSynchronously` loop on this side, and the
driver's transport abstraction (`struct gud_transport`) grows an `interrupt_in`
alongside `bulk_out`.

### 3. A bulk IN endpoint

Simpler on some gadget stacks than an interrupt endpoint, but bulk has no
guaranteed service interval — it is scheduled from whatever bandwidth is left
after periodic traffic. The jitter is unbounded in principle. Not worth it when
option 2 exists.

### 4. Polling a vendor control request

No new endpoint, but every poll costs a control transfer — the same 0.83 ms-ish
microframe that makes rects expensive — and to catch the phase at all you would
have to poll far faster than the frame rate. It spends the exact resource the
budget is tightest on. Mentioned to be dismissed.

**Recommendation: 1 to prove the model, then 2 for the real thing.** They share
the payload structure, so the host code written for 1 is most of the host code
for 2.

## Mapping the device clock onto the host clock

This is the part that is easy to underestimate. The device counts in its own
clock domain, derived from its own crystal; the host counts QPC. The two drift
by tens of parts per million, which is *seconds per day* — far too much to
ignore over a gaming session, and far too little to notice in a five-minute
test. Getting this wrong produces something that works on the bench and tears
once an hour.

The standard treatment applies, and it is worth using the standard names for it
because the failure modes are well documented. Each sample is a round trip:

```
t0 = QPC before the request
                          -> device samples its counter, d
t1 = QPC after the reply
```

The true correspondence between `d` and host time lies somewhere in `[t0, t1]`.
Best estimate is the midpoint with an uncertainty of half the round trip, which
is Cristian's algorithm, and the same reasoning NTP and PTP are built on.

Two properties matter for the estimator:

- **Filter on round-trip time, not on age.** USB round trips have a hard floor
  and a long tail; a sample with a short round trip is far more informative than
  a recent one with a long round trip. Keeping the best few samples from a
  window beats averaging all of them, and is what PTP does.
- **Fit rate as well as offset.** A pure offset estimate re-converges forever
  and never predicts. Fitting a line over a window of samples gives the ppm
  difference, and with rate in hand the next vblank can be extrapolated many
  frames ahead:

```
vblank_qpc(n) = offset + n * field_period_qpc
field_period_qpc = device_field_period * (1 + ppm/1e6)
```

A least-squares fit over a sliding window is the obvious start. A PI controller
locking a local oscillator to the samples is the other well-trodden option and
is more resistant to outliers. Either is fine; what is not fine is a naive
average, which is what makes clock code work in testing and drift in the field.

## Using the phase once it is known

### Scheduling the write

With `vblank_qpc(n)` predictable, the frame loop stops sending as soon as it has
pixels and instead aims the transfer at the window. Conversion and compression
happen early; the bulk write is held until the estimated blanking, minus a guard
band for USB scheduling jitter and the time the transfer itself takes.

The guard band is the honest source of the remaining risk. USB gives no
guarantee about when a bulk transfer actually leaves — it is scheduled against
other traffic in the microframe. Aiming at the middle of the window rather than
its start is the cheap mitigation.

### Beam chasing, for what does not fit

A full 648x480i surface is 1.59 ms against a 1.43 ms window, so it cannot be
written in one blanking. Two ways out:

- **Split on the interlace.** The device interleaves on read, so writing the
  frame in two halves across two consecutive blankings falls out of what the
  hardware already does. It halves the effective update rate for full-surface
  changes, which for a 480i mode is 30 full updates a second — the frame rate
  the mode actually has.
- **Write behind the beam.** With the phase known, the current scanline is
  known too, so a rect entirely above the beam can be written at any time
  without tearing. This is what fixed-function consoles did and it is strictly
  better than waiting, at the cost of arithmetic that is wrong in an
  embarrassing way if the phase estimate is off.

Split-on-interlace first. Beam chasing is an optimisation and should not be the
thing being debugged while the clock model is still being trusted.

### The rate mismatch that cannot be removed

The OS paces DWM to the `vSyncFreq` the driver declared. The device runs at a
rate set by its PLL and crystal. Even with the declared rational made exact —
and it can be, `vSyncFreq` is a numerator and denominator, not a float — the
host's *clock* and the device's still differ by ppm.

So frames will occasionally have to be dropped or repeated, and this is not a
bug to be fixed but a rate to be managed. What the design can do is make it rare
and put it somewhere harmless: a repeat during blanking is invisible, a repeat
mid-scanout is a stutter. Making it rare is what the ppm estimate buys.

For the emulation case specifically the right answer is further up the stack:
GroovyMAME already adapts its own pacing to the display it is on, and a display
that reports its true rate and holds phase is exactly what that machinery wants.

### What to tell Windows

`IddCxSwapChainReportFrameStatistics()` should be filled in honestly once the
timing is known — `SendStartQpcTime` and `SendStopQpcTime` around the bulk
write, `SendCompleteQpcTime` from the completion, and `ProcessedPixelCount` and
`FrameSizeInBytes` which the frame loop already computes for its own per-second
line. This is telemetry rather than control: it will not change how the OS
paces us, but it is what makes the OS's own diagnostics tell the truth about
this display, and it costs nothing.

## How this fails, and what each failure looks like

Worth writing down in advance, because several of these look like each other.

| symptom | likely cause |
| --- | --- |
| A tear line that sits still | Phase estimate is stable but wrong. Offset error. |
| A tear line that crawls slowly up or down | Rate error, ppm. The classic sign of offset-only estimation. |
| Tearing only under heavy update | Writes exceeding the window; the guard band is too small or the rect is too big for one blanking. |
| Occasional stutter, no tearing | Frame repeat or drop landing outside blanking. |
| `UNDERRUN` set | Host writing faster than the line fetcher absorbs — unrelated to phase, and the device is saying so. |
| Correct on the bench, tears after an hour | Rate not being fitted at all. |

## Milestones

In the order they should be proved, matching how `BRINGUP.md` is organised.

1. **The device reports a field counter and a timestamp.** Extend the status
   payload. **Proved when:** `gudprobe` can print the counter advancing at the
   field rate and the timestamp advancing monotonically.
2. **The host can predict a vblank.** Build the estimator against the extended
   status read alone, and log predicted-versus-actual. **Proved when:** the
   residual is under 100 us over a ten-minute run, and the fitted ppm is stable.
   No pixels change; this is arithmetic being checked against reality.
3. **Writes are aimed at the window.** Hold the bulk write until the predicted
   blanking. **Proved when:** a full-screen flicker test on a 240p mode shows no
   tear line, and `UNDERRUN` stays clear.
4. **The interrupt endpoint replaces polling.** Same payload, steady phase when
   idle. **Proved when:** the estimator holds lock with no frames flowing, and
   reacquires within one frame after a mode change.
5. **480i splits across fields.** **Proved when:** a full-surface update at
   648x480i60 shows no tearing and the device reports no underrun.
6. **Frame statistics reported.** **Proved when:** the OS's own presentation
   diagnostics agree with the driver's per-second line.

## Open questions

Things I do not know and that should be settled on the device side before the
host commits to a shape.

- **What clock should the device timestamp come from, and what is its
  resolution?** A counter in the video clock domain is ideal — it is the clock
  that actually matters — but it stops during a modeset. An HPS monotonic clock
  is easier and is one domain removed from the raster.
- **Does the field counter latch atomically with the timestamp?** If they are
  read separately, a sample can straddle a field boundary and be wrong by a
  whole period. It should be one register read, or a snapshot latched by
  reading the first.
- **What happens across a modeset?** The period changes, so the estimator must
  be told rather than left to converge — it would take seconds and tear the
  whole time. A field-counter reset or an explicit sequence number in the status
  payload would let the host discard its history immediately.
- **Is there a scanline register, or only vblank?** Beam chasing needs the
  current line. Vblank alone is enough for milestones 1 to 5.
- **How does this interact with `SET_STATE_COMMIT`?** The fabric latches timing
  on the next vblank, so a modeset is already vblank-synchronised on the device
  side; the host should know when that has taken effect rather than guessing.
