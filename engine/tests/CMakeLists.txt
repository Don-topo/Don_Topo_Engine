# Todo test pasa por aqui: asi la copia de FMOD (y lo que venga) no depende de
# acordarse de apuntar el target en una lista aparte.
function(dt_add_test name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib})
    target_compile_features(${name} PRIVATE cxx_std_20)
    dt_copy_fmod_runtime(${name})
endfunction()

# DonTopoCore expone PhysX y DT_PHYSX_ENABLED como PUBLIC: el test los hereda.
dt_add_test(dt_physics_tests physics_tests.cpp DonTopoCore)

dt_add_test(dt_content_browser_tests content_browser_tests.cpp DonTopoEditor)

dt_add_test(dt_camera_tests camera_tests.cpp DonTopoEditor)

# DonTopoEditor por lo mismo que dt_content_browser_tests: LogPanel vive en la
# libreria del editor. push() no toca ImGui (solo draw()), asi que el test
# corre sin ventana ni contexto.
dt_add_test(dt_log_panel_tests log_panel_tests.cpp DonTopoEditor)

dt_add_test(dt_animator_tests animator_tests.cpp DonTopoEditor)

dt_add_test(dt_exporter_tests exporter_tests.cpp DonTopoEditor)

# DonTopoEditor y no DonTopoCore: los tests cubren tambien los comandos de undo
# del panel Properties (AudioClipComponentCommand, AudioListenerComponentCommand),
# que viven en la libreria del editor. Mismo enlace que dt_camera_tests.
dt_add_test(dt_audio_tests audio_tests.cpp DonTopoEditor)

dt_add_test(dt_scripting_tests scripting_tests.cpp DonTopoEditor)

dt_add_test(dt_splash_tests splash_tests.cpp DonTopoCore)
target_include_directories(dt_splash_tests PRIVATE ${CMAKE_SOURCE_DIR}/runtime)

dt_add_test(dt_jobsystem_tests jobsystem_tests.cpp DonTopoCore)

dt_add_test(dt_asset_loader_tests asset_loader_tests.cpp DonTopoCore)

dt_add_test(dt_scene_async_tests scene_async_tests.cpp DonTopoCore)

dt_add_test(dt_frustum_tests frustum_tests.cpp DonTopoCore)

dt_add_test(dt_shared_gpu_mesh_tests shared_gpu_mesh_tests.cpp DonTopoCore)

dt_add_test(dt_instancing_tests instancing_tests.cpp DonTopoCore)

dt_add_test(dt_ui_batch_tests ui_batch_tests.cpp DonTopoCore)

dt_add_test(dt_input_actions_tests input_actions_tests.cpp DonTopoEditor)

# DonTopoEditor por lo mismo que dt_camera_tests: UndoManager y los comandos de
# undo viven en la libreria del editor. RendererState llega por DonTopoCore, que
# DonTopoEditor arrastra.
dt_add_test(dt_render_settings_undo_tests render_settings_undo_tests.cpp DonTopoEditor)

# Sin enlazar nada: SlotPool es header-only y no arrastra ni GPU ni FMOD, asi
# que este test solo necesita el include del motor.
# Enlaza DonTopoCore por la implementacion de readSpvFile; el header pide
# vulkan.h, que Core ya expone.
dt_add_test(dt_shader_module_tests shader_module_tests.cpp DonTopoCore)

# Solo el include del motor: el reparto de ranuras es una plantilla pura en
# UniformBufferObject.h y no arrastra ni GPU ni FMOD.
add_executable(dt_shadow_slots_tests shadow_slots_tests.cpp)
target_include_directories(dt_shadow_slots_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_shadow_slots_tests PRIVATE glm::glm)
target_compile_features(dt_shadow_slots_tests PRIVATE cxx_std_20)

add_executable(dt_slot_pool_tests slot_pool_tests.cpp)
target_include_directories(dt_slot_pool_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(dt_slot_pool_tests PRIVATE cxx_std_20)

# Solo el include del motor y glm: TransformDecompose.h es header-only y no
# arrastra ni GPU ni PhysX.
add_executable(dt_transform_decompose_tests transform_decompose_tests.cpp)
target_include_directories(dt_transform_decompose_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_transform_decompose_tests PRIVATE glm::glm)
target_compile_features(dt_transform_decompose_tests PRIVATE cxx_std_20)

# Solo el include del motor: GpuTimeFormat.h es header-only y no arrastra ni
# ImGui ni GPU — es formato puro.
add_executable(dt_gpu_time_format_tests gpu_time_format_tests.cpp)
target_include_directories(dt_gpu_time_format_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(dt_gpu_time_format_tests PRIVATE cxx_std_20)

# La acumulacion de gizmos no toca la API grafica, pero draw() y el resto viven
# en Gizmos.cpp, que llega por DonTopoCore.
dt_add_test(dt_gizmos_tests gizmos_tests.cpp DonTopoCore)

# Solo el include del motor y el de Vulkan: PipelineDefaults.h es header-only y
# no llama a una sola funcion de la API, solo rellena structs.
add_executable(dt_pipeline_defaults_tests pipeline_defaults_tests.cpp)
target_include_directories(dt_pipeline_defaults_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_pipeline_defaults_tests PRIVATE Vulkan::Vulkan)
target_compile_features(dt_pipeline_defaults_tests PRIVATE cxx_std_20)

# VisibleSet.h es header-only, pero pide la caché de mallas (SharedGpuMesh.cpp)
# y vulkan.h: las dos cosas llegan por DonTopoCore.
dt_add_test(dt_visible_set_tests visible_set_tests.cpp DonTopoCore)

# DonTopoEditor y no DonTopoCore: en la Task 8 este mismo binario cubre
# MaterialTextureCommand, que vive en la libreria del editor. Mismo enlace que
# dt_audio_tests, y por el mismo motivo.
dt_add_test(dt_material_texture_tests material_texture_tests.cpp DonTopoEditor)

# Lee el codigo en disco. Enlaza DonTopoCore porque tambien prueba
# DonTopo::platform (la capa de plataforma).
dt_add_test(dt_portability_tests portability_tests.cpp DonTopoCore)
