// Tests headless de AudioClipComponent: rangos de volume/pitch y su
// serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
//
// El componente se construye a pelo con m_audio = nullptr y soundId = -1:
// así los setters ejercitan el clamp sin necesitar FMOD ni dispositivo de
// audio. Mismo truco que usa exporter_tests.cpp.
#include "DonTopo/Audio/AudioClipComponent.h"

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

int main()
{
    test_defaults_are_neutral();
    test_volume_clamps_to_range();
    test_pitch_clamps_to_range();
    test_setters_survive_without_manager();

    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
