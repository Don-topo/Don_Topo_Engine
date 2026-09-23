# Importación real de assets externos — Diseño

Fecha: 2026-09-23. Deriva de `docs/assets-editor-audit.md` (hallazgos U1/U2),
Fase 3, ítem "Importación real de ficheros externos".

## Contexto y motivación

Hoy no existe ningún camino para traer un fichero de fuera del proyecto
(un FBX o una textura nueva desde el Explorador de Windows) al editor:

- No hay drag&drop OS-level en todo el repo (`glfwSetDropCallback`/
  `WM_DROPFILES` no aparecen ni en `engine/` ni en `sandbox/`).
- Los 18 diálogos `IGFD::FileDialog` de `PropertiesPanel.cpp` (Add Mesh, Add
  Audio, Texture, Font, y 13 diálogos de atlas/fuente de componentes UI)
  pasan por `canAcceptAsset` (`PropertiesPanel.cpp:75-99`), que **rechaza**
  cualquier fichero fuera de `ctx.project` o del workspace compartido del
  motor, con un log ("Asset de otro proyecto, rechazado") y nada más.

El único camino de facto es copiar el fichero a mano con el Explorador de
Windows dentro de la carpeta del proyecto *antes* de abrir el editor o el
diálogo. Este documento diseña el camino que falta: **copiar el fichero
externo al proyecto** (no solo referenciarlo), por dos entradas — drag&drop
sobre el Content Browser, y los 18 diálogos "Browse..." de Properties.

## Decisiones ya tomadas (brainstorming)

- El drop OS-level se acepta **solo sobre el Content Browser**. Soltar sobre
  Viewport/Properties/etc. no hace nada, ni deja log — igual que hoy arrastrar
  un `DT_ASSET_PATH` interno a una zona sin `DragDropTarget` no hace nada.
- v1 solo importa las extensiones que ya son arrastrables hoy dentro del
  editor (`kDraggableExt`, `ContentBrowserPanel.cpp:588-592`): `.fbx`,
  `.wav/.mp3/.ogg/.flac`, `.png/.jpg/.jpeg/.bmp/.tga`, `.ttf/.otf/.ttc`.
  Cualquier otra extensión soltada o elegida se rechaza con log.
- Los 18 diálogos de Properties **se unifican** con el mismo mecanismo de
  copia: un fichero externo elegido ahí también se copia al proyecto, en vez
  de rechazarse sin más.
- Conflicto de nombre en destino: **se rechaza**, no se auto-renombra. El
  usuario borra o renombra a mano primero.
- Destino de los diálogos de Properties (que no tienen concepto de "carpeta
  actual" como el Content Browser): **carpeta fija por tipo de extensión**,
  bajo `assets/Imported/` (`Meshes/`, `Audio/`, `Textures/`, `Fonts/`).
- Un drop puede traer varios ficheros a la vez (arrastre múltiple desde
  Explorer): **todos se procesan, cada uno con su propio resultado** — los
  que valen se copian, los que no se rechazan con su log, sin abortar el resto.

## Arquitectura

### 1. Captura del drop OS-level

`sandbox/src/main.cpp` registra `glfwSetDropCallback` junto a los demás
callbacks de la ventana (`:846-884`, mismo patrón de
`glfwSetWindowUserPointer` ya usado por cursor/mouse/scroll/char/key). El
callback de GLFW da `(GLFWwindow*, int count, const char** paths)` sin
posición; se complementa con `glfwGetCursorPos` en el momento de la llamada
para saber dónde cayó cada fichero (todos comparten la posición del cursor en
ese evento, que es como el sistema operativo entrega el batch).

Los paths + posición se encolan en una estructura simple (vector con mutex no
hace falta: GLFW despacha callbacks en el hilo principal, igual que el resto
de callbacks de este fichero) que vive en el `main()` de `sandbox`, no en el
motor. `runtime/main.cpp` no registra este callback: no tiene editor ni
Content Browser.

### 2. Transporte hasta el panel

`engine/include/DonTopo/Editor/EditorContext.h` gana:

```cpp
struct DroppedFile {
    std::filesystem::path path;
    float screenX = 0.0f;
    float screenY = 0.0f;
};

// Vacía la cola de ficheros soltados sobre la ventana este frame (drop
// OS-level, no el drag&drop interno de ImGui). Vacío/no asignado en tests
// headless y en runtime — solo lo rellena el main() de sandbox. Se consume
// una vez: llamarlo dos veces en el mismo frame devuelve vacío la segunda.
std::function<std::vector<DroppedFile>()> takeDroppedFiles;
```

Mismo patrón que `openScript`/`openAnimator`: callback opcional, vacío por
defecto, solo `EditorUI::draw()` lo rellena a partir de un puntero no-dueño
hacia la cola de `main()`.

### 3. Consumo en el Content Browser

`ContentBrowserPanel::draw` llama `ctx.takeDroppedFiles()` una vez por frame
(si el `std::function` está asignado). El panel ya guarda su rect de pantalla
de forma indirecta vía `ImGui::Begin`/`ImGui::GetWindowPos`/`GetWindowSize`
dentro del propio `draw`; se añaden dos miembros (`m_lastScreenPos`,
`m_lastScreenSize`) actualizados al principio de `draw` para poder comparar
contra los `DroppedFile` de *este mismo* frame (la comparación ocurre después
de conocer el rect, dentro de la misma llamada a `draw`, así que no hay
desfase de un frame).

Cada `DroppedFile` cuyo `(screenX, screenY)` cae dentro del rect se pasa a
`importExternalAsset(path, m_currentDir)` (ver §4). Los que caen fuera se
descartan sin log — nadie más los reclama, y no hay señal de que el usuario
quisiera soltarlos ahí.

### 4. Función de import compartida

Nuevo `engine/include/DonTopo/Editor/AssetImport.h` +
`engine/src/Editor/AssetImport.cpp` — deliberadamente fuera de
`ContentBrowserPanel.h`, para que `PropertiesPanel.cpp` no dependa del
Content Browser:

```cpp
namespace DonTopo {

enum class AssetImportResult {
    Copied,
    RejectedExtension,
    RejectedNameConflict,
    RejectedCopyFailed,
};

struct AssetImportOutcome {
    AssetImportResult result;
    std::filesystem::path destPath;   // válido solo si result == Copied
    std::string errorMessage;         // vacío salvo RejectedCopyFailed
};

// true si ext (con el punto, en minúsculas) es uno de los tipos
// importables: mismo set que ya usa el drag&drop interno del Content
// Browser. Se expone aquí, no en ContentBrowserPanel.h, porque
// PropertiesPanel también la necesita y no debe incluir el header del
// Content Browser.
bool isImportableExtension(const std::string& ext);

// Bucket de destino por extensión, bajo destRoot/Imported/: Meshes, Audio,
// Textures o Fonts. destRoot es la raíz del proyecto (ctx.project->root());
// nunca ContentBrowserPanel::m_currentDir en el camino de Properties, que no
// tiene ese concepto. Vacío si la extensión no es importable.
std::filesystem::path importedAssetDestDir(const std::filesystem::path& destRoot,
                                            const std::string& ext);

// Copia source a destDir/source.filename(). No sobreescribe: si el destino
// ya existe, devuelve RejectedNameConflict sin tocar disco.
// std::filesystem::copy_file con overload de error_code (nunca lanza).
AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir);

} // namespace DonTopo
```

`ContentBrowserPanel.cpp` mueve su `kDraggableExt` local a delegar en
`isImportableExtension` (mismo set, una sola fuente de verdad); no se
duplica la lista.

### 5. Content Browser — camino del drop

```cpp
for (const DroppedFile& f : ctx.takeDroppedFiles())
{
    if (!dentroDelRect(f)) continue;
    AssetImportOutcome r = importExternalAsset(f.path, m_currentDir);
    if (r.result == AssetImportResult::Copied)
    {
        ctx.pushLog("Asset importado: " + r.destPath.filename().string());
        m_scanned = false;
    }
    else
    {
        ctx.pushLog("Import rechazado (" + f.path.filename().string() + "): " +
                    describirResultado(r));
    }
}
```

### 6. Properties — unificación de los 18 diálogos

`canAcceptAsset` (`PropertiesPanel.cpp:75-99`) cambia de firma:

```cpp
// Antes: bool canAcceptAsset(const EditorContext&, const std::filesystem::path&);
std::optional<std::filesystem::path> canAcceptAsset(const EditorContext& ctx,
                                                     const std::filesystem::path& path);
```

- Vetos existentes (edición bloqueada por Load Scene en curso, asset de
  *otro* proyecto que no es ni el actual ni el workspace compartido) se
  mantienen igual, devolviendo `std::nullopt`.
- Donde antes devolvía `true` para "no hay proyecto abierto" o "ya está
  dentro del proyecto/workspace", ahora devuelve `path` sin tocarlo (no es
  externo, no hay nada que copiar).
- Donde antes devolvía `false` por "asset de otro proyecto, rechazado":
  ahora, **antes** de rechazar, comprueba `isImportableExtension`. Si es
  importable, llama `importExternalAsset(path, importedAssetDestDir(ctx.project->root(), ext))`
  y devuelve `destPath` en éxito, o `std::nullopt` (con log del motivo) si
  falla. Si la extensión no es importable, rechaza igual que hoy.

Los 18 call-sites (`PropertiesPanel.cpp`, patrón repetido en cada
`drawXDialog`) cambian de:

```cpp
if (m_xFileDialog->IsOk() && canAcceptAsset(ctx, m_xFileDialog->GetFilePathName()))
    aplicar(ctx, ..., m_xFileDialog->GetFilePathName());
```

a:

```cpp
if (m_xFileDialog->IsOk())
{
    if (auto resolved = canAcceptAsset(ctx, m_xFileDialog->GetFilePathName()))
        aplicar(ctx, ..., resolved->string());
}
```

Es el bloque de trabajo más grande del plan: mecánico (mismo reemplazo 18
veces), pero real — cada call-site hay que localizarlo y tocarlo porque
`GetFilePathName()` ya no puede llamarse una segunda vez asumiendo que
devuelve la ruta que se validó (ahora puede haber cambiado tras la copia).

## Manejo de errores

| Caso | Resultado | Feedback |
|---|---|---|
| Extensión no importable | `RejectedExtension` | Log: "Formato no soportado para importar: .xyz" |
| Ya existe un fichero con ese nombre en destino | `RejectedNameConflict` | Log: "Ya existe '<nombre>' en el destino; import cancelado" |
| Fallo de copia (permisos, disco lleno, etc.) | `RejectedCopyFailed` | Log con `errorMessage` (mensaje de `std::error_code`) |
| Edición bloqueada (`ctx.editingLocked`) | veto ya existente, sin cambios | Log ya existente: "Carga de escena en curso..." |
| Asset de otro proyecto abierto (no el workspace compartido) y extensión NO importable | veto ya existente | Log ya existente: "Asset de otro proyecto, rechazado" |

Ningún caso lanza excepción: `importExternalAsset` usa
`std::filesystem::copy_file` con la sobrecarga de `std::error_code`, mismo
patrón que el resto del panel (`ContentBrowserPanel.cpp` ya sigue esta regla
en rename/delete).

## Testing

- **Headless, nuevo `engine/tests/asset_import_tests.cpp`** (mismo patrón
  `content_browser_tests.cpp`: plain `main()` + `CHECK`):
  - `isImportableExtension`: casos positivos/negativos, mayúsculas.
  - `importedAssetDestDir`: cada bucket de extensión, extensión no importable
    → path vacío.
  - `importExternalAsset`: copia exitosa (fichero aparece en destino, mismo
    contenido); origen inexistente → `RejectedCopyFailed`; destino ya
    ocupado → `RejectedNameConflict` sin tocar el fichero existente;
    extensión no importable **no se comprueba aquí** (es responsabilidad del
    llamante decidir si llama a la función, `importExternalAsset` no
    revalida extensión — separación de responsabilidad de §4).
- **No testeable sin GUI** (verificación manual, igual que el resto del
  Content Browser):
  - El drop real desde el Explorador de Windows sobre la ventana.
  - El hit-test contra el rect del panel cuando está dockeado / redimensionado.
  - Los 18 diálogos de Properties con un fichero externo real.

## Fuera de alcance de este spec

- Miniaturas/preview de los assets importados (U3 del audit) — feature
  separada.
- FileWatcher para reflejar cambios hechos fuera del editor sin ningún gesto
  (U9) — feature separada, no relacionada con importar.
- Reorganizar `assets/Imported/` automáticamente después de importar (mover,
  renombrar) — el usuario lo hace a mano desde el Content Browser, que ya
  soporta rename/delete.
- Cambiar el comportamiento cuando el asset SÍ pertenece a `ctx.project` o al
  workspace compartido — ese camino no toca `importExternalAsset` en
  absoluto, sigue exactamente igual que hoy.
