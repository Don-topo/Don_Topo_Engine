#include "DonTopo/Renderer/SplashScreen.h"
#include <stb_image.h>

namespace DonTopo {

bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH)
{
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!px) return false;
    outRGBA.assign(px, px + (size_t)w * h * 4);
    outW = w;
    outH = h;
    stbi_image_free(px);
    return true;
}

} // namespace DonTopo
