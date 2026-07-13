#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <string.h>

#include "d3dkmt_gpu.h"

static bool load_functions(D3dkmtGpuContext *context) {
    context->open_adapter = GetProcAddress(context->library,
                                           "D3DKMTOpenAdapterFromLuid");
    context->close_adapter = GetProcAddress(context->library,
                                            "D3DKMTCloseAdapter");
    context->query_video_memory = GetProcAddress(context->library,
                                                 "D3DKMTQueryVideoMemoryInfo");
    return context->open_adapter && context->close_adapter &&
           context->query_video_memory;
}

static PFND3DKMT_OPENADAPTERFROMLUID open_adapter_function(
    const D3dkmtGpuContext *context) {
    PFND3DKMT_OPENADAPTERFROMLUID function = NULL;
    memcpy(&function, &context->open_adapter, sizeof(function));
    return function;
}

static PFND3DKMT_CLOSEADAPTER close_adapter_function(
    const D3dkmtGpuContext *context) {
    PFND3DKMT_CLOSEADAPTER function = NULL;
    memcpy(&function, &context->close_adapter, sizeof(function));
    return function;
}

static PFND3DKMT_QUERYVIDEOMEMORYINFO query_memory_function(
    const D3dkmtGpuContext *context) {
    PFND3DKMT_QUERYVIDEOMEMORYINFO function = NULL;
    memcpy(&function, &context->query_video_memory, sizeof(function));
    return function;
}

bool d3dkmt_gpu_open(D3dkmtGpuContext *context, const LUID *adapter_luids,
                     unsigned int adapter_count) {
    if (!context || !adapter_luids || adapter_count == 0) return false;
    memset(context, 0, sizeof(*context));
    context->library = LoadLibraryExW(L"gdi32.dll", NULL,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!context->library) {
        snprintf(context->error, sizeof(context->error),
                 "gdi32.dll could not be loaded (%lu)",
                 (unsigned long)GetLastError());
        return false;
    }
    if (!load_functions(context)) {
        snprintf(context->error, sizeof(context->error),
                 "Windows direct video-memory query APIs are unavailable");
        d3dkmt_gpu_close(context);
        return false;
    }

    PFND3DKMT_OPENADAPTERFROMLUID open_adapter =
        open_adapter_function(context);
    context->adapter_count = adapter_count > VGPU_MAX_GPUS
        ? VGPU_MAX_GPUS : adapter_count;
    for (unsigned int i = 0; i < context->adapter_count; ++i) {
        D3DKMT_OPENADAPTERFROMLUID request;
        memset(&request, 0, sizeof(request));
        request.AdapterLuid = adapter_luids[i];
        if (open_adapter(&request) >= 0) {
            context->adapter_handles[i] = request.hAdapter;
            context->adapter_open[i] = true;
            context->initialized = true;
        }
    }
    if (!context->initialized) {
        snprintf(context->error, sizeof(context->error),
                 "Windows could not open a direct video-memory adapter");
        d3dkmt_gpu_close(context);
        return false;
    }
    return true;
}

void d3dkmt_gpu_close(D3dkmtGpuContext *context) {
    if (!context) return;
    if (context->close_adapter) {
        PFND3DKMT_CLOSEADAPTER close_adapter = close_adapter_function(context);
        for (unsigned int i = 0; i < context->adapter_count; ++i) {
            if (!context->adapter_open[i]) continue;
            D3DKMT_CLOSEADAPTER request;
            memset(&request, 0, sizeof(request));
            request.hAdapter = context->adapter_handles[i];
            close_adapter(&request);
            context->adapter_open[i] = false;
            context->adapter_handles[i] = 0;
        }
    }
    if (context->library) FreeLibrary(context->library);
    context->library = NULL;
    context->open_adapter = NULL;
    context->close_adapter = NULL;
    context->query_video_memory = NULL;
    context->adapter_count = 0;
    context->initialized = false;
}

static bool query_process_group(const D3dkmtGpuContext *context,
                                unsigned int adapter_index, HANDLE process,
                                D3DKMT_MEMORY_SEGMENT_GROUP group,
                                uint64_t *usage) {
    D3DKMT_QUERYVIDEOMEMORYINFO request;
    memset(&request, 0, sizeof(request));
    request.hProcess = process;
    request.hAdapter = context->adapter_handles[adapter_index];
    request.MemorySegmentGroup = group;
    request.PhysicalAdapterIndex = 0;
    PFND3DKMT_QUERYVIDEOMEMORYINFO query = query_memory_function(context);
    NTSTATUS status = query(&request);
    if (status < 0) return false;
    *usage = (uint64_t)request.CurrentUsage;
    return true;
}

size_t d3dkmt_gpu_enrich_process_memory(D3dkmtGpuContext *context,
                                        unsigned int adapter_index,
                                        GpuProcess *processes, size_t count) {
    if (!context || !context->initialized || !processes ||
        adapter_index >= context->adapter_count ||
        !context->adapter_open[adapter_index]) return 0;
    size_t direct_count = 0;
    for (size_t i = 0; i < count; ++i) {
        GpuProcess *process_row = &processes[i];
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                     process_row->pid);
        if (!process) continue;
        uint64_t local = 0;
        uint64_t non_local = 0;
        bool local_ok = query_process_group(
            context, adapter_index, process,
            D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        bool non_local_ok = query_process_group(
            context, adapter_index, process,
            D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local);
        CloseHandle(process);
        if (local_ok) {
            process_row->dedicated_bytes = local;
            process_row->dedicated_memory_direct = true;
            direct_count++;
        }
        if (non_local_ok) {
            process_row->shared_bytes = non_local;
            process_row->shared_memory_direct = true;
        }
    }
    return direct_count;
}
