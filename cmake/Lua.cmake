# cmake/Lua.cmake
# Lua 5.4 from the official GitHub mirror via FetchContent.
# The repo has no CMakeLists (only a Makefile), so a static lib of our own
# is defined here with the core's .c files (same pattern as PhysX.cmake).
# lua.c/luac.c (mains of the standalone interpreter) and onelua.c
# (the amalgamation, it would duplicate symbols) are excluded.

# The root project is LANGUAGES CXX; Lua is pure C.
enable_language(C)

include(FetchContent)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()

set(_LUA_CORE_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
    ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
    lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
    lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
    lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM _LUA_CORE_SOURCES PREPEND "${lua_SOURCE_DIR}/")

add_library(lua_lib STATIC ${_LUA_CORE_SOURCES})
target_include_directories(lua_lib PUBLIC ${lua_SOURCE_DIR})
