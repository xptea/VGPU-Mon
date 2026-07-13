#define _CRT_SECURE_NO_WARNINGS
#include "vgpu.h"

#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

const char *format_bytes(uint64_t bytes, char *buffer, size_t buffer_size) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(buffer, buffer_size, "%llu %s", (unsigned long long)bytes, units[unit]);
    } else if (value >= 100.0) {
        snprintf(buffer, buffer_size, "%.0f %s", value, units[unit]);
    } else if (value >= 10.0) {
        snprintf(buffer, buffer_size, "%.1f %s", value, units[unit]);
    } else {
        snprintf(buffer, buffer_size, "%.2f %s", value, units[unit]);
    }
    return buffer;
}

const char *format_rate(double mib_s, char *buffer, size_t buffer_size) {
    if (mib_s < 1.0) {
        snprintf(buffer, buffer_size, "%.0f KiB/s", mib_s * 1024.0);
    } else if (mib_s < 1024.0) {
        snprintf(buffer, buffer_size, "%.1f MiB/s", mib_s);
    } else {
        snprintf(buffer, buffer_size, "%.2f GiB/s", mib_s / 1024.0);
    }
    return buffer;
}

void wide_to_utf8(const wchar_t *source, char *target, size_t target_size) {
    if (!target || target_size == 0) return;
    target[0] = '\0';
    if (!source) return;
    int result = WideCharToMultiByte(CP_UTF8, 0, source, -1, target, (int)target_size, NULL, NULL);
    if (result == 0) target[0] = '\0';
    target[target_size - 1] = '\0';
}

void sanitize_csv_field(const char *source, char *target, size_t target_size) {
    if (!target || target_size == 0) return;
    size_t out = 0;
    if (!source) source = "";
    if ((*source == '=' || *source == '+' || *source == '-' || *source == '@') &&
        out + 1 < target_size) {
        target[out++] = '\'';
    }
    for (size_t i = 0; source[i] && out + 1 < target_size; ++i) {
        char c = source[i];
        if (c == '"') {
            if (out + 2 >= target_size) break;
            target[out++] = '"';
            target[out++] = '"';
        } else if (c == '\r' || c == '\n') {
            target[out++] = ' ';
        } else {
            target[out++] = c;
        }
    }
    target[out] = '\0';
}

bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t needle_length = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < needle_length && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_length) return true;
    }
    return false;
}

bool dedicated_gpu_memory_plausible(uint64_t bytes, uint64_t physical_total) {
    return physical_total == 0 || bytes <= physical_total;
}

size_t invalidate_implausible_dedicated_gpu_memory(
    GpuProcess *processes, size_t count, uint64_t physical_total,
    uint64_t board_memory_used, bool board_memory_used_available) {
    if (!processes) return 0;
    uint64_t ceiling = physical_total;
    if (board_memory_used_available && board_memory_used > 0) {
        const uint64_t minimum_slack = 256ULL * 1024ULL * 1024ULL;
        uint64_t slack = board_memory_used / 8ULL;
        if (slack < minimum_slack) slack = minimum_slack;
        uint64_t used_ceiling = UINT64_MAX - board_memory_used < slack
            ? UINT64_MAX : board_memory_used + slack;
        if (ceiling == 0 || used_ceiling < ceiling) ceiling = used_ceiling;
    }
    if (ceiling == 0) return 0;

    size_t invalid_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].dedicated_bytes > ceiling) {
            processes[i].dedicated_memory_invalid = true;
            processes[i].dedicated_bytes = 0;
            invalid_count++;
        }
    }
    return invalid_count;
}

void iso_timestamp(char *buffer, size_t buffer_size) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buffer, buffer_size, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void filetime_to_text(const FILETIME *file_time, char *buffer, size_t buffer_size) {
    FILETIME local_time;
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(file_time, &local_time) && FileTimeToSystemTime(&local_time, &st)) {
        snprintf(buffer, buffer_size, "%04u-%02u-%02u %02u:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    } else {
        snprintf(buffer, buffer_size, "Unavailable");
    }
}

static void query_process_user(HANDLE process, char *buffer, size_t buffer_size) {
    HANDLE token = NULL;
    buffer[0] = '\0';
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    TOKEN_USER *token_user = needed ? (TOKEN_USER *)malloc(needed) : NULL;
    if (token_user && GetTokenInformation(token, TokenUser, token_user, needed, &needed)) {
        wchar_t user[128] = L"";
        wchar_t domain[128] = L"";
        DWORD user_size = (DWORD)(sizeof(user) / sizeof(user[0]));
        DWORD domain_size = (DWORD)(sizeof(domain) / sizeof(domain[0]));
        SID_NAME_USE use;
        if (LookupAccountSidW(NULL, token_user->User.Sid, user, &user_size,
                              domain, &domain_size, &use)) {
            wchar_t combined[260];
            if (domain[0]) _snwprintf_s(combined, _countof(combined), _TRUNCATE, L"%ls\\%ls", domain, user);
            else _snwprintf_s(combined, _countof(combined), _TRUNCATE, L"%ls", user);
            wide_to_utf8(combined, buffer, buffer_size);
        }
    }
    free(token_user);
    CloseHandle(token);
}

bool query_process_details(DWORD pid, ProcessDetails *details) {
    if (!details) return false;
    memset(details, 0, sizeof(*details));
    snprintf(details->path, sizeof(details->path), "Access denied or process exited");
    snprintf(details->user, sizeof(details->user), "Unavailable");
    snprintf(details->started, sizeof(details->started), "Unavailable");

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    details->accessible = true;

    wchar_t path[1024];
    DWORD path_size = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(process, 0, path, &path_size)) {
        wide_to_utf8(path, details->path, sizeof(details->path));
    }

    PROCESS_MEMORY_COUNTERS_EX memory;
    memset(&memory, 0, sizeof(memory));
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory))) {
        details->working_set = (uint64_t)memory.WorkingSetSize;
        details->private_bytes = (uint64_t)memory.PrivateUsage;
    }

    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        filetime_to_text(&created, details->started, sizeof(details->started));
    }
    query_process_user(process, details->user, sizeof(details->user));
    CloseHandle(process);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == pid) details->threads++;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return true;
}

bool terminate_process_safely(DWORD pid, char *message, size_t message_size) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        snprintf(message, message_size, "Refused to terminate protected PID %lu", (unsigned long)pid);
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        snprintf(message, message_size, "Cannot open PID %lu (Win32 error %lu)",
                 (unsigned long)pid, (unsigned long)GetLastError());
        return false;
    }
    bool ok = TerminateProcess(process, 1) != FALSE;
    if (ok) snprintf(message, message_size, "Terminated PID %lu", (unsigned long)pid);
    else snprintf(message, message_size, "Failed to terminate PID %lu (Win32 error %lu)",
                  (unsigned long)pid, (unsigned long)GetLastError());
    CloseHandle(process);
    return ok;
}
