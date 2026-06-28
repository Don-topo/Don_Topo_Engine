add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
)

target_include_directories(DonTopoEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${stb_SOURCE_DIR}
)

target_link_libraries(DonTopoEngine
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
)

if(FMOD_FOUND)
    target_link_libraries(DonTopoEngine PUBLIC FMOD::FMOD)
    target_compile_definitions(DonTopoEngine PUBLIC DT_FMOD_ENABLED)
endif()

target_compile_features(DonTopoEngine PUBLIC cxx_std_20)
