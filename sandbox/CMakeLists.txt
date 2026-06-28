add_executable(Sandbox
    src/main.cpp
)

target_link_libraries(Sandbox
    PRIVATE
        DonTopoEngine
)

# Compilar shaders GLSL -> SPIR-V con glslc
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin)

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR $<TARGET_FILE_DIR:Sandbox>/shaders)

file(GLOB GLSL_SOURCES
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
)

foreach(SHADER ${GLSL_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUT "${CMAKE_BINARY_DIR}/spv/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT  ${SPV_OUT}
        COMMAND ${GLSLC} ${SHADER} -o ${SPV_OUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_FILES ${SPV_OUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_FILES})
add_dependencies(Sandbox Shaders)

# Copiar assets al lado del ejecutable
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:Sandbox>/assets
    COMMENT "Copying assets to output directory"
)

# Copiar los .spv compilados al lado del ejecutable
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        ${SHADER_OUT_DIR}
    COMMENT "Copying compiled shaders to output directory"
)
