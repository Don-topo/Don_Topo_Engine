# Script Editor Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Panel de editor de código embebido en el editor (ImGui), multi-tab, con resaltado Lua vía ImGuiColorTextEdit, que solo abre/edita/guarda ficheros `.lua`.

**Architecture:** Nueva clase aislada `ScriptEditorPanel` (no conoce `ScriptManager` ni `GameObject`, solo `std::filesystem::path` + texto) dueña de una lista de tabs (`TextEditor` de ImGuiColorTextEdit + path + flag dirty). `EditorUI` la posee vía `unique_ptr` (mismo patrón que `m_meshFileDialog`) y la alimenta desde 3 puntos de entrada: doble-click `.lua` en Content Browser, botón "Edit" en la sección Scripts de Properties, y el popup "Nuevo Script" tras crear el fichero. Guardar escribe a disco con nuevos helpers de `FileManager`; la recarga en `ScriptManager` la sigue haciendo el polling de mtime ya existente — el panel no llama `loadScript`.

**Tech Stack:** ImGuiColorTextEdit (BalazsJako, FetchContent), ImGui docking (ya presente), `FileManager` (ya presente, se extiende).

**Spec:** `docs/superpowers/specs/2026-07-13-script-editor-panel-design.md`

## Global Constraints

- Build SIEMPRE vía `./configure.bat` (solo si cambia CMake) y `./build.bat` en PowerShell — nunca cmake/ninja crudo en Bash.
- C++20, comentarios en español, estilo del código circundante (4 espacios, llaves estilo repo).
- Commits estilo repo: `feat(script): ...` en español, cuerpo solo si el porqué no es obvio.
- No hay framework de tests: la verificación de cada task es build limpio; la verificación funcional final es Task 8 + GUI manual.
- Solo `.lua`: ningún punto de entrada nuevo abre otra extensión; no se expone diálogo de apertura genérico.
- Cierre de app/escena con tabs dirty: fuera de alcance (spec, YAGNI explícito) — no tocar `main.cpp` para eso.

---

### Task 1: Dependencia CMake — ImGuiColorTextEdit

**Files:**
- Modify: `CMakeLists.txt` (raíz, tras el bloque ImGuizmo, ~línea 141)
- Modify: `engine/CMakeLists.txt` (target_link_libraries, líneas 44-48)

**Interfaces:**
- Produces: target `imgui_texteditor` (STATIC). `#include <TextEditor.h>` disponible para DonTopoEngine; clase global `TextEditor` con `SetLanguageDefinition(TextEditor::LanguageDefinition::Lua())`, `SetText`, `GetText`, `Render`, `IsTextChanged`.

- [ ] **Step 1: Añadir el bloque FetchContent al `CMakeLists.txt` raíz**

Insertar tras el bloque de ImGuizmo (después de la línea `target_compile_features(imguizmo PUBLIC cxx_std_17)`, ~línea 141), antes del comentario `# nlohmann/json`:

```cmake
# ImGuiColorTextEdit — editor de código embebido (Script Editor Panel)
# Igual que ImGuiFileDialog/ImGuizmo: Populate en vez de MakeAvailable, el
# repo no trae CMakeLists usable (solo demo standalone).
FetchContent_Declare(
    ImGuiColorTextEdit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(ImGuiColorTextEdit)
if(NOT imguicolortextedit_POPULATED)
    FetchContent_Populate(ImGuiColorTextEdit)
endif()

add_library(imgui_texteditor STATIC
    ${imguicolortextedit_SOURCE_DIR}/TextEditor.cpp
)
target_include_directories(imgui_texteditor PUBLIC
    ${imguicolortextedit_SOURCE_DIR}
)
target_link_libraries(imgui_texteditor PUBLIC imgui_backend)
target_compile_features(imgui_texteditor PUBLIC cxx_std_17)
```

- [ ] **Step 2: Linkear en `engine/CMakeLists.txt`**

En `target_link_libraries(DonTopoEngine PUBLIC ...)` (líneas 38-49), añadir tras `imguizmo`:

```cmake
        imguizmo
        imgui_texteditor
```

- [ ] **Step 3: Configurar y compilar**

Run (PowerShell): `./configure.bat; if ($?) { ./build.bat }`
Expected: configure descarga ImGuiColorTextEdit, build sin errores (el target aún no se usa desde ningún `.cpp`, solo valida que compila el `.cpp` de la librería).

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt engine/CMakeLists.txt
git commit -m "feat(script): añade ImGuiColorTextEdit vía FetchContent"
```

---

### Task 2: FileManager — helpers de texto plano

**Files:**
- Modify: `engine/include/DonTopo/FileManager.h`
- Modify: `engine/src/FileManager.cpp`

**Interfaces:**
- Produces: `static std::optional<std::string> FileManager::readText(const std::string& path)`, `static bool FileManager::writeText(const std::string& path, const std::string& content)`.

- [ ] **Step 1: Declarar los métodos en el header**

En `engine/include/DonTopo/FileManager.h`, añadir dentro de `class FileManager`, tras `readJson`:

```cpp
            // Lee el contenido completo de path como texto plano. std::nullopt
            // si el fichero no existe o no se pudo abrir.
            static std::optional<std::string> readText(const std::string& path);

            // Escribe content en path, reemplazando cualquier contenido previo.
            // false si el fichero no se pudo abrir/escribir.
            static bool writeText(const std::string& path, const std::string& content);
```

- [ ] **Step 2: Implementar en `FileManager.cpp`**

En `engine/src/FileManager.cpp`, añadir tras `readJson` (necesita `<sstream>`):

```cpp
    std::optional<std::string> FileManager::readText(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return std::nullopt;

        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool FileManager::writeText(const std::string& path, const std::string& content)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
            return false;

        out << content;
        return out.good();
    }
```

Añadir `#include <sstream>` al principio del fichero, junto a `#include <fstream>`.

- [ ] **Step 3: Compilar**

Run (PowerShell): `./build.bat`
Expected: sin errores (no hay cambios de CMake, no hace falta reconfigure).

- [ ] **Step 4: Smoke test manual temporal**

Añadir temporalmente al final de `engine/src/Engine.cpp`:

```cpp
#include "DonTopo/FileManager.h"
namespace { [[maybe_unused]] void dtFileManagerTextSmoke() {
    DonTopo::FileManager::writeText("smoke_test.txt", "hola");
    auto r = DonTopo::FileManager::readText("smoke_test.txt");
    if (!r || *r != "hola") std::abort();
} }
```

Run: `./build.bat` → sin errores. Ejecutar `./build-ninja/sandbox/Sandbox.exe` brevemente y cerrarlo — si `dtFileManagerTextSmoke` no se llama en runtime no verifica nada por sí solo, así que en su lugar llamarla una vez desde el constructor de `Engine` (temporalmente) para forzar la ejecución, confirmar que la app no aborta al arrancar, y luego **revertir el cambio de `Engine.cpp` y borrar `smoke_test.txt`**.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/FileManager.h engine/src/FileManager.cpp
git commit -m "feat(script): añade FileManager::readText/writeText"
```

---

### Task 3: Clase `ScriptEditorPanel` (core, aislada)

**Files:**
- Create: `engine/include/DonTopo/ScriptEditorPanel.h`
- Create: `engine/src/ScriptEditorPanel.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/ScriptEditorPanel.cpp` a la lista de sources, tras `src/Gizmos.cpp`)

**Interfaces:**
- Consumes: `FileManager::readText`/`writeText` (Task 2).
- Produces: `class ScriptEditorPanel { void openFile(const std::filesystem::path&); void draw(); };` — usado por `EditorUI` (Task 4).

- [ ] **Step 1: Crear el header**

`engine/include/DonTopo/ScriptEditorPanel.h`:

```cpp
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <TextEditor.h>

namespace DonTopo {

// Panel dockeable con tabs de ficheros .lua abiertos para edición manual.
// No conoce ScriptManager ni GameObject — solo lee/escribe texto en disco
// (FileManager). La recarga en la VM Lua la hace ScriptManager::pollChanges
// por su cuenta (mtime), este panel nunca llama loadScript.
class ScriptEditorPanel {
public:
    // No-op si path ya está abierto en alguna tab (esa tab pasa a tener foco).
    void openFile(const std::filesystem::path& path);
    void draw();
    // Fallos de lectura/escritura se reportan aquí en vez de silenciarse
    // (spec: deben verse en el Log Console del editor, pero este panel no
    // conoce EditorUI — EditorUI inyecta pushLog vía este callback).
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }

private:
    struct Tab {
        std::filesystem::path path;
        TextEditor editor;
        bool dirty = false;
    };

    void saveTab(Tab& tab);
    void log(const std::string& msg) { if (m_log) m_log(msg); }

    std::vector<Tab> m_tabs;
    // Índice de tab a enfocar en el próximo draw() (-1 = ninguno); se consume
    // (vuelve a -1) tras cada frame.
    int m_focusIndex = -1;
    // Índice de tab con el popup "cambios sin guardar" pendiente (-1 = ninguno).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    std::function<void(const std::string&)> m_log;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear la implementación**

`engine/src/ScriptEditorPanel.cpp`:

```cpp
#include "DonTopo/ScriptEditorPanel.h"
#include "DonTopo/FileManager.h"
#include <imgui.h>

namespace DonTopo {

void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    for (size_t i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs[i].path == path)
        {
            m_focusIndex = static_cast<int>(i);
            return;
        }
    }

    std::optional<std::string> content = FileManager::readText(path.string());
    if (!content)
    {
        log("Script Editor: no se pudo abrir '" + path.string() + "'");
        return;
    }

    Tab tab;
    tab.path = path;
    tab.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    tab.editor.SetText(*content);
    m_tabs.push_back(std::move(tab));
    m_focusIndex = static_cast<int>(m_tabs.size()) - 1;
}

void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
        tab.dirty = false;
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");
}

void ScriptEditorPanel::draw()
{
    ImGui::Begin("Script Editor");

    int closeRequested = -1;

    if (ImGui::BeginTabBar("##ScriptEditorTabs", ImGuiTabBarFlags_Reorderable))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            Tab& tab = m_tabs[i];
            std::string title = tab.path.filename().string() + (tab.dirty ? " *" : "");
            ImGuiTabItemFlags flags = (m_focusIndex == i) ? ImGuiTabItemFlags_SetSelected
                                                           : ImGuiTabItemFlags_None;
            bool open = true;

            ImGui::PushID(i);
            if (ImGui::BeginTabItem(title.c_str(), &open, flags))
            {
                if (ImGui::Button("Save"))
                    saveTab(tab);

                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
                    saveTab(tab);

                tab.editor.Render("##TextEditor", ImGui::GetContentRegionAvail());
                if (tab.editor.IsTextChanged())
                    tab.dirty = true;

                ImGui::EndTabItem();
            }
            ImGui::PopID();

            if (!open)
                closeRequested = i;
        }
        ImGui::EndTabBar();
    }
    m_focusIndex = -1;

    if (closeRequested >= 0)
    {
        if (m_tabs[closeRequested].dirty)
        {
            m_closeConfirmIndex = closeRequested;
            m_openCloseConfirmPopup = true;
        }
        else
            m_tabs.erase(m_tabs.begin() + closeRequested);
    }

    if (m_openCloseConfirmPopup)
    {
        ImGui::OpenPopup("Cambios sin guardar##ScriptEditor");
        m_openCloseConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Cambios sin guardar##ScriptEditor", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        Tab& tab = m_tabs[m_closeConfirmIndex];
        ImGui::Text("'%s' tiene cambios sin guardar.", tab.path.filename().string().c_str());

        if (ImGui::Button("Guardar"))
        {
            saveTab(tab);
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Descartar"))
        {
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar"))
        {
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace DonTopo
```

- [ ] **Step 3: Añadir el nuevo `.cpp` a `engine/CMakeLists.txt`**

En la lista de sources de `add_library(DonTopoEngine STATIC ...)` (líneas 1-30), añadir tras `src/Gizmos.cpp`:

```cmake
    src/ScriptEditorPanel.cpp
```

- [ ] **Step 4: Compilar**

Run (PowerShell): `./build.bat`
Expected: sin errores. (La clase aún no se instancia desde ningún sitio — solo valida que compila standalone; Task 4 la conecta.)

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/ScriptEditorPanel.h engine/src/ScriptEditorPanel.cpp engine/CMakeLists.txt
git commit -m "feat(script): añade ScriptEditorPanel (editor multi-tab con ImGuiColorTextEdit)"
```

---

### Task 4: Wiring en `EditorUI`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h` (forward decl + miembro, líneas ~14-27 y ~300-304)
- Modify: `engine/src/EditorUI.cpp` (constructor línea 209-214, `draw()` líneas 218-231)

**Interfaces:**
- Consumes: `ScriptEditorPanel::openFile`/`draw` (Task 3).
- Produces: `EditorUI` expone `m_scriptEditor` (privado, `std::unique_ptr<ScriptEditorPanel>`) usado por Tasks 5-7.

- [ ] **Step 1: Forward declaration en el header**

En `engine/include/DonTopo/EditorUI.h`, añadir junto a las demás forward decls (tras `class ScriptManager;`, línea 27):

```cpp
class ScriptEditorPanel;
```

- [ ] **Step 2: Miembro en el header**

Añadir tras `ScriptManager*  m_scriptManager = nullptr;` (línea 304):

```cpp
    // Panel de edición de código .lua (Task: Script Editor Panel). unique_ptr
    // + forward declaration para no arrastrar <TextEditor.h>/<imgui.h> a todo
    // el que incluya EditorUI.h — mismo patrón que m_meshFileDialog.
    std::unique_ptr<ScriptEditorPanel> m_scriptEditor;
```

- [ ] **Step 3: Construcción en `EditorUI.cpp`**

Añadir `#include "DonTopo/ScriptEditorPanel.h"` a los includes del principio de `engine/src/EditorUI.cpp` (junto a los demás `#include "DonTopo/..."`).

Modificar el constructor (líneas 209-214) para construir el panel y engancharlo al Log Console (mismo `pushLog` que usa el resto del editor):

```cpp
EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_scriptEditor(std::make_unique<ScriptEditorPanel>())
{
    m_scriptEditor->setLogCallback([this](const std::string& msg) { pushLog(msg); });
}
```

- [ ] **Step 4: Dibujarlo en `draw()`**

Modificar `EditorUI::draw` (líneas 218-231), añadir la llamada al final:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawLogPanel();
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
    m_scriptEditor->draw();
}
```

- [ ] **Step 5: Compilar y verificar visualmente**

Run (PowerShell): `./build.bat`
Expected: sin errores.

Run: `./build-ninja/sandbox/Sandbox.exe` — debe aparecer un panel dockeado "Script Editor" vacío (sin tabs). Cerrar la app.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(script): integra ScriptEditorPanel en EditorUI"
```

---

### Task 5: Content Browser — doble-click abre `.lua`

**Files:**
- Modify: `engine/src/EditorUI.cpp:2163-2170` (dentro de `drawContentBrowser`)

**Interfaces:**
- Consumes: `m_scriptEditor->openFile(const std::filesystem::path&)` (Task 4).

- [ ] **Step 1: Añadir la rama de doble-click para ficheros `.lua`**

En `drawContentBrowser`, justo después del bloque existente:

```cpp
            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_dlgReopenPath = path.string();
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen    = false;
                m_currentDir = path.string();
                m_scanned    = false;
            }
```

añadir:

```cpp
            if (!isDir && ext == ".lua" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_scriptEditor->openFile(path);
            }
```

- [ ] **Step 2: Compilar y verificar visualmente**

Run (PowerShell): `./build.bat`

Run: `./build-ninja/sandbox/Sandbox.exe` → Content Browser → doble-click en `Scripts/Rotator.lua` (o `Test.lua`) → debe abrir una tab en "Script Editor" con el contenido del fichero y resaltado de sintaxis Lua. Cerrar la app.

- [ ] **Step 3: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(script): doble-click en .lua del Content Browser abre Script Editor"
```

---

### Task 6: Properties → Scripts — botón "Edit"

**Files:**
- Modify: `engine/src/EditorUI.cpp:1750-1756` (dentro de `drawScriptsSection`)

**Interfaces:**
- Consumes: `m_scriptManager->scriptsDirPath()` (ya existente), `m_scriptEditor->openFile` (Task 4).

- [ ] **Step 1: Añadir el botón "Edit" junto al "x" de cada ScriptComponent**

Reemplazar:

```cpp
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((comp->scriptName + " (Script)").c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (ImGui::SmallButton("x"))
            toRemove = comp;
```

por:

```cpp
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((comp->scriptName + " (Script)").c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 65.0f);
        if (ImGui::SmallButton("Edit"))
            m_scriptEditor->openFile(m_scriptManager->scriptsDirPath() / (comp->scriptName + ".lua"));
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (ImGui::SmallButton("x"))
            toRemove = comp;
```

- [ ] **Step 2: Compilar y verificar visualmente**

Run (PowerShell): `./build.bat`

Run: `./build-ninja/sandbox/Sandbox.exe` → seleccionar un GameObject con `ScriptComponent` → Properties → botón "Edit" → abre tab en Script Editor con ese script. Cerrar la app.

- [ ] **Step 3: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(script): botón Edit en sección Scripts de Properties abre Script Editor"
```

---

### Task 7: Popup "Nuevo Script" — abre tab tras crear

**Files:**
- Modify: `engine/src/EditorUI.cpp:2018-2020` (dentro de `drawNewScriptPopup`)

**Interfaces:**
- Consumes: `m_scriptEditor->openFile` (Task 4).

- [ ] **Step 1: Abrir la tab tras un `loadScript` exitoso**

Reemplazar:

```cpp
                if (m_scriptManager->loadScript(path))
                {
                    // El GameObject pudo borrarse mientras el popup estaba
                    // abierto — comprobar que sigue vivo antes de añadir.
                    bool targetAlive = false;
```

por:

```cpp
                if (m_scriptManager->loadScript(path))
                {
                    m_scriptEditor->openFile(path);

                    // El GameObject pudo borrarse mientras el popup estaba
                    // abierto — comprobar que sigue vivo antes de añadir.
                    bool targetAlive = false;
```

- [ ] **Step 2: Compilar y verificar visualmente**

Run (PowerShell): `./build.bat`

Run: `./build-ninja/sandbox/Sandbox.exe` → seleccionar GameObject → Add → Script → Nuevo Script... → crear uno con nombre nuevo → debe abrirse automáticamente una tab en Script Editor con la plantilla generada. Cerrar la app.

- [ ] **Step 3: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(script): Nuevo Script abre automáticamente su tab en Script Editor"
```

---

### Task 8: Verificación manual completa + README

**Files:**
- Modify: `README.md` (sección `## Lua Scripting`, líneas 84-113)

**Interfaces:** Ninguna (task de documentación + checklist manual).

- [ ] **Step 1: Actualizar README**

En `README.md`, tras el párrafo (líneas 86-89):

```
Attach one or more `ScriptComponent`s to a GameObject via **Properties → Add → Script**
(or **Add → Script → Nuevo Script...** to scaffold a new `.lua` file from a template).
Editing a loaded script while the engine is running hot-reloads it (~1s polling),
preserving serializable property values.
```

añadir un párrafo nuevo:

```
Double-clicking a `.lua` file in the Content Browser (or the **Edit** button next to a
`ScriptComponent` in Properties) opens it in the **Script Editor** panel — a multi-tab
code editor (ImGuiColorTextEdit, Lua syntax highlighting) docked alongside the other
panels. `Ctrl+S` or the **Save** button writes the file to disk; the existing hot-reload
polling picks up the change like any external edit. Closing a tab with unsaved changes
prompts to save/discard/cancel.
```

- [ ] **Step 2: Checklist de verificación manual (GUI, no automatizable)**

Ejecutar `./build-ninja/sandbox/Sandbox.exe` y confirmar cada punto:

- [ ] Doble-click en `Scripts/Rotator.lua` (Content Browser) abre tab con resaltado Lua correcto.
- [ ] Botón "Edit" en Properties → Scripts abre la misma tab (foco, no duplicado) si ya estaba abierta.
- [ ] Editar texto y `Ctrl+S` guarda; el asterisco `*` del título desaparece.
- [ ] Tras guardar, esperar ~1-2s: el hot-reload existente (`pollChanges`) recoge el cambio (ver Log Console).
- [ ] Abrir 2+ ficheros simultáneamente, cambiar entre tabs sin perder contenido.
- [ ] Cerrar una tab con cambios sin guardar → aparece popup Guardar/Descartar/Cancelar; cada botón se comporta como se espera.
- [ ] Add → Script → Nuevo Script... abre automáticamente su tab con la plantilla.
- [ ] Abrir un fichero no-`.lua` (doble-click en `.png`/`.wav` en Content Browser) no abre el Script Editor (comportamiento sin cambios respecto a antes).

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(script): documenta Script Editor Panel en README"
```
