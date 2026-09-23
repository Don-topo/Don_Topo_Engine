#pragma once
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace DonTopo {

// Mas de estos pixeles de origen y ni se intenta decodificar: un PNG de 16k x
// 16k son 1 GB transitorios en un worker.
constexpr uint64_t kThumbMaxSourcePixels = 100'000'000;

enum class ThumbnailStatus { Ok, Unreadable, TooLarge };

struct ThumbnailResult
{
    ThumbnailStatus      status = ThumbnailStatus::Unreadable;
    std::vector<uint8_t> rgba;   // kThumbCell*kThumbCell*4 si status == Ok; vacio si no
};

// Decodifica path y lo reduce a UNA casilla kThumbCell x kThumbCell RGBA8:
// conserva la proporcion (filtro de caja ponderado por alfa), no amplia lo que
// ya cabe, lo centra y deja el resto transparente. CPU pura: se llama desde un
// worker. Nunca lanza.
ThumbnailResult makeThumbnail(const std::filesystem::path& path);

} // namespace DonTopo
