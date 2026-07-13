add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/EditorUI.cpp
    src/Skybox.cpp
    src/ModelLoader.cpp
    src/Cube.cpp
    src/Sphere.cpp
    src/Plane.cpp
    src/Capsule.cpp
    src/Camera.cpp
    src/GameObject.cpp
    src/Scene.cpp
    src/FileManager.cpp
    src/AudioManager.cpp
    src/AudioClipComponent.cpp
    src/ScriptComponent.cpp
    src/ScriptManager.cpp
    src/Input.cpp
    src/ScriptBindings.cpp
    src/PhysicsManager.cpp
    src/BoxCollider.cpp
    src/SphereCollider.cpp
    src/CapsuleCollider.cpp
    src/PlaneCollider.cpp
    src/GpuDevice.cpp
    src/GpuResources.cpp
    src/Gizmos.cpp
    src/ScriptEditorPanel.cpp
    src/LuaSyntaxCheck.cpp
    src/LuaApiReference.cpp
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
        assimp
        imgui_backend
        imgui_filedialog
        imguizmo
        imgui_texteditor
        nlohmann_json::nlohmann_json
        sol2_lib
)

if(FMOD_FOUND)
    target_link_libraries(DonTopoEngine PUBLIC FMOD::FMOD)
    target_compile_definitions(DonTopoEngine PUBLIC DT_FMOD_ENABLED)
endif()

if(PHYSX_FOUND)
    target_link_libraries(DonTopoEngine PUBLIC PhysX::SDK)
    target_compile_definitions(DonTopoEngine PUBLIC DT_PHYSX_ENABLED)
endif()

target_compile_features(DonTopoEngine PUBLIC cxx_std_20)

# ScriptBindings.cpp registra muchos usertypes de sol2 en una sola TU: el
# volumen de plantillas generadas supera el límite de secciones de MSVC
# (C1128) sin /bigobj.
if(MSVC)
    set_source_files_properties(src/ScriptBindings.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
