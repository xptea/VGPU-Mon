cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" VGPU_VERSION_LINE
     REGEX "^#define VGPU_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
string(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1"
       VGPU_VERSION "${VGPU_VERSION_LINE}")

project(VGPUMon VERSION "${VGPU_VERSION}" LANGUAGES C RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "VGPU-Mon currently supports 64-bit Windows with MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VGPU-Mon must be built for x64.")
endif()

function(vgpu_configure_target target)
    set_property(TARGET ${target} PROPERTY C_STANDARD 11)
    set_property(TARGET ${target} PROPERTY C_STANDARD_REQUIRED ON)
    set_property(TARGET ${target} PROPERTY C_EXTENSIONS OFF)
    set_property(TARGET ${target} PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:/utf-8 /W4 /WX /sdl /guard:cf>)
    target_link_options(${target} PRIVATE
        /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /CETCOMPAT /MANIFEST:EMBED)
endfunction()

add_executable(vgpu-mon
    src/main.c
    src/util.c
    src/nvml_dyn.c
    src/dxgi_gpu.c
    src/d3dkmt_gpu.c
    src/pdh_gpu.c
    src/updater.c
    src/vgpu.rc)
vgpu_configure_target(vgpu-mon)
target_link_libraries(vgpu-mon PRIVATE
    pdh dxgi dxguid psapi advapi32 shell32 ole32 winhttp bcrypt)

include(CTest)
if(BUILD_TESTING)
    add_executable(vgpu-tests
        tests/test_core.c
        src/util.c
        src/pdh_gpu.c
        src/updater.c)
    vgpu_configure_target(vgpu-tests)
    target_link_libraries(vgpu-tests PRIVATE pdh psapi advapi32 shell32 winhttp bcrypt)

    add_executable(vgpu-conpty-tests tests/test_conpty.c)
    vgpu_configure_target(vgpu-conpty-tests)

    add_executable(vgpu-update-smoke
        tests/update_smoke.c
        src/updater.c)
    vgpu_configure_target(vgpu-update-smoke)
    target_link_libraries(vgpu-update-smoke PRIVATE
        advapi32 shell32 winhttp bcrypt)

    add_test(NAME core COMMAND vgpu-tests)
    add_test(NAME conpty COMMAND vgpu-conpty-tests $<TARGET_FILE:vgpu-mon>)
endif()
