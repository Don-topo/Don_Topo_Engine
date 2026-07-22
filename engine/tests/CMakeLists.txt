add_executable(dt_physics_tests physics_tests.cpp)
# DonTopoEngine expone PhysX y DT_PHYSX_ENABLED como PUBLIC: el test los hereda.
target_link_libraries(dt_physics_tests PRIVATE DonTopoEngine)
target_compile_features(dt_physics_tests PRIVATE cxx_std_20)

add_executable(dt_content_browser_tests content_browser_tests.cpp)
target_link_libraries(dt_content_browser_tests PRIVATE DonTopoEngine)
target_compile_features(dt_content_browser_tests PRIVATE cxx_std_20)

add_executable(dt_camera_tests camera_tests.cpp)
target_link_libraries(dt_camera_tests PRIVATE DonTopoEngine)
target_compile_features(dt_camera_tests PRIVATE cxx_std_20)

add_executable(dt_animator_tests animator_tests.cpp)
target_link_libraries(dt_animator_tests PRIVATE DonTopoEngine)
target_compile_features(dt_animator_tests PRIVATE cxx_std_20)

add_executable(dt_exporter_tests exporter_tests.cpp)
target_link_libraries(dt_exporter_tests PRIVATE DonTopoEngine)
target_compile_features(dt_exporter_tests PRIVATE cxx_std_20)

add_executable(dt_audio_tests audio_tests.cpp)
target_link_libraries(dt_audio_tests PRIVATE DonTopoEngine)
target_compile_features(dt_audio_tests PRIVATE cxx_std_20)

# Ambos ejecutables enlazan DonTopoEngine, que arrastra FMOD (Audio). Sin
# copiar fmod.dll al lado del .exe, un clone limpio falla en runtime con
# STATUS_ENTRYPOINT_NOT_FOUND. Mismo patrón que sandbox/CMakeLists.txt.
if(FMOD_FOUND AND WIN32)
    get_filename_component(_fmod_lib_dir "${FMOD_LIBRARY}" DIRECTORY)
    set(_fmod_dll "${_fmod_lib_dir}/fmod.dll")
    if(EXISTS "${_fmod_dll}")
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests dt_exporter_tests dt_audio_tests)
            add_custom_command(TARGET ${_dt_test_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_fmod_dll}"
                    "$<TARGET_FILE_DIR:${_dt_test_target}>/fmod.dll"
                COMMENT "Copying fmod.dll to output (${_dt_test_target})"
            )
        endforeach()
    endif()
endif()
