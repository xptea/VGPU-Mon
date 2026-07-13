#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

#include "dxgi_gpu.h"

bool dxgi_open(DxgiContext *context) {
    if (!context) return false;
    memset(context, 0, sizeof(*context));

    IDXGIFactory1 *factory = NULL;
    HRESULT result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(result)) {
        snprintf(context->error, sizeof(context->error), "CreateDXGIFactory1 failed (0x%08lx)", (unsigned long)result);
        return false;
    }
    context->factory = factory;

    for (UINT index = 0; index < VGPU_MAX_GPUS; ++index) {
        IDXGIAdapter1 *adapter1 = NULL;
        result = IDXGIFactory1_EnumAdapters1(factory, index, &adapter1);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;

        DXGI_ADAPTER_DESC1 description;
        memset(&description, 0, sizeof(description));
        IDXGIAdapter1_GetDesc1(adapter1, &description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            IDXGIAdapter1_Release(adapter1);
            continue;
        }

        IDXGIAdapter3 *adapter3 = NULL;
        result = IDXGIAdapter1_QueryInterface(adapter1, &IID_IDXGIAdapter3, (void **)&adapter3);
        IDXGIAdapter1_Release(adapter1);
        if (FAILED(result) || !adapter3) continue;

        unsigned int slot = context->count++;
        context->adapters[slot] = adapter3;
        wide_to_utf8(description.Description, context->names[slot], sizeof(context->names[slot]));
        context->adapter_luids[slot] = description.AdapterLuid;
        context->dedicated_memory[slot] = (uint64_t)description.DedicatedVideoMemory;
        context->vendor_ids[slot] = description.VendorId;
    }

    if (context->count == 0) {
        snprintf(context->error, sizeof(context->error), "DXGI found no hardware adapters");
        dxgi_close(context);
        return false;
    }
    return true;
}
void dxgi_close(DxgiContext *context) {
    if (!context) return;
    for (unsigned int i = 0; i < context->count; ++i) {
        IDXGIAdapter3 *adapter = (IDXGIAdapter3 *)context->adapters[i];
        if (adapter) IDXGIAdapter3_Release(adapter);
        context->adapters[i] = NULL;
    }
    context->count = 0;
    if (context->factory) IDXGIFactory1_Release((IDXGIFactory1 *)context->factory);
    context->factory = NULL;
}

bool dxgi_sample(DxgiContext *context, unsigned int index, GpuTelemetry *telemetry) {
    if (!context || !telemetry || index >= context->count) return false;
    IDXGIAdapter3 *adapter = (IDXGIAdapter3 *)context->adapters[index];
    if (!adapter) return false;

    telemetry->dxgi_available = true;
    if (!telemetry->name[0]) snprintf(telemetry->name, sizeof(telemetry->name), "%s", context->names[index]);
    if (!telemetry->memory_total) telemetry->memory_total = context->dedicated_memory[index];

    DXGI_QUERY_VIDEO_MEMORY_INFO information;
    memset(&information, 0, sizeof(information));
    HRESULT result = IDXGIAdapter3_QueryVideoMemoryInfo(adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &information);
    if (SUCCEEDED(result)) {
        telemetry->memory_budget = (uint64_t)information.Budget;
        if (!telemetry->nvml_available) {
            telemetry->memory_used = (uint64_t)information.CurrentUsage;
            telemetry->memory_free = information.Budget > information.CurrentUsage
                ? (uint64_t)(information.Budget - information.CurrentUsage) : 0;
            if (telemetry->memory_total == 0) telemetry->memory_total = (uint64_t)information.Budget;
        }
    }
    return true;
}
