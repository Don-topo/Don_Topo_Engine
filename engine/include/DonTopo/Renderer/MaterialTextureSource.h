#pragma once
#include <cstdint>
#include <memory>
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
    // intención completa no se cumple en todos los callers: en el camino skinned
    // de D3D12 (addSkinnedMesh) una subida fallida cae al neutro blanco del
    // hueco, no al damero, así que ahí una ruta rota se ve blanca.
    inline TextureSource chooseTextureSource(const std::string& path,
                                             const std::vector<uint8_t>& embedded)
    {
        if (!path.empty())     return TextureSource::Path;
        if (!embedded.empty()) return TextureSource::Embedded;
        return TextureSource::None;
    }

    // Libera lo que devuelve stb. Va aparte para que este header no arrastre
    // stb_image.h a todo el que lo incluya.
    struct StbPixelsFree { void operator()(unsigned char* p) const; };

    // Píxeles RGBA8 de un slot de material. `pixels` nulo = no había nada que
    // decodificar o stb falló; el relleno (blanco, normal plana, damero) lo
    // pone cada caller, que es donde vive esa política.
    struct DecodedTexture
    {
        int w = 0, h = 0;
        std::unique_ptr<unsigned char, StbPixelsFree> pixels;
        explicit operator bool() const { return pixels != nullptr; }
    };

    // El ÚNICO sitio que decodifica un slot de material, para los cuatro
    // uploaders: decodeSlot (AsyncAssetLoader), createTextureImage y
    // createNormalMapImage (GpuResources) y uploadMaterialTexture (D3D12).
    // Cada uno llevaba su propio switch sobre chooseTextureSource y nada los
    // ataba a él: revertir uno solo a "la embebida gana" dejaba la suite en
    // verde. Ahora un test decodifica con los dos campos llenos, y otro falla si
    // stbi_load_from_memory aparece fuera de MaterialTextureSource.cpp.
    DecodedTexture decodeMaterialTexture(const std::string& path, const std::vector<uint8_t>& embedded);
}
