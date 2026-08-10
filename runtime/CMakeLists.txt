add_executable(DonTopoRuntime main.cpp)

# Solo Core: el runtime no enlaza el editor ni, por tanto, ImGui.
target_link_libraries(DonTopoRuntime
    PRIVATE
        DonTopoCore
)

target_compile_features(DonTopoRuntime PRIVATE cxx_std_20)

# El juego exportado no debe abrir una ventana de consola detras de la del
# juego, asi que se enlaza contra el subsistema WINDOWS. /ENTRY:mainCRTStartup
# mantiene el main() estandar de main.cpp (sin el, el linker exigiria WinMain).
# Contrapartida: sin consola, stdout/stderr no tienen a donde escribir — main()
# los redirige a game.log y saca los errores fatales por MessageBox.
# El editor (Sandbox) se queda en consola a proposito: ahi la terminal es util.
if(WIN32)
    set_target_properties(DonTopoRuntime PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(DonTopoRuntime PRIVATE /ENTRY:mainCRTStartup)
endif()

# El exportador del editor copia este binario al paquete, y lo busca en su
# propio directorio de trabajo (el mismo donde vive Sandbox.exe). Por eso este
# add_subdirectory va DESPUÉS del de sandbox en el CMakeLists raíz: el
# generator expression necesita el target Sandbox ya definido.
add_custom_command(TARGET DonTopoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DonTopoRuntime>
        $<TARGET_FILE_DIR:Sandbox>/DonTopoRuntime.exe
    COMMENT "Copying DonTopoRuntime.exe next to the editor"
)

# Shaders y assets junto al binario del runtime, para poder EJECUTARLO desde su
# propio directorio de build (probar una escena sin pasar por Export Game). El
# runtime los busca relativos a su directorio de trabajo, y sin ellos ni
# arranca: muere en el primer pipeline que no encuentra su .spv.
#
# El juego exportado no depende de esto — GameExporter copia shaders y assets al
# paquete por su cuenta — pero el binario recien compilado sí, y hasta ahora no
# los tenia (el CMakeLists de sandbox sí los copia; este no).
#
# El target Shaders lo define sandbox/CMakeLists.txt y compila a
# ${CMAKE_BINARY_DIR}/spv; el add_subdirectory de sandbox va antes que el de
# runtime en el CMakeLists raiz, asi que aqui ya existe. copy_directory (y no
# copy_if_different) para que un .spv recompilado llegue siempre.
add_dependencies(DonTopoRuntime Shaders)

add_custom_command(TARGET DonTopoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/spv
        $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:DonTopoRuntime>/assets
    COMMENT "Copying shaders and assets next to DonTopoRuntime"
)

# Mismos shaders, otra representación: el runtime arranca con el backend que
# diga su configuración, así que necesita los DXIL al lado de los SPIR-V.
if(DTE_ENABLE_D3D12)
    add_custom_command(TARGET DonTopoRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/dxil
            $<TARGET_FILE_DIR:DonTopoRuntime>/shaders
        COMMENT "Copying DXIL shaders next to DonTopoRuntime"
    )
endif()
