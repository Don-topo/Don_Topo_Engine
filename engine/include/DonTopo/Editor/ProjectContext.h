#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

// Sandbox de rutas de un proyecto del editor.
//
// El workspace es la carpeta `projects/` junto al ejecutable; cada proyecto es
// una subcarpeta con un `project.json` y las subcarpetas `assets/`, `scenes/`
// y `scripts/`. Una vez elegido el proyecto, todo lo que el editor lee o
// escribe del usuario (escenas, scripts, assets, destino del export) pasa por
// resolve()/contains(): una ruta de otro proyecto se rechaza sin tocar disco.
//
// Los assets DEL MOTOR (logo de la ventana, shaders, splash, skybox de la
// demo) NO son del proyecto y se siguen resolviendo como siempre desde la raíz
// del ejecutable/repo: no pasan por aquí.
class ProjectContext {
public:
    ProjectContext() = default;
    explicit ProjectContext(const std::filesystem::path& root);

    // Raíz del proyecto, ya canonicalizada si se pudo. Vacía si el contexto no
    // se ha inicializado (tests headless, arranque antes del selector).
    const std::filesystem::path& root() const { return m_root; }
    bool valid() const { return !m_root.empty(); }

    // root() / relative, sin normalizar más allá de lo que hace operator/. Si
    // `relative` ya es absoluta se devuelve tal cual (operator/ la sustituye):
    // el filtro sigue siendo contains(), no esta función.
    std::filesystem::path resolve(const std::filesystem::path& relative) const;

    // ¿`absolute` cae dentro del proyecto (o es la propia raíz)?
    //
    // FALLA EN CERRADO: si el contexto no es válido, o si no se puede
    // canonicalizar la raíz o el prefijo existente del destino (permisos, ruta
    // borrada a media operación), devuelve false. Ante la duda, fuera.
    bool contains(const std::filesystem::path& absolute) const;

    // --- Workspace -------------------------------------------------------

    // `projects/` junto al ejecutable. No la crea.
    static std::filesystem::path workspaceDir();

    // Subcarpetas del workspace que tienen un `project.json`, ordenadas por
    // nombre de carpeta. Crea el workspace si no existe. Nunca lanza.
    static std::vector<std::filesystem::path> discover();

    // Nombre declarado en el `project.json` del proyecto; si el fichero falta o
    // no se puede parsear, el nombre de la carpeta.
    static std::string readProjectName(const std::filesystem::path& projectDir);

    // Valida el nombre para usarlo como carpeta del workspace: no vacío ni solo
    // espacios, ni `.`/`..`, sin separadores de ruta ni caracteres inválidos en
    // Windows, sin nombres de dispositivo reservados, y único frente a las
    // carpetas ya existentes comparando SIN distinguir mayúsculas. Rellena
    // `error` con el motivo cuando devuelve false.
    static bool validateName(const std::string& name, std::string& error);

    // Valida y, si pasa, crea `projects/<name>/` con su `project.json`, las
    // subcarpetas `assets/`, `scenes/`, `scripts/` y la escena de arranque
    // kStartupScene —vacía: al abrir el proyecto solo se ve el skybox—. Si la
    // validación falla no crea nada y devuelve false con el motivo en `error`.
    static bool create(const std::string& name, std::filesystem::path& outDir, std::string& error);

    // Versión que se escribe en los `project.json` nuevos.
    static constexpr const char* kProjectVersion = "1.0";
    // Escena que abre el editor al elegir proyecto, relativa a root().
    static constexpr const char* kStartupScene = "scenes/main.json";

private:
    std::filesystem::path m_root;
};

} // namespace DonTopo
