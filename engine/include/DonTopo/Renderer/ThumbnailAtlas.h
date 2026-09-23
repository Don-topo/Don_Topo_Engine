#pragma once
#include <cstddef>
#include <cstdint>

// Lo que comparten el editor y los backends del atlas compartido de miniaturas
// del Content Browser: dimensiones, y nada mas. La logica vive en el editor.
namespace DonTopo {

constexpr uint32_t kThumbCell       = 64;                                    // lado de una casilla
constexpr uint32_t kThumbAtlasCells = 32;                                    // casillas por lado
constexpr uint32_t kThumbAtlasSize  = kThumbCell * kThumbAtlasCells;         // 2048 px
constexpr uint32_t kThumbSlotCount  = kThumbAtlasCells * kThumbAtlasCells;   // 1024

struct UvRect { float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; };

// Una casilla lista para subir: `rgba` apunta a kThumbCell*kThumbCell*4 bytes.
struct ThumbnailTile
{
    uint32_t       slot = 0;
    const uint8_t* rgba = nullptr;
};

// UV de una casilla, con medio texel de margen por cada lado para que el
// filtrado lineal no sangre el color de la casilla vecina.
inline UvRect thumbnailUv(uint32_t slot)
{
    const uint32_t x = (slot % kThumbAtlasCells) * kThumbCell;
    const uint32_t y = (slot / kThumbAtlasCells) * kThumbCell;
    const float    s = static_cast<float>(kThumbAtlasSize);
    return { (x + 0.5f) / s, (y + 0.5f) / s,
             (x + kThumbCell - 0.5f) / s, (y + kThumbCell - 0.5f) / s };
}

} // namespace DonTopo
