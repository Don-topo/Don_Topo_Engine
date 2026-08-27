#pragma once
#include <string>

namespace DonTopo {

// Bus por el que sale un sonido. Los tres grupos existían ya dentro de
// AudioManager (el master de FMOD, "BGM" y "SFX") pero no se podía elegir: todo
// clip salía por SFX y nadie tocaba los volúmenes, así que no había forma de
// bajar la música sin bajar los efectos.
//
// Master no es un destino más: es el grupo padre de los otros dos, y su volumen
// los escala a ambos. Se puede asignar a un clip igual — a veces se quiere algo
// que ignore el mando de música y el de efectos, como una voz de sistema.
//
// En un header propio y no dentro de AudioManager.h porque AudioClipComponent
// necesita el tipo y no debe arrastrar el manager entero: la forward
// declaration de AudioManager en ese header es deliberada.
enum class AudioBus { Master, Music, Sfx };

// Nombre estable para el .scene y para la UI. Por NOMBRE y nunca por índice:
// reordenar el enum no puede cambiar el bus guardado de nadie (mismo criterio
// que los combos de ProjectContext::ViewSettings).
inline const char* audioBusToStr(AudioBus bus)
{
    switch (bus)
    {
        case AudioBus::Master: return "master";
        case AudioBus::Music:  return "music";
        case AudioBus::Sfx:
        default:               return "sfx";
    }
}

// Devuelve false si el nombre no existe, dejando out intacto: quien llama
// decide si eso merece un warning (la carga de escena) o un silencio.
inline bool audioBusFromStr(const std::string& name, AudioBus& out)
{
    if (name == "master") { out = AudioBus::Master; return true; }
    if (name == "music")  { out = AudioBus::Music;  return true; }
    if (name == "sfx")    { out = AudioBus::Sfx;    return true; }
    return false;
}

// Cómo se lleva el fichero a memoria. Es el "Load Type" de Unity, y hasta ahora
// no se podía elegir: todo clip se descomprimía entero en RAM, así que un mp3
// de tres minutos puesto en un objeto se comía decenas de MB sin avisar. El
// único streaming del motor estaba en la API de BGM, que no usaba nadie.
enum class AudioLoadMode {
    // Se descomprime entero al cargar. Arranca al instante y admite tantas
    // voces simultáneas como se quiera: es lo que se quiere para efectos.
    Sample,
    // Se lee y decodifica del disco sobre la marcha (FMOD_CREATESTREAM). Ocupa
    // muy poca RAM, a cambio de un pelín de latencia al arrancar.
    //
    // LIMITACIÓN, y por eso es para música y no para efectos: un stream lleva un
    // solo buffer de decodificación, así que NO puede sonar dos veces a la vez.
    // Como los clips con la misma ruta y el mismo modo comparten sonido (la
    // caché de AudioManager), dos GameObjects con el mismo fichero en Stream
    // tampoco pueden sonar simultáneamente — el segundo Play corta al primero.
    // [verificar contra la API de FMOD] el comportamiento exacto de FMOD al
    // pedir la segunda voz de un stream; lo que aquí se afirma es el diseño.
    Stream
};

inline const char* audioLoadModeToStr(AudioLoadMode mode)
{
    return mode == AudioLoadMode::Stream ? "stream" : "sample";
}

inline bool audioLoadModeFromStr(const std::string& name, AudioLoadMode& out)
{
    if (name == "sample") { out = AudioLoadMode::Sample; return true; }
    if (name == "stream") { out = AudioLoadMode::Stream; return true; }
    return false;
}

// ¿Es una extensión de audio de las que acepta el motor? Vive aquí, y no en el
// panel de Properties, porque hay CUATRO rutas por las que entra un audio (el
// diálogo del inspector, su drop-zone, AddComponent desde Lua y la carga de
// escena) y la lista solo cubría la primera: por Lua o por un .scene editado a
// mano entraba cualquier cosa, y con la carga asíncrona de FMOD eso acaba en un
// clip mudo sin más explicación.
//
// La comparación es en minúsculas: quien llame se encarga de bajar la extensión
// antes (no se hace aquí para no arrastrar <algorithm> a un header que incluye
// medio módulo de audio).
inline bool isSupportedAudioExtension(const std::string& lowercaseExt)
{
    return lowercaseExt == ".wav" || lowercaseExt == ".mp3"
        || lowercaseExt == ".ogg" || lowercaseExt == ".flac";
}

} // namespace DonTopo
