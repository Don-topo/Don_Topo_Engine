#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // De donde salen los pixeles de un slot de material.
    enum class TextureSource { None, Path, Embedded };

    // La RUTA gana a los bytes embebidos.
    //
    // Antes era al reves, y el motor nunca veia los dos campos llenos a la vez
    // porque ambos los rellenaba Assimp, uno u otro. Desde que Properties deja
    // poner una ruta a mano si pueden estarlo, y entonces la ruta es lo que el
    // usuario acaba de pedir: tiene que ganar. Ademas es lo que permite que
    // Clear vuelva a la embebida - si esta ganara, asignar exigiria destruirla.
    //
    // NO hay fallback de Path a Embedded si el fichero no se puede leer: una
    // ruta rota tiene que notarse (damero), no taparse con la textura del FBX.
    inline TextureSource chooseTextureSource(const std::string& path,
                                             const std::vector<uint8_t>& embedded)
    {
        if (!path.empty())     return TextureSource::Path;
        if (!embedded.empty()) return TextureSource::Embedded;
        return TextureSource::None;
    }
}
