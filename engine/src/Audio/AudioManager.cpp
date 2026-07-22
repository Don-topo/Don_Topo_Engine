#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"

#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#include <fmod_errors.h>
#include <stdexcept>

#define SYS   reinterpret_cast<FMOD::System*>(m_system)
#define SFXG  reinterpret_cast<FMOD::ChannelGroup*>(m_sfxGroup)
#define BGMG  reinterpret_cast<FMOD::ChannelGroup*>(m_bgmGroup)
#define BGMCH reinterpret_cast<FMOD::Channel*>(m_bgmCh)

static void fmodCheck(FMOD_RESULT r, const char* ctx) {
    if (r != FMOD_OK)
        throw std::runtime_error(std::string(ctx) + ": " + FMOD_ErrorString(r));
}
#endif

namespace DonTopo {

AudioManager::~AudioManager() { shutdown(); }

void AudioManager::init()
{
#ifdef DT_FMOD_ENABLED
    FMOD::System* sys;
    fmodCheck(FMOD::System_Create(&sys), "FMOD::System_Create");
    fmodCheck(sys->init(512, FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED, nullptr), "FMOD init");

    FMOD::ChannelGroup* sfx;
    FMOD::ChannelGroup* bgm;
    fmodCheck(sys->createChannelGroup("SFX", &sfx), "createChannelGroup SFX");
    fmodCheck(sys->createChannelGroup("BGM", &bgm), "createChannelGroup BGM");

    m_system   = sys;
    m_sfxGroup = sfx;
    m_bgmGroup = bgm;
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
    for (auto* s : m_bgmSounds) if (s) reinterpret_cast<FMOD::Sound*>(s)->release();
    m_sounds.clear(); m_bgmSounds.clear(); m_sfxChannels.clear();
    if (SFXG) SFXG->release();
    if (BGMG) BGMG->release();
    SYS->close();
    SYS->release();
    m_system = m_sfxGroup = m_bgmGroup = m_bgmCh = nullptr;
#endif
}

int AudioManager::loadSound(const std::string& path, bool is3D, bool loop)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D) | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
    FMOD::Sound* snd;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    m_sounds.push_back(snd);
    m_sfxChannels.push_back(nullptr);
    return (int)m_sounds.size() - 1;
#else
    (void)loop;
    return -1;
#endif
}

void AudioManager::unloadSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    reinterpret_cast<FMOD::Sound*>(m_sounds[id])->release();
    m_sounds[id] = nullptr;
    m_sfxChannels[id] = nullptr;
#endif
}

int AudioManager::loadBGM(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;
    FMOD::Sound* snd;
    FMOD_MODE mode = FMOD_2D | FMOD_LOOP_NORMAL | FMOD_CREATESTREAM;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    m_bgmSounds.push_back(snd);
    return (int)m_bgmSounds.size() - 1;
#else
    return -1;
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

void AudioManager::playSound(int id, const glm::vec3& worldPos, float volume, float pitch)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // paused = true: hay que dejar volumen, pitch y posición puestos ANTES de
    // que suene la primera muestra. Arrancándolo sonando, un clip 3D se oye un
    // instante desde el origen del mundo y con el volumen del canal anterior.
    if (SYS->playSound(snd, SFXG, true, &ch) != FMOD_OK) return;
    // Se descarta a sabiendas el canal de una reproducción anterior de este
    // mismo id: si seguía sonando, queda huérfano (sin referencia) con el
    // volumen/pitch que tenía y termina de sonar por su cuenta.
    m_sfxChannels[id] = ch;

    ch->setVolume(volume);
    ch->setPitch(pitch);

    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
    }

    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}

void AudioManager::stopSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || !m_sfxChannels[id]) return;
    reinterpret_cast<FMOD::Channel*>(m_sfxChannels[id])->stop();
#endif
}

void AudioManager::playBGM(int bgmId)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || bgmId < 0 || bgmId >= (int)m_bgmSounds.size()) return;
    stopBGM();
    FMOD::Channel* ch;
    SYS->playSound(reinterpret_cast<FMOD::Sound*>(m_bgmSounds[bgmId]), BGMG, false, &ch);
    m_bgmCh = ch;
#endif
}

void AudioManager::stopBGM()
{
#ifdef DT_FMOD_ENABLED
    if (BGMCH) { BGMCH->stop(); m_bgmCh = nullptr; }
#endif
}

void AudioManager::pauseBGM(bool paused)
{
#ifdef DT_FMOD_ENABLED
    if (BGMCH) BGMCH->setPaused(paused);
#endif
}

void AudioManager::setMasterVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    FMOD::ChannelGroup* master;
    SYS->getMasterChannelGroup(&master);
    master->setVolume(v);
#endif
}

void AudioManager::setSfxVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (SFXG) SFXG->setVolume(v);
#endif
}

void AudioManager::setBgmVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (BGMG) BGMG->setVolume(v);
#endif
}

std::shared_ptr<AudioClipComponent> AudioManager::createAudioClipComponent(const std::string& path, bool is3D, bool loop)
{
    int id = loadSound(path, is3D, loop);
    if (id < 0) return nullptr;
    return std::make_shared<AudioClipComponent>(this, path, id, is3D, loop);
}

} // namespace DonTopo
