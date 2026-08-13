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

    // Publish it for EvtIddCxParseMonitorDescription, which has no handle to
    // reach it by. Set after loading so the callback never sees a half-filled
    // store, and set on every reload so it tracks the INI.
    g_ParseStore = &ctx->Modes;

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
    // Log before touching anything. The previous version dereferenced the
    // adapter context on its first line, so a context that was not attached
    // would fault here and look exactly like the callback never being called
    // -- no log line either way. Distinguishing those two is the whole point.
    GudLog("AdapterInitFinished: CALLED. AdapterInitStatus=0x%08X",
           (unsigned)args->AdapterInitStatus);

    if (!NT_SUCCESS(args->AdapterInitStatus))
        return args->AdapterInitStatus;

    auto* actx = AdapterGetContext(adapter);
    GudLog("AdapterInitFinished: AdapterGetContext -> %p", (void*)actx);
    if (!actx || !actx->Device) {
        // IddCxAdapterInitAsync is asynchronous, so this can in principle run
        // before D0Entry has written the back-pointer. Say so rather than
        // dereferencing null.
        GudLog("AdapterInitFinished: adapter context not populated yet");
        return STATUS_INVALID_DEVICE_STATE;
    }
    auto* ctx = actx->Device;

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
    // DIAGNOSTIC: the IndirectDisplay sample's own EDID, byte for byte.
    //
    // The hand-built block this replaces was rejected somewhere -- most likely
    // for having no detailed timing descriptor at all, which EDID 1.4 requires
    // in the first descriptor slot. Rather than debug a hand-rolled EDID while
    // also debugging the driver, borrow one that is known to work and find out
    // whether the EDID is the remaining problem at all.
    //
    // This describes a Dell S2719DGF and is a lie about the hardware, so it
    // cannot stay: the modes it advertises are nothing like a 15 kHz CRT's, and
    // DESIGN.md's reasoning about not offering Windows timings the store has
    // never heard of applies with full force. It is here to answer one question.
    static BYTE edid[128] = {
        0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,
        0x24,0x1D,0x01,0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,
        0x0B,0x50,0x54,0x00,0x02,0x00,0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
        0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,
        0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,0x00,0x37,0x4A,0x51,
        0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x53,
        0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,
        0x00,0x28,0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
    };

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
    GudLog("AdapterInitFinished: IddCxMonitorCreate -> 0x%08X", (unsigned)status);
    if (!NT_SUCCESS(status))
        return status;

    MonitorGetContext(created.MonitorObject)->Device = ctx;
    ctx->Monitor = created.MonitorObject;

    GudLog("AdapterInitFinished: monitor created, calling arrival");
    IDARG_OUT_MONITORARRIVAL arrival{};
    NTSTATUS astatus = IddCxMonitorArrival(ctx->Monitor, &arrival);
    GudLog("AdapterInitFinished: IddCxMonitorArrival -> 0x%08X", (unsigned)astatus);
    return astatus;
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
    auto* store = g_ParseStore;
    if (!store) {
        out->MonitorModeBufferOutputCount = 0;
        out->PreferredMonitorModeIdx      = 0;
        return STATUS_SUCCESS;
    }

    // ---- DIAGNOSTIC, TEMPORARY ----
    // Report one plain 640x480p60 instead of the store, to answer a single
    // question: does IddCxMonitorArrival fail because of *these* modes?
    //
    // Both store entries are things Windows may refuse as monitor modes --
    // 648x480 is interlaced, and 632x240 is below the 640x480 minimum Windows
    // has historically enforced. If a monitor has no acceptable mode it cannot
    // arrive, which is exactly the symptom. A mode Windows cannot object to
    // separates "our modes are rejected" from "something else is wrong", and
    // that distinction decides whether this is a bug or a design problem.
    //
    // Remove once answered.
    static const bool kDiagSafeMode = true;
    static const gud_display_mode_req kSafe = {
        25175,                      // clock kHz -- the standard 640x480p60
        640, 656, 752, 800,         // h: display, sync start, sync end, total
        480, 490, 492, 525,         // v
        GUD_DISPLAY_MODE_FLAG_NHSYNC | GUD_DISPLAY_MODE_FLAG_NVSYNC
    };

    if (kDiagSafeMode) {
        if (in->MonitorModeBufferInputCount == 0) {
            out->MonitorModeBufferOutputCount = 1;
            return STATUS_SUCCESS;
        }
        in->pMonitorModes[0] =
            MakeMonitorMode(kSafe, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
        out->MonitorModeBufferOutputCount = 1;
        out->PreferredMonitorModeIdx      = 0;
        GudLog("  DIAG: reporting one 640x480p60 instead of the store");
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
    GudLog("  monitor mode callback: entered");

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
    GudLog("  monitor mode callback: entered");

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
    GudLog("  monitor mode callback: entered");

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
