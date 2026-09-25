#pragma once
#include "DonTopo/Core/FileStamp.h"
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <istream>
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

enum class ThumbnailStatus { Ok, Unreadable, TooLarge, AnimationOnly };

// Un fichero del que depende una miniatura y su estado al generarla.
using ThumbnailDependency = FileStamp;

struct ThumbnailResult
{
    ThumbnailStatus                  status = ThumbnailStatus::Unreadable;
    std::vector<uint8_t>             rgba;   // kThumbCell*kThumbCell*4 si status == Ok; vacio si no
    // Lo que declara el decodificador, a poder ser SELLADO antes de leer cada
    // fichero (stampFile). stampDependencies sella lo que llegue sin sellar y
    // pone el propio asset delante.
    std::vector<ThumbnailDependency> dependencies;
};

inline constexpr float kNeutralAlbedo = 0.6f;   // lineal: el gris de un .mat que hereda

// Decodifica path y lo reduce a UNA casilla kThumbCell x kThumbCell RGBA8:
// conserva la proporcion (filtro de caja ponderado por alfa), no amplia lo que
// ya cabe, lo centra y deja el resto transparente. CPU pura: se llama desde un
// worker. Nunca lanza.
ThumbnailResult makeThumbnail(const std::filesystem::path& path);

// Lo mismo desde un stream recien abierto (en su posicion 0). Solo se rebobina
// (seekg) si la imagen pasa el filtro de tamano y hay que cargarla. Existe
// aparte para poder probar que rechazar una imagen enorme cuesta su CABECERA y no
// leer el fichero entero.
ThumbnailResult makeThumbnailFromStream(std::istream& in);

// Pone `self` (el asset, sellado ANTES de decodificar) la primera, conserva el
// sello de las dependencias que el decodificador ya sello y sella las que no.
// Quita duplicados y el propio asset si el decodificador lo repitio.
void stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self);

// .fbx/.obj: los que tarda segundos en decodificar (ver el tope de ThumbnailCache).
bool isModelThumbnailPath(const std::filesystem::path& path);

// Esfera con el albedo, metallic y roughness del .mat; lo heredado, neutro.
ThumbnailResult makeMaterialThumbnail(const std::filesystem::path& mat);

// Decodificador por extension: imagen -> makeThumbnail; .fbx/.obj -> preview
// rasterizado; .mat -> esfera. Cualquier otra cosa, Unreadable. Nunca lanza.
ThumbnailResult makeAssetThumbnail(const std::filesystem::path& path);

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

class ThumbnailDiskCache;

// Orquesta las miniaturas: pedidos desde el grid, decodificacion asincrona y
// subida al atlas. NO conoce GPU ni JobSystem: recibe un Runner (para lanzar
// trabajo fuera del hilo principal) y un Uploader (para copiar casillas al
// atlas), asi que se prueba entero sin ninguno de los dos. Todo el estado vive en
// el hilo principal; los workers solo ejecutan makeThumbnail y dejan el
// resultado en una cola con mutex.
class ThumbnailCache
{
public:
    // Lanza el job fuera del hilo principal. false = el pool lo rechazo (parado):
    // la entrada queda en Failed y el hueco en vuelo se devuelve.
    using Runner   = std::function<bool(std::function<void()>)>;
    // Copia el lote de casillas al atlas. false = no se pudo (todo el lote falla).
    using Uploader = std::function<bool(const ThumbnailTile* tiles, size_t count)>;
    // Decodifica UN asset a casilla. Por defecto makeAssetThumbnail; existe como
    // parametro para poder probar un decodificador que lanza.
    using Decoder  = std::function<ThumbnailResult(const std::filesystem::path&)>;

    // disk: opcional; el worker la consulta antes de decodificar y guarda lo que
    // decodifica. maxModelsInFlight: de los maxInFlight, cuantos pueden ser
    // modelos (isModelThumbnailPath), que tardan segundos.
    ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4,
                   uint32_t slotCapacity = kThumbSlotCount, Decoder decode = {},
                   std::shared_ptr<const ThumbnailDiskCache> disk = {},
                   uint32_t maxModelsInFlight = 2);
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

    // Estado final de path: Ok si esta en el atlas; el motivo si fallo
    // (Unreadable, TooLarge, AnimationOnly); nullopt si aun no se sabe.
    std::optional<ThumbnailStatus> status(const std::filesystem::path& path) const;

    // Decodificaciones lanzadas cuyo resultado aun no se ha recogido.
    uint32_t inFlight() const { return m_inFlight; }
    // De ellas, las de modelos.
    uint32_t modelsInFlight() const { return m_modelsInFlight; }

private:
    enum class State { Queued, Running, Decoded, Ready, Failed };

    struct Entry
    {
        std::filesystem::path            path;
        std::vector<ThumbnailDependency> deps;              // [0] = el propio asset
        uint64_t                         key = 0;
        State                            state = State::Queued;
        ThumbnailStatus                  status = ThumbnailStatus::Ok;   // motivo si Failed
        bool                             model = false;     // cuenta para el tope de modelos
        std::vector<uint8_t>             pixels;            // solo en Decoded
        uint64_t                         lastRequestFrame = 0;
    };

    struct Done
    {
        uint64_t        generation = 0;
        std::string     path;
        bool            model = false;
        ThumbnailResult result;
    };

    // Lo unico que comparten los workers con el hilo principal. En un shared_ptr:
    // un resultado que llega tras destruir el cache cae aqui y no toca memoria muerta.
    struct Shared
    {
        std::mutex        mutex;
        std::vector<Done> done;
    };

    static uint64_t makeKey(const std::filesystem::path& path, int64_t mtime);
    void            startJobs();

    Runner                                    m_run;
    Uploader                                  m_upload;
    Decoder                                   m_decode;
    std::shared_ptr<const ThumbnailDiskCache> m_disk;
    uint32_t                                  m_maxInFlight;
    uint32_t                                  m_maxModelsInFlight;
    uint32_t                                  m_modelsInFlight = 0;
    uint32_t                                  m_inFlight   = 0;
    uint64_t                               m_frame      = 1;
    uint64_t                               m_generation = 1;
    ThumbnailSlots                         m_slots;
    std::unordered_map<std::string, Entry> m_entries;
    std::deque<std::string>                m_queue;
    std::shared_ptr<Shared>                m_shared = std::make_shared<Shared>();
};

} // namespace DonTopo
