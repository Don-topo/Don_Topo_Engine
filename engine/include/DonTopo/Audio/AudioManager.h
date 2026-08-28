#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // dt en segundos. Se usa SOLO para derivar la velocidad del listener, que es
    // lo que da el efecto doppler; con dt <= 0 la velocidad queda a cero y el
    // doppler no actúa (que es como se comportaba esto antes de tenerlo). Un
    // salto grande de posición —un teleport, o cargar otra escena— produciría
    // una velocidad absurda y un chirrido: por eso se descarta lo que supere
    // kMaxListenerSpeed en vez de creérselo.
    void update(const glm::vec3& listenerPos,
                const glm::vec3& listenerForward,
                const glm::vec3& listenerUp,
                float dt = 0.0f);
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
                   AudioLoadMode loadMode = AudioLoadMode::Sample,
                   AudioRolloff rolloff = AudioRolloff::Inverse);
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

    // Curva de atenuacion REAL del sonido, leida de su FMOD_MODE. Existe por lo
    // mismo que isSoundStreaming: sin ella, la linea que aplica el flag de
    // rolloff no tiene ninguna cobertura — quitarla dejaba el enum viajando por
    // la escena, la UI y Lua sin que la curva cambiara nunca (sabotaje
    // verificado). Ojo: como el streaming, el modo no es fiable hasta que la
    // carga termina (getSoundState == Ready).
    AudioRolloff getSoundRolloff(int soundId) const;

    // minDistance/maxDistance se aplican a la voz recien arrancada. Van aqui y
    // no en el FMOD::Sound porque el sonido se COMPARTE entre clips (cache por
    // path+modo): escribirlas en el sonido le cambiaria el radio de atenuacion
    // a todos los objetos que usen el mismo fichero.
    // spread: ensanchado estereo de la fuente 3D en grados [0, 360]. 0 = un
    // punto (lo de siempre); 360 = envolvente. pan: paneo manual [-1, 1], solo
    // con efecto en clips 2D — en 3D lo decide la posicion. doppler: cuanto
    // afecta la velocidad relativa al tono, [0, 5]; 0 lo apaga.
    void playSound(int soundId, const glm::vec3& worldPos = {},
                   float volume = 1.0f, float pitch = 1.0f,
                   AudioBus bus = AudioBus::Sfx,
                   float minDistance = 1.0f, float maxDistance = 100.0f,
                   float spread = 0.0f, float stereoPan = 0.0f,
                   float dopplerLevel = 0.0f);
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
                          float minDistance = 1.0f, float maxDistance = 100.0f,
                          float spread = 0.0f, float stereoPan = 0.0f,
                          float dopplerLevel = 0.0f);

    // Dispara path en una posición del mundo, SIN GameObject de por medio: el
    // PlayClipAtPoint de Unity. Es one-shot (se solapa con lo que suene) y 3D.
    //
    // El sonido queda RETENIDO en la caché para siempre — igual que en Unity,
    // donde el AudioClip es un asset cargado. Sin eso habría que descargarlo al
    // acabar la voz, y esa voz no se guarda en ningún sitio precisamente para
    // que se solape. Retener no fuga: es un FMOD::Sound por ruta distinta, y la
    // segunda llamada con la misma ruta no vuelve a contar.
    //
    // OJO con el primer disparo: FMOD carga en diferido (FMOD_NONBLOCKING), así
    // que la primera llamada de una ruta nueva casi seguro no se oye — el
    // sonido aún no está listo. Para eso está preloadClip.
    void playClipAtPoint(const std::string& path, const glm::vec3& worldPos,
                         float volume = 1.0f, float pitch = 1.0f,
                         AudioBus bus = AudioBus::Sfx,
                         float minDistance = 1.0f, float maxDistance = 100.0f);

    // Carga y retiene el sonido sin reproducirlo, para que el primer
    // playClipAtPoint de esa ruta llegue a oírse. Idempotente.
    void preloadClip(const std::string& path);

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
    // dt sirve para derivar la velocidad de la fuente (doppler), igual que en
    // update() para el listener: con dt <= 0 la velocidad va a cero.
    void setSoundPosition(int soundId, const glm::vec3& worldPos, float dt = 0.0f);

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

    // Silencia la voz sin tocar su volumen: al quitar el mute vuelve el valor
    // que tenia, sin que el script tenga que acordarse de cual era. Es la
    // diferencia con poner el volumen a 0, que es como se hacia hasta ahora.
    void setSoundMute(int soundId, bool mute);

    // Posicion de reproduccion en SEGUNDOS. Permite arrancar un clip por la
    // mitad y saber por donde va. -1 si no hay voz viva: 0 seria mentira (ese
    // es el principio del clip, no "no suena nada").
    float getSoundTime(int soundId) const;
    void  setSoundTime(int soundId, float seconds);

    // --- Pausa global ------------------------------------------------------
    //
    // Congela TODO lo que suena, conservando la posicion: es el
    // AudioListener.pause de Unity, lo que quiere un menu de pausa. Actua sobre
    // el grupo master, del que cuelgan los otros dos buses.
    //
    // No lo confundas con un timeScale: el motor no tiene pausa de simulacion,
    // asi que esto calla el audio pero la escena sigue corriendo si nadie mas
    // la para.
    void setAudioPaused(bool paused);
    bool isAudioPaused() const;

    // Volumen por bus, [0, 1]. Es el mando que el jugador espera encontrar en
    // las opciones: Master escala a los otros dos porque Music y Sfx cuelgan de
    // él en FMOD. El getter existe para que la UI dibuje el valor real y no una
    // copia suya que pueda desincronizarse.
    void  setBusVolume(AudioBus bus, float v);
    float getBusVolume(AudioBus bus) const;

    // --- Zonas de reverberacion --------------------------------------------
    //
    // Una zona es un FMOD::Reverb3D: una esfera con preset dentro de la cual
    // todo lo que suene coge esa reverb. La mezcla entre zonas solapadas y el
    // desvanecido entre min y max los hace FMOD, no nosotros.
    //
    // Se identifican por el id del GameObject dueno, no por indice: asi el
    // componente se mantiene como datos puros y el recurso nativo tiene un solo
    // dueno. syncReverbZone crea la zona la primera vez y la actualiza despues,
    // asi que se puede llamar por frame sin miedo.
    //
    // preset: nombre de un FMOD_PRESET_* en minusculas (ver reverbPresetNames).
    // Uno desconocido deja la zona con el preset anterior y devuelve false.
    bool syncReverbZone(uint64_t ownerId, const glm::vec3& worldPos,
                        float minDistance, float maxDistance,
                        const std::string& preset, bool enabled);

    // Destruye la zona de ese GameObject. No-op si no tenia.
    void removeReverbZone(uint64_t ownerId);

    // Destruye toda zona cuyo id NO este en la lista. Es como se recogen las
    // zonas de GameObjects borrados: el manager no ve la escena, asi que es la
    // escena la que le dice quien sigue vivo.
    void retainReverbZones(const std::vector<uint64_t>& aliveOwnerIds);

    // Destruye TODAS. La llama la carga de escena: las zonas de la escena
    // anterior no pueden sobrevivir a la nueva.
    void clearReverbZones();

    // Zonas vivas. Diagnostico y tests: sin esto, "la zona se creo" y "la zona
    // se creo y se perdio" son lo mismo desde fuera.
    size_t reverbZoneCount() const;

    // Presets disponibles, en el orden en que los ensena la UI. Estatico: la
    // lista es la misma con o sin FMOD compilado, para que el editor pueda
    // dibujar el combo aunque no haya audio.
    static const std::vector<std::string>& reverbPresetNames();

    // --- Efectos por bus ---------------------------------------------------
    //
    // Cuelgan un DSP de FMOD del ChannelGroup del bus, asi que afectan a TODO lo
    // que salga por el. Idempotente: pedir dos veces el mismo efecto en el mismo
    // bus no encadena dos copias.
    //
    // El parametro es el unico mando de cada efecto, normalizado a [0, 1] para
    // que la UI y Lua no tengan que conocer las unidades de FMOD:
    //   LowPass / HighPass -> frecuencia de corte (0 = mas cerrado, 1 = abierto)
    //   Echo               -> retardo entre repeticiones
    //   Reverb             -> tamano de la cola
    // El mapeo exacto a las unidades de FMOD vive en el .cpp, en un solo sitio.
    void setBusEffect(AudioBus bus, AudioEffect effect, float amount);

    // Quita el efecto del bus y libera su DSP. No-op si no estaba puesto.
    void clearBusEffect(AudioBus bus, AudioEffect effect);

    // Quita TODOS los efectos de un bus. Lo usa el editor al cambiar de escena.
    void clearBusEffects(AudioBus bus);

    // Diagnostico y tests: cuantos DSP hay colgados ahora mismo. Sin esto, "el
    // efecto se aplico" y "el efecto se creo y se perdio" son indistinguibles
    // desde fuera, y una fuga de DSP no se ve hasta que el mezclador se ahoga.
    size_t activeEffectCount() const;

    // DSP realmente conectados al grupo de un bus, preguntandoselo a FMOD. NO es
    // lo mismo que activeEffectCount, y la diferencia es justo donde vive la
    // fuga: si setBusEffect encadenara un DSP nuevo en cada llamada en vez de
    // reajustar el existente, el mapa seguiria teniendo UNA entrada (la nueva
    // pisa a la vieja) mientras el grupo acumula cien DSP perdidos. Lo descubri
    // saboteando la idempotencia y viendo que activeEffectCount no se enteraba.
    //
    // Incluye el DSP que FMOD pone de serie en cada grupo (el fader), asi que el
    // valor absoluto no significa nada: se usa por diferencia.
    size_t busDspCount(AudioBus bus) const;
    bool   hasBusEffect(AudioBus bus, AudioEffect effect) const;

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
                                 AudioLoadMode loadMode, AudioRolloff rolloff);

    // Carga y retiene el sonido de una ruta, devolviendo su id. Punto único de
    // carga de playClipAtPoint y preloadClip.
    int acquirePinnedSound(const std::string& path);

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
    // Sonidos retenidos por playClipAtPoint/preloadClip. Sin este conjunto,
    // cada disparo subiría otra vez el refcount del mismo sonido y el contador
    // crecería sin techo: el sonido no se liberaría jamás ni aunque se llamara
    // a unloadSound tantas veces como haga falta.
    std::unordered_set<int>  m_pinnedSounds;
    // DSP vivos, indexados por (bus, efecto). El valor es un FMOD::DSP* que hay
    // que desconectar Y liberar: son recursos nativos, no punteros sueltos, y
    // olvidarlos es la fuga clasica de este tipo de API.
    std::unordered_map<int, void*> m_busEffects;
    // FMOD::Reverb3D* por id de GameObject. Recursos nativos: hay que
    // liberarlos, y por eso el componente no los guarda.
    std::unordered_map<uint64_t, void*> m_reverbZones;
    // Clave del mapa. bus y efecto son enums pequenos, asi que caben de sobra.
    static int effectKey(AudioBus bus, AudioEffect effect)
    {
        return (int)bus * 16 + (int)effect;
    }
    // Ultima posicion conocida del listener y de cada fuente, para derivar la
    // velocidad que necesita el doppler. m_hasLastListenerPos evita que el
    // primer frame invente una velocidad enorme desde el origen.
    glm::vec3                m_lastListenerPos{0.0f};
    bool                     m_hasLastListenerPos = false;
    std::vector<glm::vec3>   m_soundLastPos;   // paralelo a m_sounds
    std::vector<char>        m_soundHasLastPos;
#endif
};

} // namespace DonTopo
