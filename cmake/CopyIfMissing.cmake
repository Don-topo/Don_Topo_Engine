# Copies SRC to DST only if DST does not exist yet. Used to seed the
# default imgui.ini without overwriting the layout the user has already adjusted
# on their machine (ImGui rewrites that file every time the app closes).
if(NOT EXISTS "${DST}")
    get_filename_component(DST_DIR "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST_DIR}")
    file(COPY "${SRC}" DESTINATION "${DST_DIR}")
endif()
