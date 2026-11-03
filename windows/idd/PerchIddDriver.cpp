/* Copyright (c) 2026 Mike J Hebblethwaite. All rights reserved. See ../../COPYRIGHT. */
#define NOMINMAX
#include "../PerchFrame.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wdf.h>
#include <iddcx.h>
#include <objbase.h>
#include <sddl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kWidthModes[] = {1280, 1920, 2560};
constexpr DWORD kHeightModes[] = {720, 1080, 1440};
constexpr DWORD kRefreshRate = 60;

struct DeviceContext;
struct MonitorContext;

struct DeviceContextWrapper {
    DeviceContext* context;
};

struct MonitorContextWrapper {
    MonitorContext* context;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContextWrapper, GetDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper, GetMonitorContextWrapper);

class FramePublisher {
public:
    bool initialize() {
        PSECURITY_DESCRIPTOR mappingDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GR;;;BU)", SDDL_REVISION_1, &mappingDescriptor, nullptr)) {
            return false;
        }
        PSECURITY_DESCRIPTOR eventDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;0x00100000;;;BU)", SDDL_REVISION_1, &eventDescriptor, nullptr)) {
            LocalFree(mappingDescriptor);
            return false;
        }
        SECURITY_ATTRIBUTES mappingAttributes{sizeof(mappingAttributes), mappingDescriptor, FALSE};
        SECURITY_ATTRIBUTES eventAttributes{sizeof(eventAttributes), eventDescriptor, FALSE};
        const DWORD high = static_cast<DWORD>(sizeof(perch::win::SharedFrames) >> 32);
        const DWORD low = static_cast<DWORD>(sizeof(perch::win::SharedFrames) & 0xffffffffu);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &mappingAttributes, PAGE_READWRITE, high, low,
                                      perch::win::kFrameMappingName);
        const DWORD mapError = GetLastError();
        shared_ = mapping_ ? static_cast<perch::win::SharedFrames*>(
                                 MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(perch::win::SharedFrames)))
                           : nullptr;
        event_ = CreateEventW(&eventAttributes, FALSE, FALSE, perch::win::kFrameEventName);
        LocalFree(eventDescriptor);
        LocalFree(mappingDescriptor);
        if (!mapping_ || !shared_ || !event_) return false;
        if (mapError != ERROR_ALREADY_EXISTS) {
            ZeroMemory(shared_, sizeof(*shared_));
            std::memcpy(shared_->magic, perch::win::kFrameMagic, sizeof(shared_->magic));
            shared_->version = perch::win::kFrameVersion;
            InterlockedExchange(&shared_->activeSlot, -1);
            MemoryBarrier();
        } else if (std::memcmp(shared_->magic, perch::win::kFrameMagic, sizeof(shared_->magic)) != 0 ||
                   shared_->version != perch::win::kFrameVersion) {
            return false;
        }
        return true;
    }

    ~FramePublisher() {
        if (shared_) UnmapViewOfFile(shared_);
        if (event_) CloseHandle(event_);
        if (mapping_) CloseHandle(mapping_);
    }

    bool due() {
        const ULONGLONG now = GetTickCount64();
        if (hasPublished_ && now - lastPublishMs_ < 100) return false;
        lastPublishMs_ = now;
        hasPublished_ = true;
        return true;
    }

    void publish(const D3D11_MAPPED_SUBRESOURCE& mapped, UINT width, UINT height, DXGI_FORMAT format) {
        if (!shared_ || !event_ || !mapped.pData ||
            (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
            width == 0 || height == 0 || width > perch::win::kMaxWidth || height > perch::win::kMaxHeight ||
            mapped.RowPitch < width * 4) {
            return;
        }
        const LONG active = InterlockedCompareExchange(&shared_->activeSlot, 0, 0);
        const LONG target = active == 0 ? 1 : 0;
        perch::win::FrameSlot& slot = shared_->slots[target];
        LONG64 sequence = InterlockedCompareExchange64(&slot.sequence, 0, 0);
        if (sequence & 1) ++sequence;
        InterlockedExchange64(&slot.sequence, sequence + 1);
        slot.width = width;
        slot.height = height;
        slot.stride = width * 4;
        slot.format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        slot.bytes = slot.stride * height;
        const BYTE* source = static_cast<const BYTE*>(mapped.pData);
        for (UINT y = 0; y < height; ++y) {
            const BYTE* sourceRow = source + static_cast<std::size_t>(y) * mapped.RowPitch;
            BYTE* targetRow = slot.pixels + static_cast<std::size_t>(y) * slot.stride;
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                std::memcpy(targetRow, sourceRow, slot.stride);
            } else {
                for (UINT x = 0; x < width; ++x) {
                    targetRow[x * 4] = sourceRow[x * 4 + 2];
                    targetRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                    targetRow[x * 4 + 2] = sourceRow[x * 4];
                    targetRow[x * 4 + 3] = sourceRow[x * 4 + 3];
                }
            }
        }
        MemoryBarrier();
        InterlockedExchange64(&slot.sequence, sequence + 2);
        InterlockedExchange(&shared_->activeSlot, target);
        InterlockedIncrement64(&shared_->latestSequence);
        SetEvent(event_);
    }

private:
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    perch::win::SharedFrames* shared_ = nullptr;
    ULONGLONG lastPublishMs_ = 0;
    bool hasPublished_ = false;
};

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, LUID adapter, HANDLE frameEvent)
        : swapChain_(swapChain), adapterLuid_(adapter), frameEvent_(frameEvent) {
        terminateEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!terminateEvent_) throw std::runtime_error("cannot create the IddCx stop event");
        thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, nullptr);
        if (!terminateEvent_ || !thread_) {
            if (thread_) CloseHandle(thread_);
            if (terminateEvent_) CloseHandle(terminateEvent_);
            thread_ = nullptr;
            terminateEvent_ = nullptr;
            throw std::runtime_error("cannot start the IddCx swap-chain processor");
        }
    }

    ~SwapChainProcessor() {
        if (terminateEvent_) SetEvent(terminateEvent_);
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        if (terminateEvent_) CloseHandle(terminateEvent_);
    }

private:
    static DWORD WINAPI thread_entry(void* argument) {
        static_cast<SwapChainProcessor*>(argument)->run();
        return 0;
    }

    bool create_device() {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) return false;
        hr = factory_->EnumAdapterByLuid(adapterLuid_, IID_PPV_ARGS(&adapter_));
        if (FAILED(hr)) return false;
        hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        return SUCCEEDED(hr);
    }

    void run() {
        if (!create_device()) {
            OutputDebugStringW(L"Perch IDD: Direct3D device initialization failed\n");
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
            return;
        }
        IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) {
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
            return;
        }
        setDevice.pDevice = dxgiDevice.Get();
        if (FAILED(IddCxSwapChainSetDevice(swapChain_, &setDevice))) {
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
            return;
        }

        FramePublisher publisher;
        if (!publisher.initialize()) {
            OutputDebugStringW(L"Perch IDD: cannot create the local frame feed\n");
        }

        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC sourceDescription{};
        for (;;) {
            if (WaitForSingleObject(terminateEvent_, 0) == WAIT_OBJECT_0) break;
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
            HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &buffer);
            if (hr == E_PENDING) {
                HANDLE events[] = {frameEvent_, terminateEvent_};
                const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 16);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) continue;
                break;
            }
            if (FAILED(hr)) break;

            ComPtr<IDXGIResource> acquired;
            acquired.Attach(buffer.MetaData.pSurface);
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(acquired.As(&texture))) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (!staging || desc.Width != sourceDescription.Width || desc.Height != sourceDescription.Height ||
                    desc.Format != sourceDescription.Format) {
                    staging.Reset();
                    sourceDescription = desc;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.MiscFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
                        OutputDebugStringW(L"Perch IDD: cannot create a CPU-readable staging surface\n");
                    }
                }
                if (staging && publisher.due()) {
                    context_->CopyResource(staging.Get(), texture.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                        publisher.publish(mapped, sourceDescription.Width, sourceDescription.Height, sourceDescription.Format);
                        context_->Unmap(staging.Get(), 0);
                    }
                }
            }
            acquired.Reset();
            if (FAILED(IddCxSwapChainFinishedProcessingFrame(swapChain_))) break;
        }
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
    }

    IDDCX_SWAPCHAIN swapChain_ = nullptr;
    LUID adapterLuid_{};
    HANDLE frameEvent_ = nullptr;
    HANDLE terminateEvent_ = nullptr;
    HANDLE thread_ = nullptr;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
};

struct MonitorContext {
    explicit MonitorContext(IDDCX_MONITOR monitor) : monitor(monitor) {}
    void assign(IDDCX_SWAPCHAIN chain, LUID adapter, HANDLE event) {
        processor.reset();
        try {
            processor = std::make_unique<SwapChainProcessor>(chain, adapter, event);
        } catch (...) {
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(chain));
            OutputDebugStringW(L"Perch IDD: failed to start swap-chain processing\n");
        }
    }
    IDDCX_MONITOR monitor;
    std::unique_ptr<SwapChainProcessor> processor;
};

struct DeviceContext {
    explicit DeviceContext(WDFDEVICE device) : device(device) {}

    NTSTATUS init_adapter() {
        if (adapter) return STATUS_SUCCESS;
        IDDCX_ADAPTER_CAPS caps{};
        caps.Size = sizeof(caps);
        caps.MaxMonitorsSupported = 1;
        caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
        caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
        caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
        caps.EndPointDiagnostics.pEndPointFriendlyName = L"Perch virtual display";
        caps.EndPointDiagnostics.pEndPointManufacturerName = L"Perch";
        caps.EndPointDiagnostics.pEndPointModelName = L"Perch eye";
        IDDCX_ENDPOINT_VERSION version{};
        version.Size = sizeof(version);
        version.MajorVer = 1;
        caps.EndPointDiagnostics.pFirmwareVersion = &version;
        caps.EndPointDiagnostics.pHardwareVersion = &version;

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
        IDARG_IN_ADAPTER_INIT input{};
        input.WdfDevice = device;
        input.pCaps = &caps;
        input.ObjectAttributes = &attributes;
        IDARG_OUT_ADAPTER_INIT output{};
        const NTSTATUS status = IddCxAdapterInitAsync(&input, &output);
        if (NT_SUCCESS(status)) {
            adapter = output.AdapterObject;
            GetDeviceContextWrapper(adapter)->context = this;
        }
        return status;
    }

    NTSTATUS finish_init() {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);
        attributes.EvtCleanupCallback = [](WDFOBJECT object) {
            auto* wrapper = GetMonitorContextWrapper(object);
            delete wrapper->context;
            wrapper->context = nullptr;
        };
        IDDCX_MONITOR_INFO info{};
        info.Size = sizeof(info);
        info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
        info.ConnectorIndex = 0;
        info.MonitorDescription.Size = sizeof(info.MonitorDescription);
        info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
        info.MonitorDescription.DataSize = 0;
        info.MonitorDescription.pData = nullptr;
        if (FAILED(CoCreateGuid(&info.MonitorContainerId))) return STATUS_UNSUCCESSFUL;

        IDARG_IN_MONITORCREATE input{};
        input.ObjectAttributes = &attributes;
        input.pMonitorInfo = &info;
        IDARG_OUT_MONITORCREATE output{};
        NTSTATUS status = IddCxMonitorCreate(adapter, &input, &output);
        if (!NT_SUCCESS(status)) return status;
        try {
            GetMonitorContextWrapper(output.MonitorObject)->context = new MonitorContext(output.MonitorObject);
        } catch (...) {
            WdfObjectDelete(output.MonitorObject);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        IDARG_OUT_MONITORARRIVAL arrival{};
        status = IddCxMonitorArrival(output.MonitorObject, &arrival);
        if (!NT_SUCCESS(status)) WdfObjectDelete(output.MonitorObject);
        return status;
    }

    WDFDEVICE device;
    IDDCX_ADAPTER adapter = nullptr;
};

static IDDCX_MONITOR_MODE monitor_mode(DWORD width, DWORD height) {
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    auto& signal = mode.MonitorVideoSignalInfo;
    signal.totalSize.cx = signal.activeSize.cx = width;
    signal.totalSize.cy = signal.activeSize.cy = height;
    signal.vSyncFreq.Numerator = kRefreshRate;
    signal.vSyncFreq.Denominator = 1;
    signal.hSyncFreq.Numerator = kRefreshRate * height;
    signal.hSyncFreq.Denominator = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    signal.pixelRate = static_cast<UINT64>(width) * height * kRefreshRate;
    signal.AdditionalSignalInfo.videoStandard = 255;
    return mode;
}

static IDDCX_TARGET_MODE target_mode(DWORD width, DWORD height) {
    IDDCX_TARGET_MODE mode{};
    mode.Size = sizeof(mode);
    auto& signal = mode.TargetVideoSignalInfo.targetVideoSignalInfo;
    signal.totalSize.cx = signal.activeSize.cx = width;
    signal.totalSize.cy = signal.activeSize.cy = height;
    signal.vSyncFreq.Numerator = kRefreshRate;
    signal.vSyncFreq.Denominator = 1;
    signal.hSyncFreq.Numerator = kRefreshRate * height;
    signal.hSyncFreq.Denominator = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    signal.pixelRate = static_cast<UINT64>(width) * height * kRefreshRate;
    signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
    signal.AdditionalSignalInfo.videoStandard = 255;
    return mode;
}

EVT_WDF_DRIVER_DEVICE_ADD PerchDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY PerchDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED PerchAdapterInitFinished;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION PerchParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES PerchGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES PerchQueryTargetModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES PerchCommitModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN PerchAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN PerchUnassignSwapChain;

_Use_decl_annotations_
NTSTATUS PerchDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    WDF_PNPPOWER_EVENT_CALLBACKS powerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&powerCallbacks);
    powerCallbacks.EvtDeviceD0Entry = PerchDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &powerCallbacks);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = PerchAdapterInitFinished;
    config.EvtIddCxParseMonitorDescription = PerchParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = PerchGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes = PerchQueryTargetModes;
    config.EvtIddCxAdapterCommitModes = PerchCommitModes;
    config.EvtIddCxMonitorAssignSwapChain = PerchAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = PerchUnassignSwapChain;
    NTSTATUS status = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* wrapper = GetDeviceContextWrapper(object);
        delete wrapper->context;
        wrapper->context = nullptr;
    };
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;
    try {
        GetDeviceContextWrapper(device)->context = new DeviceContext(device);
    } catch (...) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto* context = GetDeviceContextWrapper(device)->context;
    return context ? context->init_adapter() : STATUS_INVALID_DEVICE_STATE;
}

_Use_decl_annotations_
NTSTATUS PerchAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* input) {
    auto* wrapper = GetDeviceContextWrapper(adapter);
    if (!wrapper || !wrapper->context) return STATUS_INVALID_DEVICE_STATE;
    return NT_SUCCESS(input->AdapterInitStatus) ? wrapper->context->finish_init() : input->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS PerchParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION*, IDARG_OUT_PARSEMONITORDESCRIPTION* output) {
    output->MonitorModeBufferOutputCount = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
                              IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output) {
    output->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(kWidthModes);
    output->PreferredMonitorModeIdx = 0;
    if (input->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (input->DefaultMonitorModeBufferInputCount < ARRAYSIZE(kWidthModes)) return STATUS_BUFFER_TOO_SMALL;
    for (DWORD i = 0; i < ARRAYSIZE(kWidthModes); ++i) {
        input->pDefaultMonitorModes[i] = monitor_mode(kWidthModes[i], kHeightModes[i]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchQueryTargetModes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES* input, IDARG_OUT_QUERYTARGETMODES* output) {
    output->TargetModeBufferOutputCount = ARRAYSIZE(kWidthModes);
    if (input->TargetModeBufferInputCount >= ARRAYSIZE(kWidthModes)) {
        for (DWORD i = 0; i < ARRAYSIZE(kWidthModes); ++i) {
            input->pTargetModes[i] = target_mode(kWidthModes[i], kHeightModes[i]);
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* input) {
    auto* wrapper = GetMonitorContextWrapper(monitor);
    if (!wrapper || !wrapper->context) {
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(input->hSwapChain));
        return STATUS_INVALID_DEVICE_STATE;
    }
    wrapper->context->assign(input->hSwapChain, input->RenderAdapterLuid, input->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PerchUnassignSwapChain(IDDCX_MONITOR monitor) {
    auto* wrapper = GetMonitorContextWrapper(monitor);
    if (wrapper && wrapper->context) wrapper->context->processor.reset();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS driver_entry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PerchDeviceAdd);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    return WdfDriverCreate(driver, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

} // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    return driver_entry(driver, registryPath);
}
