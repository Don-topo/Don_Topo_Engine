#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // De donde salen los píxeles de un slot de material.
    enum class TextureSource { None, Path, Embedded };

    // La RUTA gana a los bytes embebidos.
    //
    // Antes era al revés, y el motor nunca veía los dos campos llenos a la vez
    // porque ambos los rellenaba Assimp, uno u otro. Desde que Properties deja
    // poner una ruta a mano, los dos SÍ pueden estar llenos a la vez, y
    // entonces la ruta es lo que el usuario acaba de pedir: tiene que ganar.
    // Además es lo que permite que Clear vuelva a la embebida: si ganara la
    // embebida, asignar exigiría destruirla primero.
    //
    // La intención es que una ruta rota SIEMPRE se note (damero), nunca se
    // tape con la textura del FBX. Esta función cumple esa parte: no hay
    // fallback de Path a Embedded si el fichero no se puede leer. Pero la
    // intención completa no se cumple en todos los callers — ver el aviso en
    // D3D12Renderer.cpp:3430-3436 sobre el camino skinned de D3D12.
    inline TextureSource chooseTextureSource(const std::string& path,
                                             const std::vector<uint8_t>& embedded)
    {
        if (!path.empty())     return TextureSource::Path;
        if (!embedded.empty()) return TextureSource::Embedded;
        return TextureSource::None;
    }
}
