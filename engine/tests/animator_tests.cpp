// Tests headless del Animator: carga de N clips, empaquetado GPU, máquina de
// estados y serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Criterio 1: todo clip cargado del FBX tiene nombre no vacío y único, duration
// y ticksPerSecond válidos. Ejercita el bucle sobre mAnimations aunque el asset
// del repo traiga una sola animación.
static void test_loader_reads_all_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    CHECK(!m.animationClips.empty());

    for (size_t i = 0; i < m.animationClips.size(); i++)
    {
        const AnimationClip& c = m.animationClips[i];
        CHECK(!c.name.empty());
        CHECK(c.duration > 0.0f);
        CHECK(c.ticksPerSecond > 0.0f);
        CHECK(!c.channels.empty());
        // Nombres únicos entre sí: el Animator resuelve por nombre.
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);
    }
}

int main()
{
    test_loader_reads_all_clips();

    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); return 1; }
    std::printf("dt_animator_tests: OK\n");
    return 0;
}
