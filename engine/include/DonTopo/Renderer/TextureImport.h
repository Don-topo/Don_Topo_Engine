#pragma once
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/SharedTextureCache.h"   // TextureKind

#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // Un nivel de mip RGBA8. El nivel 0 es la propia imagen y no se repite aqui.
    struct TextureMip
    {
        uint32_t             w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    // Niveles de una cadena completa hasta 1x1: 1 + floor(log2(max(w, h))). 1 si
    // alguna dimension es 0.
    uint32_t mipLevelCount(uint32_t w, uint32_t h);

    // Niveles 1..N-1 de la cadena (dimension max(1, d/2) cada vez, igual que
    // Vulkan y D3D12), con filtro de caja PONDERADO POR ALFA: un pixel transparente
    // no oscurece a sus vecinos opacos. Vacio si no hay nada que hacer (1x1,
    // puntero nulo, dimension 0). No modifica la entrada. Se promedia el valor
    // codificado, tambien en sRGB (limitacion conocida, ver la spec).
    std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h);

    // Auto = lo decide el slot (solo el color base es sRGB); Srgb/Linear lo pisan.
    // Es el UNICO sitio que decide el formato de una textura de material.
    bool resolveSrgb(TextureKind kind, ColorSpaceOverride o);

    // Sufijo de clave de cache con los ajustes de importacion de `path`. Vacio si
    // la ruta esta vacia o los ajustes son los de siempre, asi las claves de hoy
    // no cambian. Lee el sidecar de disco.
    std::string textureKeySuffix(const std::string& path);
}
