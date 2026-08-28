#pragma once
#include <cstdint>
#include <vector>

namespace DonTopo {

// Los dos rellenos que se ponen cuando un material no acaba dando una textura.
// Son DOS y no uno a proposito, porque las dos situaciones no son la misma:
//
//  - Una primitiva procedural (cubo, esfera, plano...) no lleva textura porque
//    no le corresponde. Ahi el relleno correcto es blanco: el shader multiplica
//    por el y el objeto sale con su color base, sin decorar nada.
//
//  - Una textura que el material SI declara pero que no se ha podido leer -el
//    fichero no esta, o esta corrupto- es un fallo, y tiene que verse. Ahi va el
//    damero, que es la convencion de "aqui falta algo".
//
// Confundirlas es lo que hacia cada backend por su lado y en direcciones
// contrarias: Vulkan pintaba damero tambien en el caso legitimo, y DirectX 12
// pintaba blanco tambien en el caso de fallo, con lo que un fichero que faltaba
// no se notaba. De paso, una superficie blanca y lisa no deja juzgar nada que
// dependa del detalle -por ejemplo si el shadow map ha cambiado de resolucion-.

// Lado del damero, en texeles, y lado de cada tesela.
constexpr int kMissingTextureSize = 64;
constexpr int kMissingTextureTile = 8;

// RGBA8, kMissingTextureSize x kMissingTextureSize. Grises y no magenta: el
// magenta saturado se confunde con un material emisivo, y el objetivo es que se
// lea como "textura ausente", no como un color de la escena.
inline std::vector<uint8_t> makeMissingTextureRgba()
{
    std::vector<uint8_t> px(static_cast<size_t>(kMissingTextureSize) * kMissingTextureSize * 4);
    for (int y = 0; y < kMissingTextureSize; ++y) {
        for (int x = 0; x < kMissingTextureSize; ++x) {
            const bool claro = ((x / kMissingTextureTile) + (y / kMissingTextureTile)) % 2 == 0;
            uint8_t*   p     = px.data() + (static_cast<size_t>(y) * kMissingTextureSize + x) * 4;
            p[0] = p[1] = p[2] = claro ? 0xCC : 0x88;
            p[3]               = 0xFF;
        }
    }
    return px;
}

}  // namespace DonTopo
