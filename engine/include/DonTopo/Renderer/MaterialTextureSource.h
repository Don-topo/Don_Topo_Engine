#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "DonTopo/Renderer/TextureImport.h"

namespace DonTopo
{
    // Where the pixels of a material slot come from.
    enum class TextureSource { None, Path, Embedded };

    // The PATH wins over the embedded bytes.
    //
    // It used to be the other way around, and the engine never saw both fields filled at
    // once because Assimp filled both, one or the other. Since Properties lets
    // you set a path by hand, both CAN be filled at the same time, and
    // then the path is what the user just asked for: it has to win.
    // It is also what lets Clear go back to the embedded one: if the
    // embedded one won, assigning would require destroying it first.
    //
    // The intent is that a broken path is ALWAYS noticed (checkerboard), never
    // covered up with the FBX texture. This function fulfills that part: there is no
    // fallback from Path to Embedded if the file cannot be read. But the full
    // intent is not fulfilled in all callers: in D3D12's skinned path
    // (addSkinnedMesh) a failed upload falls to the neutral white of the
    // slot, not the checkerboard, so there a broken path looks white.
    inline TextureSource chooseTextureSource(const std::string& path,
                                             const std::vector<uint8_t>& embedded)
    {
        if (!path.empty())     return TextureSource::Path;
        if (!embedded.empty()) return TextureSource::Embedded;
        return TextureSource::None;
    }

    // Frees what stb returns. It goes separately so that this header does not drag
    // stb_image.h into everyone that includes it.
    struct StbPixelsFree { void operator()(unsigned char* p) const; };

    // RGBA8 pixels of a material slot. Null `pixels` = there was nothing to
    // decode or stb failed; the filler (white, flat normal, checkerboard) is
    // set by each caller, which is where that policy lives.
    struct DecodedTexture
    {
        int w = 0, h = 0;
        std::unique_ptr<unsigned char, StbPixelsFree> pixels;
        // Import settings of the file (sidecar). Embedded textures and
        // those without a sidecar carry the default: Auto and no mips.
        ColorSpaceOverride      colorSpace = ColorSpaceOverride::Auto;
        std::vector<TextureMip> mips;   // levels 1..N-1; empty if mipmaps is off
        explicit operator bool() const { return pixels != nullptr; }
    };

    // The ONLY place that decodes a material slot, for the four
    // uploaders: decodeSlot (AsyncAssetLoader), createTextureImage and
    // createNormalMapImage (GpuResources) and uploadMaterialTexture (D3D12).
    // Each one carried its own switch over chooseTextureSource and nothing tied them
    // to it: reverting just one to "the embedded one wins" left the suite
    // green. Now one test decodes with both fields filled, and another fails if
    // stbi_load_from_memory appears outside MaterialTextureSource.cpp.
    DecodedTexture decodeMaterialTexture(const std::string& path, const std::vector<uint8_t>& embedded);
}
