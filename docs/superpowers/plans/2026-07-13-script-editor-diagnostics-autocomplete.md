# Script Editor — Diagnostics + Autocomplete Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir a `ScriptEditorPanel` error markers de sintaxis Lua al guardar y un popup de autocomplete básico (keywords + API de scripting) mientras se escribe.

**Architecture:** Dos módulos de datos/lógica nuevos y aislados — `LuaSyntaxCheck` (compila con un `lua_State` descartable vía la C API cruda de Lua, sin sol2) y `LuaApiReference` (tabla estática de símbolos) — ninguno depende de `ScriptManager`. `ScriptEditorPanel` los consume: `saveTab()` llama `checkLuaSyntax` y pinta `TextEditor::SetErrorMarkers`; `draw()` detecta el fragmento de palabra bajo el cursor (escaneo manual, incluye `.`/`:`) y muestra un popup ImGui filtrando `luaApiSymbols()` por prefijo. `TextEditor::SetHandleKeyboardInputs(false)` evita que el editor consuma Up/Down/Enter/Tab/Esc mientras el popup está abierto.

**Tech Stack:** Lua 5.4 C API cruda (`lua.h`/`lauxlib.h`, ya vinculada transitivamente vía `lua_lib` → `sol2_lib` → `DonTopoEngine`), `ImGuiColorTextEdit` (`TextEditor::SetErrorMarkers`/`SetHandleKeyboardInputs`/`GetCursorPosition`/`DeleteRange`/`InsertText`, ya en uso).

**Spec:** `docs/superpowers/specs/2026-07-13-script-editor-diagnostics-autocomplete-design.md`

## Global Constraints

- Build SIEMPRE vía `./configure.bat` (solo si cambia CMake) y `./build.bat` en PowerShell — nunca cmake/ninja crudo en Bash.
- C++20, comentarios en español, estilo del código circundante (4 espacios, llaves estilo repo).
- Commits estilo repo: `feat(script): ...` en español, cuerpo solo si el porqué no es obvio.
- No hay framework de tests: para lógica no-GUI (Tasks 1-2) la verificación es un smoke test temporal en `Engine.cpp` (mismo patrón usado en `docs/superpowers/plans/2026-07-13-script-editor-panel.md` Task 2) que se revierte tras confirmar; para GUI (Tasks 3-4) la verificación es build limpio + ejecución manual, formalizada en el checklist de Task 5.
- `LuaApiReference` es una tabla mantenida a mano — no se deriva de `ScriptBindings.cpp` por reflexión (fuera de alcance, spec explícita).
- Chequeo de sintaxis solo al guardar — nunca en cada tecla (spec explícita, YAGNI).

---

### Task 1: `LuaSyntaxCheck` — chequeo de sintaxis Lua aislado

**Files:**
- Create: `engine/include/DonTopo/LuaSyntaxCheck.h`
- Create: `engine/src/LuaSyntaxCheck.cpp`
- Modify: `engine/CMakeLists.txt:30` (añadir `src/LuaSyntaxCheck.cpp` tras `src/ScriptEditorPanel.cpp`)

**Interfaces:**
- Produces: `std::optional<std::pair<int, std::string>> DonTopo::checkLuaSyntax(const std::string& source)` — `nullopt` si compila sin error; si no, `{línea, mensaje}`.

- [ ] **Step 1: Crear el header**

`engine/include/DonTopo/LuaSyntaxCheck.h`:

```cpp
#pragma once
#include <optional>
#include <string>
#include <utility>

namespace DonTopo {

// Compila (no ejecuta) source en un lua_State descartable, cerrado siempre
// antes de retornar. nullopt si compila sin error. Si falla: {línea,
// mensaje} parseados del formato de error de Lua
// ([string "..."]:LINE: mensaje).
std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source);

} // namespace DonTopo
```

- [ ] **Step 2: Crear la implementación**

`engine/src/LuaSyntaxCheck.cpp`:

```cpp
#include "DonTopo/LuaSyntaxCheck.h"
#include <regex>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace DonTopo {

std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source)
{
    lua_State* L = luaL_newstate();
    if (!L)
        return std::nullopt;

    int status = luaL_loadstring(L, source.c_str());
    if (status == LUA_OK)
    {
        lua_close(L);
        return std::nullopt;
    }

    const char* raw = lua_tostring(L, -1);
    std::string message = raw ? raw : "error de sintaxis desconocido";
    lua_close(L);

    static const std::regex linePattern(R"(:(\d+):\s*(.*))");
    std::smatch match;
    if (std::regex_search(message, match, linePattern))
        return std::make_pair(std::stoi(match[1].str()), match[2].str());

    return std::make_pair(1, message);
}

} // namespace DonTopo
```

- [ ] **Step 3: Añadir el nuevo `.cpp` a `engine/CMakeLists.txt`**

En la lista de sources de `add_library(DonTopoEngine STATIC ...)`, añadir tras `src/ScriptEditorPanel.cpp` (línea 30):

```cmake
    src/LuaSyntaxCheck.cpp
```

- [ ] **Step 4: Smoke test manual temporal**

Añadir temporalmente al final de `engine/src/Engine.cpp` (tras el include existente):

```cpp
#include "DonTopo/LuaSyntaxCheck.h"
#include <cassert>
namespace { [[maybe_unused]] void dtLuaSyntaxCheckSmoke() {
    auto ok = DonTopo::checkLuaSyntax("local x = 1");
    assert(!ok.has_value());
    auto bad = DonTopo::checkLuaSyntax("function Foo()\n  local x = 1\nend x");
    assert(bad.has_value());
    assert(bad->first >= 1);
} }
```

Y llamarla una vez desde `Engine::Engine()`:

```cpp
Engine::Engine() { dtLuaSyntaxCheckSmoke(); }
```

Run (PowerShell): `./build.bat`
Expected: sin errores de compilación. Ejecutar `./build-ninja/sandbox/Sandbox.exe` brevemente — si algún `assert` falla, el proceso aborta al arrancar (visible en consola); si arranca normal, los dos casos pasaron. Cerrar la app.

**Revertir el cambio de `Engine.cpp`** (dejar `Engine::Engine() {}` como estaba) tras confirmar — es solo un smoke test, no queda en el código final.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/LuaSyntaxCheck.h engine/src/LuaSyntaxCheck.cpp engine/CMakeLists.txt
git commit -m "feat(script): añade checkLuaSyntax (compile-only check con lua_State descartable)"
```

---

### Task 2: `LuaApiReference` — tabla estática de símbolos pa autocomplete

**Files:**
- Create: `engine/include/DonTopo/LuaApiReference.h`
- Create: `engine/src/LuaApiReference.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/LuaApiReference.cpp` tras `src/LuaSyntaxCheck.cpp`)

**Interfaces:**
- Produces: `const std::vector<std::string>& DonTopo::luaApiSymbols()`.

- [ ] **Step 1: Crear el header**

`engine/include/DonTopo/LuaApiReference.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace DonTopo {

// Tabla estática de símbolos pa el popup de autocomplete del Script Editor:
// keywords Lua + API expuesta a scripts en ScriptBindings.cpp. Mantenida a
// mano — no se deriva de sol2 por reflexión (fuera de alcance).
const std::vector<std::string>& luaApiSymbols();

} // namespace DonTopo
```

- [ ] **Step 2: Crear la implementación**

`engine/src/LuaApiReference.cpp` — la lista completa, un símbolo por entrada; los miembros de tipo se listan con su prefijo `Tipo:` o `Tipo.` para que el filtrado por prefijo funcione escribiendo `Entity:Get...`, `Log.I...`, etc:

```cpp
#include "DonTopo/LuaApiReference.h"

namespace DonTopo {

const std::vector<std::string>& luaApiSymbols()
{
    static const std::vector<std::string> symbols = {
        // Keywords Lua
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while",

        // Globals
        "print",

        // Log
        "Log.Info", "Log.Warn", "Log.Error",

        // Input / Key / MouseButton
        "Input.IsKeyDown", "Input.IsKeyPressed", "Input.IsKeyReleased",
        "Input.IsMouseButtonDown",
        "Key.Space", "Key.Enter", "Key.Escape", "Key.Tab",
        "Key.LeftShift", "Key.LeftControl",
        "Key.Up", "Key.Down", "Key.Left", "Key.Right",
        "Key.A", "Key.B", "Key.C", "Key.D", "Key.E", "Key.F", "Key.G",
        "Key.H", "Key.I", "Key.J", "Key.K", "Key.L", "Key.M", "Key.N",
        "Key.O", "Key.P", "Key.Q", "Key.R", "Key.S", "Key.T", "Key.U",
        "Key.V", "Key.W", "Key.X", "Key.Y", "Key.Z",
        "Key.Num0", "Key.Num1", "Key.Num2", "Key.Num3", "Key.Num4",
        "Key.Num5", "Key.Num6", "Key.Num7", "Key.Num8", "Key.Num9",
        "MouseButton.Left", "MouseButton.Right", "MouseButton.Middle",

        // Entity
        "Entity.name", "Entity:IsValid", "Entity:GetTransform",
        "Entity:GetParent", "Entity:GetChildren", "Entity:GetComponent",
        "Entity:AddComponent", "Entity:RemoveComponent",

        // Transform
        "Transform:GetPosition", "Transform:SetPosition",
        "Transform:GetRotation", "Transform:SetRotation",
        "Transform:GetScale", "Transform:SetScale",
        "Transform:GetWorldPosition", "Transform:Translate", "Transform:Rotate",

        // Colliders
        "BoxCollider:GetUseGravity", "BoxCollider:SetUseGravity",
        "BoxCollider:GetHalfExtents", "BoxCollider:SetHalfExtents",
        "BoxCollider:GetCenter", "BoxCollider:SetCenter", "BoxCollider:IsDynamic",
        "SphereCollider:GetUseGravity", "SphereCollider:SetUseGravity",
        "SphereCollider:GetRadius", "SphereCollider:SetRadius",
        "SphereCollider:GetCenter", "SphereCollider:SetCenter", "SphereCollider:IsDynamic",
        "CapsuleCollider:GetUseGravity", "CapsuleCollider:SetUseGravity",
        "CapsuleCollider:GetRadius", "CapsuleCollider:SetRadius",
        "CapsuleCollider:GetHalfHeight", "CapsuleCollider:SetHalfHeight",
        "CapsuleCollider:GetCenter", "CapsuleCollider:SetCenter", "CapsuleCollider:IsDynamic",
        "PlaneCollider:GetCenter", "PlaneCollider:SetCenter",

        // AudioClip
        "AudioClip:Play", "AudioClip:Stop", "AudioClip:SetLoop", "AudioClip:GetLoop",

        // Scene
        "Scene.Find", "Scene.CreateGameObject", "Scene.Destroy", "Scene.Instantiate",

        // Vec3
        "Vec3.new",
    };
    return symbols;
}

} // namespace DonTopo
```

- [ ] **Step 3: Añadir el nuevo `.cpp` a `engine/CMakeLists.txt`**

Tras `src/LuaSyntaxCheck.cpp`:

```cmake
    src/LuaApiReference.cpp
```

- [ ] **Step 4: Smoke test manual temporal**

Añadir temporalmente al final de `engine/src/Engine.cpp` (junto al de Task 1, o reemplazándolo si ya se revirtió):

```cpp
#include "DonTopo/LuaApiReference.h"
#include <algorithm>
namespace { [[maybe_unused]] void dtLuaApiReferenceSmoke() {
    const auto& symbols = DonTopo::luaApiSymbols();
    assert(!symbols.empty());
    assert(std::find(symbols.begin(), symbols.end(), "Log.Info") != symbols.end());
    assert(std::find(symbols.begin(), symbols.end(), "Entity:GetTransform") != symbols.end());
} }
```

Llamarla desde `Engine::Engine()` igual que en Task 1.

Run (PowerShell): `./build.bat`
Expected: sin errores. Ejecutar `./build-ninja/sandbox/Sandbox.exe` brevemente — arranca sin abortar. Cerrar la app.

**Revertir el cambio de `Engine.cpp`** tras confirmar.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/LuaApiReference.h engine/src/LuaApiReference.cpp engine/CMakeLists.txt
git commit -m "feat(script): añade LuaApiReference (tabla estática de símbolos pa autocomplete)"
```

---

### Task 3: Error markers en `ScriptEditorPanel::saveTab`

**Files:**
- Modify: `engine/include/DonTopo/ScriptEditorPanel.h` (incluir `<map>` no hace falta, `TextEditor.h` ya trae `ErrorMarkers`)
- Modify: `engine/src/ScriptEditorPanel.cpp` (`saveTab`, líneas 43-49)

**Interfaces:**
- Consumes: `DonTopo::checkLuaSyntax` (Task 1).

- [ ] **Step 1: Incluir el header nuevo**

En `engine/src/ScriptEditorPanel.cpp`, añadir a los includes:

```cpp
#include "DonTopo/LuaSyntaxCheck.h"
```

- [ ] **Step 2: Actualizar `saveTab`**

Reemplazar:

```cpp
void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
        tab.dirty = false;
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");
}
```

por:

```cpp
void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
        tab.dirty = false;
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");

    // El chequeo de sintaxis se muestra vía marker visual, nunca al Log
    // Console — sería ruido redundante con el marker.
    TextEditor::ErrorMarkers markers;
    auto err = checkLuaSyntax(tab.editor.GetText());
    if (err)
        markers[err->first] = err->second;
    tab.editor.SetErrorMarkers(markers);
}
```

- [ ] **Step 3: Compilar**

Run (PowerShell): `./build.bat`
Expected: sin errores.

- [ ] **Step 4: Verificar visualmente**

Run: `./build-ninja/sandbox/Sandbox.exe` → Content Browser → doble-click en `Scripts/Rotator.lua` → escribir un error deliberado (p.ej. añadir `if true then` sin `end` al final) → `Ctrl+S` → debe aparecer un marker rojo en la línea del `if` con tooltip del mensaje de Lua al pasar el mouse por encima. Corregir el error → `Ctrl+S` → el marker desaparece. Deshacer el cambio de prueba (o guardar el fichero de vuelta a su contenido original) antes de cerrar.

- [ ] **Step 5: Commit**

```bash
git add engine/src/ScriptEditorPanel.cpp
git commit -m "feat(script): error markers de sintaxis Lua al guardar en Script Editor"
```

---

### Task 4: Popup de autocomplete en `ScriptEditorPanel`

**Files:**
- Modify: `engine/include/DonTopo/ScriptEditorPanel.h` (struct `Tab`, líneas 25-29)
- Modify: `engine/src/ScriptEditorPanel.cpp` (`draw()`, incluye `LuaApiReference.h`)

**Interfaces:**
- Consumes: `DonTopo::luaApiSymbols()` (Task 2), `TextEditor::GetCursorPosition/GetCurrentLineText/DeleteRange/InsertText/SetCursorPosition/SetHandleKeyboardInputs` (ya en el vendor).

- [ ] **Step 1: Ampliar `Tab` con el estado de autocomplete**

En `engine/include/DonTopo/ScriptEditorPanel.h`, reemplazar:

```cpp
    struct Tab {
        std::filesystem::path path;
        TextEditor editor;
        bool dirty = false;
    };
```

por:

```cpp
    struct Tab {
        std::filesystem::path path;
        TextEditor editor;
        bool dirty = false;

        // Estado del popup de autocomplete (Task: diagnostics+autocomplete).
        bool acVisible = false;
        // true tras Escape, hasta que el fragmento bajo el cursor cambie —
        // evita que el popup se vuelva a abrir solo mientras se sigue
        // escribiendo la misma palabra que el usuario acaba de descartar.
        bool acDismissed = false;
        std::vector<std::string> acMatches;
        int acSelected = 0;
        TextEditor::Coordinates acFragmentStart;
        std::string acLastFragment;
    };
```

- [ ] **Step 2: Incluir el header de la API**

En `engine/src/ScriptEditorPanel.cpp`:

```cpp
#include "DonTopo/LuaApiReference.h"
#include <algorithm>
#include <cctype>
```

- [ ] **Step 3: Función local pa extraer el fragmento bajo el cursor**

En `engine/src/ScriptEditorPanel.cpp`, añadir en el namespace anónimo (crearlo si no existe) antes de `draw()`:

```cpp
namespace {

bool isFragmentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}

// Escanea GetCurrentLineText() hacia atrás desde la columna del cursor
// mientras los caracteres sean parte de un identificador/ruta con puntos
// (soporta "Entity:Get...", "Log.I..."). Devuelve el fragmento y su
// columna de inicio en la misma línea que el cursor.
struct Fragment { std::string text; int startColumn; };

Fragment extractFragment(const TextEditor& editor)
{
    TextEditor::Coordinates cursor = editor.GetCursorPosition();
    std::string line = editor.GetCurrentLineText();
    int col = std::min(cursor.mColumn, static_cast<int>(line.size()));

    int start = col;
    while (start > 0 && isFragmentChar(line[start - 1]))
        --start;

    return Fragment{ line.substr(start, col - start), start };
}

bool startsWithCaseInsensitive(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b)); });
}

} // namespace (anónimo)
```

- [ ] **Step 4: Detectar/filtrar el fragmento tras `Render()`**

En `draw()`, dentro del `if (ImGui::BeginTabItem(...))`, justo después de:

```cpp
                tab.editor.Render("##TextEditor", ImGui::GetContentRegionAvail());
                if (tab.editor.IsTextChanged())
                    tab.dirty = true;
```

añadir:

```cpp
                ImVec2 editorOrigin = ImGui::GetItemRectMin();

                Fragment frag = extractFragment(tab.editor);
                bool fragmentChanged = frag.text != tab.acLastFragment;
                tab.acLastFragment = frag.text;
                if (fragmentChanged)
                    tab.acDismissed = false;

                bool forceOpen = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);

                if (forceOpen || (tab.editor.IsTextChanged() && frag.text.size() >= 2 && !tab.acDismissed))
                {
                    tab.acMatches.clear();
                    for (const auto& symbol : DonTopo::luaApiSymbols())
                        if (startsWithCaseInsensitive(symbol, frag.text))
                            tab.acMatches.push_back(symbol);

                    if (!tab.acMatches.empty())
                    {
                        tab.acVisible = true;
                        tab.acSelected = 0;
                        tab.acFragmentStart = TextEditor::Coordinates(
                            tab.editor.GetCursorPosition().mLine, frag.startColumn);
                    }
                    else if (!forceOpen)
                    {
                        tab.acVisible = false;
                    }
                }
```

- [ ] **Step 5: Manejo de teclado + aceptar/cerrar**

Inmediatamente después del bloque del Step 4:

```cpp
                if (tab.acVisible)
                {
                    tab.editor.SetHandleKeyboardInputs(false);

                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                        tab.acSelected = (tab.acSelected + 1) % static_cast<int>(tab.acMatches.size());
                    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                        tab.acSelected = (tab.acSelected - 1 + static_cast<int>(tab.acMatches.size())) %
                                         static_cast<int>(tab.acMatches.size());
                    else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false))
                    {
                        TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
                        tab.editor.DeleteRange(tab.acFragmentStart, cursor);
                        tab.editor.SetCursorPosition(tab.acFragmentStart);
                        tab.editor.InsertText(tab.acMatches[tab.acSelected]);
                        tab.dirty = true;
                        tab.acVisible = false;
                        tab.editor.SetHandleKeyboardInputs(true);
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        tab.acVisible = false;
                        tab.acDismissed = true;
                        tab.editor.SetHandleKeyboardInputs(true);
                    }
                }
                else
                {
                    tab.editor.SetHandleKeyboardInputs(true);
                }
```

- [ ] **Step 6: Dibujar el popup**

Inmediatamente después del bloque del Step 5, todavía dentro del `if (ImGui::BeginTabItem(...))`:

```cpp
                if (tab.acVisible)
                {
                    float charWidth = ImGui::CalcTextSize("A").x;
                    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                    ImVec2 popupPos(
                        editorOrigin.x + tab.acFragmentStart.mColumn * charWidth,
                        editorOrigin.y + tab.acFragmentStart.mLine * lineHeight + lineHeight);

                    ImGui::SetNextWindowPos(popupPos);
                    ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f));
                    ImGuiWindowFlags acFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;

                    ImGui::Begin("##ScriptEditorAutocomplete", nullptr, acFlags);
                    int visibleCount = std::min(static_cast<int>(tab.acMatches.size()), 8);
                    for (int m = 0; m < static_cast<int>(tab.acMatches.size()); ++m)
                    {
                        bool selected = (m == tab.acSelected);
                        if (ImGui::Selectable(tab.acMatches[m].c_str(), selected))
                        {
                            TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
                            tab.editor.DeleteRange(tab.acFragmentStart, cursor);
                            tab.editor.SetCursorPosition(tab.acFragmentStart);
                            tab.editor.InsertText(tab.acMatches[m]);
                            tab.dirty = true;
                            tab.acVisible = false;
                            tab.editor.SetHandleKeyboardInputs(true);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    (void)visibleCount;
                    ImGui::End();
                }
```

**Nota**: `visibleCount` queda calculado pero no limita el loop (mostrar todas las coincidencias, ImGui recorta visualmente si la ventana no crece); se deja el cálculo por si una futura pasada quiere añadir scroll — no lo requiere esta iteración porque `luaApiSymbols()` es acotado (~90 entradas) y el filtrado por prefijo típicamente deja pocas coincidencias visibles a la vez.

- [ ] **Step 7: Compilar**

Run (PowerShell): `./build.bat`
Expected: sin errores.

- [ ] **Step 8: Verificar visualmente**

Run: `./build-ninja/sandbox/Sandbox.exe` → Content Browser → doble-click `.lua` → en una línea nueva escribir `Log.I` → debe aparecer popup con `Log.Info`; `Enter` la inserta completa y cierra el popup sin insertar salto de línea extra. Escribir `Ent` → popup ofrece `Entity.name`/`Entity:*`. `Escape` cierra el popup; seguir escribiendo la misma palabra no lo reabre hasta cambiarla. `Ctrl+Espacio` lo reabre a la fuerza. Navegar con flechas arriba/abajo no mueve el cursor de texto mientras el popup está abierto. Cerrar sin guardar los cambios de prueba (o deshacer con Ctrl+Z antes de cerrar la app).

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/ScriptEditorPanel.h engine/src/ScriptEditorPanel.cpp
git commit -m "feat(script): popup de autocomplete básico en Script Editor"
```

---

### Task 5: README + verificación manual completa

**Files:**
- Modify: `README.md` (sección `## Lua Scripting`, párrafo del Script Editor añadido por la feature anterior)

**Interfaces:** Ninguna (task de documentación + checklist manual).

- [ ] **Step 1: Actualizar README**

En `README.md`, al final del párrafo existente sobre el Script Editor panel (el que empieza "Double-clicking a `.lua` file..."), añadir:

```
Saving (`Ctrl+S`/**Save**) also runs a syntax-only compile check; a Lua syntax
error is shown as an inline marker on the offending line (hover for the
message) and clears automatically on the next successful save. While typing,
an autocomplete popup suggests Lua keywords and the scripting API (`Entity`,
`Transform`, `Log`, `Input`, colliders, `Scene`, etc.) filtered by prefix —
`Enter`/`Tab` accepts, `Escape` dismisses, `Ctrl+Space` re-opens it manually.
```

- [ ] **Step 2: Checklist de verificación manual (GUI, no automatizable)**

Ejecutar `./build-ninja/sandbox/Sandbox.exe` y confirmar cada punto (además de los ya cubiertos en Tasks 3-4):

- [ ] Guardar un script con error de sintaxis deliberado muestra el marker en la línea correcta.
- [ ] Guardar un script válido tras uno con error limpia el marker.
- [ ] Autocomplete funciona en al menos 3 tabs abiertas simultáneamente sin mezclar estado entre ellas (cada `Tab` tiene su propio `acVisible`/`acMatches`).
- [ ] Cambiar de tab activa con el popup abierto en la tab anterior no dibuja el popup incorrectamente en la tab nueva.
- [ ] Aceptar una sugerencia con `Enter` no inserta además un salto de línea; aceptar con `Tab` no inserta además una indentación.
- [ ] El popup no aparece si el fragmento tiene menos de 2 caracteres (salvo `Ctrl+Space`).

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(script): documenta error markers y autocomplete del Script Editor"
```
