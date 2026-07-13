#define _CRT_SECURE_NO_WARNINGS
#include "../src/vgpu.h"
#include "../src/pdh_gpu.h"
#include "../src/updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

static int failures = 0;

#define EXPECT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_format_bytes(void) {
    char buffer[32];
    EXPECT(strcmp(format_bytes(0, buffer, sizeof(buffer)), "0 B") == 0);
    EXPECT(strcmp(format_bytes(1024, buffer, sizeof(buffer)), "1.00 KiB") == 0);
    EXPECT(strcmp(format_bytes(1024ULL * 1024ULL * 1024ULL, buffer, sizeof(buffer)), "1.00 GiB") == 0);
    EXPECT(dedicated_gpu_memory_plausible(16ULL * 1024ULL * 1024ULL * 1024ULL,
                                         16ULL * 1024ULL * 1024ULL * 1024ULL));
    EXPECT(!dedicated_gpu_memory_plausible(75ULL * 1024ULL * 1024ULL * 1024ULL,
                                          16ULL * 1024ULL * 1024ULL * 1024ULL));
    EXPECT(dedicated_gpu_memory_plausible(UINT64_MAX, 0));

    GpuProcess valid[2] = {{0}};
    valid[0].dedicated_bytes = 9ULL * 1024ULL * 1024ULL * 1024ULL;
    valid[1].dedicated_bytes = 512ULL * 1024ULL * 1024ULL;
    EXPECT(!quarantine_invalid_dedicated_gpu_memory(
        valid, _countof(valid), 16ULL * 1024ULL * 1024ULL * 1024ULL));
    EXPECT(!valid[0].dedicated_memory_invalid && valid[0].dedicated_bytes != 0);

    GpuProcess corrupt[3] = {{0}};
    corrupt[0].dedicated_bytes = 9ULL * 1024ULL * 1024ULL * 1024ULL;
    corrupt[1].dedicated_bytes = 75ULL * 1024ULL * 1024ULL * 1024ULL;
    corrupt[2].dedicated_bytes = 512ULL * 1024ULL * 1024ULL;
    EXPECT(quarantine_invalid_dedicated_gpu_memory(
        corrupt, _countof(corrupt), 16ULL * 1024ULL * 1024ULL * 1024ULL));
    for (size_t i = 0; i < _countof(corrupt); ++i) {
        EXPECT(corrupt[i].dedicated_memory_invalid);
        EXPECT(corrupt[i].dedicated_bytes == 0);
    }
}

static void test_parse_gpu_instance(void) {
    const wchar_t *instance = L"pid_18240_luid_0x00000000_0x0000A7F1_phys_0_eng_3_engtype_Compute_0";
    DWORD pid = 0;
    unsigned int gpu = 99;
    wchar_t engine[64];
    EXPECT(parse_gpu_instance(instance, &pid, &gpu, engine, _countof(engine)));
    EXPECT(pid == 18240);
    EXPECT(gpu == 0);
    EXPECT(wcscmp(engine, L"Compute_0") == 0);
    EXPECT(!parse_gpu_instance(L"not_a_gpu_instance", &pid, &gpu, engine, _countof(engine)));
}

static void test_text_helpers(void) {
    EXPECT(contains_case_insensitive("NVIDIA Overlay.exe", "overlay"));
    EXPECT(!contains_case_insensitive("chrome.exe", "python"));
    EXPECT(contains_case_insensitive("anything", ""));

    char csv[64];
    sanitize_csv_field("hello \"gpu\"\nnext", csv, sizeof(csv));
    EXPECT(strcmp(csv, "hello \"\"gpu\"\" next") == 0);
    sanitize_csv_field("=cmd|' /C calc'!A0", csv, sizeof(csv));
    EXPECT(csv[0] == '\'' && csv[1] == '=');
}

static void test_safe_process_helpers(void) {
    ProcessDetails details;
    EXPECT(query_process_details(GetCurrentProcessId(), &details));
    EXPECT(details.accessible);
    EXPECT(details.path[0] != '\0');
    EXPECT(details.threads > 0);

    char message[128];
    EXPECT(!terminate_process_safely(GetCurrentProcessId(), message, sizeof(message)));
    EXPECT(strstr(message, "Refused") != NULL);
}

static void test_updater_manifest(void) {
    unsigned int parts[3];
    EXPECT(updater_parse_version("1.2.0", parts));
    EXPECT(parts[0] == 1 && parts[1] == 2 && parts[2] == 0);
    EXPECT(!updater_parse_version("1.2", parts));
    EXPECT(!updater_parse_version("01.2.0", parts));
    EXPECT(!updater_parse_version("1.2.0-beta", parts));

    int comparison = 99;
    EXPECT(updater_compare_versions("1.2.0", "1.2.1", &comparison) && comparison < 0);
    EXPECT(updater_compare_versions("2.0.0", "1.9.9", &comparison) && comparison > 0);
    EXPECT(updater_compare_versions("1.2.0", "1.2.0", &comparison) && comparison == 0);

    const char *valid =
        "version=1.2.0\r\n"
        "installer=VGPU-Mon-1.2.0-setup.exe\r\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n";
    VgpuUpdateManifest manifest;
    EXPECT(updater_parse_manifest(valid, &manifest));
    EXPECT(strcmp(manifest.version, "1.2.0") == 0);
    EXPECT(strcmp(manifest.installer_name, "VGPU-Mon-1.2.0-setup.exe") == 0);
    EXPECT(manifest.sha256[0] == 0x01 && manifest.sha256[31] == 0xef);

    EXPECT(!updater_parse_manifest(
        "version=1.2.0\ninstaller=..\\evil.exe\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        &manifest));
    EXPECT(!updater_parse_manifest(
        "version=1.2.0\ninstaller=VGPU-Mon-1.2.0-setup.exe\nsha256=bad\n",
        &manifest));
    EXPECT(!updater_parse_manifest(
        "version=1.2.0\nversion=1.2.0\ninstaller=VGPU-Mon-1.2.0-setup.exe\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        &manifest));
}

static void test_updater_argument_policy(void) {
    wchar_t *interactive[] = {L"vgpu.exe", L"--chart", L"vram"};
    wchar_t *json[] = {L"vgpu.exe", L"--json"};
    wchar_t *disabled[] = {L"vgpu.exe", L"--no-update"};
    wchar_t *forced[] = {L"vgpu.exe", L"--update"};
    EXPECT(updater_should_check(3, interactive));
    EXPECT(!updater_should_check(2, json));
    EXPECT(!updater_should_check(2, disabled));
    EXPECT(updater_is_forced(2, forced));
}

int main(void) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF);
#endif
    test_format_bytes();
    test_parse_gpu_instance();
    test_text_helpers();
    test_safe_process_helpers();
    test_updater_manifest();
    test_updater_argument_policy();
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("All VGPU-Mon core tests passed.");
#ifdef _DEBUG
    if (_CrtDumpMemoryLeaks()) return 2;
#endif
    return 0;
}
