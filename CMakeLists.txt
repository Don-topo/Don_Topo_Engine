cmake_minimum_required(VERSION 3.25)
project(DonTopoEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Dependencies ────────────────────────────────────────────────────────────

include(FetchContent)

# GLFW — windowing and input
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)
# The engine does not use OpenGL: without this glfw3.h includes GL/gl.h, which on Linux
# requires the OpenGL development package (on Windows it ships with the SDK, unnoticed).
# INTERFACE: it reaches everything that links glfw without touching GLFW's own build.
target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

# GLM — math (header-only)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# Vulkan SDK — install from https://vulkan.lunarg.com/sdk/home
find_package(Vulkan REQUIRED)

# Second render backend, chosen by the user. This only decides whether it is
# COMPILED; Vulkan is still the default path at runtime.
option(DTE_ENABLE_D3D12 "Compilar el backend de render DirectX 12 (solo Windows)" ON)

if(DTE_ENABLE_D3D12 AND NOT WIN32)
    message(STATUS "DTE_ENABLE_D3D12: disabled — the DirectX 12 backend only exists on Windows")
    set(DTE_ENABLE_D3D12 OFF)
endif()

if(DTE_ENABLE_D3D12)
    # d3d12 / dxgi / dxguid come with the Windows SDK that MSVC already uses: there is
    # nothing to download or look for, linking them is enough.

    # Shaders for DX12: the .vert/.frag/.comp files in shaders/ are still the
    # ONLY source. spirv-cross translates the SPIR-V glslc already produces into HLSL and
    # dxc lowers it to DXIL. Both binaries ship inside the Vulkan SDK, so
    # this path adds no external dependency.
    find_program(SPIRV_CROSS spirv-cross HINTS $ENV{VULKAN_SDK}/Bin)
    find_program(DXC dxc HINTS $ENV{VULKAN_SDK}/Bin)
    if(NOT SPIRV_CROSS OR NOT DXC)
        message(FATAL_ERROR
            "DTE_ENABLE_D3D12=ON but the shader tools are missing:\n"
            "  spirv-cross: ${SPIRV_CROSS}\n"
            "  dxc:         ${DXC}\n"
            "Both are installed with the Vulkan SDK (VULKAN_SDK/Bin). To build "
            "only the Vulkan backend, reconfigure with -DDTE_ENABLE_D3D12=OFF.")
    endif()

    # D3D12MemoryAllocator: the DX12 equivalent of VulkanMemoryAllocator
    # (suballocation on heaps). Without it every resource would be its own committed
    # resource, with the minimum 64 KB alignment per resource.
    set(D3D12MA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        D3D12MemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
        GIT_TAG        v3.0.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(D3D12MemoryAllocator)
endif()

# FMOD SDK — install from https://www.fmod.com/download, set FMOD_DIR
# Not REQUIRED here: FMOD may not be installed yet.
# Pass -DFMOD_DIR=<install_root> to cmake or set the FMOD_DIR environment variable.
find_package(FMOD)
include(cmake/FmodRuntime.cmake)
if(NOT FMOD_FOUND)
    message(WARNING
        "FMOD SDK not found — audio will not be available.\n"
        "  Install FMOD Studio API from https://www.fmod.com/download\n"
        "  Then re-run cmake with -DFMOD_DIR=<install_root>"
    )
endif()

# stb — single-header libs (stb_image for texture loading)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    # Pinned to a commit: with a branch GIT_TAG, any configure re-downloads the
    # dependency and leaves our own .obj files compiled against old headers.
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(stb)

# Assimp — 3D model import (FBX, OBJ, glTF/GLB)
set(ASSIMP_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL                   OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT                 ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER        ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER        ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER       ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(assimp)

# ImGui — docking branch (DockSpace + editor UI over Vulkan/GLFW)
# Pinned to the docking branch commit of 2026-08-04 (v1.93.0 WIP): following the
# branch made a configure bring in a new version and the editor abort in
# IMGUI_CHECKVERSION() against our own .obj files that had not been recompiled.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        11bd4e7731dbd8745a9eccacdaac837dd0d2a38b
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_backend STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_backend PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_backend PUBLIC Vulkan::Vulkan glfw)
target_compile_features(imgui_backend PUBLIC cxx_std_17)

# ImGui's DX12 backend. It already ships in ImGui's source tree, so it only
# needs compiling; it coexists with the Vulkan one without conflict because each
# defines its own ImGui_Impl<API>_* symbols.
if(DTE_ENABLE_D3D12)
    target_sources(imgui_backend PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    target_link_libraries(imgui_backend PUBLIC d3d12 dxgi dxguid)
endif()

# ImGuiFileDialog — embedded file browser (NoDialog mode)
# We use Populate (not MakeAvailable) to download the sources without running
# the library's CMakeLists.txt, which looks for imgui via find_package and fails.
FetchContent_Declare(
    ImGuiFileDialog
    GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog.git
    GIT_TAG        d0e97b2adc3d3452d72c750c7305dc0291acd052
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

add_library(imgui_filedialog STATIC
    ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
)
target_include_directories(imgui_filedialog PUBLIC
    ${imguifiledialog_SOURCE_DIR}
)
target_link_libraries(imgui_filedialog PUBLIC imgui_backend)
target_compile_features(imgui_filedialog PUBLIC cxx_std_17)

# ImGuizmo — transform gizmo / view manipulate cube
# Same as ImGuiFileDialog: Populate instead of MakeAvailable, its CMakeLists
# looks for imgui via find_package and fails.
FetchContent_Declare(
    ImGuizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        5ab7676402ace03cdf930b2d972f59c7d03c6fa8
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(ImGuizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(ImGuizmo)
endif()

add_library(imguizmo STATIC
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)
target_include_directories(imguizmo PUBLIC
    ${imguizmo_SOURCE_DIR}/src
)
target_link_libraries(imguizmo PUBLIC imgui_backend)
target_compile_features(imguizmo PUBLIC cxx_std_17)

# ImGuiColorTextEdit — embedded code editor (Script Editor Panel)
# Same as ImGuiFileDialog/ImGuizmo: Populate instead of MakeAvailable, the
# repo has no usable CMakeLists (only a standalone demo).
FetchContent_Declare(
    ImGuiColorTextEdit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG        ca2f9f1462e3b60e56351bc466acda448c5ea50d
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(ImGuiColorTextEdit)
if(NOT imguicolortextedit_POPULATED)
    FetchContent_Populate(ImGuiColorTextEdit)
endif()

add_library(imgui_texteditor STATIC
    ${imguicolortextedit_SOURCE_DIR}/TextEditor.cpp
)
target_include_directories(imgui_texteditor PUBLIC
    ${imguicolortextedit_SOURCE_DIR}
)
target_link_libraries(imgui_texteditor PUBLIC imgui_backend)
target_compile_features(imgui_texteditor PUBLIC cxx_std_17)

# imgui-node-editor — node canvas of the Animator panel
# Same as ImGuiFileDialog/ImGuizmo/ImGuiColorTextEdit: Populate instead of
# MakeAvailable, its CMakeLists looks for imgui via find_package and fails.
FetchContent_Declare(
    ImGuiNodeEditor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        021aa0ea4da13fed864bafb2a92d4c5205076866
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(ImGuiNodeEditor)
if(NOT imguinodeeditor_POPULATED)
    FetchContent_Populate(ImGuiNodeEditor)
endif()

# This library's imgui_extra_math.inl redefines
# "operator*(const float, const ImVec2&)" without the version guard its
# neighboring operators do have (an upstream bug, present both in master and
# in the v0.9.3 tag). Our ImGui 1.92.9 WIP already implements that operator
# under IMGUI_DEFINE_MATH_OPERATORS, so without the patch there is a double
# definition and imgui_node_editor does not compile. It is applied every time (not only
# the first time) because the patch edits the already-populated sources and must be
# safe to repeat on every re-configure; see cmake/PatchImGuiNodeEditor.cmake.
include(cmake/PatchImGuiNodeEditor.cmake)

add_library(imgui_node_editor STATIC
    ${imguinodeeditor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imguinodeeditor_SOURCE_DIR}/imgui_node_editor_api.cpp
    ${imguinodeeditor_SOURCE_DIR}/imgui_canvas.cpp
    ${imguinodeeditor_SOURCE_DIR}/crude_json.cpp
)
target_include_directories(imgui_node_editor PUBLIC
    ${imguinodeeditor_SOURCE_DIR}
)
target_link_libraries(imgui_node_editor PUBLIC imgui_backend)
target_compile_features(imgui_node_editor PUBLIC cxx_std_17)

# nlohmann/json — JSON serialization (Scene save/load)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Lua 5.4 — scripting VM (built from source, see cmake/Lua.cmake)
include(cmake/Lua.cmake)

# sol2 — C++/Lua binding (header-only). Populate instead of MakeAvailable,
# same pattern as ImGuiFileDialog: we avoid its CMakeLists (tests/options
# we do not need) and define an INTERFACE target of our own.
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
    FetchContent_Populate(sol2)
endif()

add_library(sol2_lib INTERFACE)
target_include_directories(sol2_lib INTERFACE ${sol2_SOURCE_DIR}/include)
target_link_libraries(sol2_lib INTERFACE lua_lib)
# sol2's type/stack checks always on: binding errors
# show up as readable Lua errors instead of UB.
target_compile_definitions(sol2_lib INTERFACE SOL_ALL_SAFETIES_ON=1)

# PhysX SDK — physics (built from source, see cmake/PhysX.cmake)
include(cmake/PhysX.cmake)

# FreeType and msdfgen are built OPTIMIZED even when the project is in Debug.
# Measured with the same bake (the default font, 32 px): 15.2 s with them
# unoptimized versus 0.59 s with /O2. The whole editor eats that stall when it
# draws the first text, and it is not an engine problem: nobody ever debugs inside
# FreeType or msdfgen. The engine itself is still built in Debug.
#
# /RTC1 and /Od have to GO, adding /O2 is not enough: /RTC1 with /O2 is a
# compiler error, and between /Od and /O2 the last one wins, but better not to
# rely on that. /Zi stays: you can still step through it.
set(DT_FLAGS_C_DEBUG_ORIG   "${CMAKE_C_FLAGS_DEBUG}")
set(DT_FLAGS_CXX_DEBUG_ORIG "${CMAKE_CXX_FLAGS_DEBUG}")
if(MSVC)
    foreach(dt_flagvar CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REPLACE "/RTC1" "" ${dt_flagvar} "${${dt_flagvar}}")
        string(REPLACE "/Ob0"  "" ${dt_flagvar} "${${dt_flagvar}}")
        string(REPLACE "/Od"   "" ${dt_flagvar} "${${dt_flagvar}}")
        set(${dt_flagvar} "${${dt_flagvar}} /O2 /Ob2")
    endforeach()
endif()

# FreeType: only UiFont uses it, to extract the outlines and metrics of a
# TTF when baking the text atlas. No compressors and no HarfBuzz: none of that
# is needed to read outlines and it would drag in dependencies of its own.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(freetype)

# msdfgen — multi-channel distance fields (sharp text at any size).
# CORE_ONLY on purpose: its "ext" half does find_package(Freetype REQUIRED),
# which searches the SYSTEM, and here FreeType comes through FetchContent. The
# outline -> Shape step is done by UiFont with FT_Outline_Decompose, which is exactly
# what that half provided.
set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP       OFF CACHE BOOL "" FORCE)
# By default it is built with the STATIC CRT (/MTd) while the rest of the project uses
# the dynamic one: without this the link dies with thirteen RuntimeLibrary LNK2038 errors.
set(MSDFGEN_DYNAMIC_RUNTIME  ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG        v1.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(msdfgen)

# And the Debug flags are restored exactly as they were: EVERYTHING that comes
# after (the engine, the editor, the runtime, the tests) is built the same as
# always, with /Od and /RTC1.
set(CMAKE_C_FLAGS_DEBUG   "${DT_FLAGS_C_DEBUG_ORIG}")
set(CMAKE_CXX_FLAGS_DEBUG "${DT_FLAGS_CXX_DEBUG_ORIG}")

# Future dependencies go here:
# FetchContent_Declare(gainput ...) # dedicated input

# ─── Targets ─────────────────────────────────────────────────────────────────

# Linux: the binaries look for libfmod.so in their own folder (the exported
# game depends on the same). It only affects the targets below.
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH "\$ORIGIN")
endif()

# Name of the runtime binary, defined ONCE: it is used by the POST_BUILD that copies it
# next to the editor and by the editor when exporting (DT_RUNTIME_FILE_NAME).
set(DT_RUNTIME_FILE_NAME "DonTopoRuntime${CMAKE_EXECUTABLE_SUFFIX}")

add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(runtime)
