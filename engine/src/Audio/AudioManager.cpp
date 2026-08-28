#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"

#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#include <fmod_errors.h>
#include <algorithm>
#include <cmath>
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

#ifdef DT_FMOD_ENABLED
namespace {
// Tope de velocidad, en unidades de mundo por segundo. Por encima de esto no se
// cree el dato: un teleport, una carga de escena o un frame larguísimo darían
// una velocidad absurda, y el doppler la convertiría en un chirrido que dura lo
// que dure la voz. Las primitivas de este repo miden 50 unidades, así que 2000
// u/s es rapidísimo pero todavía plausible para un proyectil.
constexpr float kMaxSourceSpeed = 2000.0f;

// Velocidad entre dos posiciones, o cero si no es de fiar (dt no positivo,
// primer frame, o salto demasiado grande para ser movimiento real).
glm::vec3 safeVelocity(const glm::vec3& current, const glm::vec3& last, bool hasLast, float dt)
{
    if (!hasLast || dt <= 0.0f) return glm::vec3(0.0f);
    const glm::vec3 v = (current - last) / dt;
    const float speed = glm::length(v);
    if (!std::isfinite(speed) || speed > kMaxSourceSpeed) return glm::vec3(0.0f);
    return v;
}
} // namespace
#endif

void AudioManager::update(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up, float dt)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    // La velocidad del listener es la mitad del doppler (la otra es la de cada
    // fuente). Antes iba fija a cero, así que no había efecto por mucho que se
    // moviera la cámara.
    const glm::vec3 v = safeVelocity(pos, m_lastListenerPos, m_hasLastListenerPos, dt);
    m_lastListenerPos = pos;
    m_hasLastListenerPos = true;

    FMOD_VECTOR p   = { pos.x, pos.y, pos.z };
    FMOD_VECTOR vel = { v.x, v.y, v.z };
    FMOD_VECTOR f   = { fwd.x, fwd.y, fwd.z };
    FMOD_VECTOR u   = { up.x,  up.y,  up.z  };
    SYS->set3DListenerAttributes(0, &p, &vel, &f, &u);
    SYS->update();
#else
    (void)dt;
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
    m_soundByKey.clear(); m_pinnedSounds.clear();
    m_soundLastPos.clear(); m_soundHasLastPos.clear();
    // Los DSP ANTES que los grupos de los que cuelgan: liberar el grupo primero
    // dejaria los DSP colgando de algo que ya no existe. Son recursos nativos,
    // no punteros sueltos.
    for (auto& [key, dsp] : m_busEffects)
        if (dsp) reinterpret_cast<FMOD::DSP*>(dsp)->release();
    m_busEffects.clear();
    // Las zonas de reverb son otro recurso nativo con el mismo trato.
    clearReverbZones();
    if (SFXG) SFXG->release();
    if (MUSICG) MUSICG->release();
    SYS->close();
    SYS->release();
    m_system = m_sfxGroup = m_musicGroup = nullptr;
#endif
}

#ifdef DT_FMOD_ENABLED
std::string AudioManager::soundKey(const std::string& path, bool is3D, bool loop,
                                    AudioLoadMode loadMode, AudioRolloff rolloff)
{
    // Los flags delante del path: el path puede contener cualquier cosa, así que
    // el separador va donde no pueda colisionar con su contenido. El modo de
    // carga entra en la clave como is3D y loop: un sonido en streaming y el
    // mismo fichero descomprimido en RAM son dos FMOD::Sound distintos.
    // El rolloff tambien va en el FMOD_MODE, asi que entra en la clave: el
    // mismo fichero con curva lineal y con curva inversa son dos sonidos.
    return std::string(is3D ? "3" : "2") + (loop ? "L" : "N")
         + (loadMode == AudioLoadMode::Stream ? "S" : "M")
         + audioRolloffToStr(rolloff)[0] + "|" + path;
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

AudioRolloff AudioManager::getSoundRolloff(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id])
        return AudioRolloff::Inverse;
    FMOD_MODE mode = 0;
    if (reinterpret_cast<FMOD::Sound*>(m_sounds[id])->getMode(&mode) != FMOD_OK)
        return AudioRolloff::Inverse;
    if (mode & FMOD_3D_LINEARSQUAREROLLOFF) return AudioRolloff::LinearSquare;
    if (mode & FMOD_3D_LINEARROLLOFF)       return AudioRolloff::Linear;
    // Sin flag explicito FMOD usa la inversa, que es nuestro default.
    return AudioRolloff::Inverse;
#else
    (void)id;
    return AudioRolloff::Inverse;
#endif
}

int AudioManager::loadSound(const std::string& path, bool is3D, bool loop, AudioLoadMode loadMode,
                             AudioRolloff rolloff)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;

    // ¿Ya está cargado ese mismo fichero con el mismo modo? Entonces se comparte
    // el FMOD::Sound y solo sube el contador. Veinte objetos con el mismo
    // disparo eran veinte copias descomprimidas en memoria.
    const std::string key = soundKey(path, is3D, loop, loadMode, rolloff);
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
    // Curva de atenuación. Sin ninguno de estos flags FMOD aplica la inversa,
    // que es justo AudioRolloff::Inverse: por eso ese caso no añade nada.
    if (rolloff == AudioRolloff::Linear)            mode |= FMOD_3D_LINEARROLLOFF;
    else if (rolloff == AudioRolloff::LinearSquare) mode |= FMOD_3D_LINEARSQUAREROLLOFF;
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
        m_soundHasLastPos[id]       = 0;
    }
    else
    {
        m_sounds.push_back(snd);
        m_sfxChannels.push_back(nullptr);
        m_soundPaths.push_back(path);
        m_soundFailureReported.push_back(0);
        m_soundRefs.push_back(1);
        m_soundKeys.push_back(key);
        m_soundLastPos.push_back(glm::vec3(0.0f));
        m_soundHasLastPos.push_back(0);
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
    m_soundHasLastPos[id] = 0;
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
                              AudioBus bus, float minDistance, float maxDistance,
                              float spread, float stereoPan, float dopplerLevel)
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
        // Ensanchado estereo y sensibilidad al doppler, tambien por voz.
        ch->set3DSpread(spread);
        ch->set3DDopplerLevel(dopplerLevel);
    }
    // El paneo manual solo tiene sentido en 2D: en 3D lo decide la posicion, y
    // escribirlo ahi pelearia con el paneo espacial de FMOD. Fuera del if, con
    // su propia condicion, para que quede claro que NO es una propiedad 3D.
    if (!(mode & FMOD_3D) && stereoPan != 0.0f)
        ch->setPan(stereoPan);

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

void AudioManager::setSoundMute(int id, bool mute)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setMute(mute);
#else
    (void)id; (void)mute;
#endif
}

float AudioManager::getSoundTime(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return -1.0f;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return -1.0f;
    unsigned int ms = 0;
    if (ch->getPosition(&ms, FMOD_TIMEUNIT_MS) != FMOD_OK) return -1.0f;
    return (float)ms / 1000.0f;
#else
    (void)id;
    return -1.0f;
#endif
}

void AudioManager::setSoundTime(int id, float seconds)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (!std::isfinite(seconds) || seconds < 0.0f) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPosition((unsigned int)(seconds * 1000.0f), FMOD_TIMEUNIT_MS);
#else
    (void)id; (void)seconds;
#endif
}

void AudioManager::setAudioPaused(bool paused)
{
#ifdef DT_FMOD_ENABLED
    // Sobre el master, no sobre cada bus: los otros dos cuelgan de él, así que
    // uno solo los congela a todos —incluidas las voces sueltas de PlayOneShot,
    // que no se pueden alcanzar de otra forma.
    if (auto* g = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(AudioBus::Master)))
        g->setPaused(paused);
#else
    (void)paused;
#endif
}

bool AudioManager::isAudioPaused() const
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(AudioBus::Master)))
    {
        bool paused = false;
        if (g->getPaused(&paused) == FMOD_OK) return paused;
    }
    return false;
#else
    return false;
#endif
}

void AudioManager::setSoundPosition(int id, const glm::vec3& worldPos, float dt)
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
    // La velocidad de la fuente sale de su posición del frame anterior; es la
    // otra mitad del doppler (la del listener la lleva update()). safeVelocity
    // descarta el primer frame y los saltos imposibles, que es lo que evita el
    // chirrido de un teleport.
    const glm::vec3 vel = safeVelocity(worldPos, m_soundLastPos[id],
                                        m_soundHasLastPos[id] != 0, dt);
    m_soundLastPos[id]    = worldPos;
    m_soundHasLastPos[id] = 1;

    FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
    FMOD_VECTOR v = { vel.x, vel.y, vel.z };
    ch->set3DAttributes(&p, &v);
#else
    (void)id; (void)worldPos;
#endif
}

void AudioManager::playSoundOneShot(int id, const glm::vec3& worldPos, float volume, float pitch,
                                     AudioBus bus, float minDistance, float maxDistance,
                                     float spread, float stereoPan, float dopplerLevel)
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
        // Ensanchado estereo y sensibilidad al doppler, tambien por voz.
        ch->set3DSpread(spread);
        ch->set3DDopplerLevel(dopplerLevel);
    }
    // El paneo manual solo tiene sentido en 2D: en 3D lo decide la posicion, y
    // escribirlo ahi pelearia con el paneo espacial de FMOD. Fuera del if, con
    // su propia condicion, para que quede claro que NO es una propiedad 3D.
    if (!(mode & FMOD_3D) && stereoPan != 0.0f)
        ch->setPan(stereoPan);
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

#ifdef DT_FMOD_ENABLED
// Carga (o reutiliza) el sonido de una ruta y lo deja RETENIDO en la caché,
// devolviendo su id. Lo comparten preloadClip y playClipAtPoint: tener un solo
// sitio que cargue evita el doble loadSound que había aquí antes, con su
// unloadSound de compensación detrás — dos llamadas que se anulaban y que solo
// servían para que el refcount cuadrara.
//
// 3D y sin bucle: es el perfil de un one-shot posicional. El mismo fichero en
// 2D es otro sonido, y lo carga el AudioClipComponent que lo pida.
int AudioManager::acquirePinnedSound(const std::string& path)
{
    if (!m_system) return -1;
    const int id = loadSound(path, /*is3D=*/true, /*loop=*/false, AudioLoadMode::Sample);
    if (id < 0) return -1;
    // insert devuelve false si ya estaba retenido: ese loadSound solo ha subido
    // el refcount otra vez, y se deshace para que el pin siga contando UNA sola
    // referencia. Sin esto el sonido seguiría vivo igual (el pin no se suelta
    // nunca), pero el contador crecería sin techo y dejaría de describir la
    // realidad para cualquiera que lo mire después.
    if (!m_pinnedSounds.insert(id).second)
        unloadSound(id);
    return id;
}
#endif

void AudioManager::preloadClip(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    acquirePinnedSound(path);
#else
    (void)path;
#endif
}

void AudioManager::playClipAtPoint(const std::string& path, const glm::vec3& worldPos,
                                    float volume, float pitch, AudioBus bus,
                                    float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    const int id = acquirePinnedSound(path);
    if (id < 0) return;
    playSoundOneShot(id, worldPos, volume, pitch, bus, minDistance, maxDistance);
#else
    (void)path; (void)worldPos; (void)volume; (void)pitch;
    (void)bus; (void)minDistance; (void)maxDistance;
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

#ifdef DT_FMOD_ENABLED
namespace {
// Tipo de DSP de FMOD para cada efecto nuestro. Todos son de FMOD Core: nada de
// esto necesita FMOD Studio.
FMOD_DSP_TYPE fmodDspType(AudioEffect e)
{
    switch (e)
    {
        case AudioEffect::HighPass: return FMOD_DSP_TYPE_HIGHPASS;
        case AudioEffect::Echo:     return FMOD_DSP_TYPE_ECHO;
        case AudioEffect::Reverb:   return FMOD_DSP_TYPE_SFXREVERB;
        case AudioEffect::LowPass:
        default:                    return FMOD_DSP_TYPE_LOWPASS;
    }
}

// Traduce el [0, 1] de la API pública a las unidades de cada DSP. En UN SOLO
// sitio: si esto se repartiera por la UI y por Lua, los dos rangos acabarían
// desincronizados en cuanto alguien tocara uno.
void applyEffectAmount(FMOD::DSP* dsp, AudioEffect e, float amount)
{
    const float a = std::clamp(amount, 0.0f, 1.0f);
    switch (e)
    {
        case AudioEffect::LowPass:
            // 0 -> muy cerrado (400 Hz, "debajo del agua"); 1 -> casi
            // transparente (22 kHz). Logarítmico porque el oído lo es: lineal
            // dejaba todo el efecto apelotonado en el último 10% del slider.
            dsp->setParameterFloat(FMOD_DSP_LOWPASS_CUTOFF,
                                   400.0f * std::pow(55.0f, a));
            break;
        case AudioEffect::HighPass:
            // Al revés: 0 no corta nada, 1 se lleva todos los graves.
            dsp->setParameterFloat(FMOD_DSP_HIGHPASS_CUTOFF,
                                   10.0f * std::pow(500.0f, a));
            break;
        case AudioEffect::Echo:
            // Retardo entre repeticiones, de casi seguido a casi medio segundo.
            dsp->setParameterFloat(FMOD_DSP_ECHO_DELAY, 10.0f + a * 490.0f);
            break;
        case AudioEffect::Reverb:
            // Tamaño de la cola, de una sala pequeña a una catedral.
            dsp->setParameterFloat(FMOD_DSP_SFXREVERB_DECAYTIME, 100.0f + a * 9900.0f);
            break;
    }
}
} // namespace
#endif

const std::vector<std::string>& AudioManager::reverbPresetNames()
{
    // Un subconjunto de los FMOD_PRESET_*: los ambientes que se piden de
    // verdad. Ampliar la lista es añadir el nombre aquí y su caso en
    // fmodReverbPreset. El orden es el del combo del inspector.
    static const std::vector<std::string> names = {
        "off", "generic", "room", "bathroom", "stoneroom", "auditorium",
        "concerthall", "cave", "arena", "hangar", "hallway", "alley",
        "forest", "city", "mountains", "quarry", "underwater"
    };
    return names;
}

#ifdef DT_FMOD_ENABLED
namespace {
// Nombre -> properties de FMOD. Devuelve false si el nombre no existe, para que
// quien llama pueda avisar en vez de instalar un ambiente arbitrario.
bool fmodReverbPreset(const std::string& name, FMOD_REVERB_PROPERTIES& out)
{
    if (name == "off")         { out = FMOD_PRESET_OFF;         return true; }
    if (name == "generic")     { out = FMOD_PRESET_GENERIC;     return true; }
    if (name == "room")        { out = FMOD_PRESET_ROOM;        return true; }
    if (name == "bathroom")    { out = FMOD_PRESET_BATHROOM;    return true; }
    if (name == "stoneroom")   { out = FMOD_PRESET_STONEROOM;   return true; }
    if (name == "auditorium")  { out = FMOD_PRESET_AUDITORIUM;  return true; }
    if (name == "concerthall") { out = FMOD_PRESET_CONCERTHALL; return true; }
    if (name == "cave")        { out = FMOD_PRESET_CAVE;        return true; }
    if (name == "arena")       { out = FMOD_PRESET_ARENA;       return true; }
    if (name == "hangar")      { out = FMOD_PRESET_HANGAR;      return true; }
    if (name == "hallway")     { out = FMOD_PRESET_HALLWAY;     return true; }
    if (name == "alley")       { out = FMOD_PRESET_ALLEY;       return true; }
    if (name == "forest")      { out = FMOD_PRESET_FOREST;      return true; }
    if (name == "city")        { out = FMOD_PRESET_CITY;        return true; }
    if (name == "mountains")   { out = FMOD_PRESET_MOUNTAINS;   return true; }
    if (name == "quarry")      { out = FMOD_PRESET_QUARRY;      return true; }
    if (name == "underwater")  { out = FMOD_PRESET_UNDERWATER;  return true; }
    return false;
}
} // namespace
#endif

bool AudioManager::syncReverbZone(uint64_t ownerId, const glm::vec3& worldPos,
                                   float minDistance, float maxDistance,
                                   const std::string& preset, bool enabled)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return false;

    FMOD_REVERB_PROPERTIES props;
    // El preset se valida ANTES de crear nada: con un nombre inventado no se
    // instala una zona con un ambiente cualquiera, se dice que no y ya.
    if (!fmodReverbPreset(preset, props)) return false;

    FMOD::Reverb3D* zone = nullptr;
    if (auto it = m_reverbZones.find(ownerId); it != m_reverbZones.end())
    {
        zone = reinterpret_cast<FMOD::Reverb3D*>(it->second);
    }
    else
    {
        // FMOD limita cuántas instancias 3D puede haber a la vez; si no da más,
        // se devuelve false y quien llama decide si avisar.
        if (SYS->createReverb3D(&zone) != FMOD_OK || !zone) return false;
        m_reverbZones[ownerId] = zone;
    }

    FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
    zone->set3DAttributes(&p, minDistance, maxDistance);
    zone->setProperties(&props);
    // Deshabilitada se queda creada pero sin efecto: así alternar la casilla no
    // cuesta crear y destruir el recurso cada vez.
    zone->setActive(enabled);
    return true;
#else
    (void)ownerId; (void)worldPos; (void)minDistance; (void)maxDistance;
    (void)preset; (void)enabled;
    return false;
#endif
}

void AudioManager::removeReverbZone(uint64_t ownerId)
{
#ifdef DT_FMOD_ENABLED
    auto it = m_reverbZones.find(ownerId);
    if (it == m_reverbZones.end()) return;
    reinterpret_cast<FMOD::Reverb3D*>(it->second)->release();
    m_reverbZones.erase(it);
#else
    (void)ownerId;
#endif
}

void AudioManager::retainReverbZones(const std::vector<uint64_t>& aliveOwnerIds)
{
#ifdef DT_FMOD_ENABLED
    if (m_reverbZones.empty()) return;
    for (auto it = m_reverbZones.begin(); it != m_reverbZones.end(); )
    {
        const bool alive = std::find(aliveOwnerIds.begin(), aliveOwnerIds.end(), it->first)
                            != aliveOwnerIds.end();
        if (alive) { ++it; continue; }
        // El GameObject que la sostenía ya no está: sin esto su reverb seguiría
        // aplicándose al resto de la escena para siempre.
        if (it->second) reinterpret_cast<FMOD::Reverb3D*>(it->second)->release();
        it = m_reverbZones.erase(it);
    }
#else
    (void)aliveOwnerIds;
#endif
}

void AudioManager::clearReverbZones()
{
#ifdef DT_FMOD_ENABLED
    for (auto& [id, zone] : m_reverbZones)
        if (zone) reinterpret_cast<FMOD::Reverb3D*>(zone)->release();
    m_reverbZones.clear();
#endif
}

size_t AudioManager::reverbZoneCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_reverbZones.size();
#else
    return 0;
#endif
}

void AudioManager::setBusEffect(AudioBus bus, AudioEffect effect, float amount)
{
#ifdef DT_FMOD_ENABLED
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (!group) return;

    const int key = effectKey(bus, effect);
    // Ya colgado: solo se reajusta el mando. Sin esta rama, mover un slider
    // encadenaría un DSP nuevo por frame hasta ahogar el mezclador.
    if (auto it = m_busEffects.find(key); it != m_busEffects.end())
    {
        applyEffectAmount(reinterpret_cast<FMOD::DSP*>(it->second), effect, amount);
        return;
    }

    FMOD::DSP* dsp = nullptr;
    if (SYS->createDSPByType(fmodDspType(effect), &dsp) != FMOD_OK || !dsp) return;
    applyEffectAmount(dsp, effect, amount);
    // Si el DSP no se puede enganchar hay que liberarlo aquí mismo: guardarlo
    // sin conectar dejaría un recurso nativo vivo que nadie volvería a mirar.
    if (group->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, dsp) != FMOD_OK)
    {
        dsp->release();
        return;
    }
    m_busEffects[key] = dsp;
#else
    (void)bus; (void)effect; (void)amount;
#endif
}

void AudioManager::clearBusEffect(AudioBus bus, AudioEffect effect)
{
#ifdef DT_FMOD_ENABLED
    const int key = effectKey(bus, effect);
    auto it = m_busEffects.find(key);
    if (it == m_busEffects.end()) return;
    auto* dsp = reinterpret_cast<FMOD::DSP*>(it->second);
    // Desconectar ANTES de liberar: soltar un DSP todavía enganchado al grupo
    // deja al mezclador con un puntero muerto en su cadena.
    if (auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus)))
        group->removeDSP(dsp);
    dsp->release();
    m_busEffects.erase(it);
#else
    (void)bus; (void)effect;
#endif
}

void AudioManager::clearBusEffects(AudioBus bus)
{
#ifdef DT_FMOD_ENABLED
    for (AudioEffect e : { AudioEffect::LowPass, AudioEffect::HighPass,
                            AudioEffect::Echo, AudioEffect::Reverb })
        clearBusEffect(bus, e);
#else
    (void)bus;
#endif
}

size_t AudioManager::busDspCount(AudioBus bus) const
{
#ifdef DT_FMOD_ENABLED
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (!group) return 0;
    int n = 0;
    if (group->getNumDSPs(&n) != FMOD_OK || n < 0) return 0;
    return (size_t)n;
#else
    (void)bus;
    return 0;
#endif
}

size_t AudioManager::activeEffectCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_busEffects.size();
#else
    return 0;
#endif
}

bool AudioManager::hasBusEffect(AudioBus bus, AudioEffect effect) const
{
#ifdef DT_FMOD_ENABLED
    return m_busEffects.find(effectKey(bus, effect)) != m_busEffects.end();
#else
    (void)bus; (void)effect;
    return false;
#endif
}

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
