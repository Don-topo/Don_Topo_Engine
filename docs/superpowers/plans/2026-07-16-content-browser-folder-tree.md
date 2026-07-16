# Content Browser Folder Tree Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sustituir el `ImGuiFileDialog` embebido del panel izquierdo del Content Browser por un árbol de carpetas propio, eliminando el footer (campo de nombre, combo de filtros, OK/Cancel), el breadcrumb, el botón de crear carpeta y la barra de búsqueda que IGFD dibuja siempre.

**Arquitectura:** Un método recursivo `drawFolderTree` pinta `ImGui::TreeNodeEx` por carpeta descendiendo desde `m_projectRoot`, apoyado en una función libre `listVisibleSubdirs` que lista subcarpetas filtrando ruido (`.git`, `build-ninja`). El panel derecho (grid de assets) no cambia: sigue leyendo `m_currentDir`, que ahora fija el árbol en vez de `IGFD::GetCurrentPath()`. IGFD permanece en los modales de `EditorUI` y `PropertiesPanel`.

**Tech Stack:** C++20, Dear ImGui, `std::filesystem`, CMake + Ninja + MSVC.

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-16-content-browser-folder-tree-design.md`.
- Build: `.\build.bat` desde la raíz del repo en PowerShell (envuelve `vcvarsall` + `cmake --build build-ninja`). No invocar `cmake` crudo desde Bash.
- Los tests son plain `main()` + macro `CHECK`, sin framework — mismo patrón que `engine/tests/physics_tests.cpp`. No añadir GTest/Catch.
- Toda llamada a `std::filesystem` que pueda tocar disco usa la sobrecarga con `std::error_code`. Nada de excepciones en el bucle de render.
- No tocar el rename/delete ni el drag&drop del grid derecho.
- Comentarios de código en español, como el resto del fichero.

---

### Task 1: `listVisibleSubdirs` + test headless

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h`
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp`
- Create: `engine/tests/content_browser_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nada (primera tarea).
- Produces: `std::vector<std::filesystem::path> DonTopo::listVisibleSubdirs(const std::filesystem::path& dir)` — devuelve las subcarpetas directas de `dir` ordenadas por path, excluyendo las que empiezan por `.` y `build-ninja`. Devuelve vector vacío si `dir` no existe o no se puede leer. La Task 2 la usa desde `drawFolderTree`.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/content_browser_tests.cpp`:

```cpp
// Test headless de los helpers del Content Browser (sin GUI). Plain main +
// asserts, sin framework — mismo patrón que physics_tests.cpp.
#include "DonTopo/Editor/ContentBrowserPanel.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Crea un árbol temporal con carpetas normales, ruido a filtrar y un fichero
// suelto. Devuelve la raíz.
static fs::path makeFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_content_browser_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets", ec);
    fs::create_directories(root / "Scripts", ec);
    fs::create_directories(root / ".hidden", ec);
    fs::create_directories(root / "build-ninja", ec);
    std::ofstream(root / "readme.txt") << "x";
    return root;
}

// Lista sólo subcarpetas: ni ficheros, ni ocultas, ni build-ninja.
static void test_filters_noise(const fs::path& root)
{
    std::vector<fs::path> dirs = listVisibleSubdirs(root);
    CHECK(dirs.size() == 2);
    if (dirs.size() == 2) {
        // std::sort sobre paths: 'S' (83) va antes que 'a' (97).
        CHECK(dirs[0].filename() == "Scripts");
        CHECK(dirs[1].filename() == "assets");
    }
}

// Las carpetas vacías salen como tal, no como error.
static void test_empty_dir(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "assets").empty());
}

// Un directorio inexistente devuelve vacío sin lanzar.
static void test_missing_dir(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "no_existe_esta_carpeta").empty());
}

// Un fichero (no directorio) devuelve vacío sin lanzar.
static void test_path_is_file(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "readme.txt").empty());
}

int main()
{
    fs::path root = makeFixture();
    test_filters_noise(root);
    test_empty_dir(root);
    test_missing_dir(root);
    test_path_is_file(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL CONTENT BROWSER TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Registrar el test en CMake**

En `engine/tests/CMakeLists.txt`, añadir al final (dejando intacto el bloque de `dt_physics_tests`):

```cmake
add_executable(dt_content_browser_tests content_browser_tests.cpp)
target_link_libraries(dt_content_browser_tests PRIVATE DonTopoEngine)
target_compile_features(dt_content_browser_tests PRIVATE cxx_std_20)
```

- [ ] **Step 3: Compilar para verificar que el test falla**

Run: `.\build.bat`

Expected: FALLO de compilación con un error tipo `'listVisibleSubdirs': identifier not found` / `is not a member of 'DonTopo'`. La función aún no existe.

- [ ] **Step 4: Declarar la función en el header**

En `engine/include/DonTopo/Editor/ContentBrowserPanel.h`, dentro de `namespace DonTopo {`, **antes** de `class ContentBrowserPanel {`:

```cpp
// Subcarpetas directas de dir, ordenadas por path, filtrando el ruido que
// no interesa ver en el árbol del Content Browser: entradas ocultas (nombre
// que empieza por '.') y el directorio de build. Devuelve vacío —sin
// lanzar— si dir no existe, no es un directorio o no se puede leer.
// Declarada aquí (y no en el anonymous namespace del .cpp) para que el test
// headless pueda enlazarla.
std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir);
```

El header ya incluye `<filesystem>`, `<vector>` y `<string>`, así que no hacen falta includes nuevos.

- [ ] **Step 5: Implementar la función**

En `engine/src/Editor/ContentBrowserPanel.cpp`, el anonymous namespace ocupa
las líneas 13-82 (`} // namespace`) y `namespace DonTopo {` abre en la 84.
Insertar la definición justo **después** de esa apertura de `namespace
DonTopo {`, antes del primer método de la clase — así queda fuera del
anonymous namespace (visible para el test) y ve los helpers `samePath` /
`pathUnderDir` que la Task 2 necesitará:

```cpp
std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir)
{
    static const std::set<std::string> kHiddenDirs = { "build-ninja" };

    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return out; // no existe, es un fichero o no hay permisos

    for (const auto& entry : it)
    {
        if (!entry.is_directory(ec) || ec) { ec.clear(); continue; }
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || kHiddenDirs.count(name)) continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}
```

`<set>`, `<algorithm>` y `<filesystem>` ya están incluidos en el fichero.

- [ ] **Step 6: Compilar y correr el test**

Run: `.\build.bat`
Expected: build OK.

Run: `.\build-ninja\engine\tests\dt_content_browser_tests.exe`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit code 0.

- [ ] **Step 7: Verificar que no se rompió el test de física**

Run: `.\build-ninja\engine\tests\dt_physics_tests.exe`
Expected: `ALL PHYSICS TESTS PASSED`, exit code 0.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(editor): listVisibleSubdirs helper for the Content Browser folder tree

Lists a directory's immediate subfolders, sorted, skipping hidden entries
and build-ninja. Declared in the header (not the anonymous namespace) so
the headless test can link it.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Árbol de carpetas en sustitución del IGFD embebido

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h`
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:1-11` (includes), `:229-270` (panel izquierdo), `:277-282` (fuente de `m_currentDir`), `:331-338` (doble-clic en carpeta del grid)

**Interfaces:**
- Consumes: `DonTopo::listVisibleSubdirs(const std::filesystem::path&) -> std::vector<std::filesystem::path>` (Task 1). Del anonymous namespace ya existente en el `.cpp`: `bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)` (compara ignorando mayúsculas y relativo/absoluto) y `bool pathUnderDir(const std::filesystem::path& p, const std::filesystem::path& dir)` (true si `p` está *estrictamente* dentro de `dir`).
- Produces: nada que consuman tareas posteriores (última tarea).

**Nota sobre los números de línea:** los rangos de arriba son del fichero tal como está antes de la Task 1. La Task 1 inserta la definición de `listVisibleSubdirs`, así que los bloques a modificar aparecerán desplazados hacia abajo. Localízalos por su contenido, no por el número.

- [ ] **Step 1: Declarar `drawFolderTree` y borrar el estado del diálogo**

En `engine/include/DonTopo/Editor/ContentBrowserPanel.h`, añadir a la sección `private:` de la clase, junto al resto de métodos:

```cpp
    // Pinta recursivamente dir y sus subcarpetas visibles como TreeNodes.
    // Click en la etiqueta selecciona la carpeta (m_currentDir); click en la
    // flecha sólo expande. Escanea disco en cada frame para los nodos
    // abiertos: sin caché que invalidar y los cambios hechos fuera del editor
    // aparecen solos.
    void drawFolderTree(const std::filesystem::path& dir);
```

Y **borrar** estos dos miembros junto con sus comentarios:

```cpp
    bool m_dlgOpen = false;
```

```cpp
    // Path al que reabrir el diálogo IGFD la próxima vez que !m_dlgOpen;
    // vacío = reabrir en m_projectRoot. Se consume (se vacía) en cada
    // reapertura — quien quiera reabrir en una carpeta concreta debe
    // asignarlo de nuevo antes de poner m_dlgOpen = false.
    std::string m_dlgReopenPath;
```

Actualizar también el comentario de `m_projectRoot`, que describe un clamp que deja de existir:

```cpp
    // Raíz del proyecto (canonicalizada una vez); es la raíz del árbol de
    // carpetas, y por tanto el límite natural de navegación del panel.
    std::filesystem::path m_projectRoot;
```

- [ ] **Step 2: Quitar el include de IGFD**

En `engine/src/Editor/ContentBrowserPanel.cpp`, borrar la línea:

```cpp
#include <ImGuiFileDialog.h>
```

- [ ] **Step 3: Compilar para verificar que falla**

Run: `.\build.bat`

Expected: FALLO de compilación en `ContentBrowserPanel.cpp` con errores tipo `'IGFD': undeclared identifier` y `'m_dlgOpen': undeclared identifier`, apuntando al bloque del panel izquierdo. Confirma que el código viejo está aislado en los sitios que toca esta tarea.

- [ ] **Step 4: Implementar `drawFolderTree`**

En `engine/src/Editor/ContentBrowserPanel.cpp`, añadir el método (dentro de `namespace DonTopo`, junto a los demás métodos de la clase):

```cpp
void ContentBrowserPanel::drawFolderTree(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> subdirs = listVisibleSubdirs(dir);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (subdirs.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (samePath(dir, std::filesystem::path(m_currentDir)))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (samePath(dir, m_projectRoot))
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // Si la carpeta seleccionada cuelga de ésta, forzar la rama abierta para
    // que se vea (p.ej. tras doble-clic en una carpeta del grid derecho).
    if (pathUnderDir(std::filesystem::path(m_currentDir), dir))
        ImGui::SetNextItemOpen(true);

    ImGui::PushID(dir.string().c_str());
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", dir.filename().string().c_str());

    // IsItemToggledOpen: pulsar la flecha expande, pero no cambia selección.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        m_currentDir = dir.string();
        m_scanned    = false;
    }

    if (open)
    {
        for (const auto& sub : subdirs)
            drawFolderTree(sub);
        ImGui::TreePop();
    }
    ImGui::PopID();
}
```

- [ ] **Step 5: Sustituir el panel izquierdo por el árbol**

En `ContentBrowserPanel::draw`, reemplazar **todo** el bloque `##FileDlgPane` — desde `ImGui::BeginChild("##FileDlgPane", ...)` hasta su `ImGui::EndChild();`, incluyendo la apertura del diálogo, el `Display()` y el clamp de raíz — por:

```cpp
    // Left: árbol de carpetas
    ImGui::BeginChild("##FolderTreePane", ImVec2(leftWidth, totalHeight), false);
    drawFolderTree(m_projectRoot);
    ImGui::EndChild();
```

- [ ] **Step 6: Fijar `m_currentDir` desde el árbol, no desde IGFD**

Sigue existiendo la inicialización de `m_projectRoot` al principio de `draw`:

```cpp
    if (m_projectRoot.empty())
        m_projectRoot = std::filesystem::canonical(std::filesystem::current_path());
```

Añadir justo debajo el arranque de la selección (antes valía `"assets"` por defecto; ahora la raíz del árbol):

```cpp
    if (m_currentDir.empty())
        m_currentDir = m_projectRoot.string();
```

Y en el bloque `##AssetPane`, borrar las cuatro líneas que leían el directorio del diálogo:

```cpp
        std::string browsedDir = IGFD::FileDialog::Instance()->GetCurrentPath();
        if (browsedDir.empty()) browsedDir = "assets";
        if (browsedDir != m_currentDir) {
            m_currentDir = browsedDir;
            m_scanned = false;
        }
```

No se sustituyen por nada: `m_currentDir` y `m_scanned` ya los mantiene `drawFolderTree`, y el `if (!m_scanned)` que va justo detrás sigue funcionando igual.

- [ ] **Step 7: Simplificar el doble-clic en carpeta del grid derecho**

Reemplazar el cuerpo del `if` de doble-clic sobre directorios:

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

por:

```cpp
            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_currentDir = path.string();
                m_scanned    = false;
            }
```

El `SetNextItemOpen` del Step 4 se encarga de que el árbol expanda la rama y marque la carpeta.

- [ ] **Step 8: Compilar**

Run: `.\build.bat`
Expected: build OK, sin warnings nuevos en `ContentBrowserPanel.cpp`.

- [ ] **Step 9: Correr los tests headless**

Run: `.\build-ninja\engine\tests\dt_content_browser_tests.exe`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit code 0.

Run: `.\build-ninja\engine\tests\dt_physics_tests.exe`
Expected: `ALL PHYSICS TESTS PASSED`, exit code 0.

- [ ] **Step 10: Verificar que IGFD sigue vivo en los modales**

Los modales de `EditorUI.cpp` (Save/Load Scene) y `PropertiesPanel.cpp` (Add Mesh/Audio) mantienen su `#include <ImGuiFileDialog.h>` y su uso. Confirmar que el include sólo desapareció del Content Browser:

Run: `git grep -n "ImGuiFileDialog.h" -- engine/`
Expected: hits en `engine/src/Editor/EditorUI.cpp` y `engine/src/Editor/PropertiesPanel.cpp` (o en sus headers, según dónde estuviera), y **ningún** hit en `ContentBrowserPanel.*`.

- [ ] **Step 11: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp
git commit -m "feat(editor): replace embedded IGFD with a folder tree in the Content Browser

The left pane embedded ImGuiFileDialog in NoDialog mode just to navigate
directories, but IGFD always draws its footer (file name field, filter
combo, OK/Cancel) with no flag to hide it, plus a breadcrumb, a create
folder button and a search bar. None of it was used: the panel only read
GetCurrentPath().

A plain TreeNode walk from m_projectRoot replaces it, which also drops the
root-clamp workaround and m_dlgReopenPath - a tree can only descend from
its root. IGFD stays in the Save/Load Scene and Add Mesh/Audio modals.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 12: Verificación manual en la GUI**

Los tests headless no cubren ImGui. Arrancar el sandbox y comprobar en el Content Browser:

1. El panel izquierdo muestra un árbol con la raíz del repo abierta, y **no** aparecen `.git`, `.vscode` ni `build-ninja`.
2. Ya no hay campo `File Name`, combo de filtros, botones OK/Cancel, breadcrumb, botón de crear carpeta ni barra de búsqueda.
3. Click en una carpeta la marca como seleccionada y el grid derecho pasa a mostrar su contenido.
4. Click en la flecha expande/pliega **sin** cambiar la selección ni el grid.
5. Doble-clic en una carpeta del grid derecho expande esa rama en el árbol y la deja seleccionada.
6. Crear una carpeta desde el Explorador de Windows: aparece en el árbol sin reiniciar ni pulsar nada.
7. Right-click > Rename y right-click > Delete en el grid derecho siguen funcionando.
8. El drag&drop de un `.fbx`/`.wav` del grid a la escena sigue funcionando.
9. Save Scene / Load Scene y Add Mesh / Add Audio siguen abriendo su diálogo IGFD normal.
