# Repo Setup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish the full foundational repository structure for Don Topo Engine — .gitignore, README, CMakeLists hierarchy, and minimal source skeleton that configures and builds cleanly.

**Architecture:** Flat modular layout with `engine/` as a static library and `sandbox/` as a test executable. Open-source dependencies (GLFW, GLM) fetched via CMake FetchContent; proprietary SDKs (Vulkan, FMOD) located via `find_package` using a custom `cmake/FindFMOD.cmake` module.

**Tech Stack:** C++20, CMake 3.25+, Vulkan SDK, FMOD SDK, GLFW 3.4 (FetchContent), GLM 1.0.1 (FetchContent)

## Global Constraints

- CMake minimum version: 3.25
- C++ standard: 20, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`
- Target platforms: Windows, Linux, macOS
- GLFW pinned tag: `3.4`
- GLM pinned tag: `1.0.1`
- Engine library name: `DonTopoEngine` (static)
- Sandbox executable name: `Sandbox`
- FMOD imported target name: `FMOD::FMOD`
- Vulkan imported target name: `Vulkan::Vulkan` (CMake built-in)
- Public engine headers live under `engine/include/DonTopo/`

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `.gitignore` | Create | Ignore build dirs, IDE files, compiled artifacts, OS artifacts |
| `README.md` | Create | Project description, stack, prerequisites, build instructions |
| `cmake/FindFMOD.cmake` | Create | Locate FMOD SDK, create `FMOD::FMOD` imported target |
| `CMakeLists.txt` | Create | Root: C++20 config, FetchContent, find_package, subdirs |
| `engine/CMakeLists.txt` | Create | Static lib target, include dirs, link dependencies |
| `engine/include/DonTopo/.gitkeep` | Create | Preserve public headers directory in git |
| `engine/src/Engine.cpp` | Create | Minimal placeholder so static lib compiles |
| `sandbox/CMakeLists.txt` | Create | Executable target linked to DonTopoEngine |
| `sandbox/src/main.cpp` | Create | Minimal entry point |

---

## Task 1: Directory skeleton + .gitignore

**Files:**
- Create: `.gitignore`
- Create: `engine/include/DonTopo/.gitkeep`
- Create: `engine/src/Engine.cpp`
- Create: `sandbox/src/main.cpp`

**Interfaces:**
- Produces: tracked directory structure; `.gitignore` rules for all subsequent generated files

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p engine/include/DonTopo engine/src sandbox/src cmake
```

- [ ] **Step 2: Create placeholder files to track empty dirs**

`engine/include/DonTopo/.gitkeep` — empty file.

`engine/src/Engine.cpp`:
```cpp
#include "DonTopo/Engine.h"
```

`engine/include/DonTopo/Engine.h`:
```cpp
#pragma once

namespace DonTopo {

class Engine {
public:
    Engine() = default;
    ~Engine() = default;
};

} // namespace DonTopo
```

`sandbox/src/main.cpp`:
```cpp
#include "DonTopo/Engine.h"

int main()
{
    DonTopo::Engine engine;
    return 0;
}
```

- [ ] **Step 3: Create .gitignore**

`.gitignore`:
```gitignore
# Build directories
build/
cmake-build-*/
.cmake/
out/

# CMake generated (in build dir, already covered by build/ — listed explicitly
# for in-source build users)
CMakeFiles/
CMakeCache.txt
cmake_install.cmake

# IDEs
.vs/
.vscode/
.idea/
*.user
*.suo
*.sln.docinfo
*.vcxproj.user
.clangd/
compile_commands.json

# Compiled artifacts
*.o
*.obj
*.a
*.lib
*.exe
*.dll
*.so
*.dylib
*.pdb
*.ilk
*.exp
*.map

# OS
.DS_Store
.DS_Store?
._*
.Spotlight-V100
.Trashes
Thumbs.db
desktop.ini
ehthumbs.db
```

- [ ] **Step 4: Verify git status shows correct tracked/ignored files**

Run:
```bash
git status
git check-ignore -v build/ .vs/ CMakeFiles/
```

Expected: `build/`, `.vs/`, `CMakeFiles/` all reported as ignored.

- [ ] **Step 5: Commit**

```bash
git add .gitignore engine/ sandbox/ cmake/
git commit -m "chore: add project skeleton and .gitignore"
```

---

## Task 2: cmake/FindFMOD.cmake

**Files:**
- Create: `cmake/FindFMOD.cmake`

**Interfaces:**
- Consumes: nothing
- Produces: `FMOD::FMOD` imported target, `FMOD_FOUND` variable — consumed by root `CMakeLists.txt`

- [ ] **Step 1: Create cmake/FindFMOD.cmake**

```cmake
# FindFMOD.cmake
# Locates the FMOD Studio Core SDK and creates the FMOD::FMOD imported target.
#
# Usage:
#   find_package(FMOD REQUIRED)
#
# Set FMOD_DIR (cmake var or env var) to the FMOD SDK install root if not found automatically.
# Example: cmake -DFMOD_DIR="C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows" ..
#
# Defines:
#   FMOD_FOUND             - True if found
#   FMOD_INCLUDE_DIR       - Path to fmod.hpp
#   FMOD_LIBRARY           - Path to import library / shared lib
#   FMOD::FMOD             - Imported target

include(FindPackageHandleStandardArgs)

# Resolve architecture suffix used in FMOD's lib layout
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_FMOD_ARCH "x64")
else()
    set(_FMOD_ARCH "x86")
endif()

# Search roots: cmake var > env var > platform defaults
set(_FMOD_ROOTS
    "${FMOD_DIR}"
    "$ENV{FMOD_DIR}"
)

if(WIN32)
    list(APPEND _FMOD_ROOTS
        "C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
        "C:/Program Files/FMOD SoundSystem/FMOD Studio API Windows"
    )
elseif(APPLE)
    list(APPEND _FMOD_ROOTS
        "/Applications/FMOD Studio API Mac"
        "$ENV{HOME}/FMOD Studio API Mac"
    )
elseif(UNIX)
    list(APPEND _FMOD_ROOTS
        "/opt/fmod"
        "$ENV{HOME}/fmod"
    )
endif()

find_path(FMOD_INCLUDE_DIR
    NAMES fmod.hpp
    PATHS ${_FMOD_ROOTS}
    PATH_SUFFIXES api/core/inc inc include
    NO_DEFAULT_PATH
)

if(WIN32)
    find_library(FMOD_LIBRARY
        NAMES fmod_vc fmod
        PATHS ${_FMOD_ROOTS}
        PATH_SUFFIXES
            "api/core/lib/${_FMOD_ARCH}"
            api/core/lib
            lib
        NO_DEFAULT_PATH
    )
else()
    find_library(FMOD_LIBRARY
        NAMES fmod
        PATHS ${_FMOD_ROOTS}
        PATH_SUFFIXES
            "api/core/lib/${_FMOD_ARCH}"
            api/core/lib
            lib
        NO_DEFAULT_PATH
    )
endif()

find_package_handle_standard_args(FMOD
    REQUIRED_VARS FMOD_LIBRARY FMOD_INCLUDE_DIR
    FAIL_MESSAGE
        "FMOD SDK not found.\n"
        "  1. Download from https://www.fmod.com/download\n"
        "  2. Install the FMOD Studio API\n"
        "  3. Pass -DFMOD_DIR=<install_root> to cmake or set the FMOD_DIR environment variable"
)

if(FMOD_FOUND AND NOT TARGET FMOD::FMOD)
    add_library(FMOD::FMOD UNKNOWN IMPORTED)
    set_target_properties(FMOD::FMOD PROPERTIES
        IMPORTED_LOCATION             "${FMOD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FMOD_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(FMOD_INCLUDE_DIR FMOD_LIBRARY)
```

- [ ] **Step 2: Verify syntax (no cmake configure needed yet)**

```bash
cmake --help-command find_package   # confirms cmake version supports it
```

Expected: help text printed without error.

- [ ] **Step 3: Commit**

```bash
git add cmake/FindFMOD.cmake
git commit -m "build: add FindFMOD cmake module"
```

---

## Task 3: CMakeLists.txt hierarchy + cmake configure checkpoint

**Files:**
- Create: `CMakeLists.txt`
- Create: `engine/CMakeLists.txt`
- Create: `sandbox/CMakeLists.txt`

**Interfaces:**
- Consumes: `FMOD::FMOD` from `cmake/FindFMOD.cmake`; `Vulkan::Vulkan` from CMake built-in; `glfw`, `glm::glm` from FetchContent
- Produces: `DonTopoEngine` static lib target; `Sandbox` executable target

- [ ] **Step 1: Create root CMakeLists.txt**

```cmake
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
find_package(FMOD REQUIRED)

# Future dependencies go here:
# find_package(PhysX REQUIRED)   # physics
# FetchContent_Declare(gainput ...) # dedicated input

# ─── Targets ─────────────────────────────────────────────────────────────────

add_subdirectory(engine)
add_subdirectory(sandbox)
```

- [ ] **Step 2: Create engine/CMakeLists.txt**

```cmake
add_library(DonTopoEngine STATIC
    src/Engine.cpp
)

target_include_directories(DonTopoEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DonTopoEngine
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
        FMOD::FMOD
)

target_compile_features(DonTopoEngine PUBLIC cxx_std_20)
```

- [ ] **Step 3: Create sandbox/CMakeLists.txt**

```cmake
add_executable(Sandbox
    src/main.cpp
)

target_link_libraries(Sandbox
    PRIVATE
        DonTopoEngine
)
```

- [ ] **Step 4: Configure cmake (checkpoint — must pass before committing)**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

Expected output contains:
```
-- Configuring done
-- Build files have been written to: .../build
```

If FMOD not installed: set `FMOD_DIR` env var or pass `-DFMOD_DIR=<path>`.
If Vulkan not found: install Vulkan SDK from lunarg.com.

- [ ] **Step 5: Build (checkpoint — must pass before committing)**

```bash
cmake --build build --config Debug
```

Expected: `Sandbox` executable produced with no errors.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt engine/CMakeLists.txt sandbox/CMakeLists.txt
git commit -m "build: add CMakeLists hierarchy — engine lib + sandbox exe"
```

---

## Task 4: README.md

**Files:**
- Create: `README.md`

**Interfaces:**
- Produces: human-readable project documentation

- [ ] **Step 1: Create README.md**

```markdown
# Don Topo Engine

A cross-platform, Vulkan-based game engine written in C++20.

## Tech Stack

| Component | Library |
|---|---|
| Graphics | Vulkan |
| Window / Input | GLFW 3.4 |
| Math | GLM 1.0.1 |
| Audio | FMOD Studio |
| Build | CMake 3.25+ |
| Language | C++20 |

## Prerequisites

All of these must be installed before configuring:

| Tool | Version | Download |
|---|---|---|
| CMake | 3.25+ | https://cmake.org/download |
| Vulkan SDK | Latest | https://vulkan.lunarg.com/sdk/home |
| FMOD Studio API | Latest | https://www.fmod.com/download |
| C++20 compiler | — | GCC 11+, Clang 13+, or MSVC 2022+ |

GLFW and GLM are downloaded automatically by CMake at configure time.

## Build

```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# If FMOD is not in a standard location:
cmake -S . -B build -DFMOD_DIR="/path/to/fmod/api" -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --config Debug

# Run sandbox
./build/sandbox/Sandbox          # Linux / macOS
build\sandbox\Debug\Sandbox.exe  # Windows
```

## Project Structure

```
Don_Topo_Engine/
├── cmake/          # Custom Find modules for external SDKs
├── engine/         # Core engine (static library: DonTopoEngine)
│   ├── include/    # Public headers (DonTopo/)
│   └── src/        # Implementation
└── sandbox/        # Test playground executable (Sandbox)
```

## Planned / Future Dependencies

| System | Candidate libraries |
|---|---|
| Physics | Jolt Physics, Bullet, PhysX |
| Dedicated input | gainput, SDL3 (input only) |
| Editor | TBD — separate `editor/` module |

## License

TBD
```

- [ ] **Step 2: Verify README renders correctly**

Open `README.md` in a Markdown previewer (VS Code: `Ctrl+Shift+V`). Confirm tables and code blocks display properly.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: add README with build instructions and project overview"
```

---

## Self-Review Checklist (executor: verify before marking plan done)

- [ ] `cmake -S . -B build` completes without errors on developer machine
- [ ] `cmake --build build` produces `Sandbox` executable
- [ ] `git status` shows no untracked build artifacts
- [ ] `cmake/FindFMOD.cmake` produces a clear error message when `FMOD_DIR` is not set
- [ ] All spec requirements covered:
  - [x] .gitignore covers build dirs, IDEs, artifacts, OS files
  - [x] README has stack, prerequisites, build instructions, structure, future deps
  - [x] CMake 3.25+, C++20, extensions off
  - [x] GLFW + GLM via FetchContent, pinned tags
  - [x] Vulkan via find_package
  - [x] FMOD via cmake/FindFMOD.cmake with FMOD_DIR support
  - [x] engine/ as static lib, sandbox/ as executable
  - [x] cmake/ module path registered
  - [x] Future dependencies comment block in root CMakeLists
