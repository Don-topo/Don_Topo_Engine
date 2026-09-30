# Every test goes through here: that way the FMOD copy (and whatever comes next) does
# not depend on remembering to add the target to a separate list.
function(dt_add_test name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib})
    target_compile_features(${name} PRIVATE cxx_std_20)
    dt_copy_fmod_runtime(${name})
endfunction()

# DonTopoCore exposes PhysX and DT_PHYSX_ENABLED as PUBLIC: the test inherits them.
dt_add_test(dt_physics_tests physics_tests.cpp DonTopoCore)

dt_add_test(dt_content_browser_tests content_browser_tests.cpp DonTopoEditor)

dt_add_test(dt_asset_import_tests asset_import_tests.cpp DonTopoEditor)

dt_add_test(dt_thumbnail_tests thumbnail_tests.cpp DonTopoEditor)

dt_add_test(dt_import_settings_tests import_settings_tests.cpp DonTopoCore)

dt_add_test(dt_material_asset_tests material_asset_tests.cpp DonTopoCore)

dt_add_test(dt_texture_import_tests texture_import_tests.cpp DonTopoCore)

dt_add_test(dt_model_import_tests model_import_tests.cpp DonTopoCore)

dt_add_test(dt_camera_tests camera_tests.cpp DonTopoEditor)

# DonTopoEditor for the same reason as dt_content_browser_tests: LogPanel lives in the
# editor library. push() does not touch ImGui (only draw() does), so the test
# runs without a window or a context.
dt_add_test(dt_log_panel_tests log_panel_tests.cpp DonTopoEditor)

dt_add_test(dt_animator_tests animator_tests.cpp DonTopoEditor)

dt_add_test(dt_exporter_tests exporter_tests.cpp DonTopoEditor)

# DonTopoEditor and not DonTopoCore: the tests also cover the Properties panel's undo
# commands (AudioClipComponentCommand, AudioListenerComponentCommand),
# which live in the editor library. Same linking as dt_camera_tests.
dt_add_test(dt_audio_tests audio_tests.cpp DonTopoEditor)

dt_add_test(dt_scripting_tests scripting_tests.cpp DonTopoEditor)

dt_add_test(dt_splash_tests splash_tests.cpp DonTopoCore)
target_include_directories(dt_splash_tests PRIVATE ${CMAKE_SOURCE_DIR}/runtime)

dt_add_test(dt_jobsystem_tests jobsystem_tests.cpp DonTopoCore)

dt_add_test(dt_asset_loader_tests asset_loader_tests.cpp DonTopoCore)

dt_add_test(dt_scene_async_tests scene_async_tests.cpp DonTopoCore)

dt_add_test(dt_frustum_tests frustum_tests.cpp DonTopoCore)

dt_add_test(dt_shared_gpu_mesh_tests shared_gpu_mesh_tests.cpp DonTopoCore)
dt_add_test(dt_shared_texture_cache_tests shared_texture_cache_tests.cpp DonTopoCore)

dt_add_test(dt_instancing_tests instancing_tests.cpp DonTopoCore)

dt_add_test(dt_ui_batch_tests ui_batch_tests.cpp DonTopoCore)

dt_add_test(dt_input_actions_tests input_actions_tests.cpp DonTopoEditor)

# DonTopoEditor for the same reason as dt_camera_tests: UndoManager and the undo
# commands live in the editor library. RendererState comes through DonTopoCore, which
# DonTopoEditor pulls in.
dt_add_test(dt_render_settings_undo_tests render_settings_undo_tests.cpp DonTopoEditor)

# No linking at all: SlotPool is header-only and pulls in neither GPU nor FMOD, so
# this test only needs the engine's include.
# It links DonTopoCore for the implementation of readSpvFile; the header needs
# vulkan.h, which Core already exposes.
dt_add_test(dt_shader_module_tests shader_module_tests.cpp DonTopoCore)

# Only the engine's include: the slot allocation is a pure template in
# UniformBufferObject.h and pulls in neither GPU nor FMOD.
add_executable(dt_shadow_slots_tests shadow_slots_tests.cpp)
target_include_directories(dt_shadow_slots_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_shadow_slots_tests PRIVATE glm::glm)
target_compile_features(dt_shadow_slots_tests PRIVATE cxx_std_20)

add_executable(dt_slot_pool_tests slot_pool_tests.cpp)
target_include_directories(dt_slot_pool_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(dt_slot_pool_tests PRIVATE cxx_std_20)

# Only the engine's include and glm: TransformDecompose.h is header-only and
# pulls in neither GPU nor PhysX.
add_executable(dt_transform_decompose_tests transform_decompose_tests.cpp)
target_include_directories(dt_transform_decompose_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_transform_decompose_tests PRIVATE glm::glm)
target_compile_features(dt_transform_decompose_tests PRIVATE cxx_std_20)

# Only the engine's include: GpuTimeFormat.h is header-only and pulls in neither
# ImGui nor GPU; it is pure formatting.
add_executable(dt_gpu_time_format_tests gpu_time_format_tests.cpp)
target_include_directories(dt_gpu_time_format_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(dt_gpu_time_format_tests PRIVATE cxx_std_20)

# Gizmo accumulation does not touch the graphics API, but draw() and the rest live
# in Gizmos.cpp, which comes through DonTopoCore.
dt_add_test(dt_gizmos_tests gizmos_tests.cpp DonTopoCore)

# Only the engine's include and Vulkan's: PipelineDefaults.h is header-only and
# does not call a single API function, it only fills in structs.
add_executable(dt_pipeline_defaults_tests pipeline_defaults_tests.cpp)
target_include_directories(dt_pipeline_defaults_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(dt_pipeline_defaults_tests PRIVATE Vulkan::Vulkan)
target_compile_features(dt_pipeline_defaults_tests PRIVATE cxx_std_20)

# VisibleSet.h is header-only, but it needs the mesh cache (SharedGpuMesh.cpp)
# and vulkan.h: both come through DonTopoCore.
dt_add_test(dt_visible_set_tests visible_set_tests.cpp DonTopoCore)

# DonTopoEditor and not DonTopoCore: in Task 8 this same binary covers
# MaterialTextureCommand, which lives in the editor library. Same linking as
# dt_audio_tests, and for the same reason.
dt_add_test(dt_material_texture_tests material_texture_tests.cpp DonTopoEditor)

# Reads the code on disk. It links DonTopoCore because it also tests
# DonTopo::platform (the platform layer).
dt_add_test(dt_portability_tests portability_tests.cpp DonTopoCore)
