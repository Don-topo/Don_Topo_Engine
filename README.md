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
