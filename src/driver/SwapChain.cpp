// SPDX-License-Identifier: MIT
//
// SwapChain.cpp -- the frame loop.
//
// One thread per assigned swapchain. Per frame:
//
//   acquire  ->  dirty rects  ->  staging copy  ->  convert  ->  LZ4  ->  bulk
//
// The staging copy is the part with no way around it. IddCx hands over a
// DXGI surface that lives in GPU memory on the rendering adapter, and there is
// no CPU pointer to it. Reading it means a CopySubresourceRegion into a
// D3D11_USAGE_STAGING texture and then Map, which costs a GPU round trip per
// frame. Copying only the dirty rects rather than the whole surface is what
// keeps that affordable, and it is also what keeps the wire cheap, so the two
// motivations point the same way.
//
// Rough budget at 640x480i60, from the device side's own measurements of the
// same work in the other direction: 16.7 ms available, the bulk transfer is
// about 1.5 ms of it and LZ4 on 600 KB is a fraction of a millisecond on any
// desktop CPU. The staging copy and Map are the unknowns and are the thing to
// measure first.

#include "Driver.h"

#include <algorithm>

using namespace Microsoft::WRL;
using namespace gudwin;

namespace {

// Union of two rects.
RECT UnionRect(const RECT& a, const RECT& b)
{
    RECT r;
    // std::min/std::max rather than the min/max macros windows.h defines.
    // Those macros are absent under NOMINMAX, which anything including a C++
    // standard header tends to want, so relying on them makes the file's
    // compilability depend on include order.
    r.left   = std::min(a.left,   b.left);
    r.top    = std::min(a.top,    b.top);
    r.right  = std::max(a.right,  b.right);
    r.bottom = std::max(a.bottom, b.bottom);
    return r;
}

bool RectEmpty(const RECT& r)
{
    return r.right <= r.left || r.bottom <= r.top;
}

} // namespace

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain,
                                       LUID renderAdapter,
                                       HANDLE newFrameEvent,
                                       gud_device* dev,
                                       const gud_display_mode_req& mode)
    : m_swapChain(swapChain)
    , m_renderAdapter(renderAdapter)
    , m_newFrameEvent(newFrameEvent)
    , m_dev(dev)
    , m_mode(mode)
    , m_lz(std::make_unique<lz4enc_ctx>())
{
    size_t full = (size_t)mode.hdisplay * mode.vdisplay * dev->bpp;
    m_conv.resize(full);
    m_comp.resize(lz4enc_bound(full));

    m_thread = std::thread([this] { Run(); });
}

SwapChainProcessor::~SwapChainProcessor()
{
    m_terminate = true;
    // Waking the thread matters: it is blocked in
    // IddCxSwapChainWaitForNextFrame and will not notice the flag on its own.
    // Not doing this leaves a thread wedged in Session 0 on every unplug,
    // which is exactly the shape of fault the device side hit on host detach
    // -- a hang that only shows up when the cable comes out.
    if (m_newFrameEvent)
        SetEvent(m_newFrameEvent);
    if (m_thread.joinable())
        m_thread.join();
}

void SwapChainProcessor::Run()
{
    // Raise priority: this thread's deadline is the CRT's field rate, and
    // missing it is a dropped frame with no way to catch up.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    RunCore();
    // Tell IddCx the swapchain is finished with, whatever the reason.
    IddCxSwapChainFinishedProcessingFrame(m_swapChain);
}

void SwapChainProcessor::RunCore()
{
    // --- D3D on the rendering adapter ---
    ComPtr<IDXGIFactory5> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == m_renderAdapter.LowPart &&
            desc.AdapterLuid.HighPart == m_renderAdapter.HighPart)
            break;
        adapter.Reset();
    }
    if (!adapter)
        return;

    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &m_d3dDevice, &level, &m_d3dContext)))
        return;

    // Written first as an immediately-invoked lambda returning &arg, which
    // returns the address of the lambda's own local and is dangling by the
    // time IddCx reads it. It compiled and would have worked most of the time,
    // which is the worst kind of wrong. Caught by -Wreturn-local-addr.
    IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
    setDevice.pDevice = m_d3dDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(m_swapChain, &setDevice)))
        return;

    // A staging texture the size of the whole surface. Sized once rather than
    // per rect: creating a texture per frame is a driver-level allocation and
    // shows up as jitter, and jitter costs a frame -- the device side measured
    // exactly that, 58.5 fps instead of 60, from two milliseconds of variance.
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = m_mode.hdisplay;
        d.Height = m_mode.vdisplay;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_d3dDevice->CreateTexture2D(&d, nullptr, &m_staging)))
            return;
    }

    while (!m_terminate) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(m_swapChain, &buffer);

        if (hr == E_PENDING) {
            // Nothing new. Wait, with a bound so termination is noticed.
            HANDLE waits[] = { m_newFrameEvent };
            WaitForMultipleObjects(1, waits, FALSE, 16);
            continue;
        }
        if (FAILED(hr))
            break;

        ComPtr<ID3D11Texture2D> acquired;
        if (SUCCEEDED(buffer.MetaData.pSurface->QueryInterface(
                          IID_PPV_ARGS(&acquired)))) {

            // --- work out what changed ---
            //
            // IddCx supplies dirty rects and move regions. Move regions are
            // the compositor telling us a block of pixels slid; the source is
            // already correct on the device and only the destination needs
            // sending, so only the destination goes in the list. Treating both
            // as damage would double the cost of every window drag.
            std::vector<RECT> rects;
            const auto& md = buffer.MetaData;

            if (md.DirtyRectCount == 0 && md.MoveRegionCount == 0) {
                // No metadata means the whole surface, which is what the first
                // frame after a modeset always is.
                RECT full{ 0, 0, (LONG)m_mode.hdisplay, (LONG)m_mode.vdisplay };
                rects.push_back(full);
            } else {
                for (UINT i = 0; i < md.DirtyRectCount; i++)
                    rects.push_back(md.pDirtyRect[i]);
                for (UINT i = 0; i < md.MoveRegionCount; i++)
                    rects.push_back(md.pMoveRegions[i].DestRect);
            }

            // Past a threshold, one full surface is cheaper than the rects.
            // Each rect costs a SET_BUFFER control transfer plus a status read
            // behind it, and a control transfer occupies a whole microframe
            // whatever it carries.
            if (rects.size() > kMaxRects) {
                RECT u = rects[0];
                for (size_t i = 1; i < rects.size(); i++)
                    u = UnionRect(u, rects[i]);
                rects.clear();
                rects.push_back(u);
            }

            // One staging copy covering everything, then read rects out of it.
            // Copying each rect separately would be more precise and is worse:
            // each CopySubresourceRegion is a separate GPU command and the
            // Map at the end has to wait for all of them anyway.
            {
                RECT u = rects[0];
                for (size_t i = 1; i < rects.size(); i++)
                    u = UnionRect(u, rects[i]);
                u.left   = std::max(u.left, 0L);
                u.top    = std::max(u.top, 0L);
                u.right  = std::min(u.right,  (LONG)m_mode.hdisplay);
                u.bottom = std::min(u.bottom, (LONG)m_mode.vdisplay);

                if (!RectEmpty(u)) {
                    D3D11_BOX box{};
                    box.left = u.left; box.top = u.top; box.front = 0;
                    box.right = u.right; box.bottom = u.bottom; box.back = 1;

                    m_d3dContext->CopySubresourceRegion(
                        m_staging.Get(), 0, u.left, u.top, 0,
                        acquired.Get(), 0, &box);

                    for (const RECT& r : rects) {
                        RECT c = r;
                        c.left   = std::max(c.left, 0L);
                        c.top    = std::max(c.top, 0L);
                        c.right  = std::min(c.right,  (LONG)m_mode.hdisplay);
                        c.bottom = std::min(c.bottom, (LONG)m_mode.vdisplay);
                        if (RectEmpty(c))
                            continue;
                        if (!SendRect(m_staging.Get(), c)) {
                            m_terminate = true;
                            break;
                        }
                    }
                }
            }
        }

        IddCxSwapChainFinishedProcessingFrame(m_swapChain);
    }
}

bool SwapChainProcessor::SendRect(ID3D11Texture2D* staging, const RECT& r)
{
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
        return false;

    uint32_t x = (uint32_t)r.left;
    uint32_t y = (uint32_t)r.top;
    uint32_t w = (uint32_t)(r.right - r.left);
    uint32_t h = (uint32_t)(r.bottom - r.top);
    size_t   len = (size_t)w * h * m_dev->bpp;

    // RowPitch is whatever the driver chose and is never assumed to be
    // width*4. convert_rect() reads strided and writes packed, which is the
    // arrangement gud_flush_rect() requires: getting it backwards produces a
    // picture whose stride drifts across every line, and that looks like a
    // timing fault rather than a copy fault.
    convert_rect(m_dev->format,
                 static_cast<const uint8_t*>(map.pData), map.RowPitch,
                 m_conv.data(), x, y, w, h);

    m_d3dContext->Unmap(staging, 0);

    size_t comp = 0;
    if (m_dev->compress)
        comp = lz4enc_compress(m_lz.get(), m_conv.data(), len,
                               m_comp.data(), m_comp.size());

    int err = gud_flush_rect(m_dev, x, y, w, h,
                             m_conv.data(), len,
                             comp ? m_comp.data() : nullptr, comp);
    return err == 0;
}
