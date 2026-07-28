/* SPDX-License-Identifier: MIT
 *
 * tests/wdkstub/IddCx.h -- NOT A REAL HEADER.
 *
 * Written from the documented IddCx surface so Driver.cpp and SwapChain.cpp
 * can be put through a compiler without the WDK. It proves internal
 * consistency and nothing about whether these declarations match Microsoft's.
 *
 * The structures most likely to be wrong against a real IddCx.h, in order:
 * IDDCX_METADATA (its members moved between versions), IDARG_IN_COMMITMODES_PATH,
 * and the swapchain argument structs. Diff those three first when the WDK is
 * to hand.
 */
#ifndef WDKSTUB_IDDCX_H
#define WDKSTUB_IDDCX_H

#include <windows.h>
#include <wingdi.h>
#include "wdf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID IDDCX_ADAPTER;
typedef PVOID IDDCX_MONITOR;
typedef PVOID IDDCX_SWAPCHAIN;
typedef PVOID IDDCX_OPMCTX;

typedef enum { IDDCX_FEATURE_IMPLEMENTATION_NONE = 0,
               IDDCX_FEATURE_IMPLEMENTATION_HARDWARE,
               IDDCX_FEATURE_IMPLEMENTATION_SOFTWARE } IDDCX_FEATURE_IMPLEMENTATION;
typedef enum { IDDCX_TRANSMISSION_TYPE_OTHER = 0,
               IDDCX_TRANSMISSION_TYPE_WIRED_OTHER } IDDCX_TRANSMISSION_TYPE;
typedef enum { IDDCX_MONITOR_MODE_ORIGIN_UNINITIALIZED = 0,
               IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
               IDDCX_MONITOR_MODE_ORIGIN_DRIVER } IDDCX_MONITOR_MODE_ORIGIN;
typedef enum { IDDCX_MONITOR_DESCRIPTION_TYPE_UNINITIALIZED = 0,
               IDDCX_MONITOR_DESCRIPTION_TYPE_EDID } IDDCX_MONITOR_DESCRIPTION_TYPE;

typedef struct { UINT Size; IDDCX_MONITOR_DESCRIPTION_TYPE Type;
                 UINT DataSize; BYTE *pData; } IDDCX_MONITOR_DESCRIPTION;

typedef struct { UINT Size; UINT ConnectorIndex;
                 DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY MonitorType;
                 IDDCX_MONITOR_DESCRIPTION MonitorDescription;
                 GUID MonitorContainerId; } IDDCX_MONITOR_INFO;

typedef struct { UINT Size; IDDCX_FEATURE_IMPLEMENTATION GammaSupport;
                 IDDCX_TRANSMISSION_TYPE TransmissionType;
                 const WCHAR *pEndPointFriendlyName;
                 const WCHAR *pEndPointManufacturerName;
                 const WCHAR *pEndPointModelName; } IDDCX_ENDPOINT_DIAGNOSTIC_INFO;

typedef struct { UINT Size; UINT MaxMonitorsSupported;
                 IDDCX_ENDPOINT_DIAGNOSTIC_INFO EndPointDiagnostics;
               } IDDCX_ADAPTER_CAPS;

typedef struct { UINT Size; DISPLAYCONFIG_VIDEO_SIGNAL_INFO TargetVideoSignalInfo;
               } IDDCX_TARGET_MODE;
typedef struct { UINT Size; IDDCX_MONITOR_MODE_ORIGIN Origin;
                 DISPLAYCONFIG_VIDEO_SIGNAL_INFO MonitorVideoSignalInfo;
               } IDDCX_MONITOR_MODE;

typedef struct { WDFDEVICE WdfDevice; const IDDCX_ADAPTER_CAPS *pCaps;
                 const VOID *ObjectAttributes; } IDARG_IN_ADAPTER_INIT;
typedef struct { IDDCX_ADAPTER AdapterObject; } IDARG_OUT_ADAPTER_INIT;
typedef struct { NTSTATUS AdapterInitStatus; } IDARG_IN_ADAPTER_INIT_FINISHED;

typedef struct { const IDDCX_MONITOR_INFO *pMonitorInfo;
                 const VOID *ObjectAttributes; } IDARG_IN_MONITORCREATE;
typedef struct { IDDCX_MONITOR MonitorObject; } IDARG_OUT_MONITORCREATE;
typedef struct { UINT ConnectorIndex; } IDARG_OUT_MONITORARRIVAL;

typedef struct { UINT DefaultMonitorModeBufferInputCount;
                 IDDCX_MONITOR_MODE *pDefaultMonitorModes;
               } IDARG_IN_GETDEFAULTDESCRIPTIONMODES;
typedef struct { UINT DefaultMonitorModeBufferOutputCount;
                 UINT PreferredMonitorModeIdx;
               } IDARG_OUT_GETDEFAULTDESCRIPTIONMODES;

typedef struct { IDDCX_MONITOR_MODE MonitorMode;
                 UINT TargetModeBufferInputCount;
                 IDDCX_TARGET_MODE *pTargetModes; } IDARG_IN_QUERYTARGETMODES;
typedef struct { UINT TargetModeBufferOutputCount; } IDARG_OUT_QUERYTARGETMODES;

typedef struct { BOOL Active; IDDCX_MONITOR hMonitor;
                 IDDCX_TARGET_MODE TargetVideoSignalInfo; } IDARG_IN_COMMITMODES_PATH;
typedef struct { UINT PathCount;
                 const IDARG_IN_COMMITMODES_PATH *pPaths; } IDARG_IN_COMMITMODES;

typedef struct { IDDCX_SWAPCHAIN hSwapChain; LUID RenderAdapterLuid;
                 HANDLE hNextSurfaceAvailable; } IDARG_IN_SETSWAPCHAIN;
typedef struct { IUnknown *pDevice; } IDARG_IN_SWAPCHAINSETDEVICE;

typedef struct { RECT SourceRect; RECT DestRect; } IDDCX_MOVE_REGION;

typedef struct {
    UINT               Size;
    UINT               PresentationFrameNumber;
    IUnknown          *pSurface;
    UINT               DirtyRectCount;
    const RECT        *pDirtyRect;
    UINT               MoveRegionCount;
    const IDDCX_MOVE_REGION *pMoveRegions;
} IDDCX_METADATA;

typedef struct { UINT Size; UINT PresentationFrameNumber; } IDDCX_FRAME_STATISTICS;

typedef struct { LUID MetadataAdapterLuid;
                 IDDCX_FRAME_STATISTICS FrameStatistics;
                 IDDCX_METADATA MetaData; } IDARG_OUT_RELEASEANDACQUIREBUFFER;

typedef NTSTATUS (*PFN_IDD_CX_ADAPTER_INIT_FINISHED)(IDDCX_ADAPTER, const IDARG_IN_ADAPTER_INIT_FINISHED *);
typedef NTSTATUS (*PFN_IDD_CX_ADAPTER_COMMIT_MODES)(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *);
typedef NTSTATUS (*PFN_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES)(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *);
typedef NTSTATUS (*PFN_IDD_CX_MONITOR_QUERY_TARGET_MODES)(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES *, IDARG_OUT_QUERYTARGETMODES *);
typedef NTSTATUS (*PFN_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN)(IDDCX_MONITOR, const IDARG_IN_SETSWAPCHAIN *);
typedef NTSTATUS (*PFN_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN)(IDDCX_MONITOR);

typedef struct {
    UINT Size;
    PFN_IDD_CX_ADAPTER_INIT_FINISHED                 EvtIddCxAdapterInitFinished;
    PFN_IDD_CX_ADAPTER_COMMIT_MODES                  EvtIddCxAdapterCommitModes;
    PFN_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES EvtIddCxMonitorGetDefaultDescriptionModes;
    PFN_IDD_CX_MONITOR_QUERY_TARGET_MODES            EvtIddCxMonitorQueryTargetModes;
    PFN_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN              EvtIddCxMonitorAssignSwapChain;
    PFN_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN            EvtIddCxMonitorUnassignSwapChain;
} IDD_CX_CLIENT_CONFIG;

#define IDD_CX_CLIENT_CONFIG_INIT(c) \
        do { ZeroMemory((c), sizeof(*(c))); (c)->Size = sizeof(*(c)); } while (0)

NTSTATUS IddCxDeviceInitConfig(PWDFDEVICE_INIT, const IDD_CX_CLIENT_CONFIG *);
NTSTATUS IddCxDeviceInitialize(WDFDEVICE);
NTSTATUS IddCxAdapterInitAsync(const IDARG_IN_ADAPTER_INIT *, IDARG_OUT_ADAPTER_INIT *);
NTSTATUS IddCxMonitorCreate(IDDCX_ADAPTER, const IDARG_IN_MONITORCREATE *, IDARG_OUT_MONITORCREATE *);
NTSTATUS IddCxMonitorArrival(IDDCX_MONITOR, IDARG_OUT_MONITORARRIVAL *);
NTSTATUS IddCxMonitorDeparture(IDDCX_MONITOR);
HRESULT  IddCxSwapChainSetDevice(IDDCX_SWAPCHAIN, const IDARG_IN_SWAPCHAINSETDEVICE *);
HRESULT  IddCxSwapChainReleaseAndAcquireBuffer(IDDCX_SWAPCHAIN, IDARG_OUT_RELEASEANDACQUIREBUFFER *);
HRESULT  IddCxSwapChainFinishedProcessingFrame(IDDCX_SWAPCHAIN);

#ifdef __cplusplus
}
#endif
#endif /* WDKSTUB_IDDCX_H */
