#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Editor/Thumbnail.h"

namespace DonTopo {

class GameObject;

// Subcarpetas directas de dir, ordenadas por path, filtrando el ruido que
// no interesa ver en el árbol del Content Browser: entradas ocultas (nombre
// que empieza por '.') y el directorio de build. Devuelve vacío —sin
// lanzar— si dir no existe, no es un directorio o no se puede leer.
// Declarada aquí (y no en el anonymous namespace del .cpp) para que el test
// headless pueda enlazarla.
std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir);

// Importa cada DroppedFile cuyo (screenX, screenY) caiga dentro del rect
// (rectX, rectY, rectW, rectH) a targetDir; los que caen fuera no generan
// ninguna entrada en el resultado (se ignoran en silencio). Un fallo
// individual (extension no importable, conflicto de nombre) no aborta el
// resto del lote. Declarada aquí, no en el anonymous namespace del .cpp,
// para que el test headless pueda enlazarla.
std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir);

// Tipo de un asset para el Content Browser: lo comparten el icono del grid y el
// filtro por tipo, para que no puedan discrepar.
enum class AssetKind { Folder, Model3D, Audio, Image, Font, Scene, Script, Shader, Other };

// ext con el punto y en cualquier combinación de mayúsculas ("" si no tiene). Una
// carpeta es siempre Folder, aunque se llame "a.png".
AssetKind classifyAsset(const std::string& ext, bool isDir);

// true si el asset pasa el filtro del grid: texto = subcadena sin distinguir
// mayúsculas (vacío deja pasar todo) y kindFilter = igualdad exacta de tipo
// (nullopt = todos). Ambos se combinan con AND, así que con un tipo elegido las
// carpetas quedan fuera salvo que el tipo sea Folder.
bool assetMatchesFilter(const std::string& name, AssetKind kind,
                        const std::string& text, std::optional<AssetKind> kindFilter);

// Ficheros y carpetas visibles de UNA carpeta (no recursivo), ordenados por path.
// Las carpetas ocultas y de build quedan fuera con el mismo predicado que el
// árbol; los ficheros no se filtran. Vacío —sin lanzar— si dir no existe, es un
// fichero o no se puede leer. Es lo que el grid pinta y lo que el polling compara
// entre pasadas para detectar cambios hechos fuera del editor.
std::vector<std::filesystem::path> listVisibleEntries(const std::filesystem::path& dir);

// Ancestro existente más cercano de dir sin salir de root: dir misma si existe,
// y root si dir está fuera de root o no queda ningún nivel existente hasta ella.
// Con esto el panel sigue funcionando si la carpeta actual se borra por fuera.
std::filesystem::path nearestExistingDir(const std::filesystem::path& dir,
                                         const std::filesystem::path& root);

// Un tramo del breadcrumb: lo que se pinta y a dónde salta al pulsarlo.
struct BreadcrumbSegment {
    std::string           name;
    std::filesystem::path path;
};

// Tramos desde la raíz del proyecto hasta current, con rutas acumulativas. Si
// current es la propia raíz o no cuelga de ella, devuelve solo el tramo raíz.
std::vector<BreadcrumbSegment> breadcrumbSegments(const std::filesystem::path& root,
                                                  const std::filesystem::path& current);

// Selección del grid: los paths marcados (siempre en el orden en que se ven) y el
// ancla desde la que Shift+clic calcula el rango.
struct AssetSelection {
    std::vector<std::filesystem::path>   items;
    std::optional<std::filesystem::path> anchor;

    bool contains(const std::filesystem::path& p) const;
    void clear() { items.clear(); anchor.reset(); }
};

// Aplica un clic sobre `clicked` a la selección. visible es el orden actual del
// grid (con los filtros ya aplicados). Clic = solo ese; Ctrl = alterna ese; Shift
// = rango desde el ancla hasta ese (sin ancla visible, como un clic normal).
void applyAssetClick(AssetSelection& sel, const std::vector<std::filesystem::path>& visible,
                     const std::filesystem::path& clicked, bool ctrl, bool shift);

// Quita de la selección lo que ya no está en existing; si el ancla desaparece se
// olvida. Se llama tras cambiar de carpeta o rescanear.
void pruneSelection(AssetSelection& sel, const std::vector<std::filesystem::path>& existing);

enum class MoveResult {
    Moved,
    RejectedSameFolder,    // destDir ya es la carpeta padre de src
    RejectedIntoSelf,      // una carpeta dentro de sí misma o de un descendiente
    RejectedNameConflict,  // ya hay algo con ese nombre en destDir
    RejectedFailed,        // origen inexistente o error del sistema
};

struct MoveOutcome {
    MoveResult            result = MoveResult::RejectedFailed;
    std::filesystem::path newPath;       // válido solo si result == Moved
    std::string           errorMessage;  // vacío salvo RejectedFailed
};

// Mueve src (fichero o carpeta) dentro de destDir con el mismo nombre. Nunca
// sobreescribe: un nombre ya ocupado es un rechazo, no un reemplazo. No toca la
// escena: quien llama actualiza las referencias con updateSceneReferencesForRename.
// Nunca lanza (sobrecargas con std::error_code).
MoveOutcome moveAsset(const std::filesystem::path& src, const std::filesystem::path& destDir);

// Nombre libre para una carpeta nueva dentro de dir: "Nueva carpeta", y si ya
// hay algo (carpeta o fichero) con ese nombre, "Nueva carpeta 2", "3"...
std::string uniqueFolderName(const std::filesystem::path& dir);

// Las tres funciones siguientes no tocan estado privado de ContentBrowserPanel
// (sólo sus parámetros), así que se declaran aquí como funciones libres —igual
// que listVisibleSubdirs— para que el test headless pueda enlazarlas sin
// instanciar el panel completo (que arrastraría ImGui/Vulkan).

// Cuenta cuántos GameObjects de sceneRoot referencian path (mesh o audio;
// exacto si !isDir, por prefijo si isDir).
int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
// Recorre sceneRoot actualizando Mesh::sourcePath, los 3 paths de
// Material y AudioClipComponent::getPath() que matcheen oldPath (exacto
// si !isDir, por prefijo si isDir) al nuevo valor tras un rename en
// disco ya realizado.
void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& oldPath,
                                     const std::filesystem::path& newPath, bool isDir);
// Desengancha de la escena cualquier referencia a path antes de
// borrarlo de disco: mesh en uso -> Renderer::removeMeshComponent;
// audio en uso -> setAudioClip(nullptr); textura de Material en uso ->
// el campo de path se limpia SIEMPRE (evita que un re-register intente
// stbi_load un fichero ya borrado), pero el hot-swap de GPU a la
// textura "missing" sólo se dispara si ctx.renderer && staticRenderIndex
// >= 0: replaceStaticTextureWithMissing indexa la lista de objetos
// estáticos, así que en skinned nunca ocurre — la GPU sigue mostrando
// la textura vieja hasta que se recargue la escena.
void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& path, bool isDir);

// Ventana "Content Browser" — explorador de assets del proyecto (mesh,
// audio, scripts), con rename/delete y detección de referencias en la
// escena para desengancharlas antes de borrar/renombrar en disco.
class ContentBrowserPanel {
public:
    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }

private:
    // Arma el popup modal "Rename Asset" precargado con el nombre actual de
    // path (stem si es fichero, nombre completo si es carpeta).
    void beginAssetRename(const std::filesystem::path& path, bool isDir);
    // Arma el popup modal "Delete Asset", precalculando cuántos GameObjects
    // referencian path (mesh o audio) para mostrarlo en el texto de aviso.
    // targets = (ruta, esCarpeta) de todo lo seleccionado; un solo modal para todos.
    void beginAssetDelete(GameObject* sceneRoot,
                          std::vector<std::pair<std::filesystem::path, bool>> targets);
    // Pinta recursivamente dir y sus subcarpetas visibles como TreeNodes.
    // Click en la etiqueta selecciona la carpeta (m_currentDir); click en la
    // flecha sólo expande. Escanea disco en cada frame para los nodos
    // abiertos: sin caché que invalidar y los cambios hechos fuera del editor
    // aparecen solos.
    void drawFolderTree(const std::filesystem::path& dir);
    // Convierte el último ítem dibujado en destino de soltar un asset (fichero o
    // carpeta del grid). NO mueve nada: anota en m_pendingMove, que draw() aplica
    // fuera del recorrido del árbol/grid (mover a mitad invalidaría lo que se pinta).
    void acceptAssetDropOnFolder(const std::filesystem::path& destDir);
    // Aplica y vacía m_pendingMove: mueve en disco, reescribe las referencias de
    // la escena y reubica m_currentDir si la carpeta movida la contenía.
    void applyPendingMove(EditorContext& ctx, GameObject* sceneRoot);

    struct PendingMove {
        std::vector<std::filesystem::path> srcs;   // uno, o toda la selección arrastrada
        std::filesystem::path              destDir;
    };
    std::optional<PendingMove> m_pendingMove;

    // Selección del grid (ver AssetSelection). Se poda contra lo visible cada
    // frame, así que cambiar de carpeta, filtrar o rescanear no deja fantasmas.
    AssetSelection m_selection;

    // Miniaturas de texturas. Se crea de forma perezosa cuando hay renderer con
    // atlas de miniaturas Y JobSystem; sin ellos queda en nullptr y el grid pinta
    // el icono de color de siempre.
    std::unique_ptr<ThumbnailCache> m_thumbs;
    uint64_t                        m_thumbAtlasId = 0;
    std::string                     m_thumbDir;    // carpeta de la generacion actual

    bool m_open = true;
    bool m_scanned = false;
    // Cada cuánto se relee la carpeta actual para detectar cambios hechos fuera
    // del editor (mismo enfoque que ScriptManager::pollChanges: comparar en vez de
    // vigilar). Latencia máxima de un refresco externo ≈ este intervalo.
    static constexpr double kDirPollIntervalSeconds = 0.5;
    double m_lastPollTime = 0.0;
    std::string m_currentDir;
    // Reveal de un solo frame: sólo el doble-clic en una carpeta del grid
    // derecho la pone a true (esa carpeta puede no estar visible aún en el
    // árbol). drawFolderTree la consulta para forzar abierta la rama
    // ancestro de m_currentDir, y draw() la limpia justo después de esa
    // llamada, así el usuario recupera el control para volver a colapsar esa
    // rama a mano en el siguiente frame.
    bool m_revealCurrentDir = false;
    // Raíz del proyecto (canonicalizada una vez); es la raíz del árbol de
    // carpetas, y por tanto el límite natural de navegación del panel.
    std::filesystem::path m_projectRoot;
    std::vector<std::filesystem::path> m_assets;

    // Filtros del grid (solo la carpeta actual). m_filterKindIndex indexa la
    // tabla de opciones del combo en draw(); m_filterKind es su traducción y
    // se recalcula cada frame.
    char                    m_filterText[64] = {};
    int                     m_filterKindIndex = 0;
    std::optional<AssetKind> m_filterKind;

    // Asset rename — popup modal disparado por right-click > Rename en el
    // grid derecho del Content Browser.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;

    // Asset delete — popup modal disparado por right-click > Delete.
    std::vector<std::pair<std::filesystem::path, bool>> m_assetDeleteTargets;
    int                    m_assetDeleteAffectedCount = 0;
    bool                   m_openAssetDeletePopup = false;
    std::string            m_assetDeleteError;

    // Doble clic en un .json del grid: cargar esa escena (doble y no simple,
    // igual que las carpetas y los .lua — un clic simple ocurre al pasar por
    // encima seleccionando y cargaría escenas sin querer). Si la actual tiene
    // cambios sin guardar se pregunta antes con un modal de tres opciones.
    enum class ScenePromptChoice { None, Save, Discard };
    // Escena que se cargará (vacío = ninguna pendiente). Cancelar en el modal
    // lo limpia y no se carga nada.
    std::filesystem::path  m_sceneLoadTarget;
    bool                   m_openScenePromptPopup = false;
    // Decisión tomada dentro del popup. Se consume al principio del frame
    // SIGUIENTE, fuera de todo Begin/BeginPopupModal: cargar una escena destruye
    // el árbol de GameObjects y toca la GPU, y hacerlo desde dentro del popup
    // sería reentrar en medio del propio dibujado del panel.
    ScenePromptChoice      m_scenePromptChoice = ScenePromptChoice::None;
};

} // namespace DonTopo
