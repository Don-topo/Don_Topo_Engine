add_library(DonTopoEngine STATIC
    src/Core/Engine.cpp
    src/Core/Window.cpp
    src/Renderer/Renderer.cpp
    src/Editor/EditorUI.cpp
    src/Editor/LogPanel.cpp
    src/Editor/ScenePanel.cpp
    src/Editor/ViewportPanel.cpp
    src/Editor/PropertiesPanel.cpp
    src/Editor/ContentBrowserPanel.cpp
    src/Editor/AnimatorPanel.cpp
    src/Editor/Command.cpp
    src/Editor/UndoManager.cpp
    src/Renderer/Skybox.cpp
    src/Renderer/SplashScreen.cpp
    src/Renderer/ModelLoader.cpp
    src/Renderer/SkinnedMeshPacking.cpp
    src/Renderer/SkinnedMeshAnimations.cpp
    src/Renderer/Cube.cpp
    src/Renderer/Sphere.cpp
    src/Renderer/Plane.cpp
    src/Renderer/Capsule.cpp
    src/Core/Camera.cpp
    src/Core/CameraComponent.cpp
    src/Core/AnimatorComponent.cpp
    src/Core/GameObject.cpp
    src/Core/Scene.cpp
    src/Files/FileManager.cpp
    src/Audio/AudioManager.cpp
    src/Audio/AudioClipComponent.cpp
    src/Scripting/ScriptComponent.cpp
    src/Scripting/ScriptManager.cpp
    src/Core/Input.cpp
    src/Scripting/ScriptBindings.cpp
    src/Physics/PhysicsManager.cpp
    src/Physics/Rigidbody.cpp
    src/Physics/Colliders/Collider.cpp
    src/Physics/Colliders/BoxCollider.cpp
    src/Physics/Colliders/SphereCollider.cpp
    src/Physics/Colliders/CapsuleCollider.cpp
    src/Physics/Colliders/PlaneCollider.cpp
    src/Renderer/GpuDevice.cpp
    src/Renderer/GpuResources.cpp
    src/Editor/Gizmos.cpp
    src/Editor/ScriptEditorPanel.cpp
    src/Editor/GameExporter.cpp
    src/Scripting/LuaSyntaxCheck.cpp
    src/Scripting/LuaApiReference.cpp
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
        imgui_node_editor
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
    set_source_files_properties(src/Scripting/ScriptBindings.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

add_subdirectory(tests)
