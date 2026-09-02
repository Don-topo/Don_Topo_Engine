add_executable(dt_physics_tests physics_tests.cpp)
# DonTopoCore expone PhysX y DT_PHYSX_ENABLED como PUBLIC: el test los hereda.
target_link_libraries(dt_physics_tests PRIVATE DonTopoCore)
target_compile_features(dt_physics_tests PRIVATE cxx_std_20)

add_executable(dt_content_browser_tests content_browser_tests.cpp)
target_link_libraries(dt_content_browser_tests PRIVATE DonTopoEditor)
target_compile_features(dt_content_browser_tests PRIVATE cxx_std_20)

add_executable(dt_camera_tests camera_tests.cpp)
target_link_libraries(dt_camera_tests PRIVATE DonTopoEditor)
target_compile_features(dt_camera_tests PRIVATE cxx_std_20)

add_executable(dt_animator_tests animator_tests.cpp)
target_link_libraries(dt_animator_tests PRIVATE DonTopoEditor)
target_compile_features(dt_animator_tests PRIVATE cxx_std_20)

add_executable(dt_exporter_tests exporter_tests.cpp)
target_link_libraries(dt_exporter_tests PRIVATE DonTopoEditor)
target_compile_features(dt_exporter_tests PRIVATE cxx_std_20)

add_executable(dt_audio_tests audio_tests.cpp)
# DonTopoEditor y no DonTopoCore: los tests cubren tambien los comandos de undo
# del panel Properties (AudioClipComponentCommand, AudioListenerComponentCommand),
# que viven en la libreria del editor. Mismo enlace que dt_camera_tests.
target_link_libraries(dt_audio_tests PRIVATE DonTopoEditor)
target_compile_features(dt_audio_tests PRIVATE cxx_std_20)

add_executable(dt_scripting_tests scripting_tests.cpp)
target_link_libraries(dt_scripting_tests PRIVATE DonTopoEditor)
target_compile_features(dt_scripting_tests PRIVATE cxx_std_20)

add_executable(dt_splash_tests splash_tests.cpp)
target_link_libraries(dt_splash_tests PRIVATE DonTopoCore)
target_compile_features(dt_splash_tests PRIVATE cxx_std_20)
target_include_directories(dt_splash_tests PRIVATE ${CMAKE_SOURCE_DIR}/runtime)

add_executable(dt_jobsystem_tests jobsystem_tests.cpp)
target_link_libraries(dt_jobsystem_tests PRIVATE DonTopoCore)
target_compile_features(dt_jobsystem_tests PRIVATE cxx_std_20)

add_executable(dt_asset_loader_tests asset_loader_tests.cpp)
target_link_libraries(dt_asset_loader_tests PRIVATE DonTopoCore)
target_compile_features(dt_asset_loader_tests PRIVATE cxx_std_20)

add_executable(dt_scene_async_tests scene_async_tests.cpp)
target_link_libraries(dt_scene_async_tests PRIVATE DonTopoCore)
target_compile_features(dt_scene_async_tests PRIVATE cxx_std_20)

add_executable(dt_frustum_tests frustum_tests.cpp)
target_link_libraries(dt_frustum_tests PRIVATE DonTopoCore)
target_compile_features(dt_frustum_tests PRIVATE cxx_std_20)

add_executable(dt_shared_gpu_mesh_tests shared_gpu_mesh_tests.cpp)
target_link_libraries(dt_shared_gpu_mesh_tests PRIVATE DonTopoCore)
target_compile_features(dt_shared_gpu_mesh_tests PRIVATE cxx_std_20)

add_executable(dt_instancing_tests instancing_tests.cpp)
target_link_libraries(dt_instancing_tests PRIVATE DonTopoCore)
target_compile_features(dt_instancing_tests PRIVATE cxx_std_20)

add_executable(dt_ui_batch_tests ui_batch_tests.cpp)
target_link_libraries(dt_ui_batch_tests PRIVATE DonTopoCore)
target_compile_features(dt_ui_batch_tests PRIVATE cxx_std_20)

add_executable(dt_input_actions_tests input_actions_tests.cpp)
target_link_libraries(dt_input_actions_tests PRIVATE DonTopoEditor)
target_compile_features(dt_input_actions_tests PRIVATE cxx_std_20)

# DonTopoEditor por lo mismo que dt_camera_tests: UndoManager y los comandos de
# undo viven en la libreria del editor. RendererState llega por DonTopoCore, que
# DonTopoEditor arrastra.
add_executable(dt_render_settings_undo_tests render_settings_undo_tests.cpp)
target_link_libraries(dt_render_settings_undo_tests PRIVATE DonTopoEditor)
target_compile_features(dt_render_settings_undo_tests PRIVATE cxx_std_20)

# Sin enlazar nada: SlotPool es header-only y no arrastra ni GPU ni FMOD, asi
# que este test solo necesita el include del motor.
# Enlaza DonTopoCore por la implementacion de readSpvFile; el header pide
# vulkan.h, que Core ya expone.
add_executable(dt_shader_module_tests shader_module_tests.cpp)
target_link_libraries(dt_shader_module_tests PRIVATE DonTopoCore)
target_compile_features(dt_shader_module_tests PRIVATE cxx_std_20)

# Solo el include del motor: el reparto de ranuras es una plantilla pura en
# UniformBufferObject.h y no arrastra ni GPU ni FMOD.
add_executable(dt_shadow_slots_tests shadow_slots_tests.cpp)
target_include_directories(dt_shadow_slots_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_shadow_slots_tests PRIVATE glm::glm)
target_compile_features(dt_shadow_slots_tests PRIVATE cxx_std_20)

add_executable(dt_slot_pool_tests slot_pool_tests.cpp)
target_include_directories(dt_slot_pool_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(dt_slot_pool_tests PRIVATE cxx_std_20)

# Solo el include del motor y el de Vulkan: PipelineDefaults.h es header-only y
# no llama a una sola funcion de la API, solo rellena structs.
add_executable(dt_pipeline_defaults_tests pipeline_defaults_tests.cpp)
target_include_directories(dt_pipeline_defaults_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_pipeline_defaults_tests PRIVATE Vulkan::Vulkan)
target_compile_features(dt_pipeline_defaults_tests PRIVATE cxx_std_20)

# VisibleSet.h es header-only, pero pide la caché de mallas (SharedGpuMesh.cpp)
# y vulkan.h: las dos cosas llegan por DonTopoCore.
add_executable(dt_visible_set_tests visible_set_tests.cpp)
target_link_libraries(dt_visible_set_tests PRIVATE DonTopoCore)
target_compile_features(dt_visible_set_tests PRIVATE cxx_std_20)

# Los diecisiete ejecutables enlazan DonTopoCore, que arrastra FMOD (Audio). Sin
# copiar fmod.dll al lado del .exe, un clone limpio falla en runtime con
# STATUS_ENTRYPOINT_NOT_FOUND. Mismo patrÃ³n que sandbox/CMakeLists.txt.
if(FMOD_FOUND AND WIN32)
    get_filename_component(_fmod_lib_dir "${FMOD_LIBRARY}" DIRECTORY)
    set(_fmod_dll "${_fmod_lib_dir}/fmod.dll")
    if(EXISTS "${_fmod_dll}")
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests dt_exporter_tests dt_audio_tests dt_scripting_tests dt_splash_tests dt_jobsystem_tests dt_asset_loader_tests dt_scene_async_tests dt_frustum_tests dt_shared_gpu_mesh_tests dt_instancing_tests dt_ui_batch_tests dt_input_actions_tests dt_render_settings_undo_tests dt_visible_set_tests)
            add_custom_command(TARGET ${_dt_test_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_fmod_dll}"
                    "$<TARGET_FILE_DIR:${_dt_test_target}>/fmod.dll"
                COMMENT "Copying fmod.dll to output (${_dt_test_target})"
            )
        endforeach()
    endif()
endif()
