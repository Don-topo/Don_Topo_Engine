# Copia la biblioteca de FMOD junto al binario del target: fmod.dll en Windows,
# solo su soname (libfmod.so.N) en Linux. Un solo sitio para Sandbox, runtime y
# tests; antes era un bloque solo-Windows con una lista de targets a mano.
function(dt_copy_fmod_runtime target)
    if(NOT FMOD_FOUND)
        return()
    endif()
    get_filename_component(_dir "${FMOD_LIBRARY}" DIRECTORY)
    if(WIN32)
        set(_files "${_dir}/fmod.dll")
    else()
        # Solo la que pide el binario, su soname (libfmod.so.14). La .so de
        # desarrollo y la .so.14.14 completa son el mismo fichero repetido.
        # Por nombre y no leyendo el ELF: file(READ_ELF) no tiene opcion SONAME
        # (solo RPATH/RUNPATH/BUILD_ID) y la ignora en silencio. Misma regla que
        # isAudioLibFile en el exportador: prefijo + un solo numero.
        file(GLOB _files "${_dir}/libfmod.so.*")
        list(FILTER _files INCLUDE REGEX "/libfmod[.]so[.][0-9]+$")
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
