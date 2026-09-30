// Headless test of frustum culling (no GUI, no Vulkan). Renderer::
// frustumFromViewProj and Renderer::aabbVisible are static precisely so they can
// be exercised without an initialized Renderer. Plain main + asserts, no
// framework, consistent with camera_tests.cpp.
//
// What is really tested here is not "does it cull?" but "does it cull TOO LITTLE?": a
// false negative is an object that disappears from the screen, so every case
// inside the frustum is asserted visible, and the outside cases are placed well
// away so that no conservative slack saves them by accident.
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Camera at the origin looking toward -Z, which is the glm::lookAt convention and the
// one the engine uses. Returns proj*view ready for frustumFromViewProj.
static glm::mat4 makeViewProj(bool zeroToOne)
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = zeroToOne
        ? glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f)
        : glm::perspective(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan Y-flip, same as currentFrameCamera()
    return proj * view;
}

// Unit cube centered at the local origin: everything is positioned by the model matrix,
// like the real RenderObjects.
static const glm::vec3 kMin(-10.0f, -10.0f, -10.0f);
static const glm::vec3 kMax( 10.0f,  10.0f,  10.0f);

static glm::mat4 at(float x, float y, float z)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

// In front of the camera = visible; behind, to the sides and beyond the far plane = not.
// It is tested with BOTH depth ranges because the engine mixes the two
// (glm::perspective in the editor, *RH_ZO in CameraComponent and in the light).
static void test_dentro_y_fuera(bool zeroToOne)
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(zeroToOne));

    // Straight ahead, at mid distance: the trivial case that CANNOT fail.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -100.0f)));
    // Close to the camera but in front. It is the case that would break if the near
    // plane were extracted with the wrong convention on this matrix.
    //
    // It has to be a SMALL box and very close: with the half-side 10 cube of
    // the other cases, a corner always sticks out far enough for
    // the conservative test to save it, and the case would discriminate nothing
    // (checked by sabotaging the plane by hand: it passed anyway). With half-side 0.2 at
    // 1.5 from the camera, extracting the near plane as "plain row 2" on the
    // [-1,1] matrix does discard it.
    const glm::vec3 chicoMin(-0.2f, -0.2f, -0.2f);
    const glm::vec3 chicoMax( 0.2f,  0.2f,  0.2f);
    CHECK(Renderer::aabbVisible(f, chicoMin, chicoMax, at(0.0f, 0.0f, -1.5f)));
    // Almost touching the far plane, still inside.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -900.0f)));

    // Behind the camera.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, 500.0f)));
    // Outside on the right and on the left (at a depth of 100 the half-width
    // of the 45° frustum is ~41, so 400 is comfortably outside).
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(400.0f, 0.0f, -100.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(-400.0f, 0.0f, -100.0f)));
    // Outside above and below.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 400.0f, -100.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, -400.0f, -100.0f)));
    // Beyond the far plane.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -5000.0f)));
}

// An object that peeks in over the edge has to keep being drawn: the test is
// conservative, and the error that matters is the false negative.
static void test_borde_cuenta_como_visible()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));

    // At a depth of 100 the half-width is ~41.4. A cube of half-side 10
    // centered at x=48 has its center OUTSIDE and its corner INSIDE.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(48.0f, 0.0f, -100.0f)));
    // Same case for the top.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 48.0f, -100.0f)));
}

// The AABB is in LOCAL space: the test has to apply the whole model matrix,
// rotation and scale included. Without the absolute value of the 3x3 in
// aabbVisible, a rotated box would be bounded too small and would disappear at the
// edge.
static void test_rotacion_y_escala()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));

    // Rotated about Y: the axis-aligned box that wraps it grows by a factor
    // ~1.41, so it peeks in at the edge even though unrotated it would not reach.
    //
    // 135° and not 45° on purpose: with 45° the three contributions to extent.x are
    // positive and summing with or without absolute value gives the same, so the case would
    // prove nothing (checked by removing the abs by hand: it passed anyway). At 135°
    // the terms are +0.707 and -0.707, and without the abs they cancel, leaving the
    // box with zero width.
    glm::mat4 rotada = glm::translate(glm::mat4(1.0f), glm::vec3(53.0f, 0.0f, -100.0f));
    rotada = glm::rotate(rotada, glm::radians(135.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, rotada));

    // Scaled x10 from a position where the unscaled cube would be outside:
    // the big object does come into the camera.
    glm::mat4 grande = glm::translate(glm::mat4(1.0f), glm::vec3(120.0f, 0.0f, -100.0f));
    grande = glm::scale(grande, glm::vec3(10.0f));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, grande));
    // And the same place WITHOUT scaling is outside; without this pair, the CHECK
    // above would pass anyway even if aabbVisible ignored the scale.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(120.0f, 0.0f, -100.0f)));
}

// The shadow pass culls against the light matrix, which is orthographic and
// bounded to ±350 around the origin (see shadowLightSpaceMatrix). What
// falls outside that volume does not fit in the shadow map.
static void test_frustum_ortografico_de_la_luz()
{
    glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f, 500.0f, 300.0f),
                                      glm::vec3(0.0f),
                                      glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProj = glm::orthoRH_ZO(-350.0f, 350.0f, -350.0f, 350.0f, 1.0f, 2000.0f);
    lightProj[1][1] *= -1.0f;
    const Renderer::Frustum f = Renderer::frustumFromViewProj(lightProj * lightView);

    // At the center of the volume: it casts a shadow.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, 0.0f)));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(300.0f, 0.0f, 0.0f)));
    // Very far in X: outside the ±350 of the orthographic.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(2000.0f, 0.0f, 0.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(-2000.0f, 0.0f, 0.0f)));
}

// The planes come out normalized, so dot(n,c)+d is a real distance. If
// they were not, the projected radius of the AABB would not be comparable with that
// distance and the test margin would be scaled by an arbitrary factor.
static void test_planos_normalizados()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));
    for (const glm::vec4& p : f.planes)
    {
        const float len = glm::length(glm::vec3(p));
        CHECK(std::fabs(len - 1.0f) < 0.001f);
    }
}

// ── Bound of skinned objects ──────────────────────────────────────────────
// Two-bone rig: the root at the origin and an "arm" whose origin is at
// (0,1,0). The only vertex HANGS from the arm downward, at (0,0.5,0), so
// at rest it is 0.5 from the model origin. The clip rotates the arm 180°, which
// takes it to (0,1.5,0): three times farther.
//
// That is exactly the case that makes a character disappear if the bound is
// taken from the mesh at rest, and that is why the rig is built the opposite way to the intuitive one
// (the vertex inward, not outward).
static const float kPi = 3.14159265358979f;

static SkinnedMesh makeRigDeDosHuesos(bool conClip)
{
    SkinnedMesh mesh;
    mesh.name = "rig";

    Skeleton& skel = mesh.skeleton;
    skel.names       = { "raiz", "brazo" };
    skel.parentIndex = { -1, 0 };
    skel.inverseBindPose = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) // arm at (0,1,0)
    };
    skel.boneMap = { {"raiz", 0}, {"brazo", 1} };

    SkinnedVertex v{};
    v.position    = glm::vec4(0.0f, 0.5f, 0.0f, 1.0f);
    v.boneIndices = glm::ivec4(1, 0, 0, 0);
    v.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    mesh.skinnedVertices = { v };

    if (conClip)
    {
        BoneChannel ch{};
        ch.boneIndex = 1;
        ch.posKeys   = { {0.0f, glm::vec3(0.0f, 1.0f, 0.0f)}, {1.0f, glm::vec3(0.0f, 1.0f, 0.0f)} };
        ch.scaleKeys = { {0.0f, glm::vec3(1.0f)},             {1.0f, glm::vec3(1.0f)} };
        ch.rotKeys   = { {0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)},
                         {1.0f, glm::angleAxis(kPi, glm::vec3(0.0f, 0.0f, 1.0f))} };

        AnimationClip clip{};
        clip.name           = "girar";
        clip.duration       = 1.0f;
        clip.ticksPerSecond = 1.0f;
        clip.channels       = { ch };
        mesh.animationClips = { clip };
    }
    return mesh;
}

// No instant of the clip (INTERPOLATED ones included, which is where sampling
// keyframes would fall short) can get outside the bound.
static void test_cota_skinned_cubre_toda_la_animacion()
{
    const SkinnedMesh mesh = makeRigDeDosHuesos(/*conClip=*/true);
    const float R = Renderer::skinnedBoundRadius(mesh);

    const glm::mat4 invBind = mesh.skeleton.inverseBindPose[1];
    const glm::vec3 bind    = glm::vec3(mesh.skinnedVertices[0].position);
    float maxReal = 0.0f;
    for (int i = 0; i <= 64; i++)
    {
        const float t = (float)i / 64.0f;
        // What bone_eval + bone_hierarchy do for this rig: the root
        // stays at identity and the arm rotates slerping from 0 to 180°.
        const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f))
                              * glm::mat4_cast(glm::angleAxis(kPi * t, glm::vec3(0.0f, 0.0f, 1.0f)));
        const glm::vec3 p = glm::vec3(mundo * invBind * glm::vec4(bind, 1.0f));
        maxReal = std::max(maxReal, glm::length(p));
        CHECK(glm::length(p) <= R + 1e-4f);
    }
    // The sweep above would also be passed by an absurdly large bound, so
    // both sides are asserted: that it reaches the extreme pose (1.5, which means it was
    // NOT taken from the mesh at rest, which gives 0.5) and that it does not blow up.
    CHECK(maxReal > 1.49f);
    CHECK(R >= maxReal);
    // 2.0 is the guardrail against inflated bounds: bounding the bone scale
    // with the Frobenius norm instead of the spectral one gives 2.598 here, and in
    // a real skeleton it multiplies at every level of the hierarchy.
    CHECK(R < 2.0f);
}

// Without clips, the pose is the bind pose, but the bound still has to count
// the bone offset relative to the model origin (the bindLocal), not just the
// radius of the vertices.
static void test_cota_skinned_sin_clips()
{
    const SkinnedMesh mesh = makeRigDeDosHuesos(/*conClip=*/false);
    const float R = Renderer::skinnedBoundRadius(mesh);
    CHECK(std::fabs(R - 1.5f) < 1e-3f);
}

// Without bones or without vertices there is nothing to bound with: 0 means "do not cull", which
// is the safe side.
static void test_cota_skinned_sin_nada()
{
    CHECK(Renderer::skinnedBoundRadius(SkinnedMesh{}) == 0.0f);

    SkinnedMesh sinVertices = makeRigDeDosHuesos(true);
    sinVertices.skinnedVertices.clear();
    CHECK(Renderer::skinnedBoundRadius(sinVertices) == 0.0f);
}

// The bound is a sphere centered at the LOCAL ORIGIN, so the placement is set
// entirely by the object's transform, same as with the statics.
static void test_cota_skinned_se_culea_con_el_transform()
{
    const SkinnedMesh mesh = makeRigDeDosHuesos(/*conClip=*/true);
    const float R = Renderer::skinnedBoundRadius(mesh);
    const glm::vec3 cotaMin(-R), cotaMax(R);
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));

    CHECK(Renderer::aabbVisible(f, cotaMin, cotaMax, at(0.0f, 0.0f, -50.0f)));
    CHECK(!Renderer::aabbVisible(f, cotaMin, cotaMax, at(0.0f, 0.0f, 50.0f)));   // behind
    CHECK(!Renderer::aabbVisible(f, cotaMin, cotaMax, at(500.0f, 0.0f, -50.0f))); // to the side
}


// ── Room in the instance SSBO (H23) ────────────────────────────────────
//
// Culling decides WHAT is drawn; this decides whether it FITS. They go in the same file
// because both answer the same question of the shadow pass, and the failure
// of this part is not visible: a character that does not fit loses its shadow silently.

// Skinned ones count. This is the regression: the count came only from the
// static objects, so a scene of pure characters reserved ZERO.
static void test_capacidad_cuenta_los_skinned()
{
    CHECK(shadowInstanceCapacity(0, 3) == 3u * (SHADOW_CASCADES + 2));
    CHECK(shadowInstanceCapacity(2, 3) == 5u * (SHADOW_CASCADES + 2));
}

// The real worst case: every object visible in the four cascades, in the
// scene pass and in the depth pre-pass.
static void test_capacidad_es_el_peor_caso()
{
    CHECK(shadowInstanceCapacity(10, 0) == 60u);   // 10 * (4 + 2)
}

// Empty scene: zero, and let the caller be the one to set its minimum. Returning a
// minimum here would hide the "nothing fits" case behind a magic number.
static void test_capacidad_de_escena_vacia()
{
    CHECK(shadowInstanceCapacity(0, 0) == 0u);
}

// An absurd scene cannot wrap the counter around and ask for a small capacity:
// that would reserve too little and bring back the silent failure, but worse.
static void test_capacidad_no_desborda()
{
    const uint32_t enorme = shadowInstanceCapacity(1000u * 1000u * 1000u, 0);
    CHECK(enorme == 0xFFFFFFFFu);   // saturated, not wrapped
}


// ── Device memory allocation cap (H72) ────────────────────────
//
// Vulkan asks for one allocation per resource and each mesh takes TWO alive
// (vertices and indices), so the device cap translates into a number of
// meshes. The minimum that the specification guarantees is 4096; an NVIDIA
// returns 4,189,151, so the ceiling is real on some GPUs and does not exist on
// others. This is the only part of the guard that can be tested without a device.

static void test_mallas_que_caben_en_el_minimo_de_la_spec()
{
    // 4096 / 2 = 2048 meshes. It is the case that the audit documents.
    CHECK(GpuDevice::meshesWithinAllocationLimit(4096) == 2048u);
}

// A generous GPU has no practical ceiling and must not warn about anything.
static void test_una_gpu_generosa_no_tiene_techo()
{
    CHECK(GpuDevice::meshesWithinAllocationLimit(4189151u) == 2094575u);
    CHECK(!GpuDevice::allocationLimitIsTight(4189151u));
}

// The warning fires with the spec minimum, which is where the ceiling is really
// reached with a large scene.
static void test_el_minimo_de_la_spec_si_es_estrecho()
{
    CHECK(GpuDevice::allocationLimitIsTight(4096));
}

// An absurd cap cannot wrap around or divide by zero.
static void test_tope_degenerado()
{
    CHECK(GpuDevice::meshesWithinAllocationLimit(0) == 0u);
    CHECK(GpuDevice::meshesWithinAllocationLimit(1) == 0u);
    CHECK(GpuDevice::allocationLimitIsTight(0));
}

int main()
{
    test_dentro_y_fuera(/*zeroToOne=*/true);
    test_dentro_y_fuera(/*zeroToOne=*/false);
    test_borde_cuenta_como_visible();
    test_rotacion_y_escala();
    test_frustum_ortografico_de_la_luz();
    test_planos_normalizados();
    test_cota_skinned_cubre_toda_la_animacion();
    test_cota_skinned_sin_clips();
    test_cota_skinned_sin_nada();
    test_cota_skinned_se_culea_con_el_transform();

    test_capacidad_cuenta_los_skinned();
    test_capacidad_es_el_peor_caso();
    test_capacidad_de_escena_vacia();
    test_capacidad_no_desborda();

    test_mallas_que_caben_en_el_minimo_de_la_spec();
    test_una_gpu_generosa_no_tiene_techo();
    test_el_minimo_de_la_spec_si_es_estrecho();
    test_tope_degenerado();

    if (g_failures == 0) std::printf("frustum_tests: OK\n");
    else                 std::printf("frustum_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
