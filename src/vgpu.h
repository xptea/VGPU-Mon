#ifndef VGPU_H
#define VGPU_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "version.h"

#define VGPU_MAX_GPUS 16
#define VGPU_MAX_PROCESSES 4096
#define VGPU_HISTORY_SIZE 72
#define VGPU_CHART_HISTORY_SIZE 512

typedef struct {
    unsigned int index;
    char name[128];
    char driver[64];
    char uuid[96];
    uint64_t memory_total;
    uint64_t memory_used;
    uint64_t memory_free;
    uint64_t memory_reserved;
    uint64_t memory_budget;
    double gpu_util;
    double wddm_gpu_util;
    double memory_util;
    int temperature_c;
    int fan_percent;
    double power_w;
    double power_limit_w;
    int graphics_clock_mhz;
    int memory_clock_mhz;
    int sm_clock_mhz;
    int max_graphics_clock_mhz;
    int max_memory_clock_mhz;
    int pstate;
    int encoder_util;
    int decoder_util;
    int pcie_generation;
    int pcie_width;
    double pcie_tx_mib_s;
    double pcie_rx_mib_s;
    bool nvml_available;
    bool dxgi_available;
} GpuTelemetry;

typedef struct {
    DWORD pid;
    char name[260];
    double gpu_percent;
    uint64_t dedicated_bytes;
    uint64_t shared_bytes;
    char engine[48];
} GpuProcess;

typedef struct {
    double three_d;
    double copy;
    double video_decode;
    double video_encode;
    double compute;
    double other;
} GpuEngineStats;

typedef struct {
    char path[1024];
    char user[256];
    unsigned int threads;
    uint64_t working_set;
    uint64_t private_bytes;
    char started[64];
    bool accessible;
} ProcessDetails;

typedef enum {
    SORT_GPU,
    SORT_DEDICATED,
    SORT_SHARED,
    SORT_PID,
    SORT_NAME,
    SORT_ENGINE
} SortMode;

const char *format_bytes(uint64_t bytes, char *buffer, size_t buffer_size);
const char *format_rate(double mib_s, char *buffer, size_t buffer_size);
void wide_to_utf8(const wchar_t *source, char *target, size_t target_size);
void sanitize_csv_field(const char *source, char *target, size_t target_size);
bool contains_case_insensitive(const char *haystack, const char *needle);
void iso_timestamp(char *buffer, size_t buffer_size);
bool query_process_details(DWORD pid, ProcessDetails *details);
bool terminate_process_safely(DWORD pid, char *message, size_t message_size);

#endif
