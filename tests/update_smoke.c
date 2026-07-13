#include "../src/updater.h"

#include <stdio.h>

int wmain(int argc, wchar_t **argv) {
    (void)argc;
    wchar_t *update_arguments[] = {argv[0], L"--update"};
    VgpuUpdateResult result = updater_check_and_start(
        2, update_arguments, "0.0.0", true);
    if (result == VGPU_UPDATE_STARTED) return 0;
    fprintf(stderr, "Updater smoke handoff did not start (result %d).\n", (int)result);
    return result == VGPU_UPDATE_CURRENT ? 2 : 3;
}
