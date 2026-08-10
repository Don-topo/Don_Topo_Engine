#include "DonTopo/Editor/ProjectContext.h"

#include "DonTopo/Core/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <utility>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DonTopo {

namespace {

std::string toLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool equalsNoCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && toLowerAscii(a) == toLowerAscii(b);
}

// Directorio del ejecutable. Mismo criterio que el runtime exportado
// (runtime/main.cpp): sin esto, el workspace dependería del cwd desde el que
// se lance el editor.
fs::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    DWORD   n                = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return fs::path(buffer).parent_path();
#endif
    std::error_code ec;
    fs::path        cwd = fs::current_path(ec);
    return ec ? fs::path{} : cwd;
}

// Claves de la visibilidad de panel, en el orden del enum Panel. Son parte del
// formato en disco: renombrar una pierde el ajuste guardado de ese panel.
const char* const kPanelKeys[ProjectContext::ViewSettings::PanelCount] = {
    "scene", "viewport", "properties", "log", "contentBrowser",
    "scriptEditor", "animator", "performance", "inputActions"};

// Lectores tolerantes: la clave que falta, o que trae otro tipo, devuelve el
// default sin lanzar. Es lo que hace que un "settings" a medias siga abriendo.
bool readBoolField(const nlohmann::json& j, const char* key, bool def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

float readFloatField(const nlohmann::json& j, const char* key, float def)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return def;
    const double v = it->get<double>();
    // Un NaN/inf colado en el fichero llegaria tal cual a un uniform del
    // Renderer: fuera antes de salir de aqui.
    return std::isfinite(v) ? static_cast<float>(v) : def;
}

int readIntField(const nlohmann::json& j, const char* key, int def)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer())
        return def;
    return it->get<int>();
}

std::string readStringField(const nlohmann::json& j, const char* key, const std::string& def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}

// Seccion "settings" de un proyecto recien creado: solo lo que la feature fija
// (todos los efectos apagados). Los parametros se dejan FUERA a proposito, para
// que al abrir el proyecto se queden con el default del Renderer.
nlohmann::json defaultSettingsJson()
{
    const ProjectContext::ViewSettings def;
    nlohmann::json s;
    s["version"] = ProjectContext::kSettingsVersion;
    s["ambient"] = def.ambient;
    s["bloom"]   = def.bloom;
    s["ssao"]    = def.ssao;
    s["ssr"]     = def.ssr;
    s["fog"]     = def.fog;
    s["aaMode"]  = def.aaMode;
    s["fpMode"]  = def.fpMode;
    s["renderBackend"] = def.renderBackend;
    return s;
}

nlohmann::json settingsToJson(const ProjectContext::ViewSettings& s)
{
    nlohmann::json j;
    j["version"] = ProjectContext::kSettingsVersion;

    j["ambient"] = s.ambient;
    j["bloom"]   = s.bloom;
    j["ssao"]    = s.ssao;
    j["ssr"]     = s.ssr;
    j["fog"]     = s.fog;
    j["aaMode"]  = s.aaMode;
    j["fpMode"]  = s.fpMode;
    j["renderBackend"] = s.renderBackend;

    j["ambientIntensity"] = s.ambientIntensity;
    j["bloomThreshold"]   = s.bloomThreshold;
    j["bloomKnee"]        = s.bloomKnee;
    j["bloomIntensity"]   = s.bloomIntensity;
    j["ssaoRadius"]       = s.ssaoRadius;
    j["ssaoBias"]         = s.ssaoBias;
    j["ssaoIntensity"]    = s.ssaoIntensity;
    j["ssaoPower"]        = s.ssaoPower;
    j["ssrMaxDistance"]   = s.ssrMaxDistance;
    j["ssrThickness"]     = s.ssrThickness;
    j["ssrMaxSteps"]      = s.ssrMaxSteps;
    j["ssrEdgeFade"]      = s.ssrEdgeFade;
    j["ssrIntensity"]     = s.ssrIntensity;
    j["fogDensity"]       = s.fogDensity;
    j["fogHeightFalloff"] = s.fogHeightFalloff;
    j["fogBaseHeight"]    = s.fogBaseHeight;
    j["fogAnisotropy"]    = s.fogAnisotropy;
    j["fogSteps"]         = s.fogSteps;
    j["fogScatter"]       = {s.fogScatter[0], s.fogScatter[1], s.fogScatter[2]};
    j["fxaaSubpix"]            = s.fxaaSubpix;
    j["fxaaEdgeThreshold"]     = s.fxaaEdgeThreshold;
    j["fxaaEdgeThresholdMin"]  = s.fxaaEdgeThresholdMin;
    j["ssaaFactor"]       = s.ssaaFactor;
    j["msaaSamples"]      = s.msaaSamples;
    j["taaFeedback"]      = s.taaFeedback;
    j["taaJitterScale"]   = s.taaJitterScale;
    j["fpLightRadius"]    = s.fpLightRadius;

    // Un panel sin dato (-1) no se escribe: el fichero no miente sobre lo que
    // nadie ha decidido todavia.
    nlohmann::json panels = nlohmann::json::object();
    for (int i = 0; i < ProjectContext::ViewSettings::PanelCount; ++i) {
        if (s.panelOpen[i] >= 0)
            panels[kPanelKeys[i]] = (s.panelOpen[i] != 0);
    }
    j["panels"] = panels;
    return j;
}

} // namespace

ProjectContext::ViewSettings ProjectContext::readSettings(const fs::path& projectDir, const ViewSettings& base)
{
    // Los PARAMETROS heredan de `base` (el estado actual del Renderer); los
    // ENABLES y los combos NO: su default es el de ViewSettings —todo apagado—
    // aunque el Renderer venga con otra cosa.
    const ViewSettings def;
    ViewSettings       s = base;
    s.ambient    = def.ambient;
    s.bloom      = def.bloom;
    s.ssao       = def.ssao;
    s.ssr        = def.ssr;
    s.fog        = def.fog;
    s.aaMode     = def.aaMode;
    s.fpMode     = def.fpMode;
    s.renderBackend = def.renderBackend;
    s.loadFailed = false;
    s.unknownEnum.clear();
    for (int i = 0; i < ViewSettings::PanelCount; ++i)
        s.panelOpen[i] = -1;

    std::ifstream in(projectDir / "project.json");
    if (!in.is_open())
        return s; // proyecto sin fichero: defaults, y no es un error que reportar.

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        s.loadFailed = true; // project.json roto o truncado: defaults.
        return s;
    }

    if (!j.is_object() || !j.contains("settings"))
        return s; // proyecto de antes de esta feature: defaults, sin queja.

    const nlohmann::json& v = j["settings"];
    if (!v.is_object()) {
        s.loadFailed = true; // "settings" existe pero no es un objeto.
        return s;
    }

    s.ambient = readBoolField(v, "ambient", s.ambient);
    s.bloom   = readBoolField(v, "bloom", s.bloom);
    s.ssao    = readBoolField(v, "ssao", s.ssao);
    s.ssr     = readBoolField(v, "ssr", s.ssr);
    s.fog     = readBoolField(v, "fog", s.fog);
    s.aaMode  = readStringField(v, "aaMode", s.aaMode);
    s.fpMode  = readStringField(v, "fpMode", s.fpMode);
    s.renderBackend = readStringField(v, "renderBackend", s.renderBackend);

    s.ambientIntensity = readFloatField(v, "ambientIntensity", s.ambientIntensity);
    s.bloomThreshold   = readFloatField(v, "bloomThreshold", s.bloomThreshold);
    s.bloomKnee        = readFloatField(v, "bloomKnee", s.bloomKnee);
    s.bloomIntensity   = readFloatField(v, "bloomIntensity", s.bloomIntensity);
    s.ssaoRadius       = readFloatField(v, "ssaoRadius", s.ssaoRadius);
    s.ssaoBias         = readFloatField(v, "ssaoBias", s.ssaoBias);
    s.ssaoIntensity    = readFloatField(v, "ssaoIntensity", s.ssaoIntensity);
    s.ssaoPower        = readFloatField(v, "ssaoPower", s.ssaoPower);
    s.ssrMaxDistance   = readFloatField(v, "ssrMaxDistance", s.ssrMaxDistance);
    s.ssrThickness     = readFloatField(v, "ssrThickness", s.ssrThickness);
    s.ssrMaxSteps      = readIntField(v, "ssrMaxSteps", s.ssrMaxSteps);
    s.ssrEdgeFade      = readFloatField(v, "ssrEdgeFade", s.ssrEdgeFade);
    s.ssrIntensity     = readFloatField(v, "ssrIntensity", s.ssrIntensity);
    s.fogDensity       = readFloatField(v, "fogDensity", s.fogDensity);
    s.fogHeightFalloff = readFloatField(v, "fogHeightFalloff", s.fogHeightFalloff);
    s.fogBaseHeight    = readFloatField(v, "fogBaseHeight", s.fogBaseHeight);
    s.fogAnisotropy    = readFloatField(v, "fogAnisotropy", s.fogAnisotropy);
    s.fogSteps         = readIntField(v, "fogSteps", s.fogSteps);
    {
        const auto it = v.find("fogScatter");
        if (it != v.end() && it->is_array() && it->size() == 3) {
            for (int i = 0; i < 3; ++i) {
                const nlohmann::json& c = (*it)[i];
                if (c.is_number()) {
                    const double x = c.get<double>();
                    if (std::isfinite(x))
                        s.fogScatter[i] = static_cast<float>(x);
                }
            }
        }
    }
    s.fxaaSubpix           = readFloatField(v, "fxaaSubpix", s.fxaaSubpix);
    s.fxaaEdgeThreshold    = readFloatField(v, "fxaaEdgeThreshold", s.fxaaEdgeThreshold);
    s.fxaaEdgeThresholdMin = readFloatField(v, "fxaaEdgeThresholdMin", s.fxaaEdgeThresholdMin);
    s.ssaaFactor           = readFloatField(v, "ssaaFactor", s.ssaaFactor);
    s.msaaSamples          = readIntField(v, "msaaSamples", s.msaaSamples);
    s.taaFeedback          = readFloatField(v, "taaFeedback", s.taaFeedback);
    s.taaJitterScale       = readFloatField(v, "taaJitterScale", s.taaJitterScale);
    s.fpLightRadius        = readFloatField(v, "fpLightRadius", s.fpLightRadius);

    const auto panels = v.find("panels");
    if (panels != v.end() && panels->is_object()) {
        for (int i = 0; i < ViewSettings::PanelCount; ++i) {
            const auto p = panels->find(kPanelKeys[i]);
            if (p != panels->end() && p->is_boolean())
                s.panelOpen[i] = p->get<bool>() ? 1 : 0;
        }
    }

    return s;
}

bool ProjectContext::writeSettings(const fs::path& projectDir, const ViewSettings& settings)
{
    if (projectDir.empty())
        return false;

    const fs::path file = projectDir / "project.json";

    // Se parte del fichero que ya hay: guardar los ajustes no puede perder el
    // name ni la version, que son la identidad del proyecto.
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(file);
        if (in.is_open()) {
            try {
                nlohmann::json parsed;
                in >> parsed;
                if (parsed.is_object())
                    j = std::move(parsed);
            } catch (const std::exception&) {
                // Ilegible: se reconstruye lo minimo mas abajo en vez de
                // propagar el fallo, que dejaria el proyecto sin poder guardar.
            }
        }
    }
    if (!j.contains("name") || !j["name"].is_string())
        j["name"] = readProjectName(projectDir);
    if (!j.contains("version") || !j["version"].is_string())
        j["version"] = kProjectVersion;

    j["settings"] = settingsToJson(settings);

    // Temporal en la MISMA carpeta (rename atomico solo dentro del volumen) y
    // rename encima: un fallo a mitad no puede truncar el project.json.
    const fs::path tmp = projectDir / "project.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << j.dump(4);
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    return true;
}

fs::path ProjectContext::readLastProject()
{
    const fs::path dir = executableDir();
    if (dir.empty())
        return {};

    std::ifstream in(dir / "editor.json");
    if (!in.is_open())
        return {}; // primer arranque: no es un error.

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return {}; // editor.json roto: se arranca como si no hubiera.
    }

    if (!j.is_object())
        return {};
    const auto it = j.find("lastProject");
    if (it == j.end() || !it->is_string())
        return {};

    const std::string raw = it->get<std::string>();
    if (raw.empty())
        return {};

    // El proyecto pudo borrarse o moverse entre dos arranques: una ruta muerta
    // vale lo mismo que no tener dato.
    fs::path        p = fs::path(raw);
    std::error_code ec;
    if (!fs::is_directory(p, ec) || ec)
        return {};
    return p;
}

bool ProjectContext::writeLastProject(const fs::path& projectDir)
{
    if (projectDir.empty())
        return false;

    const fs::path dir = executableDir();
    if (dir.empty())
        return false;
    const fs::path file = dir / "editor.json";

    // Igual que writeSettings: se parte de lo que ya hay, para no borrar
    // cualquier otro estado del editor que llegue a este fichero mas adelante.
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(file);
        if (in.is_open()) {
            try {
                nlohmann::json parsed;
                in >> parsed;
                if (parsed.is_object())
                    j = std::move(parsed);
            } catch (const std::exception&) {
                // Ilegible: se reescribe entero en vez de dejar de guardar.
            }
        }
    }

    // generic_string() para que la ruta quede con '/' y sea legible a mano; el
    // fs::path del lector acepta ambos separadores en Windows.
    j["lastProject"] = projectDir.generic_string();

    const fs::path tmp = dir / "editor.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << j.dump(4);
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    return true;
}

ProjectContext::ProjectContext(const fs::path& root)
{
    std::error_code ec;
    fs::path        canon = fs::canonical(root, ec);
    m_root                = ec ? root : canon;
}

fs::path ProjectContext::resolve(const fs::path& relative) const
{
    return m_root / relative;
}

bool ProjectContext::contains(const fs::path& absolute) const
{
    if (m_root.empty() || absolute.empty())
        return false;

    std::error_code ec;
    const fs::path  canonRoot = fs::canonical(m_root, ec);
    if (ec)
        return false; // la raíz ya no se puede resolver: fuera.

    const fs::path target = absolute.is_absolute() ? absolute : (m_root / absolute);

    ec.clear();
    // weakly_canonical resuelve el prefijo que existe y normaliza el resto, así
    // que también vale para ficheros que aún no se han creado (Save Scene).
    const fs::path canonTarget = fs::weakly_canonical(target, ec);
    if (ec || canonTarget.empty())
        return false;

    // Comparación componente a componente y sin distinguir mayúsculas: en
    // Windows canonical no garantiza devolver el casing real del disco, y
    // comparar la cadena entera daría falsos negativos.
    auto rootIt  = canonRoot.begin();
    auto rootEnd = canonRoot.end();
    auto tgtIt   = canonTarget.begin();
    auto tgtEnd  = canonTarget.end();

    for (; rootIt != rootEnd; ++rootIt, ++tgtIt) {
        if (tgtIt == tgtEnd)
            return false; // el destino es más corto: es un ancestro, no un hijo.
        if (!equalsNoCase(rootIt->string(), tgtIt->string()))
            return false;
    }
    return true; // prefijo completo => dentro (o la propia raíz).
}

fs::path ProjectContext::workspaceDir()
{
    const fs::path exeDir = executableDir();
    if (exeDir.empty())
        return {};
    return exeDir / "projects";
}

std::vector<fs::path> ProjectContext::discover()
{
    std::vector<fs::path> out;

    const fs::path workspace = workspaceDir();
    if (workspace.empty())
        return out;

    std::error_code ec;
    if (!fs::exists(workspace, ec)) {
        ec.clear();
        fs::create_directories(workspace, ec);
        return out;
    }

    ec.clear();
    for (fs::directory_iterator it(workspace, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc) || entryEc)
            continue;
        if (fs::exists(it->path() / "project.json", entryEc) && !entryEc)
            out.push_back(it->path());
    }

    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return toLowerAscii(a.filename().string()) < toLowerAscii(b.filename().string());
    });
    return out;
}

std::string ProjectContext::readProjectName(const fs::path& projectDir)
{
    const std::string fallback = projectDir.filename().string();

    std::ifstream in(projectDir / "project.json");
    if (!in.is_open())
        return fallback;

    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("name") && j["name"].is_string()) {
            std::string name = j["name"].get<std::string>();
            if (!name.empty())
                return name;
        }
    } catch (const std::exception&) {
        // project.json corrupto: el nombre de la carpeta sigue siendo válido.
    }
    return fallback;
}

bool ProjectContext::validateName(const std::string& name, std::string& error)
{
    if (name.empty() || name.find_first_not_of(" \t") == std::string::npos) {
        error = "El nombre no puede estar vacio.";
        return false;
    }
    if (name.size() > 64) {
        error = "El nombre no puede pasar de 64 caracteres.";
        return false;
    }
    if (name == "." || name == "..") {
        error = "Nombre reservado: '" + name + "'.";
        return false;
    }

    for (unsigned char c : name) {
        if (c < 32) {
            error = "El nombre no admite caracteres de control.";
            return false;
        }
        if (std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) {
            error = std::string("Caracter no valido en el nombre: '") + static_cast<char>(c) + "'.";
            return false;
        }
    }

    if (name.back() == ' ' || name.back() == '.') {
        error = "El nombre no puede acabar en espacio ni en punto.";
        return false;
    }

    // Nombres de dispositivo reservados de Windows: la carpeta no se puede
    // crear ni aunque el resto del nombre sea válido.
    static const std::array<const char*, 22> kReserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string lowered = toLowerAscii(name);
    const std::string stem    = lowered.substr(0, lowered.find('.'));
    for (const char* reserved : kReserved) {
        if (stem == reserved) {
            error = "Nombre reservado por Windows: '" + name + "'.";
            return false;
        }
    }

    // Unicidad contra las carpetas YA existentes del workspace (tengan o no
    // project.json), sin distinguir mayúsculas.
    const fs::path workspace = workspaceDir();
    if (workspace.empty()) {
        error = "No se pudo localizar la carpeta de proyectos.";
        return false;
    }

    std::error_code ec;
    if (fs::exists(workspace, ec) && !ec) {
        ec.clear();
        for (fs::directory_iterator it(workspace, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_directory(entryEc) || entryEc)
                continue;
            if (equalsNoCase(it->path().filename().string(), name)) {
                error = "Ya existe un proyecto que se llama '" + it->path().filename().string() + "'.";
                return false;
            }
        }
    }

    error.clear();
    return true;
}

bool ProjectContext::create(const std::string& name, fs::path& outDir, std::string& error)
{
    if (!validateName(name, error))
        return false;

    const fs::path workspace = workspaceDir();
    const fs::path dir       = workspace / name;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "No se pudo crear la carpeta del proyecto: " + ec.message();
        return false;
    }

    for (const char* sub : {"assets", "scenes", "scripts"}) {
        ec.clear();
        fs::create_directories(dir / sub, ec);
        if (ec) {
            error = std::string("No se pudo crear '") + sub + "': " + ec.message();
            return false;
        }
    }

    nlohmann::json j;
    j["name"]    = name;
    j["version"] = kProjectVersion;
    // Proyecto nuevo: todos los efectos apagados desde el primer arranque.
    j["settings"] = defaultSettingsJson();

    std::ofstream out(dir / "project.json");
    if (!out.is_open()) {
        error = "No se pudo escribir project.json.";
        return false;
    }
    out << j.dump(4);
    if (!out.good()) {
        error = "No se pudo escribir project.json.";
        return false;
    }
    out.close();

    // Escena de arranque: vacía a propósito —al abrir el proyecto solo se ve el
    // skybox, que es del motor y no de la escena— pero ya creada y en el formato
    // que espera Load Scene. La escribe la propia Scene en vez de un JSON a mano
    // para que no pueda desincronizarse del esquema que lee Scene::fromJson.
    Scene startupScene;
    if (!startupScene.save((dir / kStartupScene).string())) {
        error = "No se pudo crear la escena de arranque del proyecto.";
        return false;
    }

    outDir = dir;
    error.clear();
    return true;
}

} // namespace DonTopo
