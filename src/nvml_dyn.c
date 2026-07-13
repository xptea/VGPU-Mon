#define _CRT_SECURE_NO_WARNINGS
#include "nvml_dyn.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_SM 1
#define NVML_CLOCK_MEM 2
#define NVML_PCIE_UTIL_TX_BYTES 0
#define NVML_PCIE_UTIL_RX_BYTES 1

typedef struct {
    uint64_t total;
    uint64_t free;
    uint64_t used;
} NvmlMemory;

typedef struct {
    unsigned int version;
    uint64_t total;
    uint64_t reserved;
    uint64_t free;
    uint64_t used;
} NvmlMemoryV2;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} NvmlUtilization;

static FARPROC load_symbol(HMODULE library, const char *name) {
    return GetProcAddress(library, name);
}

static HMODULE load_nvml_library(void) {
    HMODULE library = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library) return library;

    PWSTR program_files = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT,
                                       NULL, &program_files)) && program_files) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _countof(path), _TRUNCATE,
                     L"%ls\\NVIDIA Corporation\\NVSMI\\nvml.dll", program_files);
        library = LoadLibraryExW(path, NULL,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (program_files) CoTaskMemFree(program_files);
    return library;
}

#define LOAD_REQUIRED(field, symbol) do { \
    FARPROC procedure = load_symbol(context->library, symbol); \
    memcpy(&context->field, &procedure, sizeof(context->field)); \
    if (!context->field) { \
        snprintf(context->error, sizeof(context->error), "NVML is missing %s", symbol); \
        nvml_close(context); \
        return false; \
    } \
} while (0)

#define LOAD_OPTIONAL(field, symbol) do { \
    FARPROC procedure = load_symbol(context->library, symbol); \
    memcpy(&context->field, &procedure, sizeof(context->field)); \
} while (0)

bool nvml_open(NvmlContext *context) {
    if (!context) return false;
    memset(context, 0, sizeof(*context));

    context->library = load_nvml_library();
    if (!context->library) {
        snprintf(context->error, sizeof(context->error),
                 "nvml.dll was not found in System32 or NVIDIA Corporation\\NVSMI");
        return false;
    }

    LOAD_REQUIRED(init_v2, "nvmlInit_v2");
    LOAD_REQUIRED(shutdown, "nvmlShutdown");
    LOAD_REQUIRED(device_get_count_v2, "nvmlDeviceGetCount_v2");
    LOAD_REQUIRED(device_get_handle_by_index_v2, "nvmlDeviceGetHandleByIndex_v2");
    LOAD_REQUIRED(device_get_name, "nvmlDeviceGetName");
    LOAD_REQUIRED(device_get_memory_info, "nvmlDeviceGetMemoryInfo");
    LOAD_REQUIRED(device_get_utilization_rates, "nvmlDeviceGetUtilizationRates");

    LOAD_OPTIONAL(error_string, "nvmlErrorString");
    LOAD_OPTIONAL(device_get_memory_info_v2, "nvmlDeviceGetMemoryInfo_v2");
    LOAD_OPTIONAL(system_get_driver_version, "nvmlSystemGetDriverVersion");
    LOAD_OPTIONAL(device_get_uuid, "nvmlDeviceGetUUID");
    LOAD_OPTIONAL(device_get_temperature, "nvmlDeviceGetTemperature");
    LOAD_OPTIONAL(device_get_fan_speed, "nvmlDeviceGetFanSpeed");
    LOAD_OPTIONAL(device_get_power_usage, "nvmlDeviceGetPowerUsage");
    LOAD_OPTIONAL(device_get_power_limit, "nvmlDeviceGetPowerManagementLimit");
    LOAD_OPTIONAL(device_get_clock_info, "nvmlDeviceGetClockInfo");
    LOAD_OPTIONAL(device_get_max_clock_info, "nvmlDeviceGetMaxClockInfo");
    LOAD_OPTIONAL(device_get_performance_state, "nvmlDeviceGetPerformanceState");
    LOAD_OPTIONAL(device_get_encoder_utilization, "nvmlDeviceGetEncoderUtilization");
    LOAD_OPTIONAL(device_get_decoder_utilization, "nvmlDeviceGetDecoderUtilization");
    LOAD_OPTIONAL(device_get_curr_pcie_generation, "nvmlDeviceGetCurrPcieLinkGeneration");
    LOAD_OPTIONAL(device_get_curr_pcie_width, "nvmlDeviceGetCurrPcieLinkWidth");
    LOAD_OPTIONAL(device_get_pcie_throughput, "nvmlDeviceGetPcieThroughput");

    int result = context->init_v2();
    if (result != NVML_SUCCESS) {
        const char *description = context->error_string ? context->error_string(result) : "unknown error";
        snprintf(context->error, sizeof(context->error), "NVML initialization failed: %s (%d)", description, result);
        FreeLibrary(context->library);
        context->library = NULL;
        return false;
    }

    context->initialized = true;
    result = context->device_get_count_v2(&context->device_count);
    if (result != NVML_SUCCESS) {
        snprintf(context->error, sizeof(context->error), "NVML could not enumerate GPUs (%d)", result);
        nvml_close(context);
        return false;
    }
    if (context->device_count > VGPU_MAX_GPUS) context->device_count = VGPU_MAX_GPUS;
    if (context->system_get_driver_version) {
        context->system_get_driver_version(context->driver_version, (unsigned int)sizeof(context->driver_version));
    }
    return true;
}

void nvml_close(NvmlContext *context) {
    if (!context) return;
    if (context->initialized && context->shutdown) context->shutdown();
    context->initialized = false;
    if (context->library) FreeLibrary(context->library);
    context->library = NULL;
}

static int read_uint(NvmlDevice device,
                     int (__cdecl *function)(NvmlDevice, unsigned int *), int unavailable) {
    unsigned int value = 0;
    if (!function || function(device, &value) != NVML_SUCCESS) return unavailable;
    return (int)value;
}

bool nvml_sample(NvmlContext *context, unsigned int index, GpuTelemetry *telemetry) {
    if (!context || !context->initialized || !telemetry || index >= context->device_count) return false;
    NvmlDevice device = NULL;
    if (context->device_get_handle_by_index_v2(index, &device) != NVML_SUCCESS) return false;

    /* Reset every optional sensor before querying so stale values can never
       survive a transient driver or capability failure. */
    telemetry->nvml_available = true;
    telemetry->index = index;
    telemetry->temperature_c = -1;
    telemetry->fan_percent = -1;
    telemetry->power_w = -1.0;
    telemetry->power_limit_w = -1.0;
    telemetry->graphics_clock_mhz = -1;
    telemetry->memory_clock_mhz = -1;
    telemetry->sm_clock_mhz = -1;
    telemetry->max_graphics_clock_mhz = -1;
    telemetry->max_memory_clock_mhz = -1;
    telemetry->pstate = -1;
    telemetry->encoder_util = -1;
    telemetry->decoder_util = -1;
    telemetry->pcie_generation = -1;
    telemetry->pcie_width = -1;
    telemetry->pcie_tx_mib_s = -1.0;
    telemetry->pcie_rx_mib_s = -1.0;

    context->device_get_name(device, telemetry->name, (unsigned int)sizeof(telemetry->name));
    snprintf(telemetry->driver, sizeof(telemetry->driver), "%s", context->driver_version);
    if (context->device_get_uuid) context->device_get_uuid(device, telemetry->uuid, (unsigned int)sizeof(telemetry->uuid));

    /* Prefer memory v2 because it separates driver-reserved allocation, then
       fall back to the older API exposed by legacy NVIDIA drivers. */
    bool memory_v2_ok = false;
    if (context->device_get_memory_info_v2) {
        NvmlMemoryV2 memory_v2;
        memset(&memory_v2, 0, sizeof(memory_v2));
        memory_v2.version = (unsigned int)(sizeof(memory_v2) | (2U << 24U));
        if (context->device_get_memory_info_v2(device, &memory_v2) == NVML_SUCCESS) {
            telemetry->memory_total = memory_v2.total;
            telemetry->memory_free = memory_v2.free;
            telemetry->memory_reserved = memory_v2.reserved;
            telemetry->memory_used = memory_v2.used;
            telemetry->memory_budget = memory_v2.total;
            memory_v2_ok = true;
        }
    }
    if (!memory_v2_ok) {
        NvmlMemory memory;
        if (context->device_get_memory_info(device, &memory) == NVML_SUCCESS) {
            telemetry->memory_total = memory.total;
            telemetry->memory_free = memory.free;
            telemetry->memory_used = memory.used;
            telemetry->memory_budget = memory.total;
        }
    }

    NvmlUtilization utilization;
    if (context->device_get_utilization_rates(device, &utilization) == NVML_SUCCESS) {
        telemetry->gpu_util = utilization.gpu;
        telemetry->memory_util = utilization.memory;
    }

    /* Optional board sensors are independent; one unsupported call must not
       suppress the rest of the snapshot. */
    unsigned int value = 0;
    if (context->device_get_temperature &&
        context->device_get_temperature(device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) {
        telemetry->temperature_c = (int)value;
    }
    telemetry->fan_percent = read_uint(device, context->device_get_fan_speed, -1);
    if (context->device_get_power_usage && context->device_get_power_usage(device, &value) == NVML_SUCCESS)
        telemetry->power_w = value / 1000.0;
    if (context->device_get_power_limit && context->device_get_power_limit(device, &value) == NVML_SUCCESS)
        telemetry->power_limit_w = value / 1000.0;

    if (context->device_get_clock_info) {
        if (context->device_get_clock_info(device, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS)
            telemetry->graphics_clock_mhz = (int)value;
        if (context->device_get_clock_info(device, NVML_CLOCK_SM, &value) == NVML_SUCCESS)
            telemetry->sm_clock_mhz = (int)value;
        if (context->device_get_clock_info(device, NVML_CLOCK_MEM, &value) == NVML_SUCCESS)
            telemetry->memory_clock_mhz = (int)value;
    }
    if (context->device_get_max_clock_info) {
        if (context->device_get_max_clock_info(device, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS)
            telemetry->max_graphics_clock_mhz = (int)value;
        if (context->device_get_max_clock_info(device, NVML_CLOCK_MEM, &value) == NVML_SUCCESS)
            telemetry->max_memory_clock_mhz = (int)value;
    }
    if (context->device_get_performance_state) context->device_get_performance_state(device, &telemetry->pstate);

    unsigned int sampling_period = 0;
    if (context->device_get_encoder_utilization &&
        context->device_get_encoder_utilization(device, &value, &sampling_period) == NVML_SUCCESS)
        telemetry->encoder_util = (int)value;
    if (context->device_get_decoder_utilization &&
        context->device_get_decoder_utilization(device, &value, &sampling_period) == NVML_SUCCESS)
        telemetry->decoder_util = (int)value;

    telemetry->pcie_generation = read_uint(device, context->device_get_curr_pcie_generation, -1);
    telemetry->pcie_width = read_uint(device, context->device_get_curr_pcie_width, -1);
    if (context->device_get_pcie_throughput) {
        if (context->device_get_pcie_throughput(device, NVML_PCIE_UTIL_TX_BYTES, &value) == NVML_SUCCESS)
            telemetry->pcie_tx_mib_s = value / 1024.0;
        if (context->device_get_pcie_throughput(device, NVML_PCIE_UTIL_RX_BYTES, &value) == NVML_SUCCESS)
            telemetry->pcie_rx_mib_s = value / 1024.0;
    }
    return true;
}
