#ifndef NVML_DYN_H
#define NVML_DYN_H

#include "vgpu.h"

typedef void *NvmlDevice;

typedef struct {
    HMODULE library;
    bool initialized;
    unsigned int device_count;
    char driver_version[64];
    char error[256];

    int (__cdecl *init_v2)(void);
    int (__cdecl *shutdown)(void);
    const char *(__cdecl *error_string)(int);
    int (__cdecl *system_get_driver_version)(char *, unsigned int);
    int (__cdecl *device_get_count_v2)(unsigned int *);
    int (__cdecl *device_get_handle_by_index_v2)(unsigned int, NvmlDevice *);
    int (__cdecl *device_get_name)(NvmlDevice, char *, unsigned int);
    int (__cdecl *device_get_uuid)(NvmlDevice, char *, unsigned int);
    int (__cdecl *device_get_memory_info)(NvmlDevice, void *);
    int (__cdecl *device_get_memory_info_v2)(NvmlDevice, void *);
    int (__cdecl *device_get_utilization_rates)(NvmlDevice, void *);
    int (__cdecl *device_get_temperature)(NvmlDevice, int, unsigned int *);
    int (__cdecl *device_get_fan_speed)(NvmlDevice, unsigned int *);
    int (__cdecl *device_get_power_usage)(NvmlDevice, unsigned int *);
    int (__cdecl *device_get_power_limit)(NvmlDevice, unsigned int *);
    int (__cdecl *device_get_clock_info)(NvmlDevice, int, unsigned int *);
    int (__cdecl *device_get_max_clock_info)(NvmlDevice, int, unsigned int *);
    int (__cdecl *device_get_performance_state)(NvmlDevice, int *);
    int (__cdecl *device_get_encoder_utilization)(NvmlDevice, unsigned int *, unsigned int *);
    int (__cdecl *device_get_decoder_utilization)(NvmlDevice, unsigned int *, unsigned int *);
    int (__cdecl *device_get_curr_pcie_generation)(NvmlDevice, unsigned int *);
    int (__cdecl *device_get_curr_pcie_width)(NvmlDevice, unsigned int *);
    int (__cdecl *device_get_pcie_throughput)(NvmlDevice, int, unsigned int *);
} NvmlContext;

bool nvml_open(NvmlContext *context);
void nvml_close(NvmlContext *context);
bool nvml_sample(NvmlContext *context, unsigned int index, GpuTelemetry *telemetry);

#endif
