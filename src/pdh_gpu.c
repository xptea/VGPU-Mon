#define _CRT_SECURE_NO_WARNINGS
#include "pdh_gpu.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool add_counter(PDH_HQUERY query, const wchar_t *path, PDH_HCOUNTER *counter) {
    PDH_STATUS status = PdhAddEnglishCounterW(query, path, 0, counter);
    return status == ERROR_SUCCESS;
}

bool pdh_gpu_open(PdhGpuContext *context) {
    if (!context) return false;
    memset(context, 0, sizeof(*context));
    PDH_STATUS status = PdhOpenQueryW(NULL, 0, &context->query);
    if (status != ERROR_SUCCESS) {
        snprintf(context->error, sizeof(context->error), "PdhOpenQuery failed (0x%08lx)", (unsigned long)status);
        return false;
    }

    bool engine = add_counter(context->query,
                              L"\\GPU Engine(*)\\Utilization Percentage",
                              &context->engine_counter);
    bool dedicated = add_counter(context->query,
                                 L"\\GPU Process Memory(*)\\Dedicated Usage",
                                 &context->dedicated_counter);
    bool shared = add_counter(context->query,
                              L"\\GPU Process Memory(*)\\Shared Usage",
                              &context->shared_counter);
    if (!engine && !dedicated && !shared) {
        snprintf(context->error, sizeof(context->error),
                 "Windows GPU performance counters are unavailable");
        pdh_gpu_close(context);
        return false;
    }
    context->initialized = true;
    if (!pdh_gpu_prime(context)) {
        pdh_gpu_close(context);
        return false;
    }
    return true;
}

void pdh_gpu_close(PdhGpuContext *context) {
    if (!context) return;
    if (context->query) PdhCloseQuery(context->query);
    context->query = NULL;
    context->engine_counter = NULL;
    context->dedicated_counter = NULL;
    context->shared_counter = NULL;
    context->initialized = false;
    context->primed = false;
}

bool pdh_gpu_prime(PdhGpuContext *context) {
    if (!context || !context->initialized) return false;
    PDH_STATUS status = PdhCollectQueryData(context->query);
    context->primed = status == ERROR_SUCCESS;
    if (!context->primed) {
        snprintf(context->error, sizeof(context->error), "PdhCollectQueryData failed (0x%08lx)", (unsigned long)status);
    }
    return context->primed;
}

bool parse_gpu_instance(const wchar_t *instance, DWORD *pid, unsigned int *physical_gpu,
                        wchar_t *engine, size_t engine_capacity) {
    if (!instance || !pid || !physical_gpu) return false;
    const wchar_t *pid_text = wcsstr(instance, L"pid_");
    const wchar_t *physical_text = wcsstr(instance, L"_phys_");
    if (!pid_text || !physical_text) return false;

    wchar_t *end = NULL;
    unsigned long parsed_pid = wcstoul(pid_text + 4, &end, 10);
    if (end == pid_text + 4) return false;
    unsigned long parsed_physical = wcstoul(physical_text + 6, &end, 10);
    if (end == physical_text + 6) return false;
    *pid = (DWORD)parsed_pid;
    *physical_gpu = (unsigned int)parsed_physical;

    if (engine && engine_capacity) {
        engine[0] = L'\0';
        const wchar_t *engine_text = wcsstr(instance, L"_engtype_");
        if (engine_text) {
            engine_text += 9;
            wcsncpy_s(engine, engine_capacity, engine_text, _TRUNCATE);
        }
    }
    return true;
}

static GpuProcess *find_or_add(GpuProcess *processes, size_t capacity, size_t *count, DWORD pid) {
    for (size_t i = 0; i < *count; ++i) {
        if (processes[i].pid == pid) return &processes[i];
    }
    if (*count >= capacity) return NULL;
    GpuProcess *process = &processes[(*count)++];
    memset(process, 0, sizeof(*process));
    process->pid = pid;
    snprintf(process->name, sizeof(process->name), "PID %lu", (unsigned long)pid);
    snprintf(process->engine, sizeof(process->engine), "-");
    return process;
}

static bool read_engine_counter(PDH_HCOUNTER counter, unsigned int selected_gpu,
                                GpuProcess *processes, size_t capacity, size_t *count,
                                double *overall_gpu_percent, GpuEngineStats *engine_stats) {
    if (!counter) return false;
    DWORD bytes = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                     &bytes, &item_count, NULL);
    if (status != PDH_MORE_DATA || bytes == 0) return false;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(bytes);
    if (!items) return false;
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                          &bytes, &item_count, items);
    if (status != ERROR_SUCCESS) {
        free(items);
        return false;
    }

    double engine_totals[256] = {0};
    char engine_types[256][48] = {{0}};
    for (DWORD i = 0; i < item_count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        DWORD pid = 0;
        unsigned int physical = 0;
        wchar_t engine[64];
        if (!parse_gpu_instance(items[i].szName, &pid, &physical, engine, _countof(engine))) continue;
        if (physical != selected_gpu || pid == 0) continue;
        GpuProcess *process = find_or_add(processes, capacity, count, pid);
        if (!process) break;
        double utilization = items[i].FmtValue.doubleValue;
        if (utilization < 0.0) utilization = 0.0;
        const wchar_t *engine_marker = wcsstr(items[i].szName, L"_eng_");
        if (engine_marker) {
            wchar_t *end = NULL;
            unsigned long engine_index = wcstoul(engine_marker + 5, &end, 10);
            if (end != engine_marker + 5 && engine_index < _countof(engine_totals)) {
                engine_totals[engine_index] += utilization;
                if (!engine_types[engine_index][0] && engine[0])
                    wide_to_utf8(engine, engine_types[engine_index], sizeof(engine_types[engine_index]));
            }
        }
        if (utilization > process->gpu_percent) {
            process->gpu_percent = utilization;
            if (engine[0]) wide_to_utf8(engine, process->engine, sizeof(process->engine));
        }
    }
    if (overall_gpu_percent) {
        *overall_gpu_percent = 0.0;
        for (size_t i = 0; i < _countof(engine_totals); ++i) {
            if (engine_totals[i] > *overall_gpu_percent) *overall_gpu_percent = engine_totals[i];
        }
        if (*overall_gpu_percent > 100.0) *overall_gpu_percent = 100.0;
    }
    if (engine_stats) {
        memset(engine_stats, 0, sizeof(*engine_stats));
        for (size_t i = 0; i < _countof(engine_totals); ++i) {
            double value = engine_totals[i] > 100.0 ? 100.0 : engine_totals[i];
            double *destination = &engine_stats->other;
            if (_stricmp(engine_types[i], "3D") == 0) destination = &engine_stats->three_d;
            else if (_stricmp(engine_types[i], "Copy") == 0) destination = &engine_stats->copy;
            else if (_stricmp(engine_types[i], "VideoDecode") == 0) destination = &engine_stats->video_decode;
            else if (_stricmp(engine_types[i], "VideoEncode") == 0) destination = &engine_stats->video_encode;
            else if (contains_case_insensitive(engine_types[i], "compute")) destination = &engine_stats->compute;
            if (value > *destination) *destination = value;
        }
    }
    free(items);
    return true;
}

static bool read_memory_counter(PDH_HCOUNTER counter, bool dedicated, unsigned int selected_gpu,
                                GpuProcess *processes, size_t capacity, size_t *count) {
    if (!counter) return false;
    DWORD bytes = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE,
                                                     &bytes, &item_count, NULL);
    if (status != PDH_MORE_DATA || bytes == 0) return false;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(bytes);
    if (!items) return false;
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE,
                                          &bytes, &item_count, items);
    if (status != ERROR_SUCCESS) {
        free(items);
        return false;
    }

    for (DWORD i = 0; i < item_count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        DWORD pid = 0;
        unsigned int physical = 0;
        if (!parse_gpu_instance(items[i].szName, &pid, &physical, NULL, 0)) continue;
        if (physical != selected_gpu || pid == 0) continue;
        GpuProcess *process = find_or_add(processes, capacity, count, pid);
        if (!process) break;
        LONGLONG raw = items[i].FmtValue.largeValue;
        uint64_t value = raw > 0 ? (uint64_t)raw : 0;
        if (dedicated) {
            if (value > process->dedicated_bytes) process->dedicated_bytes = value;
        } else {
            if (value > process->shared_bytes) process->shared_bytes = value;
        }
    }
    free(items);
    return true;
}

static void resolve_process_names(GpuProcess *processes, size_t count) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            for (size_t i = 0; i < count; ++i) {
                if (processes[i].pid == entry.th32ProcessID) {
                    wide_to_utf8(entry.szExeFile, processes[i].name, sizeof(processes[i].name));
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool pdh_gpu_sample(PdhGpuContext *context, unsigned int physical_gpu,
                    GpuProcess *processes, size_t capacity, size_t *count,
                    double *overall_gpu_percent, GpuEngineStats *engine_stats) {
    if (!context || !context->initialized || !processes || !count || capacity == 0) return false;
    *count = 0;
    if (overall_gpu_percent) *overall_gpu_percent = 0.0;
    if (engine_stats) memset(engine_stats, 0, sizeof(*engine_stats));
    memset(processes, 0, sizeof(*processes) * capacity);

    PDH_STATUS status = PdhCollectQueryData(context->query);
    if (status != ERROR_SUCCESS) {
        snprintf(context->error, sizeof(context->error), "PdhCollectQueryData failed (0x%08lx)", (unsigned long)status);
        return false;
    }

    bool any = false;
    any |= read_engine_counter(context->engine_counter, physical_gpu, processes, capacity, count,
                               overall_gpu_percent, engine_stats);
    any |= read_memory_counter(context->dedicated_counter, true, physical_gpu, processes, capacity, count);
    any |= read_memory_counter(context->shared_counter, false, physical_gpu, processes, capacity, count);
    resolve_process_names(processes, *count);
    return any;
}
