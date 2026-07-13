#define _CRT_SECURE_NO_WARNINGS
#include "../src/vgpu.h"
#include "../src/pdh_gpu.h"

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
