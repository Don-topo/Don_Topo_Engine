# Script Editor Panel — Design

## Objetivo

Editor de código Lua embebido en el editor del engine, usando `ImGuiColorTextEdit` con resaltado de sintaxis Lua. Solo abre/edita ficheros `.lua`. Soporta guardar y cargar contenido de texto en disco.

## Contexto

- El módulo de scripting Lua ya existe: `ScriptManager` (sol2, VM única, hot-reload por polling de mtime en `pollChanges()`), `ScriptComponent` (referencia por nombre a un script, sin contenido).
- `EditorUI` (`engine/src/EditorUI.cpp`) es el punto único de dibujado de paneles del editor; cada panel es una función `drawX()` llamada desde `EditorUI::draw()`.
- `FileManager` hoy solo soporta JSON (`readJson`/`writeJson`); no hay helpers de texto plano.
- Content Browser ya categoriza `.lua` mas no hace nada con doble-click salvo en directorios.
- No existe ningún editor de código ni integración de `ImGuiColorTextEdit` previa en el repo.

## Arquitectura

Nueva clase aislada `ScriptEditorPanel` (`engine/include/DonTopo/ScriptEditorPanel.h` + `engine/src/ScriptEditorPanel.cpp`), dueña de su propio estado multi-tab:

```cpp
struct Tab {
    std::filesystem::path path;
    TextEditor editor;   // ImGuiColorTextEdit
    bool dirty = false;
};

class ScriptEditorPanel {
public:
    void openFile(const std::filesystem::path& path); // no-op si ya abierto (foco tab existente)
    void draw();                                       // ImGui::Begin("Script Editor") + tab bar
private:
    void saveTab(Tab& tab);
    std::vector<Tab> m_tabs;
    int m_activeTab = -1;
};
```

`EditorUI` posee una instancia (`ScriptEditorPanel m_scriptEditor`) y llama `m_scriptEditor.draw()` desde `EditorUI::draw()`, igual que el resto de paneles (Log, Scene, Properties, etc). Se dockea automáticamente en el dockspace existente.

## Comportamiento del panel

- **Tabs**: `ImGui::BeginTabBar`, un `TabItem` por fichero abierto. Título muestra nombre de fichero, con `*` sufijo si `dirty == true`.
- **Edición**: cada tab renderiza su `TextEditor::Render()`. Tras cada frame, si `editor.IsTextChanged()`, marca `dirty = true`.
- **Guardar**: botón "Save" en la tab activa, o `Ctrl+S` con foco en el panel. Llama `saveTab()`.
- **Cerrar tab**: botón `x` en la tab. Si `dirty`, popup de confirmación (Save / Descartar / Cancelar) antes de cerrar.
- **Solo `.lua`**: `openFile` no valida extensión por contrato (los tres puntos de entrada solo pasan rutas `.lua`); no se expone ningún diálogo de apertura genérico.

## Puntos de integración

1. **Content Browser** (`EditorUI.cpp:2163`, hoy doble-click solo actúa en directorios): añadir rama — si el fichero clicado tiene extensión `.lua`, llamar `m_scriptEditor.openFile(path)`.
2. **Properties → Scripts** (`drawScriptsSection`, `EditorUI.cpp:1736`): junto a cada `ScriptComponent` listado, botón "Edit" que resuelve `m_scriptManager->scriptsDirPath() / (scriptName + ".lua")` y llama `openFile`.
3. **Popup "Nuevo Script"** (`drawNewScriptPopup`, `EditorUI.cpp:1968`): tras escribir la plantilla y llamar `loadScript`, añadir `m_scriptEditor.openFile(newPath)` para abrir la tab de inmediato.

## Guardado / carga en disco

`FileManager` gana dos métodos nuevos, mismo patrón sin excepciones que `readJson`/`writeJson`:

```cpp
static std::optional<std::string> readText(const std::filesystem::path& path);
static bool writeText(const std::filesystem::path& path, const std::string& content);
```

`ScriptEditorPanel::openFile` usa `readText`; `saveTab` usa `writeText`.

## Recarga en ScriptManager

`Save` **no** llama `loadScript` explícitamente. Se confía en el polling de mtime ya implementado en `ScriptManager::pollChanges()` — el mismo camino que detecta ediciones externas. Ninguna lógica nueva de reload.

## Dependencia nueva (CMake)

`ImGuiColorTextEdit` se añade con el mismo patrón que `ImGuiFileDialog`/`ImGuizmo` (`CMakeLists.txt:97-141`): `FetchContent_Declare` + `FetchContent_Populate` (no `MakeAvailable`, la lib no trae CMake usable) + target manual `imgui_texteditor` (STATIC, un único `.cpp`), linkado a `imgui_backend` y añadido a `target_link_libraries(DonTopoEngine ...)` en `engine/CMakeLists.txt:44-48`.

## Manejo de errores

- Fallo de lectura (fichero borrado externamente, permisos): log a Log Console existente; la tab no se abre (o si ya estaba abierta y el fichero desapareció, no se autocierra — el usuario puede seguir editando y el Save fallará con el mismo log).
- Fallo de escritura (Save): log a Log Console; la tab permanece `dirty`, no hay crash.
- Cierre de aplicación o cambio de escena con tabs `dirty`: **fuera de alcance** (YAGNI explícito) — no hay chequeo, los cambios sin guardar se pierden silenciosamente. Se puede añadir después si resulta molesto en uso real.

## Testing

Sin tests automatizados posibles: feature es 100% GUI y ningún subagente tiene acceso visual. Verificación manual pendiente tras merge:

- Abrir editor desde Content Browser (doble-click `.lua`).
- Abrir editor desde Properties (botón Edit en ScriptComponent).
- Editar y guardar; confirmar que `pollChanges()` recoge el cambio (hot-reload).
- Multi-tab con 2+ ficheros abiertos simultáneamente.
- Cerrar tab con cambios sin guardar → aparece popup de confirmación.
- Crear script nuevo vía popup existente → tab se abre automáticamente.
