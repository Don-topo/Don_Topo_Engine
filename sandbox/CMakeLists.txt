add_executable(Sandbox
    src/main.cpp
    Sandbox.rc
)

# DonTopoEditor arrastra DonTopoCore como PUBLIC.
target_link_libraries(Sandbox
    PRIVATE
        DonTopoEditor
)

# Compilar shaders GLSL -> SPIR-V con glslc
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin)

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR $<TARGET_FILE_DIR:Sandbox>/shaders)

file(GLOB GLSL_SOURCES
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
    "${SHADER_SRC_DIR}/*.comp"
)

# Trozos de GLSL que se incluyen desde otros shaders. NO entran en el GLOB de
# arriba —un .glsl no es una etapa y glslc no sabría con qué perfil compilarlo—,
# pero sí tienen que estar en los DEPENDS: sin eso, editar uno de estos deja los
# .spv que lo incluyen sin regenerar, y el sintoma es un shader que se comporta
# como la version anterior sin que nada falle.
#
# Existen porque la alternativa es copiar la misma funcion en varios shaders, y
# eso ya salio mal: pbr.frag y fog.comp muestrean el MISMO shadow map, cada uno
# tenia su copia del criterio, y cambiar uno y no el otro dejo el in-scattering
# apuntando a un lado y el shadow map construido hacia otro (H65).
file(GLOB GLSL_INCLUDES "${SHADER_SRC_DIR}/*.glsl")

foreach(SHADER ${GLSL_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUT "${CMAKE_BINARY_DIR}/spv/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT  ${SPV_OUT}
        COMMAND ${GLSLC} -I ${SHADER_SRC_DIR} ${SHADER} -o ${SPV_OUT}
        DEPENDS ${SHADER} ${GLSL_INCLUDES}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_FILES ${SPV_OUT})

    # Rama DX12: SPIR-V -> HLSL (spirv-cross) -> DXIL (dxc). Cuelga del .spv, no
    # del .glsl, para no compilar el GLSL dos veces y para que el HLSL sea
    # siempre traducción de EXACTAMENTE el mismo SPIR-V que usa Vulkan.
    #
    # --shader-model 60 es el mínimo que mapea los descriptor sets de Vulkan a
    # register spaces de HLSL (layout(set=N) -> space N); por debajo de 5.1 no
    # hay spaces y la traducción de pbr.frag, con 8 sets, colisionaría.
    if(DTE_ENABLE_D3D12)
        get_filename_component(SHADER_EXT ${SHADER} EXT)
        if(SHADER_EXT STREQUAL ".vert")
            set(DXC_PROFILE vs_6_0)
        elseif(SHADER_EXT STREQUAL ".frag")
            set(DXC_PROFILE ps_6_0)
        elseif(SHADER_EXT STREQUAL ".comp")
            set(DXC_PROFILE cs_6_0)
        else()
            message(FATAL_ERROR "Extensión de shader sin perfil DXC: ${SHADER}")
        endif()

        set(HLSL_OUT "${CMAKE_BINARY_DIR}/hlsl/${SHADER_NAME}.hlsl")
        set(DXIL_OUT "${CMAKE_BINARY_DIR}/dxil/${SHADER_NAME}.dxil")
        add_custom_command(
            OUTPUT  ${DXIL_OUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/hlsl
            COMMAND ${SPIRV_CROSS} --hlsl --shader-model 60 ${SPV_OUT} --output ${HLSL_OUT}
            COMMAND ${DXC} -T ${DXC_PROFILE} -E main ${HLSL_OUT} -Fo ${DXIL_OUT}
            DEPENDS ${SPV_OUT}
            BYPRODUCTS ${HLSL_OUT}
            COMMENT "Translating shader ${SHADER_NAME} to DXIL (${DXC_PROFILE})"
        )
        list(APPEND DXIL_FILES ${DXIL_OUT})
    endif()
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_FILES} ${DXIL_FILES})
add_dependencies(Sandbox Shaders)

# Copiar assets al lado del ejecutable
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:Sandbox>/assets
    COMMENT "Copying assets to output directory"
)

# Copiar los .spv compilados al lado del ejecutable y a la raíz del proyecto
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        ${SHADER_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        ${CMAKE_SOURCE_DIR}/shaders
    COMMENT "Copying compiled shaders to output directory and project shaders/"
)

# Los .dxil van a los mismos dos sitios que los .spv, para que el backend DX12
# encuentre sus shaders con las mismas rutas relativas al directorio de trabajo.
if(DTE_ENABLE_D3D12)
    add_custom_command(TARGET Sandbox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            ${SHADER_OUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            ${CMAKE_SOURCE_DIR}/shaders
        COMMENT "Copying compiled DXIL shaders to output directory and project shaders/"
    )
endif()

# Sembrar imgui.ini por defecto (layout del editor) solo si el ejecutable
# todavía no tiene uno propio — así un clone+build nuevo arranca con un
# layout razonable, pero no se pisa el que el usuario ya haya personalizado.
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/imgui.ini
        -DDST=$<TARGET_FILE_DIR:Sandbox>/imgui.ini
        -P ${CMAKE_SOURCE_DIR}/cmake/CopyIfMissing.cmake
    COMMENT "Seeding default imgui.ini layout (if missing)"
)

# CRT de MSVC junto al ejecutable del editor, para que GameExporter pueda
# meterlo app-local en el juego exportado (el paquete es una carpeta que se
# copia: en la maquina del jugador no hay instalador que registre el VC++
# Redistributable). Mismo patron que fmod.dll: primero al lado del .exe, y el
# exportador lo recoge de ahi.
#
# Solo en Release: el CRT de depuracion no es redistribuible, asi que en Debug
# no se copia nada y GameExporter avisa en su lugar. Se comprueba
# CMAKE_BUILD_TYPE porque los presets del repo usan Ninja (single-config).
#
# _SKIP evita que esto añada reglas de install(); aqui solo interesa la lista de
# DLL que resuelve InstallRequiredSystemLibraries — inventar rutas a
# VC/Redist/... a mano se rompe con cada actualizacion de la toolchain.
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET Sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                $<TARGET_FILE_DIR:Sandbox>
            COMMENT "Copying MSVC runtime DLLs to output (app-local CRT for exported games)"
        )
    else()
        message(WARNING
            "InstallRequiredSystemLibraries no encontro el CRT de MSVC: los juegos "
            "exportados desde este build solo arrancaran donde ya este instalado el "
            "Visual C++ Redistributable.")
    endif()
endif()

# Copy FMOD runtime DLL next to the executable (Windows only)
if(FMOD_FOUND AND WIN32)
    get_filename_component(_fmod_lib_dir "${FMOD_LIBRARY}" DIRECTORY)
    set(_fmod_dll "${_fmod_lib_dir}/fmod.dll")
    if(EXISTS "${_fmod_dll}")
        add_custom_command(TARGET Sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_fmod_dll}"
                "$<TARGET_FILE_DIR:Sandbox>/fmod.dll"
            COMMENT "Copying fmod.dll to output"
        )
    endif()
endif()
