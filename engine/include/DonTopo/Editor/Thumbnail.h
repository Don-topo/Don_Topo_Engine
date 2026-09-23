#pragma once
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

// Orquesta las miniaturas: pedidos desde el grid, decodificacion asincrona y
// subida al atlas. NO conoce GPU ni JobSystem: recibe un Runner (para lanzar
// trabajo fuera del hilo principal) y un Uploader (para copiar casillas al
// atlas), asi que se prueba entero sin ninguno de los dos. Todo el estado vive en
// el hilo principal; los workers solo ejecutan makeThumbnail y dejan el
// resultado en una cola con mutex.
class ThumbnailCache
{
public:
    using Runner   = std::function<void(std::function<void()>)>;
    // Copia el lote de casillas al atlas. false = no se pudo (todo el lote falla).
    using Uploader = std::function<bool(const ThumbnailTile* tiles, size_t count)>;

    ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4,
                   uint32_t slotCapacity = kThumbSlotCount);
    ThumbnailCache(const ThumbnailCache&)            = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Una vez por frame, antes de los request() de ese frame.
    void beginFrame();

    // Miniatura de path si ya esta en el atlas; nullopt = todavia no (pendiente,
    // fallida o desalojada) y quien pide sigue con su icono. La primera vez que
    // se pide una ruta se ENCOLA su decodificacion; nunca bloquea.
    std::optional<UvRect> request(const std::filesystem::path& path);

    // Recoge lo decodificado, sube como mucho maxUploads casillas en UNA llamada
    // al Uploader y lanza decodificaciones hasta maxInFlight. Una vez por frame.
    void pump(int maxUploads = 8);

    // Vuelve a leer el mtime de lo pedido el frame anterior o este y descarta lo
    // que cambio, para que se regenere. Lo llama el polling del panel, no cada frame.
    void refreshStamps();

    // Cambio de carpeta: descarta lo pendiente (en cola, en vuelo, decodificado sin
    // subir). Lo ya subido y lo fallido se conserva; un resultado tardio de la
    // generacion anterior se ignora.
    void newGeneration();

    // Decodificaciones lanzadas cuyo resultado aun no se ha recogido.
    uint32_t inFlight() const { return m_inFlight; }

private:
    enum class State { Queued, Running, Decoded, Ready, Failed };

    struct Entry
    {
        std::filesystem::path           path;
        std::filesystem::file_time_type mtime{};
        uint64_t                        key = 0;
        State                           state = State::Queued;
        std::vector<uint8_t>            pixels;            // solo en Decoded
        uint64_t                        lastRequestFrame = 0;
    };

    struct Done
    {
        uint64_t        generation = 0;
        std::string     path;
        ThumbnailResult result;
    };

    // Lo unico que comparten los workers con el hilo principal. En un shared_ptr:
    // un resultado que llega tras destruir el cache cae aqui y no toca memoria muerta.
    struct Shared
    {
        std::mutex        mutex;
        std::vector<Done> done;
    };

    static uint64_t makeKey(const std::filesystem::path& path, std::filesystem::file_time_type mtime);
    void            startJobs();

    Runner                                 m_run;
    Uploader                               m_upload;
    uint32_t                               m_maxInFlight;
    uint32_t                               m_inFlight   = 0;
    uint64_t                               m_frame      = 1;
    uint64_t                               m_generation = 1;
    ThumbnailSlots                         m_slots;
    std::unordered_map<std::string, Entry> m_entries;
    std::deque<std::string>                m_queue;
    std::shared_ptr<Shared>                m_shared = std::make_shared<Shared>();
};

} // namespace DonTopo
