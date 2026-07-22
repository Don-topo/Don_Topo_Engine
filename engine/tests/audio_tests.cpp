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
#include "DonTopo/Audio/AudioManager.h"
#include <nlohmann/json.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// createAudioClipComponent devuelve nullptr por DOS motivos distintos: FMOD
// no disponible en la máquina (SKIP legítimo) o el exe se lanzó desde un
// directorio donde no existe "assets/audio.mp3" (cwd equivocado). Sin
// distinguirlos, el segundo caso da exit 0 con el test sin ejecutar: un falso
// verde para un criterio de repo que es "exit code 0".
static bool checkAudioProbe(const std::shared_ptr<AudioClipComponent>& probe, const char* testName)
{
    if (probe) return true;
    if (!std::filesystem::exists("assets/audio.mp3"))
    {
        std::printf("FAIL: %s - assets/audio.mp3 no existe (ejecuta los tests desde la raiz del repo)\n", testName);
        ++g_failures;
    }
    else
    {
        std::printf("SKIP %s (FMOD no disponible)\n", testName);
    }
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
    if (!checkAudioProbe(probe, "test_volume_pitch_round_trip")) return;

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
    if (!checkAudioProbe(probe, "test_scene_without_volume_loads_neutral")) return;

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
    if (!checkAudioProbe(probe, "test_scene_with_null_volume_loads_with_warning")) return;

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

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

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

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
