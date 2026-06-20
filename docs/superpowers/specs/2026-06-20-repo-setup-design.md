# Don Topo Engine — Repo Setup Design

**Date:** 2026-06-20
**Scope:** Initial repository setup — .gitignore, README, CMakeLists.txt structure

---

## Overview

Set up the foundational structure for Don Topo Engine, a cross-platform Vulkan-based game engine written in C++20. This phase establishes the build system, dependency management strategy, and project layout that all future work will build on.

---

## Technical Stack

| Component | Choice | Rationale |
|---|---|---|
| Language | C++20 | Concepts, ranges, coroutines; broad compiler support |
| Graphics API | Vulkan | Modern, high-performance, cross-platform |
| Window / Input | GLFW | Lightweight, focused, Vulkan-native, cross-platform |
| Math | GLM | Header-only, GLSL-mirrored API, zero-overhead |
| Audio | FMOD | Professional audio middleware, proprietary SDK |
| Build system | CMake 3.25+ | Industry standard, cross-platform |
| Dependency mgmt | FetchContent (open-source) + cmake/ Find modules (proprietary SDKs) |

**Target platforms:** Windows, Linux, macOS

---

## Directory Structure

```
Don_Topo_Engine/
├── CMakeLists.txt              # Root: project config, FetchContent, subdirectories
├── .gitignore
├── README.md
├── cmake/                      # Custom Find modules for external SDKs
│   └── FindFMOD.cmake          # Locates FMOD SDK on developer machine
├── engine/
│   ├── CMakeLists.txt          # Target: DonTopoEngine (static lib)
│   ├── include/
│   │   └── DonTopo/            # Public headers
│   └── src/                    # Implementation files
└── sandbox/
    ├── CMakeLists.txt          # Target: Sandbox (executable for testing)
    └── src/
        └── main.cpp            # Minimal entry point
```

`vendor/` does not exist as a committed directory — FetchContent manages downloaded sources inside the CMake build directory, which is gitignored.

---

## CMake Design

### Root `CMakeLists.txt`

- `cmake_minimum_required(VERSION 3.25)`
- `project(DonTopoEngine VERSION 0.1.0 LANGUAGES CXX)`
- `set(CMAKE_CXX_STANDARD 20)` + `CMAKE_CXX_STANDARD_REQUIRED ON`
- `list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")` — registers custom Find modules
- FetchContent declarations for GLFW and GLM
- `find_package(Vulkan REQUIRED)` — requires Vulkan SDK installed on host
- `find_package(FMOD REQUIRED)` — uses `cmake/FindFMOD.cmake`, requires FMOD SDK installed
- `add_subdirectory(engine)` + `add_subdirectory(sandbox)`
- Clearly commented `# Dependencies` section for easy future extension

### `engine/CMakeLists.txt`

- `add_library(DonTopoEngine STATIC)`
- `target_sources(...)` — all .cpp under `engine/src/`
- `target_include_directories(DonTopoEngine PUBLIC include/)` — exposes `DonTopo/` headers
- `target_link_libraries(DonTopoEngine PUBLIC Vulkan::Vulkan glfw glm::glm FMOD::FMOD)`
- `target_compile_features(DonTopoEngine PUBLIC cxx_std_20)`

### `sandbox/CMakeLists.txt`

- `add_executable(Sandbox)`
- `target_sources(...)` — `sandbox/src/main.cpp`
- `target_link_libraries(Sandbox PRIVATE DonTopoEngine)`

---

## Dependency Details

### GLFW (FetchContent)
- Fetched from official GitHub repo, pinned to a stable tag
- `GLFW_BUILD_DOCS OFF`, `GLFW_BUILD_TESTS OFF`, `GLFW_BUILD_EXAMPLES OFF`

### GLM (FetchContent)
- Header-only, fetched from official GitHub repo, pinned to stable tag
- No build step required

### Vulkan SDK
- Installed externally by developer (lunarg.com/vulkan-sdk)
- Found via `find_package(Vulkan REQUIRED)` — CMake built-in
- Error message if not found: instructs developer to install Vulkan SDK

### FMOD SDK
- Proprietary, installed externally by developer (fmod.com)
- Found via `cmake/FindFMOD.cmake` using `FMOD_DIR` env var or common install paths
- Error message if not found: instructs developer to set `FMOD_DIR`

---

## .gitignore Coverage

- Build directories: `build/`, `cmake-build-*/`, `.cmake/`, `out/`
- IDEs: `.vs/`, `.vscode/`, `.idea/`, `*.user`, `*.suo`, `*.sln.docinfo`
- Compiled artifacts: `*.o`, `*.obj`, `*.a`, `*.lib`, `*.exe`, `*.dll`, `*.so`, `*.dylib`, `*.pdb`
- OS artifacts: `.DS_Store`, `Thumbs.db`, `desktop.ini`
- FetchContent cache lives inside build dir — already covered

---

## README Structure

1. Project description (one paragraph)
2. Tech stack table
3. Prerequisites (CMake 3.25+, Vulkan SDK, FMOD SDK, C++20 compiler)
4. Build instructions (cmake configure + build commands for all platforms)
5. Project structure overview
6. Planned / Future dependencies section (physics, dedicated input lib, etc.)

---

## Extensibility

The design explicitly leaves room for:
- **Physics:** add `cmake/FindPhysX.cmake` or FetchContent for Jolt/Bullet; add `physics/` module
- **Input:** dedicated input library (e.g., gainput) as FetchContent or Find module
- **Editor:** `editor/` as new `add_subdirectory` in root CMakeLists
- **Additional engine modules:** `engine/renderer/`, `engine/audio/`, etc. as future subdirectories or source groups within `engine/`

No structural changes needed to accommodate any of the above.

---

## Out of Scope (this phase)

- Actual Vulkan initialization code
- FMOD integration code
- Engine architecture (ECS, scene graph, etc.)
- CI/CD pipeline
- Testing framework
