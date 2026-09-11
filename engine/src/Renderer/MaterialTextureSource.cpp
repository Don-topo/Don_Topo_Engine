#include "DonTopo/Renderer/MaterialTextureSource.h"

#include <stb_image.h>

namespace DonTopo
{
    void StbPixelsFree::operator()(unsigned char* p) const
    {
        stbi_image_free(p);
    }

    DecodedTexture decodeMaterialTexture(const std::string& path, const std::vector<uint8_t>& embedded)
    {
        DecodedTexture out;
        int channels = 0;
        unsigned char* px = nullptr;

        switch (chooseTextureSource(path, embedded)) {
            case TextureSource::Path:
                px = stbi_load(path.c_str(), &out.w, &out.h, &channels, STBI_rgb_alpha);
                break;
            case TextureSource::Embedded:
                px = stbi_load_from_memory(embedded.data(), static_cast<int>(embedded.size()),
                                           &out.w, &out.h, &channels, STBI_rgb_alpha);
                break;
            case TextureSource::None:
                break;
        }

        out.pixels.reset(px);
        if (!px) out.w = out.h = 0;
        return out;
    }
}
