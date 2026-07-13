# Script Editor — Chequeo de sintaxis y autocomplete básico — Design

## Objetivo

Añadir a `ScriptEditorPanel` (editor Lua embebido, ya existente) dos capacidades:

1. **Error markers**: al guardar (`Ctrl+S` / botón Save), marcar en el editor la línea con error de sintaxis Lua, si lo hay.
2. **Autocomplete básico**: popup con sugerencias (keywords Lua + símbolos de la API expuesta a scripts) mientras se escribe, filtrado por prefijo.

## Contexto

- `ScriptEditorPanel` (`engine/include/DonTopo/ScriptEditorPanel.h`, `engine/src/ScriptEditorPanel.cpp`) usa `TextEditor` (`ImGuiColorTextEdit`) con `LanguageDefinition::Lua()` — solo resaltado de sintaxis, sin diagnóstico ni autocomplete propios.
- `TextEditor` expone (confirmado en `build-ninja/_deps/imguicolortextedit-src/TextEditor.h`):
  - `SetErrorMarkers(const std::map<int,std::string>&)` — pinta línea + tooltip.
  - `GetCursorPosition()`, `SetCursorPosition()`, `GetCurrentLineText()`, `DeleteRange()`, `InsertText()`.
  - `SetHandleKeyboardInputs(bool)` — desactiva el manejo interno de teclado del editor (por defecto `true`). Con esto en `false`, `Render()` sigue dibujando pero no consume Up/Down/Enter/Tab/Esc — los podemos interceptar nosotros sin pelear con el editor.
  - No trae autocomplete ni chequeo de sintaxis integrados — hay que construir ambos.
- La API expuesta a los scripts Lua vive en `engine/src/ScriptBindings.cpp` (`Log`, `Input`, `Key`, `MouseButton`, `Transform`, `Entity`, colliders, `AudioClip`, `Scene`, `Vec3`) — fuente de la lista de autocomplete.
- El panel está intencionalmente desacoplado de `ScriptManager` (comentario en el header: "no conoce ScriptManager"). Ambas features nuevas respetan esto: el chequeo de sintaxis usa un `lua_State` propio y descartable, la lista de autocomplete es una tabla estática de datos.

## Arquitectura

Dos ficheros nuevos, sin dependencias de `ScriptManager`:

### `LuaSyntaxCheck.h/.cpp`

```cpp
namespace DonTopo {
// Compila (no ejecuta) el fuente en un lua_State descartable.
// nullopt si compila OK. Si falla: {línea, mensaje} parseados del error de Lua.
std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source);
}
```

Implementación: `luaL_newstate()` + `luaL_loadstring()` (no hace falta `luaL_openlibs`, solo se parsea). El error de Lua tiene forma `[string "..."]:LINE: mensaje`; se extrae `LINE` y `mensaje` con una regex simple. Se cierra el `lua_State` siempre (RAII o `lua_close` en todos los caminos).

### `LuaApiReference.h/.cpp`

```cpp
namespace DonTopo {
// Tabla estática (keywords Lua + símbolos de ScriptBindings.cpp), construida una vez.
const std::vector<std::string>& luaApiSymbols();
}
```

Contenido (mantenido a mano, sin reflexión de sol2):

- Keywords: `and break do else elseif end false for function goto if in local nil not or repeat return then true until while`.
- Globals/tablas: `print`, `Log.Info` `Log.Warn` `Log.Error`, `Input.IsKeyDown` `Input.IsKeyPressed` `Input.IsKeyReleased` `Input.IsMouseButtonDown`, `Key.*` (Space/Enter/Escape/Tab/LeftShift/LeftControl/Up/Down/Left/Right/A-Z/Num0-9), `MouseButton.Left` `MouseButton.Right` `MouseButton.Middle`.
- `Entity`: `name`, `IsValid`, `GetTransform`, `GetParent`, `GetChildren`, `GetComponent`, `AddComponent`, `RemoveComponent` (prefijo `Entity:` en la entrada de la tabla, p.ej. `Entity:GetTransform`).
- `Transform:GetPosition/SetPosition/GetRotation/SetRotation/GetScale/SetScale/GetWorldPosition/Translate/Rotate`.
- `BoxCollider/SphereCollider/CapsuleCollider/PlaneCollider:` getters/setters correspondientes + `IsDynamic`.
- `AudioClip:Play/Stop/SetLoop/GetLoop`.
- `Scene.Find/CreateGameObject/Destroy/Instantiate`.
- `Vec3.new`.

## Cambios en `ScriptEditorPanel`

### Error markers (al guardar)

En `saveTab()`, tras `FileManager::writeText` exitoso:

```cpp
auto err = checkLuaSyntax(tab.editor.GetText());
TextEditor::ErrorMarkers markers;
if (err) markers[err->first] = err->second;
tab.editor.SetErrorMarkers(markers);
```

Si no hay error, `markers` vacío limpia cualquier marca previa. No corre en cada tecla — solo en Save, coincide con el flujo actual.

### Autocomplete

Nuevo estado por `Tab`:

```cpp
struct Tab {
    // ...existente...
    bool acVisible = false;
    bool acDismissed = false;     // true tras Esc, hasta que el fragmento cambie
    std::vector<std::string> acMatches;
    int acSelected = 0;
    TextEditor::Coordinates acFragmentStart;
};
```

Flujo en `draw()`, para la tab activa, después de `tab.editor.Render(...)`:

1. **Detectar fragmento actual**: desde `GetCursorPosition()`, escanear `GetCurrentLineText()` hacia atrás mientras el carácter sea `[A-Za-z0-9_.:]` (incluye `.`/`:` para soportar `Entity:Get...` y `Log.I...`). Da `acFragmentStart` + el texto del fragmento.
2. **Trigger automático**: si `tab.editor.IsTextChanged()` y el fragmento tiene ≥ 2 caracteres y `!acDismissed`: filtrar `luaApiSymbols()` por *prefijo case-insensitive* → `acMatches`. Si no vacío, `acVisible = true`, `acSelected = 0`. Si el fragmento cambia respecto al último chequeo, resetear `acDismissed = false` (permite que vuelva a aparecer al seguir escribiendo tras un Esc).
3. **Trigger manual**: `Ctrl+Space` con foco en la tab → recalcula y fuerza `acVisible = true` (ignora `acDismissed` y el mínimo de 2 caracteres), aunque el fragmento esté vacío (lista completa filtrable).
4. **Mientras `acVisible`**:
   - `tab.editor.SetHandleKeyboardInputs(false)` antes de que termine el frame (para el próximo `Render()`).
   - Manejo manual de teclas (vía `ImGui::IsKeyPressed`, ya que el editor no las consume):
     - `Down`/`Up`: mueve `acSelected` (wrap).
     - `Enter`/`Tab`: acepta — `tab.editor.DeleteRange(acFragmentStart, cursorActual)`, `tab.editor.InsertText(acMatches[acSelected])`, `acVisible = false`, `tab.editor.SetHandleKeyboardInputs(true)`.
     - `Escape`: `acVisible = false`, `acDismissed = true`, `tab.editor.SetHandleKeyboardInputs(true)`.
   - Si `acVisible == false` (por no-match o cierre), `tab.editor.SetHandleKeyboardInputs(true)`.
5. **Render del popup**: ventana ImGui pequeña sin decoración (`ImGuiWindowFlags_NoTitleBar | NoResize | NoMove | NoFocusOnAppearing`), posicionada con `ImGui::SetNextWindowPos(...)` aproximando la posición del cursor: origen = `ImGui::GetItemRectMin()` capturado tras el `Render()` del editor, offset X = columna del fragmento × `ImGui::CalcTextSize("A").x`, offset Y = (línea actual − primera línea visible) × `ImGui::GetTextLineHeightWithSpacing()`. La fuente por defecto de ImGui (Proggy Clean) es monoespaciada, así que la aproximación es exacta en la práctica. Lista de hasta 8 entradas visibles (scroll si hay más), entrada seleccionada resaltada.
   - **Límite conocido**: `TextEditor` no expone getter público de scroll/primera-línea-visible. Como aproximación inicial se asume "primera línea visible = 0" (offset Y = línea actual × altura), lo cual desplaza el popup verticalmente si el usuario escribe con el editor scrolleado hacia abajo. No bloquea funcionalidad (teclado sigue funcionando igual), solo estética. Se documenta como límite conocido; no se resuelve en esta iteración (evitar reabrir el child window de `TextEditor` con su mismo ID para leer `GetScrollY()` — truco fráfil, fuera de alcance de "básico").

## Archivos nuevos / modificados

- **Nuevos**: `engine/include/DonTopo/LuaSyntaxCheck.h`, `engine/src/LuaSyntaxCheck.cpp`, `engine/include/DonTopo/LuaApiReference.h`, `engine/src/LuaApiReference.cpp`.
- **Modificados**: `engine/include/DonTopo/ScriptEditorPanel.h`, `engine/src/ScriptEditorPanel.cpp`, `engine/CMakeLists.txt` (añadir las dos fuentes nuevas al target).

## Manejo de errores

- Error de sintaxis Lua: no es un fallo del engine, es información pa el usuario — se muestra vía `SetErrorMarkers`, nunca se loguea a Log Console (sería ruido redundante con el marker visual).
- `checkLuaSyntax` nunca lanza — cualquier fallo interno de `lua_State` (alloc failure de `luaL_newstate`, extremadamente raro) se trata como "no se pudo verificar", `nullopt`, sin crashear el editor.
- Autocomplete sin matches: `acMatches` vacío simplemente no muestra popup, sin log ni error.

## Fuera de alcance (YAGNI explícito)

- Autocomplete con conocimiento de tipos/scope real (variables locales del script, tabla de `self`, etc) — solo símbolos globales estáticos.
- Chequeo semántico (nombres de componentes inválidos, llamadas a métodos que no existen en runtime) — solo sintaxis.
- Actualización automática de `LuaApiReference` si cambia `ScriptBindings.cpp` — lista mantenida a mano.
- Chequeo de sintaxis en vivo mientras se escribe (debounce) — solo al guardar.

## Testing

Sin tests automatizados posibles (feature 100% GUI, ningún subagente tiene acceso visual). Verificación manual pendiente tras merge:

- Guardar script con error de sintaxis deliberado (p.ej. `if` sin `end`) → aparece marker en la línea correcta con el mensaje de Lua.
- Guardar script válido tras uno con error → marker desaparece.
- Escribir `Log.I` → popup ofrece `Log.Info`; `Enter` la inserta completa.
- Escribir `Ent` → popup ofrece símbolos `Entity:*`/`Entity.name` que empiezan así.
- `Escape` cierra popup; seguir escribiendo la misma palabra no lo vuelve a abrir hasta que cambie el fragmento; `Ctrl+Space` lo reabre a la fuerza.
- Navegar con `Up`/`Down` no mueve el cursor de texto del editor mientras el popup está abierto.
- Aceptar sugerencia no dispara además un salto de línea/tab en el texto (verifica que `SetHandleKeyboardInputs(false)` bloqueó el `Enter`/`Tab` del editor ese frame).
