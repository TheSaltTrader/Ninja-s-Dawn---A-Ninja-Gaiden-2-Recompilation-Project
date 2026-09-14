# ng2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ng2 LANGUAGES CXX)

# RC is needed for the icon and version block in resources/ng2.rc. Enabled only
# on Windows: it is the only platform with an .rc compiler, and the executable
# is Windows-only anyway.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# The port's own version, read from VERSION rather than typed here, so the
# number on screen is the number the release script packages. Listed as a
# configure dependency so bumping VERSION re-runs CMake - without that, the
# build happily bakes in the previous number.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NG2_VERSION LIMIT_COUNT 1)
string(STRIP "${NG2_VERSION}" NG2_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
message(STATUS "ng2 version ${NG2_VERSION}")

# Sources
set(NG2_SOURCES
    src/main.cpp
    src/diag_hooks.cpp
    src/patch_hooks.cpp
    src/ng2_audio_fix.cpp
    src/ng2_chapter_fix.cpp
    src/ng2_disc.cpp
    src/ng2_menu.cpp
    src/ng2_platform.cpp
    src/ng2_saveimport.cpp
    src/ng2_texnotify.cpp
    src/ng2_textool.cpp
    src/ng2_update.cpp
    src/ng2_diagnostics.cpp
    src/ng2_hwdetect.cpp
    src/ng2_perf.cpp
    src/ng2_autoskip.cpp
    src/third_party/stb_image.cpp
)

# The icon. Explorer, the desktop shortcut and the taskbar all read it from
# here - see the comment at the top of resources/ng2.rc.in.
#
# The .rc is generated so its version block comes from the VERSION file. A
# hardcoded copy drifts the moment VERSION is bumped, and drifts silently:
# nothing reads it except the Properties dialog.
if(WIN32)
    # VERSIONINFO wants four comma-separated numbers.
    string(REPLACE "." ";" NG2_VERSION_PARTS "${NG2_VERSION}")
    list(LENGTH NG2_VERSION_PARTS NG2_VERSION_PART_COUNT)
    while(NG2_VERSION_PART_COUNT LESS 4)
        list(APPEND NG2_VERSION_PARTS "0")
        list(LENGTH NG2_VERSION_PARTS NG2_VERSION_PART_COUNT)
    endwhile()
    list(JOIN NG2_VERSION_PARTS ", " NG2_VERSION_RC)

    # rc resolves a relative path against the .rc's own directory, and the
    # generated one lives in the build tree - so hand it an absolute path.
    set(NG2_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/ng2.ico")
    if(NOT EXISTS "${NG2_ICON_PATH}")
        message(FATAL_ERROR
            "resources/ng2.ico is missing. Regenerate it: python tools/make_icon.py")
    endif()
    # Backslashes are escape characters to the resource compiler.
    string(REPLACE "\\" "/" NG2_ICON_PATH "${NG2_ICON_PATH}")

    configure_file(resources/ng2.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ng2.rc" @ONLY)
    list(APPEND NG2_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/ng2.rc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/resources/ng2.rc.in")
endif()

if(WIN32)
    add_executable(ng2 WIN32 ${NG2_SOURCES})
else()
    add_executable(ng2 ${NG2_SOURCES})
endif()

# GPU_PLUGINS stages the SDK's Xenos emulation plugin (rexgpu-xenos.dll)
# next to the executable. It is runtime-loaded, never linked.
rexglue_setup_target(ng2 GPU_PLUGINS xenos)

# Post-codegen guard for the local recompilation crash patches. These two fixes
# (local/diag/patch_missed_regs.py + patch_scanguard.py) edit the *generated*
# translated source, so a codegen regeneration overwrites them. That happened
# once and silently shipped two crashes back into v1.0.17 (a null write on
# Ninpo/a late boss, and the chapter 12->13 transition). Wiring the re-apply
# into the build graph after codegen and before the recomp compile makes that
# regression impossible: whenever codegen refreshes codegen.build.stamp, the
# patches are re-applied (idempotent - each script skips a file already carrying
# its marker) before the generated .cpp are compiled.
if(TARGET ng2_codegen AND TARGET ng2_recomp)
    find_program(NG2_PYTHON NAMES python python3)
    if(NG2_PYTHON)
        set(_ng2_gen "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
        add_custom_command(
            OUTPUT "${_ng2_gen}/local_patches.stamp"
            DEPENDS "${_ng2_gen}/codegen.build.stamp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_missed_regs.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_scanguard.py"
            COMMAND "${NG2_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_missed_regs.py"
            COMMAND "${NG2_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_scanguard.py"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_ng2_gen}/local_patches.stamp"
            COMMENT "Re-applying NG2 post-codegen crash patches (missed_regs, scanguard)"
            VERBATIM)
        add_custom_target(ng2_local_patches
            DEPENDS "${_ng2_gen}/local_patches.stamp")
        add_dependencies(ng2_recomp ng2_local_patches)
    else()
        message(WARNING
            "python not found: NG2 post-codegen crash patches will NOT be auto-"
            "re-applied. After any codegen regeneration, run by hand: "
            "local/diag/patch_missed_regs.py and local/diag/patch_scanguard.py")
    endif()
endif()

target_compile_definitions(ng2 PRIVATE NG2_VERSION="${NG2_VERSION}")

# DXGI, for reading the video-memory budget in src/ng2_hwdetect.cpp.
# WinHTTP, for the on-launch update check and download in src/ng2_update.cpp.
if(WIN32)
    target_link_libraries(ng2 PRIVATE dxgi winhttp)
endif()
