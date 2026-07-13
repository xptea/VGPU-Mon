#define _CRT_SECURE_NO_WARNINGS
#include "updater.h"

#include <bcrypt.h>
#include <limits.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

#define UPDATE_MANIFEST_URL \
    L"https://github.com/xptea/VGPU-Mon/releases/latest/download/version.txt"
#define UPDATE_MAX_MANIFEST_BYTES 1024U
#define UPDATE_MAX_INSTALLER_BYTES (64U * 1024U * 1024U)
#define UPDATE_HELPER_CAPACITY (256U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} ScriptBuffer;

static bool wide_argument_equals(const wchar_t *argument, const wchar_t *expected) {
    return argument && expected && _wcsicmp(argument, expected) == 0;
}

bool updater_parse_version(const char *text, unsigned int parts[3]) {
    if (!text || !parts || !*text) return false;
    const char *cursor = text;
    for (size_t index = 0; index < 3; ++index) {
        if (*cursor < '0' || *cursor > '9') return false;
        if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9') return false;
        unsigned long value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            unsigned int digit = (unsigned int)(*cursor - '0');
            if (value > (UINT_MAX - digit) / 10UL) return false;
            value = value * 10UL + digit;
            cursor++;
        }
        parts[index] = (unsigned int)value;
        if (index < 2) {
            if (*cursor != '.') return false;
            cursor++;
        }
    }
    return *cursor == '\0';
}

bool updater_compare_versions(const char *left, const char *right, int *result) {
    unsigned int left_parts[3], right_parts[3];
    if (!result || !updater_parse_version(left, left_parts) ||
        !updater_parse_version(right, right_parts)) return false;
    *result = 0;
    for (size_t index = 0; index < 3; ++index) {
        if (left_parts[index] < right_parts[index]) {
            *result = -1;
            break;
        }
        if (left_parts[index] > right_parts[index]) {
            *result = 1;
            break;
        }
    }
    return true;
}

static char *trim_ascii(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
    return text;
}

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool updater_parse_manifest(const char *text, VgpuUpdateManifest *manifest) {
    if (!text || !manifest) return false;
    size_t length = strnlen_s(text, UPDATE_MAX_MANIFEST_BYTES + 1U);
    if (length == 0 || length > UPDATE_MAX_MANIFEST_BYTES) return false;

    char copy[UPDATE_MAX_MANIFEST_BYTES + 1U];
    memcpy(copy, text, length + 1U);
    memset(manifest, 0, sizeof(*manifest));
    bool have_version = false, have_installer = false, have_hash = false;

    char *context = NULL;
    for (char *line = strtok_s(copy, "\n", &context); line;
         line = strtok_s(NULL, "\n", &context)) {
        char *trimmed_line = trim_ascii(line);
        if (!*trimmed_line) continue;
        char *separator = strchr(trimmed_line, '=');
        if (!separator) return false;
        *separator = '\0';
        char *key = trim_ascii(trimmed_line);
        char *value = trim_ascii(separator + 1);

        if (strcmp(key, "version") == 0) {
            unsigned int parts[3];
            if (have_version || !updater_parse_version(value, parts) ||
                strlen(value) >= sizeof(manifest->version)) return false;
            strcpy_s(manifest->version, sizeof(manifest->version), value);
            have_version = true;
        } else if (strcmp(key, "installer") == 0) {
            if (have_installer || !*value ||
                strlen(value) >= sizeof(manifest->installer_name)) return false;
            strcpy_s(manifest->installer_name, sizeof(manifest->installer_name), value);
            have_installer = true;
        } else if (strcmp(key, "sha256") == 0) {
            if (have_hash || strlen(value) != 64U) return false;
            for (size_t index = 0; index < 32; ++index) {
                int high = hex_digit(value[index * 2U]);
                int low = hex_digit(value[index * 2U + 1U]);
                if (high < 0 || low < 0) return false;
                manifest->sha256[index] = (unsigned char)((high << 4) | low);
            }
            have_hash = true;
        } else {
            return false;
        }
    }

    if (!have_version || !have_installer || !have_hash) return false;
    char expected[VGPU_UPDATE_INSTALLER_CAP];
    int written = _snprintf_s(expected, sizeof(expected), _TRUNCATE,
                              "VGPU-Mon-%s-setup.exe", manifest->version);
    return written > 0 && strcmp(expected, manifest->installer_name) == 0;
}

bool updater_is_forced(int argc, wchar_t **argv) {
    for (int index = 1; index < argc; ++index) {
        if (wide_argument_equals(argv[index], L"--update")) return true;
    }
    return false;
}

bool updater_should_check(int argc, wchar_t **argv) {
    wchar_t disabled[8];
    DWORD disabled_length = GetEnvironmentVariableW(
        L"VGPU_MON_NO_UPDATE", disabled, (DWORD)_countof(disabled));
    if (disabled_length >= _countof(disabled)) return false;
    if (disabled_length > 0 && !wide_argument_equals(disabled, L"0")) return false;

    static const wchar_t *skip_arguments[] = {
        L"--no-update", L"--once", L"--json", L"--version",
        L"--help", L"-h", L"--demo"
    };
    for (int index = 1; index < argc; ++index) {
        for (size_t skip = 0; skip < _countof(skip_arguments); ++skip) {
            if (wide_argument_equals(argv[index], skip_arguments[skip])) return false;
        }
    }
    return true;
}

static bool get_http_components(const wchar_t *url, wchar_t *host, size_t host_cap,
                                wchar_t *object, size_t object_cap,
                                INTERNET_PORT *port) {
    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1L;
    parts.dwUrlPathLength = (DWORD)-1L;
    parts.dwExtraInfoLength = (DWORD)-1L;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        !parts.lpszHostName || parts.dwHostNameLength == 0 ||
        parts.dwHostNameLength >= host_cap) return false;
    wcsncpy_s(host, host_cap, parts.lpszHostName, parts.dwHostNameLength);

    size_t path_length = parts.dwUrlPathLength;
    size_t extra_length = parts.dwExtraInfoLength;
    if (path_length + extra_length + 1U > object_cap) return false;
    if (path_length) wcsncpy_s(object, object_cap, parts.lpszUrlPath, path_length);
    else wcscpy_s(object, object_cap, L"/");
    if (extra_length) {
        wcsncat_s(object, object_cap, parts.lpszExtraInfo, extra_length);
    }
    *port = parts.nPort;
    return true;
}

static HINTERNET open_http_request(const wchar_t *url, HINTERNET *session,
                                   HINTERNET *connection) {
    wchar_t host[256], object[1024];
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!get_http_components(url, host, _countof(host), object, _countof(object), &port))
        return NULL;

    *session = WinHttpOpen(L"VGPU-Mon Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!*session) return NULL;
    WinHttpSetTimeouts(*session, 1000, 1500, 1500, 4000);
    *connection = WinHttpConnect(*session, host, port, 0);
    if (!*connection) return NULL;
    HINTERNET request = WinHttpOpenRequest(*connection, L"GET", object, NULL,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) return NULL;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));
    WinHttpAddRequestHeaders(request, L"Cache-Control: no-cache\r\n", (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    return request;
}

static void close_http_handles(HINTERNET request, HINTERNET connection,
                               HINTERNET session) {
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

static bool begin_http_get(HINTERNET request) {
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) return false;
    DWORD status = 0, status_size = sizeof(status);
    return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                               &status, &status_size, WINHTTP_NO_HEADER_INDEX) && status == 200;
}

static bool http_get_memory(const wchar_t *url, char *buffer, size_t capacity,
                            size_t *received) {
    if (!url || !buffer || capacity < 2 || capacity > MAXDWORD || !received) return false;
    HINTERNET session = NULL, connection = NULL;
    HINTERNET request = open_http_request(url, &session, &connection);
    bool success = false;
    size_t total = 0;
    if (!request || !begin_http_get(request)) goto cleanup;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) goto cleanup;
        if (available == 0) break;
        if ((size_t)available > capacity - total - 1U) goto cleanup;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + total, available, &read) || read == 0)
            goto cleanup;
        total += read;
    }
    if (memchr(buffer, '\0', total) != NULL) goto cleanup;
    buffer[total] = '\0';
    *received = total;
    success = total > 0;

cleanup:
    close_http_handles(request, connection, session);
    return success;
}

static bool http_download_file(const wchar_t *url, const wchar_t *path) {
    HINTERNET session = NULL, connection = NULL;
    HINTERNET request = open_http_request(url, &session, &connection);
    HANDLE file = INVALID_HANDLE_VALUE;
    bool success = false;
    unsigned long long total = 0;
    if (!request || !begin_http_get(request)) goto cleanup;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) goto cleanup;
        if (available == 0) break;
        if (total + available > UPDATE_MAX_INSTALLER_BYTES) goto cleanup;
        unsigned char buffer[8U * 1024U];
        while (available) {
            DWORD request_bytes = available > (DWORD)sizeof(buffer) ?
                                  (DWORD)sizeof(buffer) : available;
            DWORD read = 0, written = 0;
            if (!WinHttpReadData(request, buffer, request_bytes, &read) || read == 0 ||
                !WriteFile(file, buffer, read, &written, NULL) || written != read) goto cleanup;
            available -= read;
            total += read;
        }
    }
    success = total > 0 && FlushFileBuffers(file);

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    close_http_handles(request, connection, session);
    if (!success) DeleteFileW(path);
    return success;
}

static bool sha256_file(const wchar_t *path, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char *object = NULL;
    DWORD object_size = 0, result_size = 0;
    bool success = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &result_size, 0) < 0 ||
        object_size == 0) goto cleanup;
    object = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, object_size);
    if (!object || BCryptCreateHash(algorithm, &hash, object, object_size,
                                    NULL, 0, 0) < 0) goto cleanup;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;
    for (;;) {
        unsigned char buffer[8U * 1024U];
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) goto cleanup;
        if (read == 0) break;
        if (BCryptHashData(hash, buffer, read, 0) < 0) goto cleanup;
    }
    success = BCryptFinishHash(hash, digest, 32, 0) >= 0;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (object) {
        SecureZeroMemory(object, object_size);
        HeapFree(GetProcessHeap(), 0, object);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

static bool make_temporary_path(const wchar_t *extension, wchar_t *path,
                                size_t capacity) {
    wchar_t temporary[MAX_PATH];
    DWORD length = GetTempPathW((DWORD)_countof(temporary), temporary);
    if (length == 0 || length >= _countof(temporary)) return false;
    for (unsigned int attempt = 0; attempt < 16; ++attempt) {
        int written = _snwprintf_s(
            path, capacity, _TRUNCATE, L"%lsVGPU-Mon-%lu-%llu-%u%ls", temporary,
            (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
            attempt, extension);
        if (written <= 0) return false;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    return false;
}

static bool get_current_executable(wchar_t *path, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    return length > 0 && length < capacity;
}

static bool get_installed_directory(wchar_t *path, size_t capacity) {
    DWORD bytes = (DWORD)(capacity * sizeof(wchar_t));
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\VGPU-Mon",
                                  L"PathEntry", RRF_RT_REG_SZ | RRF_NOEXPAND,
                                  NULL, path, &bytes);
    if (status != ERROR_SUCCESS || !path[0]) return false;
    size_t length = wcslen(path);
    while (length > 3 && (path[length - 1] == L'\\' || path[length - 1] == L'/'))
        path[--length] = L'\0';
    return true;
}

static bool executable_is_installed(const wchar_t *executable) {
    wchar_t directory[MAX_PATH];
    if (!get_installed_directory(directory, _countof(directory))) return false;
    wchar_t current[MAX_PATH];
    wcscpy_s(current, _countof(current), executable);
    wchar_t *separator = wcsrchr(current, L'\\');
    if (!separator) return false;
    *separator = L'\0';
    return _wcsicmp(current, directory) == 0;
}

static bool get_default_installed_executable(wchar_t *path, size_t capacity) {
    wchar_t local_app_data[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL,
                         SHGFP_TYPE_CURRENT, local_app_data) != S_OK) return false;
    return _snwprintf_s(path, capacity, _TRUNCATE,
                        L"%ls\\Programs\\VGPU-Mon\\vgpu.exe", local_app_data) > 0;
}

static bool script_append(ScriptBuffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (!buffer || !buffer->data || length > buffer->capacity - buffer->length - 1U)
        return false;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool script_append_quoted_wide(ScriptBuffer *buffer, const wchar_t *text) {
    if (!script_append(buffer, "'")) return false;
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                       NULL, 0, NULL, NULL);
    if (required <= 0) return false;
    char *utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)required);
    if (!utf8) return false;
    bool success = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                       utf8, required, NULL, NULL) > 0;
    if (success) {
        for (char *cursor = utf8; *cursor; ++cursor) {
            if (*cursor == '\'') success = script_append(buffer, "''");
            else {
                char value[2] = {*cursor, '\0'};
                success = script_append(buffer, value);
            }
            if (!success) break;
        }
    }
    HeapFree(GetProcessHeap(), 0, utf8);
    return success && script_append(buffer, "'");
}

static bool write_helper_script(const wchar_t *path, int argc, wchar_t **argv) {
    ScriptBuffer script;
    memset(&script, 0, sizeof(script));
    script.capacity = UPDATE_HELPER_CAPACITY;
    script.data = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, script.capacity);
    if (!script.data) return false;
    bool success = script_append(&script,
        "[CmdletBinding()]\r\n"
        "param([Parameter(Mandatory=$true)][string]$InstallerPath,"
        "[Parameter(Mandatory=$true)][string]$RelaunchPath)\r\n"
        "$ErrorActionPreference = 'Stop'\r\n"
        "$relaunchArguments = @('--no-update'");
    if (success && updater_is_forced(argc, argv)) {
        success = script_append(&script, ",'--version'");
    }
    for (int index = 1; success && index < argc; ++index) {
        if (wide_argument_equals(argv[index], L"--update") ||
            wide_argument_equals(argv[index], L"--no-update")) continue;
        success = script_append(&script, ",") &&
                  script_append_quoted_wide(&script, argv[index]);
    }
    success = success && script_append(&script,
        ")\r\n"
        "$updateError = $null\r\n"
        "try {\r\n"
        "  Start-Sleep -Milliseconds 750\r\n"
        "  $process = Start-Process -FilePath $InstallerPath -ArgumentList "
        "@('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-') "
        "-Wait -PassThru -WindowStyle Hidden\r\n"
        "  if ($process.ExitCode -ne 0) { throw \"Setup exited with code "
        "$($process.ExitCode).\" }\r\n"
        "}\r\n"
        "catch { $updateError = $_.Exception.Message }\r\n"
        "finally {\r\n"
        "  Remove-Item -LiteralPath $InstallerPath -Force -ErrorAction SilentlyContinue\r\n"
        "  Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue\r\n"
        "}\r\n"
        "if ($updateError) { Write-Warning \"VGPU-Mon update failed: $updateError\" }\r\n"
        "if (Test-Path -LiteralPath $RelaunchPath) {\r\n"
        "  & $RelaunchPath @relaunchArguments\r\n"
        "  exit $LASTEXITCODE\r\n"
        "}\r\n"
        "exit 1\r\n");

    HANDLE file = INVALID_HANDLE_VALUE;
    if (success) {
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file == INVALID_HANDLE_VALUE) success = false;
    }
    if (success) {
        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        success = WriteFile(file, bom, sizeof(bom), &written, NULL) &&
                  written == sizeof(bom) &&
                  WriteFile(file, script.data, (DWORD)script.length, &written, NULL) &&
                  written == (DWORD)script.length && FlushFileBuffers(file);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SecureZeroMemory(script.data, script.capacity);
    HeapFree(GetProcessHeap(), 0, script.data);
    if (!success) DeleteFileW(path);
    return success;
}

static bool launch_helper(const wchar_t *helper, const wchar_t *installer,
                          const wchar_t *relaunch) {
    wchar_t system_directory[MAX_PATH], powershell[MAX_PATH];
    const size_t command_capacity = 32768U;
    wchar_t *command = (wchar_t *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, command_capacity * sizeof(wchar_t));
    if (!command) return false;
    UINT length = GetSystemDirectoryW(system_directory, (UINT)_countof(system_directory));
    if (length == 0 || length >= _countof(system_directory) ||
        _snwprintf_s(powershell, _countof(powershell), _TRUNCATE,
                     L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe",
                     system_directory) <= 0 ||
        _snwprintf_s(command, command_capacity, _TRUNCATE,
                     L"\"%ls\" -NoLogo -NoProfile -ExecutionPolicy Bypass "
                     L"-File \"%ls\" -InstallerPath \"%ls\" -RelaunchPath \"%ls\"",
                     powershell, helper, installer, relaunch) <= 0) {
        HeapFree(GetProcessHeap(), 0, command);
        return false;
    }

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(powershell, command, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &startup, &process)) {
        HeapFree(GetProcessHeap(), 0, command);
        return false;
    }
    HeapFree(GetProcessHeap(), 0, command);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

VgpuUpdateResult updater_check_and_start(int argc, wchar_t **argv,
                                         const char *current_version,
                                         bool force) {
    wchar_t current_executable[MAX_PATH], relaunch_path[MAX_PATH];
    if (!get_current_executable(current_executable, _countof(current_executable)))
        return VGPU_UPDATE_ERROR;
    bool installed = executable_is_installed(current_executable);
    if (!installed && !force) return VGPU_UPDATE_SKIPPED;
    if (installed) wcscpy_s(relaunch_path, _countof(relaunch_path), current_executable);
    else if (!get_default_installed_executable(relaunch_path, _countof(relaunch_path)))
        return VGPU_UPDATE_ERROR;

    char manifest_text[UPDATE_MAX_MANIFEST_BYTES + 1U];
    size_t manifest_size = 0;
    VgpuUpdateManifest manifest;
    int comparison = 0;
    if (!http_get_memory(UPDATE_MANIFEST_URL, manifest_text,
                         sizeof(manifest_text), &manifest_size) || manifest_size == 0 ||
        !updater_parse_manifest(manifest_text, &manifest) ||
        !updater_compare_versions(current_version, manifest.version, &comparison))
        return VGPU_UPDATE_ERROR;
    if (comparison >= 0) return VGPU_UPDATE_CURRENT;

    wchar_t installer_url[1024], installer_path[MAX_PATH], helper_path[MAX_PATH];
    wchar_t version_wide[VGPU_UPDATE_VERSION_CAP];
    wchar_t installer_name_wide[VGPU_UPDATE_INSTALLER_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, manifest.version, -1,
                             version_wide, (int)_countof(version_wide)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, manifest.installer_name, -1,
                             installer_name_wide, (int)_countof(installer_name_wide)) ||
        _snwprintf_s(installer_url, _countof(installer_url), _TRUNCATE,
                     L"https://github.com/xptea/VGPU-Mon/releases/download/v%ls/%ls",
                     version_wide, installer_name_wide) <= 0)
        return VGPU_UPDATE_ERROR;
    if (!make_temporary_path(L".exe", installer_path, _countof(installer_path)))
        return VGPU_UPDATE_ERROR;
    if (!make_temporary_path(L".ps1", helper_path, _countof(helper_path))) {
        DeleteFileW(installer_path);
        return VGPU_UPDATE_ERROR;
    }

    bool installer_ready = false, helper_ready = false;
    unsigned char actual_hash[32];
    if (force) {
        printf("VGPU-Mon %s is available; downloading verified update...\n",
               manifest.version);
        fflush(stdout);
    }
    if (!http_download_file(installer_url, installer_path) ||
        !sha256_file(installer_path, actual_hash) ||
        memcmp(actual_hash, manifest.sha256, sizeof(actual_hash)) != 0) goto cleanup;
    installer_ready = true;
    if (!write_helper_script(helper_path, argc, argv)) goto cleanup;
    helper_ready = true;

    printf("Updating VGPU-Mon %s to %s; the monitor will reopen automatically...\n",
           current_version, manifest.version);
    fflush(stdout);
    if (!launch_helper(helper_path, installer_path, relaunch_path)) goto cleanup;
    SecureZeroMemory(actual_hash, sizeof(actual_hash));
    return VGPU_UPDATE_STARTED;

cleanup:
    SecureZeroMemory(actual_hash, sizeof(actual_hash));
    if (installer_ready || GetFileAttributesW(installer_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(installer_path);
    if (helper_ready || GetFileAttributesW(helper_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(helper_path);
    return VGPU_UPDATE_ERROR;
}
