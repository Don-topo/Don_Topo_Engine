add_executable(Sandbox
    src/main.cpp
)
# Windows resource (the .exe icon): it does not exist outside Windows.
if(WIN32)
    target_sources(Sandbox PRIVATE Sandbox.rc)
endif()

# DonTopoEditor pulls in DonTopoCore as PUBLIC.
target_link_libraries(Sandbox
    PRIVATE
        DonTopoEditor
)

# Compile GLSL shaders -> SPIR-V with glslc
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR $<TARGET_FILE_DIR:Sandbox>/shaders)

file(GLOB GLSL_SOURCES
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
    "${SHADER_SRC_DIR}/*.comp"
)

# GLSL fragments included from other shaders. They do NOT go into the GLOB
# above (a .glsl is not a stage and glslc would not know which profile to compile it
# with), but they must be in DEPENDS: without that, editing one of them leaves the
# .spv files that include it unregenerated, and the symptom is a shader that behaves
# like the previous version with nothing failing.
#
# They exist because the alternative is copying the same function into several shaders,
# and that already went wrong: pbr.frag and fog.comp sample the SAME shadow map, each
# had its own copy of the criterion, and changing one and not the other left the
# in-scattering pointing one way and the shadow map built the other (H65).
file(GLOB GLSL_INCLUDES "${SHADER_SRC_DIR}/*.glsl")

foreach(SHADER ${GLSL_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUT "${CMAKE_BINARY_DIR}/spv/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT  ${SPV_OUT}
        COMMAND ${GLSLC} -I ${SHADER_SRC_DIR} ${SHADER} -o ${SPV_OUT}
        DEPENDS ${SHADER} ${GLSL_INCLUDES}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_FILES ${SPV_OUT})

    # DX12 branch: SPIR-V -> HLSL (spirv-cross) -> DXIL (dxc). It hangs off the .spv, not
    # the .glsl, so the GLSL is not compiled twice and the HLSL is
    # always a translation of EXACTLY the same SPIR-V Vulkan uses.
    #
    # --shader-model 60 is the minimum that maps Vulkan's descriptor sets to
    # HLSL register spaces (layout(set=N) -> space N); below 5.1 there are
    # no spaces and the translation of pbr.frag, with 8 sets, would collide.
    if(DTE_ENABLE_D3D12)
        get_filename_component(SHADER_EXT ${SHADER} EXT)
        if(SHADER_EXT STREQUAL ".vert")
            set(DXC_PROFILE vs_6_0)
        elseif(SHADER_EXT STREQUAL ".frag")
            set(DXC_PROFILE ps_6_0)
        elseif(SHADER_EXT STREQUAL ".comp")
            set(DXC_PROFILE cs_6_0)
        else()
            message(FATAL_ERROR "Shader extension without a DXC profile: ${SHADER}")
        endif()

        set(HLSL_OUT "${CMAKE_BINARY_DIR}/hlsl/${SHADER_NAME}.hlsl")
        set(DXIL_OUT "${CMAKE_BINARY_DIR}/dxil/${SHADER_NAME}.dxil")
        add_custom_command(
            OUTPUT  ${DXIL_OUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/hlsl
            COMMAND ${SPIRV_CROSS} --hlsl --shader-model 60 ${SPV_OUT} --output ${HLSL_OUT}
            COMMAND ${DXC} -T ${DXC_PROFILE} -E main ${HLSL_OUT} -Fo ${DXIL_OUT}
            DEPENDS ${SPV_OUT}
            BYPRODUCTS ${HLSL_OUT}
            COMMENT "Translating shader ${SHADER_NAME} to DXIL (${DXC_PROFILE})"
        )
        list(APPEND DXIL_FILES ${DXIL_OUT})
    endif()
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_FILES} ${DXIL_FILES})
add_dependencies(Sandbox Shaders)

# Copy assets next to the executable
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:Sandbox>/assets
    COMMENT "Copying assets to output directory"
)

# Copy the compiled .spv files next to the executable and to the project root
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        ${SHADER_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        ${CMAKE_SOURCE_DIR}/shaders
    COMMENT "Copying compiled shaders to output directory and project shaders/"
)

# The .dxil files go to the same two places as the .spv files, so the DX12 backend
# finds its shaders with the same paths relative to the working directory.
if(DTE_ENABLE_D3D12)
    add_custom_command(TARGET Sandbox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            ${SHADER_OUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            ${CMAKE_SOURCE_DIR}/shaders
        COMMENT "Copying compiled DXIL shaders to output directory and project shaders/"
    )
endif()

# Seed the default imgui.ini (editor layout) only if the executable
# does not have its own yet: that way a fresh clone+build starts with a
# reasonable layout, but the one the user has already customized is not overwritten.
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/imgui.ini
        -DDST=$<TARGET_FILE_DIR:Sandbox>/imgui.ini
        -P ${CMAKE_SOURCE_DIR}/cmake/CopyIfMissing.cmake
    COMMENT "Seeding default imgui.ini layout (if missing)"
)

# MSVC CRT next to the editor executable, so GameExporter can put it
# app-local into the exported game (the package is a folder that gets
# copied: on the player's machine there is no installer to register the VC++
# Redistributable). Same pattern as fmod.dll: first next to the .exe, and the
# exporter picks it up from there.
#
# Release only: the debug CRT is not redistributable, so in Debug
# nothing is copied and GameExporter warns instead. CMAKE_BUILD_TYPE is checked
# because the repo's presets use Ninja (single-config).
#
# _SKIP keeps this from adding install() rules; here we only want the list of
# DLLs InstallRequiredSystemLibraries resolves. Making up paths to
# VC/Redist/... by hand breaks with every toolchain update.
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET Sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                $<TARGET_FILE_DIR:Sandbox>
            COMMENT "Copying MSVC runtime DLLs to output (app-local CRT for exported games)"
        )
    else()
        message(WARNING
            "InstallRequiredSystemLibraries did not find the MSVC CRT: games "
            "exported from this build will only start where the Visual C++ "
            "Redistributable is already installed.")
    endif()
endif()

dt_copy_fmod_runtime(Sandbox)
