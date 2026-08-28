// Tests headless de AudioClipComponent: rangos de volume/pitch y su
// serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
//
// El componente se construye a pelo con m_audio = nullptr y soundId = -1:
// así los setters ejercitan el clamp sin necesitar FMOD ni dispositivo de
// audio. Mismo truco que usa exporter_tests.cpp.
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Editor/Command.h"
#include <nlohmann/json.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// createAudioClipComponent devuelve nullptr por DOS motivos distintos: FMOD
// no disponible en la máquina (SKIP legítimo) o el exe se lanzó desde un
// directorio donde no existe "assets/audio.mp3" (cwd equivocado). Sin
// distinguirlos, el segundo caso da exit 0 con el test sin ejecutar: un falso
// verde para un criterio de repo que es "exit code 0".
//
// El SKIP se decide por AudioManager::available() y NO por "existe el fichero":
// antes, con FMOD compilado y sin dispositivo de salida, init() lanzaba y el
// binario abortaba sin llegar nunca a imprimir este SKIP — el mensaje prometía
// un camino que no existía. Ahora init() devuelve false y available() lo dice.
static bool checkAudioProbe(const AudioManager& am,
                             const std::shared_ptr<AudioClipComponent>& probe, const char* testName)
{
    if (probe) return true;
    if (!am.available())
    {
        std::printf("SKIP %s (FMOD no disponible)\n", testName);
        return false;
    }
    if (!std::filesystem::exists("assets/audio.mp3"))
        std::printf("FAIL: %s - assets/audio.mp3 no existe (ejecuta los tests desde la raiz del repo)\n", testName);
    else
        std::printf("FAIL: %s - createAudioClipComponent fallo con FMOD vivo y el asset presente\n", testName);
    ++g_failures;
    return false;
}

static std::shared_ptr<AudioClipComponent> makeClip()
{
    return std::make_shared<AudioClipComponent>(nullptr, "assets/audio.mp3", -1, false, false);
}

// Un clip recién creado suena tal cual está grabado: sin atenuar y sin
// alterar el tono. Si estos defaults cambiaran, toda escena guardada antes
// de esta feature sonaría distinta al recargarla.
static void test_defaults_are_neutral()
{
    auto clip = makeClip();
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
    CHECK(nearlyEqual(clip->getPitch(), 1.0f));
}

static void test_volume_clamps_to_range()
{
    auto clip = makeClip();

    clip->setVolume(0.5f);
    CHECK(nearlyEqual(clip->getVolume(), 0.5f));

    clip->setVolume(-1.0f);
    CHECK(nearlyEqual(clip->getVolume(), 0.0f));

    clip->setVolume(5.0f);
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
}

// El mínimo NO es 0: un pitch de 0 pararía el sonido en seco en vez de
// bajarlo, y FMOD no lo admite como "silencio".
static void test_pitch_clamps_to_range()
{
    auto clip = makeClip();

    clip->setPitch(1.5f);
    CHECK(nearlyEqual(clip->getPitch(), 1.5f));

    clip->setPitch(0.1f);
    CHECK(nearlyEqual(clip->getPitch(), 0.5f));

    clip->setPitch(10.0f);
    CHECK(nearlyEqual(clip->getPitch(), 2.0f));
}

// Sin AudioManager no hay canal al que empujar el valor. El setter tiene que
// guardarlo igual y no tocar un puntero nulo.
static void test_setters_survive_without_manager()
{
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.75f);
    CHECK(nearlyEqual(clip->getVolume(), 0.25f));
    CHECK(nearlyEqual(clip->getPitch(), 1.75f));
}

// El JSON tiene que llevar los dos campos: sin ellos, mover un slider y
// guardar la escena no dejaría rastro.
static void test_tojson_emits_volume_and_pitch()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.5f);
    go->setAudioClip(clip);

    nlohmann::json j = scene.toJson();
    const nlohmann::json& node = j["root"]["children"][0];
    // Ancla explícita: si el constructor de Scene algún día sembrara un hijo
    // por defecto, este CHECK señala la causa real en vez de que los asserts
    // de abajo fallen contra el nodo equivocado sin ninguna pista.
    CHECK(node["name"] == "altavoz");
    CHECK(node.contains("audioClip"));
    if (!node.contains("audioClip")) return;
    CHECK(nearlyEqual(node["audioClip"].value("volume", -1.0f), 0.25f));
    CHECK(nearlyEqual(node["audioClip"].value("pitch",  -1.0f), 1.5f));
}

// Round-trip completo por toJson/fromJson con valores NO neutros y, a
// propósito, DISTINTOS entre sí (mismo patrón que
// camera_tests.cpp:190-221, test_serialization_round_trip). 1.0/1.0 es a la
// vez el neutro de fábrica del componente y el default con el que carga
// Scene::fromJson cuando faltan las claves: un round-trip con esos valores
// "pasaría" igual aunque nadie escribiera ni leyera nada (ver hallazgo 1 del
// review — es justo lo que test_scene_without_volume_loads_neutral no podía
// distinguir por sí solo). Que volume != pitch además destapa un cruce de
// setters (setVolume(c.value("pitch",...)) o al revés): con valores iguales
// el cruce pasaría desapercibido.
//
// Necesita FMOD vivo, igual que el back-compat: Scene::fromJson crea el clip
// con AudioManager::createAudioClipComponent, que sin sonido cargado
// devuelve nullptr. Mismo SKIP si no hay FMOD disponible en la máquina.
static void test_volume_pitch_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_volume_pitch_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.25f);
    probe->setPitch(1.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(found->getAudioClip()->getVolume(), 0.25f));
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(),  1.5f));
}

// Back-compat: una escena guardada antes de esta feature no trae los campos y
// tiene que cargar con los valores neutros. Es lo que se rompe si alguien
// cambia el .value() de la carga por un .at().
//
// El JSON se construye a partir de scene.toJson() y no a mano: un literal
// escrito a pelo ya se desincronizó una vez del formato real de
// nodeFromJson/Scene::fromJson (le faltaba "version" y usaba
// position/rotation/scale en vez de localTransform). Partir de toJson() y
// borrar ahí las claves que queremos que falten es inmune a cambios de
// esquema (mismo patrón que camera_tests.cpp:246-257). Los valores previos al
// borrado son NO neutros a propósito: si erase() no quitara de verdad las
// claves (o fromJson las leyera de otro lado), el test vería 0.25/1.5 en vez
// del neutro 1.0/1.0 y fallaría igual.
//
// Necesita FMOD vivo: Scene::fromJson crea el clip con
// AudioManager::createAudioClipComponent, que sin sonido cargado devuelve
// nullptr. En una máquina sin dispositivo de audio el test se salta a sí
// mismo en vez de dar un falso rojo.
static void test_scene_without_volume_loads_neutral(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_without_volume_loads_neutral")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.25f);
    probe->setPitch(1.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("volume"));
    CHECK(audioClip.contains("pitch"));
    audioClip.erase("volume");
    audioClip.erase("pitch");

    Scene loaded("Vacia");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* loadedGo = loaded.findById(go->id);
    CHECK(loadedGo != nullptr);
    if (!loadedGo || !loadedGo->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(loadedGo->getAudioClip()->getVolume(), 1.0f));
    CHECK(nearlyEqual(loadedGo->getAudioClip()->getPitch(),  1.0f));
}

// setVolume(NaN)/setPitch(NaN) tienen que dejar el valor anterior intacto.
// El clamp por sí solo NO lo hacía: std::clamp(NaN, lo, hi) devuelve NaN
// (toda comparación con NaN es falsa), así que antes de este fix un NaN
// llegado desde un script Lua roto (un 0/0, por ejemplo) pasaba de largo el
// clamp y se guardaba tal cual en m_volume/m_pitch — para acabar
// serializado como "null" en el .scene y tumbar Scene::fromJson entero (ver
// el resto de tests de este fichero). Este test ejercita el guard añadido
// directamente en AudioClipComponent::setVolume/setPitch, sin pasar por
// Lua ni por Scene.
static void test_setVolume_setPitch_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    clip->setVolume(0.6f);
    clip->setVolume(nan);
    CHECK(nearlyEqual(clip->getVolume(), 0.6f));

    clip->setPitch(1.4f);
    clip->setPitch(nan);
    CHECK(nearlyEqual(clip->getPitch(), 1.4f));
}

// EL TEST QUE IMPORTA: una escena cuyo JSON trae "volume": null (el mismo
// "null" que nlohmann escribe al serializar un NaN, ver
// AudioClipComponent::setVolume) tiene que cargar bien entera — no solo el
// audioClip roto, sino el resto de sus campos (pitch) también — con el
// clip cayendo al volumen neutro por defecto y un aviso en
// Scene::lastWarnings() que nombra el campo. Antes de este fix,
// Scene::fromJson devolvía false: json::exception (302, "type must be
// number, but is null") escapaba de nodeFromJson y el catch de fromJson
// tiraba la carga de TODA la escena por este único campo.
static void test_scene_with_null_volume_loads_with_warning(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_with_null_volume_loads_with_warning")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.4f);
    probe->setPitch(1.3f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("volume"));
    // Mete el null a mano: así es exactamente como llega un NaN serializado.
    audioClip["volume"] = nullptr;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(found->getAudioClip()->getVolume(), 1.0f)); // default neutro
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(),  1.3f)); // el resto siguió cargando bien

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("volume") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Compara componente a componente contra la identidad (evita depender de
// que glm::mat4 tenga operator== disponible en este TU).
static bool isIdentity(const glm::mat4& m)
{
    const glm::mat4 id(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!nearlyEqual(m[c][r], id[c][r])) return false;
    return true;
}

// Un localTransform con un null entre sus 16 floats (mismo origen que el
// volume: un NaN serializado) no puede tumbar la escena entera. Diseño
// elegido (documentado también en Scene.cpp junto a jsonToMat4): CUALQUIER
// float corrupto de los 16 descarta la matriz entera y cae a la identidad
// completa, no solo ese componente — una matriz "a medias" podría parecer
// válida y tener la escala o la rotación rotas en silencio. No necesita
// FMOD: el nodo no lleva audioClip.
static void test_localTransform_null_element_loads_identity(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("cosa");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 30.0f));

    nlohmann::json j = scene.toJson();
    nlohmann::json& lt = j["root"]["children"][0]["localTransform"];
    CHECK(lt.is_array());
    CHECK(lt.size() == 16);
    lt[5] = nullptr; // uno de los 16 floats corrupto (posición arbitraria)

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(isIdentity(found->localTransform));

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("localTransform") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Hallazgo 1 del review: boxCollider.halfExtents lo escribe nodeToJson
// SIEMPRE (nunca es opcional, a diferencia de volume/pitch/fov...) — si el
// .scene lo pierde (merge mal resuelto, escritura truncada, edición a mano)
// NO es back-compat legítima, es corrupción, y tiene que avisar nombrando el
// campo y el objeto en vez de caer en un valor plausible sin ni un WARN.
// Antes de este fix: caja de 25 unidades centrada en el origen, cero avisos,
// el usuario la ve, no cuestiona nada, pulsa Guardar y las medidas originales
// se pierden para siempre. No necesita FMOD (el nodo no lleva audioClip).
static void test_boxCollider_missing_halfExtents_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Caja");
    go->setBoxCollider(pm.createBoxColliderComponent(glm::vec3(3.0f, 4.0f, 5.0f), glm::vec3(0.0f),
                                                      go->worldTransform, /*dynamic=*/false));

    nlohmann::json j = scene.toJson();
    nlohmann::json& box = j["root"]["children"][0]["boxCollider"];
    CHECK(box.contains("halfExtents"));
    box.erase("halfExtents");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasBoxCollider()) { CHECK(false); return; }

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("halfExtents") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Hallazgo 2 del review: "center": null (la forma EXACTA que toma un NaN
// serializado, ver el resto de este fichero) no puede caer en silencio a
// (0,0,0). Antes de este fix, readArrayFloat trataba "no es un array" igual
// que "índice fuera de rango" (los dos por la misma rama silenciosa): una
// cápsula con center corrupto se movía al origen sin dejar ni rastro en el
// Log — "la cápsula se ha movido sola", tal cual lo describe el review.
static void test_capsuleCollider_null_center_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Capsula");
    go->setCapsuleCollider(pm.createCapsuleColliderComponent(
        15.0f, 25.0f, glm::vec3(10.0f, 20.0f, 30.0f), go->worldTransform, /*dynamic=*/false));

    nlohmann::json j = scene.toJson();
    nlohmann::json& cap = j["root"]["children"][0]["capsuleCollider"];
    CHECK(cap.contains("center"));
    cap["center"] = nullptr; // así llega exactamente un NaN serializado

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasCapsuleCollider()) { CHECK(false); return; }

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("center") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// path/is3D/loop los escribe nodeToJson SIEMPRE. Hasta este fix se leían con
// .at(): a un audioClip al que le faltara CUALQUIERA de los tres, nlohmann le
// lanzaba una json::exception que subía hasta el catch de fromJson y tiraba la
// carga de TODA la escena — un objeto ajeno, en otra rama del árbol, se perdía
// por un campo de audio. Ahora la escena carga entera, el clip cae a su default
// (2D) y el warning nombra el campo.
//
// El "loop" NO se borra a propósito: si el fix se hubiera aplicado solo a dos de
// los tres campos, este test lo vería igual (el que falta basta para lanzar),
// así que hay un test por campo abajo en lugar de uno que los borre a la vez.
static void test_scene_audioclip_missing_is3D_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_missing_is3D_warns")) return;

    Scene scene("Test");
    GameObject* go    = scene.addGameObject("altavoz");
    // Un segundo objeto, hermano y SIN audio: es el que demuestra el daño real
    // del bug viejo. Con .at(), este nodo tampoco llegaba a cargarse.
    GameObject* otro  = scene.addGameObject("sin_audio");
    probe->setPitch(1.3f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("is3D"));
    audioClip.erase("is3D");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findById(otro->id) != nullptr);

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getIs3D() == false);          // default
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(), 1.3f)); // el resto siguió cargando

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("is3D") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

static void test_scene_audioclip_missing_loop_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_missing_loop_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("loop"));
    audioClip.erase("loop");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getLoop() == false); // default

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("loop") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Sin "path" no hay nada que cargar: el nodo se queda SIN clip (no con uno
// apuntando a la cadena vacía) y el resto de la escena carga igual. No necesita
// FMOD: nunca se llega a createAudioClipComponent.
static void test_scene_audioclip_missing_path_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    // El bloque audioClip se inyecta a mano: construirlo con un clip real
    // exigiría FMOD, y aquí el caso que importa es justo el que no lo usa.
    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"] = { {"is3D", false}, {"loop", false},
                                              {"volume", 0.5f}, {"pitch", 1.0f} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAudioClip());

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("path") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Un tipo equivocado (no solo la ausencia) tampoco puede tumbar la escena:
// "is3D": "true" como cadena es lo que deja un .scene editado a mano.
static void test_scene_audioclip_wrong_type_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_wrong_type_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"]["is3D"] = "true"; // cadena, no bool

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getIs3D() == false);

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("is3D") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// init() dos veces no puede dejar huérfanos el System ni los ChannelGroup
// anteriores. No hay forma de contar objetos FMOD desde aquí, así que lo que se
// comprueba es el contrato observable: la segunda llamada devuelve true, el
// manager sigue disponible y sigue cargando sonidos con el MISMO sistema (si el
// segundo init hubiera creado uno nuevo, los soundId de antes apuntarían a
// sonidos de un System ya sin dueño).
static void test_init_is_reentrant(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_init_is_reentrant (FMOD no disponible)\n");
        return;
    }
    auto before = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(before != nullptr);

    CHECK(am.init());       // segunda llamada: no-op
    CHECK(am.available());

    auto after = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(after != nullptr);
    // El clip de antes del segundo init sigue siendo utilizable (su soundId no
    // se quedó apuntando a un System huérfano).
    if (before) before->setVolume(0.5f);
    CHECK(before && nearlyEqual(before->getVolume(), 0.5f));
}

// Las distancias de atenuación tenían clamp y NINGÚN test: los rangos
// ([0.1, 50] y [1, 1000]) solo estaban escritos en un comentario del header.
static void test_distances_clamp_to_range()
{
    auto clip = makeClip();

    clip->setMinDistance(12.5f);
    CHECK(nearlyEqual(clip->getMinDistance(), 12.5f));
    clip->setMinDistance(-3.0f);
    CHECK(nearlyEqual(clip->getMinDistance(), 0.1f));
    clip->setMinDistance(999.0f);
    CHECK(nearlyEqual(clip->getMinDistance(), 50.0f));

    // Tras el clamp de min a 50, max sigue en su default de 100: el invariante
    // no se ha tenido que tocar todavía.
    CHECK(nearlyEqual(clip->getMaxDistance(), 100.0f));

    clip->setMaxDistance(250.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 250.0f));
    clip->setMaxDistance(-1.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 1.0f));
    clip->setMaxDistance(99999.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 1000.0f));
}

// NaN: mismo agujero que volume/pitch (std::clamp(NaN,...) devuelve NaN, así
// que el clamp por sí solo no lo para) y acaba igual, como "null" en el .scene.
static void test_distances_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    clip->setMinDistance(7.0f);
    clip->setMaxDistance(80.0f);
    clip->setMinDistance(nan);
    clip->setMaxDistance(nan);
    CHECK(nearlyEqual(clip->getMinDistance(), 7.0f));
    CHECK(nearlyEqual(clip->getMaxDistance(), 80.0f));
}

// El invariante min <= max vive en el componente, no en la UI: un .scene
// editado a mano tampoco puede instalar una atenuación invertida. Se ataca
// desde los DOS setters: cada uno arrastra al otro, y probar solo uno dejaba la
// mitad sin cubrir.
static void test_min_max_invariant_from_both_setters()
{
    // max baja por debajo de min: min lo sigue.
    auto a = makeClip();
    a->setMinDistance(40.0f);
    a->setMaxDistance(10.0f);
    CHECK(nearlyEqual(a->getMaxDistance(), 10.0f));
    CHECK(nearlyEqual(a->getMinDistance(), 10.0f));
    CHECK(a->getMinDistance() <= a->getMaxDistance());

    // min sube por encima de max: max lo sigue.
    auto b = makeClip();
    b->setMaxDistance(5.0f);
    b->setMinDistance(30.0f);
    CHECK(nearlyEqual(b->getMinDistance(), 30.0f));
    CHECK(nearlyEqual(b->getMaxDistance(), 30.0f));
    CHECK(b->getMinDistance() <= b->getMaxDistance());
}

// Round-trip de TODOS los campos del audioClip a la vez, cada uno con un valor
// distinto de su default Y distinto de los demás. Hasta ahora solo se probaban
// volume y pitch: is3D, loop, playOnAwake, minDistance y maxDistance se
// escribían en el JSON y nadie comprobaba que se leyeran de vuelta — borrar
// cualquiera de esas cinco lecturas de Scene::nodeFromJson pasaba la suite
// entera. Que los siete valores sean distintos entre sí destapa además un cruce
// de campos (leer "minDistance" en el setter de max, por ejemplo).
static void test_all_audioclip_fields_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_all_audioclip_fields_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setPlayOnAwake(true);
    probe->setVolume(0.35f);
    probe->setPitch(1.7f);
    probe->setMaxDistance(250.0f);
    probe->setMinDistance(12.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getIs3D() == true);
    CHECK(c->getLoop() == true);
    CHECK(c->getPlayOnAwake() == true);
    CHECK(nearlyEqual(c->getVolume(), 0.35f));
    CHECK(nearlyEqual(c->getPitch(), 1.7f));
    CHECK(nearlyEqual(c->getMinDistance(), 12.5f));
    CHECK(nearlyEqual(c->getMaxDistance(), 250.0f));
    CHECK(c->getPath() == "assets/audio.mp3");
}

// EL TEST DE H2: un fichero que no existe se acepta sin rechistar y solo se
// detecta consultando el estado DESPUÉS. Con FMOD_NONBLOCKING, createSound
// devuelve FMOD_OK aunque el path sea basura, así que loadSound entrega un id
// válido y createAudioClipComponent un componente entero — antes de este fix
// eso era silencio absoluto: la rama de error de la UI y la de Scene::fromJson
// no llegaban a ejecutarse nunca, y playSound se rendía sin decir nada.
//
// El fallo aparece en el hilo interno de FMOD unos frames más tarde, de ahí el
// bucle con update(): no es una espera arbitraria, es el mismo pump por frame
// que hacen el editor y el runtime.
static void test_missing_file_reports_load_failure(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_missing_file_reports_load_failure (FMOD no disponible)\n");
        return;
    }
    const std::string bogus = "assets/__no_existe_este_audio__.mp3";
    CHECK(!std::filesystem::exists(bogus));

    auto clip = am.createAudioClipComponent(bogus, false, false);
    // Se crea IGUAL. Si algún día createSound empezara a fallar aquí, este
    // CHECK lo delata en vez de que el test siga probando otra cosa.
    CHECK(clip != nullptr);
    if (!clip) return;

    bool failed = false;
    for (int i = 0; i < 2000 && !failed; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        failed = clip->hasLoadError();
    }
    CHECK(failed);

    std::vector<std::string> out;
    am.pollLoadFailures(out);
    bool reported = false;
    for (const auto& p : out)
        if (p.find("__no_existe_este_audio__") != std::string::npos) reported = true;
    CHECK(reported);

    // Y una sola vez: pollLoadFailures se llama en CADA frame, así que un
    // sonido roto que se reportara siempre inundaría el Log Console.
    out.clear();
    am.pollLoadFailures(out);
    for (const auto& p : out)
        CHECK(p.find("__no_existe_este_audio__") == std::string::npos);
}

// El otro fallo de carga, y el más traicionero: el fichero EXISTE, tiene
// extensión de audio, pasa la whitelist de la UI... y no es audio. Un asset
// truncado por un merge o una descarga a medias entra por aquí. Se prueba
// aparte del path inexistente porque el TIEMPO en detectarlo es distinto: el
// path que no existe falla al instante (ni se llega a abrir), y este hay que
// leerlo y descartarlo en el hilo interno de FMOD, lo que tarda decenas de ms
// reales. Un solo test con el caso rápido daba por buena una espera que no
// vale para el caso lento.
//
// Los dos acaban saliendo por la MISMA rama de getSoundState (el valor de
// retorno de getOpenState); no se ha conseguido provocar la rama de
// FMOD_OPENSTATE_ERROR, y así está anotado allí.
static void test_corrupt_file_reports_load_failure(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_corrupt_file_reports_load_failure (FMOD no disponible)\n");
        return;
    }
    const std::filesystem::path bogus =
        std::filesystem::temp_directory_path() / "dt_audio_corrupto.mp3";
    {
        std::FILE* f = std::fopen(bogus.string().c_str(), "wb");
        if (!f) { std::printf("SKIP test_corrupt_file_reports_load_failure (no se pudo escribir el temporal)\n"); return; }
        const char basura[] = "esto no es un mp3, solo bytes";
        std::fwrite(basura, 1, sizeof(basura), f);
        std::fclose(f);
    }

    auto clip = am.createAudioClipComponent(bogus.string(), false, false);
    CHECK(clip != nullptr);

    // Con espera real entre vueltas, no solo iteraciones: el path inexistente
    // falla al instante (el open ni llega a abrir), pero un fichero que SÍ
    // existe hay que leerlo y descartarlo, y eso lo hace el hilo interno de
    // FMOD a su ritmo. 2000 vueltas sin dormir se agotaban en milisegundos y el
    // test daba un rojo que no era del código.
    bool failed = false;
    if (clip)
        for (int i = 0; i < 200 && !failed; ++i)
        {
            am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            failed = clip->hasLoadError();
            if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    CHECK(failed);

    std::vector<std::string> out;
    am.pollLoadFailures(out);
    bool reported = false;
    for (const auto& p : out)
        if (p.find("dt_audio_corrupto") != std::string::npos) reported = true;
    CHECK(reported);

    // El clip se suelta antes de borrar el fichero: su destructor libera el
    // FMOD::Sound, que hasta ese momento puede tener el fichero abierto.
    clip.reset();
    std::error_code ec;
    std::filesystem::remove(bogus, ec);
}

// Seguimiento 3D por frame (H1). ALCANCE DE ESTE TEST: cubre que la ruta
// existe y que aguanta los casos degenerados — clip sin AudioManager, soundId
// inválido, clip 2D, nodo sin clip. Lo que NO puede cubrir es el efecto
// audible: comprobar que la voz se ha movido de verdad exigiría exponer un
// getter de la posición del canal que nadie más usaría, y eso es API de
// producción escrita solo para un test. Que la atenuación y el paneo sigan al
// objeto se verifica a mano en el editor, y así está anotado en el audit.
static void test_updateSpatial_survives_degenerate_cases(AudioManager& am)
{
    // Sin manager y 2D: el gate por m_is3D corta antes de tocar nada.
    auto orphan = makeClip();
    orphan->updateSpatial(glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(!orphan->getIs3D());

    // Con manager y soundId inválido: es el estado en el que queda un clip cuyo
    // reload() falló. Las guardas de rango de AudioManager tienen que absorberlo.
    AudioClipComponent bad(&am, "no_existe.mp3", -1, /*is3D=*/true, /*loop=*/false);
    bad.updateSpatial(glm::vec3(5.0f, 0.0f, 0.0f));
    CHECK(bad.getIs3D());
}

// Scene::update tiene que recorrer el árbol entero sin tropezar con los nodos
// que no llevan clip, y sin exigir que haya nada sonando. Es la llamada que
// hace que el seguimiento ocurra: si alguien la borra de Scene::update, el
// hallazgo H1 vuelve — este test no lo detectaría, pero deja la ruta escrita y
// ejercitada.
static void test_scene_updateAudioSpatial_walks_tree(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* vacio = scene.addGameObject("sin_audio");
    CHECK(vacio != nullptr);

    auto clip3d = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (clip3d)
    {
        GameObject* go = scene.addGameObject("altavoz");
        go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
        go->setAudioClip(clip3d);
    }
    scene.getRoot().updateWorldTransforms();

    // Sin nada sonando: no-op limpio en todos los nodos.
    scene.updateAudioSpatial();
    // Y por la ruta real, la que corre en Play.
    scene.update(0.016f, pm);
    CHECK(scene.findById(vacio->id) != nullptr);
}

// H5: quitar un Audio Clip perdía volumen, pitch y las dos distancias para
// siempre — Ctrl+Z no devolvía nada porque la baja no pasaba por el stack de
// undo. El comando tiene que devolver el componente con los SIETE valores, no
// uno recién creado con defaults. Los valores del snapshot son todos distintos
// del default y distintos entre sí: con defaults, un comando que no restaurara
// nada pasaría igual.
static void test_audioclip_command_restores_full_state(AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_audioclip_command_restores_full_state")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setPlayOnAwake(true);
    probe->setVolume(0.35f);
    probe->setPitch(1.7f);
    probe->setMaxDistance(250.0f);
    probe->setMinDistance(12.5f);
    go->setAudioClip(probe);

    const AudioClipState snapshot{ 0.35f, 1.7f, 12.5f, 250.0f,
                                    /*loop=*/true, /*is3D=*/true, /*playOnAwake=*/true };
    AudioClipComponentCommand cmd(scene, am, "quitar", go->id, /*add=*/false,
                                   "assets/audio.mp3", snapshot);

    cmd.execute();
    CHECK(!go->hasAudioClip());

    cmd.undo();
    CHECK(go->hasAudioClip());
    if (!go->hasAudioClip()) return;
    const auto& c = go->getAudioClip();
    CHECK(nearlyEqual(c->getVolume(), 0.35f));
    CHECK(nearlyEqual(c->getPitch(), 1.7f));
    CHECK(nearlyEqual(c->getMinDistance(), 12.5f));
    CHECK(nearlyEqual(c->getMaxDistance(), 250.0f));
    CHECK(c->getLoop() == true);
    CHECK(c->getIs3D() == true);
    CHECK(c->getPlayOnAwake() == true);
    CHECK(c->getPath() == "assets/audio.mp3");

    // Y el redo vuelve a quitarlo: un comando que solo supiera deshacer dejaría
    // el stack inconsistente en cuanto se rehiciera.
    cmd.execute();
    CHECK(!go->hasAudioClip());
}

// El estado del listener es un solo bool, y es justo el que se puede perder:
// quitar un listener DESHABILITADO y deshacer tiene que devolverlo
// deshabilitado. Un comando que creara siempre uno por defecto pasaría
// cualquier test que no mirara esto (el default es enabled = true).
static void test_audiolistener_command_preserves_disabled_state()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("oido");
    auto listener = std::make_shared<AudioListenerComponent>();
    listener->setEnabled(false);
    go->setAudioListener(listener);

    AudioListenerComponentCommand cmd(scene, "quitar", go->id, /*add=*/false, /*enabled=*/false);
    cmd.execute();
    CHECK(!go->hasAudioListener());

    cmd.undo();
    CHECK(go->hasAudioListener());
    if (!go->hasAudioListener()) return;
    CHECK(go->getAudioListener()->getEnabled() == false);
}

// PlayOneShot dispara una voz SUELTA: no se registra en m_sfxChannels, así que
// no corta la anterior y varios disparos se solapan. Eso no se puede oír desde
// un test, pero sí tiene una consecuencia observable y exacta: después de un
// PlayOneShot, isPlaying() sigue diciendo false, porque no hay canal guardado
// al que preguntar. Si alguien "simplificara" playOneShot delegando en
// playSound, ese CHECK se pondría en true y el solapamiento se habría perdido.
//
// El play() previo no es decorado: es el control que demuestra que el sonido
// llegó a cargar y que isPlaying() sabe decir true. Sin él, el aserto final
// pasaría también con un sonido que nunca sonó.
static void test_playOneShot_does_not_register_a_channel(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_playOneShot_does_not_register_a_channel (FMOD no disponible)\n");
        return;
    }
    auto clip = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/true);
    if (!checkAudioProbe(am, clip, "test_playOneShot_does_not_register_a_channel")) return;

    const glm::vec3 pos(0.0f);
    auto pump = [&am, &pos]() {
        am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    };
    // FMOD no responde al instante en ninguna de las dos direcciones: la carga
    // ocurre en su hilo interno y el efecto de un stop tampoco tiene por qué
    // verse en la misma vuelta. Los dos asertos de "espera a que pase" van por
    // esta ayuda con tope; hacerlos inmediatos daba un test intermitente, que
    // es peor que no tenerlo.
    auto waitUntil = [&pump](const std::function<bool()>& cond) {
        for (int i = 0; i < 400; ++i)
        {
            pump();
            if (cond()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    // Control: el sonido llega a cargar y a sonar, e isPlaying() sabe decir
    // true. Sin esto, el aserto de abajo pasaría también con un clip que nunca
    // sonó — que es justo el falso verde que hay que evitar.
    const bool playing = waitUntil([&]() { clip->play(pos); return clip->isPlaying(); });
    CHECK(playing);
    if (!playing) return;

    clip->stop();
    CHECK(waitUntil([&]() { return !clip->isPlaying(); }));

    // EL ASERTO QUE IMPORTA, y este sí es inmediato: no es una condición que
    // FMOD deba alcanzar con el tiempo, es que playOneShot NO guarda el canal.
    // isPlaying() pregunta por el canal guardado, así que tiene que seguir
    // diciendo false en la misma vuelta, esté sonando la voz o no.
    clip->playOneShot(pos);
    pump();
    CHECK(!clip->isPlaying());

    // Y no ha pisado la referencia: un play() posterior sigue registrando. Esto
    // descarta que playOneShot deje m_sfxChannels en un estado que rompa la
    // reproducción normal.
    CHECK(waitUntil([&]() { clip->play(pos); return clip->isPlaying(); }));
    clip->stop();
}

// El bus viaja al .scene por NOMBRE y vuelve. Se elige Music a propósito: no es
// el default (Sfx), así que un fromJson que no leyera el campo daría Sfx y el
// test lo vería.
static void test_bus_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_bus_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setBus(AudioBus::Music);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    // En el JSON tiene que estar el NOMBRE, no el índice del enum: si alguien
    // lo cambiara a un entero, reordenar AudioBus movería el bus de todas las
    // escenas guardadas sin que nadie se enterase.
    CHECK(j["root"]["children"][0]["audioClip"]["bus"] == "music");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Music);
}

// Back-compat: una escena guardada antes de los buses no trae el campo y tiene
// que cargar como Sfx, que es por donde salía TODO entonces — así suena igual.
// Y sin warning: la ausencia aquí es legítima, no corrupción.
static void test_scene_without_bus_loads_sfx(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_without_bus_loads_sfx")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setBus(AudioBus::Master); // no neutro: si erase() fallara, se vería
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"].erase("bus");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Sfx);

    for (const auto& w : loaded.lastWarnings())
        CHECK(w.find("bus") == std::string::npos);
}

// Un nombre que NO existe sí avisa: es corrupción o un proyecto de una versión
// más nueva. Caer a sfx en silencio dejaría un clip sonando por el bus
// equivocado sin ninguna pista de por qué.
static void test_scene_with_unknown_bus_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_with_unknown_bus_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"]["bus"] = "ambience_3d";

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Sfx);

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("bus") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Los tres volúmenes son independientes: escribir uno no puede mover a los
// otros dos. Con un solo bus probado, un getBusVolume que devolviera siempre el
// del master pasaría igual.
static void test_bus_volumes_are_independent(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_bus_volumes_are_independent (FMOD no disponible)\n");
        return;
    }
    am.setBusVolume(AudioBus::Master, 0.9f);
    am.setBusVolume(AudioBus::Music,  0.4f);
    am.setBusVolume(AudioBus::Sfx,    0.7f);

    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Master), 0.9f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music),  0.4f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx),    0.7f));

    // Master escala a los otros dos, pero NO los reescribe: bajar el master no
    // puede cambiar lo que el usuario tenga puesto en Music o en SFX.
    am.setBusVolume(AudioBus::Master, 0.5f);
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music), 0.4f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx),   0.7f));

    // Se dejan neutros: los tests que van detrás reproducen sonidos y un bus a
    // 0.2 heredado los dejaría prácticamente mudos sin explicación.
    am.setBusVolume(AudioBus::Music, 1.0f);
    am.setBusVolume(AudioBus::Sfx,   1.0f);
}

// H11: dos clips del MISMO fichero y el MISMO modo comparten un solo
// FMOD::Sound. Antes, veinte objetos con el mismo disparo cargaban veinte
// copias descomprimidas en RAM.
static void test_same_path_shares_one_sound(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_same_path_shares_one_sound (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();

    auto a = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/false);
    if (!checkAudioProbe(am, a, "test_same_path_shares_one_sound")) return;
    CHECK(am.loadedSoundCount() == before + 1);

    auto b = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/false);
    CHECK(b != nullptr);
    // El aserto de H11: el segundo NO carga nada nuevo.
    CHECK(am.loadedSoundCount() == before + 1);

    // Pero el modo va horneado en el FMOD_MODE, así que el mismo fichero en 3D
    // es OTRO sonido y no se puede compartir. Si la clave de la caché fuera solo
    // el path, marcar "Is 3D?" en un clip se lo cambiaría a los demás.
    auto c = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/false);
    CHECK(c != nullptr);
    CHECK(am.loadedSoundCount() == before + 2);

    // Refcount: soltar uno de los dos que comparten NO puede liberar el sonido,
    // o el que queda se queda con un puntero muerto.
    a.reset();
    CHECK(am.loadedSoundCount() == before + 2);
    CHECK(!b->hasLoadError()); // el superviviente sigue siendo utilizable

    // Y al soltar el último sí se libera.
    b.reset();
    CHECK(am.loadedSoundCount() == before + 1);
    c.reset();
    CHECK(am.loadedSoundCount() == before);
}

// H10: los ids se reciclan. Antes, cada ciclo Play->Stop (que recrea la escena
// entera) añadía una entrada por clip a unos vectores que solo crecían.
static void test_sound_slots_are_recycled(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_sound_slots_are_recycled (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();
    // El recuento de SLOTS, no el de sonidos vivos: los dos son distintos y es
    // justo la diferencia lo que prueba el reciclado. Sin reciclar, los sonidos
    // vivos vuelven a cero igual (el slot queda a nullptr) pero el vector crece
    // una entrada por vuelta — con loadedSoundCount solo, este test pasaba
    // aunque no se reciclara nada.
    const size_t slotsBefore = am.soundSlotCount();

    // Diez ciclos de crear y soltar. Cada ciclo Play->Stop del editor recrea la
    // escena entera, así que esto es lo que pasaba en una sesión normal.
    for (int i = 0; i < 10; ++i)
    {
        auto clip = am.createAudioClipComponent("assets/audio.mp3", false, false);
        CHECK(clip != nullptr);
        CHECK(am.loadedSoundCount() == before + 1);
        clip.reset();
        CHECK(am.loadedSoundCount() == before);
    }
    CHECK(am.soundSlotCount() == slotsBefore);

    // El id reciclado tiene que quedar utilizable de verdad, no solo contado:
    // un slot mal limpiado daría un clip que dice estar roto o que no suena.
    auto reused = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(reused != nullptr);
    if (reused)
    {
        CHECK(!reused->hasLoadError());
        CHECK(reused->getPath() == "assets/audio.mp3");
    }
}

// El modo de carga viaja al .scene y vuelve, y NO comparte sonido con el mismo
// fichero cargado del otro modo: un stream y una muestra descomprimida son dos
// FMOD::Sound distintos, igual que pasa con is3D y loop.
static void test_load_mode_round_trip_and_cache(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                              AudioLoadMode::Stream);
    if (!checkAudioProbe(am, probe, "test_load_mode_round_trip_and_cache")) return;
    CHECK(probe->getLoadMode() == AudioLoadMode::Stream);

    // Que el modo LLEGUE a FMOD, no solo que el componente lo recuerde. Sin este
    // par de asertos, borrar la línea que aplica FMOD_CREATESTREAM dejaba la
    // feature entera sin efecto y la suite en verde: el modo se guardaba, se
    // serializaba y se leía de vuelta igual.
    const int streamId = am.loadSound("assets/audio.mp3", false, true, AudioLoadMode::Stream);
    const int sampleId = am.loadSound("assets/audio.mp3", false, true, AudioLoadMode::Sample);
    CHECK(streamId >= 0);
    CHECK(sampleId >= 0);
    // Hay que ESPERAR a que termine la carga: con FMOD_NONBLOCKING, getMode()
    // no refleja todavía FMOD_CREATESTREAM mientras el sonido está en
    // OPENSTATE_LOADING. Sin esta espera el aserto fallaba siempre — y de forma
    // engañosa, porque también "fallaba" con el sabotaje puesto y parecía que lo
    // estaba detectando.
    for (int i = 0; i < 200 && am.getSoundState(streamId) == AudioManager::SoundLoadState::Loading; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(am.getSoundState(streamId) == AudioManager::SoundLoadState::Ready);
    CHECK(am.isSoundStreaming(streamId));
    CHECK(!am.isSoundStreaming(sampleId));
    am.unloadSound(streamId);
    am.unloadSound(sampleId);

    // Mismo fichero y mismos flags, pero cargado como Sample: sonido aparte.
    const size_t before = am.loadedSoundCount();
    auto sample = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                               AudioLoadMode::Sample);
    CHECK(sample != nullptr);
    CHECK(am.loadedSoundCount() == before + 1);
    sample.reset();

    Scene scene("Test");
    GameObject* go = scene.addGameObject("musica");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["audioClip"]["loadMode"] == "stream");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Stream);
}

// Back-compat: una escena anterior a esta feature no trae el campo y carga como
// Sample, que es como se cargaba TODO entonces. Sin warning: la ausencia es
// legítima. Un nombre desconocido sí avisa.
static void test_load_mode_back_compat_and_unknown(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                              AudioLoadMode::Stream);
    if (!checkAudioProbe(am, probe, "test_load_mode_back_compat_and_unknown")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("musica");
    go->setAudioClip(probe);
    nlohmann::json base = scene.toJson();

    {
        nlohmann::json j = base;
        j["root"]["children"][0]["audioClip"].erase("loadMode");
        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(go->id);
        if (!found || !found->hasAudioClip()) { CHECK(false); return; }
        CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Sample);
        for (const auto& w : loaded.lastWarnings())
            CHECK(w.find("loadMode") == std::string::npos);
    }
    {
        nlohmann::json j = base;
        j["root"]["children"][0]["audioClip"]["loadMode"] = "compressed_in_memory";
        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(go->id);
        if (!found || !found->hasAudioClip()) { CHECK(false); return; }
        CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Sample);
        bool warned = false;
        for (const auto& w : loaded.lastWarnings())
            if (w.find("loadMode") != std::string::npos) { warned = true; break; }
        CHECK(warned);
    }
}

// H16: la whitelist de extensiones vivía SOLO en la ruta de UI, así que un
// .scene con cualquier extensión creaba el clip igual y —por la carga diferida
// de FMOD— el único síntoma era el silencio. Ahora la carga de escena lo nombra
// y descarta el clip, dejando cargar el resto.
static void test_scene_rejects_unsupported_extension(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go   = scene.addGameObject("altavoz");
    GameObject* otro = scene.addGameObject("sin_audio");

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"] = { {"path", "assets/musica.xyz"},
                                              {"is3D", false}, {"loop", false} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    // El resto de la escena carga: descartar el clip no puede llevarse por
    // delante a un objeto ajeno.
    CHECK(loaded.findById(otro->id) != nullptr);

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAudioClip());

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("no soportado") != std::string::npos) { warned = true; break; }
    CHECK(warned);

    // Y el caso de control: la MISMA ruta con una extensión válida sí crea el
    // clip. Sin esto, un filtro que rechazara siempre pasaría el test de arriba.
    if (am.available())
    {
        nlohmann::json ok = scene.toJson();
        ok["root"]["children"][0]["audioClip"] = { {"path", "assets/audio.mp3"},
                                                    {"is3D", false}, {"loop", false} };
        Scene loadedOk("LoadedOk");
        CHECK(loadedOk.fromJson(ok, pm, am));
        GameObject* f2 = loadedOk.findById(go->id);
        CHECK(f2 != nullptr && f2->hasAudioClip());
    }
}

// PlayClipAtPoint retiene el sonido en la caché tras el primer uso, y las
// llamadas siguientes NO vuelven a contar. Es la parte delicada: sin el
// conjunto de retenidos, cada disparo subiría otra vez el refcount y el sonido
// quedaría imposible de liberar (no fugaría memoria, pero el contador crecería
// sin techo y el diagnóstico mentiría).
static void test_playClipAtPoint_pins_sound_once(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_playClipAtPoint_pins_sound_once (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();
    const glm::vec3 pos(10.0f, 0.0f, 0.0f);

    am.playClipAtPoint("assets/audio.mp3", pos);
    const size_t afterFirst = am.loadedSoundCount();
    // La primera vez sí carga: es un sonido 3D no-loop, distinto de los que
    // usan los demás tests (que van en 2D).
    CHECK(afterFirst == before + 1);

    // Y diez disparos más no cargan nada nuevo.
    for (int i = 0; i < 10; ++i)
        am.playClipAtPoint("assets/audio.mp3", pos);
    CHECK(am.loadedSoundCount() == afterFirst);

    // Preload de la misma ruta tampoco: es idempotente.
    am.preloadClip("assets/audio.mp3");
    CHECK(am.loadedSoundCount() == afterFirst);

    // El sonido retenido NO se puede soltar desde fuera: un AudioClipComponent
    // que use esa misma ruta y modo comparte el slot, y al destruirse no puede
    // llevarse por delante el sonido que playClipAtPoint mantiene vivo.
    {
        auto clip = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/false);
        CHECK(clip != nullptr);
        CHECK(am.loadedSoundCount() == afterFirst); // comparte, no carga otro
    }
    CHECK(am.loadedSoundCount() == afterFirst); // y sigue vivo tras destruirlo

    // Una ruta con extensión no soportada no carga nada (la whitelist vive en
    // el binding de Lua, pero el motor tampoco debe crear un sonido de la nada).
    am.playClipAtPoint("assets/__no_existe__.mp3", pos);
    // Sí sube: el path es válido como extensión aunque el fichero no exista —
    // eso lo reporta pollLoadFailures, no esta ruta. Se comprueba que al menos
    // no rompe nada y que el fallo se puede observar por el canal de siempre.
    std::vector<std::string> failures;
    for (int i = 0; i < 100 && failures.empty(); ++i)
    {
        am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        am.pollLoadFailures(failures);
        if (failures.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool reported = false;
    for (const auto& f : failures)
        if (f.find("__no_existe__") != std::string::npos) reported = true;
    CHECK(reported);
}

// P12: rolloff, spread, paneo y doppler. Clamps y rechazo de no-finitos, con el
// mismo criterio que volume/pitch — un NaN acabaría en el .scene como "null".
static void test_p12_props_clamp_and_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(nearlyEqual(clip->getSpread(), 0.0f));        // neutros por defecto:
    CHECK(nearlyEqual(clip->getStereoPan(), 0.0f));     // una escena vieja suena
    CHECK(nearlyEqual(clip->getDopplerLevel(), 0.0f));  // exactamente igual
    CHECK(clip->getRolloff() == AudioRolloff::Inverse);

    clip->setSpread(90.0f);
    CHECK(nearlyEqual(clip->getSpread(), 90.0f));
    clip->setSpread(-10.0f);
    CHECK(nearlyEqual(clip->getSpread(), 0.0f));
    clip->setSpread(1000.0f);
    CHECK(nearlyEqual(clip->getSpread(), 360.0f));
    clip->setSpread(nan);
    CHECK(nearlyEqual(clip->getSpread(), 360.0f)); // conserva el anterior

    clip->setStereoPan(-0.5f);
    CHECK(nearlyEqual(clip->getStereoPan(), -0.5f));
    clip->setStereoPan(-9.0f);
    CHECK(nearlyEqual(clip->getStereoPan(), -1.0f));
    clip->setStereoPan(9.0f);
    CHECK(nearlyEqual(clip->getStereoPan(), 1.0f));
    clip->setStereoPan(nan);
    CHECK(nearlyEqual(clip->getStereoPan(), 1.0f));

    clip->setDopplerLevel(2.5f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 2.5f));
    clip->setDopplerLevel(-1.0f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 0.0f));
    clip->setDopplerLevel(50.0f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 5.0f));
    clip->setDopplerLevel(nan);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 5.0f));
}

// Round-trip de los cuatro, y la caché los separa por rolloff: la curva va en el
// FMOD_MODE, así que el mismo fichero con curva lineal es OTRO sonido. Si el
// rolloff no entrara en la clave, cambiar la curva de un clip se la cambiaría a
// todos los que compartan el fichero.
static void test_p12_round_trip_and_cache(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (!checkAudioProbe(am, probe, "test_p12_round_trip_and_cache")) return;

    probe->setRolloff(AudioRolloff::Linear);

    // El rolloff tiene que entrar en la clave de la caché: va en el FMOD_MODE,
    // así que el mismo fichero con dos curvas son dos sonidos. Sin esto,
    // cambiar la curva de un clip se la cambiaría a todos los que compartan el
    // fichero.
    //
    // Se mide con loadSound directo y con una combinación de flags que no usa
    // ningún otro test (3D + loop): este binario comparte AudioManager entre
    // tests y varios dejan sonidos retenidos —playClipAtPoint pinea justo
    // 3D/no-loop/Sample/Inverse—, así que apoyarse en lo que haya cargado es
    // atarse al orden de ejecución. Ya me pasó al escribir este test.
    {
        const size_t base = am.loadedSoundCount();
        const int inverse = am.loadSound("assets/audio.mp3", true, true,
                                          AudioLoadMode::Sample, AudioRolloff::Inverse);
        CHECK(inverse >= 0);
        CHECK(am.loadedSoundCount() == base + 1);

        const int linear = am.loadSound("assets/audio.mp3", true, true,
                                         AudioLoadMode::Sample, AudioRolloff::Linear);
        CHECK(linear >= 0);
        CHECK(linear != inverse);
        CHECK(am.loadedSoundCount() == base + 2);

        // Y que la curva LLEGUE a FMOD, no solo que el componente la recuerde.
        // Hay que esperar a Ready: con FMOD_NONBLOCKING el modo no está
        // completo mientras carga (la misma trampa que con CREATESTREAM).
        for (int i = 0; i < 200 && am.getSoundState(linear) == AudioManager::SoundLoadState::Loading; ++i)
        {
            am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(am.getSoundState(linear) == AudioManager::SoundLoadState::Ready);
        CHECK(am.getSoundRolloff(linear) == AudioRolloff::Linear);
        CHECK(am.getSoundRolloff(inverse) == AudioRolloff::Inverse);

        am.unloadSound(inverse);
        am.unloadSound(linear);
        CHECK(am.loadedSoundCount() == base);
    }

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setSpread(120.0f);
    probe->setStereoPan(-0.75f);
    probe->setDopplerLevel(3.0f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["audioClip"]["rolloff"] == "linear");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getRolloff() == AudioRolloff::Linear);
    CHECK(nearlyEqual(c->getSpread(), 120.0f));
    CHECK(nearlyEqual(c->getStereoPan(), -0.75f));
    CHECK(nearlyEqual(c->getDopplerLevel(), 3.0f));
}

// Back-compat: una escena anterior a P12 no trae ninguno de los cuatro campos y
// tiene que cargar con los neutros, sin warnings.
static void test_p12_back_compat(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", true, false);
    if (!checkAudioProbe(am, probe, "test_p12_back_compat")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setSpread(200.0f);
    probe->setDopplerLevel(4.0f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    auto& ac = j["root"]["children"][0]["audioClip"];
    ac.erase("rolloff"); ac.erase("spread"); ac.erase("stereoPan"); ac.erase("dopplerLevel");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getRolloff() == AudioRolloff::Inverse);
    CHECK(nearlyEqual(c->getSpread(), 0.0f));
    CHECK(nearlyEqual(c->getStereoPan(), 0.0f));
    CHECK(nearlyEqual(c->getDopplerLevel(), 0.0f));
    for (const auto& w : loaded.lastWarnings())
    {
        CHECK(w.find("rolloff") == std::string::npos);
        CHECK(w.find("spread") == std::string::npos);
        CHECK(w.find("doppler") == std::string::npos);
    }
}

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    // init() ya no lanza: sin dispositivo de salida devuelve false y los tests
    // que necesitan FMOD se saltan solos por checkAudioProbe/available(). Antes
    // la excepción escapaba de aquí y abortaba el binario, con lo que el SKIP
    // que prometían esos tests no llegaba a imprimirse nunca.
    if (!am.init())
        std::printf("AVISO: FMOD no disponible; los tests que lo necesitan se saltaran\n");

    test_defaults_are_neutral();
    test_volume_clamps_to_range();
    test_pitch_clamps_to_range();
    test_setters_survive_without_manager();
    test_tojson_emits_volume_and_pitch();
    test_volume_pitch_round_trip(pm, am);
    test_scene_without_volume_loads_neutral(pm, am);
    test_setVolume_setPitch_reject_nan();
    test_scene_with_null_volume_loads_with_warning(pm, am);
    test_localTransform_null_element_loads_identity(pm, am);
    test_boxCollider_missing_halfExtents_warns(pm, am);
    test_capsuleCollider_null_center_warns(pm, am);
    test_scene_audioclip_missing_is3D_warns(pm, am);
    test_scene_audioclip_missing_loop_warns(pm, am);
    test_scene_audioclip_missing_path_warns(pm, am);
    test_scene_audioclip_wrong_type_warns(pm, am);
    test_init_is_reentrant(am);
    test_missing_file_reports_load_failure(am);
    test_corrupt_file_reports_load_failure(am);
    test_distances_clamp_to_range();
    test_distances_reject_nan();
    test_min_max_invariant_from_both_setters();
    test_all_audioclip_fields_round_trip(pm, am);
    test_updateSpatial_survives_degenerate_cases(am);
    test_scene_updateAudioSpatial_walks_tree(pm, am);
    test_audioclip_command_restores_full_state(am);
    test_audiolistener_command_preserves_disabled_state();
    test_playOneShot_does_not_register_a_channel(am);
    test_bus_round_trip(pm, am);
    test_scene_without_bus_loads_sfx(pm, am);
    test_scene_with_unknown_bus_warns(pm, am);
    test_bus_volumes_are_independent(am);
    test_same_path_shares_one_sound(am);
    test_sound_slots_are_recycled(am);
    test_load_mode_round_trip_and_cache(pm, am);
    test_load_mode_back_compat_and_unknown(pm, am);
    test_scene_rejects_unsupported_extension(pm, am);
    test_playClipAtPoint_pins_sound_once(am);
    test_p12_props_clamp_and_reject_nan();
    test_p12_round_trip_and_cache(pm, am);
    test_p12_back_compat(pm, am);

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
