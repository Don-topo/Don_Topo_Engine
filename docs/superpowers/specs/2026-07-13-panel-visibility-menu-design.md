# Panel visibility + View menu

## Problema

Los paneles ImGui del editor (Scene, Viewport, Properties, Log, Content Browser, Script Editor) se dibujan siempre, sin `p_open`. No hay forma de cerrarlos ni de reabrirlos: ningún panel tiene botón de cerrar en su titlebar, y no existe menú alguno en el editor (no hay `MenuBar`, `File`, `Edit` ni `View`).

## Objetivo

Permitir cerrar cualquier panel persistente desde su "x" de titlebar, y reabrirlo desde un menú `View` nuevo con checkboxes.

## Alcance

Paneles afectados (todos los persistentes de `EditorUI`):
- Scene (Hierarchy)
- Viewport
- Properties
- Log
- Content Browser
- Script Editor (`ScriptEditorPanel`)

Fuera de alcance: diálogos/popups modales (Mesh, AudioClip, Scene, New Script, rename/delete de assets) — son overlays puntuales, no paneles persistentes, no llevan entrada en `View`.

## Arquitectura

Sin clase `Panel` ni registro genérico nuevo — se sigue el patrón actual de `EditorUI` (miembros sueltos + un método `drawX()` por panel). Justificación: solo 6 paneles, el código existente no tiene abstracción de panel, introducir una crearía indirección sin beneficio real (YAGNI).

### 1. Flags de visibilidad

En `EditorUI.h`, nuevos miembros:
```cpp
bool m_sceneOpen = true;
bool m_viewportOpen = true;
bool m_propertiesOpen = true;
bool m_logOpen = true;
bool m_contentBrowserOpen = true;
```

Cada método `drawX()` correspondiente en `EditorUI.cpp` se envuelve así:
```cpp
void EditorUI::drawX()
{
    if (!m_xxxOpen) return;
    if (ImGui::Begin("Panel", &m_xxxOpen)) {
        // ...contenido actual sin cambios...
    }
    ImGui::End();
}
```
`End()` se llama siempre que `Begin()` se llamó (patrón estándar ImGui), incluso si `Begin` devuelve `false` por colapso.

### 2. ScriptEditorPanel

`ScriptEditorPanel` es una clase separada (no un método de `EditorUI`) y hoy solo gestiona visibilidad a nivel de tabs internas, no de ventana. Se añade:
- `bool m_open = true;` (privado, en `ScriptEditorPanel.h`)
- `bool* GetOpenPtr() { return &m_open; }` (público) — usado por el menú `View` para el checkbox.
- `draw()` se guarda con el mismo patrón: `if (!m_open) return;` antes de `ImGui::Begin("Script Editor", &m_open)`.

Cerrar la ventana Script Editor solo la oculta. Las tabs de fichero abiertas (con su estado `dirty`, cursor, autocomplete) permanecen en memoria intactas; al reabrir desde `View`, todo sigue como estaba.

### 3. MenuBar nuevo

Nuevo método `EditorUI::drawMenuBar()`, usando `ImGui::BeginMainMenuBar()` / `ImGui::EndMainMenuBar()` — franja propia en la parte superior de la ventana de la aplicación, independiente y por encima de la Toolbar existente (que no se toca).

Se llama primero en `EditorUI::draw()`:
```cpp
void EditorUI::draw(...)
{
    drawMenuBar();
    drawToolbar();
    drawDockSpace();
    ...
}
```

Contenido del menú `View`:
```cpp
if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Scene", nullptr, &m_sceneOpen);
    ImGui::MenuItem("Viewport", nullptr, &m_viewportOpen);
    ImGui::MenuItem("Properties", nullptr, &m_propertiesOpen);
    ImGui::MenuItem("Log", nullptr, &m_logOpen);
    ImGui::MenuItem("Content Browser", nullptr, &m_contentBrowserOpen);
    ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
    ImGui::EndMenu();
}
```
`ImGui::MenuItem` con un `bool*` ya renderiza el checkbox y togglea el valor al click — no requiere lógica adicional.

### 4. Reapertura y persistencia de layout

Al pasar un `m_xxxOpen` de `false` a `true` vía el menú, ImGui vuelve a dibujar esa ventana en su posición docked previa: el layout (posición, tamaño, dock) se persiste automáticamente por nombre de ventana en `imgui.ini`, sin trabajo adicional.

### 5. Estado por defecto

Todos los paneles arrancan con `open = true`, igual que el comportamiento actual (todo visible desde el primer frame).

## Testing

No hay tests automatizados de ImGui en este repo. Verificación manual: cerrar cada panel con la "x", confirmar que desaparece; reabrir cada uno desde `View`, confirmar que reaparece en su posición docked previa con su estado intacto (selección en Scene, tabs en Script Editor, etc.).
