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

# Search roots: local third_party > cmake var > env var > platform defaults
set(_FMOD_ROOTS
    "${CMAKE_SOURCE_DIR}/third_party/fmod"
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
        "  Option A (recommended): download 'FMOD Studio API' zip from https://www.fmod.com/download,\n"
        "    extract it, and copy/move the extracted folder to <project_root>/third_party/fmod/\n"
        "    (CMake will find it automatically on the next configure).\n"
        "  Option B: pass -DFMOD_DIR=<install_root> or set the FMOD_DIR environment variable."
)

if(FMOD_FOUND AND NOT TARGET FMOD::FMOD)
    add_library(FMOD::FMOD UNKNOWN IMPORTED)
    set_target_properties(FMOD::FMOD PROPERTIES
        IMPORTED_LOCATION             "${FMOD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FMOD_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(FMOD_INCLUDE_DIR FMOD_LIBRARY)
