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

# Future dependencies go here:
# find_package(PhysX REQUIRED)   # physics
# FetchContent_Declare(gainput ...) # dedicated input

# ─── Targets ─────────────────────────────────────────────────────────────────

add_subdirectory(engine)
add_subdirectory(sandbox)
