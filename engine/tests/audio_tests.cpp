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

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

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
    CHECK(node.contains("audioClip"));
    if (!node.contains("audioClip")) return;
    CHECK(nearlyEqual(node["audioClip"].value("volume", -1.0f), 0.25f));
    CHECK(nearlyEqual(node["audioClip"].value("pitch",  -1.0f), 1.5f));
}

// Back-compat: una escena guardada antes de esta feature no trae los campos y
// tiene que cargar con los valores neutros. Es lo que se rompe si alguien
// cambia el .value() de la carga por un .at().
//
// Necesita FMOD vivo: Scene::fromJson crea el clip con
// AudioManager::createAudioClipComponent, que sin sonido cargado devuelve
// nullptr. En una máquina sin dispositivo de audio el test se salta a sí
// mismo en vez de dar un falso rojo.
static void test_scene_without_volume_loads_neutral(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!probe)
    {
        std::printf("SKIP test_scene_without_volume_loads_neutral (FMOD no disponible)\n");
        return;
    }

    // NOTA: el brief de la tarea traía "position"/"rotation"/"scale" y sin
    // "version" en la raíz, pero el formato real (nodeFromJson/Scene::fromJson)
    // exige "version":1 y una "localTransform" (mat4 de 16 floats, aquí
    // identidad) por nodo — con el formato del brief fromJson devuelve false
    // pase lo que pase con volume/pitch. Se corrige aquí al formato real,
    // conservando la intención del test: ni "volume" ni "pitch" en audioClip.
    const auto identity = nlohmann::json::array({ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 });
    nlohmann::json j = {
        { "version", 1 },
        { "name", "Test" },
        { "root", {
            { "name", "Root" }, { "id", 1 },
            { "localTransform", identity },
            { "children", nlohmann::json::array({
                {
                    { "name", "altavoz" }, { "id", 2 },
                    { "localTransform", identity },
                    { "children", nlohmann::json::array() },
                    { "audioClip", {
                        { "path", "assets/audio.mp3" },
                        { "loop", false }, { "is3D", false }, { "playOnAwake", false }
                    }}
                }
            })}
        }}
    };

    Scene loaded("Vacia");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* go = loaded.findById(2);
    CHECK(go != nullptr);
    if (!go || !go->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(go->getAudioClip()->getVolume(), 1.0f));
    CHECK(nearlyEqual(go->getAudioClip()->getPitch(),  1.0f));
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
    test_scene_without_volume_loads_neutral(pm, am);

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
