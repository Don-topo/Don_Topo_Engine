# Copia SRC a DST solo si DST todavía no existe. Usado para sembrar el
# imgui.ini por defecto sin pisar el layout que el usuario ya haya ajustado
# en su máquina (ImGui reescribe ese archivo en cada cierre de la app).
if(NOT EXISTS "${DST}")
    get_filename_component(DST_DIR "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST_DIR}")
    file(COPY "${SRC}" DESTINATION "${DST_DIR}")
endif()
