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

# GLM — math (header-only)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# Vulkan SDK — install from https://vulkan.lunarg.com/sdk/home
find_package(Vulkan REQUIRED)

# ─── DirectX 12 ──────────────────────────────────────────────────────────────
# Segundo backend de render, elegible por el usuario. Esto solo decide si se
# COMPILA; Vulkan sigue siendo el camino por defecto en tiempo de ejecución.
option(DTE_ENABLE_D3D12 "Compilar el backend de render DirectX 12 (solo Windows)" ON)

if(DTE_ENABLE_D3D12 AND NOT WIN32)
    message(STATUS "DTE_ENABLE_D3D12: desactivado — el backend DirectX 12 solo existe en Windows")
    set(DTE_ENABLE_D3D12 OFF)
endif()

if(DTE_ENABLE_D3D12)
    # d3d12 / dxgi / dxguid vienen con el Windows SDK que ya usa MSVC: no hay
    # nada que descargar ni que buscar, basta con enlazarlas.

    # Shaders para DX12: los .vert/.frag/.comp de shaders/ siguen siendo la
    # ÚNICA fuente. spirv-cross traduce a HLSL el SPIR-V que ya genera glslc y
    # dxc lo baja a DXIL. Los dos binarios vienen dentro del Vulkan SDK, así que
    # esta ruta no añade ninguna dependencia externa.
    find_program(SPIRV_CROSS spirv-cross HINTS $ENV{VULKAN_SDK}/Bin)
    find_program(DXC dxc HINTS $ENV{VULKAN_SDK}/Bin)
    if(NOT SPIRV_CROSS OR NOT DXC)
        message(FATAL_ERROR
            "DTE_ENABLE_D3D12=ON pero faltan las herramientas de shaders:\n"
            "  spirv-cross: ${SPIRV_CROSS}\n"
            "  dxc:         ${DXC}\n"
            "Ambas se instalan con el Vulkan SDK (VULKAN_SDK/Bin). Para compilar "
            "solo el backend Vulkan, reconfigura con -DDTE_ENABLE_D3D12=OFF.")
    endif()

    # D3D12MemoryAllocator — el equivalente de VulkanMemoryAllocator para DX12
    # (suballocation sobre heaps). Sin él cada recurso sería un committed
    # resource propio, con la alineación mínima de 64 KB por recurso.
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
if(NOT FMOD_FOUND)
    message(WARNING
        "FMOD SDK not found — audio will not be available.\n"
        "  Install FMOD Studio API from https://www.fmod.com/download\n"
        "  Then re-run cmake with -DFMOD_DIR=<install_root>"
    )
endif()

# stb — single-header libs (stb_image para carga de texturas)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    # Pineado a commit: con GIT_TAG de rama, cualquier configure re-descarga la
    # dependencia y deja los .obj propios compilados contra headers viejos.
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(stb)

# Assimp — importación de modelos 3D (FBX, OBJ, glTF...)
set(ASSIMP_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL                   OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT                 ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER        ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER        ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(assimp)

# ImGui — docking branch (DockSpace + editor UI over Vulkan/GLFW)
# Pineado al commit de la rama docking en 2026-08-04 (v1.93.0 WIP): seguir la
# rama hacía que un configure trajera una versión nueva y el editor abortara en
# IMGUI_CHECKVERSION() contra los .obj propios sin recompilar.
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

# Backend DX12 de ImGui. Ya viene en el árbol de fuentes de ImGui, así que solo
# hay que compilarlo; convive con el de Vulkan sin conflicto porque cada uno
# define sus propios símbolos ImGui_Impl<API>_*.
if(DTE_ENABLE_D3D12)
    target_sources(imgui_backend PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    target_link_libraries(imgui_backend PUBLIC d3d12 dxgi dxguid)
endif()

# ImGuiFileDialog — embedded file browser (NoDialog mode)
# Usamos Populate (no MakeAvailable) para descargar fuentes sin ejecutar
# el CMakeLists.txt de la librería, que busca imgui via find_package y falla.
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

# ImGuizmo — gizmo de transformación / view manipulate cube
# Igual que ImGuiFileDialog: Populate en vez de MakeAvailable, su CMakeLists
# busca imgui via find_package y falla.
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

# ImGuiColorTextEdit — editor de código embebido (Script Editor Panel)
# Igual que ImGuiFileDialog/ImGuizmo: Populate en vez de MakeAvailable, el
# repo no trae CMakeLists usable (solo demo standalone).
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

# imgui-node-editor — canvas de nodos del panel Animator
# Igual que ImGuiFileDialog/ImGuizmo/ImGuiColorTextEdit: Populate en vez de
# MakeAvailable, su CMakeLists busca imgui por find_package y falla.
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

# imgui_extra_math.inl de esta librería redefine
# "operator*(const float, const ImVec2&)" sin el guard de versión que sí
# tienen sus operadores vecinos (bug upstream, presente tanto en master como
# en el tag v0.9.3). Nuestro ImGui 1.92.9 WIP ya implementa ese operador
# bajo IMGUI_DEFINE_MATH_OPERATORS, así que sin parchear hay doble
# definición y imgui_node_editor no compila. Se aplica siempre (no solo la
# primera vez) porque el parche edita las fuentes ya populadas y debe ser
# seguro repetirlo en cada re-configure; ver cmake/PatchImGuiNodeEditor.cmake.
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

# nlohmann/json — serialización JSON (Scene save/load)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Lua 5.4 — VM de scripting (compilado desde fuente, ver cmake/Lua.cmake)
include(cmake/Lua.cmake)

# sol2 — binding C++/Lua (header-only). Populate en vez de MakeAvailable,
# mismo patrón que ImGuiFileDialog: evitamos su CMakeLists (tests/opciones
# que no necesitamos) y definimos una INTERFACE propia.
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
# Chequeos de tipo/stack de sol2 activos siempre: los errores de binding
# aparecen como errores Lua legibles en vez de UB.
target_compile_definitions(sol2_lib INTERFACE SOL_ALL_SAFETIES_ON=1)

# PhysX SDK — física (compilado desde fuente, ver cmake/PhysX.cmake)
include(cmake/PhysX.cmake)

# FreeType y msdfgen se compilan OPTIMIZADOS aunque el proyecto vaya en Debug.
# Medido con el mismo horneado (la fuente por defecto, 32 px): 15,2 s con ellos
# sin optimizar contra 0,59 s con /O2. Ese parón se lo come el editor entero al
# dibujar el primer texto, y no es un problema del motor: dentro de FreeType y
# de msdfgen no se depura nunca. El motor sigue compilándose en Debug.
#
# /RTC1 y /Od tienen que SALIR, no basta con añadir /O2: /RTC1 con /O2 es un
# error del compilador, y de /Od contra /O2 gana la última pero mejor no
# depender de eso. /Zi se queda: se sigue pudiendo pisar el paso a paso.
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

# FreeType — solo lo usa UiFont para sacar los contornos y las métricas de un
# TTF al hornear el atlas de texto. Sin compresores ni HarfBuzz: nada de eso
# hace falta para leer outlines y arrastraría dependencias propias.
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

# msdfgen — campos de distancia multicanal (texto nítido a cualquier tamaño).
# CORE_ONLY a propósito: su mitad "ext" hace find_package(Freetype REQUIRED),
# que busca en el SISTEMA y aquí FreeType viene por FetchContent. El paso
# contorno -> Shape lo hace UiFont con FT_Outline_Decompose, que es justo lo
# que esa mitad aportaba.
set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP       OFF CACHE BOOL "" FORCE)
# Por defecto se compila con el CRT ESTÁTICO (/MTd) y el resto del proyecto va
# con el dinámico: sin esto el link muere con trece LNK2038 de RuntimeLibrary.
set(MSDFGEN_DYNAMIC_RUNTIME  ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG        v1.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(msdfgen)

# Y se devuelven las flags de Debug tal y como estaban: TODO lo que venga
# después (el motor, el editor, el runtime, los tests) se compila igual que
# siempre, con /Od y /RTC1.
set(CMAKE_C_FLAGS_DEBUG   "${DT_FLAGS_C_DEBUG_ORIG}")
set(CMAKE_CXX_FLAGS_DEBUG "${DT_FLAGS_CXX_DEBUG_ORIG}")

# Future dependencies go here:
# FetchContent_Declare(gainput ...) # dedicated input

# ─── Targets ─────────────────────────────────────────────────────────────────

add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(runtime)
