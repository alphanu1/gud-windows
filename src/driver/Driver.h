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

#include <windows.h>
#include <wdf.h>
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

    std::unique_ptr<SwapChainProcessor> Processor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, DeviceGetContext);

// Reload the modeline store from the device list plus the on-disk INI, and
// tell IddCx the monitor's mode list changed. Called at start and whenever the
// device raises GUD_CONNECTOR_STATUS_CHANGED.
NTSTATUS ReloadModes(DeviceContext* ctx);

} // namespace gudwin
