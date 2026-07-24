add_executable(DonTopoRuntime main.cpp)

target_link_libraries(DonTopoRuntime
    PRIVATE
        DonTopoEngine
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
