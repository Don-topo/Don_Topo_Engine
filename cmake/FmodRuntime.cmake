# Copia la biblioteca de FMOD junto al binario del target: fmod.dll en Windows,
# libfmod.so* (con su soname) en Linux. Un solo sitio para Sandbox, runtime y
# tests; antes era un bloque solo-Windows con una lista de targets a mano.
function(dt_copy_fmod_runtime target)
    if(NOT FMOD_FOUND)
        return()
    endif()
    get_filename_component(_dir "${FMOD_LIBRARY}" DIRECTORY)
    if(WIN32)
        set(_files "${_dir}/fmod.dll")
    else()
        file(GLOB _files "${_dir}/libfmod.so*")
    endif()
    foreach(_f IN LISTS _files)
        if(EXISTS "${_f}")
            get_filename_component(_n "${_f}" NAME)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:${target}>/${_n}"
                COMMENT "Copying ${_n} next to ${target}")
        endif()
    endforeach()
endfunction()
