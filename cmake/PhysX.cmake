# cmake/PhysX.cmake
# Descarga y compila PhysX 5.x (NVIDIA-Omniverse/PhysX) desde fuente vía FetchContent.
# Solo soportado en Windows/Linux (PhysX de NVIDIA no soporta macOS oficialmente).
#
# Define:
#   PHYSX_FOUND    - TRUE si PhysX se descargó y configuró correctamente
#   PhysX::SDK     - target INTERFACE con las libs necesarias (solo si PHYSX_FOUND)

set(PHYSX_FOUND FALSE)

if(WIN32 OR (UNIX AND NOT APPLE))
    if(WIN32)
        set(_PHYSX_TARGET_PLATFORM "windows")
    else()
        set(_PHYSX_TARGET_PLATFORM "linux")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        physx
        GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git
        GIT_TAG        110.0-omni-and-physx-5.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(physx)
    if(NOT physx_POPULATED)
        FetchContent_Populate(physx)
    endif()

    set(PHYSX_ROOT_DIR "${physx_SOURCE_DIR}/physx" CACHE INTERNAL "PhysX SDK root")
    set(TARGET_BUILD_PLATFORM "${_PHYSX_TARGET_PLATFORM}" CACHE INTERNAL "PhysX target platform")
    set(PX_GENERATE_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)

    # PhysX's own CMake (NvidiaBuildOptions.cmake) requires these two output-dir
    # variables to be defined — normally set by NVIDIA's GenerateProjects.bat to
    # PHYSX_ROOT_DIR itself. We redirect them into our own build tree instead of
    # the fetched source tree, so the source stays pristine.
    set(PX_OUTPUT_LIB_DIR "${CMAKE_BINARY_DIR}/physx_build" CACHE INTERNAL "PhysX lib output dir")
    set(PX_OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/physx_build" CACHE INTERNAL "PhysX bin output dir")

    if(WIN32)
        # The "public" flavor of PhysX's Windows CMake always runs in
        # PUBLIC_RELEASE mode, which unconditionally FILE(COPY)'s freeglut
        # DLLs from an NVIDIA-internal package-manager path (PM_freeglut_PATH)
        # that doesn't exist outside NVIDIA's build farm. We don't use PhysX's
        # snippets/samples, so the DLLs themselves are never loaded — we just
        # need the copy to not fail. Point it at empty placeholder files.
        set(_PHYSX_FREEGLUT_STUB_DIR "${CMAKE_BINARY_DIR}/physx_stub/freeglut/bin/win64")
        file(MAKE_DIRECTORY "${_PHYSX_FREEGLUT_STUB_DIR}")
        if(NOT EXISTS "${_PHYSX_FREEGLUT_STUB_DIR}/freeglut.dll")
            file(TOUCH "${_PHYSX_FREEGLUT_STUB_DIR}/freeglut.dll")
        endif()
        if(NOT EXISTS "${_PHYSX_FREEGLUT_STUB_DIR}/freeglutd.dll")
            file(TOUCH "${_PHYSX_FREEGLUT_STUB_DIR}/freeglutd.dll")
        endif()
        set(PHYSX_SLN_FREEGLUT_PATH "${CMAKE_BINARY_DIR}/physx_stub/freeglut/bin" CACHE INTERNAL "PhysX freeglut stub copy path")
    endif()

    if(WIN32)
        # On Windows/MSVC, PhysX's CMake picks a debug/release C runtime library
        # via CMAKE_MSVC_RUNTIME_LIBRARY based on the active config. By default
        # (NV_USE_DEBUG_WINCRT=OFF) its "debug" config flags *also* define NDEBUG
        # (PhysX's "debug" maps closer to a checked/optimized build), while the
        # debug DLL runtime (/MDd) implicitly defines _DEBUG — PhysX's own
        # PxPreprocessor.h then fails a static_assert-style #error requiring
        # exactly one of NDEBUG/_DEBUG. Forcing NV_USE_DEBUG_WINCRT=ON makes its
        # debug config define only _DEBUG, matching the debug CRT and resolving
        # the conflict for our single-config (Ninja) Debug build.
        set(NV_USE_DEBUG_WINCRT ON CACHE BOOL "" FORCE)
    endif()

    if(EXISTS "${PHYSX_ROOT_DIR}")
        # PhysX's own CMake hardcodes /std:c++14 (MSVC) via its own flag string.
        # Our top-level CMAKE_CXX_STANDARD (20) would otherwise leak into every
        # target created inside this add_subdirectory (targets capture
        # CMAKE_CXX_STANDARD at creation time), appending a conflicting
        # /std:c++20 that wins (last flag on the command line) and builds PhysX
        # against a C++ standard it wasn't validated against. Unset it for the
        # duration of PhysX's own subdirectory so its own flags are authoritative.
        set(_DT_SAVED_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
        set(_DT_SAVED_CXX_STANDARD_REQUIRED "${CMAKE_CXX_STANDARD_REQUIRED}")
        unset(CMAKE_CXX_STANDARD)
        unset(CMAKE_CXX_STANDARD_REQUIRED)

        add_subdirectory(${PHYSX_ROOT_DIR}/compiler/public ${CMAKE_BINARY_DIR}/physx_build)

        set(CMAKE_CXX_STANDARD "${_DT_SAVED_CXX_STANDARD}")
        set(CMAKE_CXX_STANDARD_REQUIRED "${_DT_SAVED_CXX_STANDARD_REQUIRED}")

        set(PHYSX_FOUND TRUE)
    else()
        message(WARNING "PhysX source no se descargó correctamente en ${PHYSX_ROOT_DIR}")
    endif()
endif()

if(PHYSX_FOUND)
    add_library(PhysX::SDK INTERFACE IMPORTED)
    target_link_libraries(PhysX::SDK INTERFACE
        PhysX
        PhysXCommon
        PhysXFoundation
        PhysXExtensions
        PhysXPvdSDK
    )
    target_include_directories(PhysX::SDK INTERFACE
        ${PHYSX_ROOT_DIR}/include
    )
else()
    message(WARNING
        "PhysX SDK no disponible en esta plataforma — física no estará disponible.\n"
        "  Soportado solo en Windows/Linux."
    )
endif()
