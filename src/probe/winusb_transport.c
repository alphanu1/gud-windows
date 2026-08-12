/* SPDX-License-Identifier: MIT */
/*
 * winusb_transport.c -- gud_transport over WinUSB, for the probe tool.
 *
 * This exists so the wire can be proved before any of IddCx is written. The
 * driver does not use it: inside a UMDF driver the same three operations go
 * through WdfUsbTargetDevice, which has different types and different error
 * reporting but issues byte-for-byte identical transfers. Everything above
 * struct gud_transport is shared.
 *
 * Getting a handle at all requires that something has bound winusb.sys to the
 * device. The device advertises a vendor-class interface with no Microsoft OS
 * descriptors, so Windows will not do it unaided -- see docs/BRINGUP.md.
 */

#include "winusb_transport.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <usbiodef.h>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

struct winusb_dev {
    HANDLE          file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR           ifnum;
    UCHAR           bulk_out_pipe;
    ULONG           max_transfer;
};

/* ---------------- open ---------------- */

/*
 * Match on the hardware ID rather than a device interface GUID.
 *
 * A GUID would be cleaner, but it only exists once an INF has declared one,
 * and the whole point of this tool is to work at the stage where the only
 * thing installed is a stock WinUSB binding -- from Zadig, or from the plain
 * INF in this directory -- which declares its own GUID that varies by however
 * the user got there. The hardware ID does not vary.
 */
static int path_for_vidpid(unsigned vid, unsigned pid, char *out, size_t out_len)
{
    GUID guid = GUID_DEVINTERFACE_USB_DEVICE;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    char want[64];
    int found = 0;

    snprintf(want, sizeof want, "USB\\VID_%04X&PID_%04X", vid, pid);

    set = SetupDiGetClassDevsA(&guid, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return -1;

    ifd.cbSize = sizeof ifd;
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &ifd); i++) {
        SP_DEVINFO_DATA info;

        info.cbSize = sizeof info;
        DWORD need = 0;
        char ids[512];
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
            char raw[1024];
        } det;

        det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, &det.d,
                                              sizeof det, &need, &info))
            continue;

        if (!SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID,
                                               NULL, (PBYTE)ids, sizeof ids, NULL))
            continue;

        if (_strnicmp(ids, want, strlen(want)) != 0)
            continue;

        strncpy(out, det.d.DevicePath, out_len - 1);
        out[out_len - 1] = '\0';
        found = 1;
        break;
    }

    SetupDiDestroyDeviceInfoList(set);
    return found ? 0 : -1;
}

struct winusb_dev *winusb_open(unsigned vid, unsigned pid, char *err, size_t err_len)
{
    struct winusb_dev *d;
    char path[512];
    USB_INTERFACE_DESCRIPTOR ifdesc;
    UCHAR p;

#define FAIL(msg) do { snprintf(err, err_len, "%s (GetLastError=%lu)", msg, \
                                (unsigned long)GetLastError()); goto fail; } while (0)

    d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->file = INVALID_HANDLE_VALUE;

    if (path_for_vidpid(vid, pid, path, sizeof path) < 0) {
        snprintf(err, err_len,
                 "no device with VID_%04X&PID_%04X, or no WinUSB binding on it",
                 vid, pid);
        goto fail;
    }

    d->file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (d->file == INVALID_HANDLE_VALUE)
        FAIL("CreateFile on the device path failed");

    if (!WinUsb_Initialize(d->file, &d->usb))
        FAIL("WinUsb_Initialize failed -- winusb.sys is probably not the "
             "function driver for this device");

    if (!WinUsb_QueryInterfaceSettings(d->usb, 0, &ifdesc))
        FAIL("WinUsb_QueryInterfaceSettings failed");
    d->ifnum = ifdesc.bInterfaceNumber;

    /*
     * Find the bulk OUT endpoint. The device declares exactly one and no IN
     * endpoint at all -- everything the host reads comes back on ep0 -- so
     * anything else here means we are talking to the wrong interface.
     */
    d->bulk_out_pipe = 0xff;
    for (p = 0; p < ifdesc.bNumEndpoints; p++) {
        WINUSB_PIPE_INFORMATION pi;
        if (!WinUsb_QueryPipe(d->usb, 0, p, &pi))
            continue;
        if (pi.PipeType == UsbdPipeTypeBulk && !(pi.PipeId & 0x80))
            d->bulk_out_pipe = pi.PipeId;
    }
    if (d->bulk_out_pipe == 0xff) {
        snprintf(err, err_len, "no bulk OUT endpoint on interface %u", d->ifnum);
        goto fail;
    }

    /*
     * Pipe policy. Three of these matter and the defaults are wrong for us.
     *
     * SHORT_PACKET_TERMINATE stays OFF. With it on, WinUSB appends a
     * zero-length packet after any transfer that is an exact multiple of the
     * endpoint's max packet size. The device's reader asks for a length
     * rounded up to a packet boundary and treats what comes back as one rect;
     * an extra zero-length read is a rect header of nothing, and the stream
     * desynchronises from there with no way to resynchronise, because there is
     * no framing on the bulk stream to recover against. A 512-byte-aligned
     * rect is not rare -- it is most of them.
     *
     * AUTO_CLEAR_STALL on, so a stalled pipe recovers without the driver
     * having to notice. The device stalls ep0 on a request it does not
     * support, and a host that leaves a halt set never sends another pixel.
     *
     * PIPE_TRANSFER_TIMEOUT bounded. The default is infinite, and an infinite
     * write is how a display driver hangs the compositor when the cable is
     * pulled mid-transfer. 3000 ms matches the Linux driver's bulk timeout.
     */
    {
        UCHAR off = FALSE, on = TRUE;
        ULONG timeout = 3000;
        ULONG len;

        WinUsb_SetPipePolicy(d->usb, d->bulk_out_pipe, SHORT_PACKET_TERMINATE,
                             sizeof off, &off);
        WinUsb_SetPipePolicy(d->usb, d->bulk_out_pipe, AUTO_CLEAR_STALL,
                             sizeof on, &on);
        WinUsb_SetPipePolicy(d->usb, d->bulk_out_pipe, PIPE_TRANSFER_TIMEOUT,
                             sizeof timeout, &timeout);

        len = sizeof d->max_transfer;
        if (!WinUsb_GetPipePolicy(d->usb, d->bulk_out_pipe, MAXIMUM_TRANSFER_SIZE,
                                  &len, &d->max_transfer) || !d->max_transfer)
            d->max_transfer = 1024 * 1024;
    }

    return d;

fail:
    if (d->usb)
        WinUsb_Free(d->usb);
    if (d->file != INVALID_HANDLE_VALUE)
        CloseHandle(d->file);
    free(d);
    return NULL;
#undef FAIL
}

void winusb_close(struct winusb_dev *d)
{
    if (!d)
        return;
    if (d->usb)
        WinUsb_Free(d->usb);
    if (d->file != INVALID_HANDLE_VALUE)
        CloseHandle(d->file);
    free(d);
}

/* ---------------- transport ---------------- */

static int ctrl(void *ctx, int in, uint8_t request, uint16_t value,
                void *buf, size_t len)
{
    struct winusb_dev *d = ctx;
    WINUSB_SETUP_PACKET sp;
    ULONG moved = 0;

    /*
     * Vendor request to the interface, with wIndex carrying the interface
     * number. That is what gud_usb_control_msg() in the in-tree driver sends
     * and a device is entitled to rely on it. Sending wIndex = 0 works by
     * accident on any device with one interface and is the kind of thing that
     * breaks on the second device somebody tries.
     *
     * 0x41, not 0x40: USB_TYPE_VENDOR is 0x40 and USB_RECIP_INTERFACE is 0x01,
     * and the recipient nibble decides routing on the far end. With 0x40 the
     * Linux gadget core hands the request to the composite setup() rather than
     * to the function that owns the interface, and it is stalled there without
     * the daemon ever seeing it -- ERROR_GEN_FAILURE on this side, with the
     * device otherwise enumerating perfectly. Found on first hardware contact.
     */
    sp.RequestType = (UCHAR)(0x41 | (in ? 0x80 : 0x00));
    sp.Request     = request;
    sp.Value       = value;
    sp.Index       = d->ifnum;
    sp.Length      = (USHORT)len;

    /* WinUsb_ControlTransfer takes PUCHAR in both directions, so an OUT
     * transfer casts away const at the API boundary. Unavoidable, and the
     * only place in this file that does it. */
    if (!WinUsb_ControlTransfer(d->usb, sp, (PUCHAR)buf, (ULONG)len,
                                &moved, NULL)) {
        /*
         * Say why. A bare "transfer failed" at bring-up leaves half a dozen
         * candidates and no way to separate them from outside, and the win32
         * code separates the two that actually happen:
         *
         *   31  ERROR_GEN_FAILURE  the device stalled ep0. It enumerated, so
         *       the gadget is up, but nothing answered this vendor request --
         *       the daemon is not running, or it does not implement it.
         *   121 ERROR_SEM_TIMEOUT  no response at all within the timeout.
         *
         * Printed rather than returned, because every caller collapses this
         * to GUD_E_IO and the detail is worth more than the tidiness.
         */
        DWORD e = GetLastError();
        const char *hint = "";

        if (e == 31)
            hint = "  (device stalled ep0 -- is blitscrtd running?)";
        else if (e == 121)
            hint = "  (timed out -- device enumerated but is not answering)";
        else if (e == 2 || e == 1167)
            hint = "  (device gone -- cable or reset)";

        fprintf(stderr, "  ctrl %s req 0x%02x val %u len %u: win32 %lu%s\n",
                in ? "in " : "out", request, value, (unsigned)len,
                (unsigned long)e, hint);
        return GUD_E_IO;
    }
    return (int)moved;
}

static int t_ctrl_in(void *ctx, uint8_t r, uint16_t v, void *buf, size_t len)
{
    return ctrl(ctx, 1, r, v, buf, len);
}

static int t_ctrl_out(void *ctx, uint8_t r, uint16_t v, const void *buf, size_t len)
{
    return ctrl(ctx, 0, r, v, (void *)buf, len);
}

static int t_bulk_out(void *ctx, const void *buf, size_t len)
{
    struct winusb_dev *d = ctx;
    const UCHAR *p = buf;
    size_t left = len;

    /*
     * Split at MAXIMUM_TRANSFER_SIZE rather than assuming any length is
     * accepted. WinUSB fails the whole write if it is over, and a 640x480
     * uncompressed surface at 600 KB is close enough to the common 1 MB limit
     * that a deeper format would cross it.
     *
     * Splitting is safe because the device reads a length it computed from the
     * SET_BUFFER header, not from transfer boundaries -- consecutive writes
     * concatenate on the wire.
     */
    while (left) {
        ULONG chunk = (ULONG)(left > d->max_transfer ? d->max_transfer : left);
        ULONG moved = 0;

        if (!WinUsb_WritePipe(d->usb, d->bulk_out_pipe, (PUCHAR)p, chunk,
                              &moved, NULL))
            return GUD_E_IO;
        if (moved != chunk)
            return GUD_E_IO;
        p += chunk;
        left -= chunk;
    }
    return (int)len;
}

void winusb_transport(struct winusb_dev *d, struct gud_transport *t)
{
    t->ctrl_in  = t_ctrl_in;
    t->ctrl_out = t_ctrl_out;
    t->bulk_out = t_bulk_out;
    t->ctx      = d;
}
