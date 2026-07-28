/* SPDX-License-Identifier: MIT
 *
 * tests/wdkstub/wdf.h -- NOT A REAL HEADER.
 *
 * Enough of UMDF2 to let Driver.cpp and SwapChain.cpp be compiled by a
 * cross-compiler that has no WDK. This checks the driver code against *these
 * declarations*, which were written from the documented API, and not against
 * Microsoft's. So it catches C++ errors, typos, wrong member names, bad
 * control flow and const mistakes, and it does not and cannot confirm that the
 * API surface is right.
 *
 * What it does catch is worth having: nothing here had ever been through a
 * compiler before, and "it looks right" is not a claim anybody should accept
 * about 700 lines of driver.
 *
 * Never ship this. `make syntax` uses it; the real build uses the WDK.
 */
#ifndef WDKSTUB_WDF_H
#define WDKSTUB_WDF_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef LONG NTSTATUS;

/* Kernel types UMDF re-exports that a plain windows.h has no reason to carry. */
typedef struct _DRIVER_OBJECT  *PDRIVER_OBJECT;
typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; }
        UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;

/* windows.h already defines several of these as DWORD. Ours are NTSTATUS. */
#undef STATUS_SUCCESS
#undef STATUS_UNSUCCESSFUL
#undef STATUS_INVALID_PARAMETER
#undef STATUS_INVALID_DEVICE_STATE
#undef STATUS_DEVICE_CONFIGURATION_ERROR
#undef STATUS_DEVICE_DATA_ERROR
#undef STATUS_DEVICE_PROTOCOL_ERROR

#define STATUS_SUCCESS                      ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL                 ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER            ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_STATE         ((NTSTATUS)0xC0000184L)
#define STATUS_DEVICE_CONFIGURATION_ERROR   ((NTSTATUS)0xC0000182L)
#define STATUS_DEVICE_DATA_ERROR            ((NTSTATUS)0xC000009CL)
#define STATUS_DEVICE_PROTOCOL_ERROR        ((NTSTATUS)0xC0000186L)
#define NT_SUCCESS(s)                       (((NTSTATUS)(s)) >= 0)

#define _Use_decl_annotations_
#define WDFAPI

typedef PVOID WDFOBJECT;
typedef PVOID WDFDRIVER;
typedef PVOID WDFDEVICE;
typedef PVOID WDFREQUEST;
typedef PVOID WDFUSBDEVICE;
typedef PVOID WDFUSBINTERFACE;
typedef PVOID WDFUSBPIPE;
typedef struct WDFDEVICE_INIT_ *PWDFDEVICE_INIT;

#define WDF_NO_OBJECT_ATTRIBUTES  ((PWDF_OBJECT_ATTRIBUTES)NULL)
#define WDF_NO_HANDLE             (NULL)

typedef enum { WdfPowerDeviceD0 = 1, WdfPowerDeviceInvalid = 0 }
    WDF_POWER_DEVICE_STATE;

typedef struct _WDF_OBJECT_ATTRIBUTES {
    ULONG  Size;
    PVOID  ContextTypeInfo;
} WDF_OBJECT_ATTRIBUTES, *PWDF_OBJECT_ATTRIBUTES;

#define WDF_OBJECT_ATTRIBUTES_INIT(a) do { (a)->Size = sizeof(*(a)); \
                                           (a)->ContextTypeInfo = NULL; } while (0)
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a, type) \
        do { (a)->Size = sizeof(*(a)); (a)->ContextTypeInfo = NULL; } while (0)

/* The real macro emits a typed accessor. This does the same, badly but
 * type-correctly, which is all the syntax check needs. */
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, getter) \
        static inline type *getter(WDFOBJECT o) { return (type *)o; }

WDFOBJECT WdfObjectContextGetObject(PVOID context);

typedef NTSTATUS (*PFN_WDF_DEVICE_D0_ENTRY)(WDFDEVICE, WDF_POWER_DEVICE_STATE);
typedef NTSTATUS (*PFN_WDF_DEVICE_D0_EXIT )(WDFDEVICE, WDF_POWER_DEVICE_STATE);

typedef struct _WDF_PNPPOWER_EVENT_CALLBACKS {
    ULONG Size;
    PFN_WDF_DEVICE_D0_ENTRY EvtDeviceD0Entry;
    PFN_WDF_DEVICE_D0_EXIT  EvtDeviceD0Exit;
} WDF_PNPPOWER_EVENT_CALLBACKS, *PWDF_PNPPOWER_EVENT_CALLBACKS;

#define WDF_PNPPOWER_EVENT_CALLBACKS_INIT(c) \
        do { ZeroMemory((c), sizeof(*(c))); (c)->Size = sizeof(*(c)); } while (0)

void WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT,
                                            PWDF_PNPPOWER_EVENT_CALLBACKS);

typedef NTSTATUS (*PFN_WDF_DRIVER_DEVICE_ADD)(WDFDRIVER, PWDFDEVICE_INIT);

typedef struct _WDF_DRIVER_CONFIG {
    ULONG Size;
    PFN_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;
    ULONG DriverPoolTag;
    ULONG DriverInitFlags;
} WDF_DRIVER_CONFIG, *PWDF_DRIVER_CONFIG;

#define WDF_DRIVER_CONFIG_INIT(c, add) \
        do { ZeroMemory((c), sizeof(*(c))); (c)->Size = sizeof(*(c)); \
             (c)->EvtDriverDeviceAdd = (add); } while (0)

NTSTATUS WdfDriverCreate(PDRIVER_OBJECT, PCUNICODE_STRING,
                         PWDF_OBJECT_ATTRIBUTES, PWDF_DRIVER_CONFIG, WDFDRIVER *);
NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *, PWDF_OBJECT_ATTRIBUTES, WDFDEVICE *);

/* ---------------- memory and request options ---------------- */

typedef struct _WDF_MEMORY_DESCRIPTOR {
    ULONG Type;
    PVOID Buffer;
    ULONG Length;
} WDF_MEMORY_DESCRIPTOR, *PWDF_MEMORY_DESCRIPTOR;

#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(d, buf, len) \
        do { (d)->Type = 1; (d)->Buffer = (buf); (d)->Length = (len); } while (0)

typedef LONGLONG WDF_TIMEOUT;
#define WDF_REL_TIMEOUT_IN_MS(ms) ((LONGLONG)(-1) * (ms) * 10000)
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 0x00000004

typedef struct _WDF_REQUEST_SEND_OPTIONS {
    ULONG     Size;
    ULONG     Flags;
    LONGLONG  Timeout;
} WDF_REQUEST_SEND_OPTIONS, *PWDF_REQUEST_SEND_OPTIONS;

#define WDF_REQUEST_SEND_OPTIONS_INIT(o, flags) \
        do { ZeroMemory((o), sizeof(*(o))); (o)->Size = sizeof(*(o)); \
             (o)->Flags = (flags); } while (0)
#define WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(o, t) ((o)->Timeout = (t))

/* ---------------- USB ---------------- */

#define USBD_CLIENT_CONTRACT_VERSION_602 0x602

typedef enum {
    WdfUsbPipeTypeInvalid = 0,
    WdfUsbPipeTypeControl,
    WdfUsbPipeTypeIsochronous,
    WdfUsbPipeTypeBulk,
    WdfUsbPipeTypeInterrupt
} WDF_USB_PIPE_TYPE;

typedef enum {
    BmRequestHostToDevice = 0,
    BmRequestDeviceToHost = 1
} WDF_USB_BMREQUEST_DIRECTION;

typedef enum {
    BmRequestToDevice = 0,
    BmRequestToInterface = 1,
    BmRequestToEndpoint = 2,
    BmRequestToOther = 3
} WDF_USB_BMREQUEST_RECIPIENT;

typedef struct _WDF_USB_DEVICE_CREATE_CONFIG {
    ULONG Size;
    ULONG USBDClientContractVersion;
} WDF_USB_DEVICE_CREATE_CONFIG, *PWDF_USB_DEVICE_CREATE_CONFIG;

#define WDF_USB_DEVICE_CREATE_CONFIG_INIT(c, ver) \
        do { ZeroMemory((c), sizeof(*(c))); (c)->Size = sizeof(*(c)); \
             (c)->USBDClientContractVersion = (ver); } while (0)

typedef struct _WDF_USB_DEVICE_SELECT_CONFIG_PARAMS {
    ULONG Size;
    ULONG Type;
    union {
        struct {
            WDFUSBINTERFACE ConfiguredUsbInterface;
            UCHAR           NumberConfiguredPipes;
        } SingleInterface;
    } Types;
} WDF_USB_DEVICE_SELECT_CONFIG_PARAMS, *PWDF_USB_DEVICE_SELECT_CONFIG_PARAMS;

#define WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(p) \
        do { ZeroMemory((p), sizeof(*(p))); (p)->Size = sizeof(*(p)); } while (0)

typedef struct _WDF_USB_PIPE_INFORMATION {
    ULONG              Size;
    ULONG              MaximumPacketSize;
    UCHAR              EndpointAddress;
    UCHAR              Interval;
    UCHAR              SettingIndex;
    WDF_USB_PIPE_TYPE  PipeType;
    ULONG              MaximumTransferSize;
} WDF_USB_PIPE_INFORMATION, *PWDF_USB_PIPE_INFORMATION;

#define WDF_USB_PIPE_INFORMATION_INIT(i) \
        do { ZeroMemory((i), sizeof(*(i))); (i)->Size = sizeof(*(i)); } while (0)

typedef struct _WDF_USB_CONTROL_SETUP_PACKET {
    struct {
        UCHAR  bmRequestType;
        UCHAR  bRequest;
        USHORT wValue;
        USHORT wIndex;
        USHORT wLength;
    } Packet;
} WDF_USB_CONTROL_SETUP_PACKET, *PWDF_USB_CONTROL_SETUP_PACKET;

#define WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(p, dir, recip, req, val, idx) \
        do { ZeroMemory((p), sizeof(*(p)));                                   \
             (p)->Packet.bmRequestType =                                      \
                 (UCHAR)(0x40 | ((dir) ? 0x80 : 0x00) | ((recip) & 0x1f));    \
             (p)->Packet.bRequest = (UCHAR)(req);                             \
             (p)->Packet.wValue   = (USHORT)(val);                            \
             (p)->Packet.wIndex   = (USHORT)(idx); } while (0)

NTSTATUS WdfUsbTargetDeviceCreateWithParameters(WDFDEVICE,
            PWDF_USB_DEVICE_CREATE_CONFIG, PWDF_OBJECT_ATTRIBUTES, WDFUSBDEVICE *);
NTSTATUS WdfUsbTargetDeviceSelectConfig(WDFUSBDEVICE, PWDF_OBJECT_ATTRIBUTES,
            PWDF_USB_DEVICE_SELECT_CONFIG_PARAMS);
NTSTATUS WdfUsbTargetDeviceSendControlTransferSynchronously(WDFUSBDEVICE,
            WDFREQUEST, PWDF_REQUEST_SEND_OPTIONS, PWDF_USB_CONTROL_SETUP_PACKET,
            PWDF_MEMORY_DESCRIPTOR, PULONG);

UCHAR      WdfUsbInterfaceGetInterfaceNumber(WDFUSBINTERFACE);
BYTE       WdfUsbInterfaceGetNumConfiguredPipes(WDFUSBINTERFACE);
WDFUSBPIPE WdfUsbInterfaceGetConfiguredPipe(WDFUSBINTERFACE, UCHAR,
                                            PWDF_USB_PIPE_INFORMATION);
BOOLEAN    WdfUsbTargetPipeIsInEndpoint(WDFUSBPIPE);
void       WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(WDFUSBPIPE);
NTSTATUS   WdfUsbTargetPipeWriteSynchronously(WDFUSBPIPE, WDFREQUEST,
                PWDF_REQUEST_SEND_OPTIONS, PWDF_MEMORY_DESCRIPTOR, PULONG);
NTSTATUS   WdfUsbTargetPipeResetSynchronously(WDFUSBPIPE, WDFREQUEST,
                PWDF_REQUEST_SEND_OPTIONS);

#ifdef __cplusplus
}
#endif

#endif /* WDKSTUB_WDF_H */
