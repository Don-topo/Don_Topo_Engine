#include "DonTopo/Editor/ProjectContext.h"

#include "DonTopo/Core/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <utility>
#include <system_error>

#include "DonTopo/Core/Platform.h"

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

// Executable directory. Same criterion as the exported runtime
// (runtime/main.cpp): without this, the workspace would depend on the cwd the
// editor is launched from.
fs::path executableDir()
{
    return platform::executableDir();
}

// Panel visibility keys, in the order of the Panel enum. They are part of the
// on-disk format: renaming one loses that panel's saved setting.
//
// Without an explicit size and with the static_assert below on purpose: declared
// as [PanelCount], a panel added to the enum without a key here left a
// **nullptr** that ended up being used as a JSON key. This way it does not compile.
const char* const kPanelKeys[] = {
    "scene", "viewport", "properties", "log", "contentBrowser",
    "scriptEditor", "animator", "performance", "rendering", "inputActions"};
static_assert(std::size(kPanelKeys) == ProjectContext::ViewSettings::PanelCount,
              "kPanelKeys and the Panel enum go together: every panel in the enum needs its key, "
              "in the same order");

// Tolerant readers: a missing key, or one of another type, returns the
// default without throwing. It is what makes a half-written "settings" still open.
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
    // A NaN/inf slipped into the file would reach a Renderer uniform
    // as is: out before leaving here.
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

// "settings" section of a newly created project: only what the feature fixes
// (all effects off). The parameters are left OUT on purpose, so that
// when the project opens they keep the Renderer's default.
nlohmann::json defaultSettingsJson()
{
    const ProjectContext::ViewSettings def;
    nlohmann::json s;
    s["version"] = ProjectContext::kSettingsVersion;
    s["ambient"] = def.ambient;
    s["wireframe"] = def.wireframe;
    s["bloom"]   = def.bloom;
    s["ssao"]    = def.ssao;
    s["ssr"]     = def.ssr;
    s["fog"]     = def.fog;
    s["motionBlur"] = def.motionBlur;
    s["aaMode"]  = def.aaMode;
    s["fpMode"]  = def.fpMode;
    s["renderBackend"] = def.renderBackend;
    return s;
}

nlohmann::json settingsToJson(const ProjectContext::ViewSettings& s)
{
    nlohmann::json j;
    j["version"] = ProjectContext::kSettingsVersion;

    j["skyboxFolder"] = s.skyboxFolder;
    j["ambient"] = s.ambient;
    j["wireframe"] = s.wireframe;
    j["bloom"]   = s.bloom;
    j["ssao"]    = s.ssao;
    j["ssr"]     = s.ssr;
    j["fog"]     = s.fog;
    j["motionBlur"] = s.motionBlur;
    j["aaMode"]  = s.aaMode;
    j["fpMode"]  = s.fpMode;
    j["renderBackend"] = s.renderBackend;

    j["ambientIntensity"] = s.ambientIntensity;
    j["masterVolume"] = s.masterVolume;
    j["musicVolume"]  = s.musicVolume;
    j["sfxVolume"]    = s.sfxVolume;
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
    j["motionBlurIntensity"] = s.motionBlurIntensity;
    j["motionBlurMaxRadius"] = s.motionBlurMaxRadius;
    j["motionBlurSamples"]   = s.motionBlurSamples;
    j["fxaaSubpix"]            = s.fxaaSubpix;
    j["fxaaEdgeThreshold"]     = s.fxaaEdgeThreshold;
    j["fxaaEdgeThresholdMin"]  = s.fxaaEdgeThresholdMin;
    j["ssaaFactor"]       = s.ssaaFactor;
    j["msaaSamples"]      = s.msaaSamples;
    j["taaFeedback"]      = s.taaFeedback;
    j["taaJitterScale"]   = s.taaJitterScale;
    j["fpLightRadius"]    = s.fpLightRadius;
    j["shadowDistance"]   = s.shadowDistance;
    j["cascadeLambda"]    = s.cascadeLambda;
    j["shadowResolution"] = s.shadowResolution;
    j["presentMode"]      = s.presentMode;

    // A panel without data is not written: the file does not lie about what nobody
    // has decided yet.
    nlohmann::json panels = nlohmann::json::object();
    for (int i = 0; i < ProjectContext::ViewSettings::PanelCount; ++i) {
        if (s.panelOpen[i].has_value())
            panels[kPanelKeys[i]] = *s.panelOpen[i];
    }
    j["panels"] = panels;

    // Physics layers: the 32 names and the 32 masks ALWAYS, even if they are at
    // their default value. The matrix is a complete snapshot: writing only what
    // changed would force guessing on read whether a gap is "untouched" or
    // "disabled".
    nlohmann::json names = nlohmann::json::array();
    nlohmann::json masks = nlohmann::json::array();
    for (int i = 0; i < ProjectContext::ViewSettings::LayerCount; ++i) {
        names.push_back(s.layerNames[static_cast<size_t>(i)]);
        masks.push_back(s.layerMasks[static_cast<size_t>(i)]);
    }
    j["layerNames"]     = names;
    j["layerCollision"] = masks;
    j["layerActive"]    = s.layerActive;
    return j;
}

} // namespace

ProjectContext::ViewSettings ProjectContext::readSettings(const fs::path& projectDir, const ViewSettings& base)
{
    // The PARAMETERS inherit from `base` (the Renderer's current state); the
    // ENABLES and the combos do NOT: their default is that of ViewSettings (all off)
    // even if the Renderer comes with something else.
    const ViewSettings def;
    ViewSettings       s = base;
    s.ambient    = def.ambient;
    s.bloom      = def.bloom;
    s.ssao       = def.ssao;
    s.ssr        = def.ssr;
    s.fog        = def.fog;
    s.motionBlur = def.motionBlur;
    s.aaMode     = def.aaMode;
    s.fpMode     = def.fpMode;
    s.renderBackend = def.renderBackend;
    // The layers go with the ENABLES, not with the parameters: missing or
    // corrupt they fall back to the default (matrix without filters, only 0 named), not
    // to whatever the PhysicsManager has loaded from the previous session.
    s.layerNames  = def.layerNames;
    s.layerMasks  = def.layerMasks;
    s.layerActive = def.layerActive;
    s.loadFailed = false;
    s.unknownEnum.clear();
    // To "no data", not to whatever the `base` carried: the panel visibility of the
    // previous project must not leak into the one being opened now.
    for (int i = 0; i < ViewSettings::PanelCount; ++i)
        s.panelOpen[i].reset();

    std::ifstream in(projectDir / "project.json");
    if (!in.is_open())
        return s; // project without a file: defaults, and it is not an error to report.

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        s.loadFailed = true; // broken or truncated project.json: defaults.
        return s;
    }

    if (!j.is_object() || !j.contains("settings"))
        return s; // project from before this feature: defaults, no complaint.

    const nlohmann::json& v = j["settings"];
    if (!v.is_object()) {
        s.loadFailed = true; // "settings" exists but is not an object.
        return s;
    }

    if (v.contains("skyboxFolder") && v["skyboxFolder"].is_string())
        s.skyboxFolder = v["skyboxFolder"].get<std::string>();
    s.ambient   = readBoolField(v, "ambient", s.ambient);
    s.wireframe = readBoolField(v, "wireframe", s.wireframe);
    s.bloom   = readBoolField(v, "bloom", s.bloom);
    s.ssao    = readBoolField(v, "ssao", s.ssao);
    s.ssr     = readBoolField(v, "ssr", s.ssr);
    s.fog     = readBoolField(v, "fog", s.fog);
    s.motionBlur = readBoolField(v, "motionBlur", s.motionBlur);
    s.aaMode  = readStringField(v, "aaMode", s.aaMode);
    s.fpMode  = readStringField(v, "fpMode", s.fpMode);
    s.renderBackend = readStringField(v, "renderBackend", s.renderBackend);

    s.ambientIntensity = readFloatField(v, "ambientIntensity", s.ambientIntensity);
    // All three fall back to the value of `base` if missing, which is the neutral one: a project
    // from before the buses opens sounding the same.
    s.masterVolume = readFloatField(v, "masterVolume", s.masterVolume);
    s.musicVolume  = readFloatField(v, "musicVolume",  s.musicVolume);
    s.sfxVolume    = readFloatField(v, "sfxVolume",    s.sfxVolume);
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
    s.motionBlurIntensity = readFloatField(v, "motionBlurIntensity", s.motionBlurIntensity);
    s.motionBlurMaxRadius = readFloatField(v, "motionBlurMaxRadius", s.motionBlurMaxRadius);
    s.motionBlurSamples   = readIntField(v, "motionBlurSamples", s.motionBlurSamples);
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
    s.shadowDistance       = readFloatField(v, "shadowDistance", s.shadowDistance);
    s.cascadeLambda        = readFloatField(v, "cascadeLambda", s.cascadeLambda);
    s.shadowResolution     = v.value("shadowResolution", s.shadowResolution);
    s.presentMode          = v.value("presentMode", s.presentMode);

    // Physics layers. Tolerant ELEMENT BY ELEMENT: an array of another size,
    // or with a gap of another type, leaves that index with its default instead of
    // discarding the whole read. Never throws.
    {
        const auto it = v.find("layerNames");
        if (it != v.end() && it->is_array()) {
            const size_t n = std::min<size_t>(it->size(), ViewSettings::LayerCount);
            for (size_t i = 0; i < n; ++i)
                if ((*it)[i].is_string())
                    s.layerNames[i] = (*it)[i].get<std::string>();
        }
    }
    {
        const auto it = v.find("layerCollision");
        if (it != v.end() && it->is_array()) {
            const size_t n = std::min<size_t>(it->size(), ViewSettings::LayerCount);
            for (size_t i = 0; i < n; ++i) {
                const nlohmann::json& m = (*it)[i];
                // Unsigned and within 32 bits: a negative or a huge number
                // would silently truncate on conversion, and a truncated mask would
                // turn off collisions that nobody asked to turn off.
                if (!m.is_number_unsigned()) continue;
                const uint64_t raw = m.get<uint64_t>();
                if (raw > 0xFFFFFFFFull) continue;
                s.layerMasks[i] = static_cast<uint32_t>(raw);
            }
        }
    }

    // How many layers are created. It is clamped to [1, 32]: a 0 or a negative from the
    // file would leave the list without the Default layer, which always exists.
    s.layerActive = std::clamp(readIntField(v, "layerActive", s.layerActive), 1,
                               ViewSettings::LayerCount);

    const auto panels = v.find("panels");
    if (panels != v.end() && panels->is_object()) {
        for (int i = 0; i < ViewSettings::PanelCount; ++i) {
            const auto p = panels->find(kPanelKeys[i]);
            if (p != panels->end() && p->is_boolean())
                s.panelOpen[i] = p->get<bool>();
        }
    }

    return s;
}

bool ProjectContext::writeSettings(const fs::path& projectDir, const ViewSettings& settings)
{
    if (projectDir.empty())
        return false;

    const fs::path file = projectDir / "project.json";

    // It starts from the file that is already there: saving the settings cannot lose the
    // name or the version, which are the project's identity.
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
                // Unreadable: the minimum is rebuilt below instead of
                // propagating the failure, which would leave the project unable to save.
            }
        }
    }
    if (!j.contains("name") || !j["name"].is_string())
        j["name"] = readProjectName(projectDir);
    if (!j.contains("version") || !j["version"].is_string())
        j["version"] = kProjectVersion;

    j["settings"] = settingsToJson(settings);

    // Temporary in the SAME folder (atomic rename only within the volume) and
    // rename over it: a failure halfway cannot truncate project.json.
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
        return {}; // first startup: not an error.

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return {}; // broken editor.json: start as if there were none.
    }

    if (!j.is_object())
        return {};
    const auto it = j.find("lastProject");
    if (it == j.end() || !it->is_string())
        return {};

    const std::string raw = it->get<std::string>();
    if (raw.empty())
        return {};

    // The project may have been deleted or moved between two startups: a dead path
    // is worth the same as having no data.
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

    // Same as writeSettings: it starts from what is already there, so as not to erase
    // any other editor state that reaches this file later on.
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
                // Unreadable: it is rewritten whole instead of giving up on saving.
            }
        }
    }

    // generic_string() so the path ends up with '/' and is readable by hand; the
    // reader's fs::path accepts both separators on Windows.
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
        return false; // the root can no longer be resolved: out.

    const fs::path target = absolute.is_absolute() ? absolute : (m_root / absolute);

    ec.clear();
    // weakly_canonical resolves the prefix that exists and normalizes the rest, so
    // it also works for files that have not been created yet (Save Scene).
    const fs::path canonTarget = fs::weakly_canonical(target, ec);
    if (ec || canonTarget.empty())
        return false;

    // Component-by-component comparison, case-insensitive: on
    // Windows canonical does not guarantee returning the real casing on disk, and
    // comparing the whole string would give false negatives.
    auto rootIt  = canonRoot.begin();
    auto rootEnd = canonRoot.end();
    auto tgtIt   = canonTarget.begin();
    auto tgtEnd  = canonTarget.end();

    for (; rootIt != rootEnd; ++rootIt, ++tgtIt) {
        if (tgtIt == tgtEnd)
            return false; // the destination is shorter: it is an ancestor, not a child.
        if (!equalsNoCase(rootIt->string(), tgtIt->string()))
            return false;
    }
    return true; // full prefix => inside (or the root itself).
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
        // corrupt project.json: the folder name is still valid.
    }
    return fallback;
}

bool ProjectContext::validateName(const std::string& name, std::string& error)
{
    if (name.empty() || name.find_first_not_of(" \t") == std::string::npos) {
        error = "The name cannot be empty.";
        return false;
    }
    if (name.size() > 64) {
        error = "The name cannot be longer than 64 characters.";
        return false;
    }
    if (name == "." || name == "..") {
        error = "Reserved name: '" + name + "'.";
        return false;
    }

    for (unsigned char c : name) {
        if (c < 32) {
            error = "The name does not allow control characters.";
            return false;
        }
        if (std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) {
            error = std::string("Invalid character in the name: '") + static_cast<char>(c) + "'.";
            return false;
        }
    }

    if (name.back() == ' ' || name.back() == '.') {
        error = "The name cannot end in a space or a dot.";
        return false;
    }

    // Windows reserved device names: the folder cannot be
    // created even if the rest of the name is valid.
    static const std::array<const char*, 22> kReserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string lowered = toLowerAscii(name);
    const std::string stem    = lowered.substr(0, lowered.find('.'));
    for (const char* reserved : kReserved) {
        if (stem == reserved) {
            error = "Name reserved by Windows: '" + name + "'.";
            return false;
        }
    }

    // Uniqueness against the workspace's ALREADY existing folders (with or without
    // project.json), case-insensitive.
    const fs::path workspace = workspaceDir();
    if (workspace.empty()) {
        error = "Could not locate the projects folder.";
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
                error = "There is already a project named '" + it->path().filename().string() + "'.";
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
        error = "Could not create the project folder: " + ec.message();
        return false;
    }

    for (const char* sub : {"assets", "scenes", "scripts"}) {
        ec.clear();
        fs::create_directories(dir / sub, ec);
        if (ec) {
            error = std::string("Could not create '") + sub + "': " + ec.message();
            return false;
        }
    }

    nlohmann::json j;
    j["name"]    = name;
    j["version"] = kProjectVersion;
    // New project: all effects off from the first startup.
    j["settings"] = defaultSettingsJson();

    std::ofstream out(dir / "project.json");
    if (!out.is_open()) {
        error = "Could not write project.json.";
        return false;
    }
    out << j.dump(4);
    if (!out.good()) {
        error = "Could not write project.json.";
        return false;
    }
    out.close();

    // Startup scene: empty on purpose (when the project opens only the
    // skybox is seen, which belongs to the engine and not the scene) but already created and in the format
    // that Load Scene expects. It is written by Scene itself instead of a hand-written JSON
    // so that it cannot get out of sync with the schema that Scene::fromJson reads.
    Scene startupScene;
    if (!startupScene.save((dir / kStartupScene).string())) {
        error = "Could not create the project's startup scene.";
        return false;
    }

    outDir = dir;
    error.clear();
    return true;
}

} // namespace DonTopo
