add_executable(Sandbox
    src/main.cpp
)

target_link_libraries(Sandbox
    PRIVATE
        DonTopoEngine
)

add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:Sandbox>/shaders
    COMMENT "Copying shaders to output directory"
)
