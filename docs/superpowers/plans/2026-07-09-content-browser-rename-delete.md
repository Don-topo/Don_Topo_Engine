# Content Browser Rename/Delete Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Click derecho sobre un fichero o carpeta del grid derecho del Content Browser (`##AssetPane`) para Rename o Delete, con actualización automática de cualquier referencia que la escena tenga a ese recurso (mesh, audio clip, texturas de material).

**Architecture:** El grid derecho (código propio, ya renderiza todos los ficheros) se amplía para incluir subcarpetas y ganar un menú contextual por item. Se añade tracking de path a `Mesh` (que hoy no lo tiene) para poder localizar qué `GameObject` usa un `.fbx` dado. Rename y Delete comparten dos helpers de matching de paths (`samePath`/`pathUnderDir`) y recorren la escena vía `GameObject::traverse`. Delete de una textura en uso dispara un hot-swap de GPU (nuevo `Renderer::replaceStaticTextureWithMissing`) que reutiliza el generador de checkerboard "missing" que `GpuResources::createTextureImage` ya tiene para path vacío.

**Tech Stack:** C++20, Vulkan, ImGui, ImGuiFileDialog (embebido, sin dialog nativo), `std::filesystem`, CMake + Ninja (preset `debug`).

## Global Constraints

- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build (`cmake --build --preset debug`) + ejecutar `build-ninja/sandbox/Sandbox.exe` + revisar visualmente, igual que el resto del proyecto.
- Toda acción de right-click vive en el grid derecho (`##AssetPane`), nunca en el panel izquierdo (`IGFD::FileDialog::Instance()`, sin hook de extensión disponible sin vendorizar la librería).
- Rename de fichero solo edita el *stem* — la extensión queda fija, no editable (decisión ya confirmada con el usuario).
- El hot-swap de textura "missing" (Task 5) cubre solo meshes estáticos (`go->staticRenderIndex`), porque `Renderer::replaceStaticTextureWithMissing` indexa la lista de objetos estáticos y no existe equivalente skinned. **Nota posterior (rama `feature/animator-auto-skinned-import`):** la justificación original de este punto ("los meshes skinned no son alcanzables desde el Content Browser") ya NO es cierta — `loadMeshForSelected` usa `ModelLoader::loadAuto` y crea `SkinnedMesh` cuando el FBX trae rig. Desde entonces las tres funciones que recorren la escena —`countSceneReferences`, `detachSceneReferencesForDelete` y `updateSceneReferencesForRename`— sí recorren `SkinnedMesh::materials[]` vía el helper `materialsOf()` (ahí es donde `loadSkinned` deja los materiales, nunca en el `Mesh::material` heredado): el conteo de objetos afectados, la limpieza de paths al borrar y la reescritura de paths al renombrar cubren ya los personajes con rig. Lo único que sigue siendo solo-estático es el hot-swap de GPU.
- Si el rebuild incremental de Ninja no recompila un `.cpp` tras editar un header (síntoma: crash con puntero corrupto inmediatamente al arrancar), borrar `build-ninja/engine/CMakeFiles` y `build-ninja/sandbox/CMakeFiles` y reconstruir completo — ver memoria `ninja_stale_header_deps`. Esto es especialmente relevante en Task 1 (`Mesh.h`/`AudioClipComponent.h` cambian, y `Renderer.h` embebe `EditorUI` **por valor**, así que cualquier `.cpp` que incluya `Renderer.h` transitivamente debe recompilarse).
- Spec completo: `docs/superpowers/specs/2026-07-09-content-browser-rename-delete-design.md`.

---

### Task 1: Datos — `Mesh::sourcePath` y `AudioClipComponent::setPath`

**Files:**
- Modify: `engine/include/DonTopo/Mesh.h`
- Modify: `engine/src/EditorUI.cpp:325-349` (`loadMeshForSelected`)
- Modify: `engine/include/DonTopo/AudioClipComponent.h`

**Interfaces:**
- Produces: `std::string Mesh::sourcePath` — vacío para meshes procedurales (Cube/Sphere/Plane/Capsule), poblado con el path del `.fbx` de origen para meshes cargados vía `loadMeshForSelected`.
- Produces: `void AudioClipComponent::setPath(const std::string& path)` — actualiza `m_path` sin recargar el sonido FMOD (mismo contenido, solo bookkeeping).

Este task no cambia ningún comportamiento visible — `sourcePath` no se lee desde ningún sitio hasta Task 3. Verificación = solo compila + smoke test de que cargar un mesh/audio sigue funcionando igual que antes.

- [ ] **Step 1: Añadir `sourcePath` a `Mesh`**

En `engine/include/DonTopo/Mesh.h`, reemplazar el contenido completo por:

```cpp
#pragma once
#include <vector>
#include <string>
#include "DonTopo/Vertex.h"
#include "DonTopo/Material.h"

namespace DonTopo
{
    struct Mesh
    {
        std::string             name;
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        Material                material;
        // Path del .fbx de origen (vacío para meshes procedurales: Cube/Sphere/
        // Plane/Capsule creados desde "Basic Shapes"). Content Browser lo usa
        // para localizar qué GameObjects referencian un fichero al hacer
        // rename/delete.
        std::string             sourcePath;

        virtual ~Mesh() = default;
    };
}
```

- [ ] **Step 2: Poblar `sourcePath` al cargar un mesh**

En `engine/src/EditorUI.cpp:340`, reemplazar:

```cpp
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
```

por:

```cpp
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        mesh->sourcePath = path;
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
```

- [ ] **Step 3: Añadir `setPath` a `AudioClipComponent`**

En `engine/include/DonTopo/AudioClipComponent.h`, reemplazar:

```cpp
    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }
```

por:

```cpp
    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }
    // Actualiza solo el bookkeeping del path (ej. tras un rename en disco);
    // el sonido FMOD ya cargado no cambia de contenido, no hace falta reload.
    void setPath(const std::string& path) { m_path = path; }
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

Si el build incremental falla de forma extraña (crash al arrancar `Sandbox.exe` con puntero corrupto, no error de compilación), borrar `build-ninja/engine/CMakeFiles` y `build-ninja/sandbox/CMakeFiles` y reconstruir — ver Global Constraints.

- [ ] **Step 5: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Cargar un `.fbx` en un GameObject (Browse o drag&drop) — sigue renderizando igual que antes.
2. Cargar un audio en un GameObject (Browse o drag&drop) — sigue reproduciéndose igual que antes.
3. Sin crashes, sin validation errors de Vulkan.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Mesh.h engine/src/EditorUI.cpp engine/include/DonTopo/AudioClipComponent.h
git commit -m "feat(assets): trackear sourcePath de Mesh y setPath de AudioClipComponent"
```

---

### Task 2: Grid derecho — carpetas y navegación

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h` (nuevo campo `m_dlgReopenPath`)
- Modify: `engine/src/EditorUI.cpp:1271-1398` (`drawContentBrowser`)

**Interfaces:**
- Produces: `std::string EditorUI::m_dlgReopenPath` — path al que debe reabrir el diálogo IGFD la próxima vez que `!m_dlgOpen`; vacío = reabrir en `m_projectRoot` (comportamiento actual). Consumido por Task 3/4 para reabrir en la carpeta actual tras rename/delete en vez de resetear a la raíz.

Sin este campo, cerrar+reabrir el diálogo (para forzar refresco tras un rename/delete, o al navegar por doble-click en una carpeta del grid) rompería el clamp de raíz existente: reabriría siempre en `m_projectRoot`, perdiendo la navegación a la subcarpeta actual.

- [ ] **Step 1: Añadir `m_dlgReopenPath` a `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h:112-113`, reemplazar:

```cpp
    std::filesystem::path m_projectRoot;
    std::vector<std::filesystem::path> m_assets;
```

por:

```cpp
    std::filesystem::path m_projectRoot;
    // Path al que reabrir el diálogo IGFD la próxima vez que !m_dlgOpen;
    // vacío = reabrir en m_projectRoot. Se consume (se vacía) en cada
    // reapertura — quien quiera reabrir en una carpeta concreta debe
    // asignarlo de nuevo antes de poner m_dlgOpen = false.
    std::string m_dlgReopenPath;
    std::vector<std::filesystem::path> m_assets;
```

- [ ] **Step 2: Usar `m_dlgReopenPath` al reabrir el diálogo**

En `engine/src/EditorUI.cpp:1284-1296`, reemplazar:

```cpp
        if (!m_dlgOpen) {
            IGFD::FileDialogConfig cfg;
            cfg.path  = m_projectRoot.string();
            cfg.flags = ImGuiFileDialogFlags_NoDialog |
                        ImGuiFileDialogFlags_DontShowHiddenFiles |
                        ImGuiFileDialogFlags_HideColumnType |
                        ImGuiFileDialogFlags_HideColumnDate |
                        ImGuiFileDialogFlags_DisableThumbnailMode |
                        ImGuiFileDialogFlags_DisablePlaceMode;
            IGFD::FileDialog::Instance()->OpenDialog(
                "##ContentDlg", "Files", ".*", cfg);
            m_dlgOpen = true;
        }
```

por:

```cpp
        if (!m_dlgOpen) {
            IGFD::FileDialogConfig cfg;
            cfg.path  = m_dlgReopenPath.empty() ? m_projectRoot.string() : m_dlgReopenPath;
            cfg.flags = ImGuiFileDialogFlags_NoDialog |
                        ImGuiFileDialogFlags_DontShowHiddenFiles |
                        ImGuiFileDialogFlags_HideColumnType |
                        ImGuiFileDialogFlags_HideColumnDate |
                        ImGuiFileDialogFlags_DisableThumbnailMode |
                        ImGuiFileDialogFlags_DisablePlaceMode;
            IGFD::FileDialog::Instance()->OpenDialog(
                "##ContentDlg", "Files", ".*", cfg);
            m_dlgOpen = true;
            m_dlgReopenPath.clear();
        }
```

- [ ] **Step 3: Asegurar que el clamp de raíz reabre en la raíz (no en `m_dlgReopenPath` stale)**

En `engine/src/EditorUI.cpp:1315-1318`, reemplazar:

```cpp
            if (!insideRoot) {
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen = false;
            }
```

por:

```cpp
            if (!insideRoot) {
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen = false;
                m_dlgReopenPath.clear();
            }
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 5: Verificación manual del clamp (regresión)**

Run: `build-ninja/sandbox/Sandbox.exe`

1. En el panel izquierdo, navegar hacia arriba (`..`) intentando salir de la raíz del proyecto — sigue clampeando a la raíz igual que antes (regresión check, este task no debería cambiar este comportamiento).

- [ ] **Step 6: Incluir carpetas en el scan del grid**

En `engine/src/EditorUI.cpp:1335-1343`, reemplazar:

```cpp
        if (!m_scanned) {
            m_assets.clear();
            if (std::filesystem::exists(m_currentDir))
                for (auto& e : std::filesystem::directory_iterator(m_currentDir))
                    if (e.is_regular_file())
                        m_assets.push_back(e.path());
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }
```

por:

```cpp
        if (!m_scanned) {
            m_assets.clear();
            if (std::filesystem::exists(m_currentDir))
                for (auto& e : std::filesystem::directory_iterator(m_currentDir))
                    if (e.is_regular_file() || e.is_directory())
                        m_assets.push_back(e.path());
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }
```

- [ ] **Step 7: Renderizar carpetas en el grid con icono propio y navegación por doble-click**

En `engine/src/EditorUI.cpp:1354-1393`, reemplazar el bloque completo del `for`:

```cpp
        for (auto& path : m_assets) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac") {
                btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG";
            } else if (ext == ".spv") {
                btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV";
            } else {
                btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "...";
            }

            ImGui::PushID(path.string().c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);

            if (kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
```

por:

```cpp
        for (auto& path : m_assets) {
            bool isDir = std::filesystem::is_directory(path);
            std::string ext = isDir ? "" : path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (isDir) {
                btnColor = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); label = "DIR";
            } else if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac") {
                btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG";
            } else if (ext == ".spv") {
                btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV";
            } else {
                btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "...";
            }

            ImGui::PushID(path.string().c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);

            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_dlgReopenPath = path.string();
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen    = false;
                m_currentDir = path.string();
                m_scanned    = false;
            }

            if (!isDir && kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
```

- [ ] **Step 8: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 9: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. El grid derecho muestra ahora también las subcarpetas de `assets/` (icono gris, label "DIR"), junto a los ficheros.
2. Doble-click sobre una carpeta del grid → el grid y el panel izquierdo navegan dentro de ella (se ven sus ficheros/subcarpetas).
3. Doble-click sobre una subcarpeta anidada (2+ niveles) → sigue navegando correctamente.
4. Tras navegar por doble-click, intentar subir por encima de la raíz del proyecto desde el panel izquierdo (`..`) → sigue clampeando a la raíz (no se rompió el clamp de Task anterior).
5. Sin crashes, sin validation errors.

- [ ] **Step 10: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): mostrar carpetas en el grid del Content Browser con navegación"
```

---

### Task 3: Rename de assets (fichero y carpeta)

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp` (helpers, `draw`, `drawContentBrowser`)

**Interfaces:**
- Consumes: `Mesh::sourcePath`, `AudioClipComponent::setPath` (Task 1); `m_dlgReopenPath` (Task 2).
- Produces: `void EditorUI::beginAssetRename(const std::filesystem::path& path, bool isDir)`, `void EditorUI::updateSceneReferencesForRename(GameObject* sceneRoot, const std::filesystem::path& oldPath, const std::filesystem::path& newPath, bool isDir)` — consumidos por Task 4 (Delete reutiliza los helpers de matching `samePath`/`pathUnderDir` definidos aquí).
- Produces: `drawContentBrowser` pasa a requerir `GameObject* sceneRoot` como parámetro — `draw()` debe pasarlo.

- [ ] **Step 1: Cambiar la firma de `drawContentBrowser` para recibir `sceneRoot`**

En `engine/include/DonTopo/EditorUI.h:99`, reemplazar:

```cpp
    void drawContentBrowser();
```

por:

```cpp
    void drawContentBrowser(GameObject* sceneRoot);
```

En `engine/src/EditorUI.cpp:125`, reemplazar:

```cpp
    drawContentBrowser();
```

por:

```cpp
    drawContentBrowser(sceneRoot);
```

En `engine/src/EditorUI.cpp:1271`, reemplazar:

```cpp
void EditorUI::drawContentBrowser()
{
```

por:

```cpp
void EditorUI::drawContentBrowser(GameObject* sceneRoot)
{
```

- [ ] **Step 2: Compilar (solo el cambio de firma)**

Run: `cmake --build --preset debug`
Expected: build termina sin error (parámetro `sceneRoot` sin usar todavía dentro de la función es aceptable en este punto intermedio — se usa en el Step 6).

- [ ] **Step 3: Helpers de matching de paths y validación de nombre**

En `engine/src/EditorUI.cpp`, dentro del `namespace { ... }` anónimo (después del cierre de `moveGameObject`, línea 101, antes de `} // namespace` en línea 103), añadir:

```cpp
// Compara dos paths de forma robusta a mayúsc/minúsc (Windows es
// case-insensitive pero std::filesystem::path::operator== no lo es) y a
// formato relativo/absoluto (weakly_canonical antes de comparar).
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ecA, ecB;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ecA);
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ecB);
    std::string sa = (ecA ? a : ca).string();
    std::string sb = (ecB ? b : cb).string();
    std::transform(sa.begin(), sa.end(), sa.begin(), ::tolower);
    std::transform(sb.begin(), sb.end(), sb.begin(), ::tolower);
    return sa == sb;
}

// true si p está estrictamente dentro de dir (p == dir cuenta como false).
bool pathUnderDir(const std::filesystem::path& p, const std::filesystem::path& dir)
{
    std::error_code ecP, ecD;
    std::filesystem::path cp = std::filesystem::weakly_canonical(p, ecP);
    std::filesystem::path cd = std::filesystem::weakly_canonical(dir, ecD);
    std::string sp = (ecP ? p : cp).string();
    std::string sd = (ecD ? dir : cd).string();
    std::transform(sp.begin(), sp.end(), sp.begin(), ::tolower);
    std::transform(sd.begin(), sd.end(), sd.begin(), ::tolower);
    if (sp.size() <= sd.size() || sp.compare(0, sd.size(), sd) != 0)
        return false;
    char sep = sp[sd.size()];
    return sep == '\\' || sep == '/';
}

// Sustituye el prefijo oldDir por newDir en original. Asume que
// pathUnderDir(original, oldDir) ya dio true.
std::string replacePathPrefix(const std::string& original,
                               const std::filesystem::path& oldDir,
                               const std::filesystem::path& newDir)
{
    std::string oldStr = oldDir.string();
    return newDir.string() + original.substr(oldStr.size());
}

// Nombre de fichero/carpeta válido: no vacío tras trim, sin separadores de
// path ni caracteres reservados de Windows.
bool isValidFileName(const std::string& name)
{
    if (name.empty())
        return false;
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
        if (kReserved.find(c) != std::string::npos)
            return false;
    return true;
}
```

- [ ] **Step 4: Declarar estado y métodos nuevos en `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h:99`, reemplazar (ya tiene la firma nueva del Step 1):

```cpp
    void drawContentBrowser(GameObject* sceneRoot);
```

por:

```cpp
    void drawContentBrowser(GameObject* sceneRoot);
    // Arma el popup modal "Rename Asset" precargado con el nombre actual de
    // path (stem si es fichero, nombre completo si es carpeta).
    void beginAssetRename(const std::filesystem::path& path, bool isDir);
    // Recorre sceneRoot actualizando Mesh::sourcePath, los 3 paths de
    // Material y AudioClipComponent::getPath() que matcheen oldPath (exacto
    // si !isDir, por prefijo si isDir) al nuevo valor tras un rename en
    // disco ya realizado.
    void updateSceneReferencesForRename(GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath,
                                         bool isDir);
```

En `engine/include/DonTopo/EditorUI.h:112-114` (bloque ya modificado por Task 2), reemplazar:

```cpp
    std::filesystem::path m_projectRoot;
    // Path al que reabrir el diálogo IGFD la próxima vez que !m_dlgOpen;
    // vacío = reabrir en m_projectRoot. Se consume (se vacía) en cada
    // reapertura — quien quiera reabrir en una carpeta concreta debe
    // asignarlo de nuevo antes de poner m_dlgOpen = false.
    std::string m_dlgReopenPath;
    std::vector<std::filesystem::path> m_assets;
```

por:

```cpp
    std::filesystem::path m_projectRoot;
    // Path al que reabrir el diálogo IGFD la próxima vez que !m_dlgOpen;
    // vacío = reabrir en m_projectRoot. Se consume (se vacía) en cada
    // reapertura — quien quiera reabrir en una carpeta concreta debe
    // asignarlo de nuevo antes de poner m_dlgOpen = false.
    std::string m_dlgReopenPath;
    std::vector<std::filesystem::path> m_assets;

    // Asset rename — popup modal disparado por right-click > Rename en el
    // grid derecho del Content Browser.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;
```

- [ ] **Step 5: Implementar `beginAssetRename`**

En `engine/src/EditorUI.cpp`, añadir junto a `beginRename` (después de su cierre, línea 313):

```cpp
void EditorUI::beginAssetRename(const std::filesystem::path& path, bool isDir)
{
    m_assetRenameTarget = path;
    m_assetRenameIsDir  = isDir;
    m_assetRenameError.clear();
    std::string prefill = isDir ? path.filename().string() : path.stem().string();
    std::strncpy(m_assetRenameBuffer, prefill.c_str(), sizeof(m_assetRenameBuffer) - 1);
    m_assetRenameBuffer[sizeof(m_assetRenameBuffer) - 1] = '\0';
    m_openAssetRenamePopup = true;
}
```

- [ ] **Step 6: Implementar `updateSceneReferencesForRename`**

En `engine/src/EditorUI.cpp`, añadir justo después de `beginAssetRename`:

```cpp
void EditorUI::updateSceneReferencesForRename(GameObject* sceneRoot,
                                               const std::filesystem::path& oldPath,
                                               const std::filesystem::path& newPath,
                                               bool isDir)
{
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto updateField = [&](std::string& field)
        {
            if (field.empty()) return;
            bool matches = isDir ? pathUnderDir(field, oldPath) : samePath(field, oldPath);
            if (matches)
                field = isDir ? replacePathPrefix(field, oldPath, newPath) : newPath.string();
        };

        if (go->hasMesh())
        {
            Mesh* mesh = go->getMesh().get();
            updateField(mesh->sourcePath);
            updateField(mesh->material.texturePath);
            updateField(mesh->material.normalMapPath);
            updateField(mesh->material.metallicRoughnessPath);
        }
        if (go->hasAudioClip())
        {
            std::string audioPath = go->getAudioClip()->getPath();
            bool matches = isDir ? pathUnderDir(audioPath, oldPath) : samePath(audioPath, oldPath);
            if (matches)
            {
                std::string newAudioPath = isDir ? replacePathPrefix(audioPath, oldPath, newPath) : newPath.string();
                go->getAudioClip()->setPath(newAudioPath);
            }
        }
    });
}
```

- [ ] **Step 7: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error. `beginAssetRename`/`updateSceneReferencesForRename` no se llaman desde ningún sitio todavía (se usan en el Step 8) — warning de función no usada es esperado en este punto.

- [ ] **Step 8: Menú contextual "Rename" y popup modal en el grid**

En `engine/src/EditorUI.cpp`, dentro del loop de `drawContentBrowser` (bloque modificado por Task 2 Step 7), justo después del bloque de drag&drop (`if (!isDir && kDraggableExt.count(ext) && ImGui::BeginDragDropSource()) { ... }`) y antes de `std::string fname = path.filename().string();`, insertar:

```cpp
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                    beginAssetRename(path, isDir);
                ImGui::EndPopup();
            }
```

Después del cierre del `for` (`ImGui::Columns(1);`, antes de `ImGui::EndChild();` que cierra `##AssetPane`), añadir el popup modal (queda dentro del mismo bloque `{ ... }` de `##AssetPane`, se dibuja una vez por frame independientemente de cuántos items tenga el grid):

```cpp
        ImGui::Columns(1);

        if (m_openAssetRenamePopup)
        {
            ImGui::OpenPopup("Rename Asset");
            m_openAssetRenamePopup = false;
        }
        if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            bool enterPressed = ImGui::InputText("##assetRenameInput", m_assetRenameBuffer,
                                                  sizeof(m_assetRenameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetRenameIsDir)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_assetRenameTarget.extension().string().c_str());
            }
            if (!m_assetRenameError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetRenameError.c_str());
            ImGui::Separator();
            bool accept = ImGui::Button("Accept") || enterPressed;
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancel");

            if (accept)
            {
                std::string newStem = trim(m_assetRenameBuffer);
                if (!isValidFileName(newStem))
                {
                    m_assetRenameError = "Nombre invalido";
                }
                else
                {
                    std::string newName = m_assetRenameIsDir
                        ? newStem
                        : (newStem + m_assetRenameTarget.extension().string());
                    std::filesystem::path newPath = m_assetRenameTarget.parent_path() / newName;
                    std::error_code existsEc;
                    if (!samePath(newPath, m_assetRenameTarget) && std::filesystem::exists(newPath, existsEc))
                    {
                        m_assetRenameError = "Ya existe un fichero/carpeta con ese nombre";
                    }
                    else
                    {
                        std::error_code renameEc;
                        std::filesystem::rename(m_assetRenameTarget, newPath, renameEc);
                        if (renameEc)
                        {
                            m_assetRenameError = renameEc.message();
                        }
                        else
                        {
                            updateSceneReferencesForRename(sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned       = false;
                            m_dlgReopenPath = m_currentDir;
                            m_dlgOpen       = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
```

- [ ] **Step 9: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 10: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Right-click sobre un `.fbx` sin uso en la escena → menú con "Rename" → aceptar con un nombre nuevo → el fichero cambia de nombre en disco (verificar en el explorador de Windows o repitiendo el right-click), el grid se refresca mostrando el nuevo nombre.
2. Right-click sobre un `.fbx` que un GameObject tiene cargado como mesh → Rename → tras aceptar, el modelo sigue viéndose igual en el viewport (no se recarga geometría). Verificar que la referencia se actualizó: renombrar ese mismo fichero una segunda vez desde el grid — debe volver a encontrarlo (si `sourcePath` no se hubiera actualizado, este segundo rename seguiría funcionando igual porque el matching es por path del grid, no por `sourcePath` — para confirmar de verdad que `sourcePath` se actualizó, usar Task 4 después: borrar el `.fbx` renombrado y comprobar que el GameObject pierde el mesh).
3. Right-click sobre una carpeta → Rename → la carpeta cambia de nombre, el grid y panel izquierdo reflejan el cambio.
4. Rename de una carpeta que contiene un `.fbx`/audio en uso por algún GameObject → tras aceptar, sin crash; navegar dentro de la carpeta renombrada confirma que el contenido sigue ahí.
5. Intentar rename a un nombre ya existente en la misma carpeta → aparece "Ya existe un fichero/carpeta con ese nombre" en rojo, popup no se cierra, disco sin cambios.
6. Intentar rename dejando el campo vacío o con un carácter inválido (ej. `/`) → "Nombre invalido" en rojo, popup no se cierra.
7. Cancelar el popup → disco y escena sin cambios.
8. Rename de un fichero — confirmar que el campo de texto NO incluye la extensión (se ve como sufijo aparte, no editable).

- [ ] **Step 11: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): rename de assets (fichero y carpeta) desde Content Browser"
```

---

### Task 4: Delete de assets (fichero y carpeta)

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `samePath`/`pathUnderDir` (Task 3), `Mesh::sourcePath` (Task 1), `Renderer::removeMeshComponent(GameObject*)` (ya existente en el repo, `engine/include/DonTopo/Renderer.h:49`).
- Produces: `void EditorUI::beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)`, `int EditorUI::countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)`, `void EditorUI::detachSceneReferencesForDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)` — el bloque de limpieza de paths de textura dentro de `detachSceneReferencesForDelete` lo reemplaza/amplía Task 5 para el hot-swap de GPU.

En este task, borrar una textura en uso limpia su campo de `Material` a `""` (bookkeeping) pero **no** cambia lo que se ve en el viewport todavía — Task 5 añade el hot-swap visual. El resto del comportamiento (delete de mesh/audio en uso, delete de fichero/carpeta sin uso, delete de carpeta completa) queda completo en este task.

- [ ] **Step 1: Declarar estado y métodos nuevos en `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h`, reemplazar el bloque de Asset rename añadido en Task 3 Step 4:

```cpp
    // Asset rename — popup modal disparado por right-click > Rename en el
    // grid derecho del Content Browser.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;
```

por (añade el bloque de Asset delete a continuación):

```cpp
    // Asset rename — popup modal disparado por right-click > Rename en el
    // grid derecho del Content Browser.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;

    // Asset delete — popup modal disparado por right-click > Delete.
    std::filesystem::path m_assetDeleteTarget;
    bool                   m_assetDeleteIsDir = false;
    int                    m_assetDeleteAffectedCount = 0;
    bool                   m_openAssetDeletePopup = false;
```

En `engine/include/DonTopo/EditorUI.h`, reemplazar la declaración de `updateSceneReferencesForRename` añadida en Task 3 Step 4:

```cpp
    void updateSceneReferencesForRename(GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath,
                                         bool isDir);
```

por (añade las 3 declaraciones nuevas a continuación):

```cpp
    void updateSceneReferencesForRename(GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath,
                                         bool isDir);
    // Arma el popup modal "Delete Asset", precalculando cuántos GameObjects
    // referencian path (mesh o audio) para mostrarlo en el texto de aviso.
    void beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Cuenta cuántos GameObjects de sceneRoot referencian path (mesh o
    // audio; exacto si !isDir, por prefijo si isDir).
    int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Desengancha de la escena cualquier referencia a path antes de
    // borrarlo de disco: mesh en uso -> Renderer::removeMeshComponent;
    // audio en uso -> setAudioClip(nullptr); textura de Material en uso ->
    // limpia el campo de path (Task 5 añade el hot-swap de GPU aquí).
    void detachSceneReferencesForDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
```

- [ ] **Step 2: Implementar `countSceneReferences`, `detachSceneReferencesForDelete` y `beginAssetDelete`**

En `engine/src/EditorUI.cpp`, añadir justo después del cierre de `updateSceneReferencesForRename` (Task 3 Step 6):

```cpp
int EditorUI::countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return 0;

    int count = 0;
    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        bool meshMatches  = go->hasMesh() && matches(go->getMesh()->sourcePath);
        bool audioMatches = go->hasAudioClip() && matches(go->getAudioClip()->getPath());
        if (meshMatches || audioMatches)
            ++count;
    });
    return count;
}

void EditorUI::detachSceneReferencesForDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        if (go->hasMesh())
        {
            Mesh* mesh = go->getMesh().get();
            if (matches(mesh->sourcePath))
            {
                if (m_renderer)
                    m_renderer->removeMeshComponent(go);
            }
            else
            {
                if (matches(mesh->material.texturePath))            mesh->material.texturePath.clear();
                if (matches(mesh->material.normalMapPath))           mesh->material.normalMapPath.clear();
                if (matches(mesh->material.metallicRoughnessPath))   mesh->material.metallicRoughnessPath.clear();
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void EditorUI::beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    m_assetDeleteTarget         = path;
    m_assetDeleteIsDir          = isDir;
    m_assetDeleteAffectedCount  = countSceneReferences(sceneRoot, path, isDir);
    m_openAssetDeletePopup      = true;
}
```

- [ ] **Step 3: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error. Warning de función no usada esperado (se usan en el Step 4).

- [ ] **Step 4: Menú contextual "Delete" y popup modal de confirmación**

En `engine/src/EditorUI.cpp`, dentro del bloque de menú contextual añadido en Task 3 Step 8, reemplazar:

```cpp
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                    beginAssetRename(path, isDir);
                ImGui::EndPopup();
            }
```

por:

```cpp
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                    beginAssetRename(path, isDir);
                if (ImGui::MenuItem("Delete"))
                    beginAssetDelete(sceneRoot, path, isDir);
                ImGui::EndPopup();
            }
```

Justo después del popup modal `"Rename Asset"` añadido en Task 3 Step 8 (después de su `ImGui::EndPopup();` de cierre, todavía dentro del bloque `{ ... }` de `##AssetPane`), añadir:

```cpp
        if (m_openAssetDeletePopup)
        {
            ImGui::OpenPopup("Delete Asset");
            m_openAssetDeletePopup = false;
        }
        if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Borrar '%s'?", m_assetDeleteTarget.filename().string().c_str());
            if (m_assetDeleteAffectedCount > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "%d objeto(s) lo usan y perderan la referencia.", m_assetDeleteAffectedCount);
            ImGui::Separator();
            bool confirm = ImGui::Button("Borrar");
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancelar");

            if (confirm)
            {
                detachSceneReferencesForDelete(sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
                std::error_code removeEc;
                if (m_assetDeleteIsDir)
                    std::filesystem::remove_all(m_assetDeleteTarget, removeEc);
                else
                    std::filesystem::remove(m_assetDeleteTarget, removeEc);
                m_scanned       = false;
                m_dlgReopenPath = m_currentDir;
                m_dlgOpen       = false;
                ImGui::CloseCurrentPopup();
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
```

- [ ] **Step 5: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 6: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Right-click sobre un audio en uso por un GameObject → Delete → el modal muestra "1 objeto(s) lo usan..." → confirmar → el `AudioClipComponent` desaparece del GameObject (sección Properties deja de mostrarlo como asignado), fichero borrado de disco (comprobar que ya no aparece en el grid).
2. Right-click sobre un `.fbx` en uso → Delete → confirmar → el modelo desaparece del viewport, el GameObject sigue existiendo en el Scene panel, sección Mesh vuelve a mostrar "Browse...".
3. Right-click sobre un fichero sin uso → Delete → el modal NO muestra el texto de "objeto(s) lo usan" (count es 0) → confirmar → desaparece del disco/grid sin afectar la escena.
4. Right-click sobre una carpeta que contiene varios assets en uso → Delete → el modal cuenta correctamente todos los GameObjects afectados → confirmar → todas las referencias se limpian (meshes/audios desaparecen de sus GameObjects), la carpeta entera desaparece de disco.
5. Cancelar el modal (botón Cancelar) → disco y escena sin cambios.
6. Borrar una textura (`.png`) referenciada por un modelo cargado → el `Material.texturePath` se limpia (sin crash), el modelo sigue viéndose con la textura vieja en pantalla por ahora (comportamiento visual completo llega en Task 5) — confirmar solo que no crashea y que el fichero desaparece de disco.
7. Sin crashes, sin validation errors, en ningún escenario.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): delete de assets (fichero y carpeta) desde Content Browser"
```

---

### Task 5: Hot-swap de textura "missing" en `Renderer`

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `engine/src/Renderer.cpp`
- Modify: `engine/src/EditorUI.cpp` (`detachSceneReferencesForDelete`, Task 4)

**Interfaces:**
- Consumes: `GpuResources::createTextureImage/createTextureImageView/createTextureSampler` (ya existentes), `RenderObject::descriptorSets` (ya existente).
- Produces: `enum class Renderer::TextureSlot { Diffuse, Normal, MetallicRoughness }`, `void Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)`.

- [ ] **Step 1: Declarar `TextureSlot` y `replaceStaticTextureWithMissing` en `Renderer.h`**

En `engine/include/DonTopo/Renderer.h:47-49`, reemplazar:

```cpp
            // Quita solo el componente Mesh de go (no borra el GameObject ni sus
            // otros componentes). No-op si go es nullptr o no tiene mesh.
            void removeMeshComponent(GameObject* go);
```

por:

```cpp
            // Quita solo el componente Mesh de go (no borra el GameObject ni sus
            // otros componentes). No-op si go es nullptr o no tiene mesh.
            void removeMeshComponent(GameObject* go);
            enum class TextureSlot { Diffuse, Normal, MetallicRoughness };
            // Sustituye la textura del slot indicado por el checkerboard
            // "missing" (mismo generador que createTextureImage usa cuando no
            // hay path/bytes). No-op si renderIndex está fuera de rango.
            // Sincroniza con vkDeviceWaitIdle antes de tocar el descriptor
            // set (evita pisar un frame en vuelo). Solo cubre meshes
            // estáticos — no hay UI hoy que asigne meshes skinned.
            void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot);
```

- [ ] **Step 2: Implementar `replaceStaticTextureWithMissing`**

En `engine/src/Renderer.cpp`, añadir justo después del cierre de `removeMeshComponent` (línea 2105):

```cpp
    void Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)
    {
        if (renderIndex < 0 || renderIndex >= (int)m_objects.size()) return;
        RenderObject& obj = m_objects[renderIndex];

        // Evita tocar un descriptor set que un command buffer en vuelo
        // (double buffering) pudiera seguir referenciando.
        vkDeviceWaitIdle(m_gpu.device());

        VkImage*        img     = nullptr;
        VkDeviceMemory* mem     = nullptr;
        VkImageView*    view    = nullptr;
        VkSampler*      sampler = nullptr;
        VkFormat        format  = VK_FORMAT_R8G8B8A8_SRGB;
        uint32_t        binding = 1;

        switch (slot)
        {
            case TextureSlot::Diffuse:
                img = &obj.textureImage; mem = &obj.textureMem; view = &obj.textureView; sampler = &obj.sampler;
                format = VK_FORMAT_R8G8B8A8_SRGB; binding = 1;
                break;
            case TextureSlot::Normal:
                img = &obj.normalImage; mem = &obj.normalMem; view = &obj.normalView; sampler = &obj.normalSampler;
                format = VK_FORMAT_R8G8B8A8_UNORM; binding = 2;
                break;
            case TextureSlot::MetallicRoughness:
                img = &obj.ormImage; mem = &obj.ormMem; view = &obj.ormView; sampler = &obj.ormSampler;
                format = VK_FORMAT_R8G8B8A8_UNORM; binding = 4;
                break;
        }

        vkDestroySampler(m_gpu.device(),   *sampler, nullptr);
        vkDestroyImageView(m_gpu.device(), *view,    nullptr);
        vkDestroyImage(m_gpu.device(),     *img,     nullptr);
        vkFreeMemory(m_gpu.device(),       *mem,     nullptr);

        // path vacío + sin bytes embebidos = createTextureImage genera el
        // checkerboard "missing" de fallback (mismo camino que un modelo
        // cargado sin textura).
        m_res.createTextureImage("", {}, *img, *mem);
        m_res.createTextureImageView(*img, *view, format);
        m_res.createTextureSampler(*sampler);

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView   = *view;
            imageInfo.sampler     = *sampler;

            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = obj.descriptorSets[i];
            write.dstBinding      = binding;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &imageInfo;

            vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
        }
    }

```

- [ ] **Step 3: Compilar (solo el nuevo método, todavía sin uso)**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 4: Conectar el hot-swap al delete de texturas en `EditorUI::detachSceneReferencesForDelete`**

En `engine/src/EditorUI.cpp`, dentro de `detachSceneReferencesForDelete` (Task 4 Step 2), reemplazar:

```cpp
            else
            {
                if (matches(mesh->material.texturePath))            mesh->material.texturePath.clear();
                if (matches(mesh->material.normalMapPath))           mesh->material.normalMapPath.clear();
                if (matches(mesh->material.metallicRoughnessPath))   mesh->material.metallicRoughnessPath.clear();
            }
```

por:

```cpp
            else if (m_renderer && go->staticRenderIndex >= 0)
            {
                if (matches(mesh->material.texturePath))
                {
                    mesh->material.texturePath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Diffuse);
                }
                if (matches(mesh->material.normalMapPath))
                {
                    mesh->material.normalMapPath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Normal);
                }
                if (matches(mesh->material.metallicRoughnessPath))
                {
                    mesh->material.metallicRoughnessPath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::MetallicRoughness);
                }
            }
```

- [ ] **Step 5: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 6: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Cargar un modelo `.fbx` con textura diffuse externa (no embebida) en un GameObject — se ve con su textura normal.
2. Right-click sobre esa textura (`.png`/`.jpg`) en el grid → Delete → confirmar → el modelo pasa a mostrar el checkerboard gris "missing" en esa textura **al instante**, sin crash, sin validation errors de Vulkan (revisar consola de debug si hay validation layers activas).
3. Repetir con una textura normal map en uso → tras borrarla, el modelo sigue viéndose razonable (normal map "missing" es plano, no debería romper el shading de forma grotesca) sin crash.
4. Orbitar la cámara y mover el GameObject tras el hot-swap — sin crash, sin flicker raro.
5. Borrar un `.fbx` completo (no solo su textura) que esté en uso — sigue funcionando como en Task 4 (el mesh entero desaparece, no pasa por el hot-swap de textura).
6. Repetir el ciclo completo: cargar modelo → borrar su textura (missing) → cargar el mismo modelo en OTRO GameObject nuevo (drag&drop de un `.fbx` distinto o el mismo si aún existe) → confirmar que no hay corrupción de estado entre GameObjects.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp engine/src/EditorUI.cpp
git commit -m "feat(renderer): hot-swap de textura a checkerboard missing al borrar en uso"
```

---

## Self-Review

**Cobertura del spec:**
- Datos nuevos (`Mesh::sourcePath`, `AudioClipComponent::setPath`) → Task 1.
- Grid derecho con carpetas → Task 2.
- Menú contextual (right-click) → Task 3 Step 8 (Rename), Task 4 Step 4 (Delete).
- Rename completo (fichero/carpeta, validación, colisión, actualización de referencias mesh/audio/texturas) → Task 3.
- Delete completo (confirm modal con conteo, detach mesh/audio, limpieza de path de textura, fs remove/remove_all) → Task 4.
- Hot-swap de textura "missing" → Task 5.
- Refresco de UI tras rename/delete (`m_scanned=false`, `m_dlgReopenPath`) → Task 3 Step 8, Task 4 Step 4.
- Fuera de alcance (menú en panel izquierdo, batch, undo, shaders/skybox, skinned meshes en el hot-swap) → documentado en Global Constraints y en el spec, ningún task los implementa.

**Placeholders:** ninguno — todo paso de código muestra la implementación completa, ningún TODO/TBD.

**Consistencia de tipos:** `samePath`/`pathUnderDir`/`replacePathPrefix`/`isValidFileName` (Task 3 Step 3) se usan con la misma firma en Task 4 (`countSceneReferences`, `detachSceneReferencesForDelete`). `Renderer::TextureSlot` (Task 5 Step 1) se referencia como `Renderer::TextureSlot::Diffuse/Normal/MetallicRoughness` consistentemente en Task 5 Step 4. `drawContentBrowser(GameObject* sceneRoot)` (Task 3 Step 1) se usa con ese mismo parámetro en Task 3 Step 8 y Task 4 Step 4.
