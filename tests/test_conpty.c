#define _CRT_SECURE_NO_WARNINGS
#include "../src/vgpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#define CAPTURE_CAPACITY (4U * 1024U * 1024U)

typedef struct {
    HANDLE pipe;
    char *data;
    size_t length;
    bool overflow;
} Capture;

static DWORD WINAPI reader_thread(void *parameter) {
    Capture *capture = (Capture *)parameter;
    char block[4096];
    DWORD read = 0;
    while (ReadFile(capture->pipe, block, sizeof(block), &read, NULL) && read > 0) {
        size_t remaining = CAPTURE_CAPACITY - capture->length - 1;
        size_t copy = read < remaining ? read : remaining;
        if (copy) {
            memcpy(capture->data + capture->length, block, copy);
            capture->length += copy;
            capture->data[capture->length] = '\0';
        }
        if (copy < read) capture->overflow = true;
    }
    return 0;
}

static size_t count_occurrences(const char *text, const char *needle) {
    size_t count = 0;
    size_t needle_length = strlen(needle);
    const char *cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_length;
    }
    return count;
}

static size_t max_complete_frame_newlines(const char *text) {
    const char *home = "\x1b[H";
    size_t maximum = 0;
    const char *frame = strstr(text, home);
    while (frame) {
        const char *next = strstr(frame + 3, home);
        if (!next) break;
        size_t lines = 0;
        for (const char *cursor = frame + 3; cursor < next; ++cursor) {
            if (*cursor == '\n') lines++;
        }
        if (lines > maximum) maximum = lines;
        frame = next;
    }
    return maximum;
}

static size_t max_complete_frame_columns(const char *text) {
    const char *home = "\x1b[H";
    size_t maximum = 0;
    const char *frame = strstr(text, home);
    while (frame) {
        const char *next = strstr(frame + 3, home);
        if (!next) break;
        size_t column = 0;
        const unsigned char *cursor = (const unsigned char *)(frame + 3);
        const unsigned char *end = (const unsigned char *)next;
        while (cursor < end) {
            if (*cursor == 0x1b && cursor + 1 < end && cursor[1] == '[') {
                cursor += 2;
                while (cursor < end && !(*cursor >= 0x40 && *cursor <= 0x7e)) cursor++;
                if (cursor < end) cursor++;
                continue;
            }
            if (*cursor == '\r') {
                column = 0;
                cursor++;
                continue;
            }
            if (*cursor == '\n') {
                if (column > maximum) maximum = column;
                column = 0;
                cursor++;
                continue;
            }
            if (*cursor >= 0x20 && *cursor != 0x7f) {
                column++;
                if ((*cursor & 0xe0U) == 0xc0U) cursor += 2;
                else if ((*cursor & 0xf0U) == 0xe0U) cursor += 3;
                else if ((*cursor & 0xf8U) == 0xf0U) cursor += 4;
                else cursor++;
                continue;
            }
            cursor++;
        }
        if (column > maximum) maximum = column;
        frame = next;
    }
    return maximum;
}

static void close_if_valid(HANDLE handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

static void set_std_handle_for_conpty(DWORD stream, HANDLE handle) {
#pragma warning(suppress: 6387)
    SetStdHandle(stream, handle);
}

static bool finish_reader(HANDLE reader, HANDLE *pipe) {
    DWORD wait = WaitForSingleObject(reader, 5000);
    if (wait == WAIT_OBJECT_0) return true;
    CancelSynchronousIo(reader);
    if (pipe && *pipe) {
        close_if_valid(*pipe);
        *pipe = NULL;
    }
    return WaitForSingleObject(reader, 5000) == WAIT_OBJECT_0;
}

int main(int argc, char **argv) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF);
#endif
    const char *target_utf8 = argc > 1 ? argv[1] : "build\\vgpu-mon.exe";
    wchar_t target[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, target_utf8, -1, target, _countof(target))) {
        fputs("ConPTY test: invalid target path.\n", stderr);
        return 1;
    }
    wchar_t full_target[MAX_PATH];
    if (!GetFullPathNameW(target, _countof(full_target), full_target, NULL)) {
        fputs("ConPTY test: could not resolve target path.\n", stderr);
        return 1;
    }

    HANDLE input_read = NULL, input_write = NULL;
    HANDLE output_read = NULL, output_write = NULL;
    if (!CreatePipe(&input_read, &input_write, NULL, 0) ||
        !CreatePipe(&output_read, &output_write, NULL, 0)) {
        fputs("ConPTY test: CreatePipe failed.\n", stderr);
        close_if_valid(input_read);
        close_if_valid(input_write);
        close_if_valid(output_read);
        close_if_valid(output_write);
        return 1;
    }

    HPCON pseudo_console = NULL;
    COORD initial_size = {120, 30};
    HRESULT result = CreatePseudoConsole(initial_size, input_read, output_write, 0, &pseudo_console);
    close_if_valid(input_read);
    close_if_valid(output_write);
    if (FAILED(result)) {
        fprintf(stderr, "ConPTY test: CreatePseudoConsole failed (0x%08lx).\n", (unsigned long)result);
        close_if_valid(input_write);
        close_if_valid(output_read);
        return 1;
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attribute_size);
    bool attribute_list_initialized = startup.lpAttributeList &&
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size);
    if (!attribute_list_initialized ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudo_console, sizeof(pseudo_console), NULL, NULL)) {
        fputs("ConPTY test: could not prepare process attributes.\n", stderr);
        if (attribute_list_initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (startup.lpAttributeList) HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        ClosePseudoConsole(pseudo_console);
        close_if_valid(input_write);
        close_if_valid(output_read);
        return 1;
    }

    Capture capture;
    memset(&capture, 0, sizeof(capture));
    capture.pipe = output_read;
    capture.data = (char *)calloc(1, CAPTURE_CAPACITY);
    HANDLE reader = capture.data ? CreateThread(NULL, 0, reader_thread, &capture, 0, NULL) : NULL;
    if (!capture.data || !reader) {
        fputs("ConPTY test: could not start output reader.\n", stderr);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        ClosePseudoConsole(pseudo_console);
        close_if_valid(input_write);
        close_if_valid(output_read);
        free(capture.data);
        return 1;
    }

    wchar_t command_line[MAX_PATH + 128];
    _snwprintf_s(command_line, _countof(command_line), _TRUNCATE,
                 L"\"%ls\" --demo --interval 250 --chart vram", full_target);
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    HANDLE old_stdin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE old_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE old_stderr = GetStdHandle(STD_ERROR_HANDLE);
    set_std_handle_for_conpty(STD_INPUT_HANDLE, NULL);
    set_std_handle_for_conpty(STD_OUTPUT_HANDLE, NULL);
    set_std_handle_for_conpty(STD_ERROR_HANDLE, NULL);
    BOOL created = CreateProcessW(NULL, command_line, NULL, NULL, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                  NULL, NULL, &startup.StartupInfo, &process);
    set_std_handle_for_conpty(STD_INPUT_HANDLE, old_stdin);
    set_std_handle_for_conpty(STD_OUTPUT_HANDLE, old_stdout);
    set_std_handle_for_conpty(STD_ERROR_HANDLE, old_stderr);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    if (!created) {
        fprintf(stderr, "ConPTY test: CreateProcess failed (%lu).\n", (unsigned long)GetLastError());
        ClosePseudoConsole(pseudo_console);
        close_if_valid(input_write);
        bool reader_finished = finish_reader(reader, &output_read);
        close_if_valid(reader);
        close_if_valid(output_read);
        if (reader_finished) free(capture.data);
        return 1;
    }

    Sleep(700);
    DWORD written = 0;
    WriteFile(input_write, "c", 1, &written, NULL);
    Sleep(400);
    const char mouse_click[] = "\x1b[<0;72;11M\x1b[<0;72;11m";
    WriteFile(input_write, mouse_click, (DWORD)(sizeof(mouse_click) - 1), &written, NULL);
    Sleep(300);
    COORD small = {60, 12};
    HRESULT resize_small = ResizePseudoConsole(pseudo_console, small);
    Sleep(500);
    COORD large = {120, 30};
    HRESULT resize_large = ResizePseudoConsole(pseudo_console, large);
    Sleep(500);
    WriteFile(input_write, "q", 1, &written, NULL);

    DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 99);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = 99;
    GetExitCodeProcess(process.hProcess, &exit_code);
    close_if_valid(process.hThread);
    close_if_valid(process.hProcess);
    close_if_valid(input_write);
    ClosePseudoConsole(pseudo_console);
    bool reader_finished = finish_reader(reader, &output_read);
    close_if_valid(reader);
    close_if_valid(output_read);
    if (!reader_finished) {
        fputs("ConPTY test: output reader did not stop.\n", stderr);
        return 1;
    }

    size_t clears = count_occurrences(capture.data, "\x1b[2J");
    size_t line_erases = count_occurrences(capture.data, "\x1b[K");
    size_t max_frame_lines = max_complete_frame_newlines(capture.data);
    size_t max_frame_columns = max_complete_frame_columns(capture.data);
    bool entered_alternate = strstr(capture.data, "\x1b[?1049h") != NULL;
    bool left_alternate = strstr(capture.data, "\x1b[?1049l") != NULL;
    bool rendered_chart = strstr(capture.data, "Allocated VRAM") != NULL;
    bool clicked_sort = strstr(capture.data, "GPU ^") != NULL;
    bool no_debug_leaks = strstr(capture.data, "Detected memory leaks!") == NULL;
    bool passed = SUCCEEDED(resize_small) && SUCCEEDED(resize_large) && wait == WAIT_OBJECT_0 &&
                  exit_code == 0 && clears >= 1 && line_erases >= 20 && max_frame_lines <= 29 &&
                  max_frame_columns <= 120 &&
                  entered_alternate && left_alternate && rendered_chart && clicked_sort &&
                  no_debug_leaks && !capture.overflow;
    if (!passed) {
        fprintf(stderr,
                "ConPTY test failed: exit=%lu wait=%lu clears=%zu line-erases=%zu max-frame-newlines=%zu max-frame-columns=%zu enter=%d leave=%d chart=%d mouse-sort=%d no-leaks=%d overflow=%d resize=0x%08lx/0x%08lx bytes=%zu\n",
                (unsigned long)exit_code, (unsigned long)wait, clears, line_erases,
                max_frame_lines, max_frame_columns,
                entered_alternate, left_alternate, rendered_chart, clicked_sort, no_debug_leaks, capture.overflow,
                (unsigned long)resize_small, (unsigned long)resize_large, capture.length);
        fputs("--- captured pseudoconsole output ---\n", stderr);
        fwrite(capture.data, 1, capture.length, stderr);
        fputs("\n--- end captured output ---\n", stderr);
        free(capture.data);
        return 1;
    }

    printf("ConPTY resize/chart test passed (%zu bytes, %zu line erases, max %zu x %zu frame bounds).\n",
           capture.length, line_erases, max_frame_columns, max_frame_lines + 1);
    free(capture.data);
#ifdef _DEBUG
    if (_CrtDumpMemoryLeaks()) return 2;
#endif
    return 0;
}
