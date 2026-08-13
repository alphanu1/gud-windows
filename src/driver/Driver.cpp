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

    // Interlace: report it, because the sink is a 15 kHz CRT and 480i is the
    // whole point. Note the asymmetry with GUD, which carries rects in the
    // mode's coordinate space with vdisplay = 480 for 480i and lets the device
    // interleave on read. So the surface Windows hands over is the full
    // progressive one either way, and interlace changes nothing above this
    // line -- only what the raster does with it.
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

IDDCX_MONITOR_MODE MakeMonitorMode(const gud_display_mode_req& m)
{
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    mode.MonitorVideoSignalInfo =
        MakeTargetMode(m).TargetVideoSignalInfo.targetVideoSignalInfo;
    return mode;
}

} // namespace

NTSTATUS gudwin::ReloadModes(DeviceContext* ctx)
{
    modeline_store_reset(&ctx->Modes);

    // Device list first, INI second, so a hand-written modeline overrides an
    // advertised one with the same geometry and rate. Someone who wrote out a
    // modeline meant it.
    if (gud_read_modes(&ctx->Gud) != 0)
        return STATUS_DEVICE_DATA_ERROR;
    modeline_store_load_device(&ctx->Modes, &ctx->Gud);
    modeline_store_load_ini(&ctx->Modes, kIniPath);

    if (ctx->Modes.n == 0)
        return STATUS_DEVICE_DATA_ERROR;

    // IddCx reads the mode list once per monitor arrival. Growing it later
    // means taking the monitor away and bringing it back, which is the same
    // constraint GUD has on the device side: GET_CONNECTOR_MODES is asked once
    // during enumeration. Both ends re-probe rather than grow.
    if (ctx->Monitor) {
        IddCxMonitorDeparture(ctx->Monitor);
        ctx->Monitor = nullptr;
    }
    return STATUS_SUCCESS;
}

// ===========================================================================
// IddCx callbacks
// ===========================================================================

_Use_decl_annotations_
static NTSTATUS EvtIddCxAdapterInitFinished(
    IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args)
{
    auto* ctx = AdapterGetContext(adapter)->Device;
    if (!NT_SUCCESS(args->AdapterInitStatus))
        return args->AdapterInitStatus;

    // Create the monitor. Connector index 0; see gud_probe().
    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15;   // VGA
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
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = 0;
    info.MonitorDescription.pData = nullptr;

    // The monitor gets its own context carrying the same back-pointer, so the
    // monitor callbacks can reach the device without guessing.
    WDF_OBJECT_ATTRIBUTES monAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttrs, MonitorContext);

    IDARG_IN_MONITORCREATE create{};
    create.pMonitorInfo     = &info;
    create.ObjectAttributes = &monAttrs;

    IDARG_OUT_MONITORCREATE created{};
    NTSTATUS status = IddCxMonitorCreate(adapter, &create, &created);
    if (!NT_SUCCESS(status))
        return status;

    MonitorGetContext(created.MonitorObject)->Device = ctx;
    ctx->Monitor = created.MonitorObject;

    GudLog("AdapterInitFinished: monitor created, calling arrival");
    IDARG_OUT_MONITORARRIVAL arrival{};
    return IddCxMonitorArrival(ctx->Monitor, &arrival);
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
    UNREFERENCED_PARAMETER(in);

    out->MonitorModeBufferOutputCount = 0;
    out->PreferredMonitorModeIdx      = 0;
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

    if (in->DefaultMonitorModeBufferInputCount == 0) {
        out->DefaultMonitorModeBufferOutputCount = ctx->Modes.n;
        return STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < ctx->Modes.n; i++)
        in->pDefaultMonitorModes[i] = MakeMonitorMode(ctx->Modes.e[i].mode);

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
        int interlaced = (sig.scanLineOrdering ==
                          DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED) ? 1 : 0;

        // Exact first.
        //
        // pixelRate and totalSize came from MakeTargetMode(), so four of the
        // modeline's five identifying numbers come straight back and the match
        // is exact rather than approximate. Only the sync starts and ends are
        // missing, and recovering those is the whole reason the store exists.
        const modeline_entry* e = modeline_store_find_exact(
            &ctx->Modes,
            (uint32_t)(sig.pixelRate / 1000),
            sig.totalSize.cx, sig.totalSize.cy,
            w, h, interlaced);

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
            e = modeline_store_find(&ctx->Modes, w, h, refresh_mhz, interlaced);
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
            return STATUS_INVALID_PARAMETER;
        }

        int err = gud_set_state(&ctx->Gud, &e->mode, ctx->Format);
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
    if (!ctx->Gud.active_valid)
        return STATUS_INVALID_DEVICE_STATE;

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
    return status;
}

_Use_decl_annotations_
static NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    auto* ctx = DeviceGetContext(device);

    gud_transport t{};
    ctx->Transport.Fill(&t);

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

    // Guard against a second adapter on resume. EvtDeviceD0Entry fires on
    // every power transition, and IddCxAdapterInitAsync is not idempotent.
    // The IndirectDisplay sample initialises here too, so the placement is
    // kept and the re-entry is refused instead.
    if (ctx->Adapter)
        return STATUS_SUCCESS;

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device;
    init.pCaps = nullptr;   // see below

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;

    // Not required -- the IndirectDisplay sample leaves this zero and works --
    // but it is free to state truthfully, and it tracks the hardware rather
    // than a constant: the descriptor's maximum raster at 60 Hz.
    caps.MaxDisplayPipelineRate =
        (UINT64)ctx->Gud.desc.max_width * ctx->Gud.desc.max_height * 60;

    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;

    // WIRED_USB, not OTHER. It is what this is, and the enumeration has a value
    // for it -- OTHER is 0xFFFFFFFF and exists for links that do not fit.
    caps.EndPointDiagnostics.TransmissionType =
        IDDCX_TRANSMISSION_TYPE_WIRED_USB;

    caps.EndPointDiagnostics.pEndPointFriendlyName     = L"GUD display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Generic USB Display";
    caps.EndPointDiagnostics.pEndPointModelName        = L"blitsCRT";

    // pFirmwareVersion and pHardwareVersion are not optional. Left null,
    // IddCxAdapterInitAsync refuses the caps with STATUS_INVALID_PARAMETER and
    // says nothing about which field it objected to. The IndirectDisplay sample
    // points both at one IDDCX_ENDPOINT_VERSION with only Size and MajorVer
    // filled in, which is what this does -- GUD carries no firmware or hardware
    // revision, so there is nothing truthful to put in the other fields.
    //
    // Must outlive the call. IddCxAdapterInitAsync is asynchronous by name and
    // reads the caps before returning, but a local here would be a stack
    // address handed to the framework either way.
    static IDDCX_ENDPOINT_VERSION version{};
    version.Size     = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    init.pCaps = &caps;

    // The adapter carries a back-pointer to us, so EvtIddCxAdapterInitFinished
    // and EvtIddCxAdapterCommitModes can find the device context.
    WDF_OBJECT_ATTRIBUTES adapterAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttrs, AdapterContext);
    init.ObjectAttributes = &adapterAttrs;

    IDARG_OUT_ADAPTER_INIT out{};
    GudLog("D0Entry: IddCxAdapterInitAsync...");
    status = IddCxAdapterInitAsync(&init, &out);
    GudLog("D0Entry: IddCxAdapterInitAsync -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    AdapterGetContext(out.AdapterObject)->Device = ctx;
    ctx->Adapter = out.AdapterObject;
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
    power.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    power.EvtDeviceD0Entry         = EvtDeviceD0Entry;
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
