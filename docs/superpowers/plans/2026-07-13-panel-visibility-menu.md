# Panel Visibility + View Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un menú `View` nuevo (en un `MenuBar` que hoy no existe) permite reabrir cualquiera de los 6 paneles persistentes del editor (Scene, Viewport, Properties, Log, Content Browser, Script Editor) tras cerrarlos con su "x" de titlebar — hoy, una vez cerrados, no hay forma de recuperarlos.

**Architecture:** Cada panel gana un `bool` de visibilidad (`m_sceneOpen`, `m_viewportOpen`, etc. en `EditorUI`; `m_open` en `ScriptEditorPanel`, que ya es una clase separada). Cada método `drawX()` se guarda con `if (!m_xxxOpen) return;` antes de su `ImGui::Begin`, y ese `Begin` pasa `&m_xxxOpen` como `p_open` (así la "x" de la titlebar togglea el mismo flag). Un método nuevo `EditorUI::drawMenuBar()` usa `ImGui::BeginMainMenuBar()` con un único menú `View` que expone cada flag vía `ImGui::MenuItem(nombre, nullptr, &flag)` (ImGui renderiza el checkbox y lo togglea solo). `drawToolbar()`/`drawDockSpace()` cambian de `vp->Pos`/`vp->Size` a `vp->WorkPos`/`vp->WorkSize` para no solaparse con la franja que reserva el nuevo MenuBar. Sin clase `Panel` ni registro genérico — se sigue el patrón actual de miembros sueltos + un método por panel (spec: YAGNI, solo 6 paneles).

**Tech Stack:** C++20, Dear ImGui (ya integrado). Sin test framework (no gtest/ctest en el repo) — verificación por compilación + ejecución manual del editor.

## Global Constraints

- No hay gtest/ctest — cada tarea se verifica con `configure.bat`/`build.bat` vía **PowerShell** (no Bash), y `.\build-ninja\sandbox\Sandbox.exe` para la verificación manual final (Tarea 4).
- Sin clase base `Panel` ni colección de paneles — cada panel sigue siendo un método privado de `EditorUI` (o, para Script Editor, su clase ya existente `ScriptEditorPanel`), con su flag como miembro suelto. No introducir abstracciones nuevas.
- Patrón de guardado por panel: `if (!m_xxxOpen) return;` justo antes del `Begin`, y `Begin(nombre, &m_xxxOpen)` — sin envolver el cuerpo existente en `if (ImGui::Begin(...)) { ... }`. El fichero no usa ese patrón en ningún otro sitio (nunca se comprueba el valor de retorno de `Begin`); reindentar cuerpos de 100+ líneas para adoptarlo sería un diff grande sin beneficio funcional.
- Diálogos/popups modales (Mesh, AudioClip, Scene, New Script, rename/delete de assets) quedan fuera de alcance — no llevan flag de visibilidad ni entrada en `View`.
- Alcance: exactamente 6 paneles — Scene, Viewport, Properties, Log, Content Browser, Script Editor. Todos arrancan `open = true` (comportamiento actual sin cambios en el primer frame).
- Cerrar un panel solo lo oculta — nunca se destruye ni se pierde su estado interno (selección de Scene, tabs de Script Editor, cache de Properties, scroll de Log/Content Browser).

---

### Task 1: `EditorUI.h` — flags de visibilidad + declaración de `drawMenuBar`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h:80-82`
- Modify: `engine/include/DonTopo/EditorUI.h:166-167`

**Interfaces:**
- Produces: `EditorUI::m_sceneOpen`, `m_viewportOpen`, `m_propertiesOpen`, `m_logOpen`, `m_contentBrowserOpen` (todos `bool`, `= true`), y la declaración privada `void drawMenuBar();`. Task 4 los consume; Task 3 consume los 5 flags de `EditorUI` (no `drawMenuBar`).

- [ ] **Step 1: Declarar `drawMenuBar()` antes de `drawToolbar()`**

En `engine/include/DonTopo/EditorUI.h:80-82`, reemplaza:

```cpp
private:
    static constexpr float kToolbarHeight = 30.0f;
    void drawToolbar();
    void drawDockSpace();
```

por:

```cpp
private:
    static constexpr float kToolbarHeight = 30.0f;
    void drawMenuBar();
    void drawToolbar();
    void drawDockSpace();
```

- [ ] **Step 2: Añadir los 5 flags de visibilidad antes del bloque `// Viewport`**

En `engine/include/DonTopo/EditorUI.h:166-167`, reemplaza:

```cpp
    // Viewport
    bool m_viewportHovered = false;
```

por:

```cpp
    // Visibilidad de paneles — togglable desde el menú View (drawMenuBar).
    // Todos empiezan visibles; cerrar solo oculta la ventana ImGui, el
    // estado interno de cada panel (selección, scroll, tabs de Script
    // Editor...) no se pierde mientras está oculto.
    bool m_sceneOpen          = true;
    bool m_viewportOpen       = true;
    bool m_propertiesOpen     = true;
    bool m_logOpen            = true;
    bool m_contentBrowserOpen = true;

    // Viewport
    bool m_viewportHovered = false;
```

- [ ] **Step 3: Compilar**

En PowerShell:
```powershell
& .\build.bat
```
Expected: build limpio, sin errores (los flags nuevos no se usan todavía — es normal que MSVC no avise de "unused member" en C++).

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h
git commit -m "feat(editor): declara flags de visibilidad de paneles y drawMenuBar"
```

---

### Task 2: `ScriptEditorPanel` — visibilidad de ventana

**Files:**
- Modify: `engine/include/DonTopo/ScriptEditorPanel.h:14-24`
- Modify: `engine/include/DonTopo/ScriptEditorPanel.h:54-62`
- Modify: `engine/src/ScriptEditorPanel.cpp:105-107`
- Modify: `engine/src/ScriptEditorPanel.cpp:156-158`

**Interfaces:**
- Consumes: nada nuevo.
- Produces: `ScriptEditorPanel::GetOpenPtr() -> bool*` (público) — Task 4 lo usa en el `ImGui::MenuItem` de Script Editor.

- [ ] **Step 1: Exponer `GetOpenPtr()` en la interfaz pública**

En `engine/include/DonTopo/ScriptEditorPanel.h:14-24`, reemplaza:

```cpp
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
```

por:

```cpp
class ScriptEditorPanel {
public:
    // No-op si path ya está abierto en alguna tab (esa tab pasa a tener foco).
    // También reabre el panel si estaba cerrado (m_open = true) — abrir un
    // fichero desde Properties/Content Browser debe hacerlo visible.
    void openFile(const std::filesystem::path& path);
    void draw();
    // Fallos de lectura/escritura se reportan aquí en vez de silenciarse
    // (spec: deben verse en el Log Console del editor, pero este panel no
    // conoce EditorUI — EditorUI inyecta pushLog vía este callback).
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }
    // Puntero al flag de visibilidad de la ventana, usado por el checkbox
    // del menú View de EditorUI (ImGui::MenuItem togglea *bool directamente).
    bool* GetOpenPtr() { return &m_open; }

private:
```

- [ ] **Step 2: Añadir el flag `m_open`**

En `engine/include/DonTopo/ScriptEditorPanel.h:54-62`, reemplaza:

```cpp
    std::vector<Tab> m_tabs;
    // Índice de tab a enfocar en el próximo draw() (-1 = ninguno); se consume
    // (vuelve a -1) tras cada frame.
    int m_focusIndex = -1;
    // Índice de tab con el popup "cambios sin guardar" pendiente (-1 = ninguno).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    std::function<void(const std::string&)> m_log;
};
```

por:

```cpp
    std::vector<Tab> m_tabs;
    // Índice de tab a enfocar en el próximo draw() (-1 = ninguno); se consume
    // (vuelve a -1) tras cada frame.
    int m_focusIndex = -1;
    // Índice de tab con el popup "cambios sin guardar" pendiente (-1 = ninguno).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    // Visibilidad de la ventana del panel — togglable desde el menú View de
    // EditorUI vía GetOpenPtr(). No afecta a m_tabs: cerrar el panel solo
    // oculta la ventana, las tabs y su estado siguen en memoria.
    bool m_open = true;
    std::function<void(const std::string&)> m_log;
};
```

- [ ] **Step 3: Reabrir el panel al abrir un fichero**

En `engine/src/ScriptEditorPanel.cpp:105-107`, reemplaza:

```cpp
void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    // Canonicalizamos el path antes de comparar/guardar: los distintos call sites
```

por:

```cpp
void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    m_open = true;

    // Canonicalizamos el path antes de comparar/guardar: los distintos call sites
```

- [ ] **Step 4: Guardar `draw()` con el flag**

En `engine/src/ScriptEditorPanel.cpp:156-158`, reemplaza:

```cpp
void ScriptEditorPanel::draw()
{
    ImGui::Begin("Script Editor");
```

por:

```cpp
void ScriptEditorPanel::draw()
{
    if (!m_open) return;
    ImGui::Begin("Script Editor", &m_open);
```

- [ ] **Step 5: Compilar y verificar manualmente**

```powershell
& .\build.bat
& .\build-ninja\sandbox\Sandbox.exe
```
En el editor: cierra el panel "Script Editor" con la "x" de su titlebar. Expected: el panel desaparece. (Reabrirlo sin reiniciar la app llega en la Tarea 4 — por ahora solo hay que confirmar que cierra.)

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/ScriptEditorPanel.h engine/src/ScriptEditorPanel.cpp
git commit -m "feat(script-editor): panel cerrable via titlebar, reabre solo al abrir fichero"
```

---

### Task 3: `EditorUI` — guardar Scene/Viewport/Properties/Log/Content Browser

**Files:**
- Modify: `engine/src/EditorUI.cpp:370-372` (`drawScene`)
- Modify: `engine/src/EditorUI.cpp:894-897` (`drawViewport`)
- Modify: `engine/src/EditorUI.cpp:951-954` (`drawProperties`)
- Modify: `engine/src/EditorUI.cpp:354-356` (`drawLogPanel`)
- Modify: `engine/src/EditorUI.cpp:2059-2061` (`drawContentBrowser`)

**Interfaces:**
- Consumes: `m_sceneOpen`, `m_viewportOpen`, `m_propertiesOpen`, `m_logOpen`, `m_contentBrowserOpen` (Task 1).
- Produces: nada nuevo — cada `Begin` ya pasa su flag como `p_open`, listo pa que Task 4 los togglee desde `View`.

- [ ] **Step 1: `drawScene`**

En `engine/src/EditorUI.cpp:370-372`, reemplaza:

```cpp
void EditorUI::drawScene(GameObject* sceneRoot)
{
    ImGui::Begin("Scene");
```

por:

```cpp
void EditorUI::drawScene(GameObject* sceneRoot)
{
    if (!m_sceneOpen) return;
    ImGui::Begin("Scene", &m_sceneOpen);
```

- [ ] **Step 2: `drawViewport`**

En `engine/src/EditorUI.cpp:894-897`, reemplaza:

```cpp
void EditorUI::drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    ImGui::Begin("Viewport");
    m_viewportHovered = ImGui::IsWindowHovered();
```

por:

```cpp
void EditorUI::drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    if (!m_viewportOpen)
    {
        // Sin esto, cerrar Viewport dejaría m_viewportHovered en su último
        // valor (posiblemente true) y el mouse-look de la cámara en
        // sandbox/src/main.cpp seguiría respondiendo con el panel oculto.
        m_viewportHovered = false;
        return;
    }
    ImGui::Begin("Viewport", &m_viewportOpen);
    m_viewportHovered = ImGui::IsWindowHovered();
```

- [ ] **Step 3: `drawProperties`**

En `engine/src/EditorUI.cpp:951-954`, reemplaza:

```cpp
void EditorUI::drawProperties()
{
    ImGui::Begin("Properties");
    if (!m_selected)
```

por:

```cpp
void EditorUI::drawProperties()
{
    if (!m_propertiesOpen) return;
    ImGui::Begin("Properties", &m_propertiesOpen);
    if (!m_selected)
```

- [ ] **Step 4: `drawLogPanel`**

En `engine/src/EditorUI.cpp:354-356`, reemplaza:

```cpp
void EditorUI::drawLogPanel()
{
    ImGui::Begin("Log");
```

por:

```cpp
void EditorUI::drawLogPanel()
{
    if (!m_logOpen) return;
    ImGui::Begin("Log", &m_logOpen);
```

- [ ] **Step 5: `drawContentBrowser`**

En `engine/src/EditorUI.cpp:2059-2061`, reemplaza:

```cpp
void EditorUI::drawContentBrowser(GameObject* sceneRoot)
{
    ImGui::Begin("Content Browser");
```

por:

```cpp
void EditorUI::drawContentBrowser(GameObject* sceneRoot)
{
    if (!m_contentBrowserOpen) return;
    ImGui::Begin("Content Browser", &m_contentBrowserOpen);
```

- [ ] **Step 6: Compilar y verificar manualmente**

```powershell
& .\build.bat
& .\build-ninja\sandbox\Sandbox.exe
```
Cierra, uno por uno, Scene, Viewport, Properties, Log y Content Browser con su "x". Expected: cada uno desaparece al cerrarlo, sin crashear ni afectar a los demás. (Reabrirlos sin reiniciar llega en la Tarea 4.)

- [ ] **Step 7: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(editor): Scene/Viewport/Properties/Log/Content Browser cerrables via titlebar"
```

---

### Task 4: `View` menu — `MenuBar` nuevo, wiring y reposicionamiento

**Files:**
- Modify: `engine/src/EditorUI.cpp:221-224` (`draw`)
- Modify: `engine/src/EditorUI.cpp` — nuevo método `drawMenuBar()`, insertado justo antes de `drawToolbar` (línea 237 antes de este cambio)
- Modify: `engine/src/EditorUI.cpp:239-241` (`drawToolbar`)
- Modify: `engine/src/EditorUI.cpp:328-331` (`drawDockSpace`)

**Interfaces:**
- Consumes: los 5 flags de `EditorUI` (Task 1) + `m_scriptEditor->GetOpenPtr()` (Task 2).
- Produces: `EditorUI::drawMenuBar()` implementado.

- [ ] **Step 1: Llamar `drawMenuBar()` primero en `draw()`**

En `engine/src/EditorUI.cpp:221-224`, reemplaza:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawToolbar();
    drawDockSpace();
```

por:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawMenuBar();
    drawToolbar();
    drawDockSpace();
```

- [ ] **Step 2: Implementar `drawMenuBar()`**

En `engine/src/EditorUI.cpp`, justo antes de `void EditorUI::drawToolbar()` (línea 237 antes de este cambio), inserta:

```cpp
void EditorUI::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Scene", nullptr, &m_sceneOpen);
            ImGui::MenuItem("Viewport", nullptr, &m_viewportOpen);
            ImGui::MenuItem("Properties", nullptr, &m_propertiesOpen);
            ImGui::MenuItem("Log", nullptr, &m_logOpen);
            ImGui::MenuItem("Content Browser", nullptr, &m_contentBrowserOpen);
            ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

```

(`ImGui::MenuItem(label, shortcut, bool*)` ya renderiza el checkbox y togglea el `bool` al clicar — sin lógica adicional.)

- [ ] **Step 3: Reposicionar `drawToolbar` bajo el `MenuBar`**

En `engine/src/EditorUI.cpp:239-241`, reemplaza:

```cpp
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kToolbarHeight));
```

por:

```cpp
    // vp->WorkPos/WorkSize (no vp->Pos/vp->Size) porque BeginMainMenuBar
    // reserva su franja restando de WorkPos/WorkSize del viewport principal
    // — así la Toolbar queda justo debajo del MenuBar en vez de solaparlo.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kToolbarHeight));
```

- [ ] **Step 4: Reposicionar `drawDockSpace` bajo el `MenuBar`**

En `engine/src/EditorUI.cpp:328-331`, reemplaza:

```cpp
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - kToolbarHeight));
    ImGui::SetNextWindowViewport(vp->ID);
```

por:

```cpp
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kToolbarHeight));
    ImGui::SetNextWindowViewport(vp->ID);
```

- [ ] **Step 5: Compilar**

```powershell
& .\build.bat
```
Expected: build limpio, sin errores.

- [ ] **Step 6: Verificación manual completa**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```

Comprobar, en orden:
1. Aparece una franja `MenuBar` nueva arriba del todo con un único menú `View`; la Toolbar (Play/Stop/Wireframe/Save/Load) queda justo debajo, sin solaparse.
2. Abrir `View` — 6 checkboxes (Scene, Viewport, Properties, Log, Content Browser, Script Editor), todos marcados.
3. Desmarcar "Log" desde `View` → el panel Log desaparece.
4. Volver a marcar "Log" desde `View` → reaparece en su posición docked previa.
5. Cerrar "Properties" con la "x" de su titlebar → desaparece y su checkbox en `View` se desmarca solo (mismo `bool`).
6. Reabrir "Properties" desde `View` → si había un GameObject seleccionado antes de cerrar, sus valores siguen mostrados igual (cache no se pierde).
7. Cerrar "Script Editor" desde `View`; hacer doble-click en un `.lua` del Content Browser → el panel se reabre solo con esa tab enfocada.
8. Cerrar "Viewport" desde `View`; mover el ratón sobre la zona donde estaba → la cámara no gira (sin mouse-look colgado en `m_viewportHovered`).
9. Repetir cierre/reapertura con "Scene" y "Content Browser" → cada uno reaparece en su posición docked previa.

- [ ] **Step 7: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(editor): menu View para reabrir paneles cerrados"
```
