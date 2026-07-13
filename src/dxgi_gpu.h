#ifndef DXGI_GPU_H
#define DXGI_GPU_H

#include "vgpu.h"

typedef struct {
    void *factory;
    void *adapters[VGPU_MAX_GPUS];
    unsigned int count;
    char names[VGPU_MAX_GPUS][128];
    LUID adapter_luids[VGPU_MAX_GPUS];
    uint64_t dedicated_memory[VGPU_MAX_GPUS];
    unsigned int vendor_ids[VGPU_MAX_GPUS];
    char error[256];
} DxgiContext;

bool dxgi_open(DxgiContext *context);
void dxgi_close(DxgiContext *context);
bool dxgi_sample(DxgiContext *context, unsigned int index, GpuTelemetry *telemetry);

#endif
