#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace DonTopo {

class Scene;

// Un asset que el paquete exportado debe contener.
struct ExportAsset {
    // Ruta absoluta en disco, canonicalizada. Origen de la copia.
    std::string sourcePath;
    // Ruta relativa a la raíz del paquete, con '/' como separador:
    // "assets/model.fbx". Destino de la copia y valor que acaba en el
    // .scene exportado.
    std::string packagePath;
    // false si sourcePath no existe en disco: dispara el aborto del export
    // antes de copiar nada.
    bool        existsOnDisk = false;
};

// Resultado de escribir el paquete, para volcar al Log Console.
struct ExportResult {
    bool                     ok         = false;
    int                      fileCount  = 0;
    std::uintmax_t           totalBytes = 0;
    std::vector<std::string> messages;
};

// Clave canónica de un path para comparar y deduplicar: weakly_canonical +
// minúsculas + '/' como separador. Windows es case-insensitive pero
// std::filesystem::path::operator== no lo es, y los paths llegan mezclados
// (absolutos de IGFD, relativos de un .scene editado a mano). Mismo criterio
// que samePath() en ContentBrowserPanel.cpp:25, expuesto aquí porque
// rewriteScenePaths y sus tests necesitan generar exactamente la misma clave.
std::string exportPathKey(const std::string& path);

// Todos los assets que la escena referencia: meshes de origen, fuentes de
// animación, texturas de material, audio clips y los .lua de los
// ScriptComponent. Deduplicado y ordenado por packagePath.
//
// scriptPaths mapea ScriptComponent::scriptName -> fichero .lua (lo construye
// el llamador desde ScriptManager::getRegistry()). Un nombre ausente se
// ignora en silencio: el .lua llega igual al paquete porque Scripts/ se copia
// entera.
//
// NO incluye el skybox, los shaders, Scripts/ ni el ejecutable: eso lo añade
// writeExportPackage. Esta función responde solo a "qué referencia la escena".
std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const std::filesystem::path& projectRoot,
    const std::map<std::string, std::filesystem::path>& scriptPaths);

// Reescribe in-place mesh.sourcePath, mesh.animationSources[].path y
// audioClip.path de todo el árbol a su packagePath. sourceToPackage va
// keyeado por exportPathKey(sourcePath). Devuelve cuántos paths se
// reescribieron. Las texturas no aparecen aquí porque el .scene no las
// serializa: ModelLoader las deriva como dirname(fbx)/filename.
int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage);

// Crea <destDir>/<gameName>/ (borrando su contenido si ya existía: la
// confirmación es responsabilidad de la UI), copia el runtime, los assets,
// el skybox, shaders/*.spv, Scripts/ y fmod.dll, y escribe game.scene.
ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const std::filesystem::path& destDir,
                                const std::string& gameName,
                                const std::filesystem::path& projectRoot,
                                const std::filesystem::path& scriptsDir,
                                const std::filesystem::path& runtimeExe);

} // namespace DonTopo
