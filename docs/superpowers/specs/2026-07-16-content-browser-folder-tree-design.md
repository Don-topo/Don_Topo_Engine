# Content Browser — árbol de carpetas propio

Fecha: 2026-07-16

## Problema

El panel izquierdo del Content Browser incrusta `ImGuiFileDialog` en modo
`NoDialog` (`ContentBrowserPanel.cpp:229-270`). IGFD dibuja siempre tres
bloques (`ImGuiFileDialog.cpp:3908-3910`):

- **Header**: botón crear carpeta, breadcrumb de ruta, barra de búsqueda.
- **Content**: tabla de ficheros con columnas Name/Size.
- **Footer**: `File Name : [input] [combo de filtros] [OK] [Cancel]`.

El footer no tiene flag para ocultarse: se dibuja incluso con `NoDialog`.
Y es decorado puro — el Content Browser nunca lee el fichero seleccionado
del diálogo, solo usa `GetCurrentPath()` (`ContentBrowserPanel.cpp:277`).
El resultado es un file picker completo haciendo de simple navegador de
carpetas, con una UI que el usuario percibe como sobrecargada.

Alternativas descartadas:

- **Solo flags** (`DisableCreateDirectoryButton`, `HideColumnSize`,
  `ReadOnlyFileNameField`): quita el botón `+` y una columna, pero el
  footer con OK/Cancel y el combo de filtros permanecen.
- **Header de config propio** (`IMGUIFILEDIALOG_CONFIG_FILE`): toca CMake,
  afecta también a los modales Save/Load, y tampoco elimina el footer.

## Solución

Sustituir el IGFD embebido del Content Browser por un árbol de carpetas
propio (`ImGui::TreeNodeEx` + `std::filesystem`) anclado en
`m_projectRoot`. Elimina de golpe footer, combo de filtros, OK/Cancel,
breadcrumb, botón crear carpeta, barra de búsqueda y tabla de ficheros.

IGFD sigue usándose en los modales (Save/Load Scene en `EditorUI.cpp`,
Add Mesh/Audio en `PropertiesPanel.cpp`), donde un file picker completo sí
es lo apropiado. La dependencia `imgui_filedialog` del CMake se queda.

Layout resultante — carpetas a la izquierda, grid de assets a la derecha:

```text
┌─ Content Browser ───────────────────────┐
│ ▼ Don_Topo_Engine  │  [3D] [3D] [SFX]  │
│   ▶ assets         │  cube  hero  jump │
│   ▼ Scripts        │                   │
│       shaders      │  [...] [...]      │
│   ▶ engine         │  a.lua movef.lua  │
└────────────────────┴───────────────────┘
```

## Arquitectura

### Estado

Se elimina de `ContentBrowserPanel.h`:

- `bool m_dlgOpen`
- `std::string m_dlgReopenPath`

Se conserva:

- `m_projectRoot`: pasa de ser un límite que vigilar a ser la raíz del
  árbol. Un árbol que solo desciende desde la raíz no puede salirse de
  ella, así que el clamp deja de ser necesario.
- `m_currentDir` / `m_scanned`: siguen gobernando la caché del grid
  derecho. Ahora `m_currentDir` lo fija el árbol en vez de
  `IGFD::FileDialog::Instance()->GetCurrentPath()`.

Se elimina el `#include` de `ImGuiFileDialog.h` de
`ContentBrowserPanel.cpp`.

### `listVisibleSubdirs`

Función libre del namespace `DonTopo`, declarada en
`ContentBrowserPanel.h` y definida en `ContentBrowserPanel.cpp` (no en el
anonymous namespace: el test headless necesita enlazarla):

```cpp
std::vector<std::filesystem::path>
listVisibleSubdirs(const std::filesystem::path& dir);
```

Recorre `dir` con `directory_iterator` y `std::error_code`, se queda con
las entradas que son directorio y descarta las ocultas. Devuelve el
resultado ordenado por path (coherente con el `std::sort` que ya usa el
grid derecho).

Oculta una entrada si:

- el nombre empieza por `.` (`.git`, `.vscode`), o
- el nombre está en el conjunto `{ "build-ninja" }`.

Si `dir` no se puede leer (permisos, borrada entre frames), el
`error_code` absorbe el fallo y devuelve lista vacía: el nodo se dibuja
como hoja y no se lanza excepción.

El filtro aplica solo al árbol. El grid derecho mantiene su escaneo
actual sin filtrar — cambiarlo queda fuera de este alcance.

### `drawFolderTree`

Método privado recursivo de `ContentBrowserPanel`:

```cpp
void drawFolderTree(const std::filesystem::path& dir);
```

Por cada nodo:

1. `subdirs = listVisibleSubdirs(dir)` — escaneo en cada frame, sin
   caché.
2. Flags: `OpenOnArrow | OpenOnDoubleClick | SpanAvailWidth`, más
   `Selected` si `dir == m_currentDir`, más `Leaf` si `subdirs` está
   vacío.
3. Si `dir` es prefijo estricto de `m_currentDir`, llamar antes
   `ImGui::SetNextItemOpen(true)` — así una carpeta seleccionada desde el
   grid derecho queda visible aunque su rama estuviera plegada.
4. `bool open = ImGui::TreeNodeEx(...)` con etiqueta
   `dir.filename().string()`.
5. Si `ImGui::IsItemClicked()` y no `ImGui::IsItemToggledOpen()`:
   `m_currentDir = dir.string(); m_scanned = false;` — la guarda evita
   que pulsar la flecha de expandir cambie también la selección.
6. Si `open`: recursión sobre cada elemento de `subdirs`, luego
   `ImGui::TreePop()`.

El árbol muestra solo carpetas; los ficheros son responsabilidad del grid
derecho.

La raíz se dibuja con `ImGuiTreeNodeFlags_DefaultOpen` y etiqueta
`m_projectRoot.filename().string()`.

El escaneo por frame afecta solo a los nodos expandidos — decenas de
entradas en el peor caso, coste despreciable frente a eliminar la caché,
su invalidación y un botón Refresh. A cambio, los cambios hechos en disco
fuera del editor aparecen solos.

### Sincronización con el grid derecho

El doble-clic en una carpeta del grid (`ContentBrowserPanel.cpp:331-338`)
pierde las llamadas a `Close()` y la asignación de `m_dlgReopenPath`, y
se queda en:

```cpp
m_currentDir = path.string();
m_scanned    = false;
```

El paso 3 de `drawFolderTree` se encarga de que el árbol expanda la rama
y muestre la carpeta como seleccionada.

### Fuera de alcance

- El rename/delete por right-click vive en el grid derecho y no toca
  IGFD: no se ve afectado.
- El drag&drop de assets (`DT_ASSET_PATH`) vive en el grid derecho: no se
  ve afectado.
- No se añade right-click al árbol.

## Testing

`listVisibleSubdirs` es lógica pura sobre disco y se testea headless: un
directorio temporal con subcarpetas normales, una `.oculta` y una
`build-ninja`, más un fichero suelto. Se verifica que devuelve solo las
subcarpetas normales, ordenadas, y que un directorio inexistente devuelve
lista vacía sin lanzar.

`drawFolderTree` es ImGui y requiere verificación manual en la GUI:
expandir/plegar, click selecciona sin togglear, click en flecha togglea
sin seleccionar, doble-clic en carpeta del grid expande la rama del
árbol, y ausencia de `.git`/`build-ninja`.
