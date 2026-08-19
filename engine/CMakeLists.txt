# El motor, sin nada del editor. Es lo unico que enlaza el runtime: por eso
# aqui no puede aparecer ninguna de las libs de ImGui ni ningun fuente de
# src/Editor/.
add_library(DonTopoCore STATIC
    src/Core/Engine.cpp
    src/Core/Window.cpp
    src/Renderer/RenderBackend.cpp
    src/Renderer/Renderer.cpp
    # Pases del Renderer de Vulkan sacados a su propia clase.
    src/Renderer/Passes/AaPass.cpp
    src/Renderer/Passes/BloomPass.cpp
    src/Renderer/Passes/DepthPrepassPass.cpp
    src/Renderer/Passes/FogPass.cpp
    src/Renderer/Passes/ForwardPlusPass.cpp
    src/Renderer/Passes/IblPass.cpp
    src/Renderer/Passes/ReflectionProbePass.cpp
    src/Renderer/Passes/ShadowPass.cpp
    src/Renderer/Passes/SkinningPass.cpp
    src/Renderer/Passes/SsaoPass.cpp
    src/Renderer/Passes/SsrPass.cpp
    src/Renderer/Passes/MotionBlurPass.cpp
    src/Renderer/Skybox.cpp
    src/Renderer/SplashScreen.cpp
    src/Renderer/ModelLoader.cpp
    src/Renderer/SkinnedBounds.cpp
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
    src/Core/JobSystem.cpp
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
    src/Renderer/SharedGpuMesh.cpp
    src/Renderer/InstanceBatching.cpp
    src/Renderer/DeferredDelete.cpp
    src/Renderer/TransferBatch.cpp
    src/Renderer/AsyncAssetLoader.cpp
    # Gizmos vive en Core aunque solo lo pinte el editor: no depende de ImGui y
    # lo graba el pass principal del Renderer, asi que dejarlo en Editor haria
    # que Core dependiera del editor.
    src/Renderer/Gizmos.cpp
    # UI de juego (canvas 2D). Vive en Core, no en el editor: el runtime
    # exportado dibuja el mismo canvas.
    src/UI/UiCanvas.cpp
    src/UI/UiInputBridge.cpp
    src/UI/UiSpriteBatch.cpp
    src/UI/UiTextureAtlas.cpp
    src/UI/UiFont.cpp
    src/Scripting/LuaSyntaxCheck.cpp
    src/Scripting/LuaApiReference.cpp
)

target_include_directories(DonTopoCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${stb_SOURCE_DIR}
)

target_link_libraries(DonTopoCore
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
        assimp
        nlohmann_json::nlohmann_json
        sol2_lib
    # Solo UiFont.cpp los ve: UiFont.h no expone ni un tipo de FreeType ni de
    # msdfgen, así que nadie que use DonTopoCore hereda sus includes.
    PRIVATE
        freetype
        msdfgen::msdfgen-core
)

if(FMOD_FOUND)
    target_link_libraries(DonTopoCore PUBLIC FMOD::FMOD)
    target_compile_definitions(DonTopoCore PUBLIC DT_FMOD_ENABLED)
endif()

if(PHYSX_FOUND)
    target_link_libraries(DonTopoCore PUBLIC PhysX::SDK)
    target_compile_definitions(DonTopoCore PUBLIC DT_PHYSX_ENABLED)
endif()

# Backend DirectX 12. Las libs van PRIVATE porque ningún header público de
# DonTopoCore incluye d3d12.h — mismo criterio que freetype/msdfgen. El define
# sí es PUBLIC: el editor lo necesita para saber si puede ofrecer el backend.
if(DTE_ENABLE_D3D12)
    target_sources(DonTopoCore PRIVATE
        src/Renderer/D3D12/D3D12Support.cpp
        src/Renderer/D3D12/D3D12Renderer.cpp
    )
    target_link_libraries(DonTopoCore PRIVATE d3d12 dxgi dxguid D3D12MemoryAllocator)
    target_compile_definitions(DonTopoCore PUBLIC DT_D3D12_ENABLED)
endif()

target_compile_features(DonTopoCore PUBLIC cxx_std_20)

# ScriptBindings.cpp registra muchos usertypes de sol2 en una sola TU: el
# volumen de plantillas generadas supera el límite de secciones de MSVC
# (C1128) sin /bigobj.
if(MSVC)
    set_source_files_properties(src/Scripting/ScriptBindings.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

# El editor: paneles, undo, exportador y el backend de ImGui. Depende de Core,
# nunca al reves — el Renderer solo lo ve por la interfaz UiLayer.
add_library(DonTopoEditor STATIC
    src/Editor/EditorUI.cpp
    src/Editor/ProjectContext.cpp
    src/Editor/LoadingModal.cpp
    src/Editor/LogPanel.cpp
    src/Editor/ScenePanel.cpp
    src/Editor/ViewportPanel.cpp
    src/Editor/PropertiesPanel.cpp
    src/Editor/ContentBrowserPanel.cpp
    src/Editor/AnimatorPanel.cpp
    src/Editor/SpriteEditorPanel.cpp
    src/Editor/PerformancePanel.cpp
    src/Editor/InputActionsPanel.cpp
    src/Editor/Command.cpp
    src/Editor/UndoManager.cpp
    src/Editor/ScriptEditorPanel.cpp
    src/Editor/GameExporter.cpp
)

target_link_libraries(DonTopoEditor
    PUBLIC
        DonTopoCore
        imgui_backend
        imgui_filedialog
        imguizmo
        imgui_texteditor
        imgui_node_editor
)

target_compile_features(DonTopoEditor PUBLIC cxx_std_20)

add_subdirectory(tests)
