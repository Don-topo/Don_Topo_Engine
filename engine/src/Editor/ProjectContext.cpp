#include "DonTopo/Editor/ProjectContext.h"

#include "DonTopo/Core/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
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

} // namespace

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
