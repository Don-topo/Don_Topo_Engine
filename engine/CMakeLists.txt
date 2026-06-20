add_library(DonTopoEngine STATIC
    src/Engine.cpp
)

target_include_directories(DonTopoEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DonTopoEngine
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
        FMOD::FMOD
)

target_compile_features(DonTopoEngine PUBLIC cxx_std_20)
