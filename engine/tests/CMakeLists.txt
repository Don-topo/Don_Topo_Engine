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
target_link_libraries(dt_audio_tests PRIVATE DonTopoCore)
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

# Los catorce ejecutables enlazan DonTopoCore, que arrastra FMOD (Audio). Sin
# copiar fmod.dll al lado del .exe, un clone limpio falla en runtime con
# STATUS_ENTRYPOINT_NOT_FOUND. Mismo patrÃ³n que sandbox/CMakeLists.txt.
if(FMOD_FOUND AND WIN32)
    get_filename_component(_fmod_lib_dir "${FMOD_LIBRARY}" DIRECTORY)
    set(_fmod_dll "${_fmod_lib_dir}/fmod.dll")
    if(EXISTS "${_fmod_dll}")
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests dt_exporter_tests dt_audio_tests dt_scripting_tests dt_splash_tests dt_jobsystem_tests dt_asset_loader_tests dt_scene_async_tests dt_frustum_tests dt_shared_gpu_mesh_tests dt_instancing_tests)
            add_custom_command(TARGET ${_dt_test_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_fmod_dll}"
                    "$<TARGET_FILE_DIR:${_dt_test_target}>/fmod.dll"
                COMMENT "Copying fmod.dll to output (${_dt_test_target})"
            )
        endforeach()
    endif()
endif()
