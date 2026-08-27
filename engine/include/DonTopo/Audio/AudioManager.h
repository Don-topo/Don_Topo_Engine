#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

#include "DonTopo/Audio/AudioBus.h"

namespace DonTopo {

class AudioClipComponent;

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // true si el audio quedó operativo. NO lanza: una máquina sin dispositivo
    // de salida (o sin FMOD compilado) devuelve false, escribe una línea en
    // cerr y deja el motor funcionando mudo — antes la excepción subía hasta el
    // main del editor y el editor no arrancaba. Llamarlo dos veces es no-op.
    bool init();
    // ¿Hay sistema FMOD vivo? Es lo que distingue "sin audio en esta máquina"
    // de cualquier otro fallo; los tests lo usan para saltarse de verdad los
    // casos que necesitan FMOD.
    bool available() const;
    void update(const glm::vec3& listenerPos,
                const glm::vec3& listenerForward,
                const glm::vec3& listenerUp);
    void shutdown();

    // Estado de carga de un sonido. Con FMOD_NONBLOCKING, createSound retorna
    // FMOD_OK aunque el fichero no exista o esté corrupto: el fallo real solo
    // aparece DESPUÉS, aquí. Sin consultarlo, un asset roto era silencio total
    // — ni error en la UI al soltarlo, ni warning al cargar la escena, ni una
    // línea en el Log al darle a Play.
    enum class SoundLoadState {
        Missing,  // id fuera de rango, slot liberado, o sin sistema FMOD
        Loading,  // el hilo interno de FMOD sigue leyendo
        Ready,    // utilizable
        Failed    // fichero ausente, formato no soportado, datos corruptos
    };
    SoundLoadState getSoundState(int soundId) const;

    // Vacía en out los paths de los sonidos que han pasado a Failed y aún no se
    // habían reportado. Pensado para llamarse una vez por frame desde el host,
    // que es quien tiene canal al usuario (Log Console en el editor, cerr en el
    // runtime): AudioManager no lo tiene. Cada sonido se reporta UNA vez, no
    // uno por frame.
    void pollLoadFailures(std::vector<std::string>& out);

    // Retorna al instante: FMOD carga en su hilo interno (FMOD_NONBLOCKING). El
    // id es válido desde ya, pero un playSound antes de que termine la carga no
    // reproduce nada — no es un error, solo aún no está listo. Que devuelva un
    // id >= 0 NO significa que el fichero sea válido: eso lo dice getSoundState.
    int  loadSound(const std::string& path, bool is3D = true, bool loop = false,
                   AudioLoadMode loadMode = AudioLoadMode::Sample);
    void unloadSound(int soundId);

    // Sonidos FMOD vivos ahora mismo (slots ocupados, no ids repartidos).
    // Diagnóstico: es la única forma de ver desde fuera que la caché comparte
    // de verdad — dos clips del mismo fichero con el mismo modo tienen que
    // dejar esto en 1, no en 2.
    size_t loadedSoundCount() const;

    // Slots reservados, ocupados o no. Junto con loadedSoundCount es lo que
    // hace observable el reciclado: crear y soltar mil sonidos tiene que dejar
    // esto plano, no en mil. Sin este getter, "no se reciclan los ids" no se
    // puede distinguir de "sí se reciclan" desde fuera — los dos dejan el mismo
    // número de sonidos vivos.
    size_t soundSlotCount() const;

    // ¿El sonido está en streaming de verdad? Lee el FMOD_MODE real, no lo que
    // el componente cree. Existe porque sin esto la línea que aplica
    // FMOD_CREATESTREAM no tiene ninguna cobertura: quitarla dejaba todo
    // "funcionando" —el modo se guarda, se serializa y se lee de vuelta— con la
    // feature entera sin efecto (sabotaje verificado). false si el id no existe.
    bool isSoundStreaming(int soundId) const;

    // minDistance/maxDistance se aplican a la voz recien arrancada. Van aqui y
    // no en el FMOD::Sound porque el sonido se COMPARTE entre clips (cache por
    // path+modo): escribirlas en el sonido le cambiaria el radio de atenuacion
    // a todos los objetos que usen el mismo fichero.
    void playSound(int soundId, const glm::vec3& worldPos = {},
                   float volume = 1.0f, float pitch = 1.0f,
                   AudioBus bus = AudioBus::Sfx,
                   float minDistance = 1.0f, float maxDistance = 100.0f);
    void stopSound(int soundId);

    // Dispara una voz SUELTA del sonido: no se guarda en m_sfxChannels, así que
    // no corta la reproducción anterior ni se corta con la siguiente, y varios
    // disparos se solapan. Es el PlayOneShot de Unity, y es lo que faltaba para
    // que dos pasos seguidos, o dos balas, no se pisaran — playSound tiene la
    // semántica contraria a propósito (un Play nuevo corta el anterior).
    //
    // Consecuencia de no guardar el canal: esa voz ya no se puede alcanzar.
    // stopSound, los setters de volumen/pitch y isSoundPlaying NO la ven; suena
    // hasta que termina. Por eso un one-shot en bucle sería una voz inmortal:
    // se usa con clips de disparo, no con loops.
    void playSoundOneShot(int soundId, const glm::vec3& worldPos = {},
                          float volume = 1.0f, float pitch = 1.0f,
                          AudioBus bus = AudioBus::Sfx,
                          float minDistance = 1.0f, float maxDistance = 100.0f);

    // Empujan el valor al canal de la última reproducción de soundId, si
    // sigue siendo suyo. FMOD recicla los Channel*: un canal que ya terminó
    // puede haber sido reasignado a otro sonido, y escribirle el volumen se
    // lo cambiaría a un sonido ajeno. Por eso se comprueba isPlaying() y que
    // getCurrentSound() sea el sonido de ese id.
    //
    // No pasa nada si no hay canal: el valor vive en AudioClipComponent y se
    // aplicará en el siguiente playSound.
    void setChannelVolume(int soundId, float volume);
    void setChannelPitch (int soundId, float pitch);

    // Atenuación 3D del sonido: por debajo de minDistance suena a volumen
    // pleno, y de ahí a maxDistance va cayendo. Se escribe en el FMOD::Sound
    // (vale pa las reproducciones futuras) Y en el canal vivo si lo hay, con la
    // misma comprobación que los dos setters de arriba. No-op si el sonido no
    // se cargó con FMOD_3D: en 2D no hay atenuación que ajustar.
    void setSound3DMinMaxDistance(int soundId, float minDistance, float maxDistance);

    // Reposiciona la voz que está sonando de soundId. Es lo que hace que un
    // sonido 3D SIGA a su GameObject: playSound solo escribe la posición una
    // vez, al arrancar, así que sin esto un objeto en movimiento dejaba el
    // sonido clavado donde estaba al pulsar Play (ni atenuación ni paneo
    // cambiaban). No-op si el sonido no es 3D o si no hay voz viva, con la
    // misma comprobación de liveChannel que los setters de volumen/pitch.
    void setSoundPosition(int soundId, const glm::vec3& worldPos);

    // ¿Hay una voz viva de este sonido? La lógica ya existía dentro del .cpp
    // (liveChannel, el guard contra el reciclado de voces de FMOD) pero no
    // estaba expuesta, así que un script no podía esperar a que un sonido
    // terminara. OJO: una voz PAUSADA sigue contando como "playing" para FMOD,
    // igual que en Unity; para distinguirla está isSoundPaused.
    bool isSoundPlaying(int soundId) const;
    bool isSoundPaused (int soundId) const;

    // Pausa/reanuda la voz viva del sonido, conservando la posición de
    // reproducción — a diferencia de stopSound, que la tira. No-op si no hay
    // voz: no es estado persistente del componente, es de la voz.
    void setSoundPaused(int soundId, bool paused);

    // Volumen por bus, [0, 1]. Es el mando que el jugador espera encontrar en
    // las opciones: Master escala a los otros dos porque Music y Sfx cuelgan de
    // él en FMOD. El getter existe para que la UI dibuje el valor real y no una
    // copia suya que pueda desincronizarse.
    void  setBusVolume(AudioBus bus, float v);
    float getBusVolume(AudioBus bus) const;

    // Carga path con el modo dado (is3D/loop horneados en el FMOD_MODE) y
    // envuelve el soundId resultante en un AudioClipComponent listo para
    // colgar de un GameObject (GameObject::setAudioClip). nullptr si
    // loadSound falla (fichero inválido/no soportado por FMOD).
    std::shared_ptr<AudioClipComponent> createAudioClipComponent(
        const std::string& path, bool is3D, bool loop,
        AudioLoadMode loadMode = AudioLoadMode::Sample);

private:
#ifdef DT_FMOD_ENABLED
    // FMOD::ChannelGroup* del bus, o el master del sistema. nullptr sin sistema.
    void* groupForBus(AudioBus bus) const;

    // Clave de la caché de sonidos. NO es solo el path: is3D y loop van
    // horneados en el FMOD_MODE, así que el mismo fichero cargado como 3D y como
    // 2D son dos FMOD::Sound distintos y no se pueden compartir. Meter solo el
    // path aquí haría que marcar "Is 3D?" en un clip cambiara en silencio el de
    // otro GameObject.
    static std::string soundKey(const std::string& path, bool is3D, bool loop,
                                 AudioLoadMode loadMode);

    void* m_system   = nullptr;  // FMOD::System*
    void* m_sfxGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_musicGroup = nullptr; // FMOD::ChannelGroup* del bus Music
    std::vector<void*> m_sounds;      // FMOD::Sound* SFX clips
    std::vector<void*> m_sfxChannels; // FMOD::Channel* de la última reproducción de cada id (paralelo a m_sounds)
    // Paralelos a m_sounds. El path es lo único que le sirve al usuario del
    // aviso (el id es interno), y el flag evita repetir el mismo fallo en cada
    // frame — pollLoadFailures se llama por frame.
    std::vector<std::string> m_soundPaths;
    std::vector<char>        m_soundFailureReported;
    // Cuántos AudioClipComponent usan cada slot. El FMOD::Sound solo se libera
    // cuando llega a cero: antes, veinte objetos con el mismo .wav cargaban
    // veinte copias descomprimidas en RAM, y el primero en destruirse se
    // llevaba por delante... nada, porque cada uno tenía la suya. Compartiendo,
    // soltar sin contar sería un use-after-free para los otros diecinueve.
    std::vector<int>         m_soundRefs;
    // Clave de cada slot, para poder borrar la entrada del mapa al liberarlo.
    std::vector<std::string> m_soundKeys;
    // Slots libres, para reutilizarlos en vez de crecer sin techo: cada ciclo
    // Play->Stop recrea la escena entera y pedía ids nuevos.
    std::vector<int>         m_freeSlots;
    std::unordered_map<std::string, int> m_soundByKey;
#endif
};

} // namespace DonTopo
