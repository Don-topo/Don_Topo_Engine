#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

class GameObject;
struct EditorContext;

// Subcarpetas directas de dir, ordenadas por path, filtrando el ruido que
// no interesa ver en el árbol del Content Browser: entradas ocultas (nombre
// que empieza por '.') y el directorio de build. Devuelve vacío —sin
// lanzar— si dir no existe, no es un directorio o no se puede leer.
// Declarada aquí (y no en el anonymous namespace del .cpp) para que el test
// headless pueda enlazarla.
std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir);

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
    // Recorre sceneRoot actualizando Mesh::sourcePath, los 3 paths de
    // Material y AudioClipComponent::getPath() que matcheen oldPath (exacto
    // si !isDir, por prefijo si isDir) al nuevo valor tras un rename en
    // disco ya realizado.
    void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath, bool isDir);
    // Cuenta cuántos GameObjects de sceneRoot referencian path (mesh o
    // audio; exacto si !isDir, por prefijo si isDir).
    int  countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Desengancha de la escena cualquier referencia a path antes de
    // borrarlo de disco: mesh en uso -> Renderer::removeMeshComponent;
    // audio en uso -> setAudioClip(nullptr); textura de Material en uso ->
    // limpia el campo de path y hace hot-swap de GPU.
    void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                         const std::filesystem::path& path, bool isDir);
    // Arma el popup modal "Delete Asset", precalculando cuántos GameObjects
    // referencian path (mesh o audio) para mostrarlo en el texto de aviso.
    void beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Pinta recursivamente dir y sus subcarpetas visibles como TreeNodes.
    // Click en la etiqueta selecciona la carpeta (m_currentDir); click en la
    // flecha sólo expande. Escanea disco en cada frame para los nodos
    // abiertos: sin caché que invalidar y los cambios hechos fuera del editor
    // aparecen solos.
    void drawFolderTree(const std::filesystem::path& dir);

    bool m_open = true;
    bool m_scanned = false;
    std::string m_currentDir;
    // Raíz del proyecto (canonicalizada una vez); es la raíz del árbol de
    // carpetas, y por tanto el límite natural de navegación del panel.
    std::filesystem::path m_projectRoot;
    std::vector<std::filesystem::path> m_assets;

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
    std::string            m_assetDeleteError;
};

} // namespace DonTopo
