#include "DonTopo/Core/MaterialAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace DonTopo {

namespace {
constexpr std::uintmax_t kMaxMaterialAssetBytes = 64 * 1024;

// Ruta relativa a `base` si es posible (con ".." si hace falta subir), o
// absoluta tal cual si no se puede expresar relativa (otra unidad).
std::string toRelativeToFolder(const std::string& path, const std::filesystem::path& base)
{
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, base, ec);
    if (ec || rel.empty()) return path;
    return rel.generic_string();
}

// La inversa: relativa a `base`, o ya absoluta.
std::string fromRelativeToFolder(const std::string& stored, const std::filesystem::path& base)
{
    if (stored.empty()) return stored;
    std::filesystem::path p(stored);
    if (p.is_absolute()) return stored;
    return std::filesystem::weakly_canonical(base / p).string();
}

float clampFactor(float v)
{
    if (!std::isfinite(v)) return -1.0f;   // no finito: como si no estuviera puesto
    return std::clamp(v, 0.0f, 1.0f);
}
} // namespace

MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning)
{
    MaterialAsset out;
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    std::error_code ec;
    if (!std::filesystem::is_regular_file(mat, ec) || ec)
    {
        // Aunque el spec lo cuenta junto a "ilegible", SÍ se distingue del
        // resto: applyMaterialOverrides llama a esta función sin `warning`
        // (nullptr), así que `warn` no hace nada en el camino caliente — el
        // coste solo existe cuando collectMaterialOverrideWarnings (que solo
        // corre al cargar la escena) lo pide.
        warn("material inexistente; se hereda todo del modelo");
        return out;
    }

    const std::uintmax_t size = std::filesystem::file_size(mat, ec);
    if (ec || size > kMaxMaterialAssetBytes)
    {
        warn("material ilegible o demasiado grande; se hereda todo del modelo");
        return out;
    }
    if (size == 0) return out;

    std::ifstream in(mat, std::ios::binary);
    if (!in) { warn("no se pudo abrir el material; se hereda todo del modelo"); return out; }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("JSON invalido; se hereda todo del modelo");
        return out;
    }
    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("version de material desconocida; se hereda todo del modelo");
        return out;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != "material")
    {
        warn("el fichero no es de tipo material; se hereda todo del modelo");
        return out;
    }

    const std::filesystem::path folder = mat.parent_path();
    std::string problems;
    auto readPath = [&](const char* key, std::string& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_string()) dst = fromRelativeToFolder(it->get<std::string>(), folder);
        else problems += std::string(key) + " no es una ruta valida (se hereda). ";
    };
    readPath("albedo", out.albedo);
    readPath("normal", out.normal);
    readPath("orm",    out.orm);

    auto readFactor = [&](const char* key, float& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_number()) { problems += std::string(key) + " no es numerico (se hereda). "; return; }
        const double v = it->get<double>();
        if (!std::isfinite(v)) { problems += std::string(key) + " no es finito (se hereda). "; return; }
        dst = clampFactor(static_cast<float>(v));
        if (static_cast<double>(dst) != v)
            problems += std::string(key) + " fuera de rango (acotado). ";
    };
    readFactor("metallic",  out.metallic);
    readFactor("roughness", out.roughness);

    if (!problems.empty()) warn(problems);
    return out;
}

bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset, std::string* error)
{
    const std::filesystem::path folder = mat.parent_path();
    nlohmann::json j;
    j["version"]  = 1;
    j["type"]     = "material";
    if (!asset.albedo.empty()) j["albedo"] = toRelativeToFolder(asset.albedo, folder);
    if (!asset.normal.empty()) j["normal"] = toRelativeToFolder(asset.normal, folder);
    if (!asset.orm.empty())    j["orm"]    = toRelativeToFolder(asset.orm,    folder);
    if (asset.metallic  >= 0.0f) j["metallic"]  = asset.metallic;
    if (asset.roughness >= 0.0f) j["roughness"] = asset.roughness;

    std::error_code ec;
    std::filesystem::path tmp = mat;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "no se pudo escribir " + mat.string(); return false; }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "escritura incompleta de " + mat.string();
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, mat, ec);
    if (ec)
    {
        if (error) *error = "no se pudo renombrar a " + mat.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

} // namespace DonTopo
