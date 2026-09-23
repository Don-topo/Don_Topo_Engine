#pragma once
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
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

// Reparto de las casillas del atlas entre claves (una por miniatura), con
// desalojo LRU. "Uso" = pedir la casilla en el frame actual (assign/find).
class ThumbnailSlots
{
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    explicit ThumbnailSlots(uint32_t capacity = kThumbSlotCount);

    // Una vez por frame, antes de cualquier assign/find de ese frame.
    void beginFrame() { ++m_frame; }

    // Casilla de key: la que ya tenia (marcada como usada este frame) o una
    // nueva. Sin hueco libre desaloja la menos usada recientemente que NO se usara
    // este frame (empate: la de indice menor) y, si `evicted` no es nulo, dice
    // cual. kNone si todas las casillas se usaron este frame.
    uint32_t assign(uint64_t key, std::optional<uint64_t>* evicted = nullptr);

    // Casilla de key sin reservar ninguna (y marcandola como usada); kNone si no esta.
    uint32_t find(uint64_t key);

    // Libera la casilla de key, si tenia. No desaloja a nadie.
    void release(uint64_t key);

    uint32_t capacity() const { return static_cast<uint32_t>(m_slots.size()); }

private:
    struct Slot
    {
        uint64_t key       = 0;
        bool     used      = false;
        uint64_t lastFrame = 0;
    };

    std::vector<Slot>                      m_slots;
    std::unordered_map<uint64_t, uint32_t> m_byKey;
    uint64_t                               m_frame = 1;
};

} // namespace DonTopo
