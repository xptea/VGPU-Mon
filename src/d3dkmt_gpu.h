#ifndef D3DKMT_GPU_H
#define D3DKMT_GPU_H

#include "vgpu.h"

typedef struct {
    HMODULE library;
    FARPROC open_adapter;
    FARPROC close_adapter;
    FARPROC query_video_memory;
    unsigned int adapter_handles[VGPU_MAX_GPUS];
    bool adapter_open[VGPU_MAX_GPUS];
    unsigned int adapter_count;
    bool initialized;
    char error[256];
} D3dkmtGpuContext;

bool d3dkmt_gpu_open(D3dkmtGpuContext *context, const LUID *adapter_luids,
                     unsigned int adapter_count);
void d3dkmt_gpu_close(D3dkmtGpuContext *context);
size_t d3dkmt_gpu_enrich_process_memory(D3dkmtGpuContext *context,
                                        unsigned int adapter_index,
                                        GpuProcess *processes, size_t count);

#endif
