#pragma once
#include <array>
#include <cstdint>
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
    // Ajustes del menu View que se guardan POR PROYECTO en la seccion
    // "settings" del project.json.
    //
    // Los ENABLES arrancan en false a proposito: un proyecto nuevo, o uno sin
    // seccion "settings", abre con todos los efectos apagados aunque el
    // Renderer tenga otros defaults.
    //
    // Los PARAMETROS (intensidades, radios, pasos) NO llevan aqui el valor por
    // defecto real: readSettings() recibe una `base` que el editor rellena
    // leyendo el propio Renderer, y cada campo ausente en el JSON se queda con
    // el valor de esa base. Asi el default de un parametro sigue siendo el del
    // Renderer sin duplicar aqui sus numeros (y sin que ProjectContext dependa
    // del Renderer).
    struct ViewSettings {
        // Indices de la visibilidad de panel, en el mismo orden que el menu View.
        enum Panel {
            PanelScene = 0,
            PanelViewport,
            PanelProperties,
            PanelLog,
            PanelContentBrowser,
            PanelScriptEditor,
            PanelAnimator,
            PanelPerformance,
            PanelInputActions,
            PanelCount
        };

        bool        ambient = false;
        bool        bloom   = false;
        bool        ssao    = false;
        bool        ssr     = false;
        bool        fog     = false;
        bool        motionBlur = false;
        // Combos por NOMBRE, nunca por indice: reordenar el array de opciones
        // no puede cambiar el ajuste guardado de nadie.
        std::string aaMode = "None";
        std::string fpMode = "Off";
        // Backend de render. No se aplica al leerlo: el device ya está creado
        // cuando se abre el proyecto, así que solo surte efecto en el arranque
        // siguiente (ver readLastProject). Nombres en RenderBackend.h.
        std::string renderBackend = "Vulkan";

        float ambientIntensity = 0.0f;
        float bloomThreshold   = 0.0f;
        float bloomKnee        = 0.0f;
        float bloomIntensity   = 0.0f;
        float ssaoRadius       = 0.0f;
        float ssaoBias         = 0.0f;
        float ssaoIntensity    = 0.0f;
        float ssaoPower        = 0.0f;
        float ssrMaxDistance   = 0.0f;
        float ssrThickness     = 0.0f;
        int   ssrMaxSteps      = 0;
        float ssrEdgeFade      = 0.0f;
        float ssrIntensity     = 0.0f;
        float fogDensity       = 0.0f;
        float fogHeightFalloff = 0.0f;
        float fogBaseHeight    = 0.0f;
        float fogAnisotropy    = 0.0f;
        int   fogSteps         = 0;
        float fogScatter[3]    = {0.0f, 0.0f, 0.0f};
        float motionBlurIntensity = 0.0f;
        float motionBlurMaxRadius = 0.0f;
        int   motionBlurSamples   = 0;
        float fxaaSubpix       = 0.0f;
        float fxaaEdgeThreshold    = 0.0f;
        float fxaaEdgeThresholdMin = 0.0f;
        float ssaaFactor       = 0.0f;
        int   msaaSamples      = 0;
        float taaFeedback      = 0.0f;
        float taaJitterScale   = 0.0f;
        float fpLightRadius    = 0.0f;

        // --- Volumenes de audio por bus ---------------------------------------
        //
        // Neutros (1.0) a proposito, y NO entran en la regla de "todo apagado"
        // que aplica a los efectos: un proyecto sin estos campos tiene que
        // abrirse sonando igual que antes de la feature, no en silencio.
        float masterVolume = 1.0f;
        float musicVolume  = 1.0f;
        float sfxVolume    = 1.0f;

        // Tri-estado: -1 = sin dato guardado, el panel se queda como este. Los
        // paneles NO entran en la regla de "todo apagado".
        int panelOpen[PanelCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

        // --- Capas de colisión de física -------------------------------------
        //
        // Mismo índice que PhysicsManager (0-31), pero sin incluirlo: la
        // dependencia va Editor -> Physics en el .cpp del editor, no en este
        // header. La matriz viaja comprimida a una máscara por capa (bit b de
        // layerMasks[a] = "a colisiona con b") y arranca ENTERA a unos, que es
        // la matriz que no filtra nada — un proyecto sin estos campos abre
        // exactamente como antes de la feature.
        static constexpr int LayerCount = 32;

        // Capas realmente creadas (el prefijo [0, layerActive) de los arrays de
        // abajo). Siempre >= 1: la capa 0 ("Default") no se puede borrar.
        int layerActive = 1;

        std::array<std::string, LayerCount> layerNames = [] {
            std::array<std::string, LayerCount> n;
            n[0] = "Default";
            return n;
        }();

        std::array<uint32_t, LayerCount> layerMasks = [] {
            std::array<uint32_t, LayerCount> m{};
            m.fill(0xFFFFFFFFu);
            return m;
        }();

        // Diagnostico de la ultima lectura, para el Log del editor. No se
        // serializa.
        bool        loadFailed = false; // JSON ilegible o "settings" corrupto
        std::string unknownEnum;        // nombre de combo que no existe hoy
    };

    // Lee la seccion "settings" del project.json. NUNCA lanza: fichero ausente,
    // JSON roto, "settings" que no es un objeto o campos con el tipo cambiado se
    // caen al valor de `base` (enables y combos, al default de ViewSettings).
    // No escribe nada: un fichero corrupto se queda como esta.
    static ViewSettings readSettings(const std::filesystem::path& projectDir, const ViewSettings& base);

    // Sustituye la seccion "settings" del project.json conservando el resto del
    // fichero (name, version, lo que haya). Escribe en un temporal de la misma
    // carpeta y hace rename encima: un fallo a mitad no deja el project.json
    // truncado. Devuelve false sin tocar el original si algo falla.
    static bool writeSettings(const std::filesystem::path& projectDir, const ViewSettings& settings);

    // Version de la propia seccion "settings", independiente de kProjectVersion.
    static constexpr const char* kSettingsVersion = "1.0";

    // --- Estado del editor (editor.json, junto al ejecutable) -------------
    //
    // El backend de render se guarda POR PROYECTO, pero el Renderer se crea
    // antes de que el usuario elija proyecto: al arrancar, el editor todavia no
    // sabe de que project.json leerlo. Por eso recuerda aqui cual fue el ultimo
    // proyecto abierto, y de ese saca el backend con el que arranca.
    //
    // Es estado del editor, no del proyecto: no viaja con el, no se exporta y
    // perderlo solo significa volver a arrancar en Vulkan.

    // Ruta del ultimo proyecto abierto. Vacia si no hay editor.json, si esta
    // corrupto, o si la carpeta que apunta ya no existe (proyecto borrado o
    // movido): en todos esos casos el arranque se cae a Vulkan sin quejarse.
    static std::filesystem::path readLastProject();

    // Recuerda `projectDir` como ultimo proyecto abierto. Misma escritura
    // atomica que writeSettings (temporal + rename). Devuelve false sin tocar
    // el fichero anterior si algo falla; nadie debe abortar por eso.
    static bool writeLastProject(const std::filesystem::path& projectDir);

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
