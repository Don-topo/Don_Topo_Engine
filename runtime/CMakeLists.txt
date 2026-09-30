add_executable(DonTopoRuntime main.cpp)

# Core only: the runtime does not link the editor and therefore not ImGui.
target_link_libraries(DonTopoRuntime
    PRIVATE
        DonTopoCore
)

target_compile_features(DonTopoRuntime PRIVATE cxx_std_20)

# The exported game must not open a console window behind the game's own,
# so it links against the WINDOWS subsystem. /ENTRY:mainCRTStartup
# keeps main.cpp's standard main() (without it, the linker would demand WinMain).
# The trade-off: without a console, stdout/stderr have nowhere to write, so main()
# redirects them to game.log and shows fatal errors in a MessageBox.
# The editor (Sandbox) stays a console app on purpose: there the terminal is useful.
if(WIN32)
    set_target_properties(DonTopoRuntime PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(DonTopoRuntime PRIVATE /ENTRY:mainCRTStartup)
endif()

# The editor's exporter copies this binary into the package, and looks for it in its
# own working directory (the same one where Sandbox.exe lives). That is why this
# add_subdirectory comes AFTER sandbox's in the root CMakeLists: the
# generator expression needs the Sandbox target to be defined already.
add_custom_command(TARGET DonTopoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DonTopoRuntime>
        $<TARGET_FILE_DIR:Sandbox>/${DT_RUNTIME_FILE_NAME}
    COMMENT "Copying ${DT_RUNTIME_FILE_NAME} next to the editor"
)

# Shaders and assets next to the runtime binary, so it can be RUN from its
# own build directory (testing a scene without going through Export Game). The
# runtime looks for them relative to its working directory, and without them it does
# not even start: it dies at the first pipeline that cannot find its .spv.
#
# The exported game does not depend on this (GameExporter copies shaders and assets
# into the package on its own), but the freshly built binary does, and until now it
# did not have them (sandbox's CMakeLists copies them; this one did not).
#
# The Shaders target is defined by sandbox/CMakeLists.txt and compiles to
# ${CMAKE_BINARY_DIR}/spv; sandbox's add_subdirectory comes before runtime's
# in the root CMakeLists, so it already exists here. copy_directory (and not
# copy_if_different) so that a recompiled .spv always arrives.
add_dependencies(DonTopoRuntime Shaders)

add_custom_command(TARGET DonTopoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:DonTopoRuntime>/assets
    COMMENT "Copying shaders and assets next to DonTopoRuntime"
)

# Same shaders, another representation: the runtime starts with the backend its
# configuration says, so it needs the DXIL files next to the SPIR-V ones.
if(DTE_ENABLE_D3D12)
    add_custom_command(TARGET DonTopoRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
        COMMENT "Copying DXIL shaders next to DonTopoRuntime"
    )
endif()

dt_copy_fmod_runtime(DonTopoRuntime)

# The exported game does not depend on the player's libstdc++ (it avoids the
# "GLIBCXX_3.4.xx not found" on old distros). glibc is still dynamic:
# the exporter warns about the minimum version.
if(UNIX AND NOT APPLE)
    target_link_options(DonTopoRuntime PRIVATE -static-libstdc++ -static-libgcc)
    # Only $ORIGIN in the rpath: CMake adds on its own the FMOD folder of
    # the build machine, an absolute path that has no business in a game
    # that gets shipped.
    set_target_properties(DonTopoRuntime PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "\$ORIGIN")
endif()
