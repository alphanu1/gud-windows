// SPDX-License-Identifier: MIT
//
// Driver.h -- the IddCx side.
//
// Shape of the thing, because it is not the shape the Microsoft sample has.
//
// The IddCx sample enumerates a root device: a software monitor with no
// hardware behind it, installed against a synthetic hardware ID. That is the
// right shape for a virtual display and the wrong one here, because the pixels
// have to reach a USB endpoint and a root-enumerated driver has no path to it
// short of a second driver and an IPC channel between them -- which would mean
// copying every frame across a process boundary in Session 0.
//
// So this driver binds directly to the USB device. It is a UMDF2 driver on
// USB\VID_1D50&PID_614D with `UmdfDispatcher = WinUsb` in the INF, which puts
// winusb.sys underneath and gives the driver a WDFUSBDEVICE, and it calls
// IddCxDeviceInitConfig on top of that. One package, one process, and the
// frame never leaves it.
//
// The first thing to establish on a real machine is that IddCx accepts a
// device node it did not create itself. Microsoft documents this exact case --
// a USB dongle with a monitor attached -- so it should, but "should" is not
// "does" and it is the one assumption the whole structure rests on. Prove it
// with a driver that does nothing but enumerate a monitor before wiring any
// pixels to it. See docs/BRINGUP.md, step 4.

#pragma once

// Before windows.h. Without it windows.h defines min and max as macros, and
// the std::min/std::max in SwapChain.cpp expand to std::( -- "illegal token on
// right side of ::". mingw's headers do not define them, so the cross-build
// passes and only MSVC sees it.
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>

// USB core types before wdfusb.h, which uses USBD_STATUS, PURB and the
// USB_REQUEST_* constants without including anything that defines them:
//   usbspec.h  descriptors and the standard request codes
//   usb.h      USBD_STATUS and the URB structures
#include <usbspec.h>
#include <usb.h>

#include <wdf.h>
// wdfusb.h is a separate header in the real WDK -- wdf.h does not pull it in,
// and without it every WdfUsbTargetDevice*/WDF_USB_* name is undeclared. The
// stub in tests/wdkstub merged the two, so this only shows up against the real
// thing.
#include <wdfusb.h>
#include <wudfwdm.h>
#include <IddCx.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <memory>
#include <thread>
#include <atomic>
#include <vector>

extern "C" {
#include "gud_host.h"
#include "modeline.h"
#include "lz4enc.h"
#include "convert.h"
}

namespace gudwin {

// ---------------------------------------------------------------------------
// Transport over WDFUSBDEVICE. Same three operations the probe tool implements
// over WinUSB, issuing byte-identical transfers.
// ---------------------------------------------------------------------------

class UsbTransport {
public:
    NTSTATUS Init(WDFDEVICE device);
    void Fill(gud_transport* t);

    WDFUSBDEVICE     UsbDevice = nullptr;
    WDFUSBINTERFACE  Interface = nullptr;
    WDFUSBPIPE       BulkOut   = nullptr;
    UCHAR            IfNum     = 0;

private:
    static int CtrlIn (void* ctx, uint8_t r, uint16_t v, void* buf, size_t len);
    static int CtrlOut(void* ctx, uint8_t r, uint16_t v, const void* buf, size_t len);
    static int BulkOutFn(void* ctx, const void* buf, size_t len);
    int Ctrl(bool in, uint8_t r, uint16_t v, void* buf, size_t len);
};

// ---------------------------------------------------------------------------
// The frame loop. One per assigned swapchain, on its own thread.
// ---------------------------------------------------------------------------

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter,
                       HANDLE newFrameEvent, gud_device* dev,
                       const gud_display_mode_req& mode);
    ~SwapChainProcessor();

private:
    void Run();
    void RunCore();

    // Copy one dirty rect out of the acquired surface, convert, compress,
    // and put it on the wire.
    bool SendRect(ID3D11Texture2D* staging, const RECT& r);

    // Cost model for deciding between a list of rects and one full surface.
    // Every rect carries a 25-byte SET_BUFFER control transfer and a status
    // read behind it, which at 60 Hz is not free: a control transfer costs a
    // whole microframe whatever it carries. Past a few dozen small rects the
    // per-rect overhead exceeds what the smaller payload saves, and one full
    // surface -- which compresses well precisely because most of it did not
    // change -- is cheaper. 32 is a starting number, not a measured one.
    static constexpr size_t kMaxRects = 32;

    // Send the whole surface every frame and skip damage entirely.
    //
    // For bring-up. A full compressed 648x480 surface measured 1.6 ms on this
    // hardware against a 16.7 ms field, so this costs about a tenth of the
    // budget and nothing else -- cheap enough to prove the driver with, and it
    // takes every rect-arithmetic fault off the table while the IddCx side is
    // still unproven. The damage path is written and sits behind this.
    static constexpr bool kFullFrameOnly = true;

    IDDCX_SWAPCHAIN m_swapChain;
    LUID            m_renderAdapter;
    HANDLE          m_newFrameEvent;
    gud_device*     m_dev;
    gud_display_mode_req m_mode;

    Microsoft::WRL::ComPtr<ID3D11Device>        m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>     m_staging;

    std::vector<uint8_t> m_conv;      // wire format, tightly packed
    std::vector<uint8_t> m_comp;      // LZ4 output
    std::unique_ptr<lz4enc_ctx> m_lz; // 256 KB, heap not stack

    // Damage, fetched per frame into arrays this side owns. Kept as members
    // and resized rather than allocated per frame: this runs at the field
    // rate and a per-frame allocation is the jitter the staging texture is
    // already sized once to avoid.
    std::vector<RECT>             m_dirty;
    std::vector<IDDCX_MOVEREGION> m_moves;

    std::thread       m_thread;
    std::atomic<bool> m_terminate{ false };
};

// ---------------------------------------------------------------------------
// Device context
// ---------------------------------------------------------------------------

struct DeviceContext {
    WDFDEVICE       Device        = nullptr;
    IDDCX_ADAPTER   Adapter       = nullptr;
    IDDCX_MONITOR   Monitor       = nullptr;

    UsbTransport    Transport;
    gud_device      Gud{};
    modeline_store  Modes{};

    // Formats we will negotiate, best first. RGB565 before RGB332 because the
    // depth is worth more than the bandwidth at every mode this device runs;
    // RGB332 exists for anything that turns out not to fit.
    uint8_t         Format = GUD_PIXEL_FORMAT_RGB565;

    // Set when there is no USB device beneath this one, which happens only on
    // the root-enumerated diagnostic INF. The IddCx path then runs against a
    // hardcoded mode and sends nothing.
    bool            NoUsb = false;

    std::unique_ptr<SwapChainProcessor> Processor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, DeviceGetContext);

// ---------------------------------------------------------------------------
// Back-pointers from the IddCx objects to the device context.
//
// A callback is handed an IDDCX_ADAPTER or an IDDCX_MONITOR and has to get
// from there to our own state. The way that reads naturally --
// DeviceGetContext(WdfObjectContextGetObject(adapter)) -- is wrong and was
// what this driver did: WdfObjectContextGetObject takes a *context pointer*
// and returns the object owning it, so feeding it a handle and expecting a
// WDFDEVICE back has no valid path. It typechecks because every WDF handle is
// a void* typedef, and it would have dereferenced garbage in every callback.
//
// The correct pattern, which the IndirectDisplay sample uses: give the adapter
// and the monitor their own WDF contexts holding a back-pointer, attached
// through the ObjectAttributes field of IDARG_IN_ADAPTER_INIT and
// IDARG_IN_MONITORCREATE.
// ---------------------------------------------------------------------------

struct AdapterContext {
    DeviceContext* Device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, AdapterGetContext);

struct MonitorContext {
    DeviceContext* Device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, MonitorGetContext);

// Reload the modeline store from the device list plus the on-disk INI, and
// tell IddCx the monitor's mode list changed. Called at start and whenever the
// device raises GUD_CONNECTOR_STATUS_CHANGED.
NTSTATUS ReloadModes(DeviceContext* ctx);

} // namespace gudwin
