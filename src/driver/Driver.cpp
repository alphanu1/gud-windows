// SPDX-License-Identifier: MIT
//
// Driver.cpp -- IddCx driver for a GUD display device.
//
// This file needs the WDK to build and cannot be compiled or run anywhere
// else, so treat the IddCx struct field names as needing one pass against your
// own IddCx.h before first build. The version differences are all in
// IDDCX_METADATA and the mode-reporting argument structures; the shape of the
// thing does not change between versions.

#include "Driver.h"

#include <stdio.h>

using namespace Microsoft::WRL;
using namespace gudwin;

// ===========================================================================
// Bring-up logging
// ===========================================================================
//
// The driver runs in WUDFHost in Session 0: printf goes nowhere, a message box
// hangs the process, and a debugger means attaching to WUDFHost. UMDF's own
// channel reports a failed load as one line with an NTSTATUS and no indication
// of which call produced it, which is not enough to work with when every
// experiment costs a driver reinstall and a reboot.
//
// So the driver says where it got to, in a file. WUDFHost runs as LocalSystem,
// so ProgramData is writable. Opened and closed per line rather than held: this
// has to survive the process being killed, and losing a buffered last line is
// exactly the case being diagnosed.
static void GudLog(const char* fmt, ...)
{
    CreateDirectoryA("C:\\ProgramData\\gud-windows", nullptr);

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\ProgramData\\gud-windows\\driver.log", "a") || !f)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

// ===========================================================================
// USB transport
// ===========================================================================

NTSTATUS UsbTransport::Init(WDFDEVICE device)
{
    NTSTATUS status;
    WDF_USB_DEVICE_CREATE_CONFIG cfg;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS sel;

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&cfg, USBD_CLIENT_CONTRACT_VERSION_602);
    GudLog("UsbTransport::Init: entered");
    status = WdfUsbTargetDeviceCreateWithParameters(device, &cfg,
                                                    WDF_NO_OBJECT_ATTRIBUTES,
                                                    &UsbDevice);
    GudLog("  WdfUsbTargetDeviceCreateWithParameters -> 0x%08X", (unsigned)status);

    if (!NT_SUCCESS(status)) {
        // Fall back to the plain form. WithParameters carries a USBD client
        // contract version, which is a KMDF concept -- a UMDF driver reaches
        // USB through winusb.sys and never speaks USBD, so the contract
        // version may be what is being rejected rather than anything about
        // the device. Trying both in one pass rather than guessing, because
        // each guess otherwise costs a reinstall.
        status = WdfUsbTargetDeviceCreate(device, WDF_NO_OBJECT_ATTRIBUTES,
                                          &UsbDevice);
        GudLog("  WdfUsbTargetDeviceCreate (plain)      -> 0x%08X", (unsigned)status);
    }
    if (!NT_SUCCESS(status))
        return status;

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&sel);
    status = WdfUsbTargetDeviceSelectConfig(UsbDevice, WDF_NO_OBJECT_ATTRIBUTES,
                                            &sel);
    GudLog("  WdfUsbTargetDeviceSelectConfig -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    Interface = sel.Types.SingleInterface.ConfiguredUsbInterface;
    IfNum = WdfUsbInterfaceGetInterfaceNumber(Interface);

    // One bulk OUT endpoint, no IN endpoint: everything the host reads comes
    // back on ep0. Anything else means the wrong interface.
    BYTE count = WdfUsbInterfaceGetNumConfiguredPipes(Interface);
    for (BYTE i = 0; i < count; i++) {
        WDF_USB_PIPE_INFORMATION info;
        WDF_USB_PIPE_INFORMATION_INIT(&info);
        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(Interface, i, &info);

        if (WdfUsbPipeTypeBulk == info.PipeType &&
            !WdfUsbTargetPipeIsInEndpoint(pipe)) {
            BulkOut = pipe;
            // The write must not be terminated with a zero-length packet. The
            // device reads a length rounded up to a packet boundary and treats
            // the result as one rect; a trailing ZLP is a zero-byte rect and
            // the bulk stream desynchronises with nothing to resynchronise
            // against. Explicitly off rather than relying on the default.
            WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);
        }
    }
    return BulkOut ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

int UsbTransport::Ctrl(bool in, uint8_t request, uint16_t value,
                       void* buf, size_t len)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG moved = 0;

    // Vendor request, recipient interface, wIndex = bInterfaceNumber. This is
    // gud_usb_control_msg() in the in-tree Linux driver, and a device is
    // entitled to depend on it.
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &packet,
        in ? BmRequestDeviceToHost : BmRequestHostToDevice,
        BmRequestToInterface,
        request, value, IfNum);

    if (len)
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, buf, (ULONG)len);

    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts,
        WDF_REL_TIMEOUT_IN_MS(1500));

    NTSTATUS status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        UsbDevice, WDF_NO_HANDLE, &opts, &packet,
        len ? &desc : nullptr, &moved);

    return NT_SUCCESS(status) ? (int)moved : GUD_E_IO;
}

int UsbTransport::CtrlIn(void* ctx, uint8_t r, uint16_t v, void* buf, size_t len)
{ return static_cast<UsbTransport*>(ctx)->Ctrl(true, r, v, buf, len); }

int UsbTransport::CtrlOut(void* ctx, uint8_t r, uint16_t v, const void* buf, size_t len)
{ return static_cast<UsbTransport*>(ctx)->Ctrl(false, r, v, const_cast<void*>(buf), len); }

int UsbTransport::BulkOutFn(void* ctx, const void* buf, size_t len)
{
    auto* self = static_cast<UsbTransport*>(ctx);
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG moved = 0;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, const_cast<void*>(buf), (ULONG)len);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    // Bounded, because an infinite write is how a display driver wedges the
    // compositor when the cable is pulled mid-transfer. Matches the Linux
    // driver's own bulk timeout.
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(3000));

    NTSTATUS status = WdfUsbTargetPipeWriteSynchronously(
        self->BulkOut, WDF_NO_HANDLE, &opts, &desc, &moved);

    if (!NT_SUCCESS(status)) {
        // A stalled pipe stays stalled until it is cleared, and a host that
        // does not clear it never sends another pixel.
        WdfUsbTargetPipeResetSynchronously(self->BulkOut, WDF_NO_HANDLE, nullptr);
        return GUD_E_IO;
    }
    return (int)moved;
}

void UsbTransport::Fill(gud_transport* t)
{
    t->ctrl_in  = CtrlIn;
    t->ctrl_out = CtrlOut;
    t->bulk_out = BulkOutFn;
    t->ctx      = this;
}

// ===========================================================================
// Mode plumbing -- the join described in modeline.h
// ===========================================================================

namespace {

// Where an administrator drops modelines. A file rather than the registry
// because it wants to be editable and diffable, and because it is the same
// vocabulary the device's own blitscrt.ini will use on the card -- one person
// operating both should not have to learn two spellings.
const char* kIniPath = "C:\\ProgramData\\gud-windows\\modelines.ini";

// Where the driver writes what it is actually running, for anything that needs
// the real timings rather than the ones Windows reports. See WriteActiveModes.
const char* kActivePath = "C:\\ProgramData\\gud-windows\\modes.active";

// The store, reachable from EvtIddCxParseMonitorDescription.
//
// That callback is handed a description and a mode buffer and nothing else --
// no monitor, no adapter, no device -- so there is no object to hang a context
// off and no way to reach the DeviceContext by the usual route. A file-scope
// pointer is the way to it.
//
// This assumes one device, which is already assumed throughout: GET_CONNECTORS
// takes connector 0, MaxMonitorsSupported is 1, and DESIGN.md's "what is not
// here" says as much. A second GUD device on one machine needs this replaced
// along with the rest of that assumption.
modeline_store* g_ParseStore = nullptr;

// Guards the store, which stopped being written once at start the moment the
// INI could be re-read while running. The mode-watch timer rewrites it in
// place; the IddCx callbacks read it. Both run at passive level on different
// threads, so a commit could otherwise walk a store mid-rebuild -- and the
// thing it would come away with is a set of porches, which go to a deflection
// circuit.
//
// File-scope for the same reason g_ParseStore is: the callback that needs it
// most has no handle to reach a device by.
//
// Nothing may hold this across a call into IddCx or into the device. IddCx
// answers UpdateModes by calling straight back into the mode callbacks, which
// take it, and gud_set_state is USB I/O with a timeout on it. Copy out what is
// needed, release, then call.
std::mutex g_StoreLock;

IDDCX_TARGET_MODE MakeTargetMode(const gud_display_mode_req& m)
{
    IDDCX_TARGET_MODE mode{};
    mode.Size = sizeof(mode);

    // IddCx wants pixel rate, active size and a rational refresh. It has
    // nowhere to put porches, which is the entire problem: this is a lossy
    // projection of the modeline, and the modeline itself has to be found
    // again at commit time from the store.
    // IDDCX_TARGET_MODE::TargetVideoSignalInfo is a DISPLAYCONFIG_TARGET_MODE,
    // which is a one-field wrapper around the signal info -- so the fields sit
    // one level deeper than they do on IDDCX_PATH, which carries a bare
    // DISPLAYCONFIG_VIDEO_SIGNAL_INFO. Note the lowercase spelling of the
    // inner member; the outer one is capitalised.
    auto& si = mode.TargetVideoSignalInfo.targetVideoSignalInfo;

    si.totalSize.cx  = m.htotal;
    si.totalSize.cy  = m.vtotal;
    si.activeSize.cx = m.hdisplay;
    si.activeSize.cy = m.vdisplay;
    si.pixelRate     = (UINT64)m.clock * 1000;
    si.hSyncFreq.Numerator   = m.clock * 1000;
    si.hSyncFreq.Denominator = m.htotal;

    // vSync as an exact rational of millihertz over 1000, rather than
    // pixelRate over htotal*vtotal. Two reasons. The interlace doubling is
    // already in gud_mode_refresh_mhz(), and forgetting it here reports a 480i
    // mode at 30 Hz -- which Windows accepts and then drives at half rate.
    // And an exact rational is what makes the commit-time lookup exact: these
    // numbers come straight back in IDARG_IN_COMMITMODES_PATH.
    si.vSyncFreq.Numerator   = gud_mode_refresh_mhz(&m);
    si.vSyncFreq.Denominator = 1000;

    // videoStandard = 255 and vSyncFreqDivider = 1, as the IndirectDisplay
    // sample sets them. Both live in the AdditionalSignalInfo bitfield and both
    // are zero if the structure is merely zero-initialised, which is what this
    // did -- and a zero videoStandard was enough for IddCxMonitorArrival to
    // refuse the mode list with STATUS_INVALID_PARAMETER and name nothing.
    //
    // 255 is "other": there is no D3DKMDT video standard for a 15 kHz arcade
    // raster, and claiming one of the named ones would be a lie.
    //
    // The divider is 1 here and 0 on monitor modes -- see MakeMonitorMode. It
    // divides vSyncFreq to give the rate at which the OS updates the desktop,
    // so 1 means every field. The header says a target mode's divider cannot be
    // zero and a monitor mode's must be, which reads as a contradiction until
    // you notice the two structures are being described in one comment.
    si.AdditionalSignalInfo.videoStandard    = 255;
    si.AdditionalSignalInfo.vSyncFreqDivider = 1;

    // Interlace: report it, because the sink is a 15 kHz CRT and 480i is the
    // whole point. Note the asymmetry with GUD, which carries rects in the
    // mode's coordinate space with vdisplay = 480 for 480i and lets the device
    // interleave on read. So the surface Windows hands over is the full
    // progressive one either way, and interlace changes nothing above this
    // line -- only what the raster does with it.
    //
    // It also does not survive the round trip: a mode set here as INTERLACED
    // comes back through EvtIddCxAdapterCommitModes as PROGRESSIVE. Set it
    // anyway, since it describes the mode honestly and the OS is free to
    // ignore it, but nothing downstream may key on getting it back -- see the
    // don't-care in the commit-time lookup.
    si.scanLineOrdering =
        (m.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE)
        ? DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED
        : DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    return mode;
}

// The access predicted to disagree with a real IddCx.h, and it did -- though
// the other way round from the guess. A commit path carries IDDCX_PATH, which
// holds a bare DISPLAYCONFIG_VIDEO_SIGNAL_INFO; it is IDDCX_TARGET_MODE that
// wraps one in a DISPLAYCONFIG_TARGET_MODE. Isolated here regardless, so a
// future version moving it again is one line.
const DISPLAYCONFIG_VIDEO_SIGNAL_INFO&
SignalInfo(const IDDCX_PATH& p)
{
    return p.TargetVideoSignalInfo;
}

// `origin` says where the mode came from, and IddCx cares which. Modes handed
// back from EvtIddCxParseMonitorDescription are answering "what does this
// descriptor say", so they are MONITORDESCRIPTOR; modes from
// EvtIddCxMonitorGetDefaultDescriptionModes are the driver's own and are
// DRIVER. Reporting DRIVER out of the parse callback is a contradiction.
IDDCX_MONITOR_MODE MakeMonitorMode(const gud_display_mode_req& m,
                                   IDDCX_MONITOR_MODE_ORIGIN origin)
{
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo =
        MakeTargetMode(m).TargetVideoSignalInfo.targetVideoSignalInfo;

    // Zero on a monitor mode, where MakeTargetMode leaves 1. The header is
    // explicit that a monitor mode's vSyncFreqDivider has to be zero; the
    // divider only means anything for a target mode, where it says how often
    // the OS refreshes the desktop relative to the signal.
    mode.MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 0;
    return mode;
}

} // namespace

// Last write time of the INI, or 0 if it is not there. Not there is ordinary:
// a machine with no per-game modelines runs on the device's list alone.
static ULONGLONG IniLastWrite()
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(kIniPath, GetFileExInfoStandard, &fad))
        return 0;
    ULARGE_INTEGER t;
    t.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
    t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

// The device's first hardware id, as ASCII. Empty if it cannot be had.
//
// This is what tells a reader which device the published mode list belongs to.
// EnumDisplayDevices reports the same string as the display adapter's DeviceID
// -- USB\VID_1D50&PID_614D&REV_0100 for the board this was brought up on -- so
// a tool holding one can match it against the other, which is the only thing
// separating this display from any other USB-attached one on the machine.
static void QueryHardwareId(WDFDEVICE device, char* out, size_t len)
{
    out[0] = '\0';

    WCHAR buf[256];
    ULONG got = 0;
    NTSTATUS s = WdfDeviceQueryProperty(device, DevicePropertyHardwareID,
                                        sizeof(buf), buf, &got);
    if (!NT_SUCCESS(s) || got < sizeof(WCHAR))
        return;

    // A REG_MULTI_SZ, most specific first. The first entry is the one that
    // carries the revision and the one EnumDisplayDevices echoes.
    buf[(got / sizeof(WCHAR)) - 1] = L'\0';
    WideCharToMultiByte(CP_ACP, 0, buf, -1, out, (int)len, nullptr, nullptr);
    out[len - 1] = '\0';
}

// Publish the active store as a file, in the same spelling the INI uses.
//
// Windows describes this display's modes badly and there is no way to fix that
// from here: an interlaced mode reaches any application as progressive, so a
// 480i timing reads as 480p and its line rate as 31 kHz rather than 15.75. Any
// tool reasoning about whether a mode suits a 15 kHz CRT -- Switchres is the
// one this project exists for -- will rule out the modes it most wants.
//
// So the driver writes down what it actually has. Same syntax as the INI it
// reads, so one parser covers both, and readable enough to be worth having
// when a mode does not behave.
//
// Written on every rebuild, so it tracks the INI. Best-effort: nothing here
// fails a mode load, and a tool that cannot read it is no worse off than one
// that never looked.
static void WriteActiveModes(const DeviceContext* ctx)
{
    FILE* f = nullptr;
    if (fopen_s(&f, kActivePath, "w") || !f)
        return;

    fprintf(f, "; gud-windows active mode list -- generated, do not edit.\n");
    fprintf(f, "; The interlace flag here is authoritative. Windows reports\n");
    fprintf(f, "; every one of these modes as progressive.\n");

    // Which device this list belongs to. A reader must check it rather than
    // assume the file describes whatever display it happens to be looking at:
    // more than one USB-attached display can exist on a machine, and only one
    // of them is this one.
    char hwid[128];
    QueryHardwareId(ctx->Device, hwid, sizeof hwid);
    if (hwid[0])
        fprintf(f, "; device = %s\n", hwid);

    fprintf(f, "[modelines]\n");

    for (unsigned i = 0; i < ctx->Modes.n; i++) {
        char line[256];
        if (modeline_format(&ctx->Modes.e[i].mode, ctx->Modes.e[i].name,
                            line, sizeof line) == 0)
            fprintf(f, "%s\n", line);
    }
    fclose(f);
}

// Rebuild Modes from the cached device list plus whatever the INI says now.
//
// Device list first, INI second, so a hand-written modeline overrides an
// advertised one with the same geometry and rate. Someone who wrote out a
// modeline meant it.
static NTSTATUS RebuildStore(DeviceContext* ctx)
{
    std::lock_guard<std::mutex> lk(g_StoreLock);

    modeline_store_reset(&ctx->Modes);
    for (unsigned i = 0; i < ctx->DeviceModes.n; i++)
        modeline_store_add(&ctx->Modes, &ctx->DeviceModes.e[i].mode,
                           ctx->DeviceModes.e[i].name, 1);
    modeline_store_load_ini(&ctx->Modes, kIniPath);

    // Publish it for EvtIddCxParseMonitorDescription, which has no handle to
    // reach it by. Set after loading so the callback never sees a half-filled
    // store, and set on every reload so it tracks the INI.
    g_ParseStore = &ctx->Modes;
    ctx->IniStamp = IniLastWrite();
    WriteActiveModes(ctx);

    return ctx->Modes.n ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

NTSTATUS gudwin::ReloadModes(DeviceContext* ctx)
{
    // The only place that asks the device. GUD sends its mode list once during
    // enumeration, so this belongs on the start path and nowhere else; every
    // later reload works from the cached copy.
    if (gud_read_modes(&ctx->Gud) != 0)
        return STATUS_DEVICE_DATA_ERROR;
    modeline_store_reset(&ctx->DeviceModes);
    modeline_store_load_device(&ctx->DeviceModes, &ctx->Gud);

    return RebuildStore(ctx);
}

// Push the current store to the OS as the monitor's target mode list.
//
// This is what makes a mode added to the INI appear without a replug. IddCx
// reads the mode list once at monitor arrival, so the alternative is departure
// and arrival -- which tears down the swap chain, blanks the CRT and moves
// every window off the display. IddCxMonitorUpdateModes exists precisely so a
// driver whose capabilities change does not have to do that.
static NTSTATUS PublishModes(DeviceContext* ctx, IDDCX_UPDATE_REASON reason)
{
    if (!ctx->Monitor)
        return STATUS_INVALID_DEVICE_STATE;

    std::vector<IDDCX_TARGET_MODE> modes;
    {
        std::lock_guard<std::mutex> lk(g_StoreLock);
        modes.reserve(ctx->Modes.n);
        for (unsigned i = 0; i < ctx->Modes.n; i++)
            modes.push_back(MakeTargetMode(ctx->Modes.e[i].mode));
    }
    if (modes.empty())
        return STATUS_INVALID_DEVICE_STATE;

    IDARG_IN_UPDATEMODES in{};
    in.Reason          = reason;
    in.TargetModeCount = (UINT)modes.size();
    in.pTargetModes    = modes.data();

    NTSTATUS s = IddCxMonitorUpdateModes(ctx->Monitor, &in);
    GudLog("PublishModes: %u modes -> 0x%08X", (unsigned)modes.size(), (unsigned)s);
    return s;
}

static NTSTATUS CreateAndArriveMonitor(DeviceContext* ctx, IDDCX_ADAPTER adapter);

// A value that changes whenever the set of modes changes.
//
// Counting them is not enough, and getting that wrong is silent: a tool that
// replaces one generated modeline with another -- which is exactly what a
// Switchres backend does between games -- leaves the count alone while
// changing the geometry. The monitor is then never re-enumerated, Windows goes
// on offering the mode that no longer exists, and the new one cannot be
// selected. Observed, with a store going 3 -> 3 across a 384x224 becoming
// 640x480.
//
// Everything that identifies a timing goes in, porches included, so editing a
// modeline in place re-enumerates too.
static uint64_t StoreSignature(const modeline_store* s)
{
    uint64_t h = 1469598103934665603ull;         // FNV-1a
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            h ^= (uint8_t)(v >> (i * 8));
            h *= 1099511628211ull;
        }
    };

    for (unsigned i = 0; i < s->n; i++) {
        const auto& m = s->e[i].mode;
        mix(m.clock);
        mix(m.hdisplay); mix(m.hsync_start); mix(m.hsync_end); mix(m.htotal);
        mix(m.vdisplay); mix(m.vsync_start); mix(m.vsync_end); mix(m.vtotal);
        mix(m.flags);
    }
    return h;
}

// Periodic: has the INI changed, and if so does the OS need telling?
static void EvtModeWatchTimer(WDFTIMER timer)
{
    auto* ctx = DeviceGetContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    if (!ctx || !ctx->Monitor)
        return;

    ULONGLONG now = IniLastWrite();
    if (now == ctx->IniStamp)
        return;

    unsigned before = ctx->Modes.n;
    uint64_t sig_before = StoreSignature(&ctx->Modes);
    NTSTATUS s = RebuildStore(ctx);
    uint64_t sig_after = StoreSignature(&ctx->Modes);

    {
        std::lock_guard<std::mutex> lk(g_StoreLock);
        GudLog("ModeWatch: modelines.ini changed -- store %u -> %u, 0x%08X",
               before, ctx->Modes.n, (unsigned)s);
        for (unsigned i = 0; i < ctx->Modes.n; i++) {
            const auto& m = ctx->Modes.e[i].mode;
            GudLog("    [%u] %s %ux%u%s clk=%u h %u/%u/%u/%u v %u/%u/%u/%u",
                   i, ctx->Modes.e[i].name, m.hdisplay, m.vdisplay,
                   (m.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p", m.clock,
                   m.hdisplay, m.hsync_start, m.hsync_end, m.htotal,
                   m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal);
        }
    }
    if (!NT_SUCCESS(s))
        return;

    // Two steps, because they update two different lists.
    //
    // IddCxMonitorUpdateModes replaces the monitor's *target* modes -- the
    // timings the driver can drive -- and takes effect at once, without
    // disturbing anything on screen. Existing modes whose porches were edited
    // are corrected by this alone.
    //
    // It does not touch the monitor's own mode list, which is what Windows
    // builds the resolution list in Display Settings from. That list is read
    // once, from EvtIddCxParseMonitorDescription, at arrival. So a genuinely
    // new geometry is published here and still cannot be selected: verified,
    // with UpdateModes returning STATUS_SUCCESS and EnumDisplaySettings
    // continuing to offer only the modes present at arrival.
    //
    // Re-arriving the monitor is what makes it selectable. It is disruptive --
    // the swap chain goes, the CRT blanks, and windows on the display move --
    // so it is done only when the set of modes actually changed, not on every
    // INI write. Touching the file without changing a timing costs nothing.
    // OTHER as the reason, because none of the named ones fit: the list did not
    // change because of power, bandwidth or configuration. Someone wrote a
    // modeline down.
    PublishModes(ctx, IDDCX_UPDATE_REASON_OTHER);

    if (sig_after != sig_before && ctx->Adapter) {
        GudLog("ModeWatch: mode set changed, re-enumerating the monitor");
        if (ctx->Monitor) {
            IddCxMonitorDeparture(ctx->Monitor);
            ctx->Monitor = nullptr;
        }
        CreateAndArriveMonitor(ctx, ctx->Adapter);
    }
}

NTSTATUS gudwin::StartModeWatch(DeviceContext* ctx)
{
    if (ctx->ModeWatch)
        return STATUS_SUCCESS;

    WDF_TIMER_CONFIG cfg;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&cfg, EvtModeWatchTimer, 2000);

    // Automatic serialisation off: it serialises the timer against the parent
    // device's other callbacks, and this driver's other callbacks come from
    // IddCx rather than WDF, so there is nothing there for WDF to serialise
    // against. g_StoreLock is what actually guards the shared state.
    cfg.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = ctx->Device;

    // ExecutionLevel is deliberately left inherited. Asking for
    // WdfExecutionLevelPassive here fails with STATUS_NOT_SUPPORTED, because
    // the WDFDEVICE this hangs off never declared a level either, so it
    // inherits the driver's -- dispatch -- and a passive child of a dispatch
    // parent is not a combination WDF allows. The callback reads a file, so
    // passive is what it needs; it gets it regardless, because UMDF has no
    // dispatch level to run at. Every callback in a UMDF host is passive and
    // ExecutionLevel is bookkeeping inherited from the KMDF object model.

    NTSTATUS s = WdfTimerCreate(&cfg, &attrs, &ctx->ModeWatch);
    GudLog("StartModeWatch: WdfTimerCreate -> 0x%08X", (unsigned)s);
    if (NT_SUCCESS(s))
        WdfTimerStart(ctx->ModeWatch, WDF_REL_TIMEOUT_IN_MS(2000));
    return s;
}

// ===========================================================================
// IddCx callbacks
// ===========================================================================

// Create the monitor and announce it.
//
// Factored out of EvtIddCxAdapterInitFinished because it runs twice: once when
// the adapter first comes up, and again whenever the modeline store changes
// shape. A monitor's mode list is read once, at arrival, so a mode added to
// the INI while running only reaches Windows by taking the monitor away and
// bringing a new one back.
static NTSTATUS CreateAndArriveMonitor(DeviceContext* ctx, IDDCX_ADAPTER adapter)
{
    // Create the monitor. Connector index 0; see gud_probe().
    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    // DIAGNOSTIC: HDMI, as the IndirectDisplay sample reports.
    //
    // This was HD15, on the reasoning that the only hop with a real connector
    // is the analog one out of the device into the CRT. That reasoning is about
    // what the user is told, and it may be that Windows will not accept it:
    // HD15 describes a VGA socket on a graphics card, and this is an indirect
    // display reached over USB. Everything else in IDDCX_MONITOR_INFO has now
    // been eliminated as the cause of IddCxMonitorArrival failing, and this is
    // the last field where the driver differs from the working sample.
    //
    // If this is what it was, the honest value is probably
    // DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED rather than HDMI --
    // revisit once it is known which way this goes.
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;   // VGA
    info.ConnectorIndex = 0;

    // No EDID.
    //
    // The device implements one and does not send it, deliberately: tested on
    // hardware, a host given that EDID picks a mode wider than the raster and
    // drops frames, with a bare name descriptor as much as with sync range
    // limits. The mode list is the only thing that works. Windows is less
    // tolerant of a monitor with no EDID than Linux is -- it will show the
    // display as "Generic PnP Monitor" -- but a wrong mode on a fixed-frequency
    // deflection circuit is worse than an unhelpful name.
    //
    // If a name turns out to be needed, build a block with a *timing*
    // descriptor for the preferred mode rather than range limits, which is the
    // untried variant and the one the device's own notes point at.
    // UNINITIALIZED, not EDID.
    //
    // This driver sends no EDID -- see DESIGN.md, and the reasons are on
    // hardware -- so there is no description to declare. Saying EDID and then
    // supplying DataSize 0 with a null pointer is a contradiction, and
    // IddCxMonitorCreate rejects it with STATUS_INVALID_PARAMETER. The
    // enumeration has a value for having nothing to say, which is this one.
    //
    // IddCx then asks EvtIddCxMonitorGetDefaultDescriptionModes for the mode
    // list instead, which is where the modeline store answers, and
    // EvtIddCxParseMonitorDescription is never given anything to parse.
    // A minimal EDID, with deliberately no timing in it.
    //
    // IddCxMonitorCreate refuses a description of UNINITIALIZED with no data:
    // it wants an EDID. The header comment saying "if the monitor does not have
    // any description data this should be set to NULL" is stale -- the field is
    // a struct by value, not a pointer, so there is no NULL to pass.
    //
    // This is the variant DESIGN.md names and had not been tried: a block with
    // no detailed timing descriptors at all. Windows can derive no modes from
    // it, so it asks EvtIddCxMonitorGetDefaultDescriptionModes instead, which
    // is where the modeline store answers -- and the store is the only thing
    // that knows the porches. An EDID carrying timings would put modes in front
    // of Windows that the store has never heard of, and the commit path would
    // rightly refuse them.
    //
    // Established and standard timings are zeroed for the same reason. The
    // descriptors are a name and three dummies. Checksum is computed rather
    // than hand-carried, because a wrong one is silent.
    // A minimal EDID carrying no timing at all.
    //
    // IddCxMonitorCreate needs a description -- a type of UNINITIALIZED with no
    // data is refused, and the header comment saying to pass NULL is stale,
    // since the field is a struct by value. So there has to be an EDID, and
    // this is the variant DESIGN.md named and had not tried: no detailed timing
    // descriptors, no established timings, no standard timings.
    //
    // That matters. Windows can derive no modes from this block, so it asks
    // EvtIddCxParseMonitorDescription instead, and the answer is the modeline
    // store -- the only thing that knows the porches. An EDID with timings in
    // it would put modes in front of Windows that the store has never heard of,
    // and EvtIddCxAdapterCommitModes would rightly refuse them, which is a
    // display that lists modes it cannot set.
    //
    // Checksum computed rather than hand-carried, because a wrong one is
    // silent.
    static BYTE edid[128] = {
        0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00, // header
        0x1E,0xA4,                               // "GUD" packed 5-bit
        0x4D,0x61,                               // product 0x614D
        0x00,0x00,0x00,0x00,                     // serial
        0x01,0x24,                               // week 1, year 2026
        0x01,0x04,                               // EDID 1.4
        0x80,0x00,0x00,0x78,0x0A,                // digital, size unknown, gamma
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // chromaticity
        0x00,0x00,0x00,                          // established timings: none
        0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
        0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, // standard timings: none
        // descriptor 1: monitor name
        0x00,0x00,0x00,0xFC,0x00,
        'G','U','D',' ','d','i','s','p','l','a','y',0x0A,0x20,
        // descriptors 2-4: unused
        0x00,0x00,0x00,0x10,0x00, 0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x00,0x00,0x00,0x10,0x00, 0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x00,0x00,0x00,0x10,0x00, 0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x00,                                    // no extensions
        0x00                                     // checksum, filled in below
    };
    {
        unsigned sum = 0;
        for (int i = 0; i < 127; i++)
            sum += edid[i];
        edid[127] = (BYTE)((256 - (sum & 0xff)) & 0xff);
    }

    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = sizeof(edid);
    info.MonitorDescription.pData = edid;

    // MonitorContainerId must be set. Zero-initialising IDDCX_MONITOR_INFO
    // leaves it GUID_NULL, and IddCxMonitorCreate rejects that with
    // STATUS_INVALID_PARAMETER without saying which field it objected to.
    //
    // A container ID groups everything that is physically the same piece of
    // equipment -- the header notes that a monitor with audio or touch in it
    // should report the same ID for all of them. It has to be stable across
    // reboots and driver upgrades, so this is a fixed constant rather than
    // CoCreateGuid(): a fresh GUID every boot would present the same CRT to
    // Windows as a different monitor each time.
    //
    // Fixed also means every GUD device on one machine shares it, which is
    // wrong the day someone attaches two. Deriving it from the device -- the
    // USB serial, or the connector index -- is the fix at that point.
    static const GUID kContainerId =
        { 0x9e8c1f3a, 0x5b47, 0x4c2e,
          { 0xa1, 0x6d, 0x3f, 0x02, 0xd8, 0x74, 0xb9, 0x51 } };
    info.MonitorContainerId = kContainerId;

    // The monitor gets its own context carrying the same back-pointer, so the
    // monitor callbacks can reach the device without guessing.
    WDF_OBJECT_ATTRIBUTES monAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttrs, MonitorContext);

    IDARG_IN_MONITORCREATE create{};
    create.pMonitorInfo     = &info;
    create.ObjectAttributes = &monAttrs;

    IDARG_OUT_MONITORCREATE created{};
    NTSTATUS status = IddCxMonitorCreate(adapter, &create, &created);
    GudLog("CreateAndArriveMonitor: IddCxMonitorCreate -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    MonitorGetContext(created.MonitorObject)->Device = ctx;
    ctx->Monitor = created.MonitorObject;

    // Arrival off the start path.
    //
    // EvtIddCxAdapterInitFinished runs inside EvtDeviceSelfManagedIoInit, which
    // is part of the PnP start sequence -- the device does not reach status=OK
    // until it returns. Arrival asks the OS to enumerate a child monitor
    // device, and doing that against a parent whose start IRP is still in
    // flight is not something to do inline. The IndirectDisplay sample never
    // meets this, a root-enumerated software device having no real start to be
    // in the middle of.
    //
    // A detached thread with a fixed sleep is not how this should be done -- a
    // WDF timer is -- and now that StartModeWatch has demonstrated a working
    // WDF timer in this driver, there is no longer an excuse for it.
    GudLog("CreateAndArriveMonitor: monitor created, deferring arrival");

    IDDCX_MONITOR mon = ctx->Monitor;
    std::thread([mon, ctx]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        IDARG_OUT_MONITORARRIVAL arrival{};
        NTSTATUS s = IddCxMonitorArrival(mon, &arrival);
        GudLog("DeferredArrival: IddCxMonitorArrival -> 0x%08X", (unsigned)s);
        if (NT_SUCCESS(s)) {
            GudLog("DeferredArrival: OsAdapterLuid=%08X:%08X OsTargetId=%u",
                   (unsigned)arrival.OsAdapterLuid.HighPart,
                   (unsigned)arrival.OsAdapterLuid.LowPart,
                   arrival.OsTargetId);
            // Only once there is a monitor. Before arrival there is nothing
            // for the watcher to update.
            StartModeWatch(ctx);
        }
    }).detach();

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS EvtIddCxAdapterInitFinished(
    IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args)
{
    // Log before touching anything. The previous version dereferenced the
    // adapter context on its first line, so a context that was not attached
    // would fault here and look exactly like the callback never being called
    // -- no log line either way. Distinguishing those two is the whole point.
    GudLog("AdapterInitFinished: CALLED. AdapterInitStatus=0x%08X",
           (unsigned)args->AdapterInitStatus);

    if (!NT_SUCCESS(args->AdapterInitStatus))
        return args->AdapterInitStatus;

    auto* actx = AdapterGetContext(adapter);
    if (!actx || !actx->Device) {
        // IddCxAdapterInitAsync is asynchronous, so this can in principle run
        // before D0Entry has written the back-pointer. Say so rather than
        // dereferencing null.
        GudLog("AdapterInitFinished: adapter context not populated yet");
        return STATUS_INVALID_DEVICE_STATE;
    }
    return CreateAndArriveMonitor(actx->Device, adapter);
}

// Parse a monitor description into modes.
//
// Required. IddCxDeviceInitConfig refuses a config without it -- it is the
// second field of IDD_CX_CLIENT_CONFIG and the reason the driver failed with
// STATUS_INVALID_PARAMETER, reported by UMDF as a level-0 load failure with no
// mention of which call was at fault.
//
// This driver deliberately supplies no EDID, so there is no description to
// parse and nothing to report: answer zero modes and succeed. IddCx then asks
// EvtIddCxMonitorGetDefaultDescriptionModes instead, which is where the
// modeline store actually answers. Failing here rather than returning zero
// would take the monitor down with it.
_Use_decl_annotations_
static NTSTATUS EvtIddCxParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* in,
    IDARG_OUT_PARSEMONITORDESCRIPTION* out)
{
    // Answer with the modeline store, not with nothing.
    //
    // Because a monitor description is supplied, this is the callback IddCx
    // asks for the monitor's modes -- EvtIddCxMonitorGetDefaultDescriptionModes
    // is for the case where there is no description to parse and is not reached
    // here. Returning zero modes leaves the monitor with no modes at all, and
    // IddCxMonitorArrival then fails with STATUS_INVALID_PARAMETER: a monitor
    // that cannot display anything cannot arrive.
    //
    // The EDID handed to IddCxMonitorCreate deliberately carries no timing, so
    // there is nothing in it to parse. The modes come from the store, which is
    // the device's advertised list plus the INI -- the only thing that knows
    // the porches. Parsing the description is the question IddCx is asking;
    // the store is the honest answer to it.
    //
    // Two-pass, as the argument pair implies: an input count of zero asks how
    // many there are, anything else is the buffer to fill.
    GudLog("ParseMonitorDescription: CALLED. inCount=%u store=%p", in->MonitorModeBufferInputCount, (void*)g_ParseStore);
    std::lock_guard<std::mutex> lk(g_StoreLock);

    auto* store = g_ParseStore;
    if (!store) {
        out->MonitorModeBufferOutputCount = 0;
        out->PreferredMonitorModeIdx      = 0;
        return STATUS_SUCCESS;
    }

    if (in->MonitorModeBufferInputCount == 0) {
        out->MonitorModeBufferOutputCount = store->n;
        return STATUS_SUCCESS;
    }

    for (unsigned i = 0; i < store->n; i++) {
        const auto& m = store->e[i].mode;
        in->pMonitorModes[i] = MakeMonitorMode(m,
                                               IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
        GudLog("  mode[%u] %ux%u%s clk=%u total %ux%u vsync=%u/1000",
               i, m.hdisplay, m.vdisplay,
               (m.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p",
               m.clock, m.htotal, m.vtotal, gud_mode_refresh_mhz(&m));
    }

    out->MonitorModeBufferOutputCount = store->n;
    out->PreferredMonitorModeIdx      = 0;
    for (unsigned i = 0; i < store->n; i++)
        if (store->e[i].mode.flags & GUD_DISPLAY_MODE_FLAG_PREFERRED)
            out->PreferredMonitorModeIdx = i;

    return STATUS_SUCCESS;
}

// Windows asks what modes the monitor supports. Answer from the store, which
// is the device list plus whatever has been written into the INI.
_Use_decl_annotations_
static NTSTATUS EvtIddCxMonitorGetDefaultModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out)
{
    auto* ctx = MonitorGetContext(monitor)->Device;
    std::lock_guard<std::mutex> lk(g_StoreLock);
    GudLog("GetDefaultDescriptionModes: %u modes", ctx->Modes.n);

    if (in->DefaultMonitorModeBufferInputCount == 0) {
        out->DefaultMonitorModeBufferOutputCount = ctx->Modes.n;
        return STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < ctx->Modes.n; i++)
        in->pDefaultMonitorModes[i] = MakeMonitorMode(ctx->Modes.e[i].mode,
                                                      IDDCX_MONITOR_MODE_ORIGIN_DRIVER);

    out->DefaultMonitorModeBufferOutputCount = ctx->Modes.n;
    out->PreferredMonitorModeIdx = 0;
    for (unsigned i = 0; i < ctx->Modes.n; i++)
        if (ctx->Modes.e[i].mode.flags & GUD_DISPLAY_MODE_FLAG_PREFERRED)
            out->PreferredMonitorModeIdx = i;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS EvtIddCxMonitorQueryModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES* in,
    IDARG_OUT_QUERYTARGETMODES* out)
{
    auto* ctx = MonitorGetContext(monitor)->Device;
    std::lock_guard<std::mutex> lk(g_StoreLock);
    GudLog("QueryTargetModes: %u modes", ctx->Modes.n);

    if (in->TargetModeBufferInputCount == 0) {
        out->TargetModeBufferOutputCount = ctx->Modes.n;
        return STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < ctx->Modes.n; i++)
        in->pTargetModes[i] = MakeTargetMode(ctx->Modes.e[i].mode);

    out->TargetModeBufferOutputCount = ctx->Modes.n;
    return STATUS_SUCCESS;
}

// The modeset. This is where the porches come back.
_Use_decl_annotations_
static NTSTATUS EvtIddCxAdapterCommitModes(
    IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* in)
{
    auto* ctx = AdapterGetContext(adapter)->Device;
    GudLog("CommitModes: entered, %u paths", in->PathCount);

    for (UINT i = 0; i < in->PathCount; i++) {
        const IDDCX_PATH& path = in->pPaths[i];

        // Activity is a flag on the path, not a bool member.
        if (!(path.Flags & IDDCX_PATH_FLAGS_ACTIVE)) {
            ctx->Processor.reset();
            gud_set_controller_enable(&ctx->Gud, 0);
            continue;
        }

        const auto& sig = SignalInfo(path);
        uint32_t w = sig.activeSize.cx;
        uint32_t h = sig.activeSize.cy;
        GudLog("  path active %ux%u pixelRate=%llu total %ux%u slo=%u",
               (unsigned)w, (unsigned)h,
               (unsigned long long)sig.pixelRate,
               (unsigned)sig.totalSize.cx, (unsigned)sig.totalSize.cy,
               (unsigned)sig.scanLineOrdering);

        // Exact first, with interlace as don't-care.
        //
        // pixelRate and totalSize came from MakeTargetMode(), so four of the
        // modeline's five identifying numbers come straight back and the match
        // is exact rather than approximate. Only the sync starts and ends are
        // missing, and recovering those is the whole reason the store exists.
        //
        // The fifth number, interlace, does not come back. MakeTargetMode sets
        // scanLineOrdering to INTERLACED for a 480i mode and the commit path
        // reports PROGRESSIVE for that same mode -- the desktop IddCx
        // composites is progressive, so the field order is normalised away
        // somewhere above this driver. Keying on it means never matching an
        // interlaced modeline, which refuses the commit, which restarts the
        // driver, which is the loop this replaced.
        //
        // Dropping it costs nothing: two modelines agreeing on clock, both
        // totals and both actives, and differing only in interlace, are not
        // distinguishable from anything Windows hands us here either.
        // Copied out under the lock, not pointed at: the mode-watch timer can
        // rewrite the store from another thread, and everything below this
        // wants a modeline that will not move.
        gud_display_mode_req chosen{};
        char chosen_name[32] = { 0 };

        std::unique_lock<std::mutex> lk(g_StoreLock);

        const modeline_entry* e = modeline_store_find_exact(
            &ctx->Modes,
            (uint32_t)(sig.pixelRate / 1000),
            sig.totalSize.cx, sig.totalSize.cy,
            w, h, -1);

        if (!e) {
            // Fallback, for a path that lost the totals somewhere. Tight
            // window on purpose: 59.94 and 60.00 are 60 mHz apart and are
            // different modelines on a CRT, so there is no tolerance that both
            // survives arbitrary rounding and keeps them distinct. Better to
            // miss than to return the wrong one.
            uint32_t refresh_mhz = 0;
            if (sig.vSyncFreq.Denominator)
                refresh_mhz = (uint32_t)(((UINT64)sig.vSyncFreq.Numerator * 1000)
                                         / sig.vSyncFreq.Denominator);
            e = modeline_store_find(&ctx->Modes, w, h, refresh_mhz, -1);
        }

        if (!e) {
            // Refuse rather than guess.
            //
            // The temptation is to synthesise something plausible from
            // width, height and refresh -- a GTF or CVT timing, say. Do not.
            // CVT is designed for multisync panels and produces line rates
            // well outside what a fixed-frequency deflection circuit will
            // take. The device's own mode_check would reject it, which is the
            // safety net working, but a driver whose normal path depends on
            // the far end refusing it is a driver waiting for a device with a
            // looser check.
            GudLog("  no modeline for this path -- refusing. store has %u:",
                   ctx->Modes.n);
            for (unsigned k = 0; k < ctx->Modes.n; k++) {
                const auto& me = ctx->Modes.e[k].mode;
                GudLog("    [%u] %ux%u%s clk=%u total %ux%u", k,
                       (unsigned)me.hdisplay, (unsigned)me.vdisplay,
                       (me.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p",
                       (unsigned)me.clock,
                       (unsigned)me.htotal, (unsigned)me.vtotal);
            }
            return STATUS_INVALID_PARAMETER;
        }

        chosen = e->mode;
        strncpy_s(chosen_name, e->name, _TRUNCATE);
        lk.unlock();

        GudLog("  modeline %s: %ux%u%s clk=%u h %u/%u/%u/%u v %u/%u/%u/%u",
               chosen_name, (unsigned)chosen.hdisplay, (unsigned)chosen.vdisplay,
               (chosen.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p",
               (unsigned)chosen.clock,
               (unsigned)chosen.hdisplay, (unsigned)chosen.hsync_start,
               (unsigned)chosen.hsync_end, (unsigned)chosen.htotal,
               (unsigned)chosen.vdisplay, (unsigned)chosen.vsync_start,
               (unsigned)chosen.vsync_end, (unsigned)chosen.vtotal);

        int err = gud_set_state(&ctx->Gud, &chosen, ctx->Format);
        GudLog("  gud_set_state -> %d", err);
        if (err)
            return STATUS_INVALID_PARAMETER;

        gud_set_controller_enable(&ctx->Gud, 1);
        gud_set_display_enable(&ctx->Gud, 1);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS EvtIddCxMonitorAssignSwapChain(
    IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in)
{
    auto* ctx = MonitorGetContext(monitor)->Device;

    ctx->Processor.reset();
    if (!ctx->Gud.active_valid) {
        GudLog("AssignSwapChain: no active mode -- refusing");
        return STATUS_INVALID_DEVICE_STATE;
    }
    GudLog("AssignSwapChain: streaming %ux%u",
           (unsigned)ctx->Gud.active.hdisplay, (unsigned)ctx->Gud.active.vdisplay);

    ctx->Processor = std::make_unique<SwapChainProcessor>(
        in->hSwapChain, in->RenderAdapterLuid, in->hNextSurfaceAvailable,
        &ctx->Gud, ctx->Gud.active);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS EvtIddCxMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    MonitorGetContext(monitor)->Device->Processor.reset();
    return STATUS_SUCCESS;
}

// ===========================================================================
// WDF plumbing
// ===========================================================================

// Create the USB target here, not in EvtDriverDeviceAdd.
//
// WdfUsbTargetDeviceCreateWithParameters needs the device started and the USB
// stack present beneath it. Called from EvtDriverDeviceAdd -- where this driver
// called it -- there is nothing underneath yet and it returns
// STATUS_INVALID_PARAMETER, which reads like a bad argument and is really "too
// early". EvtDevicePrepareHardware is the first callback where the hardware is
// there, and it runs before EvtDeviceD0Entry, so gud_probe() still has a
// working transport by the time it runs.
_Use_decl_annotations_
static NTSTATUS EvtDevicePrepareHardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST)
{
    auto* ctx = DeviceGetContext(device);

    GudLog("EvtDevicePrepareHardware: entered");
    NTSTATUS status = ctx->Transport.Init(device);
    GudLog("EvtDevicePrepareHardware: Transport.Init -> 0x%08X", (unsigned)status);

    // A failure here is fatal on the real device and expected on the
    // root-enumerated diagnostic one, where there is no USB beneath us at all.
    // Carry on without a transport in that case: the IddCx path is what is
    // being tested and it does not need pixels to reach anything.
    if (!NT_SUCCESS(status)) {
        GudLog("EvtDevicePrepareHardware: no USB -- continuing without a transport");
        ctx->NoUsb = true;
        return STATUS_SUCCESS;
    }
    return status;
}

// Adapter creation runs here, not in EvtDeviceD0Entry.
//
// "The IDDCX_ADAPTER should only be created once all the PnP devices that make
// up the indirect display solution are successfully started" -- and D0Entry
// runs *during* the start transition, not after it. EvtDeviceSelfManagedIoInit
// is the WDF callback that means the device has started and is ready for I/O,
// which is the state that sentence is describing.
//
// The IndirectDisplay sample initialises from D0Entry and works, but it is
// root-enumerated: a software device with no hardware behind it has no real
// start sequence to be in the middle of. A USB PDO does. The symptom here was
// IddCxAdapterInitAsync returning STATUS_SUCCESS and EvtIddCxAdapterInitFinished
// never being called -- IddCx accepting the adapter and never completing the
// second half of the two-stage creation.
_Use_decl_annotations_
static NTSTATUS EvtDeviceSelfManagedIoInit(WDFDEVICE device)
{
    auto* ctx = DeviceGetContext(device);
    GudLog("SelfManagedIoInit: entered");

    if (ctx->Adapter) {
        GudLog("SelfManagedIoInit: adapter already exists, nothing to do");
        return STATUS_SUCCESS;
    }

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device;

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    // Left at zero, as the IndirectDisplay sample leaves it.
    //
    // This was set to max_width * max_height * 60 on the grounds that it was
    // free to state truthfully. It is not free. The header says the OS "will
    // ensure that the combined display pipeline rate of all the active modes
    // will never exceed this value", and there is no way for the driver to
    // declare what unit it meant -- so a number that means pixels per second
    // here may be compared against something the OS reckons in bytes per
    // second. At 32 bits per pixel a 640x480p60 mode is 73 Mbyte/s against a
    // cap of 49 M, and then no mode fits and no monitor can arrive.
    caps.MaxDisplayPipelineRate = 0;

    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType =
        IDDCX_TRANSMISSION_TYPE_WIRED_USB;
    caps.EndPointDiagnostics.pEndPointFriendlyName     = L"GUD display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Generic USB Display";
    caps.EndPointDiagnostics.pEndPointModelName        = L"blitsCRT";

    static IDDCX_ENDPOINT_VERSION version{};
    version.Size     = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    init.pCaps = &caps;

    WDF_OBJECT_ATTRIBUTES adapterAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttrs, AdapterContext);
    init.ObjectAttributes = &adapterAttrs;

    IDARG_OUT_ADAPTER_INIT out{};
    GudLog("SelfManagedIoInit: IddCxAdapterInitAsync...");
    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
    GudLog("SelfManagedIoInit: IddCxAdapterInitAsync -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    AdapterGetContext(out.AdapterObject)->Device = ctx;
    ctx->Adapter = out.AdapterObject;
    GudLog("SelfManagedIoInit: adapter context wired, waiting for InitFinished");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    auto* ctx = DeviceGetContext(device);

    gud_transport t{};
    ctx->Transport.Fill(&t);

    if (ctx->NoUsb) {
        // Root-enumerated diagnostic: no device to probe. One hardcoded mode,
        // which is what BRINGUP step 4's minimal driver was to report.
        GudLog("D0Entry: no USB, using one hardcoded 640x480p60");
        modeline_store_reset(&ctx->Modes);
        gud_display_mode_req m{};
        m.clock = 25175;
        m.hdisplay = 640; m.hsync_start = 656; m.hsync_end = 752; m.htotal = 800;
        m.vdisplay = 480; m.vsync_start = 490; m.vsync_end = 492; m.vtotal = 525;
        m.flags = GUD_DISPLAY_MODE_FLAG_NHSYNC | GUD_DISPLAY_MODE_FLAG_NVSYNC |
                  GUD_DISPLAY_MODE_FLAG_PREFERRED;
        modeline_store_add(&ctx->Modes, &m, "diag", 1);
        g_ParseStore = &ctx->Modes;
        GudLog("D0Entry: store has %u modes", ctx->Modes.n);
        return STATUS_SUCCESS;
    }

    GudLog("D0Entry: probing device over WDF-USB");
    if (int err = gud_probe(&ctx->Gud, &t)) {
        // A bad magic here is the useful diagnostic: it means something
        // enumerated with the right VID:PID that is not a GUD device, which on
        // a MiSTer board is most likely the USB hub add-on still fitted.
        return (err == GUD_E_NODEV) ? STATUS_DEVICE_PROTOCOL_ERROR
                                    : STATUS_DEVICE_DATA_ERROR;
    }

    if (!gud_has_format(&ctx->Gud, ctx->Format)) {
        if (gud_has_format(&ctx->Gud, GUD_PIXEL_FORMAT_RGB332))
            ctx->Format = GUD_PIXEL_FORMAT_RGB332;
        else if (ctx->Gud.n_formats)
            ctx->Format = ctx->Gud.formats[0];
        else
            return STATUS_DEVICE_DATA_ERROR;
    }

    GudLog("D0Entry: gud_probe ok. magic=0x%08X modes=%u formats=%u fmt=0x%02x",
           ctx->Gud.desc.magic, ctx->Gud.n_modes, ctx->Gud.n_formats, ctx->Format);

    NTSTATUS status = ReloadModes(ctx);
    GudLog("D0Entry: ReloadModes -> 0x%08X, store has %u modes",
           (unsigned)status, ctx->Modes.n);
    if (!NT_SUCCESS(status))
        return status;

    // Adapter creation is NOT here -- it moved to EvtDeviceSelfManagedIoInit,
    // which runs once the device has actually started. D0Entry only brings the
    // device up and reads its mode list.
    GudLog("D0Entry: done, adapter deferred to SelfManagedIoInit");
    return STATUS_SUCCESS;
}

// Runs when the WDFDEVICE is being torn down. Destroys what the placement-new
// in EvtDriverDeviceAdd constructed, which is what joins the frame thread.
static void EvtDeviceContextCleanup(WDFOBJECT object)
{
    auto* ctx = DeviceGetContext(static_cast<WDFDEVICE>(object));
    if (ctx)
        ctx->~DeviceContext();
}

_Use_decl_annotations_
static NTSTATUS EvtDriverDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit)
{
    // IddCxDeviceInitConfig must run before WdfDeviceCreate. This is the call
    // that turns an ordinary UMDF device into an indirect display, and the
    // thing to establish first on real hardware is that it is willing to do
    // that to a PDO it did not create -- see the note at the top of Driver.h.
    GudLog("EvtDriverDeviceAdd: entered");

    IDD_CX_CLIENT_CONFIG config{};
    IDD_CX_CLIENT_CONFIG_INIT(&config);

    config.EvtIddCxParseMonitorDescription      = EvtIddCxParseMonitorDescription;
    config.EvtIddCxAdapterInitFinished          = EvtIddCxAdapterInitFinished;
    config.EvtIddCxAdapterCommitModes           = EvtIddCxAdapterCommitModes;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = EvtIddCxMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes      = EvtIddCxMonitorQueryModes;
    config.EvtIddCxMonitorAssignSwapChain       = EvtIddCxMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain     = EvtIddCxMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
    GudLog("EvtDriverDeviceAdd: IddCxDeviceInitConfig -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware   = EvtDevicePrepareHardware;
    power.EvtDeviceD0Entry           = EvtDeviceD0Entry;
    power.EvtDeviceSelfManagedIoInit = EvtDeviceSelfManagedIoInit;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DeviceContext);
    // The context is placement-newed below, so something has to destroy it.
    // Without this ~DeviceContext never runs, so ~SwapChainProcessor never
    // runs, so the frame thread is never woken or joined -- a wedged thread in
    // Session 0 on every unplug, which is the failure BRINGUP warns about.
    attrs.EvtCleanupCallback = EvtDeviceContextCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attrs, &device);
    GudLog("EvtDriverDeviceAdd: WdfDeviceCreate -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    status = IddCxDeviceInitialize(device);
    GudLog("EvtDriverDeviceAdd: IddCxDeviceInitialize -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    auto* ctx = new (DeviceGetContext(device)) DeviceContext();
    ctx->Device = device;

    // Transport.Init has moved to EvtDevicePrepareHardware -- see there.
    GudLog("EvtDriverDeviceAdd: ok, USB deferred to PrepareHardware");
    return STATUS_SUCCESS;
}

extern "C" _Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    GudLog("DriverEntry: entered.  driverObject=%p registryPath=%p",
           (void*)driverObject, (void*)registryPath);

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);

    // No DriverPoolTag. The field is present in the UMDF headers so it
    // compiles, but a pool tag is a kernel allocation concept and this driver
    // runs in WUDFHost against the process heap.

    GudLog("DriverEntry: config.Size=%u DriverInitFlags=0x%x",
           config.Size, config.DriverInitFlags);

    NTSTATUS status = WdfDriverCreate(driverObject, registryPath,
                                      WDF_NO_OBJECT_ATTRIBUTES,
                                      &config, WDF_NO_HANDLE);

    GudLog("DriverEntry: WdfDriverCreate -> 0x%08X %s",
           (unsigned)status, NT_SUCCESS(status) ? "(ok)" : "(FAILED)");

    return status;
}
