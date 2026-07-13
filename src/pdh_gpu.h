#ifndef PDH_GPU_H
#define PDH_GPU_H

#include "vgpu.h"
#include <pdh.h>
#include <pdhmsg.h>

typedef struct {
    PDH_HQUERY query;
    PDH_HCOUNTER engine_counter;
    PDH_HCOUNTER dedicated_counter;
    PDH_HCOUNTER shared_counter;
    bool initialized;
    bool primed;
    char error[256];
} PdhGpuContext;

bool pdh_gpu_open(PdhGpuContext *context);
void pdh_gpu_close(PdhGpuContext *context);
bool pdh_gpu_prime(PdhGpuContext *context);
bool pdh_gpu_sample(PdhGpuContext *context, unsigned int physical_gpu,
                    GpuProcess *processes, size_t capacity, size_t *count,
                    double *overall_gpu_percent, GpuEngineStats *engine_stats);
bool parse_gpu_instance(const wchar_t *instance, DWORD *pid, unsigned int *physical_gpu,
                        wchar_t *engine, size_t engine_capacity);

#endif
