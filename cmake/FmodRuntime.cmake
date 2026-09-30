# Copies the FMOD library next to the target's binary: fmod.dll on Windows,
# only its soname (libfmod.so.N) on Linux. A single place for Sandbox, runtime and
# tests; it used to be a Windows-only block with a hand-written list of targets.
function(dt_copy_fmod_runtime target)
    if(NOT FMOD_FOUND)
        return()
    endif()
    get_filename_component(_dir "${FMOD_LIBRARY}" DIRECTORY)
    if(WIN32)
        set(_files "${_dir}/fmod.dll")
    else()
        # Only the one the binary asks for, its soname (libfmod.so.14). The development
        # .so and the full .so.14.14 are the same file repeated.
        # By name and not by reading the ELF: file(READ_ELF) has no SONAME option
        # (only RPATH/RUNPATH/BUILD_ID) and silently ignores it. Same rule as
        # isAudioLibFile in the exporter: prefix + a single number.
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
