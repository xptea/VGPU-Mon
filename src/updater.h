#ifndef VGPU_UPDATER_H
#define VGPU_UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

#define VGPU_UPDATE_VERSION_CAP 32
#define VGPU_UPDATE_INSTALLER_CAP 128

typedef struct {
    char version[VGPU_UPDATE_VERSION_CAP];
    char installer_name[VGPU_UPDATE_INSTALLER_CAP];
    unsigned char sha256[32];
} VgpuUpdateManifest;

typedef enum {
    VGPU_UPDATE_SKIPPED,
    VGPU_UPDATE_CURRENT,
    VGPU_UPDATE_STARTED,
    VGPU_UPDATE_ERROR
} VgpuUpdateResult;

bool updater_parse_version(const char *text, unsigned int parts[3]);
bool updater_compare_versions(const char *left, const char *right, int *result);
bool updater_parse_manifest(const char *text, VgpuUpdateManifest *manifest);
bool updater_should_check(int argc, wchar_t **argv);
bool updater_is_forced(int argc, wchar_t **argv);
VgpuUpdateResult updater_check_and_start(int argc, wchar_t **argv,
                                         const char *current_version,
                                         bool force);

#endif
