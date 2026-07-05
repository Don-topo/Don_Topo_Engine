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
    GIT_TAG        master
    GIT_SHALLOW    TRUE
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
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
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

# ImGuiFileDialog — embedded file browser (NoDialog mode)
# Usamos Populate (no MakeAvailable) para descargar fuentes sin ejecutar
# el CMakeLists.txt de la librería, que busca imgui via find_package y falla.
FetchContent_Declare(
    ImGuiFileDialog
    GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
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
    GIT_TAG        master
    GIT_SHALLOW    TRUE
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

# PhysX SDK — física (compilado desde fuente, ver cmake/PhysX.cmake)
include(cmake/PhysX.cmake)

# Future dependencies go here:
# FetchContent_Declare(gainput ...) # dedicated input

# ─── Targets ─────────────────────────────────────────────────────────────────

add_subdirectory(engine)
add_subdirectory(sandbox)
