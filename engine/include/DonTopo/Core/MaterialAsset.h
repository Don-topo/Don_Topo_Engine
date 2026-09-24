#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// Un material reutilizable, referenciado por ruta desde un slot de objeto (ver
// MaterialOverride::matAsset). Un campo vacio/-1 significa "heredar del modelo":
// un .mat con solo roughness deja intactas las tres texturas y el metallic.
struct MaterialAsset
{
    std::string albedo, normal, orm;   // absolutas en memoria; "" = heredar
    float       metallic  = -1.0f;     // -1 = heredar; si no, [0, 1]
    float       roughness = -1.0f;
};

inline bool operator==(const MaterialAsset& a, const MaterialAsset& b)
{
    return a.albedo == b.albedo && a.normal == b.normal && a.orm == b.orm &&
           a.metallic == b.metallic && a.roughness == b.roughness;
}
inline bool isDefault(const MaterialAsset& m) { return m == MaterialAsset{}; }

// Nunca lanza. Ausente = todo heredar, SIN aviso (es lo normal: un slot puede no
// tener .mat). Roto, de version/tipo desconocido, mayor de 64 KiB, o un campo con
// el tipo equivocado -> ese campo (o todo el fichero) hereda, CON aviso.
MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning = nullptr);

// SIEMPRE escribe el fichero, aunque `asset` sea el defecto (un .mat es un asset
// con nombre, no un ajuste opcional que desaparece). false = no se pudo, y
// `error` dice por que.
bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset,
                       std::string* error = nullptr);

} // namespace DonTopo
