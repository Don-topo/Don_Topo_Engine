# Parchea imgui_extra_math.inl dentro de las fuentes ya populadas de
# imgui-node-editor.
#
# El problema: imgui_extra_math.h define IMGUI_DEFINE_MATH_OPERATORS antes de
# incluir <imgui.h>. Con esa macro activa, nuestro imgui.h (1.92.9 WIP) ya
# implementa "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)".
# Los operadores vecinos en imgui_extra_math.inl están guardados con
# "#if IMGUI_VERSION_NUM < XXXXX" para no redefinirse si ImGui ya los trae,
# pero a este operator* en concreto le falta ese guard (visto tanto en
# master como en el tag v0.9.3 de imgui-node-editor: no hay versión mejor a
# la que fijarse). Resultado: doble definición, imgui_node_editor.lib no
# compila.
#
# La solución usa el propio centinela que imgui.h define junto al operador
# real (IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED, ver imgui.h línea ~3071):
# si está definido es porque ImGui ya implementó estos operadores, sin
# necesidad de comparar números de versión y autocorrectivo si ImGui cambia.
#
# No usamos FetchContent PATCH_COMMAND porque las fuentes ya están populadas
# desde un intento anterior y PATCH_COMMAND no se re-ejecuta sobre una
# población existente. Este script se invoca a mano tras
# FetchContent_Populate y es idempotente: se puede llamar tantas veces como
# se quiera (cada re-configure) sin duplicar el guard.

set(_dtNodeEditorMathInl "${imguinodeeditor_SOURCE_DIR}/imgui_extra_math.inl")

if(NOT EXISTS "${_dtNodeEditorMathInl}")
    message(FATAL_ERROR
        "PatchImGuiNodeEditor: no se encuentra ${_dtNodeEditorMathInl}. "
        "¿Cambió la estructura del repo de imgui-node-editor?"
    )
endif()

file(READ "${_dtNodeEditorMathInl}" _dtNodeEditorMathInlContents)

set(_dtGuardMarker "IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED")
set(_dtUnguardedOperator "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}")
set(_dtGuardedOperator "# ifndef IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n# endif")

string(FIND "${_dtNodeEditorMathInlContents}" "${_dtGuardMarker}" _dtGuardAlreadyPresent)

if(_dtGuardAlreadyPresent GREATER -1)
    # Ya parcheado en una configuración anterior: no-op.
    return()
endif()

string(FIND "${_dtNodeEditorMathInlContents}" "${_dtUnguardedOperator}" _dtUnguardedOperatorPos)

if(_dtUnguardedOperatorPos EQUAL -1)
    # No encontramos el texto exacto que esperábamos parchear. Puede que
    # upstream haya corregido el bug (ya no habría nada que parchear: no es
    # motivo para romper el build) o que haya reescrito el archivo de forma
    # incompatible con nuestro parche (sí sería motivo de alarma). No
    # podemos distinguir ambos casos automáticamente, así que avisamos sin
    # abortar la configuración: si el segundo caso es el real, la compilación
    # de imgui_node_editor fallará a continuación con un error claro de
    # símbolo duplicado, y ese error señalará directamente aquí.
    message(WARNING
        "PatchImGuiNodeEditor: no se encontró el operator* sin guard en "
        "${_dtNodeEditorMathInl}. Puede que upstream ya lo haya corregido "
        "(nada que hacer) o que el archivo cambió de forma inesperada "
        "(revisar este script). Si la compilación de imgui_node_editor "
        "falla por 'operator*' redefinido, este es el sitio a mirar."
    )
    return()
endif()

string(REPLACE "${_dtUnguardedOperator}" "${_dtGuardedOperator}" _dtNodeEditorMathInlContents "${_dtNodeEditorMathInlContents}")

file(WRITE "${_dtNodeEditorMathInl}" "${_dtNodeEditorMathInlContents}")

message(STATUS "PatchImGuiNodeEditor: operator*(float, ImVec2) guardado con IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED en ${_dtNodeEditorMathInl}")
