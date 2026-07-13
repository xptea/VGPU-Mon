#define _CRT_SECURE_NO_WARNINGS
#include "vgpu.h"
#include "nvml_dyn.h"
#include "dxgi_gpu.h"
#include "d3dkmt_gpu.h"
#include "pdh_gpu.h"
#include "updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#define ANSI_RESET "\x1b[0m"
#define ANSI_BOLD "\x1b[1m"
#define ANSI_DIM "\x1b[2m"
#define ANSI_GREEN "\x1b[38;5;82m"
#define ANSI_YELLOW "\x1b[38;5;220m"
#define ANSI_RED "\x1b[38;5;203m"
#define ANSI_CYAN "\x1b[38;5;45m"
#define ANSI_BLUE "\x1b[38;5;75m"
#define ANSI_REVERSE "\x1b[7m"

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int row;
} TextBuffer;

typedef enum {
    CHART_GPU,
    CHART_VRAM,
    CHART_MEMORY_CONTROLLER,
    CHART_3D,
    CHART_COPY,
    CHART_VIDEO_DECODE,
    CHART_VIDEO_ENCODE,
    CHART_COMPUTE,
    CHART_TEMPERATURE,
    CHART_POWER,
    CHART_METRIC_COUNT
} ChartMetric;

typedef struct {
    int start_x;
    int end_x;
    SortMode mode;
} HeaderHit;

typedef struct {
    SortMode mode;
    const char *label;
    int width;
    int start_x;
    bool right_aligned;
    bool visible;
} TableColumn;

typedef struct {
    TableColumn columns[6];
    size_t count;
    int indent;
    bool usable;
} TableLayout;

typedef struct {
    HANDLE input;
    HANDLE output;
    DWORD old_input_mode;
    DWORD old_output_mode;
    UINT old_input_cp;
    UINT old_output_cp;
    wchar_t old_title[512];
    bool active;
} ConsoleState;

typedef struct {
    NvmlContext nvml;
    DxgiContext dxgi;
    D3dkmtGpuContext d3dkmt;
    PdhGpuContext pdh;
    bool nvml_ready;
    bool dxgi_ready;
    bool d3dkmt_ready;
    bool pdh_ready;
    unsigned int gpu_count;
    unsigned int selected_gpu;
    unsigned int interval_ms;
    GpuTelemetry telemetry;
    GpuEngineStats engines;
    bool wddm_process_memory_suspect;
    size_t direct_process_memory_count;
    GpuProcess processes[VGPU_MAX_PROCESSES];
    size_t process_count;
    GpuProcess *visible[VGPU_MAX_PROCESSES];
    size_t visible_count;
    size_t selection;
    DWORD selected_pid;
    size_t scroll;
    SortMode sort_mode;
    bool sort_descending;
    char filter[128];
    char filter_draft[128];
    bool editing_filter;
    bool paused;
    bool show_help;
    bool show_gpu_info;
    bool show_details;
    bool chart_view;
    bool demo_mode;
    ChartMetric chart_metric;
    DWORD confirm_pid;
    char message[256];
    ULONGLONG message_until;
    double utilization_history[VGPU_HISTORY_SIZE];
    double memory_history[VGPU_HISTORY_SIZE];
    size_t history_count;
    size_t history_next;
    double chart_history[CHART_METRIC_COUNT][VGPU_CHART_HISTORY_SIZE];
    ULONGLONG chart_history_time[VGPU_CHART_HISTORY_SIZE];
    size_t chart_history_count;
    size_t chart_history_next;
    ULONGLONG chart_window_ms;
    ULONGLONG chart_pan_ms;
    bool chart_hover_active;
    int chart_hover_x;
    int chart_hover_y;
    int chart_plot_left;
    int chart_plot_right;
    int chart_plot_top;
    int chart_plot_bottom;
    double *chart_plot_values;
    unsigned char *chart_plot_present;
    size_t chart_plot_capacity;
    FILE *log_file;
    char log_path[MAX_PATH];
    ProcessDetails details_cache;
    DWORD details_pid;
    ULONGLONG details_sampled_at;
    int viewport_left;
    int viewport_top;
    int last_columns;
    int last_rows;
    int table_header_row;
    int table_first_process_row;
    int table_last_process_row;
    HeaderHit header_hits[6];
    size_t header_hit_count;
} App;

static volatile LONG g_interrupted = 0;
static SortMode g_sort_mode = SORT_GPU;
static bool g_sort_descending = true;

static BOOL WINAPI console_control_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE;
    }
    return FALSE;
}

static void text_buffer_init(TextBuffer *buffer, size_t capacity) {
    buffer->data = (char *)malloc(capacity);
    buffer->length = 0;
    buffer->capacity = buffer->data ? capacity : 0;
    buffer->row = 0;
    if (buffer->data) buffer->data[0] = '\0';
}

static void text_buffer_free(TextBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void text_buffer_append(TextBuffer *buffer, const char *format, ...) {
    if (!buffer->data || buffer->length >= buffer->capacity) return;
    size_t old_length = buffer->length;
    va_list arguments;
    va_start(arguments, format);
    int written = _vsnprintf_s(buffer->data + buffer->length,
                               buffer->capacity - buffer->length, _TRUNCATE,
                               format, arguments);
    va_end(arguments);
    if (written > 0) buffer->length += (size_t)written;
    else buffer->length = strlen(buffer->data);
    for (size_t i = old_length; i < buffer->length; ++i) {
        if (buffer->data[i] == '\n') buffer->row++;
    }
}

static void text_buffer_reset(TextBuffer *buffer) {
    buffer->length = 0;
    buffer->row = 0;
    if (buffer->data) buffer->data[0] = '\0';
}

static void text_buffer_append_span(TextBuffer *buffer, const char *data, size_t length) {
    if (!buffer->data || !data || length == 0 || buffer->length >= buffer->capacity - 1) return;
    size_t available = buffer->capacity - buffer->length - 1;
    if (length > available) length = available;
    memcpy(buffer->data + buffer->length, data, length);
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n') buffer->row++;
    }
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void compose_terminal_frame(const TextBuffer *frame, TextBuffer *output, int rows) {
    text_buffer_reset(output);
    if (rows < 1) rows = 1;
    text_buffer_append(output, "\x1b[H");

    size_t start = 0;
    int rendered_rows = 0;
    while (start < frame->length && rendered_rows < rows) {
        const char *line_start = frame->data + start;
        const char *newline = (const char *)memchr(line_start, '\n', frame->length - start);
        size_t length = newline ? (size_t)(newline - line_start) : frame->length - start;
        text_buffer_append_span(output, line_start, length);
        text_buffer_append(output, ANSI_RESET "\x1b[K");
        rendered_rows++;
        if (!newline || rendered_rows >= rows) break;
        text_buffer_append(output, "\n");
        start += length + 1;
    }
    if (frame->length == 0) text_buffer_append(output, ANSI_RESET "\x1b[K");
    text_buffer_append(output, ANSI_RESET "\x1b[J");
}

static void reset_telemetry(GpuTelemetry *telemetry, unsigned int index) {
    memset(telemetry, 0, sizeof(*telemetry));
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
}

static int compare_processes(const void *left_ptr, const void *right_ptr) {
    const GpuProcess *left = *(GpuProcess * const *)left_ptr;
    const GpuProcess *right = *(GpuProcess * const *)right_ptr;
    int result = 0;
    switch (g_sort_mode) {
        case SORT_GPU:
            result = left->gpu_percent < right->gpu_percent ? -1 : left->gpu_percent > right->gpu_percent ? 1 : 0;
            break;
        case SORT_DEDICATED:
            result = left->dedicated_bytes < right->dedicated_bytes ? -1 : left->dedicated_bytes > right->dedicated_bytes ? 1 : 0;
            break;
        case SORT_SHARED:
            result = left->shared_bytes < right->shared_bytes ? -1 : left->shared_bytes > right->shared_bytes ? 1 : 0;
            break;
        case SORT_PID:
            result = left->pid < right->pid ? -1 : left->pid > right->pid ? 1 : 0;
            break;
        case SORT_NAME:
            result = _stricmp(left->name, right->name);
            break;
        case SORT_ENGINE:
            result = _stricmp(left->engine, right->engine);
            break;
    }
    if (result == 0) result = left->pid < right->pid ? -1 : left->pid > right->pid ? 1 : 0;
    return g_sort_descending ? -result : result;
}

static void update_visible_processes(App *app) {
    app->visible_count = 0;
    for (size_t i = 0; i < app->process_count; ++i) {
        GpuProcess *process = &app->processes[i];
        if (contains_case_insensitive(process->name, app->filter)) {
            app->visible[app->visible_count++] = process;
        }
    }
    g_sort_mode = app->sort_mode;
    g_sort_descending = app->sort_descending;
    qsort(app->visible, app->visible_count, sizeof(app->visible[0]), compare_processes);

    if (app->selected_pid) {
        for (size_t i = 0; i < app->visible_count; ++i) {
            if (app->visible[i]->pid == app->selected_pid) {
                app->selection = i;
                break;
            }
        }
    }
    if (app->visible_count == 0) {
        app->selection = 0;
        app->selected_pid = 0;
        app->scroll = 0;
    } else {
        if (app->selection >= app->visible_count) app->selection = app->visible_count - 1;
        app->selected_pid = app->visible[app->selection]->pid;
    }
}

static void add_history_sample(App *app) {
    ULONGLONG sampled_at = GetTickCount64();
    if (app->chart_history_count > 0 && app->chart_pan_ms > 0) {
        size_t newest = (app->chart_history_next + VGPU_CHART_HISTORY_SIZE - 1) %
                        VGPU_CHART_HISTORY_SIZE;
        ULONGLONG previous = app->chart_history_time[newest];
        if (sampled_at > previous) app->chart_pan_ms += sampled_at - previous;
    }

    app->utilization_history[app->history_next] = app->telemetry.gpu_util;
    double memory_percent = app->telemetry.memory_total
        ? (double)app->telemetry.memory_used * 100.0 / (double)app->telemetry.memory_total : 0.0;
    app->memory_history[app->history_next] = memory_percent;
    app->history_next = (app->history_next + 1) % VGPU_HISTORY_SIZE;
    if (app->history_count < VGPU_HISTORY_SIZE) app->history_count++;

    double values[CHART_METRIC_COUNT] = {0};
    values[CHART_GPU] = app->telemetry.gpu_util;
    values[CHART_VRAM] = memory_percent;
    values[CHART_MEMORY_CONTROLLER] = app->telemetry.memory_util;
    values[CHART_3D] = app->engines.three_d;
    values[CHART_COPY] = app->engines.copy;
    values[CHART_VIDEO_DECODE] = app->engines.video_decode;
    values[CHART_VIDEO_ENCODE] = app->engines.video_encode;
    values[CHART_COMPUTE] = app->engines.compute;
    values[CHART_TEMPERATURE] = app->telemetry.temperature_c >= 0 ? app->telemetry.temperature_c : 0.0;
    values[CHART_POWER] = app->telemetry.power_w >= 0 ? app->telemetry.power_w : 0.0;
    for (size_t i = 0; i < CHART_METRIC_COUNT; ++i)
        app->chart_history[i][app->chart_history_next] = values[i];
    app->chart_history_time[app->chart_history_next] = sampled_at;
    app->chart_history_next = (app->chart_history_next + 1) % VGPU_CHART_HISTORY_SIZE;
    if (app->chart_history_count < VGPU_CHART_HISTORY_SIZE) app->chart_history_count++;
}

static const char *chart_metric_name(ChartMetric metric) {
    switch (metric) {
        case CHART_GPU: return "Overall GPU utilization";
        case CHART_VRAM: return "Allocated VRAM";
        case CHART_MEMORY_CONTROLLER: return "Memory controller utilization";
        case CHART_3D: return "3D engine";
        case CHART_COPY: return "Copy engine";
        case CHART_VIDEO_DECODE: return "Video decode engine";
        case CHART_VIDEO_ENCODE: return "Video encode engine";
        case CHART_COMPUTE: return "Compute engine";
        case CHART_TEMPERATURE: return "GPU temperature";
        case CHART_POWER: return "Board power";
        default: return "Unknown metric";
    }
}

static const char *chart_metric_unit(ChartMetric metric) {
    if (metric == CHART_TEMPERATURE) return "C";
    if (metric == CHART_POWER) return "W";
    return "%";
}

static double current_chart_value(const App *app, ChartMetric metric) {
    if (app->chart_history_count == 0) return 0.0;
    size_t index = (app->chart_history_next + VGPU_CHART_HISTORY_SIZE - 1) % VGPU_CHART_HISTORY_SIZE;
    return app->chart_history[metric][index];
}

static bool chart_metric_available(const App *app, ChartMetric metric) {
    if (app->demo_mode) return true;
    switch (metric) {
        case CHART_GPU: return app->telemetry.nvml_available || app->pdh_ready;
        case CHART_VRAM: return app->telemetry.memory_total > 0;
        case CHART_MEMORY_CONTROLLER: return app->telemetry.nvml_available;
        case CHART_3D:
        case CHART_COPY:
        case CHART_VIDEO_DECODE:
        case CHART_VIDEO_ENCODE:
        case CHART_COMPUTE: return app->pdh_ready;
        case CHART_TEMPERATURE: return app->telemetry.temperature_c >= 0;
        case CHART_POWER: return app->telemetry.power_w >= 0;
        default: return false;
    }
}

static size_t chart_oldest_index(const App *app) {
    return app->chart_history_count < VGPU_CHART_HISTORY_SIZE ? 0 : app->chart_history_next;
}

static size_t chart_ring_index(const App *app, size_t logical) {
    return (chart_oldest_index(app) + logical) % VGPU_CHART_HISTORY_SIZE;
}

static ULONGLONG chart_timestamp(const App *app, size_t logical) {
    return app->chart_history_time[chart_ring_index(app, logical)];
}

static double chart_value(const App *app, ChartMetric metric, size_t logical) {
    return app->chart_history[metric][chart_ring_index(app, logical)];
}

static ULONGLONG chart_max_window_ms(const App *app) {
    ULONGLONG maximum = (ULONGLONG)app->interval_ms * (ULONGLONG)VGPU_CHART_HISTORY_SIZE;
    ULONGLONG duration = 0;
    if (app->chart_history_count > 1) {
        duration = chart_timestamp(app, app->chart_history_count - 1) - chart_timestamp(app, 0);
    }
    if (duration > maximum) maximum = duration;
    return maximum < 10000ULL ? 10000ULL : maximum;
}

static void clamp_chart_window(App *app) {
    ULONGLONG minimum = (ULONGLONG)app->interval_ms * 2ULL;
    ULONGLONG maximum = chart_max_window_ms(app);
    if (minimum < 10000ULL) minimum = 10000ULL;
    if (minimum > maximum) minimum = maximum;
    if (app->chart_window_ms < minimum) app->chart_window_ms = minimum;
    if (app->chart_window_ms > maximum) app->chart_window_ms = maximum;
}

static ULONGLONG chart_max_pan_ms(const App *app) {
    if (app->chart_history_count < 2) return 0;
    ULONGLONG duration = chart_timestamp(app, app->chart_history_count - 1) - chart_timestamp(app, 0);
    return duration > app->chart_window_ms ? duration - app->chart_window_ms : 0;
}

static void clamp_chart_pan(App *app) {
    ULONGLONG maximum = chart_max_pan_ms(app);
    if (app->chart_pan_ms > maximum) app->chart_pan_ms = maximum;
}

static void format_chart_duration(ULONGLONG milliseconds, char *buffer, size_t buffer_size) {
    if (milliseconds < 1000ULL) {
        snprintf(buffer, buffer_size, "%.1fs", (double)milliseconds / 1000.0);
        return;
    }
    ULONGLONG seconds = milliseconds / 1000ULL;
    if (seconds < 60ULL) {
        snprintf(buffer, buffer_size, "%llus", (unsigned long long)seconds);
    } else if (seconds < 3600ULL) {
        snprintf(buffer, buffer_size, "%llum%02llus",
                 (unsigned long long)(seconds / 60ULL),
                 (unsigned long long)(seconds % 60ULL));
    } else {
        snprintf(buffer, buffer_size, "%lluh%02llum",
                 (unsigned long long)(seconds / 3600ULL),
                 (unsigned long long)((seconds % 3600ULL) / 60ULL));
    }
}

static void zoom_chart(App *app, bool zoom_out) {
    static const ULONGLONG windows[] = {
        10000ULL, 15000ULL, 30000ULL, 60000ULL, 120000ULL, 300000ULL,
        600000ULL, 1800000ULL, 3600000ULL, 7200000ULL, 14400000ULL,
        28800000ULL, 43200000ULL, 86400000ULL
    };
    clamp_chart_window(app);
    ULONGLONG minimum = (ULONGLONG)app->interval_ms * 2ULL;
    ULONGLONG maximum = chart_max_window_ms(app);
    if (minimum < 10000ULL) minimum = 10000ULL;
    if (minimum > maximum) minimum = maximum;
    ULONGLONG next = zoom_out ? maximum : minimum;

    if (zoom_out) {
        for (size_t i = 0; i < _countof(windows); ++i) {
            if (windows[i] > app->chart_window_ms && windows[i] <= maximum) {
                next = windows[i];
                break;
            }
        }
        if (maximum <= app->chart_window_ms) next = app->chart_window_ms;
    } else {
        for (size_t i = 0; i < _countof(windows); ++i) {
            if (windows[i] >= minimum && windows[i] < app->chart_window_ms) next = windows[i];
            if (windows[i] >= app->chart_window_ms) break;
        }
        if (minimum >= app->chart_window_ms) next = app->chart_window_ms;
    }
    app->chart_window_ms = next;
    clamp_chart_pan(app);
}

static void pan_chart(App *app, int direction, bool full_window) {
    clamp_chart_window(app);
    clamp_chart_pan(app);
    ULONGLONG step = full_window ? app->chart_window_ms : app->chart_window_ms / 4ULL;
    if (step < 1000ULL) step = 1000ULL;
    ULONGLONG maximum = chart_max_pan_ms(app);
    if (direction > 0) {
        app->chart_pan_ms = maximum - app->chart_pan_ms < step
            ? maximum : app->chart_pan_ms + step;
    } else {
        app->chart_pan_ms = app->chart_pan_ms < step ? 0 : app->chart_pan_ms - step;
    }
}

static bool ensure_chart_plot_capacity(App *app, size_t needed) {
    if (needed <= app->chart_plot_capacity) return true;
    if (needed > SIZE_MAX / sizeof(*app->chart_plot_values)) return false;
    double *values = (double *)malloc(needed * sizeof(*values));
    unsigned char *present = (unsigned char *)malloc(needed * sizeof(*present));
    if (!values || !present) {
        free(present);
        free(values);
        return false;
    }
    free(app->chart_plot_present);
    free(app->chart_plot_values);
    app->chart_plot_values = values;
    app->chart_plot_present = present;
    app->chart_plot_capacity = needed;
    return true;
}

static const char *dedicated_memory_source(const App *app,
                                           const GpuProcess *process) {
    if (app->demo_mode) return "demo";
    if (process->dedicated_memory_invalid) return "unavailable";
    return process->dedicated_memory_direct ? "d3dkmt" : "pdh";
}

static const char *shared_memory_source(const App *app,
                                        const GpuProcess *process) {
    if (app->demo_mode) return "demo";
    return process->shared_memory_direct ? "d3dkmt" : "pdh";
}

static void log_sample(App *app) {
    if (!app->log_file) return;
    char timestamp[64], gpu_name[256], process_name[520], engine[96];
    iso_timestamp(timestamp, sizeof(timestamp));
    sanitize_csv_field(app->telemetry.name, gpu_name, sizeof(gpu_name));
    size_t rows = app->process_count ? app->process_count : 1;
    for (size_t i = 0; i < rows; ++i) {
        GpuProcess *process = app->process_count ? &app->processes[i] : NULL;
        fprintf(app->log_file,
                "%s,%u,\"%s\",%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%llu,%llu,%llu,%d,%d,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,",
                timestamp, app->selected_gpu, gpu_name, app->telemetry.gpu_util,
                app->telemetry.wddm_gpu_util,
                app->engines.three_d, app->engines.copy,
                app->engines.video_decode, app->engines.video_encode,
                app->engines.compute, app->engines.other,
                app->telemetry.memory_util,
                (unsigned long long)app->telemetry.memory_used,
                (unsigned long long)app->telemetry.memory_total,
                (unsigned long long)app->telemetry.memory_reserved,
                (unsigned long long)app->telemetry.memory_budget,
                app->telemetry.temperature_c, app->telemetry.fan_percent,
                app->telemetry.power_w, app->telemetry.power_limit_w,
                app->telemetry.graphics_clock_mhz, app->telemetry.memory_clock_mhz,
                app->telemetry.pstate, app->telemetry.encoder_util, app->telemetry.decoder_util,
                app->telemetry.pcie_generation, app->telemetry.pcie_width,
                app->telemetry.pcie_tx_mib_s, app->telemetry.pcie_rx_mib_s);
        if (!process) {
            fputs(",,,,,,,\n", app->log_file);
            continue;
        }
        sanitize_csv_field(process->name, process_name, sizeof(process_name));
        sanitize_csv_field(process->engine, engine, sizeof(engine));
        fprintf(app->log_file, "%lu,\"%s\",%.2f,",
                (unsigned long)process->pid, process_name, process->gpu_percent);
        if (!process->dedicated_memory_invalid)
            fprintf(app->log_file, "%llu", (unsigned long long)process->dedicated_bytes);
        fprintf(app->log_file, ",%llu,\"%s\",%s,%s\n",
                (unsigned long long)process->shared_bytes, engine,
                dedicated_memory_source(app, process),
                shared_memory_source(app, process));
    }
    if (fflush(app->log_file) != 0 || ferror(app->log_file)) {
        fclose(app->log_file);
        app->log_file = NULL;
        snprintf(app->message, sizeof(app->message), "CSV logging stopped after a write error");
        app->message_until = GetTickCount64() + 5000;
    }
}

static bool start_logging(App *app, const char *path) {
    if (app->log_file) return true;
    snprintf(app->log_path, sizeof(app->log_path), "%s", path && *path ? path : "vgpu-mon.csv");
    wchar_t wide_path[MAX_PATH];
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, app->log_path, -1,
                                        wide_path, _countof(wide_path));
    int descriptor = -1;
    if (converted > 0 &&
        _wsopen_s(&descriptor, wide_path,
                  _O_APPEND | _O_CREAT | _O_BINARY | _O_WRONLY | _O_NOINHERIT,
                  _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0) {
        app->log_file = _wfdopen(descriptor, L"ab");
        if (!app->log_file) _close(descriptor);
    }
    if (!app->log_file) {
        snprintf(app->message, sizeof(app->message), "Cannot open log: %s", app->log_path);
        app->message_until = GetTickCount64() + 4000;
        return false;
    }
    fseek(app->log_file, 0, SEEK_END);
    if (ftell(app->log_file) == 0) {
        fputs("timestamp,gpu_index,gpu_name,gpu_percent,wddm_busiest_engine_percent,wddm_3d_percent,wddm_copy_percent,wddm_video_decode_percent,wddm_video_encode_percent,wddm_compute_percent,wddm_other_percent,memory_controller_percent,vram_used_bytes,vram_total_bytes,vram_reserved_bytes,memory_budget_bytes,temperature_c,fan_percent,power_w,power_limit_w,graphics_clock_mhz,memory_clock_mhz,pstate,encoder_percent,decoder_percent,pcie_generation,pcie_width,pcie_tx_mib_s,pcie_rx_mib_s,pid,process,process_gpu_percent,dedicated_commit_bytes,shared_commit_bytes,engine,dedicated_memory_source,shared_memory_source\n",
              app->log_file);
        fflush(app->log_file);
    }
    snprintf(app->message, sizeof(app->message), "Logging to %s", app->log_path);
    app->message_until = GetTickCount64() + 4000;
    return true;
}

static void stop_logging(App *app) {
    if (!app->log_file) return;
    fclose(app->log_file);
    app->log_file = NULL;
    snprintf(app->message, sizeof(app->message), "Logging stopped");
    app->message_until = GetTickCount64() + 3000;
}

static bool sample_app(App *app) {
    reset_telemetry(&app->telemetry, app->selected_gpu);
    memset(&app->engines, 0, sizeof(app->engines));
    if (app->demo_mode) {
        double phase = (double)(GetTickCount64() % 12000ULL) / 12000.0 * 6.283185307179586;
        snprintf(app->telemetry.name, sizeof(app->telemetry.name), "VGPU-Mon Demo GPU");
        snprintf(app->telemetry.driver, sizeof(app->telemetry.driver), "demo");
        snprintf(app->telemetry.uuid, sizeof(app->telemetry.uuid), "GPU-DEMO-0000");
        app->telemetry.dxgi_available = true;
        app->telemetry.memory_total = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        app->telemetry.memory_used = (uint64_t)((5.0 + 1.5 * sin(phase)) * 1024.0 * 1024.0 * 1024.0);
        app->telemetry.memory_free = app->telemetry.memory_total - app->telemetry.memory_used;
        app->telemetry.memory_budget = 15ULL * 1024ULL * 1024ULL * 1024ULL;
        app->telemetry.gpu_util = 45.0 + 30.0 * sin(phase);
        app->telemetry.wddm_gpu_util = app->telemetry.gpu_util;
        app->telemetry.memory_util = 32.0 + 12.0 * sin(phase * 0.7);
        app->telemetry.temperature_c = 52 + (int)(5.0 * sin(phase * 0.5));
        app->telemetry.fan_percent = 34;
        app->telemetry.power_w = 118.0 + 25.0 * sin(phase);
        app->telemetry.power_limit_w = 320.0;
        app->telemetry.graphics_clock_mhz = 2100;
        app->telemetry.max_graphics_clock_mhz = 2700;
        app->telemetry.sm_clock_mhz = 2100;
        app->telemetry.memory_clock_mhz = 12000;
        app->telemetry.max_memory_clock_mhz = 14000;
        app->telemetry.pstate = 2;
        app->telemetry.encoder_util = 8;
        app->telemetry.decoder_util = 17;
        app->telemetry.pcie_generation = 4;
        app->telemetry.pcie_width = 16;
        app->telemetry.pcie_tx_mib_s = 82.0;
        app->telemetry.pcie_rx_mib_s = 146.0;
        app->engines.three_d = app->telemetry.gpu_util;
        app->engines.copy = 6.0 + 4.0 * sin(phase);
        app->engines.video_decode = 17.0;
        app->engines.video_encode = 8.0;
        app->engines.compute = 23.0 + 10.0 * sin(phase * 0.8);

        static const char *names[] = {"renderer.exe", "video-player.exe", "compute.exe", "desktop.exe", "browser.exe", "capture.exe"};
        static const char *engines[] = {"3D", "VideoDecode", "Compute", "3D", "3D", "VideoEncode"};
        static const double base_util[] = {31.0, 17.0, 12.0, 5.0, 3.0, 2.0};
        app->process_count = _countof(names);
        memset(app->processes, 0, sizeof(app->processes));
        for (size_t i = 0; i < app->process_count; ++i) {
            GpuProcess *process = &app->processes[i];
            process->pid = 4100U + (DWORD)i * 137U;
            snprintf(process->name, sizeof(process->name), "%s", names[i]);
            snprintf(process->engine, sizeof(process->engine), "%s", engines[i]);
            process->gpu_percent = base_util[i] + 2.0 * sin(phase + (double)i);
            process->dedicated_bytes = (uint64_t)(512U - (unsigned int)i * 54U) * 1024ULL * 1024ULL;
            process->shared_bytes = (uint64_t)(24U + (unsigned int)i * 7U) * 1024ULL * 1024ULL;
        }
        app->wddm_process_memory_suspect = false;
        update_visible_processes(app);
        add_history_sample(app);
        log_sample(app);
        return true;
    }
    bool telemetry_ok = false;
    if (app->nvml_ready && app->selected_gpu < app->nvml.device_count)
        telemetry_ok |= nvml_sample(&app->nvml, app->selected_gpu, &app->telemetry);
    if (app->dxgi_ready && app->selected_gpu < app->dxgi.count)
        telemetry_ok |= dxgi_sample(&app->dxgi, app->selected_gpu, &app->telemetry);
    if (!app->telemetry.name[0]) snprintf(app->telemetry.name, sizeof(app->telemetry.name), "GPU %u", app->selected_gpu);

    bool processes_ok = false;
    if (app->pdh_ready) {
        double wddm_gpu_percent = 0.0;
        processes_ok = pdh_gpu_sample(&app->pdh, app->selected_gpu,
                                      app->processes, VGPU_MAX_PROCESSES, &app->process_count,
                                      &wddm_gpu_percent, &app->engines);
        app->telemetry.wddm_gpu_util = wddm_gpu_percent;
        if (!app->telemetry.nvml_available) app->telemetry.gpu_util = wddm_gpu_percent;
    } else {
        app->process_count = 0;
    }
    app->direct_process_memory_count = 0;
    if (app->d3dkmt_ready) {
        app->direct_process_memory_count = d3dkmt_gpu_enrich_process_memory(
            &app->d3dkmt, app->selected_gpu, app->processes,
            app->process_count);
    }
    /* Direct D3DKMT values need only fit physical VRAM. PDH fallback rows also
       use current board allocation as a guard against its documented stale
       allocation bug. A protected process such as DWM can therefore become
       unavailable without contaminating ordinary process rows. */
    app->wddm_process_memory_suspect =
        invalidate_implausible_dedicated_gpu_memory(
            app->processes, app->process_count, app->telemetry.memory_total,
            app->telemetry.memory_used, app->telemetry.nvml_available) > 0;
    update_visible_processes(app);
    add_history_sample(app);
    log_sample(app);
    return telemetry_ok || processes_ok;
}

static const char *sort_name(SortMode mode) {
    switch (mode) {
        case SORT_GPU: return "GPU";
        case SORT_DEDICATED: return "VRAM";
        case SORT_SHARED: return "Shared";
        case SORT_PID: return "PID";
        case SORT_NAME: return "Name";
        case SORT_ENGINE: return "Engine";
        default: return "Unknown";
    }
}

static void terminal_geometry(int *columns, int *rows, int *left, int *top) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *columns = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (left) *left = info.srWindow.Left;
        if (top) *top = info.srWindow.Top;
    } else {
        *columns = 120;
        *rows = 30;
        if (left) *left = 0;
        if (top) *top = 0;
    }
}

static TableLayout compute_table_layout(int columns) {
    TableLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.indent = columns >= 50 ? 2 : 0;
    layout.count = 6;
    layout.columns[0] = (TableColumn){SORT_PID, "PID", columns < 40 ? 6 : 8, 0, false, true};
    layout.columns[1] = (TableColumn){SORT_NAME, "PROCESS", 0, 0, false, true};
    layout.columns[2] = (TableColumn){SORT_GPU, "GPU", columns < 40 ? 7 : 8, 0, true, true};
    layout.columns[3] = (TableColumn){SORT_DEDICATED, "DEDICATED*", 12, 0, true, columns >= 64};
    layout.columns[4] = (TableColumn){SORT_SHARED, "SHARED*", 12, 0, true, columns >= 82};
    layout.columns[5] = (TableColumn){SORT_ENGINE, "ENGINE", 14, 0, false, columns >= 100};

    for (;;) {
        int fixed_width = layout.indent;
        int visible_count = 0;
        for (size_t i = 0; i < layout.count; ++i) {
            if (!layout.columns[i].visible || layout.columns[i].mode == SORT_NAME) continue;
            fixed_width += layout.columns[i].width;
            visible_count++;
        }
        visible_count++;
        fixed_width += visible_count - 1;
        int name_width = columns - fixed_width;
        if (name_width >= 8) {
            layout.columns[1].width = name_width;
            break;
        }
        if (layout.columns[5].visible) layout.columns[5].visible = false;
        else if (layout.columns[4].visible) layout.columns[4].visible = false;
        else if (layout.columns[3].visible) layout.columns[3].visible = false;
        else break;
    }

    layout.usable = layout.columns[1].width >= 8 && columns >= 24;
    int x = layout.indent;
    for (size_t i = 0; i < layout.count; ++i) {
        if (!layout.columns[i].visible) continue;
        layout.columns[i].start_x = x;
        x += layout.columns[i].width + 1;
    }
    return layout;
}

static const char *utilization_color(double value) {
    if (value >= 90.0) return ANSI_RED;
    if (value >= 65.0) return ANSI_YELLOW;
    return ANSI_GREEN;
}

static void append_bar(TextBuffer *buffer, double value, int width) {
    if (value < 0.0) value = 0.0;
    if (value > 100.0) value = 100.0;
    int filled = (int)round(value * width / 100.0);
    text_buffer_append(buffer, "%s[", utilization_color(value));
    for (int i = 0; i < width; ++i) text_buffer_append(buffer, "%c", i < filled ? '#' : '-');
    text_buffer_append(buffer, "]%s", ANSI_RESET);
}

static void append_sparkline(TextBuffer *buffer, const double *history,
                             size_t count, size_t next, int width) {
    static const char *levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    if (width < 1) return;
    size_t shown = count < (size_t)width ? count : (size_t)width;
    for (int i = 0; i < width - (int)shown; ++i) text_buffer_append(buffer, " ");
    size_t oldest = count < VGPU_HISTORY_SIZE ? 0 : next;
    size_t skip = count > shown ? count - shown : 0;
    for (size_t i = skip; i < count; ++i) {
        size_t index = (oldest + i) % VGPU_HISTORY_SIZE;
        double value = history[index];
        int level = (int)ceil(value * 8.0 / 100.0);
        if (level < 0) level = 0;
        if (level > 8) level = 8;
        text_buffer_append(buffer, "%s", levels[level]);
    }
}

static void truncate_text(const char *source, char *target, size_t target_size, size_t max_chars) {
    if (target_size == 0) return;
    size_t length = strlen(source);
    if (length <= max_chars) {
        snprintf(target, target_size, "%s", source);
    } else if (max_chars > 3) {
        size_t copy = max_chars - 3;
        if (copy >= target_size) copy = target_size - 1;
        while (copy > 0 && (((unsigned char)source[copy] & 0xC0U) == 0x80U)) copy--;
        memcpy(target, source, copy);
        target[copy] = '\0';
        strncat_s(target, target_size, "...", _TRUNCATE);
    } else {
        snprintf(target, target_size, "%.*s", (int)max_chars, source);
    }
}

static void render_help(TextBuffer *buffer) {
    text_buffer_append(buffer,
        "\n%sKeyboard%s\n"
        "  Up/Down, PgUp/PgDn  Select process       [ / ]  Previous/next GPU\n"
        "  u                     Sort GPU usage      m       Sort dedicated VRAM\n"
        "  s                     Sort shared RAM     p       Sort PID\n"
        "  n                     Sort name           e       Sort engine\n"
        "  o                     Reverse order       f       Filter by process name\n"
        "  d                     Process details     i       Full GPU information\n"
        "  c                     Full-screen charts k       Terminate process\n"
        "  l                     Toggle CSV logging  1-0     Select chart metric\n"
        "  Space                 Pause/resume\n"
        "  + / -                 Refresh faster/slower       h Help  q Quit\n\n"
        "%sChart navigation%s\n"
        "  Wheel                 Zoom visible time   Shift+wheel  Pan through history\n"
        "  Left/Right            Pan by 1/4 view     PgUp/PgDn   Pan by a full view\n"
        "  Home/End              Oldest retained/live         Hover  Exact point value\n\n"
        "%sAccounting%s\n"
        "  Process GPU %% is the busiest Windows GPU engine for that PID, matching Task Manager's\n"
        "  model. Dedicated/shared values use Windows' direct per-process video-memory API.\n"
        "  Protected processes may fall back to WDDM counters; an implausible fallback is\n"
        "  shown as N/A while other rows remain visible. A trailing ! marks that rejection.\n"
        "  Board telemetry comes from NVML; DXGI supplies a vendor-neutral memory fallback.\n",
        ANSI_BOLD, ANSI_RESET, ANSI_BOLD, ANSI_RESET, ANSI_BOLD, ANSI_RESET);
}

static void render_gpu_info(TextBuffer *buffer, const App *app) {
    char used[32], free_memory[32], reserved[32], total[32], budget[32], tx[32], rx[32];
    const GpuTelemetry *gpu = &app->telemetry;
    text_buffer_append(buffer, "\n%sGPU information%s\n", ANSI_BOLD, ANSI_RESET);
    text_buffer_append(buffer, "  Adapter      %s\n", gpu->name);
    text_buffer_append(buffer, "  UUID         %s\n", gpu->uuid[0] ? gpu->uuid : "Unavailable");
    text_buffer_append(buffer, "  Driver       %s\n", gpu->driver[0] ? gpu->driver : "Unavailable");
    if (app->demo_mode) {
        text_buffer_append(buffer, "  Sources      Deterministic demo data\n");
    } else {
        text_buffer_append(buffer, "  Sources      %s%s%s%s\n",
                           gpu->nvml_available ? "NVML " : "",
                           gpu->dxgi_available ? "DXGI " : "",
                           app->d3dkmt_ready ? "D3DKMT " : "",
                           app->pdh_ready ? "WDDM performance counters" : "");
    }
    text_buffer_append(buffer, "  VRAM         %s allocated, %s reserved, %s free, %s physical\n",
                       format_bytes(gpu->memory_used, used, sizeof(used)),
                       format_bytes(gpu->memory_reserved, reserved, sizeof(reserved)),
                       format_bytes(gpu->memory_free, free_memory, sizeof(free_memory)),
                       format_bytes(gpu->memory_total, total, sizeof(total)));
    text_buffer_append(buffer, "  OS budget    %s\n", format_bytes(gpu->memory_budget, budget, sizeof(budget)));
    text_buffer_append(buffer, "  Utilization  GPU %.1f%%, WDDM busiest engine %.1f%%, memory controller %.1f%%\n",
                       gpu->gpu_util, gpu->wddm_gpu_util, gpu->memory_util);
    text_buffer_append(buffer, "               encoder %d%%, decoder %d%%\n",
                       gpu->encoder_util, gpu->decoder_util);
    text_buffer_append(buffer, "  Thermals     %d C, fan %d%%, power %.2f W / %.2f W\n",
                       gpu->temperature_c, gpu->fan_percent, gpu->power_w, gpu->power_limit_w);
    text_buffer_append(buffer, "  Clocks       graphics %d / %d MHz, SM %d MHz, memory %d / %d MHz\n",
                       gpu->graphics_clock_mhz, gpu->max_graphics_clock_mhz,
                       gpu->sm_clock_mhz, gpu->memory_clock_mhz, gpu->max_memory_clock_mhz);
    text_buffer_append(buffer, "  Performance  P%d, PCIe Gen%d x%d, TX %s, RX %s\n",
                       gpu->pstate, gpu->pcie_generation, gpu->pcie_width,
                       gpu->pcie_tx_mib_s >= 0 ? format_rate(gpu->pcie_tx_mib_s, tx, sizeof(tx)) : "N/A",
                       gpu->pcie_rx_mib_s >= 0 ? format_rate(gpu->pcie_rx_mib_s, rx, sizeof(rx)) : "N/A");
    text_buffer_append(buffer, "\n  Values shown as -1 are not exposed by the installed driver for this GPU.\n");
}

static void render_details(TextBuffer *buffer, App *app, const GpuProcess *process) {
    if (!process) return;
    ULONGLONG now = GetTickCount64();
    if (app->details_pid != process->pid || now - app->details_sampled_at >= 2000) {
        query_process_details(process->pid, &app->details_cache);
        app->details_pid = process->pid;
        app->details_sampled_at = now;
    }
    const ProcessDetails *details = &app->details_cache;
    char working[32], private_memory[32];
    text_buffer_append(buffer, "\n%sProcess details%s\n", ANSI_BOLD, ANSI_RESET);
    text_buffer_append(buffer, "  PID %-8lu Threads %-5u User %s\n",
                       (unsigned long)process->pid, details->threads, details->user);
    text_buffer_append(buffer, "  CPU RAM %-12s Private %-12s Started %s\n",
                       format_bytes(details->working_set, working, sizeof(working)),
                       format_bytes(details->private_bytes, private_memory, sizeof(private_memory)),
                       details->started);
    text_buffer_append(buffer, "  Path %s\n", details->path);
}

static void append_table_cell(TextBuffer *buffer, const char *text, int width, bool right_aligned) {
    char clipped[256];
    truncate_text(text, clipped, sizeof(clipped), (size_t)width);
    if (right_aligned) text_buffer_append(buffer, "%*s", width, clipped);
    else text_buffer_append(buffer, "%-*s", width, clipped);
}

static void render_process_table(App *app, TextBuffer *buffer, int columns, int available_rows) {
    TableLayout layout = compute_table_layout(columns);
    app->header_hit_count = 0;
    app->table_header_row = -1;
    app->table_first_process_row = -1;
    app->table_last_process_row = -1;

    text_buffer_append(buffer, "\n\n");
    if (!layout.usable) {
        text_buffer_append(buffer, "%sTerminal is too narrow. Resize to at least 24 columns.%s\n",
                           ANSI_YELLOW, ANSI_RESET);
        return;
    }

    if (available_rows < 1) available_rows = 1;
    if (app->selection < app->scroll) app->scroll = app->selection;
    if (app->selection >= app->scroll + (size_t)available_rows)
        app->scroll = app->selection - (size_t)available_rows + 1;

    app->table_header_row = buffer->row;
    text_buffer_append(buffer, "%s", ANSI_BOLD);
    for (int i = 0; i < layout.indent; ++i) text_buffer_append(buffer, " ");
    bool first = true;
    for (size_t i = 0; i < layout.count; ++i) {
        TableColumn *column = &layout.columns[i];
        if (!column->visible) continue;
        if (!first) text_buffer_append(buffer, " ");
        first = false;
        char label[48];
        if (app->sort_mode == column->mode)
            snprintf(label, sizeof(label), "%s %c", column->label, app->sort_descending ? 'v' : '^');
        else snprintf(label, sizeof(label), "%s", column->label);
        append_table_cell(buffer, label, column->width, column->right_aligned);
        if (app->header_hit_count < _countof(app->header_hits)) {
            HeaderHit *hit = &app->header_hits[app->header_hit_count++];
            hit->start_x = column->start_x;
            hit->end_x = column->start_x + column->width - 1;
            hit->mode = column->mode;
        }
    }
    text_buffer_append(buffer, "%s\n", ANSI_RESET);
    app->table_first_process_row = buffer->row;

    size_t end = app->scroll + (size_t)available_rows;
    if (end > app->visible_count) end = app->visible_count;
    for (size_t row = app->scroll; row < end; ++row) {
        GpuProcess *process = app->visible[row];
        char pid[32], gpu[32], dedicated[32], shared[32];
        snprintf(pid, sizeof(pid), "%lu", (unsigned long)process->pid);
        snprintf(gpu, sizeof(gpu), "%.1f%%", process->gpu_percent);
        if (process->dedicated_memory_invalid)
            snprintf(dedicated, sizeof(dedicated), "N/A !");
        else
            format_bytes(process->dedicated_bytes, dedicated, sizeof(dedicated));
        format_bytes(process->shared_bytes, shared, sizeof(shared));

        if (row == app->selection) text_buffer_append(buffer, "%s", ANSI_REVERSE);
        for (int i = 0; i < layout.indent; ++i) text_buffer_append(buffer, " ");
        first = true;
        for (size_t i = 0; i < layout.count; ++i) {
            TableColumn *column = &layout.columns[i];
            if (!column->visible) continue;
            if (!first) text_buffer_append(buffer, " ");
            first = false;
            const char *value = "";
            switch (column->mode) {
                case SORT_PID: value = pid; break;
                case SORT_NAME: value = process->name; break;
                case SORT_GPU: value = gpu; break;
                case SORT_DEDICATED: value = dedicated; break;
                case SORT_SHARED: value = shared; break;
                case SORT_ENGINE: value = process->engine; break;
            }
            append_table_cell(buffer, value, column->width, column->right_aligned);
        }
        if (row == app->selection) text_buffer_append(buffer, "%s", ANSI_RESET);
        text_buffer_append(buffer, "\n");
    }
    if (end > app->scroll)
        app->table_last_process_row = app->table_first_process_row + (int)(end - app->scroll) - 1;
    if (app->visible_count == 0)
        text_buffer_append(buffer, "%sNo GPU processes match the current filter.%s\n", ANSI_DIM, ANSI_RESET);
    if (app->show_details && app->visible_count) render_details(buffer, app, app->visible[app->selection]);
}

static const char *chart_color(ChartMetric metric) {
    if (metric == CHART_TEMPERATURE) return ANSI_YELLOW;
    if (metric == CHART_POWER) return ANSI_RED;
    if (metric == CHART_VRAM || metric == CHART_MEMORY_CONTROLLER) return ANSI_CYAN;
    if (metric >= CHART_3D && metric <= CHART_COMPUTE) return ANSI_BLUE;
    return ANSI_GREEN;
}

static void render_chart_view_fallback(App *app, TextBuffer *buffer, int columns, int rows) {
    app->header_hit_count = 0;
    app->table_header_row = -1;
    app->table_first_process_row = -1;
    app->table_last_process_row = -1;
    if (columns < 24 || rows < 8) {
        text_buffer_append(buffer, "%sVGPU-Mon%s\nResize to at least 24 x 8 for chart view.\n",
                           ANSI_BOLD, ANSI_RESET);
        return;
    }

    ChartMetric metric = app->chart_metric;
    const char *unit = chart_metric_unit(metric);
    bool available = chart_metric_available(app, metric);
    double current = current_chart_value(app, metric);
    double observed_max = current;
    size_t count = app->chart_history_count;
    size_t oldest = count < VGPU_CHART_HISTORY_SIZE ? 0 : app->chart_history_next;
    for (size_t i = 0; i < count; ++i) {
        double value = app->chart_history[metric][(oldest + i) % VGPU_CHART_HISTORY_SIZE];
        if (value > observed_max) observed_max = value;
    }
    double scale = 100.0;
    if (metric == CHART_POWER) {
        scale = app->telemetry.power_limit_w > 0 ? app->telemetry.power_limit_w : 100.0;
        if (observed_max > scale) scale = ceil(observed_max * 1.10 / 10.0) * 10.0;
    } else if (metric == CHART_TEMPERATURE && observed_max > scale) {
        scale = ceil(observed_max * 1.10 / 10.0) * 10.0;
    }

    char title[256], title_display[256], gpu_name[256], status[256], status_display[256];
    snprintf(title, sizeof(title), "VGPU-Mon %s | %s%s%s",
             VGPU_VERSION, chart_metric_name(metric), app->paused ? " | PAUSED" : "",
             app->log_file ? " | REC" : "");
    truncate_text(title, title_display, sizeof(title_display), (size_t)columns);
    truncate_text(app->telemetry.name, gpu_name, sizeof(gpu_name), (size_t)columns);
    if (available) {
        snprintf(status, sizeof(status), "Current %.1f %s | Peak %.1f %s | sample %u ms | %zu samples",
                 current, unit, observed_max, unit, app->interval_ms, count);
    } else {
        snprintf(status, sizeof(status),
                 "Unavailable from this GPU, driver, or Windows driver mode | sample %u ms",
                 app->interval_ms);
    }
    truncate_text(status, status_display, sizeof(status_display), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s\n%s%s%s\n%s\n",
                       ANSI_BOLD, title_display, ANSI_RESET,
                       ANSI_CYAN, gpu_name, ANSI_RESET, status_display);

    int prefix_width = 8;
    int plot_width = columns - prefix_width;
    /* The caller reserves at least eight rows before entering chart view. */
    int chart_height = rows - 6;
    size_t shown = count < (size_t)plot_width ? count : (size_t)plot_width;
    size_t skip = count > shown ? count - shown : 0;
    int left_padding = plot_width - (int)shown;
    const char *color = chart_color(metric);

    for (int row = 0; row < chart_height; ++row) {
        double upper = scale * (double)(chart_height - row) / (double)chart_height;
        double lower = scale * (double)(chart_height - row - 1) / (double)chart_height;
        bool labeled = row == 0 || row == chart_height - 1 || row == chart_height / 2;
        if (labeled) text_buffer_append(buffer, "%6.1f%s|", upper, unit);
        else text_buffer_append(buffer, "       |");
        text_buffer_append(buffer, "%s", color);
        for (int x = 0; x < plot_width; ++x) {
            if (x < left_padding) {
                text_buffer_append(buffer, "%c", (row == chart_height / 2) ? '-' : ' ');
                continue;
            }
            size_t logical = skip + (size_t)(x - left_padding);
            size_t index = (oldest + logical) % VGPU_CHART_HISTORY_SIZE;
            double value = app->chart_history[metric][index];
            if (value >= upper) text_buffer_append(buffer, "█");
            else if (value > lower) text_buffer_append(buffer, "▄");
            else text_buffer_append(buffer, "%c", (row == chart_height / 2) ? '-' : ' ');
        }
        text_buffer_append(buffer, "%s\n", ANSI_RESET);
    }

    const char *controls = columns >= 104
        ? "1 GPU  2 VRAM  3 3D  4 Copy  5 Decode  6 Encode  7 Compute  8 MemCtl  9 Temp  0 Power | c table | q quit"
        : "1-0 change metric | c table | Space pause | q quit";
    char clipped_controls[256];
    truncate_text(controls, clipped_controls, sizeof(clipped_controls), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s", ANSI_DIM, clipped_controls, ANSI_RESET);
}

static void render_chart_view(App *app, TextBuffer *buffer, int columns, int rows) {
    app->header_hit_count = 0;
    app->table_header_row = -1;
    app->table_first_process_row = -1;
    app->table_last_process_row = -1;
    app->chart_plot_left = -1;
    app->chart_plot_right = -1;
    app->chart_plot_top = -1;
    app->chart_plot_bottom = -1;
    if (columns < 24 || rows < 8) {
        app->chart_hover_active = false;
        text_buffer_append(buffer, "%sVGPU-Mon%s\nResize to at least 24 x 8 for chart view.\n",
                           ANSI_BOLD, ANSI_RESET);
        return;
    }

    clamp_chart_window(app);
    clamp_chart_pan(app);
    /* Define the visible range with monotonic sample timestamps so changing
       the refresh interval cannot distort older data. */
    ChartMetric metric = app->chart_metric;
    const char *unit = chart_metric_unit(metric);
    bool available = chart_metric_available(app, metric);
    double current = current_chart_value(app, metric);
    double observed_max = 0.0;
    size_t count = app->chart_history_count;
    ULONGLONG newest_time = count ? chart_timestamp(app, count - 1) : GetTickCount64();
    ULONGLONG view_end = newest_time >= app->chart_pan_ms
        ? newest_time - app->chart_pan_ms : 0;
    ULONGLONG view_start = view_end >= app->chart_window_ms
        ? view_end - app->chart_window_ms : 0;

    const int prefix_width = 8;
    int plot_width = columns - prefix_width;
    int chart_height = rows - 6;
    /* Scratch columns are retained across frames and only grow on resize. */
    if (!ensure_chart_plot_capacity(app, (size_t)plot_width)) {
        render_chart_view_fallback(app, buffer, columns, rows);
        return;
    }
    double *plot_values = app->chart_plot_values;
    unsigned char *plot_present = app->chart_plot_present;
    memset(plot_values, 0, (size_t)plot_width * sizeof(*plot_values));
    memset(plot_present, 0, (size_t)plot_width * sizeof(*plot_present));

    size_t first_visible = count;
    size_t last_visible = count;
    /* Bucket real samples into terminal columns while preserving peaks. */
    for (size_t i = 0; i < count; ++i) {
        ULONGLONG timestamp = chart_timestamp(app, i);
        if (timestamp < view_start || timestamp > view_end) continue;
        if (first_visible == count) first_visible = i;
        last_visible = i + 1;
        double value = chart_value(app, metric, i);
        if (value > observed_max) observed_max = value;
        ULONGLONG offset = timestamp - view_start;
        int x = (int)((offset * (ULONGLONG)(plot_width - 1)) / app->chart_window_ms);
        if (x < 0) x = 0;
        if (x >= plot_width) x = plot_width - 1;
        if (!plot_present[x] || value > plot_values[x]) plot_values[x] = value;
        plot_present[x] = 1;
    }

    if (first_visible < count) {
        /* Carry the last sample through empty columns to form a continuous
           step chart without fabricating samples before history begins. */
        size_t cursor = first_visible;
        bool have_value = false;
        double held_value = 0.0;
        for (int x = 0; x < plot_width; ++x) {
            ULONGLONG target = view_start +
                (app->chart_window_ms * (ULONGLONG)x) / (ULONGLONG)(plot_width - 1);
            while (cursor < last_visible && chart_timestamp(app, cursor) <= target) {
                held_value = chart_value(app, metric, cursor);
                have_value = true;
                cursor++;
            }
            if (!plot_present[x] && have_value) {
                plot_values[x] = held_value;
                plot_present[x] = 1;
            }
        }
    }

    double scale = 100.0;
    /* Percent metrics use a fixed scale; temperature and power expand only
       when the visible data exceeds their normal ceiling. */
    if (metric == CHART_POWER) {
        scale = app->telemetry.power_limit_w > 0 ? app->telemetry.power_limit_w : 100.0;
        if (observed_max > scale) scale = ceil(observed_max * 1.10 / 10.0) * 10.0;
    } else if (metric == CHART_TEMPERATURE && observed_max > scale) {
        scale = ceil(observed_max * 1.10 / 10.0) * 10.0;
    }

    char title[256], title_display[256], gpu_name[256], status[256], status_display[256];
    char span[32], retained[32], edge[48];
    format_chart_duration(app->chart_window_ms, span, sizeof(span));
    ULONGLONG retained_ms = count > 1 ? newest_time - chart_timestamp(app, 0) : 0;
    format_chart_duration(retained_ms, retained, sizeof(retained));
    if (app->chart_pan_ms == 0) {
        snprintf(edge, sizeof(edge), "LIVE");
    } else {
        char age[32];
        format_chart_duration(app->chart_pan_ms, age, sizeof(age));
        snprintf(edge, sizeof(edge), "%s ago", age);
    }
    snprintf(title, sizeof(title), "VGPU-Mon %s | %s%s%s",
             VGPU_VERSION, chart_metric_name(metric), app->paused ? " | PAUSED" : "",
             app->log_file ? " | REC" : "");
    truncate_text(title, title_display, sizeof(title_display), (size_t)columns);
    truncate_text(app->telemetry.name, gpu_name, sizeof(gpu_name), (size_t)columns);
    if (available) {
        snprintf(status, sizeof(status),
                 "Now %.1f%s | View peak %.1f%s | Span %s | %s | Retained %s",
                 current, unit, observed_max, unit, span, edge, retained);
    } else {
        snprintf(status, sizeof(status),
                 "Unavailable from this GPU, driver, or Windows driver mode | Span %s | %s",
                 span, edge);
    }
    truncate_text(status, status_display, sizeof(status_display), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s\n%s%s%s\n%s\n",
                       ANSI_BOLD, title_display, ANSI_RESET,
                       ANSI_CYAN, gpu_name, ANSI_RESET, status_display);

    app->chart_plot_left = prefix_width;
    app->chart_plot_right = prefix_width + plot_width - 1;
    app->chart_plot_top = 3;
    app->chart_plot_bottom = 3 + chart_height - 1;

    /* Resolve the pointer's time coordinate to the nearest stored sample and
       prepare an ASCII tooltip that can be overlaid without changing width. */
    int hover_plot_x = -1;
    int tooltip_row = -1;
    int tooltip_start = -1;
    int tooltip_length = 0;
    char tooltip[96] = "";
    char tooltip_display[96] = "";
    if (app->chart_hover_active &&
        app->chart_hover_x >= app->chart_plot_left &&
        app->chart_hover_x <= app->chart_plot_right &&
        app->chart_hover_y >= app->chart_plot_top &&
        app->chart_hover_y <= app->chart_plot_bottom) {
        hover_plot_x = app->chart_hover_x - app->chart_plot_left;
        tooltip_row = app->chart_hover_y - app->chart_plot_top;
        ULONGLONG target = view_start +
            (app->chart_window_ms * (ULONGLONG)hover_plot_x) /
            (ULONGLONG)(plot_width - 1);
        size_t nearest = count;
        ULONGLONG nearest_distance = ~(ULONGLONG)0;
        if (first_visible < count && target >= chart_timestamp(app, first_visible)) {
            for (size_t i = first_visible; i < last_visible; ++i) {
                ULONGLONG timestamp = chart_timestamp(app, i);
                ULONGLONG distance = timestamp > target
                    ? timestamp - target : target - timestamp;
                if (distance < nearest_distance) {
                    nearest = i;
                    nearest_distance = distance;
                }
            }
        }
        if (nearest < count) {
            ULONGLONG age_ms = newest_time - chart_timestamp(app, nearest);
            if (age_ms < 1000ULL) {
                snprintf(tooltip, sizeof(tooltip), " Point %.1f%s | now ",
                         chart_value(app, metric, nearest), unit);
            } else {
                char age[32];
                format_chart_duration(age_ms, age, sizeof(age));
                snprintf(tooltip, sizeof(tooltip), " Point %.1f%s | %s ago ",
                         chart_value(app, metric, nearest), unit, age);
            }
        } else {
            ULONGLONG age_ms = newest_time > target ? newest_time - target : 0;
            char age[32];
            format_chart_duration(age_ms, age, sizeof(age));
            snprintf(tooltip, sizeof(tooltip), " No sample | %s ago ", age);
        }
        truncate_text(tooltip, tooltip_display, sizeof(tooltip_display), (size_t)plot_width);
        tooltip_length = (int)strlen(tooltip_display);
        tooltip_start = hover_plot_x + 2;
        if (tooltip_start + tooltip_length > plot_width)
            tooltip_start = hover_plot_x - tooltip_length - 1;
        if (tooltip_start < 0) tooltip_start = 0;
        if (tooltip_start + tooltip_length > plot_width)
            tooltip_start = plot_width - tooltip_length;
    }

    const char *color = chart_color(metric);
    /* Draw one bounded terminal row at a time. Tooltip-covered iterations
       emit no cell because the first iteration emitted the complete overlay. */
    for (int row = 0; row < chart_height; ++row) {
        double upper = scale * (double)(chart_height - row) / (double)chart_height;
        double lower = scale * (double)(chart_height - row - 1) / (double)chart_height;
        bool labeled = row == 0 || row == chart_height - 1 || row == chart_height / 2;
        if (labeled) text_buffer_append(buffer, "%6.1f%s|", upper, unit);
        else text_buffer_append(buffer, "       |");
        text_buffer_append(buffer, "%s", color);
        for (int x = 0; x < plot_width; ++x) {
            if (row == tooltip_row && x >= tooltip_start &&
                x < tooltip_start + tooltip_length) {
                if (x == tooltip_start) {
                    text_buffer_append(buffer, "%s%s%s%s", ANSI_RESET, ANSI_REVERSE,
                                       tooltip_display, ANSI_RESET);
                    text_buffer_append(buffer, "%s", color);
                }
                continue;
            }
            const char *cell = (row == chart_height / 2) ? "-" : " ";
            if (plot_present[x]) {
                if (plot_values[x] >= upper) cell = "\xE2\x96\x88";
                else if (plot_values[x] > lower) cell = "\xE2\x96\x84";
            }
            if (x == hover_plot_x) text_buffer_append(buffer, "%s", ANSI_REVERSE);
            text_buffer_append(buffer, "%s", cell);
            if (x == hover_plot_x) text_buffer_append(buffer, "%s%s", ANSI_RESET, color);
        }
        text_buffer_append(buffer, "%s\n", ANSI_RESET);
    }

    const char *controls = columns >= 112
        ? "Wheel zoom | Shift+wheel / Left/Right pan | Home oldest | End live | Hover exact | 1-0 metric | c table | q quit"
        : "Wheel zoom | Left/Right pan | End live | Hover exact value | c table | q quit";
    char clipped_controls[256];
    truncate_text(controls, clipped_controls, sizeof(clipped_controls), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s", ANSI_DIM, clipped_controls, ANSI_RESET);
}

static void render_app(App *app, TextBuffer *buffer) {
    int columns, rows;
    terminal_geometry(&columns, &rows, &app->viewport_left, &app->viewport_top);
    if (columns < 1) columns = 1;
    if (rows < 1) rows = 1;
    /* Never write the terminal's final column. Doing so leaves a pending wrap
       that can scroll the viewport when the next newline is emitted. */
    if (columns > 1) columns--;
    if (app->chart_view) {
        render_chart_view(app, buffer, columns, rows);
        return;
    }

    char used[32], total[32], budget[32], tx[32], rx[32];
    double memory_percent = app->telemetry.memory_total
        ? (double)app->telemetry.memory_used * 100.0 / (double)app->telemetry.memory_total : 0.0;

    char title_line[256], gpu_line[256];
    snprintf(title_line, sizeof(title_line), "VGPU-Mon %s [%s] GPU %u/%u %ums%s",
             VGPU_VERSION, app->paused ? "PAUSED" : "LIVE", app->selected_gpu + 1,
             app->gpu_count, app->interval_ms, app->log_file ? " [REC]" : "");
    truncate_text(title_line, gpu_line, sizeof(gpu_line), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s\n", ANSI_BOLD, gpu_line, ANSI_RESET);
    snprintf(title_line, sizeof(title_line), "%s%s%s", app->telemetry.name,
             app->telemetry.driver[0] ? " | Driver " : "", app->telemetry.driver);
    truncate_text(title_line, gpu_line, sizeof(gpu_line), (size_t)columns);
    text_buffer_append(buffer, "%s%s%s", ANSI_CYAN, gpu_line, ANSI_RESET);

    bool compact = columns < 72 || rows < 16;
    if (compact) {
        text_buffer_append(buffer, "\nGPU %.1f%% | VRAM %s / %s",
                           app->telemetry.gpu_util,
                           format_bytes(app->telemetry.memory_used, used, sizeof(used)),
                           format_bytes(app->telemetry.memory_total, total, sizeof(total)));
        text_buffer_append(buffer, "\nTemp ");
        if (app->telemetry.temperature_c >= 0) text_buffer_append(buffer, "%d C", app->telemetry.temperature_c);
        else text_buffer_append(buffer, "N/A");
        if (app->telemetry.power_w >= 0) text_buffer_append(buffer, " | Power %.1f W", app->telemetry.power_w);
    } else {
        text_buffer_append(buffer, "\n\n GPU  %5.1f%% ", app->telemetry.gpu_util);
        append_bar(buffer, app->telemetry.gpu_util, columns > 110 ? 28 : 18);
        text_buffer_append(buffer, "  ");
        append_sparkline(buffer, app->utilization_history, app->history_count, app->history_next,
                         columns > 110 ? 30 : 16);
        text_buffer_append(buffer, "\n VRAM %5.1f%% ", memory_percent);
        append_bar(buffer, memory_percent, columns > 110 ? 28 : 18);
        text_buffer_append(buffer, "  %s / %s", format_bytes(app->telemetry.memory_used, used, sizeof(used)),
                           format_bytes(app->telemetry.memory_total, total, sizeof(total)));
        if (columns >= 100 && app->telemetry.memory_budget && app->telemetry.memory_budget != app->telemetry.memory_total)
            text_buffer_append(buffer, "  budget %s", format_bytes(app->telemetry.memory_budget, budget, sizeof(budget)));
        text_buffer_append(buffer, "\n\n Temp %s", app->telemetry.temperature_c >= 0 ? "" : "N/A");
        if (app->telemetry.temperature_c >= 0) text_buffer_append(buffer, "%d C", app->telemetry.temperature_c);
        text_buffer_append(buffer, "   Power ");
        if (app->telemetry.power_w >= 0) {
            text_buffer_append(buffer, "%.1f W", app->telemetry.power_w);
            if (app->telemetry.power_limit_w > 0) text_buffer_append(buffer, " / %.0f W", app->telemetry.power_limit_w);
        } else text_buffer_append(buffer, "N/A");
        text_buffer_append(buffer, "   Fan %s", app->telemetry.fan_percent >= 0 ? "" : "N/A");
        if (app->telemetry.fan_percent >= 0) text_buffer_append(buffer, "%d%%", app->telemetry.fan_percent);
        text_buffer_append(buffer, "   P-state %s", app->telemetry.pstate >= 0 ? "P" : "N/A");
        if (app->telemetry.pstate >= 0) text_buffer_append(buffer, "%d", app->telemetry.pstate);
        text_buffer_append(buffer, "\n Clock core %s", app->telemetry.graphics_clock_mhz >= 0 ? "" : "N/A");
        if (app->telemetry.graphics_clock_mhz >= 0) text_buffer_append(buffer, "%d MHz", app->telemetry.graphics_clock_mhz);
        text_buffer_append(buffer, "   memory %s", app->telemetry.memory_clock_mhz >= 0 ? "" : "N/A");
        if (app->telemetry.memory_clock_mhz >= 0) text_buffer_append(buffer, "%d MHz", app->telemetry.memory_clock_mhz);
        text_buffer_append(buffer, "   Encode %s", app->telemetry.encoder_util >= 0 ? "" : "N/A");
        if (app->telemetry.encoder_util >= 0) text_buffer_append(buffer, "%d%%", app->telemetry.encoder_util);
        text_buffer_append(buffer, "   Decode %s", app->telemetry.decoder_util >= 0 ? "" : "N/A");
        if (app->telemetry.decoder_util >= 0) text_buffer_append(buffer, "%d%%", app->telemetry.decoder_util);
        if (columns >= 105) {
            text_buffer_append(buffer, "\n PCIe ");
            if (app->telemetry.pcie_generation >= 0) text_buffer_append(buffer, "Gen%d x%d", app->telemetry.pcie_generation, app->telemetry.pcie_width);
            else text_buffer_append(buffer, "N/A");
            text_buffer_append(buffer, "   TX %s   RX %s",
                               app->telemetry.pcie_tx_mib_s >= 0 ? format_rate(app->telemetry.pcie_tx_mib_s, tx, sizeof(tx)) : "N/A",
                               app->telemetry.pcie_rx_mib_s >= 0 ? format_rate(app->telemetry.pcie_rx_mib_s, rx, sizeof(rx)) : "N/A");
        }
    }

    if (app->show_help) {
        render_help(buffer);
    } else if (app->show_gpu_info) {
        render_gpu_info(buffer, app);
    } else {
        int detail_rows = app->show_details ? 5 : 0;
        /* Two spacer rows, the header, and the footer consume five cursor rows
           around the process list. Keep the final cursor on-screen. */
        int available_rows = rows - buffer->row - detail_rows - 5;
        render_process_table(app, buffer, columns, available_rows);
    }

    if (app->editing_filter) {
        text_buffer_append(buffer, "\n%sFilter:%s %s_  %s(Enter apply, Esc cancel, Backspace edit)%s",
                           ANSI_BOLD, ANSI_RESET, app->filter_draft, ANSI_DIM, ANSI_RESET);
    } else if (app->confirm_pid) {
        text_buffer_append(buffer, "\n%sTerminate PID %lu? Press y to confirm or any other key to cancel.%s",
                           ANSI_RED, (unsigned long)app->confirm_pid, ANSI_RESET);
    } else if (app->message[0] && GetTickCount64() < app->message_until) {
        text_buffer_append(buffer, "\n%s%s%s", ANSI_YELLOW, app->message, ANSI_RESET);
    } else {
        char footer[512], clipped[512];
        const char *health = !app->pdh_ready && !app->demo_mode ? "! WDDM counters unavailable | " :
                             !app->d3dkmt_ready && !app->demo_mode ? "! Direct process memory unavailable | " :
                             app->wddm_process_memory_suspect ? "! WDDM fallback anomaly | " : "";
        snprintf(footer, sizeof(footer), "%s%s %s | %zu processes | filter: %s | c chart | h help | q quit",
                 health,
                 sort_name(app->sort_mode), app->sort_descending ? "high-low" : "low-high",
                 app->visible_count, app->filter[0] ? app->filter : "none");
        truncate_text(footer, clipped, sizeof(clipped), (size_t)columns);
        text_buffer_append(buffer, "\n%s%s%s", ANSI_DIM, clipped, ANSI_RESET);
    }
}

static bool console_enter(ConsoleState *state) {
    memset(state, 0, sizeof(*state));
    state->input = GetStdHandle(STD_INPUT_HANDLE);
    state->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(state->input, &state->old_input_mode) ||
        !GetConsoleMode(state->output, &state->old_output_mode)) return false;
    state->old_input_cp = GetConsoleCP();
    state->old_output_cp = GetConsoleOutputCP();
    GetConsoleTitleW(state->old_title, (DWORD)_countof(state->old_title));

    DWORD input_mode = state->old_input_mode;
    input_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE);
    input_mode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    DWORD output_mode = (state->old_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) &
                        ~DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(state->input, input_mode)) return false;
    if (!SetConsoleMode(state->output, output_mode)) {
        SetConsoleMode(state->input, state->old_input_mode);
        return false;
    }
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"VGPU-Mon");
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", stdout);
    fflush(stdout);
    state->active = true;
    return true;
}

static void console_leave(ConsoleState *state) {
    if (!state || !state->active) return;
    fputs(ANSI_RESET "\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    SetConsoleMode(state->input, state->old_input_mode);
    SetConsoleMode(state->output, state->old_output_mode);
    SetConsoleCP(state->old_input_cp);
    SetConsoleOutputCP(state->old_output_cp);
    SetConsoleTitleW(state->old_title);
    state->active = false;
}

static void select_relative(App *app, int delta) {
    if (app->visible_count == 0) return;
    long long next = (long long)app->selection + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= app->visible_count) next = (long long)app->visible_count - 1;
    app->selection = (size_t)next;
    app->selected_pid = app->visible[app->selection]->pid;
}

static void set_sort(App *app, SortMode mode) {
    if (app->sort_mode == mode) app->sort_descending = !app->sort_descending;
    else {
        app->sort_mode = mode;
        app->sort_descending = mode != SORT_NAME && mode != SORT_ENGINE && mode != SORT_PID;
    }
    update_visible_processes(app);
}

static void select_gpu(App *app, int delta) {
    if (app->gpu_count <= 1) return;
    int next = (int)app->selected_gpu + delta;
    if (next < 0) next = (int)app->gpu_count - 1;
    if ((unsigned int)next >= app->gpu_count) next = 0;
    app->selected_gpu = (unsigned int)next;
    app->selection = 0;
    app->selected_pid = 0;
    app->scroll = 0;
    app->history_count = 0;
    app->history_next = 0;
    app->chart_history_count = 0;
    app->chart_history_next = 0;
    app->chart_pan_ms = 0;
    app->chart_hover_active = false;
    sample_app(app);
}

static bool handle_filter_key(App *app, int key) {
    if (key == 27) {
        app->editing_filter = false;
        return true;
    }
    if (key == 13) {
        snprintf(app->filter, sizeof(app->filter), "%s", app->filter_draft);
        app->editing_filter = false;
        app->selection = 0;
        app->selected_pid = 0;
        update_visible_processes(app);
        return true;
    }
    if (key == 8) {
        size_t length = strlen(app->filter_draft);
        if (length) app->filter_draft[length - 1] = '\0';
        return true;
    }
    if (key >= 32 && key <= 126) {
        size_t length = strlen(app->filter_draft);
        if (length + 1 < sizeof(app->filter_draft)) {
            app->filter_draft[length] = (char)key;
            app->filter_draft[length + 1] = '\0';
        }
        return true;
    }
    return true;
}

static bool handle_key(App *app, int key) {
    if (app->editing_filter) return handle_filter_key(app, key);
    if (app->confirm_pid) {
        DWORD pid = app->confirm_pid;
        app->confirm_pid = 0;
        if (key == 'y' || key == 'Y') {
            terminate_process_safely(pid, app->message, sizeof(app->message));
            app->message_until = GetTickCount64() + 5000;
            Sleep(100);
            sample_app(app);
        }
        return true;
    }

    switch (key) {
        case 'q': case 'Q': return false;
        case 'h': case 'H': app->show_help = !app->show_help; break;
        case 'i': case 'I': app->show_gpu_info = !app->show_gpu_info; break;
        case 'c': case 'C':
            app->chart_view = !app->chart_view;
            if (!app->chart_view) app->chart_hover_active = false;
            break;
        case 'd': case 'D': app->show_details = !app->show_details; break;
        case 'u': case 'U': set_sort(app, SORT_GPU); break;
        case 'm': case 'M': set_sort(app, SORT_DEDICATED); break;
        case 's': case 'S': set_sort(app, SORT_SHARED); break;
        case 'p': case 'P': set_sort(app, SORT_PID); break;
        case 'n': case 'N': set_sort(app, SORT_NAME); break;
        case 'e': case 'E': set_sort(app, SORT_ENGINE); break;
        case 'o': case 'O': app->sort_descending = !app->sort_descending; update_visible_processes(app); break;
        case 'f': case 'F':
            app->editing_filter = true;
            snprintf(app->filter_draft, sizeof(app->filter_draft), "%s", app->filter);
            break;
        case 'k': case 'K':
            if (app->visible_count) app->confirm_pid = app->visible[app->selection]->pid;
            break;
        case 'l': case 'L':
            if (app->log_file) stop_logging(app); else start_logging(app, "vgpu-mon.csv");
            break;
        case ' ': app->paused = !app->paused; break;
        case '\t': app->chart_metric = (ChartMetric)((app->chart_metric + 1) % CHART_METRIC_COUNT); break;
        case '1': app->chart_metric = CHART_GPU; app->chart_view = true; break;
        case '2': app->chart_metric = CHART_VRAM; app->chart_view = true; break;
        case '3': app->chart_metric = CHART_3D; app->chart_view = true; break;
        case '4': app->chart_metric = CHART_COPY; app->chart_view = true; break;
        case '5': app->chart_metric = CHART_VIDEO_DECODE; app->chart_view = true; break;
        case '6': app->chart_metric = CHART_VIDEO_ENCODE; app->chart_view = true; break;
        case '7': app->chart_metric = CHART_COMPUTE; app->chart_view = true; break;
        case '8': app->chart_metric = CHART_MEMORY_CONTROLLER; app->chart_view = true; break;
        case '9': app->chart_metric = CHART_TEMPERATURE; app->chart_view = true; break;
        case '0': app->chart_metric = CHART_POWER; app->chart_view = true; break;
        case '[': select_gpu(app, -1); break;
        case ']': select_gpu(app, 1); break;
        case '+': case '=':
            if (app->interval_ms > 250) app->interval_ms -= 250;
            if (app->interval_ms < 250) app->interval_ms = 250;
            clamp_chart_window(app);
            clamp_chart_pan(app);
            break;
        case '-': case '_':
            if (app->interval_ms < 5000) app->interval_ms += 250;
            if (app->interval_ms > 5000) app->interval_ms = 5000;
            clamp_chart_window(app);
            clamp_chart_pan(app);
            break;
    }
    return true;
}

static bool handle_console_key(App *app, const KEY_EVENT_RECORD *event) {
    if (!event->bKeyDown) return true;
    int repeat = event->wRepeatCount > 0 ? event->wRepeatCount : 1;
    if (app->chart_view && !app->editing_filter && !app->confirm_pid) {
        switch (event->wVirtualKeyCode) {
            case VK_LEFT:
                for (int i = 0; i < repeat; ++i) pan_chart(app, 1, false);
                return true;
            case VK_RIGHT:
                for (int i = 0; i < repeat; ++i) pan_chart(app, -1, false);
                return true;
            case VK_PRIOR:
                for (int i = 0; i < repeat; ++i) pan_chart(app, 1, true);
                return true;
            case VK_NEXT:
                for (int i = 0; i < repeat; ++i) pan_chart(app, -1, true);
                return true;
            case VK_HOME:
                clamp_chart_window(app);
                app->chart_pan_ms = chart_max_pan_ms(app);
                return true;
            case VK_END:
                app->chart_pan_ms = 0;
                return true;
        }
    }
    switch (event->wVirtualKeyCode) {
        case VK_UP: select_relative(app, -repeat); return true;
        case VK_DOWN: select_relative(app, repeat); return true;
        case VK_PRIOR: select_relative(app, -10 * repeat); return true;
        case VK_NEXT: select_relative(app, 10 * repeat); return true;
        case VK_HOME: select_relative(app, -(int)app->visible_count); return true;
        case VK_END: select_relative(app, (int)app->visible_count); return true;
        case VK_ESCAPE: return handle_key(app, 27);
        case VK_BACK: return handle_key(app, 8);
        case VK_RETURN: return handle_key(app, 13);
        case VK_TAB: return handle_key(app, '\t');
    }
    wchar_t character = event->uChar.UnicodeChar;
    if (character > 0 && character <= 0x7f) return handle_key(app, (int)character);
    return true;
}

static void handle_console_mouse(App *app, const MOUSE_EVENT_RECORD *event) {
    int x = event->dwMousePosition.X - app->viewport_left;
    int y = event->dwMousePosition.Y - app->viewport_top;
    bool in_chart = app->chart_view &&
        x >= app->chart_plot_left && x <= app->chart_plot_right &&
        y >= app->chart_plot_top && y <= app->chart_plot_bottom;

    if (app->chart_view && event->dwEventFlags == MOUSE_MOVED) {
        app->chart_hover_active = in_chart;
        if (in_chart) {
            app->chart_hover_x = x;
            app->chart_hover_y = y;
        }
        return;
    }
    if (event->dwEventFlags == MOUSE_WHEELED) {
        SHORT delta = (SHORT)HIWORD(event->dwButtonState);
        if (in_chart) {
            app->chart_hover_active = true;
            app->chart_hover_x = x;
            app->chart_hover_y = y;
            if (event->dwControlKeyState & SHIFT_PRESSED)
                pan_chart(app, delta > 0 ? 1 : -1, false);
            else
                zoom_chart(app, delta < 0);
            return;
        }
        select_relative(app, delta > 0 ? -3 : 3);
        return;
    }
    if (event->dwEventFlags != 0 ||
        !(event->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) || app->chart_view) return;

    if (y == app->table_header_row) {
        for (size_t i = 0; i < app->header_hit_count; ++i) {
            HeaderHit *hit = &app->header_hits[i];
            if (x >= hit->start_x && x <= hit->end_x) {
                set_sort(app, hit->mode);
                return;
            }
        }
    }
    if (y >= app->table_first_process_row && y <= app->table_last_process_row) {
        size_t selected = app->scroll + (size_t)(y - app->table_first_process_row);
        if (selected < app->visible_count) {
            app->selection = selected;
            app->selected_pid = app->visible[selected]->pid;
        }
    }
}

static void poll_console_input(App *app, HANDLE input, bool *running, bool *dirty) {
    DWORD available = 0;
    while (*running && GetNumberOfConsoleInputEvents(input, &available) && available > 0) {
        INPUT_RECORD records[32];
        DWORD read = 0;
        DWORD requested = available < _countof(records) ? available : (DWORD)_countof(records);
        if (!ReadConsoleInputW(input, records, requested, &read)) break;
        for (DWORD i = 0; i < read; ++i) {
            switch (records[i].EventType) {
                case KEY_EVENT:
                    *running = handle_console_key(app, &records[i].Event.KeyEvent);
                    *dirty = true;
                    break;
                case MOUSE_EVENT:
                    handle_console_mouse(app, &records[i].Event.MouseEvent);
                    *dirty = true;
                    break;
                case WINDOW_BUFFER_SIZE_EVENT:
                    *dirty = true;
                    break;
            }
            if (!*running) break;
        }
    }
}

static void write_json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04x", *p);
                else putchar(*p);
        }
    }
    putchar('"');
}

static void print_once_json(const App *app) {
    char timestamp[64];
    iso_timestamp(timestamp, sizeof(timestamp));
    fputs("{\"timestamp\":", stdout); write_json_string(timestamp);
    printf(",\"gpu_index\":%u,\"name\":", app->selected_gpu); write_json_string(app->telemetry.name);
    fputs(",\"driver\":", stdout); write_json_string(app->telemetry.driver);
    fputs(",\"uuid\":", stdout); write_json_string(app->telemetry.uuid);
    printf(",\"wddm_process_memory_warning\":%s",
           app->wddm_process_memory_suspect ? "true" : "false");
    printf(",\"direct_process_memory_count\":%zu",
           app->direct_process_memory_count);
    printf(",\"gpu_percent\":%.1f,\"wddm_busiest_engine_percent\":%.1f,\"memory_used_bytes\":%llu,\"memory_total_bytes\":%llu,"
           "\"memory_free_bytes\":%llu,\"memory_reserved_bytes\":%llu,\"memory_budget_bytes\":%llu,\"memory_controller_percent\":%.1f,\"temperature_c\":%d,\"fan_percent\":%d,"
           "\"power_w\":%.2f,\"power_limit_w\":%.2f,\"graphics_clock_mhz\":%d,"
           "\"max_graphics_clock_mhz\":%d,\"sm_clock_mhz\":%d,\"memory_clock_mhz\":%d,\"max_memory_clock_mhz\":%d,"
           "\"pstate\":%d,\"encoder_percent\":%d,\"decoder_percent\":%d,"
           "\"pcie_generation\":%d,\"pcie_width\":%d,\"pcie_tx_mib_s\":%.3f,\"pcie_rx_mib_s\":%.3f,"
           "\"engines\":{\"3d_percent\":%.2f,\"copy_percent\":%.2f,\"video_decode_percent\":%.2f,"
           "\"video_encode_percent\":%.2f,\"compute_percent\":%.2f,\"other_percent\":%.2f},\"processes\":[",
           app->telemetry.gpu_util,
           app->telemetry.wddm_gpu_util,
           (unsigned long long)app->telemetry.memory_used,
           (unsigned long long)app->telemetry.memory_total,
           (unsigned long long)app->telemetry.memory_free,
           (unsigned long long)app->telemetry.memory_reserved,
           (unsigned long long)app->telemetry.memory_budget,
           app->telemetry.memory_util,
           app->telemetry.temperature_c, app->telemetry.fan_percent,
           app->telemetry.power_w, app->telemetry.power_limit_w,
           app->telemetry.graphics_clock_mhz, app->telemetry.max_graphics_clock_mhz,
           app->telemetry.sm_clock_mhz, app->telemetry.memory_clock_mhz,
           app->telemetry.max_memory_clock_mhz, app->telemetry.pstate,
           app->telemetry.encoder_util, app->telemetry.decoder_util,
           app->telemetry.pcie_generation, app->telemetry.pcie_width,
           app->telemetry.pcie_tx_mib_s, app->telemetry.pcie_rx_mib_s,
           app->engines.three_d, app->engines.copy,
           app->engines.video_decode, app->engines.video_encode,
           app->engines.compute, app->engines.other);
    for (size_t i = 0; i < app->visible_count; ++i) {
        const GpuProcess *process = app->visible[i];
        if (i) putchar(',');
        printf("{\"pid\":%lu,\"name\":", (unsigned long)process->pid); write_json_string(process->name);
        printf(",\"gpu_percent\":%.2f,\"dedicated_memory_bytes\":", process->gpu_percent);
        if (process->dedicated_memory_invalid) fputs("null", stdout);
        else printf("%llu", (unsigned long long)process->dedicated_bytes);
        fputs(",\"dedicated_commit_bytes\":", stdout);
        if (process->dedicated_memory_invalid) fputs("null", stdout);
        else printf("%llu", (unsigned long long)process->dedicated_bytes);
        printf(",\"shared_memory_bytes\":%llu,\"shared_commit_bytes\":%llu,\"dedicated_memory_source\":",
               (unsigned long long)process->shared_bytes,
               (unsigned long long)process->shared_bytes);
        write_json_string(dedicated_memory_source(app, process));
        fputs(",\"shared_memory_source\":", stdout);
        write_json_string(shared_memory_source(app, process));
        fputs(",\"engine\":", stdout);
        write_json_string(process->engine);
        putchar('}');
    }
    fputs("]}\n", stdout);
}

static void print_once_table(const App *app) {
    char used[32], total[32], power[32];
    if (app->telemetry.power_w >= 0) snprintf(power, sizeof(power), "%.1f W", app->telemetry.power_w);
    else snprintf(power, sizeof(power), "N/A");
    printf("VGPU-Mon %s\nGPU %u: %s\n", VGPU_VERSION, app->selected_gpu, app->telemetry.name);
    printf("GPU %.1f%% | VRAM %s / %s | Temp ", app->telemetry.gpu_util,
           format_bytes(app->telemetry.memory_used, used, sizeof(used)),
           format_bytes(app->telemetry.memory_total, total, sizeof(total)));
    if (app->telemetry.temperature_c >= 0) printf("%d C", app->telemetry.temperature_c);
    else printf("N/A");
    printf(" | Power %s\n\n", power);
    printf("%-8s %-32s %8s %12s %12s %-14s\n", "PID", "PROCESS", "GPU", "DEDICATED*", "SHARED*", "ENGINE");
    for (size_t i = 0; i < app->visible_count; ++i) {
        const GpuProcess *process = app->visible[i];
        char name[64], dedicated[32], shared[32];
        truncate_text(process->name, name, sizeof(name), 32);
        if (process->dedicated_memory_invalid) snprintf(dedicated, sizeof(dedicated), "N/A !");
        else format_bytes(process->dedicated_bytes, dedicated, sizeof(dedicated));
        printf("%-8lu %-32s %7.1f%% %12s %12s %-14s\n", (unsigned long)process->pid, name,
               process->gpu_percent, dedicated,
               format_bytes(process->shared_bytes, shared, sizeof(shared)), process->engine);
    }
}

static bool parse_chart_metric(const char *name, ChartMetric *metric) {
    if (!name || !metric) return false;
    if (_stricmp(name, "gpu") == 0 || _stricmp(name, "percent") == 0 ||
        _stricmp(name, "utilization") == 0) *metric = CHART_GPU;
    else if (_stricmp(name, "vram") == 0) *metric = CHART_VRAM;
    else if (_stricmp(name, "memory-controller") == 0 || _stricmp(name, "memory") == 0) *metric = CHART_MEMORY_CONTROLLER;
    else if (_stricmp(name, "3d") == 0) *metric = CHART_3D;
    else if (_stricmp(name, "copy") == 0) *metric = CHART_COPY;
    else if (_stricmp(name, "decode") == 0 || _stricmp(name, "video-decode") == 0) *metric = CHART_VIDEO_DECODE;
    else if (_stricmp(name, "encode") == 0 || _stricmp(name, "video-encode") == 0) *metric = CHART_VIDEO_ENCODE;
    else if (_stricmp(name, "compute") == 0) *metric = CHART_COMPUTE;
    else if (_stricmp(name, "temperature") == 0 || _stricmp(name, "temp") == 0) *metric = CHART_TEMPERATURE;
    else if (_stricmp(name, "power") == 0) *metric = CHART_POWER;
    else return false;
    return true;
}

static void print_help(void) {
    fputs(
        "VGPU-Mon " VGPU_VERSION " - live Windows GPU process monitor\n\n"
        "Usage: vgpu [options]\n\n"
        "  --once             Print one snapshot and exit\n"
        "  --json             Print one snapshot as JSON\n"
        "  --chart [METRIC]   Open a full-screen history chart (default: gpu)\n"
        "  --vram             Chart allocated VRAM percentage\n"
        "  --percent          Chart overall GPU utilization\n"
        "  --3d, --copy       Chart the selected WDDM engine type\n"
        "  --decode, --encode Chart video decode or encode engines\n"
        "  --compute          Chart compute-engine utilization\n"
        "  --memory-controller, --temperature, --power\n"
        "                     Chart another board metric\n"
        "  --gpu INDEX        Select physical GPU (default 0)\n"
        "  --interval MS      Sampling interval, 250-5000 (default 1000)\n"
        "  --log PATH         Start interactive CSV logging immediately\n"
        "  --demo             Use deterministic sample data (UI preview/testing)\n"
        "  --update           Check for and install an update now\n"
        "  --no-update        Skip the automatic update check for this run\n"
        "  --help             Show this help\n"
        "  --version          Show the version\n\n"
        "Run without options in Windows Terminal for the interactive UI.\n"
        "Headers are mouse-clickable; click again to reverse the sort.\n"
        "Charts support wheel zoom, Shift+wheel or arrow-key panning, and exact hover values.\n",
        stdout);
}

static bool initialize_app(App *app) {
    if (app->demo_mode) {
        app->gpu_count = 1;
        return true;
    }
    app->nvml_ready = nvml_open(&app->nvml);
    app->dxgi_ready = dxgi_open(&app->dxgi);
    if (app->dxgi_ready) {
        app->d3dkmt_ready = d3dkmt_gpu_open(
            &app->d3dkmt, app->dxgi.adapter_luids, app->dxgi.count);
    }
    app->pdh_ready = pdh_gpu_open(&app->pdh);
    app->gpu_count = app->dxgi_ready ? app->dxgi.count : 0;
    if (app->nvml_ready && app->nvml.device_count > app->gpu_count) app->gpu_count = app->nvml.device_count;
    if (app->gpu_count > VGPU_MAX_GPUS) app->gpu_count = VGPU_MAX_GPUS;
    return app->gpu_count > 0;
}

static void cleanup_app(App *app) {
    stop_logging(app);
    free(app->chart_plot_present);
    free(app->chart_plot_values);
    app->chart_plot_present = NULL;
    app->chart_plot_values = NULL;
    app->chart_plot_capacity = 0;
    pdh_gpu_close(&app->pdh);
    d3dkmt_gpu_close(&app->d3dkmt);
    dxgi_close(&app->dxgi);
    nvml_close(&app->nvml);
    app->pdh_ready = false;
    app->d3dkmt_ready = false;
    app->dxgi_ready = false;
    app->nvml_ready = false;
}

static bool parse_unsigned_option(const char *text, unsigned int minimum,
                                  unsigned int maximum, unsigned int *value) {
    if (!text || !*text || !value || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum) return false;
    *value = (unsigned int)parsed;
    return true;
}

static int app_main(int argc, char **argv) {
    App *app = (App *)calloc(1, sizeof(App));
    if (!app) {
        fputs("VGPU-Mon: out of memory\n", stderr);
        return 1;
    }
    app->interval_ms = 1000;
    app->sort_mode = SORT_GPU;
    app->sort_descending = true;
    app->chart_metric = CHART_GPU;
    app->chart_window_ms = VGPU_CHART_DEFAULT_WINDOW_MS;
    bool once = false;
    bool json = false;
    const char *initial_log = NULL;

    /* Consume arguments from left to right; value-taking options advance the
       cursor explicitly so malformed or missing values fail predictably. */
    int argument_index = 1;
    while (argument_index < argc) {
        const char *argument = argv[argument_index++];
        if (strcmp(argument, "--once") == 0) once = true;
        else if (strcmp(argument, "--json") == 0) { json = true; once = true; }
        else if (strcmp(argument, "--chart") == 0) {
            app->chart_view = true;
            if (argument_index < argc && argv[argument_index][0] != '-') {
                const char *metric = argv[argument_index++];
                if (!parse_chart_metric(metric, &app->chart_metric)) {
                    fprintf(stderr, "Unknown chart metric: %s\n", metric);
                    free(app);
                    return 2;
                }
            }
        }
        else if (strcmp(argument, "--vram") == 0) { app->chart_view = true; app->chart_metric = CHART_VRAM; }
        else if (strcmp(argument, "--percent") == 0 || strcmp(argument, "--utilization") == 0) { app->chart_view = true; app->chart_metric = CHART_GPU; }
        else if (strcmp(argument, "--memory-controller") == 0) { app->chart_view = true; app->chart_metric = CHART_MEMORY_CONTROLLER; }
        else if (strcmp(argument, "--3d") == 0) { app->chart_view = true; app->chart_metric = CHART_3D; }
        else if (strcmp(argument, "--copy") == 0) { app->chart_view = true; app->chart_metric = CHART_COPY; }
        else if (strcmp(argument, "--decode") == 0 || strcmp(argument, "--video-decode") == 0) { app->chart_view = true; app->chart_metric = CHART_VIDEO_DECODE; }
        else if (strcmp(argument, "--encode") == 0 || strcmp(argument, "--video-encode") == 0) { app->chart_view = true; app->chart_metric = CHART_VIDEO_ENCODE; }
        else if (strcmp(argument, "--compute") == 0) { app->chart_view = true; app->chart_metric = CHART_COMPUTE; }
        else if (strcmp(argument, "--temperature") == 0 || strcmp(argument, "--temp") == 0) { app->chart_view = true; app->chart_metric = CHART_TEMPERATURE; }
        else if (strcmp(argument, "--power") == 0) { app->chart_view = true; app->chart_metric = CHART_POWER; }
        else if (strcmp(argument, "--gpu") == 0 && argument_index < argc) {
            const char *value = argv[argument_index++];
            if (!parse_unsigned_option(value, 0, VGPU_MAX_GPUS - 1, &app->selected_gpu)) {
                fprintf(stderr, "Invalid GPU index: %s\n", value);
                free(app);
                return 2;
            }
        }
        else if (strcmp(argument, "--interval") == 0 && argument_index < argc) {
            const char *value = argv[argument_index++];
            if (!parse_unsigned_option(value, 250, 5000, &app->interval_ms)) {
                fprintf(stderr, "Invalid interval: %s (expected 250-5000)\n", value);
                free(app);
                return 2;
            }
        }
        else if (strcmp(argument, "--log") == 0 && argument_index < argc) initial_log = argv[argument_index++];
        else if (strcmp(argument, "--demo") == 0) app->demo_mode = true;
        else if (strcmp(argument, "--no-update") == 0) { /* handled before app initialization */ }
        else if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) { print_help(); free(app); return 0; }
        else if (strcmp(argument, "--version") == 0) { printf("VGPU-Mon %s\n", VGPU_VERSION); free(app); return 0; }
        else { fprintf(stderr, "Unknown or incomplete option: %s\n", argument); print_help(); free(app); return 2; }
    }
    if (app->chart_view && once) {
        fputs("VGPU-Mon: --chart cannot be combined with --once or --json. Use --json for redirected telemetry.\n", stderr);
        free(app);
        return 2;
    }

    /* Provider initialization is deliberately followed by unconditional,
       idempotent cleanup on every exit path so partial opens cannot leak. */
    if (!initialize_app(app)) {
        fprintf(stderr, "VGPU-Mon: no hardware GPU was found.\nNVML: %s\nDXGI: %s\n",
                app->nvml.error[0] ? app->nvml.error : "unavailable",
                app->dxgi.error[0] ? app->dxgi.error : "unavailable");
        cleanup_app(app);
        free(app);
        return 1;
    }
    if (app->selected_gpu >= app->gpu_count) {
        fprintf(stderr, "VGPU-Mon: GPU index %u does not exist (found %u GPU%s).\n",
                app->selected_gpu, app->gpu_count, app->gpu_count == 1 ? "" : "s");
        cleanup_app(app);
        free(app);
        return 2;
    }
    DWORD output_mode = 0;
    bool output_is_console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode) != FALSE;
    if (!output_is_console && app->chart_view) {
        fputs("VGPU-Mon: chart mode requires an interactive Windows terminal. Use --json when redirecting output.\n", stderr);
        cleanup_app(app);
        free(app);
        return 1;
    }
    if (!output_is_console) once = true;
    if (initial_log) start_logging(app, initial_log);

    Sleep(app->interval_ms);
    sample_app(app);
    if (once) {
        if (json) print_once_json(app); else print_once_table(app);
        cleanup_app(app);
        free(app);
        return 0;
    }

    ConsoleState console;
    if (!console_enter(&console)) {
        fputs("VGPU-Mon: interactive mode requires a Windows console with VT support. Use --once instead.\n", stderr);
        cleanup_app(app);
        free(app);
        return 1;
    }
    SetConsoleCtrlHandler(console_control_handler, TRUE);

    TextBuffer screen, presentation;
    text_buffer_init(&screen, 512 * 1024);
    text_buffer_init(&presentation, 1024 * 1024);
    if (!screen.data || !presentation.data) {
        text_buffer_free(&presentation);
        text_buffer_free(&screen);
        SetConsoleCtrlHandler(console_control_handler, FALSE);
        console_leave(&console);
        cleanup_app(app);
        free(app);
        fputs("VGPU-Mon: could not allocate the render buffer.\n", stderr);
        return 1;
    }
    ULONGLONG next_sample = GetTickCount64() + app->interval_ms;
    bool running = true;
    /* The interactive loop separates sampling from repaint requests and
       always composes one bounded frame for the current terminal geometry. */
    while (running && !InterlockedCompareExchange(&g_interrupted, 0, 0)) {
        bool dirty = false;
        poll_console_input(app, console.input, &running, &dirty);

        ULONGLONG now = GetTickCount64();
        if (!app->paused && now >= next_sample) {
            sample_app(app);
            next_sample = now + app->interval_ms;
            dirty = true;
        } else if (app->paused) {
            next_sample = now + app->interval_ms;
        }
        if (app->message_until && now >= app->message_until) {
            app->message_until = 0;
            dirty = true;
        }

        int current_columns, current_rows;
        terminal_geometry(&current_columns, &current_rows, NULL, NULL);
        if (current_columns != app->last_columns || current_rows != app->last_rows) {
            dirty = true;
        }

        if (dirty) {
            text_buffer_reset(&screen);
            render_app(app, &screen);
            compose_terminal_frame(&screen, &presentation, current_rows);
            fwrite(presentation.data, 1, presentation.length, stdout);
            fflush(stdout);
            app->last_columns = current_columns;
            app->last_rows = current_rows;
        }
        Sleep(20);
    }

    text_buffer_free(&presentation);
    text_buffer_free(&screen);
    SetConsoleCtrlHandler(console_control_handler, FALSE);
    console_leave(&console);
    cleanup_app(app);
    free(app);
    return 0;
}

int wmain(int argc, wchar_t **wide_argv) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF);
#endif
    bool force_update = updater_is_forced(argc, wide_argv);
    DWORD input_mode = 0, output_mode = 0;
    bool interactive_console =
        GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode) != FALSE &&
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode) != FALSE;
    if (force_update || (interactive_console && updater_should_check(argc, wide_argv))) {
        VgpuUpdateResult update = updater_check_and_start(
            argc, wide_argv, VGPU_VERSION, force_update);
        if (update == VGPU_UPDATE_STARTED) return 0;
        if (force_update) {
            if (update == VGPU_UPDATE_CURRENT) {
                printf("VGPU-Mon %s is already up to date.\n", VGPU_VERSION);
                return 0;
            }
            if (update == VGPU_UPDATE_SKIPPED) {
                fputs("VGPU-Mon: the update check was skipped.\n", stderr);
            } else {
                fputs("VGPU-Mon: the update check failed; the current version was not changed.\n",
                      stderr);
            }
            return 1;
        }
    }
    char **argv = (char **)calloc((size_t)argc, sizeof(*argv));
    if (!argv) {
        fputs("VGPU-Mon: out of memory while reading arguments.\n", stderr);
        return 1;
    }
    for (int i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0) {
            for (int j = 0; j < i; ++j) free(argv[j]);
            free(argv);
            fputs("VGPU-Mon: an argument could not be converted to UTF-8.\n", stderr);
            return 2;
        }
        argv[i] = (char *)malloc((size_t)bytes);
        if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1,
                                              argv[i], bytes, NULL, NULL)) {
            for (int j = 0; j <= i; ++j) free(argv[j]);
            free(argv);
            fputs("VGPU-Mon: out of memory while reading arguments.\n", stderr);
            return 1;
        }
    }
    int result = app_main(argc, argv);
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
#ifdef _DEBUG
    if (_CrtDumpMemoryLeaks() && result == 0) result = 3;
#endif
    return result;
}
