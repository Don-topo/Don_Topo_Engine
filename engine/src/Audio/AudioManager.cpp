#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"

#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#include <fmod_errors.h>
#include <iostream>
#include <stdexcept>

#define SYS   reinterpret_cast<FMOD::System*>(m_system)
#define SFXG  reinterpret_cast<FMOD::ChannelGroup*>(m_sfxGroup)
#define MUSICG reinterpret_cast<FMOD::ChannelGroup*>(m_musicGroup)

static void fmodCheck(FMOD_RESULT r, const char* ctx) {
    if (r != FMOD_OK)
        throw std::runtime_error(std::string(ctx) + ": " + FMOD_ErrorString(r));
}
#endif

namespace DonTopo {

AudioManager::~AudioManager() { shutdown(); }

bool AudioManager::init()
{
#ifdef DT_FMOD_ENABLED
    // Reentrada: sin esta guarda, un segundo init() pisaba los tres punteros
    // dejando el System y los ChannelGroup anteriores vivos y sin dueño.
    if (m_system) return true;

    FMOD::System* sys = nullptr;
    try
    {
        fmodCheck(FMOD::System_Create(&sys), "FMOD::System_Create");
        fmodCheck(sys->init(512, FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED, nullptr), "FMOD init");

        FMOD::ChannelGroup* sfx;
        FMOD::ChannelGroup* music;
        fmodCheck(sys->createChannelGroup("SFX", &sfx), "createChannelGroup SFX");
        fmodCheck(sys->createChannelGroup("Music", &music), "createChannelGroup Music");

        m_system   = sys;
        m_sfxGroup = sfx;
        m_musicGroup = music;
        return true;
    }
    catch (const std::exception& e)
    {
        // m_system se asigna al FINAL, así que hasta aquí sigue a nullptr y
        // shutdown() saldría por su guarda sin liberar nada: el System creado
        // se quedaba sin cerrar para siempre si fallaba cualquier paso
        // posterior a System_Create. release() cierra y libera.
        if (sys) sys->release();
        // Sin dispositivo de salida (o con FMOD mal instalado) esto ANTES
        // propagaba la excepción hasta el catch de main y el editor no
        // arrancaba. Ahora el motor sigue vivo, mudo, y lo dice una vez. No hay
        // canal al Log Console desde aquí (mismo motivo documentado en
        // AudioClipComponent::setVolume); los hosts que quieran avisar en su UI
        // tienen el bool de retorno y available().
        std::cerr << "Audio deshabilitado: " << e.what() << std::endl;
        return false;
    }
#else
    return false;
#endif
}

bool AudioManager::available() const
{
#ifdef DT_FMOD_ENABLED
    return m_system != nullptr;
#else
    return false;
#endif
}

void AudioManager::update(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    FMOD_VECTOR p   = { pos.x, pos.y, pos.z };
    FMOD_VECTOR vel = { 0, 0, 0 };
    FMOD_VECTOR f   = { fwd.x, fwd.y, fwd.z };
    FMOD_VECTOR u   = { up.x,  up.y,  up.z  };
    SYS->set3DListenerAttributes(0, &p, &vel, &f, &u);
    SYS->update();
#endif
}

void AudioManager::shutdown()
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (auto* s : m_sounds)    if (s) reinterpret_cast<FMOD::Sound*>(s)->release();
    m_sounds.clear(); m_sfxChannels.clear();
    m_soundPaths.clear(); m_soundFailureReported.clear();
    // Los de la caché también: un init() posterior encontraría el mapa
    // apuntando a slots que ya no existen y devolvería ids de sonidos muertos.
    m_soundRefs.clear(); m_soundKeys.clear(); m_freeSlots.clear();
    m_soundByKey.clear();
    if (SFXG) SFXG->release();
    if (MUSICG) MUSICG->release();
    SYS->close();
    SYS->release();
    m_system = m_sfxGroup = m_musicGroup = nullptr;
#endif
}

#ifdef DT_FMOD_ENABLED
std::string AudioManager::soundKey(const std::string& path, bool is3D, bool loop,
                                    AudioLoadMode loadMode)
{
    // Los flags delante del path: el path puede contener cualquier cosa, así que
    // el separador va donde no pueda colisionar con su contenido. El modo de
    // carga entra en la clave como is3D y loop: un sonido en streaming y el
    // mismo fichero descomprimido en RAM son dos FMOD::Sound distintos.
    return std::string(is3D ? "3" : "2") + (loop ? "L" : "N")
         + (loadMode == AudioLoadMode::Stream ? "S|" : "M|") + path;
}
#endif

size_t AudioManager::loadedSoundCount() const
{
#ifdef DT_FMOD_ENABLED
    size_t n = 0;
    for (auto* s : m_sounds) if (s) ++n;
    return n;
#else
    return 0;
#endif
}

size_t AudioManager::soundSlotCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_sounds.size();
#else
    return 0;
#endif
}

bool AudioManager::isSoundStreaming(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return false;
    FMOD_MODE mode = 0;
    if (reinterpret_cast<FMOD::Sound*>(m_sounds[id])->getMode(&mode) != FMOD_OK) return false;
    return (mode & FMOD_CREATESTREAM) != 0;
#else
    (void)id;
    return false;
#endif
}

int AudioManager::loadSound(const std::string& path, bool is3D, bool loop, AudioLoadMode loadMode)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;

    // ¿Ya está cargado ese mismo fichero con el mismo modo? Entonces se comparte
    // el FMOD::Sound y solo sube el contador. Veinte objetos con el mismo
    // disparo eran veinte copias descomprimidas en memoria.
    const std::string key = soundKey(path, is3D, loop, loadMode);
    if (auto it = m_soundByKey.find(key); it != m_soundByKey.end())
    {
        const int cached = it->second;
        // El mapa podría tener una entrada rancia si algo la dejó sin limpiar;
        // se comprueba el slot antes de devolverlo en vez de confiar.
        if (cached >= 0 && cached < (int)m_sounds.size() && m_sounds[cached])
        {
            ++m_soundRefs[cached];
            return cached;
        }
        m_soundByKey.erase(it);
    }

    // FMOD arranca con FMOD_INIT_NORMAL (línea 29), que es la API thread-safe.
    // NONBLOCKING descarga la lectura y decodificación al hilo interno de FMOD:
    // createSound retorna al instante y no escribimos ni una línea de código de
    // concurrencia. Es por lo que el audio no pasa por el JobSystem.
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D)
                   | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF)
                   | FMOD_NONBLOCKING;
    // CREATESTREAM: el fichero se lee y decodifica sobre la marcha en vez de
    // descomprimirse entero en RAM. A cambio, el sonido solo admite UNA voz a la
    // vez (un solo buffer de decodificación), que es la razón por la que este
    // modo es para música y no para efectos.
    if (loadMode == AudioLoadMode::Stream) mode |= FMOD_CREATESTREAM;
    FMOD::Sound* snd;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    // Slot reciclado si lo hay: los ids no se reutilizaban nunca y los vectores
    // crecían una entrada por clip en CADA ciclo Play->Stop (que recrea la
    // escena entera desde el snapshot).
    int id;
    if (!m_freeSlots.empty())
    {
        id = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_sounds[id]                = snd;
        m_sfxChannels[id]           = nullptr;
        m_soundPaths[id]            = path;
        m_soundFailureReported[id]  = 0;
        m_soundRefs[id]             = 1;
        m_soundKeys[id]             = key;
    }
    else
    {
        m_sounds.push_back(snd);
        m_sfxChannels.push_back(nullptr);
        m_soundPaths.push_back(path);
        m_soundFailureReported.push_back(0);
        m_soundRefs.push_back(1);
        m_soundKeys.push_back(key);
        id = (int)m_sounds.size() - 1;
    }
    m_soundByKey[key] = id;
    return id;
#else
    (void)loop;
    return -1;
#endif
}

void AudioManager::unloadSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;

    // Cuenta de referencias: el sonido lo pueden estar usando varios
    // AudioClipComponent (misma ruta y mismo modo). Soltarlo con el primer
    // destructor sería un use-after-free para todos los demás.
    if (--m_soundRefs[id] > 0) return;

    reinterpret_cast<FMOD::Sound*>(m_sounds[id])->release();
    m_sounds[id] = nullptr;
    m_sfxChannels[id] = nullptr;
    m_soundPaths[id].clear();
    m_soundFailureReported[id] = 0;
    m_soundRefs[id] = 0;
    // Fuera del mapa ANTES de marcar el slot como libre: si no, un loadSound
    // posterior con esa misma clave encontraría la entrada rancia apuntando a un
    // slot que ya es de otro sonido.
    m_soundByKey.erase(m_soundKeys[id]);
    m_soundKeys[id].clear();
    m_freeSlots.push_back(id);
#endif
}

AudioManager::SoundLoadState AudioManager::getSoundState(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id])
        return SoundLoadState::Missing;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_OPENSTATE st;
    // Cuando la carga falla, FMOD devuelve el código del error como valor de
    // retorno de getOpenState y *st puede no haberse escrito, así que se mira
    // el retorno PRIMERO. Comprobado con las dos formas de fallo que se dan en
    // la práctica —path inexistente y fichero que existe pero no es audio—: las
    // dos salen por aquí (ver los dos tests de audio_tests.cpp, que se
    // escribieron con un sabotaje a esta línea).
    if (snd->getOpenState(&st, nullptr, nullptr, nullptr) != FMOD_OK)
        return SoundLoadState::Failed;
    if (st == FMOD_OPENSTATE_LOADING) return SoundLoadState::Loading;
    // Defensiva y SIN cobertura de test: no se ha conseguido provocar un
    // getOpenState que devuelva FMOD_OK con el estado en ERROR (los dos
    // sabotajes a esta línea pasaron los tests). Se deja porque la doc de FMOD
    // define el estado y quitarla sería apostar a que nunca ocurre; no se
    // presenta como camino probado.
    if (st == FMOD_OPENSTATE_ERROR)   return SoundLoadState::Failed;
    return SoundLoadState::Ready;
#else
    (void)id;
    return SoundLoadState::Missing;
#endif
}

void AudioManager::pollLoadFailures(std::vector<std::string>& out)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (size_t i = 0; i < m_sounds.size(); ++i)
    {
        if (!m_sounds[i] || m_soundFailureReported[i]) continue;
        if (getSoundState((int)i) != SoundLoadState::Failed) continue;
        m_soundFailureReported[i] = 1;
        out.push_back(m_soundPaths[i]);
    }
#else
    (void)out;
#endif
}

#ifdef DT_FMOD_ENABLED
// El Channel* guardado para id, sólo si sigue sonando y sigue siendo el canal
// de ESE sonido. Devuelve nullptr en cualquier otro caso.
static FMOD::Channel* liveChannel(void* raw, void* expectedSound)
{
    auto* ch = reinterpret_cast<FMOD::Channel*>(raw);
    if (!ch) return nullptr;

    bool playing = false;
    if (ch->isPlaying(&playing) != FMOD_OK || !playing) return nullptr;

    FMOD::Sound* current = nullptr;
    if (ch->getCurrentSound(&current) != FMOD_OK) return nullptr;
    if (current != reinterpret_cast<FMOD::Sound*>(expectedSound)) return nullptr;

    return ch;
}
#endif

void AudioManager::setChannelVolume(int id, float volume)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setVolume(volume);
#else
    (void)id; (void)volume;
#endif
}

void AudioManager::setChannelPitch(int id, float pitch)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPitch(pitch);
#else
    (void)id; (void)pitch;
#endif
}

void AudioManager::setSound3DMinMaxDistance(int id, float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (!(mode & FMOD_3D)) return;
    // SOLO al canal, nunca al FMOD::Sound. Antes se escribía en los dos, y eso
    // dejó de ser correcto en cuanto el sonido se comparte entre varios
    // AudioClipComponent: ajustar la atenuación de un altavoz le cambiaría el
    // radio a todos los demás objetos que usen el mismo fichero.
    //
    // La contrapartida es que un sonido recién creado ya no hereda estas
    // distancias: las aplica AudioClipComponent::play, que llama a
    // applyDistances() justo después de arrancar la voz.
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->set3DMinMaxDistance(minDistance, maxDistance);
#else
    (void)id; (void)minDistance; (void)maxDistance;
#endif
}

void AudioManager::playSound(int id, const glm::vec3& worldPos, float volume, float pitch,
                              AudioBus bus, float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // Con NONBLOCKING, el Sound existe pero puede estar todavía cargando. Un
    // playSound sobre él devuelve FMOD_ERR_NOTREADY. Se ignora en silencio en
    // vez de escupir un error: el usuario ha pedido reproducir algo que aún no
    // está, y el caso normal es que dé a Play nada más soltar el fichero.
    FMOD_OPENSTATE state;
    if (snd->getOpenState(&state, nullptr, nullptr, nullptr) == FMOD_OK
        && state == FMOD_OPENSTATE_LOADING)
        return;
    // paused = true: hay que dejar volumen, pitch y posición puestos ANTES de
    // que suene la primera muestra. Arrancándolo sonando, un clip 3D se oye un
    // instante desde el origen del mundo y con el volumen del canal anterior.
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (SYS->playSound(snd, group, true, &ch) != FMOD_OK) return;
    // Si el id ya tenía una voz sonando de una reproducción anterior, se para
    // antes de pisar la referencia: si no, ese canal queda huérfano (sin
    // referencia) y sigue sonando indefinidamente si tiene loop, sin que
    // stop() ni los setters de volumen/pitch puedan alcanzarlo ya. Misma
    // semántica que AudioSource.Play() en Unity: un Play() nuevo corta el
    // anterior.
    if (FMOD::Channel* prev = liveChannel(m_sfxChannels[id], m_sounds[id])) prev->stop();
    m_sfxChannels[id] = ch;

    ch->setVolume(volume);
    ch->setPitch(pitch);

    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
        // A la VOZ, no al FMOD::Sound: el sonido se comparte entre clips y
        // escribirlas alli le cambiaria el radio a todos los demas.
        ch->set3DMinMaxDistance(minDistance, maxDistance);
    }

    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}

bool AudioManager::isSoundPlaying(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    return liveChannel(m_sfxChannels[id], m_sounds[id]) != nullptr;
#else
    (void)id;
    return false;
#endif
}

bool AudioManager::isSoundPaused(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return false;
    bool paused = false;
    if (ch->getPaused(&paused) != FMOD_OK) return false;
    return paused;
#else
    (void)id;
    return false;
#endif
}

void AudioManager::setSoundPaused(int id, bool paused)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPaused(paused);
#else
    (void)id; (void)paused;
#endif
}

void AudioManager::setSoundPosition(int id, const glm::vec3& worldPos)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_MODE mode = 0; snd->getMode(&mode);
    // En 2D no hay nada que posicionar: set3DAttributes sobre una voz 2D no
    // hace nada útil y este método se llama por frame y por clip.
    if (!(mode & FMOD_3D)) return;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return;
    FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
    // Velocidad a cero, igual que en playSound y que el listener: sin doppler.
    // Derivarla de la posición del frame anterior es lo que haría falta, y es
    // una feature aparte (un teleport daría un chirrido si se hiciera a la
    // ligera).
    FMOD_VECTOR v = { 0, 0, 0 };
    ch->set3DAttributes(&p, &v);
#else
    (void)id; (void)worldPos;
#endif
}

void AudioManager::playSoundOneShot(int id, const glm::vec3& worldPos, float volume, float pitch,
                                     AudioBus bus, float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // Mismo silencio que playSound mientras el sonido sigue cargando: no es un
    // error, es que aún no está.
    FMOD_OPENSTATE state;
    if (snd->getOpenState(&state, nullptr, nullptr, nullptr) == FMOD_OK
        && state == FMOD_OPENSTATE_LOADING)
        return;

    FMOD::Channel* ch;
    // paused = true por lo mismo que en playSound: volumen, pitch y posición
    // tienen que estar puestos antes de la primera muestra.
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (SYS->playSound(snd, group, true, &ch) != FMOD_OK) return;

    // Y AQUÍ la diferencia con playSound: ni se para la voz anterior ni se
    // guarda esta en m_sfxChannels. Es lo que permite el solapamiento.
    ch->setVolume(volume);
    ch->setPitch(pitch);

    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
        // A la VOZ, no al FMOD::Sound: el sonido se comparte entre clips y
        // escribirlas alli le cambiaria el radio a todos los demas.
        ch->set3DMinMaxDistance(minDistance, maxDistance);
    }
    // Sin referencia guardada, esta voz tampoco la alcanza el seguimiento 3D
    // por frame (setSoundPosition): un one-shot suena donde se disparó. Para
    // clips cortos —que es su caso de uso— la diferencia no se oye.
    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}

void AudioManager::stopSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || id >= (int)m_sounds.size()) return;
    // Vía liveChannel, igual que los setters de volumen/pitch: FMOD recicla los
    // Channel* de las voces que terminan, así que el puntero guardado aquí puede
    // apuntar ya a la voz de OTRO sonido. Parándolo a ciegas, un stopSound(id)
    // sobre un clip que hace rato que acabó corta el que esté sonando ahora.
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id])) ch->stop();
#else
    (void)id;
#endif
}

// SIN COBERTURA DE TEST, y conviene saberlo: que playSound mande la voz al
// grupo correcto no se puede observar desde un test headless — FMOD no expone
// "por qué bus está sonando esto" de una forma que no obligue a inventar un
// getter que nadie más usaría. Un sabotaje que ignorara el bus y enrutara todo
// a SFX pasa la suite entera. Lo que sí está cubierto es todo lo demás del
// camino: el round-trip del bus por el .scene, la back-compat, el nombre
// desconocido, y que los tres volúmenes son independientes.
//
// Verificación manual: poner un clip en Music, bajar Music Volume a 0 y
// comprobar que enmudece mientras otro clip en SFX se sigue oyendo.
#ifdef DT_FMOD_ENABLED
void* AudioManager::groupForBus(AudioBus bus) const
{
    if (!m_system) return nullptr;
    switch (bus)
    {
        case AudioBus::Music: return m_musicGroup;
        case AudioBus::Sfx:   return m_sfxGroup;
        case AudioBus::Master:
        default:
        {
            // El master no se crea aquí: lo da FMOD y es el padre de los otros
            // dos, así que su volumen escala a ambos.
            FMOD::ChannelGroup* master = nullptr;
            if (SYS->getMasterChannelGroup(&master) != FMOD_OK) return nullptr;
            return master;
        }
    }
}
#endif

void AudioManager::setBusVolume(AudioBus bus, float v)
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = groupForBus(bus))
        reinterpret_cast<FMOD::ChannelGroup*>(g)->setVolume(v);
#else
    (void)bus; (void)v;
#endif
}

float AudioManager::getBusVolume(AudioBus bus) const
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = groupForBus(bus))
    {
        float v = 1.0f;
        if (reinterpret_cast<FMOD::ChannelGroup*>(g)->getVolume(&v) == FMOD_OK) return v;
    }
    // Sin sistema (o si FMOD falla) el neutro: es lo que la UI debe dibujar
    // para no sugerir que el audio está bajado cuando lo que pasa es que no hay
    // audio.
    return 1.0f;
#else
    (void)bus;
    return 1.0f;
#endif
}


std::shared_ptr<AudioClipComponent> AudioManager::createAudioClipComponent(
    const std::string& path, bool is3D, bool loop, AudioLoadMode loadMode)
{
    int id = loadSound(path, is3D, loop, loadMode);
    if (id < 0) return nullptr;
    return std::make_shared<AudioClipComponent>(this, path, id, is3D, loop, loadMode);
}

} // namespace DonTopo
