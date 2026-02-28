cmake_minimum_required(VERSION 3.20)

project(Toolscreen LANGUAGES C CXX RC)

option(TOOLSCREEN_FORCE_MCSR_SAFE "Force MCSR-safe mode (non-approved overlay features unreachable)" OFF)

if (NOT WIN32)
    message(FATAL_ERROR "Toolscreen build is Windows-only.")
endif()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Toolscreen build currently supports x64 only.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # Avoid depending on javaw's bundled (older) msvcp140.dll in GraalVM.
    # Static runtime makes the injected DLL self-contained.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(Toolscreen SHARED
    src/config_toml.cpp
    src/dllmain.cpp
    src/expression_parser.cpp
    src/fake_cursor.cpp
    src/gui.cpp
    src/imgui_cache.cpp
    src/input_hook.cpp
    src/logic_thread.cpp
    src/mirror_thread.cpp
    src/notes_overlay.cpp
    src/obs_thread.cpp
    src/pch.cpp
    src/practice_world_launch.cpp
    src/profiler.cpp
    src/render.cpp
    src/render_thread.cpp
    src/shared_contexts.cpp
    src/stronghold_companion_overlay.cpp
    src/utils.cpp
    src/version.cpp
    src/virtual_camera.cpp
    src/window_overlay.cpp
    src/toolscreen.rc
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
    third_party/minhook/src/hook.c
    third_party/minhook/src/buffer.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)

# Optional JNI support for direct in-process world launch.
set(TOOLSCREEN_JNI_AVAILABLE OFF)
find_package(JNI QUIET)
if (JNI_FOUND)
    target_include_directories(Toolscreen PRIVATE ${JNI_INCLUDE_DIRS})
    set(TOOLSCREEN_JNI_AVAILABLE ON)
else()
    set(_toolscreen_java_hints
        "$ENV{JAVA_HOME}"
        "${CMAKE_SOURCE_DIR}/../graalvm-jdk-21.0.9+7.1"
        "${CMAKE_SOURCE_DIR}/../jdk"
    )
    find_path(TOOLSCREEN_JNI_INCLUDE_DIR jni.h HINTS ${_toolscreen_java_hints} PATH_SUFFIXES include)
    find_path(TOOLSCREEN_JNI_MD_INCLUDE_DIR jni_md.h HINTS ${_toolscreen_java_hints} PATH_SUFFIXES include/win32)
    if (TOOLSCREEN_JNI_INCLUDE_DIR AND TOOLSCREEN_JNI_MD_INCLUDE_DIR)
        target_include_directories(Toolscreen PRIVATE ${TOOLSCREEN_JNI_INCLUDE_DIR} ${TOOLSCREEN_JNI_MD_INCLUDE_DIR})
        set(TOOLSCREEN_JNI_AVAILABLE ON)
    endif()
endif()

target_include_directories(Toolscreen PRIVATE
    src
    third_party/minhook/include
    third_party/imgui
    third_party/imgui/backends
    third_party/imgui/misc/cpp
    third_party/tomlplusplus
    third_party/json
    third_party/json/nlohmann
    third_party/glew/include
)

target_compile_definitions(Toolscreen PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    TOOLSCREEN_DISABLE_ABORT_OVERRIDE
    GLEW_STATIC
)
if (TOOLSCREEN_JNI_AVAILABLE)
    target_compile_definitions(Toolscreen PRIVATE TOOLSCREEN_HAS_JNI=1)
else()
    target_compile_definitions(Toolscreen PRIVATE TOOLSCREEN_HAS_JNI=0)
endif()

if (TOOLSCREEN_FORCE_MCSR_SAFE)
    target_compile_definitions(Toolscreen PRIVATE TOOLSCREEN_FORCE_MCSR_SAFE=1)
endif()

if (MSVC)
    target_compile_options(Toolscreen PRIVATE /utf-8 /W3 /MP /bigobj)
endif()

target_link_libraries(Toolscreen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew/lib/x64/libglew32.lib"
    opengl32
    comdlg32
    msimg32
    dbghelp
    shlwapi
    psapi
    dwmapi
    user32
    gdi32
    gdiplus
    shell32
    ole32
    winmm
    version
)

set_target_properties(Toolscreen PROPERTIES
    OUTPUT_NAME "Toolscreen"
)
