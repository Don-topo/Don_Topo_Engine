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
    // Ruta absoluta en disco, resuelta con weakly_canonical (no canonical:
    // tolera que el último componente no exista, necesario para los casos con
    // existsOnDisk == false). Origen de la copia.
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

// Valida que 'name' sea un componente de ruta seguro para construir
// destDir / name. Rellena 'reason' con el motivo cuando devuelve false.
//
// Vive aquí y no en la UI porque quien lo necesita es el código que va a
// borrar: writeExportPackage() construye destDir/gameName y hace remove_all()
// sobre esa ruta. ".." sube un nivel, un nombre absoluto como "C:\Windows"
// hace que operator/ IGNORE destDir por completo (así trata operator/ una
// ruta absoluta), y Win32 descarta espacios/puntos finales del último
// componente al crear la carpeta, colapsando el destino real sobre la carpeta
// padre aunque el string en pantalla parezca inofensivo.
bool isValidExportGameName(const std::string& name, std::string& reason);

// Estado del directorio destino de un export, para decidir si es seguro
// borrarlo. El criterio es inverso al que había antes (enumerar a mano los
// sitios prohibidos: dentro del proyecto, dentro de Scripts...): esa lista
// siempre dejaba fuera un caso — el último, <repo>/assets, borraba los assets
// fuente y encima reportaba éxito. Aquí no se pregunta "¿dónde está el
// destino?" sino "¿qué hay dentro?", que es lo único que determina si un
// remove_all() destruye trabajo ajeno.
enum class ExportTargetState {
    Missing,        // no existe: se crea, nada que borrar
    Empty,          // existe y está vacío: seguro
    PriorPackage,   // existe y contiene game.scene: paquete de un export anterior, seguro
    Occupied        // existe con contenido ajeno: NUNCA se borra
};

// Clasifica el directorio de paquete 'pkg' (== destDir/gameName). Si el
// estado no se puede determinar (permisos, path inválido, cualquier error del
// sistema de ficheros) devuelve Occupied: falla en cerrado, porque el coste de
// equivocarse hacia el otro lado es borrar datos del usuario.
ExportTargetState inspectExportTarget(const std::filesystem::path& pkg);

// Crea <destDir>/<gameName>/, copia el runtime, los assets, el skybox,
// shaders/*.spv, Scripts/ y fmod.dll, y escribe game.scene.
//
// Llama a inspectExportTarget() por su cuenta y aborta sin tocar nada si el
// destino está Occupied: es autoritativo, no da por hecho que la UI haya
// mirado. Con Missing/Empty/PriorPackage sí hace remove_all() + recreado, para
// que el paquete no arrastre assets huérfanos de un export anterior (pedir
// confirmación en el caso PriorPackage sigue siendo cosa de la UI).
ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const std::filesystem::path& destDir,
                                const std::string& gameName,
                                const std::filesystem::path& projectRoot,
                                const std::filesystem::path& scriptsDir,
                                const std::filesystem::path& runtimeExe);

// Export completo: valida, recolecta, reescribe y escribe el paquete.
// Los mensajes para el usuario van en ExportResult::messages; el llamador
// decide dónde mostrarlos (el editor los vuelca al Log Console).
//
// Está aquí y no en EditorUI porque no dibuja nada: es orquestación de export
// y aritmética de rutas. Que viva en el módulo es lo que permite a
// exporter_tests ejercitar los caminos destructivos sin abrir una ventana.
ExportResult exportGame(Scene& scene,
                        const std::map<std::string, std::filesystem::path>& scriptPaths,
                        const std::filesystem::path& destDir,
                        const std::string& gameName,
                        const std::filesystem::path& projectRoot,
                        const std::filesystem::path& scriptsDir,
                        const std::filesystem::path& runtimeExe);

} // namespace DonTopo
