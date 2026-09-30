// Headless test of matrix decomposition (no GUI, no PhysX).
// decomposeTransform is pure glm, so the rule governing the five places that
// decompose a transform (physics, inspector and the two Lua bindings) can be
// asserted here in full. Plain main + asserts, consistent with
// frustum_tests.cpp.
//
// What is protected is what cost a silent editor crash, a freeze on entering
// Play and objects jumping to 1e8: glm::decompose returns bool and, given a
// singular matrix, returns false WITHOUT WRITING its outputs.
#include "DonTopo/Core/TransformDecompose.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// The case that broke: an axis at scale 0, which is what comes from typing a 0 in
// Scale.Y of the inspector.
static void test_matriz_singular()
{
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 20.0f, -7.0f)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 5.0f));

    glm::vec3 pos{-1.0f}, scale{-1.0f};
    glm::quat rot{0.0f, 9.0f, 9.0f, 9.0f};
    const bool ok = decomposeTransform(m, &pos, &rot, &scale);

    // It says it could not, which is the information nobody was looking at.
    CHECK(!ok);

    // And even so the outputs are useful. The whole POSITION: it is the fourth
    // column and does not depend on the decomposition.
    CHECK(std::fabs(pos.x - 3.0f) < 1e-5f);
    CHECK(std::fabs(pos.y - 20.0f) < 1e-5f);
    CHECK(std::fabs(pos.z + 7.0f) < 1e-5f);

    // The real SCALE, zero included: it is the length of each column.
    CHECK(std::fabs(scale.x - 2.0f) < 1e-5f);
    CHECK(std::fabs(scale.y) < 1e-5f);
    CHECK(std::fabs(scale.z - 5.0f) < 1e-5f);

    // The ROTATION to identity, which is the only honest thing: a flattened axis does
    // not define any orientation. Without this the quaternion came out uninitialized,
    // and eulerAngles turned it into the 90 degrees seen in the inspector.
    CHECK(std::fabs(rot.w - 1.0f) < 1e-5f);
    CHECK(std::fabs(rot.x) < 1e-5f);
    CHECK(std::fabs(rot.y) < 1e-5f);
    CHECK(std::fabs(rot.z) < 1e-5f);
}

// With a normal matrix it has to give exactly what it always gave: this replaces
// glm::decompose in five places, and changing the good case would be worse than
// the bug it fixes.
static void test_matriz_normal()
{
    const glm::vec3 posEsperada(1.5f, -2.0f, 3.25f);
    const glm::vec3 escalaEsperada(2.0f, 3.0f, 4.0f);
    // 90 degrees about Y: an angle that is not confused with the identity nor is
    // symmetric in the three axes.
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), posEsperada) *
                        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                        glm::scale(glm::mat4(1.0f), escalaEsperada);

    glm::vec3 pos, scale;
    glm::quat rot;
    CHECK(decomposeTransform(m, &pos, &rot, &scale));

    CHECK(glm::length(pos - posEsperada) < 1e-4f);
    CHECK(glm::length(scale - escalaEsperada) < 1e-4f);

    // The rotation is checked by what it DOES, not by its components: turning
    // 90 degrees about Y takes the X axis to -Z.
    const glm::vec3 giradoX = rot * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(std::fabs(giradoX.x) < 1e-4f);
    CHECK(std::fabs(giradoX.z + 1.0f) < 1e-4f);
}

// The three pointers are optional, and whoever wants only one must not pay for the
// others nor crash.
static void test_salidas_opcionales()
{
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 5.0f, 6.0f));

    CHECK(decomposeTransform(m));   // without any output

    glm::vec3 soloPos;
    CHECK(decomposeTransform(m, &soloPos));
    CHECK(glm::length(soloPos - glm::vec3(4.0f, 5.0f, 6.0f)) < 1e-5f);

    glm::vec3 soloEscala;
    CHECK(decomposeTransform(m, nullptr, nullptr, &soloEscala));
    CHECK(glm::length(soloEscala - glm::vec3(1.0f)) < 1e-5f);
}

// A NEGATIVE scale is a mirror. The column length is always positive,
// so the sign is lost: it is asserted so that it is on record, because the colliders
// already took the absolute value (a mirror does not thin the box) and the inspector
// looks at it with its own cache.
static void test_escala_negativa_sale_en_magnitud()
{
    const glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 3.0f, 1.0f));

    glm::vec3 scale;
    decomposeTransform(m, nullptr, nullptr, &scale);
    CHECK(std::fabs(scale.x - 2.0f) < 1e-5f);
    CHECK(std::fabs(scale.y - 3.0f) < 1e-5f);
}

int main()
{
    test_matriz_singular();
    test_matriz_normal();
    test_salidas_opcionales();
    test_escala_negativa_sale_en_magnitud();

    if (g_failures == 0) std::printf("transform_decompose_tests: OK\n");
    else                 std::printf("transform_decompose_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
