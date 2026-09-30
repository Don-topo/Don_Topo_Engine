// Headless test of the visibility guards (no GUI, no Vulkan device).
// Visibility::objectVisible and gatherCandidates do not call Vulkan: the mesh
// cache hands out handles by callback, so the full decision (checkbox,
// deleted entry, in-flight upload and frustum) can be exercised here. Plain
// main + asserts, no framework, consistent with frustum_tests.cpp.
//
// What is tested is not "does it cull?" (frustum_tests already covers that) but the
// four guards that were COPIED in the four passes of the backend. Each
// case sets up the state that makes just one of them fail: if any one
// disappears from the helper, exactly one case turns red and says which.
#include "DonTopo/Renderer/VisibleSet.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Camera at the origin looking toward -Z, same convention as the engine.
static Culling::Frustum camaraEnOrigen()
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f);
    proj[1][1] *= -1.0f; // Y-flip de Vulkan
    return Culling::frustumFromViewProj(proj * view);
}

static glm::mat4 en(float x, float y, float z)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

// Fake scene: N objects, each with its entry in the cache. The meshes
// and the objects are ALL created at once and only afterwards are pointers requested: the
// two containers are vectors, so a late creation would relocate the
// memory and leave dangling any pointer requested before.
struct Escena
{
    SharedGpuMeshCache        meshes;
    std::vector<RenderObject> objects;

    void anade(const char* key, const glm::mat4& xform)
    {
        RenderObject obj;
        // Bounded box with half-side 1, like any real mesh: the AABB is in
        // local space and the transform places it, same as in the Renderer.
        obj.sharedIndex = meshes.acquire(key, [](SharedGpuMesh& g) {
            g.aabbMin   = glm::vec3(-1.0f);
            g.aabbMax   = glm::vec3( 1.0f);
            g.hasBounds = true;
        });
        obj.transform = xform;
        objects.push_back(obj);
    }
};

static void test_guardas_una_a_una()
{
    const Culling::Frustum cam = camaraEnOrigen();

    Escena e;
    e.anade("a", en(0.0f, 0.0f, -50.0f));
    e.anade("b", en(0.0f, 0.0f, -50.0f));

    RenderObject& obj  = e.objects[0];
    SharedGpuMesh* gpu = e.meshes.get(obj.sharedIndex);

    // Base case: in front of the camera, uploaded and with the checkbox on.
    CHECK(Visibility::objectVisible(obj, gpu, 0, cam));

    // 1. "Visible" checkbox of the Mesh component.
    obj.meshVisible = false;
    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));
    obj.meshVisible = true;

    // 2. In-flight upload: the object's ticket is ahead of the last
    //    completed one. Its textures are still in TRANSFER_DST_OPTIMAL and sampling
    //    them would read garbage.
    gpu->uploadTicket = 7;
    CHECK(!Visibility::objectVisible(obj, gpu, 6, cam));
    // Boundary: a ticket EQUAL to the last completed one is already uploaded. A `<` instead
    // of `<=` would make every imported mesh flicker for one frame.
    CHECK(Visibility::objectVisible(obj, gpu, 7, cam));
    CHECK(Visibility::objectVisible(obj, gpu, 8, cam));
    gpu->uploadTicket = 0;

    // 3. Frustum: well behind the camera, far enough that no
    //    conservative slack saves it.
    obj.transform = en(0.0f, 0.0f, 400.0f);
    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));

    // 4. Without an AABB (empty mesh) it cannot be bounded: it passes even if it is behind.
    //    Discarding it would be culling TOO MUCH, which is the failure seen on screen.
    gpu->hasBounds = false;
    CHECK(Visibility::objectVisible(obj, gpu, 0, cam));

    // 5. Entry deleted from the editor: the index is no longer alive and the
    //    cache returns nullptr. It is checked with the real cache and not by
    //    passing nullptr by hand, which is what the Renderer does. It goes last
    //    because it frees a slot that the next creation would reuse.
    RenderObject& segundo = e.objects[1];
    e.meshes.release(segundo.sharedIndex, [](const SharedGpuMesh&) {});
    CHECK(e.meshes.get(segundo.sharedIndex) == nullptr);
    CHECK(!Visibility::objectVisible(segundo, e.meshes.get(segundo.sharedIndex), 0, cam));
}

// The frustum is a parameter and not a member precisely for this: the shadow
// pass evaluates the SAME object with the one of its cascade. An object that the camera
// does not see can still cast a shadow onto what is seen, so the two
// answers have to be able to differ.
static void test_dos_frustums_mismo_objeto()
{
    const Culling::Frustum cam = camaraEnOrigen();

    // Volume of a light that looks toward -X from the right: it covers what is
    // next to the camera, where the camera frustum does not reach.
    const glm::mat4 luzView = glm::lookAt(glm::vec3(200.0f, 0.0f, 0.0f), glm::vec3(0.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 luzProj = glm::orthoRH_ZO(-100.0f, 100.0f, -100.0f, 100.0f, 1.0f, 400.0f);
    const Culling::Frustum luz = Culling::frustumFromViewProj(luzProj * luzView);

    Escena e;
    e.anade("a", en(-60.0f, 0.0f, 0.0f));

    const RenderObject&  obj = e.objects[0];
    const SharedGpuMesh* gpu = e.meshes.get(obj.sharedIndex);

    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));
    CHECK(Visibility::objectVisible(obj, gpu, 0, luz));
}

static void test_gather()
{
    const Culling::Frustum cam = camaraEnOrigen();

    Escena e;
    e.anade("a", en(0.0f, 0.0f, -50.0f));  // inside
    e.anade("b", en(0.0f, 0.0f, 400.0f));  // behind the camera
    e.objects[0].ssrStrength = 0.75f;
    e.objects[1].ssrStrength = 0.5f;

    std::vector<Batching::BatchCandidate> out;

    // With prior garbage: gatherCandidates clears. If it did not, the pass
    // would also draw the candidates of the previous pass.
    out.push_back({ 99, true, nullptr, 0.0f });

    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ true,
                                 /*colorPass*/ true, out);

    // The invisible ones ALSO get in, with visible = false: the grouping skips
    // them, but the Performance panel counts them as culled. Returning only the
    // visible ones would leave that counter at zero.
    CHECK(out.size() == 2);
    CHECK(out[0].visible);
    CHECK(!out[1].visible);
    // Stable order, and that of the objects, not of the grouping.
    CHECK(out[0].sharedIndex == e.objects[0].sharedIndex);
    CHECK(out[1].sharedIndex == e.objects[1].sharedIndex);
    // The transform goes by POINTER to the object: buildInstanceBatches copies it into the
    // buffer later, so pointing to a temporary would write garbage.
    CHECK(out[0].transform == &e.objects[0].transform);
    CHECK(out[1].transform == &e.objects[1].transform);
    CHECK(out[0].ssr == 0.75f);
    CHECK(out[1].ssr == 0.5f);

    // Passes that do not draw color leave it at 0 even if the object has strength:
    // the SSR enters the grouping key, and with a single value fewer draws come out
    // and the resulting map is identical.
    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ false,
                                 /*colorPass*/ true, out);
    CHECK(out.size() == 2);
    CHECK(out[0].ssr == 0.0f);
    CHECK(out[1].ssr == 0.0f);
    // And visibility does not depend on the SSR.
    CHECK(out[0].visible);
    CHECK(!out[1].visible);
}

// The PBR factors travel per candidate since they came out of the shared
// entry, and they are governed by colorPass, NOT ssrEnabled. They are two separate flags
// on purpose: merging them would leave the whole scene matte (all objects with
// the default factors) just by turning the SSR off, which has nothing to do with it.
static void test_gather_factores()
{
    const Culling::Frustum cam = camaraEnOrigen();

    Escena e;
    e.anade("a", en(0.0f, 0.0f, -50.0f));
    e.anade("b", en(0.0f, 0.0f, -60.0f));
    e.objects[0].metallic  = 1.0f;
    e.objects[0].roughness = 0.2f;
    e.objects[1].metallic  = 0.25f;
    e.objects[1].roughness = 0.8f;

    std::vector<Batching::BatchCandidate> out;

    // Color pass with the SSR OFF: the factors have to arrive all the same.
    // It is the case that breaks if someone reuses ssrEnabled to gate them.
    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ false,
                                 /*colorPass*/ true, out);
    CHECK(out.size() == 2);
    CHECK(out[0].metallic  == 1.0f);
    CHECK(out[0].roughness == 0.2f);
    CHECK(out[1].metallic  == 0.25f);
    CHECK(out[1].roughness == 0.8f);

    // Shadows and depth: same value for all, which is what collapses the
    // groups. Being equal AMONG THEMSELVES is what matters here, not the specific number.
    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ true,
                                 /*colorPass*/ false, out);
    CHECK(out.size() == 2);
    CHECK(out[0].metallic  == out[1].metallic);
    CHECK(out[0].roughness == out[1].roughness);
    // And it is not that they kept the object's own by chance.
    CHECK(out[0].metallic != e.objects[0].metallic);
}

int main()
{
    test_guardas_una_a_una();
    test_dos_frustums_mismo_objeto();
    test_gather();
    test_gather_factores();

    if (g_failures == 0) std::printf("visible_set_tests: OK\n");
    else                 std::printf("visible_set_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
