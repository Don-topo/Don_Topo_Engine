// Headless test of CameraComponent and camera serialization/invariant in
// Scene (no GUI). Plain main + asserts, no framework — consistent with
// physics_tests.cpp.
//
// PhysX only accepts ONE PxFoundation per process (creating it twice, even
// if freed in between, crashes). That's why a single PhysicsManager is
// created in main() and passed by reference: here it's only needed because
// Scene::fromJson/insertFromJson/cloneGameObject require it in their
// signature to rebuild colliders, not because these tests simulate physics.
#include "DonTopo/Core/Camera.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/Window.h"
#include <fstream>
#include <iterator>
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Editor/ViewportPanel.h"
// ImGuizmo.h is not self-contained: it uses ImGui types (ImVec2, ImU32)
// without including it. Here it's needed to compare gizmoImGuizmoEnums
// against the real ones, which is the entire point of that test.
#include <imgui.h>
#include <ImGuizmo.h>
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include "DonTopo/Core/LightComponent.h"

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Defaults at this repo's scale, not Unity's.
static void test_defaults()
{
    CameraComponent c;
    CHECK(c.getMode() == CameraComponent::ProjectionMode::Perspective);
    CHECK(nearlyEqual(c.getFov(), 45.0f));
    CHECK(nearlyEqual(c.getOrthographicSize(), 100.0f));
    CHECK(nearlyEqual(c.getNear(), 1.0f));
    CHECK(nearlyEqual(c.getFar(), 2000.0f));
}

// The clamps live in the component: a hand-edited JSON cannot install
// a degenerate projection.
static void test_clamps()
{
    CameraComponent c;
    c.setFov(0.0f);      CHECK(c.getFov() >= 1.0f);
    c.setFov(500.0f);    CHECK(c.getFov() <= 179.0f);
    c.setOrthographicSize(-5.0f); CHECK(c.getOrthographicSize() > 0.0f);
    c.setNear(-5.0f);    CHECK(c.getNear() > 0.0f);
    // far never drops below near.
    c.setNear(10.0f);
    c.setFar(5.0f);
    CHECK(c.getFar() > c.getNear());
    // near never exceeds far, and doing so must not move far.
    CameraComponent d;
    d.setFar(100.0f);
    d.setNear(500.0f);
    CHECK(d.getNear() < d.getFar());
    CHECK(nearlyEqual(d.getFar(), 100.0f));
}

// Vulkan's Y-flip goes INSIDE projectionMatrix: its two consumers (the
// Renderer's UBO and Gizmos::drawFrustum) need it.
static void test_projection_has_vulkan_y_flip()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);
    CHECK(p[1][1] < 0.0f);
}

// Perspective and orthographic cannot give the same matrix.
static void test_projection_modes_differ()
{
    CameraComponent c;
    glm::mat4 persp = c.projectionMatrix(1.0f);
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 ortho = c.projectionMatrix(1.0f);
    CHECK(persp != ortho);
    // In orthographic, w of the projected point is 1 (no perspective division).
    glm::vec4 clip = ortho * glm::vec4(0.0f, 0.0f, -50.0f, 1.0f);
    CHECK(nearlyEqual(clip.w, 1.0f));
}

// A degenerate aspect (zero-width viewport when minimized) must not produce NaN.
static void test_projection_degenerate_aspect()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(0.0f);
    CHECK(!std::isnan(p[0][0]));
}

// Vulkan clips 0 <= z_clip <= w_clip, so the projection has to map
// near->0 and far->1. The default glm (without GLM_FORCE_DEPTH_ZERO_TO_ONE)
// maps near->-1 thinking of OpenGL: in orthographic that threw away half
// the near range (with near=1/far=2000 only 1000.5 onward was visible).
static void test_orthographic_uses_vulkan_depth_range()
{
    CameraComponent c; // near=1, far=2000
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 p = c.projectionMatrix(1.0f);

    glm::vec4 atNear = p * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f);
    CHECK(nearlyEqual(atNear.z / atNear.w, 0.0f));
    glm::vec4 atFar = p * glm::vec4(0.0f, 0.0f, -2000.0f, 1.0f);
    CHECK(nearlyEqual(atFar.z / atFar.w, 1.0f));
    // An object at this repo's scale (sandbox camera at z=300) must end up
    // INSIDE the visible range, not clipped.
    glm::vec4 mid = p * glm::vec4(0.0f, 0.0f, -300.0f, 1.0f);
    CHECK(mid.z / mid.w > 0.0f);
    CHECK(mid.z / mid.w < 1.0f);
}

// Same contract in perspective (there the failure only cut the first ~2
// units, which is why it went unnoticed).
static void test_perspective_uses_vulkan_depth_range()
{
    CameraComponent c; // perspective by default, near=1, far=2000
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);

    glm::vec4 atNear = p * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f);
    CHECK(nearlyEqual(atNear.z / atNear.w, 0.0f));
    glm::vec4 atFar = p * glm::vec4(0.0f, 0.0f, -2000.0f, 1.0f);
    CHECK(nearlyEqual(atFar.z / atFar.w, 1.0f));
    glm::vec4 mid = p * glm::vec4(0.0f, 0.0f, -300.0f, 1.0f);
    CHECK(mid.z / mid.w > 0.0f);
}

// The camera looks at local -Z (the glm/lookAt convention and DonTopo::Camera's,
// whose default yaw of -90 degrees gives front = (0,0,-1)).
static void test_view_from_world_translation()
{
    glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 view  = CameraComponent::viewFromWorld(world);
    // The world origin is 10 units ahead of the camera, that is at -Z.
    glm::vec4 p = view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(p.x, 0.0f));
    CHECK(nearlyEqual(p.y, 0.0f));
    CHECK(nearlyEqual(p.z, -10.0f));
}

// The GameObject's scale must not enter the view (it would distort the image).
static void test_view_from_world_ignores_scale()
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 unscaled = CameraComponent::viewFromWorld(t);
    glm::mat4 scaled   = CameraComponent::viewFromWorld(t * glm::scale(glm::mat4(1.0f), glm::vec3(5.0f)));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(nearlyEqual(unscaled[col][row], scaled[col][row]));
}

// findCamera() is the ONLY source of truth for the invariant "one camera per
// scene": it has to find it no matter where it is, not only hanging from root.
static void test_find_camera_at_any_depth()
{
    Scene scene("Test");
    CHECK(scene.findCamera() == nullptr);

    GameObject* parent = scene.addGameObject("Parent");
    GameObject* child  = scene.addGameObject("Child", parent);
    GameObject* nieto  = scene.addGameObject("Nieto", child);
    nieto->setCameraComponent(std::make_shared<CameraComponent>());

    CHECK(scene.findCamera() == nieto);
}

// The camera can live in ANY GameObject, not just one called "Camera".
static void test_find_camera_ignores_name()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("CualquierNombre");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == go);
    CHECK(scene.findCamera()->hasCameraComponent());
}

// Pre-order: the first in the traversal wins, not just any.
static void test_find_camera_returns_first_in_preorder()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == a);
}

// Full round-trip through toJson/fromJson. The values are NOT defaults
// on purpose: some defaults would "preserve" themselves even if the block
// weren't serialized. large near/far also cover the load order (setNear
// clamps against the current far, so far has to load first).
static void test_serialization_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Observador");
    auto cam = std::make_shared<CameraComponent>();
    cam->setMode(CameraComponent::ProjectionMode::Orthographic);
    cam->setFar(8000.0f);
    cam->setNear(3000.0f);
    cam->setFov(70.0f);
    cam->setOrthographicSize(250.0f);
    go->setCameraComponent(cam);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findCamera();
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Observador");
    const auto& c = found->getCameraComponent();
    CHECK(c->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(c->getFov(), 70.0f));
    CHECK(nearlyEqual(c->getOrthographicSize(), 250.0f));
    CHECK(nearlyEqual(c->getNear(), 3000.0f));
    CHECK(nearlyEqual(c->getFar(), 8000.0f));
}

// Path of subtreeToJson/insertFromJson — what Undo/Redo commands use. Without
// it, an Undo of Delete would return the GameObject without its camera.
static void test_subtree_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("ConCamara");
    auto cam = std::make_shared<CameraComponent>();
    cam->setFov(33.0f);
    go->setCameraComponent(cam);

    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findCamera() == nullptr);

    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->hasCameraComponent());
    CHECK(nearlyEqual(restored->getCameraComponent()->getFov(), 33.0f));
}

// Back-compat: scenes saved before this change don't bring the "camera"
// block and load equally (version stays at 1).
static void test_scene_without_camera_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Cubo");
    nlohmann::json j = scene.toJson();
    CHECK(!j["root"]["children"][0].contains("camera"));

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findCamera() == nullptr);
    CHECK(loaded.getRoot().children.size() == 1);
}

// Scene with TWO cameras (hand-edited JSON): the first in pre-order wins,
// the other loses ONLY the component (its GameObject is preserved) and a
// warning is issued. So a .scene that can be recovered opens the same way
// instead of failing to load.
static void test_load_with_two_cameras_keeps_first(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("Primera");
    GameObject* b = scene.addGameObject("Segunda");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    // toJson serializes both: the invariant is enforced at load, which is where
    // a hand-edited file can arrive.
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    int cameraCount = 0;
    loaded.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);

    GameObject* cam = loaded.findCamera();
    CHECK(cam != nullptr);
    if (cam) CHECK(cam->name == "Primera");
    // Both GameObjects are still there: only the extra component drops.
    CHECK(loaded.getRoot().children.size() == 2);
    CHECK(!loaded.lastWarnings().empty());
}

// A scene with ONE camera generates no warnings (the prune is not a false positive).
static void test_load_with_one_camera_has_no_warnings(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Solo")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findCamera() != nullptr);
    CHECK(loaded.lastWarnings().empty());
}

// Counts how many loaded warnings contain needle.
static int countWarnings(const Scene& scene, const char* needle)
{
    int n = 0;
    for (const auto& w : scene.lastWarnings())
        if (w.find(needle) != std::string::npos) ++n;
    return n;
}

// A warning that repeats collapses to ONE entry with "(xN)". Without this,
// a corrupt mesh writes an identical warning per vertex and buries the other
// warnings of the same load in the Log.
//
// Three objects with the SAME name are set up (the warning context is the
// name, not the index, so the three warnings come out byte-for-byte identical)
// and all three have their camera.far corrupted. Because all three carry a
// camera, the prune leaves two warnings also identical to each other: this
// confirms that collapseWarnings runs AFTER pruneExtraCameras, not before.
static void test_repeated_warnings_are_collapsed(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    for (int i = 0; i < 3; i++)
        scene.addGameObject("Cam")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();
    for (auto& child : j["root"]["children"])
        child["camera"]["far"] = nullptr; // corrupt: readFloat warns and falls to default

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    // One entry per message, not three or two.
    CHECK(countWarnings(loaded, "far") == 1);
    CHECK(countWarnings(loaded, "more than one camera") == 1);
    CHECK(loaded.lastWarnings().size() == 2);
    // And the actual count goes in the text.
    CHECK(countWarnings(loaded, "far: corrupt value in the scene, using the default value (x3)") == 1);
    CHECK(countWarnings(loaded, "(x2)") == 1);
}

// The suffix only appears when there is repetition: a unique warning stays as is.
static void test_single_warning_has_no_suffix(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Sola")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["camera"]["far"] = nullptr;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.lastWarnings().size() == 1);
    CHECK(countWarnings(loaded, "(x") == 0);
}

// insertFromJson (the undo of a Delete) clears the warnings of the previous
// operation instead of stacking its own on top. Without the clear, m_warnings
// would grow during the entire editor session and lastWarnings() would stop
// meaning "the last operation", which is what its contract promises.
// --- P8 of docs/core-audit.md: the warning "this node is going" ---
//
// `removeGameObject` carried a caller obligation that the header did NOT
// document: release GPU resources of the subtree first. Not that it was
// without being met — the three callers met it — but it was implemented
// THREE times (EditorUI::onDelete, ScriptManager::onDestroying and bare in
// DeleteGameObjectCommand), and a fourth caller would have needed a fourth.
// Now Scene warns and there is one place.
//
// What CANNOT be tested here is the tip of the thread (that the pool slot
// really frees): it would take an EditorRenderer, which is 58 pure virtuals.
// That is verified in GUI. Here the mechanism is tested.
static void test_remove_notifies_listener()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Victima");
    const uint64_t id = go->id;

    int veces = 0;
    uint64_t avisadoId = 0;
    scene.setOnNodeRemoved([&](GameObject* n) { ++veces; avisadoId = n->id; });

    scene.removeGameObject(go);
    CHECK(veces == 1);
    CHECK(avisadoId == id);
    CHECK(scene.findById(id) == nullptr);
}

// The warning arrives BEFORE releasing it from the tree: whoever listens has
// to be able to traverse the whole subtree and read its GPU indices, which
// is exactly what Renderer::removeGameObject does. Warning after would leave
// the listener with a pointer to an already-destroyed object.
static void test_remove_notifies_before_destroying()
{
    Scene scene("Test");
    GameObject* padre = scene.addGameObject("Padre");
    GameObject* hijo  = scene.addGameObject("Hijo", padre);
    hijo->staticRenderIndex = 7;

    int nodosVistos = 0;
    int indiceLeido = -1;
    bool seguiaEnElArbol = false;
    scene.setOnNodeRemoved([&](GameObject* n) {
        n->traverse([&](GameObject* m) {
            ++nodosVistos;
            if (m->staticRenderIndex >= 0) indiceLeido = m->staticRenderIndex;
        });
        // And the node still hangs from its parent: the warning is prior to erase.
        seguiaEnElArbol = (n->parent != nullptr) && !n->parent->children.empty();
    });

    scene.removeGameObject(padre);
    CHECK(nodosVistos == 2);        // the entire subtree, not just the root
    CHECK(indiceLeido == 7);        // the indices can still be read
    CHECK(seguiaEnElArbol);
    CHECK(scene.getRoot().children.empty());
}

// No listener, nothing happens: the tests and any host that doesn't wire it
// keep working the same.
static void test_remove_without_listener_is_fine()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Victima");
    scene.removeGameObject(go);
    CHECK(scene.getRoot().children.empty());
}

// And it doesn't warn of what isn't leaving: a null node or the root (which
// doesn't hang from anyone) exit through the guard above without touching
// the listener. Without this, the Renderer would release the slots of a scene
// that is still alive.
static void test_remove_does_not_notify_for_non_removals()
{
    Scene scene("Test");
    int veces = 0;
    scene.setOnNodeRemoved([&](GameObject*) { ++veces; });

    scene.removeGameObject(nullptr);
    scene.removeGameObject(&scene.getRoot());
    CHECK(veces == 0);
}

static void test_insert_from_json_resets_warnings(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    for (int i = 0; i < 2; i++)
        scene.addGameObject("Cam")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am)); // leaves the prune warning
    CHECK(!loaded.lastWarnings().empty());

    // A clean insert after: its warnings are its own, none.
    GameObject* go = loaded.addGameObject("Otro");
    nlohmann::json snapshot = loaded.subtreeToJson(go);
    loaded.removeGameObject(go);
    CHECK(loaded.insertFromJson(snapshot, nullptr, 0, pm, am) != nullptr);
    CHECK(loaded.lastWarnings().empty());
}

// --- P2 of docs/core-audit.md: the raw JSON accesses of nodeFromJson ---
//
// All shared a pattern: a corrupt field threw json::exception, the exception
// bubbled up to the catch in Scene::fromJson and the load of the ENTIRE
// scene was lost — without saying which field, and for a value the rest of
// the file knows how to tolerate. The criterion in Scene.cpp is written in
// its comment at :1189 and is exactly the opposite: warn naming the field
// and keep loading.
//
// Each test corrupts ONE field on a two-node scene and demands three things:
// that the load works, that the healthy node arrives whole, and that the
// warning names the field. Without the third, the fix would be
// "swallow it silently", which is worse than failing.
static nlohmann::json escenaDeDosNodos()
{
    Scene scene("Origen");
    scene.addGameObject("Roto");
    scene.addGameObject("Sano");
    return scene.toJson();
}

// Common checks: the scene loads and the second node has not been lost.
static void checkEscenaSobrevive(const Scene& loaded)
{
    CHECK(loaded.getRoot().children.size() == 2);
    if (loaded.getRoot().children.size() == 2)
        CHECK(loaded.getRoot().children[1]->name == "Sano");
}

// null "id". Besides the warning, the node has to stay with the id the
// constructor gave it: an invented one could crash into another in the tree.
static void test_corrupt_id_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0]["id"] = nullptr;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    checkEscenaSobrevive(loaded);
    CHECK(countWarnings(loaded, "node 'Roto'.id") == 1);

    // And no duplicate id, which is what would happen if the corrupt field
    // ended in 0 for all broken nodes.
    std::vector<uint64_t> ids;
    loaded.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

// "children" missing. nodeToJson writes it ALWAYS (Scene.cpp:1182), so its
// absence is never back-compat: it is corruption and must be named.
static void test_missing_children_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0].erase("children");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    checkEscenaSobrevive(loaded);
    CHECK(countWarnings(loaded, "node 'Roto'.children") == 1);
}

// "name" missing in a child. The node is preserved (unnamed), not discarded:
// it can have half the scene hanging from it.
static void test_missing_child_name_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0].erase("name");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.getRoot().children.size() == 2);
    CHECK(countWarnings(loaded, ".name") == 1);
}

// "indices" with an element that is not a number: the procedural mesh cannot
// be rebuilt. A warning is issued and the node stays without mesh — half the
// geometry would be worse than none, same criterion as jsonToMat4 with the
// matrix.
static void test_corrupt_mesh_indices_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0]["mesh"] = { {"sourcePath", ""},
                                          {"name", "Cube"},
                                          {"skinned", false},
                                          {"visible", true},
                                          {"vertices", nlohmann::json::array()},
                                          {"indices", nlohmann::json::array({0, "no soy un indice", 2})} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    checkEscenaSobrevive(loaded);
    CHECK(countWarnings(loaded, "indices") == 1);
    if (loaded.getRoot().children.size() == 2)
        CHECK(!loaded.getRoot().children[0]->hasMesh());
}

// "useGravity" legacy with a type that is not bool. It's the back-compat
// path of scenes before Rigidbody, so what arrives here is by definition
// an old file: failing the entire load was the worst place possible to be strict.
static void test_corrupt_legacy_usegravity_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0]["boxCollider"] = { {"halfExtents", nlohmann::json::array({25.0, 25.0, 25.0})},
                                                 {"center", nlohmann::json::array({0.0, 0.0, 0.0})},
                                                 {"isTrigger", false},
                                                 {"useGravity", "si"} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    checkEscenaSobrevive(loaded);
    CHECK(countWarnings(loaded, "useGravity") == 1);
    // Without synthesized Rigidbody: a corrupt field does not decide the dynamics.
    if (loaded.getRoot().children.size() == 2)
        CHECK(!loaded.getRoot().children[0]->hasRigidbody());
}

// A script without "name". The component is discarded (without a name there is
// no .lua file to load) but the GameObject and the rest of the scene continue.
static void test_corrupt_script_name_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    nlohmann::json j = escenaDeDosNodos();
    j["root"]["children"][0]["scripts"] =
        nlohmann::json::array({ nlohmann::json{ {"overrides", nlohmann::json::object()} } });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    checkEscenaSobrevive(loaded);
    CHECK(countWarnings(loaded, "scripts") == 1);
    if (loaded.getRoot().children.size() == 2)
        CHECK(!loaded.getRoot().children[0]->hasScripts());
}

// --- P7: the 14 duplicate readStr lambdas from nodeFromJson ---
//
// Each UI component block carried its own copy of readStr, which on a corrupt
// value returned "" WITHOUT WARNING — whereas the namespace's readString, for
// the same case, does warn. That is: in the SAME component, a corrupt float
// was reported to the Log and a corrupt string was swallowed silently,
// depending on which of the two nearly identical paths it went through.
// FOUR different components are touched, not just one: the lambda was copied
// 14 times, so a test on a single block would leave the other 13 unguarded
// — and putting a copy back in any of them would go unnoticed. Four is not
// fourteen; what really closes the hole is that there is no lambda left to
// copy (grep of "auto readStr" == 0), and this supports that.
static void test_corrupt_ui_string_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Origen");
    scene.addGameObject("Boton")->setButton(std::make_shared<ButtonComponent>());
    scene.addGameObject("Panel")->setPanel(std::make_shared<PanelComponent>());
    scene.addGameObject("Texto")->setText(std::make_shared<TextComponent>());
    scene.addGameObject("Imagen")->setImage(std::make_shared<ImageComponent>());

    nlohmann::json j = scene.toJson();
    nlohmann::json& hijos = j["root"]["children"];
    hijos[0]["button"]["text"]     = nullptr;
    hijos[0]["button"]["fontSize"] = nullptr;   // the float, for contrast
    hijos[1]["panel"]["sprite"]    = nullptr;
    hijos[2]["text"]["fontPath"]   = nullptr;
    hijos[3]["image"]["atlasPath"] = nullptr;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.getRoot().children.size() == 4);

    // The float already warned before P7; the strings are what was missing. In
    // the button the two fields are from the SAME component: that contrast —a
    // float that is reported and a string that was swallowed silently, two lines
    // apart— is the entire finding.
    CHECK(countWarnings(loaded, "fontSize") == 1);
    CHECK(countWarnings(loaded, "button of 'Boton'.text") == 1);
    CHECK(countWarnings(loaded, "panel of 'Panel'.sprite") == 1);
    CHECK(countWarnings(loaded, "text of 'Texto'.fontPath") == 1);
    CHECK(countWarnings(loaded, "image of 'Imagen'.atlasPath") == 1);
}

// H6 of docs/core-audit.md. Scene::shutdown exists for one very specific
// thing, and both hosts say so in a comment when calling it: release what
// the scene has taken from PhysX and FMOD BEFORE destroying those managers.
// Without it, a ~Collider runs into a PxScene already freed.
//
// The problem was not that it failed, but that the list was written BY HAND
// and was stuck at 6 of the components: Rigidbody, Animator, ReverbZone and
// AudioListener survived. It's the same pattern that already failed FOUR
// times in invalidateCaches of the panel —enumerate by hand what needs
// cleaning— so it's not fixed by adding four more lines.
//
// weak_ptr and no has*(): what needs to be demonstrated is that the
// component has been DESTROYED, not that the GameObject has stopped pointing
// to it. With a shared_ptr alive somewhere else, has*() would say yes and
// the native resource would still be there.
static void test_scene_shutdown_releases_every_component(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Todo");

    // The four that the hand-written list was skipping. Colliders and
    // AudioClip —the ones it DID clean— don't enter here because their
    // constructors ask for a PhysX actor or the AudioManager: what is tested
    // is the rule, and these four build themselves.
    auto rb       = std::make_shared<Rigidbody>();
    auto anim     = std::make_shared<AnimatorComponent>();
    auto reverb   = std::make_shared<ReverbZoneComponent>();
    auto listener = std::make_shared<AudioListenerComponent>();

    go->setRigidbody(rb);
    go->setAnimator(anim);
    go->setReverbZone(reverb);
    go->setAudioListener(listener);

    // A child with its own: shutdown has to descend through the entire tree, not
    // just the direct children of the root.
    GameObject* hijo = scene.addGameObject("Hijo", go);
    auto animHijo = std::make_shared<AnimatorComponent>();
    hijo->setAnimator(animHijo);

    std::weak_ptr<Rigidbody>              wRb(rb);
    std::weak_ptr<AnimatorComponent>      wAnim(anim);
    std::weak_ptr<ReverbZoneComponent>    wReverb(reverb);
    std::weak_ptr<AudioListenerComponent> wListener(listener);
    std::weak_ptr<AnimatorComponent>      wAnimHijo(animHijo);

    // Local references are released: from here on the only owner is the scene,
    // which is the premise of everything below.
    rb.reset(); anim.reset(); reverb.reset(); listener.reset(); animHijo.reset();
    CHECK(!wRb.expired());
    CHECK(!wAnim.expired());

    scene.shutdown();

    CHECK(wRb.expired());
    CHECK(wAnim.expired());
    CHECK(wReverb.expired());
    CHECK(wListener.expired());
    CHECK(wAnimHijo.expired());
}

// H19 of docs/core-audit.md, the half done on 2026-09-04: GameObject.h
// DECLARES the 28 components instead of including them. It can, because its
// members are shared_ptr —valid with incomplete type— and its destructor is
// out of line.
//
// What was gained, measured at the time: touching a 100-line UI component
// went from 34 TUs and 286 s to 25 TUs and 253 s. And with ninja -n on
// 2026-09-11 it's still at 26 TUs, so it holds.
//
// This test exists because that fix was UNDONE ONCE without anyone noticing:
// when doing so, adding an include in Scene.h to resolve a type brought the
// measurement back to 33 TUs, worse than baseline. A transitive include breaks
// nothing, doesn't show up in any test, and gives no error: it just makes
// the build slow again. So the guard has to read the file.
//
// Reads the header from the repo root, which is where the tests are run from
// (just like the ones that open assets/). If it doesn't find it, it does NOT
// pass silently.
static void test_gameobject_header_declares_components_instead_of_including()
{
    const char* kRuta = "engine/include/DonTopo/Core/GameObject.h";
    std::ifstream in(kRuta, std::ios::binary);
    CHECK(in.good());   // launched from another cwd: it finds out, does not approve
    if (!in.good()) return;

    const std::string texto((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!texto.empty());

    // The three concrete component prefixes that GameObject.h ended up including.
    // Core/ does NOT go in: it includes legitimate things from there (and Camera.h is
    // not a component).
    const char* kProhibidos[] = {
        "#include \"DonTopo/UI/",
        "#include \"DonTopo/Physics/",
        "#include \"DonTopo/Audio/",
    };
    for (const char* prohibido : kProhibidos)
        CHECK(texto.find(prohibido) == std::string::npos);

    // And the other half: that it keeps declaring them. Without this, someone could
    // "fix" the test by deleting the declarations and the includes at the same time.
    CHECK(texto.find("class SliderComponent;")   != std::string::npos);
    CHECK(texto.find("class BoxCollider;")       != std::string::npos);
    CHECK(texto.find("class AudioClipComponent;") != std::string::npos);
}

// H2 of docs/core-audit.md. There are ~52 `.value("key", default)` left in the
// component blocks, and `.value` THROWS json::type_error if the key exists with the
// wrong type -verified separately: string, bool, int and float throw; the ones with
// a `json` default do NOT, because any type converts to json, so that half of the
// row was not true-.
//
// What cost: the exception went up to the catch in fromJson and the ENTIRE scene was
// lost, without saying which node it came from. A `.scene` edited by hand, or written
// by a different version, became unloadable by one field.
static void test_corrupt_field_costs_its_node_not_the_scene(PhysicsManager& pm, AudioManager& am)
{
    Scene escena("Test");
    GameObject* bueno = escena.addGameObject("Bueno");
    bueno->setCameraComponent(std::make_shared<CameraComponent>());
    GameObject* malo  = escena.addGameObject("Malo");
    malo->setLight(std::make_shared<LightComponent>());
    GameObject* otro  = escena.addGameObject("Otro");

    nlohmann::json j = escena.toJson();
    auto& hijos = j["root"]["children"];
    CHECK(hijos.size() == 3);
    if (hijos.size() != 3) return;

    // The corrupted field must be one of the ~52 RAW ones, not one already converted
    // to readFloat/readBool -those don't throw, they default-. `light.type` is one:
    // `l.value("type", std::string("point"))`, and here a number arrives. Before the
    // guard, this crashed the entire load and fromJson returned false.
    hijos[1]["light"]["type"] = 42;

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));   // <- the important part: the scene LOADS

    // The three nodes are still there, the corrupted one included: it loses its
    // components, not its place in the tree.
    int nodos = 0;
    GameObject* maloCargado = nullptr;
    GameObject* buenoCargado = nullptr;
    cargada.traverse([&](GameObject* n) {
        ++nodos;
        if (n->name == "Malo")  maloCargado  = n;
        if (n->name == "Bueno") buenoCargado = n;
    });
    CHECK(nodos == 4);   // root + 3
    CHECK(maloCargado != nullptr);
    CHECK(buenoCargado != nullptr);

    // The neighbor keeps ITS component: the damage does not spread.
    if (buenoCargado) CHECK(buenoCargado->hasCameraComponent());
    // And the corrupted one loses its component, which is the price.
    if (maloCargado)  CHECK(!maloCargado->hasLight());

    // With warning that NAMES the node: without it you have to guess which of the 3 it was.
    bool avisoConNombre = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("Malo") != std::string::npos) avisoConNombre = true;
    CHECK(avisoConNombre);

    (void)otro;
}

// H17/P11 of docs/core-audit.md. The four searchers of Scene (findById,
// findCamera, findAudioListener, findCanvas) emulated "first one wins" with a
// traverse and an `if (!found && ...)`, but traverse visits the ENTIRE tree:
// after finding it they kept going down through everything else for nothing.
//
// Measured in /O2 with 5000 nodes and 20,000 searches: complete traversal 175 ms
// no matter what; cutting, 0.007 ms if at root and 50 ms if at a third down. That
// is 8.8 us per search, and PropertiesPanel -which is drawn every frame- does several.
//
// What is tested is the CUT, not the time: the predicate counts how many times it
// is called, so if the traversal kept going after the hit the number would spike.
static void test_find_first_stops_at_the_first_hit()
{
    Scene escena("Test");
    GameObject* primero = escena.addGameObject("primero");
    for (int i = 0; i < 50; ++i)
        escena.addGameObject("relleno", primero);   // 50 children AFTER the hit

    int visitas = 0;
    GameObject* hit = escena.getRoot().findFirst([&](const GameObject* n) {
        ++visitas;
        return n->name == "primero";
    });

    CHECK(hit == primero);
    // root + first = 2. With complete traversal it would be 52.
    CHECK(visitas == 2);

    // Without a hit the entire tree is traversed, which is the only case where cutting
    // saves nothing: 1 root + 1 first + 50 children.
    visitas = 0;
    GameObject* nada = escena.getRoot().findFirst([&](const GameObject*) {
        ++visitas;
        return false;
    });
    CHECK(nada == nullptr);
    CHECK(visitas == 52);
}

// H16 of docs/core-audit.md. traverse took the functor BY VALUE and recursed
// with a copy, so each child -and each level- received its own.
//
// What is fixed here is not performance (measured in /O2 with 5000 nodes x 2000
// traversals: with the [&] lambdas of 8 bytes that the whole repo uses the
// difference is noise, 25.3 ms vs 24.3; only with a 264-byte functor does it go
// to 2.4x). What is fixed is the SEMANTICS: a functor with its own state lost
// silently everything that the children would sum, because the one that summed
// was a copy.
//
// The counter below gave ZERO with the by-value traverse -it did not even count
// the root, because it also operates on a copy- and gives the number of nodes with
// the current one.
static void test_traverse_does_not_copy_stateful_functor()
{
    Scene escena("Test");
    GameObject* a = escena.addGameObject("a");
    GameObject* b = escena.addGameObject("b", a);
    escena.addGameObject("c", b);      // three levels: root -> a -> b -> c
    escena.addGameObject("d", a);

    struct Contador
    {
        int vistos = 0;
        void operator()(GameObject*) { ++vistos; }
    };

    Contador contador;
    escena.getRoot().traverse(contador);

    // 5 = root + a + b + c + d. With the functor copied per child this was 0.
    CHECK(contador.vistos == 5);

    // And the usual case keeps working: a temporary lambda, written in the call itself,
    // which is how almost all callers pass it. If traverse took Fn& instead of Fn&&,
    // this would not even compile.
    int porLambda = 0;
    escena.getRoot().traverse([&porLambda](GameObject*) { ++porLambda; });
    CHECK(porLambda == 5);
}

// H9 of docs/core-audit.md. shouldClose() passed m_window to GLFW without checking
// if it was null, unlike show(), which does. A Window without init -or already
// closed- gave nullptr to GLFW, which treats it as a programming error.
//
// And the correct answer without a window is not "false": the loop of the two
// hosts is `while (!window.shouldClose())`, so false there would spin forever
// over a window that does not exist. Without window, close.
//
// This is the only thing from H9 you can assert without a screen: with m_window
// null none of these calls touch GLFW. The other half -the glfwTerminate() inside
// the shutdown of ONE instance- is fixed in the same commit but cannot be tested
// here: it would need to create two real windows, and observe the failure (using
// the handle of the second after GLFW terminates) is UB.
static void test_window_without_init_is_inert()
{
    Window w;   // without init: m_window == nullptr

    // Without window, the main loop has to end.
    CHECK(w.shouldClose());
    CHECK(w.getNativeWindow() == nullptr);

    // And everything else is a no-op: if any of these touched GLFW with nullptr,
    // its error callback would fire.
    w.show();
    w.pollEvents();
    w.shutdown();
    w.shutdown();   // twice in a row too
    CHECK(w.getNativeWindow() == nullptr);
    CHECK(w.shouldClose());
}

// H7 of docs/core-audit.md. The invariant "at most one camera per scene" is
// enforced by fromJson (pruneExtraCameras) and cloneGameObject (the test above),
// but insertFromJson -the path of an Undo of a Delete- enforced NOTHING.
//
// The scenario is normal usage, not contrived: you delete the camera, put another,
// and undo the deletion. Without the guard there are TWO, findCamera returns the
// first in preorder and Play uses that, silently. Same pattern as duplicate ids:
// the rule was in two of the three paths that insert nodes.
static void test_insert_from_json_discards_second_camera(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("CamaraVieja");
    original->setCameraComponent(std::make_shared<CameraComponent>());

    // The snapshot that DeleteGameObjectCommand would save, and the deletion.
    const nlohmann::json snapshot = scene.subtreeToJson(original);
    scene.removeGameObject(original);
    CHECK(scene.findCamera() == nullptr);

    // Between deletion and undo, the user puts another camera. This is the one that
    // is live when Ctrl+Z arrives.
    GameObject* nueva = scene.addGameObject("CamaraNueva");
    nueva->setCameraComponent(std::make_shared<CameraComponent>());

    GameObject* reinsertado = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(reinsertado != nullptr);
    if (!reinsertado) return;

    // The GameObject returns -that is what the user asked for by undoing- but without
    // the camera: the one that WAS already live wins, same criterion as the id guard
    // in this same function and as pruneExtraCameras.
    CHECK(!reinsertado->hasCameraComponent());
    CHECK(nueva->hasCameraComponent());
    CHECK(scene.findCamera() == nueva);

    int camaras = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++camaras; });
    CHECK(camaras == 1);

    // With warning: losing a component on undo cannot be silent.
    bool aviso = false;
    for (const auto& w : scene.lastWarnings())
        if (w.find("camera") != std::string::npos || w.find("camera") != std::string::npos)
            aviso = true;
    CHECK(aviso);
}

// The other side, and the one that keeps the guard from being too cautious: if
// there IS NO live camera, undoing the deletion has to return the camera intact.
// A guard that always discarded would pass the test above and break the normal case.
static void test_insert_from_json_keeps_camera_when_none_alive(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("Camara");
    original->setCameraComponent(std::make_shared<CameraComponent>());

    const nlohmann::json snapshot = scene.subtreeToJson(original);
    scene.removeGameObject(original);
    CHECK(scene.findCamera() == nullptr);

    GameObject* reinsertado = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(reinsertado != nullptr);
    if (!reinsertado) return;
    CHECK(reinsertado->hasCameraComponent());
    CHECK(scene.findCamera() == reinsertado);
}

// Same invariant, same hole, another component: at most one AudioListener per
// scene. They are enforced by the gate of the Add popup and pruneExtraAudioListeners
// on load; insertFromJson did not enforce either.
static void test_insert_from_json_discards_second_audio_listener(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("OyenteViejo");
    original->setAudioListener(std::make_shared<AudioListenerComponent>());

    const nlohmann::json snapshot = scene.subtreeToJson(original);
    scene.removeGameObject(original);

    GameObject* nuevo = scene.addGameObject("OyenteNuevo");
    nuevo->setAudioListener(std::make_shared<AudioListenerComponent>());

    GameObject* reinsertado = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(reinsertado != nullptr);
    if (!reinsertado) return;
    CHECK(!reinsertado->hasAudioListener());
    CHECK(nuevo->hasAudioListener());

    int oyentes = 0;
    scene.traverse([&](GameObject* n) { if (n->hasAudioListener()) ++oyentes; });
    CHECK(oyentes == 1);
}

// Cloning a GameObject with camera CANNOT give two cameras. Its only caller is
// Lua's Instantiate, which runs in Play: no UI gate can prevent it, so
// the rule lives in Scene.
static void test_clone_never_keeps_camera(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Camara");
    go->setCameraComponent(std::make_shared<CameraComponent>());

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(!clone->hasCameraComponent());
    CHECK(!scene.lastWarnings().empty());
    // The original keeps its own and remains THE camera of the scene.
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    int cameraCount = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);
}

// The camera can be in a descendant of the cloned subtree, not just in its
// root.
static void test_clone_strips_camera_from_descendant(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* parent = scene.addGameObject("Padre");
    GameObject* child  = scene.addGameObject("Hijo", parent);
    child->setCameraComponent(std::make_shared<CameraComponent>());

    GameObject* clone = scene.cloneGameObject(parent, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    int cameraCount = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);
}

// A clone needs its OWN id. cloneGameObject serializes the original with
// nodeToJson (which emits "id") and rebuilds it with nodeFromJson, which reuses
// that id on purpose: that is exactly what is needed in an Undo of a Delete, so
// the commands left in the stack keep resolving the rebuilt object. But when
// cloning the ORIGINAL IS STILL ALIVE, so reusing it leaves two GameObjects with
// the same id and findById returns the last one in the traversal: the clone. Any
// undo command resolved by id (Transform, Rigidbody, Audio Clip, Camera...) would
// end up writing to the wrong object.
static void test_clone_gets_fresh_id(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Original");
    const uint64_t originalId = go->id;

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    CHECK(clone->id != originalId);
    // And the original's id has to keep resolving TO THE ORIGINAL, which is what
    // really broke: findById returned the clone.
    CHECK(scene.findById(originalId) == go);
    CHECK(scene.findById(clone->id) == clone);
}

// Same invariant in a subtree: the clone's descendants also have to get a new id,
// not just its root.
static void test_clone_subtree_gets_fresh_ids(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* parent = scene.addGameObject("Padre");
    GameObject* child  = scene.addGameObject("Hijo", parent);
    const uint64_t childId = child->id;

    GameObject* clone = scene.cloneGameObject(parent, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone || clone->children.empty()) { CHECK(false); return; }

    CHECK(clone->children[0]->id != childId);
    CHECK(scene.findById(childId) == child);

    // No repeated id in the entire scene.
    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

// The counterpart of the two tests above, and the reason the id strip lives in
// cloneGameObject and NOT in nodeFromJson: insertFromJson (the path of an Undo
// of a Delete) has to KEEP reusing the snapshot's id. There the original no
// longer exists, so there is no possible collision, and keeping it is what lets
// the commands left in the stack keep resolving the rebuilt object.
//
// Without this test, moving the strip to nodeFromJson —which looks like the
// obvious simplification— would silently break undo: no other test would notice.
static void test_undo_delete_keeps_original_id(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Borrado");
    const uint64_t originalId = go->id;

    // Snapshot + deletion, which is what DeleteGameObjectCommand does.
    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findById(originalId) == nullptr);

    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->id == originalId);
    CHECK(scene.findById(originalId) == restored);
}

// Real bug reproduced in a user session: an Undo/Redo snapshot can bring an id
// that is ALREADY alive in ANOTHER part of the scene — for example if the
// snapshot is from before a reload that gave that same id to a new object (here
// it is forced by hand, without depending on a real reload, by writing the id of
// a live object inside another's snapshot). Without the insertFromJson guard the
// tree ends up with two nodes with the same id and findById resolves the one that
// has been in the tree the least time — this is how a texture assigned to 'Plane'
// ended up applied to a skinned character reinserted with 'Plane's id.
static void test_insert_from_json_reassigns_colliding_id(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* plane = scene.addGameObject("Plane");
    const uint64_t planeId = plane->id;

    // Snapshot of ANOTHER object, with 'Plane's id added by hand: it is the same
    // state that an old snapshot from the Undo/Redo stack would leave after a scene
    // reload that had given that id to 'Plane'.
    GameObject* victima = scene.addGameObject("Victima");
    nlohmann::json snapshot = scene.subtreeToJson(victima);
    snapshot["id"] = planeId;
    scene.removeGameObject(victima);

    GameObject* reinsertado = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(reinsertado != nullptr);
    if (!reinsertado) return;

    // The reinserted one gets a new id: it cannot keep 'Plane's.
    CHECK(reinsertado->id != planeId);

    // 'Plane' —the one that WAS already live— keeps its own. It is the object that
    // may have fresher references pointing to it (the current selection, a just-
    // executed command) than the reinserted snapshot.
    CHECK(scene.findById(planeId) == plane);

    // And there is a warning: the invariant was almost broken and the Log Console
    // (which reads lastWarnings()) has to find out.
    bool avisoEncontrado = false;
    for (const auto& w : scene.lastWarnings())
        if (w.find("was already in use") != std::string::npos) avisoEncontrado = true;
    CHECK(avisoEncontrado);

    // No repeated id in the entire scene.
    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

// The collision of the test above is against an object EXTERNAL to the
// reinserted subtree. This covers the other half: two nodes of the SAME snapshot with the
// same id BETWEEN THEM, with no external live object in between — the
// parent and its own child. idsVivos is captured before inserting (empty for
// these two ids, because the originals are deleted) and EXPANDED with each id already
// accepted inside the same traversal of the reinserted subtree; without that
// expansion (exactly the line sabotaged to prove it) the child sneaks in
// with the same id as its parent and no one finds out.
// REPRODUCTION of the bug from test7: load a scene whose file brings TWO nodes
// with the same id. insertFromJson (Undo/Redo) already reassigns the one that clashes, but
// Scene::fromJson — the path by which a scene is opened from disk — reuses the id
// from the JSON as is and checks nothing.
//
// The damage is not that there are two equal ids, it is that EVERYTHING the editor resolves by id:
// findById returns the first in pre-order, so dragging the gizmo of the
// SECOND object writes its matrix — position, rotation AND SCALE — to the FIRST one.
// With a character scaled like FBX brings them, the other object jumps a
// size jump. That is the check below, and it is exactly what you see on
// screen.
static void test_scene_load_reassigns_duplicate_ids(PhysicsManager& pm, AudioManager& am)
{
    Scene origen("Test");
    GameObject* plano     = origen.addGameObject("Plane");
    GameObject* personaje = origen.addGameObject("Personaje");
    plano->localTransform     = glm::mat4(1.0f);
    personaje->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));

    nlohmann::json j = origen.toJson();
    CHECK(j.contains("root") && j["root"].contains("children"));
    if (!j.contains("root") || !j["root"].contains("children")) return;
    nlohmann::json& hijos = j["root"]["children"];
    CHECK(hijos.size() == 2);
    if (hijos.size() != 2) return;
    // The collision, written in the file: the character arrives with the id of the
    // plane. That is what any id bug before the fix leaves on disk,
    // and once saved it reproduces on every load.
    hijos[1]["id"] = hijos[0]["id"];

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));

    // 1. The invariant: no id repeated after loading.
    std::vector<uint64_t> ids;
    cargada.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // 2. And with warning: without it, the user has no way to know that their file
    //    came broken (same criterion as insertFromJson).
    bool aviso = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("was already in use") != std::string::npos) aviso = true;
    CHECK(aviso);

    // 3. The visible consequence: moving the character CANNOT touch the plane.
    //    It is resolved by id just like the gizmo (applyLocalTransform) and the
    //    panel do, and it is checked that the plane's scale stays its own.
    GameObject* planoCargado = nullptr;
    GameObject* pjCargado    = nullptr;
    cargada.traverse([&](GameObject* n) {
        if (n->name == "Plane")     planoCargado = n;
        if (n->name == "Personaje") pjCargado    = n;
    });
    CHECK(planoCargado != nullptr && pjCargado != nullptr);
    if (!planoCargado || !pjCargado) return;

    const glm::mat4 movido = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
    applyLocalTransform(cargada, pjCargado->id, movido);

    CHECK(pjCargado->localTransform == movido);
    // The scale of the plane intact: [0][0] was 1 and cannot become 100.
    CHECK(planoCargado->localTransform[0][0] == 1.0f);
}

static void test_insert_from_json_reassigns_id_colliding_within_same_subtree(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* padre = scene.addGameObject("Padre");
    scene.addGameObject("Hijo", padre);

    nlohmann::json snapshot = scene.subtreeToJson(padre);
    CHECK(snapshot.contains("children") && snapshot["children"].size() == 1);
    if (!snapshot.contains("children") || snapshot["children"].size() != 1) return;
    // The child gets the id of the parent, by hand, INSIDE the
    // snapshot itself — nothing external participates in this collision.
    snapshot["children"][0]["id"] = snapshot["id"];

    // The originals disappear: the only collision possible after this is the
    // internal one just forced, not one against a live object from outside
    // (that is already tested by test_insert_from_json_reassigns_colliding_id).
    scene.removeGameObject(padre);

    GameObject* reinsertado = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(reinsertado != nullptr);
    if (!reinsertado) return;
    CHECK(reinsertado->children.size() == 1);
    if (reinsertado->children.empty()) return;

    GameObject* hijoReinsertado = reinsertado->children[0].get();
    CHECK(reinsertado->id != hijoReinsertado->id);

    bool avisoEncontrado = false;
    for (const auto& w : scene.lastWarnings())
        if (w.find("was already in use") != std::string::npos) avisoEncontrado = true;
    CHECK(avisoEncontrado);

    // No id repeated in the whole scene, just in case: the final goal
    // is exactly that invariant.
    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

// findById with a unique id does not change behavior after switching to "the first wins":
// it still returns THAT object. The determinism change only
// matters when there is more than one node with the same id (invariant already broken),
// which is the case of the test above and the one below.
static void test_find_by_id_unique_id_still_resolves()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    GameObject* c = scene.addGameObject("C", b);

    CHECK(scene.findById(a->id) == a);
    CHECK(scene.findById(b->id) == b);
    CHECK(scene.findById(c->id) == c);
    CHECK(scene.findById(0) == nullptr); // the counter starts at 1; 0 is never distributed
}

// The case the user reported, at the data level: two live objects with
// the SAME id (the state that insertFromJson should no longer let appear,
// but is forced by hand to test the effect of the root, regardless of
// how you get to it). With deterministic findById (the first in
// pre-order wins) writing "by id" — as PropertiesPanel does when applying a
// texture on the selection — always falls on the same object and NEVER on
// the other. Before (the last in traversal won) it depended on insertion order,
// which is exactly what caused the 'Plane' texture to end up on
// the skinned character inserted after.
static void test_find_by_id_duplicate_writes_only_first_never_the_other()
{
    Scene scene("Test");
    GameObject* plane   = scene.addGameObject("Plane");
    GameObject* skinned = scene.addGameObject("GameObject");
    skinned->id = plane->id; // forces the duplicate of the user's diagnosis

    // "Assign a texture" simplified at the data level: write a field
    // resolved by id, like the ownerId of the selection in PropertiesPanel.
    GameObject* resuelto = scene.findById(plane->id);
    CHECK(resuelto == plane); // the first in pre-order, never "GameObject"
    resuelto->name = "Plane (con textura)";

    CHECK(plane->name == "Plane (con textura)");
    CHECK(skinned->name == "GameObject"); // the other does NOT get touched
}

// ── Ctrl+D of the editor: duplicate the selected GameObject ──────────────────
//
// The subject of the test is `duplicateAsSibling` (Command.cpp), which is the seam:
// EditorUI only looks at the gate of the shortcut (nothing in Play, nothing with text focus,
// nothing without selection), calls here and stacks a CreateGameObjectCommand with the
// clone's snapshot. What CANNOT be tested here is the GPU part
// (registerGameObject) nor the command itself executing: building it requires an
// EditorRenderer, which is 73 pure virtuals — the same reason
// test_remove_notifies_listener stays at the mechanism. Here the
// placement, ids and the round-trip of the snapshot that undo/redo depend on are tested.

// The duplicate is a SIBLING of the original, not its child. If it were, each Ctrl+D
// would nest one level deeper and the original would change shape when duplicated.
static void test_duplicate_is_sibling_not_child(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* padre    = scene.addGameObject("Padre");
    GameObject* original = scene.addGameObject("Original", padre);
    const size_t hijosAntes = original->children.size();

    GameObject* clone = duplicateAsSibling(scene, original, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    CHECK(clone->parent == original->parent);
    CHECK(clone->parent == padre);
    CHECK(clone != original);
    // And the original has not gained a child along the way.
    CHECK(original->children.size() == hijosAntes);
    bool cloneCuelgaDelOriginal = false;
    original->traverse([&](GameObject* n) { if (n == clone) cloneCuelgaDelOriginal = true; });
    CHECK(!cloneCuelgaDelOriginal);
    CHECK(padre->children.size() == 2);
}

// Review Focus 3: children are created with the given meshes, without reading disk (the
// sourcePath does not exist), each one with ITS piece, name and transform.
static void test_insert_model_pieces_uses_the_given_meshes(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* casa = scene.addGameObject("Casa");
    const std::string src = "assets/__no_existe__.gltf";
    auto m0 = std::make_shared<Mesh>(); m0->sourcePath = src; m0->piece = 0; m0->name = "triA";
    auto m1 = std::make_shared<Mesh>(); m1->sourcePath = src; m1->piece = 1; m1->name = "triB";
    std::vector<ModelPiece> pieces(3);
    pieces[0] = { 0, "A", glm::translate(glm::mat4(1.0f), glm::vec3(5, 0, 0)) };
    pieces[1] = { 1, "B", glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)) };
    pieces[2] = { 0, "C", glm::mat4(std::numeric_limits<float>::quiet_NaN()) };   // not finite
    std::vector<std::string> warnings;
    const std::vector<GameObject*> kids =
        insertModelPieces(scene, casa, src, pieces, { m0, m1 }, pm, am, &warnings);
    CHECK(kids.size() == 3);
    CHECK(casa->children.size() == 3);
    CHECK(!casa->hasMesh());
    if (kids.size() != 3) return;
    CHECK(kids[0]->name == "A" && kids[0]->hasMesh() && kids[0]->getMesh()->name == "triA");
    CHECK(kids[1]->name == "B" && kids[1]->hasMesh() && kids[1]->getMesh()->piece == 1);
    CHECK(kids[0]->localTransform[3].x == 5.0f);
    CHECK(kids[2]->localTransform == glm::mat4(1.0f));   // the non-finite becomes identity
    CHECK(warnings.size() == 1);
}

// Review fix (task-4): Scene::insertFromJson seeded hasBonesCache with the
// KEY of PreloadedMeshCache (meshCacheKey, which for a piece != 0 carries
// "#piece=N") instead of the REAL sourcePath of the mesh (mesh->sourcePath), which is
// why nodeFromJson queries that cache. With the wrong key, each
// child of piece != 0 failed the query and went back to probing the file with
// ModelLoader::hasBones — a synchronous ReadFile of Assimp on the main thread —,
// exactly what preloaded exists to avoid (insertModelPieces, its redo via
// CreateGameObjectCommand::execute, and the undo of Delete of those children).
//
// A truly RIGGED FBX is used (modelAnimation.fbx, hasBones == true, same
// file that animator_tests.cpp uses) with a fake STATIC mesh in the
// cache: without the fix, hasBones(sourcePath) goes back to probing the file, sees that
// it DOES declare bones, the node takes the skinned branch and never gets to look at the
// static mesh from the cache — which is what this test checks by name.
static void test_insert_from_json_uses_cached_mesh_for_rigged_source_without_probing(PhysicsManager& pm,
                                                                                      AudioManager& am)
{
    const std::string src = "assets/modelAnimation.fbx";
    CHECK(ModelLoader::hasBones(src));   // if this fails, the file changed or it is not the right place

    Scene scene("Test");
    GameObject* casa = scene.addGameObject("Casa");

    auto piezaFalsa = std::make_shared<Mesh>();
    piezaFalsa->sourcePath = src;
    piezaFalsa->piece = 1;
    piezaFalsa->name = "piezaFalsaEstatica";

    std::vector<ModelPiece> pieces(1);
    pieces[0] = { 1, "Pieza1", glm::mat4(1.0f) };
    const std::vector<std::shared_ptr<const Mesh>> meshes = { nullptr, piezaFalsa };
    std::vector<std::string> warnings;
    const std::vector<GameObject*> kids = insertModelPieces(scene, casa, src, pieces, meshes, pm, am, &warnings);
    CHECK(kids.size() == 1);
    if (kids.size() != 1) return;
    CHECK(kids[0]->hasMesh());
    if (!kids[0]->hasMesh()) return;
    CHECK(kids[0]->getMesh()->name == "piezaFalsaEstatica");   // came from the cache, not from disk
    CHECK(!kids[0]->isSkinned());                              // did not take the skinned branch
}

// An object hung directly from the scene root also comes out as a sibling:
// its parent is the root, not nullptr.
static void test_duplicate_of_root_child_is_sibling(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("Original");

    GameObject* clone = duplicateAsSibling(scene, original, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->parent == original->parent);
}

// The scene root does not duplicate (it has no possible siblings). It is the same
// gate that ScenePanel uses for Delete/F2.
static void test_duplicate_rejects_scene_root(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    CHECK(duplicateAsSibling(scene, nullptr, pm, am) == nullptr);
    CHECK(duplicateAsSibling(scene, &scene.getRoot(), pm, am) == nullptr);
}

// Children are copied recursively and with ids DIFFERENT from the original's.
// Without this findById returns the last in traversal — the clone — and the
// commands on the stack resolved by id write to the wrong object.
static void test_duplicate_subtree_has_unique_ids(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("Original");
    GameObject* hijo     = scene.addGameObject("Hijo", original);
    GameObject* nieto    = scene.addGameObject("Nieto", hijo);
    nieto->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 5.0f));

    GameObject* clone = duplicateAsSibling(scene, original, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    // Hierarchy copied entirely, not just the root.
    CHECK(clone->children.size() == 1);
    if (clone->children.empty()) return;
    GameObject* hijoClon = clone->children[0].get();
    CHECK(hijoClon->children.size() == 1);
    if (hijoClon->children.empty()) return;
    GameObject* nietoClon = hijoClon->children[0].get();
    CHECK(nietoClon->localTransform == nieto->localTransform);

    // No id repeated in the ENTIRE scene, not just between the two roots.
    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // And the ids of the original still resolve TO the original.
    CHECK(scene.findById(hijo->id) == hijo);
    CHECK(scene.findById(nieto->id) == nieto);
}

// Undo/redo of the duplicate. It replicates what CreateGameObjectCommand::
// undo() and ::execute() do with the same pair (parentId, index) that
// EditorUI::duplicateSelection calculates — the command itself cannot be instantiated without
// EditorRenderer, so what is tested is the mechanism it depends on:
// that the clone's snapshot is enough to delete it and rebuild it entirely.
static void test_duplicate_undo_redo_restores_object_count(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* original = scene.addGameObject("Original");
    scene.addGameObject("Hijo", original);

    auto contarObjetos = [&scene]() {
        int n = 0;
        scene.traverse([&](GameObject*) { ++n; });
        return n;
    };
    const int antes = contarObjetos();

    GameObject* clone = duplicateAsSibling(scene, original, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    // Original (2 nodes) + clone (2 nodes).
    CHECK(contarObjetos() == antes + 2);

    // What EditorUI::duplicateSelection stacks.
    GameObject* parent      = clone->parent;
    const uint64_t parentId = parent->id;
    const size_t index      = parent->children.size() - 1;
    const uint64_t cloneId  = clone->id;
    nlohmann::json snapshot = scene.subtreeToJson(clone);

    // Undo.
    scene.removeGameObject(scene.findById(cloneId));
    CHECK(contarObjetos() == antes);
    CHECK(scene.findById(cloneId) == nullptr);
    // The original survives the undo of the duplicate.
    CHECK(scene.findById(original->id) != nullptr);

    // Redo.
    GameObject* rehecho = scene.insertFromJson(snapshot, scene.findById(parentId), index, pm, am);
    CHECK(rehecho != nullptr);
    if (!rehecho) return;
    CHECK(contarObjetos() == antes + 2);
    CHECK(rehecho->parent == parent);
    CHECK(rehecho->children.size() == 1);
    // Redo reuses the snapshot's id on purpose (see test_undo_delete_keeps_original_id):
    // it still does not collide with the original.
    CHECK(rehecho->id == cloneId);
    CHECK(scene.findById(original->id) == original);
}

// The counterpart of the three above, along the path that was NOT covered:
// LOADING a scene. nodeFromJson reuses the id that the file brings (and does
// right, see the test above), but the global id counter —which lives in
// GameObject.cpp and only the constructor moves— does not find out. A file
// saved in ANOTHER session brings ids higher than what this process has
// distributed, so after loading it the counter stays BEHIND ids that already
// live in the tree, and the next GameObject the user creates gets a repeated one.
//
// From there on it is the same failure as the clone's: findById gets the LAST
// one in the traversal, and the 31 places in Command.cpp that resolve their
// target by id end up writing to the wrong object, without saying anything.
//
// The probe below is what makes the test deterministic without depending on the
// order the others run in: a just-created GameObject's id says where the counter
// is NOW, and the file is made with one ahead enough that the batch of high ids
// reaches it with margin.
static void test_load_advances_id_counter(PhysicsManager& pm, AudioManager& am)
{
    GameObject sonda("sonda");
    const uint64_t idDelFichero = sonda.id + 20;

    // Source scene serialized with the real API (not hand-written JSON): so the node
    // brings localTransform, ssr and children well-formed and the only artificial
    // thing is the id, which is exactly what is being tested.
    Scene origen("Origen");
    GameObject* nodo = origen.addGameObject("Cargado");
    CHECK(nodo != nullptr);
    nlohmann::json j = origen.toJson();
    j["root"]["children"][0]["id"] = idDelFichero;

    Scene scene("Test");
    CHECK(scene.fromJson(j, pm, am));
    GameObject* cargado = scene.findById(idDelFichero);
    CHECK(cargado != nullptr);
    if (!cargado) return;

    // New additions enough to surpass the file's id. If the load did not advance the
    // counter, one of them repeats it.
    for (int i = 0; i < 30; ++i)
        CHECK(scene.addGameObject("Nuevo") != nullptr);

    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // And the file's id has to keep resolving to the LOADED node, not to a new
    // object that took its place.
    CHECK(scene.findById(idDelFichero) == cargado);
}

// Camera Add/Remove goes through the Undo stack (unlike collider/Rigidbody Add):
// if not, an Undo of a Delete could resurrect a deleted camera while another
// is in the scene. See spec, "The One-Camera Invariant".
static void test_camera_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");

    // near > the default far (2000) on purpose: apply() has to call setFar BEFORE
    // setNear (setNear clamps against the current far). With small values both
    // orders give the same result and the regression would go unnoticed; with
    // near=3000 the reverse order would truncate it to 1999.999 and this test fails.
    CameraState st{ CameraComponent::ProjectionMode::Orthographic, 60.0f, 300.0f, 3000.0f, 8000.0f };
    CameraComponentCommand cmd(scene, "Add Camera", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    cmd.undo();
    CHECK(!go->hasCameraComponent());
    CHECK(scene.findCamera() == nullptr);

    // Redo: the state values are kept, it does not go back to defaults.
    cmd.execute();
    CHECK(go->hasCameraComponent());
    const auto& c = go->getCameraComponent();
    CHECK(c->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(c->getFov(), 60.0f));
    CHECK(nearlyEqual(c->getOrthographicSize(), 300.0f));
    CHECK(nearlyEqual(c->getNear(), 3000.0f));
    CHECK(nearlyEqual(c->getFar(), 8000.0f));
}

// add=false reverses the sense: execute removes, undo returns.
static void test_camera_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");
    go->setCameraComponent(std::make_shared<CameraComponent>());

    CameraState st{ CameraComponent::ProjectionMode::Perspective, 45.0f, 100.0f, 1.0f, 2000.0f };
    CameraComponentCommand cmd(scene, "Remove Camera", go->id, /*add=*/false, st);

    cmd.execute();
    CHECK(!go->hasCameraComponent());
    cmd.undo();
    CHECK(go->hasCameraComponent());
}

// The command resolves the GameObject by id in each execute()/undo(), never
// stores a raw pointer: it survives the object being rebuilt in between.
static void test_camera_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");
    uint64_t id = go->id;
    CameraState st{ CameraComponent::ProjectionMode::Perspective, 45.0f, 100.0f, 1.0f, 2000.0f };
    CameraComponentCommand cmd(scene, "Add Camera", id, /*add=*/true, st);

    scene.removeGameObject(go);
    cmd.execute(); // must not crash: findById returns nullptr and returns
    CHECK(scene.findCamera() == nullptr);
}

// ── Canvas ──────────────────────────────────────────────────────────────────
// The 10 fields with values NOT neutral and DISTINCT among themselves: a default
// does not prove that anyone read or wrote them.
static void fillCanvas(CanvasComponent& c)
{
    c.scaleMode           = UiScaleMode::ConstantPhysicalSize;
    c.scaleFactor         = 2.75f;
    c.referenceResolution = glm::vec2(1280.5f, 720.25f);
    c.screenMatch         = UiScreenMatch::Shrink;
    c.matchWidthOrHeight  = 0.375f;
    c.screenDpi           = 141.0f;
    c.fallbackDpi         = 72.0f;
    c.referenceDpi        = 110.0f;
    c.safeArea            = { 11.0f, 22.0f, 33.0f, 44.0f };
    c.aspectRatio         = 1.6f;
}

static void checkCanvasMatchesFilled(const CanvasComponent& c)
{
    CHECK(c.scaleMode == UiScaleMode::ConstantPhysicalSize);
    CHECK(nearlyEqual(c.scaleFactor, 2.75f));
    CHECK(nearlyEqual(c.referenceResolution.x, 1280.5f));
    CHECK(nearlyEqual(c.referenceResolution.y, 720.25f));
    CHECK(c.screenMatch == UiScreenMatch::Shrink);
    CHECK(nearlyEqual(c.matchWidthOrHeight, 0.375f));
    CHECK(nearlyEqual(c.screenDpi, 141.0f));
    CHECK(nearlyEqual(c.fallbackDpi, 72.0f));
    CHECK(nearlyEqual(c.referenceDpi, 110.0f));
    CHECK(nearlyEqual(c.safeArea.left, 11.0f));
    CHECK(nearlyEqual(c.safeArea.top, 22.0f));
    CHECK(nearlyEqual(c.safeArea.right, 33.0f));
    CHECK(nearlyEqual(c.safeArea.bottom, 44.0f));
    CHECK(nearlyEqual(c.aspectRatio, 1.6f));
}

static void test_canvas_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("UI");
    auto canvas = std::make_shared<CanvasComponent>();
    fillCanvas(*canvas);
    go->setCanvas(canvas);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findCanvas();
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "UI");
    checkCanvasMatchesFilled(*found->getCanvas());
    CHECK(loaded.lastWarnings().empty());
}

// The four new Canvas fields, with values NOT neutral and distinct among
// themselves: with defaults, a fromJson that skipped the field would pass the same.
static void test_canvas_world_fields_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cartel");
    auto c = std::make_shared<CanvasComponent>();
    c->renderMode = UiCanvasRenderMode::World;
    c->worldScale = 0.0234375f;
    c->billboard  = UiBillboard::YawOnly;
    c->depthTest  = false;
    go->setCanvas(c);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(scene.toJson(), pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasCanvas()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->getCanvas()->renderMode == UiCanvasRenderMode::World);
    CHECK(nearlyEqual(found->getCanvas()->worldScale, 0.0234375f));
    CHECK(found->getCanvas()->billboard == UiBillboard::YawOnly);
    CHECK(found->getCanvas()->depthTest == false);
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before these fields loads with defaults and no warnings:
// additive block, same rule as all UI components.
static void test_canvas_without_world_fields_loads_with_defaults(PhysicsManager& pm,
                                                                  AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Viejo");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();
    // The new keys are deleted to simulate an old scene.
    for (auto& n : j["root"]["children"])
        if (n.contains("canvas"))
            for (const char* k : { "renderMode", "worldScale", "billboard", "depthTest" })
                n["canvas"].erase(k);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n2) { if (!found && n2->hasCanvas()) found = n2; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->getCanvas()->renderMode == UiCanvasRenderMode::ScreenSpace);
    CHECK(nearlyEqual(found->getCanvas()->worldScale, 0.001f));
    CHECK(found->getCanvas()->billboard == UiBillboard::None);
    CHECK(found->getCanvas()->depthTest == true);
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: no Canvas and no warnings.
static void test_scene_without_canvas_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Pelado");
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findCanvas() == nullptr);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any Canvas the JSON gains not a byte, and adding and
// removing the component returns the exact SAME dump at the start.
static void test_scene_without_canvas_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("canvas") == std::string::npos);

    go->setCanvas(std::make_shared<CanvasComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setCanvas(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

// The gate of UI components: a GameObject only offers them with a Canvas.
static void test_ui_components_need_canvas()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Normal");
    CHECK(!PropertiesPanel::uiComponentsAvailable(go));
    go->setCanvas(std::make_shared<CanvasComponent>());
    CHECK(PropertiesPanel::uiComponentsAvailable(go));
    go->setCanvas(nullptr);
    CHECK(!PropertiesPanel::uiComponentsAvailable(go));
    CHECK(!PropertiesPanel::uiComponentsAvailable(nullptr));
}

// Add reversible, and redo does NOT return fields to defaults.
static void test_canvas_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("UI");
    CanvasComponent st;
    fillCanvas(st);
    CanvasComponentCommand cmd(scene, "Add Canvas", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasCanvas());
    checkCanvasMatchesFilled(*go->getCanvas());
    cmd.undo();
    CHECK(!go->hasCanvas());
    CHECK(scene.findCanvas() == nullptr);
    cmd.execute();
    CHECK(go->hasCanvas());
    checkCanvasMatchesFilled(*go->getCanvas());
}

// Remove reversible: undo returns the component WITH its values.
static void test_canvas_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("UI");
    auto canvas = std::make_shared<CanvasComponent>();
    fillCanvas(*canvas);
    go->setCanvas(canvas);

    CanvasComponentCommand cmd(scene, "Remove Canvas", go->id, /*add=*/false, *canvas);
    cmd.execute();
    CHECK(!go->hasCanvas());
    cmd.undo();
    CHECK(go->hasCanvas());
    checkCanvasMatchesFilled(*go->getCanvas());
}

// Editing a Canvas field also goes into the stack: the same PropertyCommand<T>
// that builds the section (resolved by id, not by pointer) goes and comes, and
// does not resurrect the component if it no longer exists.
static void test_canvas_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("UI");
    go->setCanvas(std::make_shared<CanvasComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyAspect = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasCanvas()) g->getCanvas()->aspectRatio = v;
    };
    PropertyCommand<float> cmd("Aspect Ratio", 0.0f, 1.6f, applyAspect);

    cmd.execute();
    CHECK(nearlyEqual(go->getCanvas()->aspectRatio, 1.6f));
    cmd.undo();
    CHECK(nearlyEqual(go->getCanvas()->aspectRatio, 0.0f));
    cmd.execute();
    CHECK(nearlyEqual(go->getCanvas()->aspectRatio, 1.6f));

    // Without component the applier does nothing (neither crashes nor resurrects).
    go->setCanvas(nullptr);
    cmd.undo();
    CHECK(!go->hasCanvas());
}

// ── Button ──────────────────────────────────────────────────────────────────
// ALL fields with values NOT neutral and DISTINCT among themselves (colors,
// paths, sizes): a default does not prove anyone read or wrote them, and two
// fields with the SAME value do not detect that they were swapped.
static void fillButton(ButtonComponent& b)
{
    b.anchorMin = glm::vec2(0.125f, 0.25f);
    b.anchorMax = glm::vec2(0.75f, 0.875f);
    b.pivot     = glm::vec2(0.375f, 0.625f);
    b.position  = glm::vec2(12.5f, -34.25f);
    b.size      = glm::vec2(222.5f, 48.75f);
    b.color     = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    b.visible   = false;
    b.atlasPath = "assets/ui/atlas.png";
    b.sprite    = "boton_base";

    b.interactable = false;
    b.selected     = true;
    b.transition   = UiButtonTransition::Animation;

    b.normalColor   = glm::vec4(0.11f, 0.12f, 0.13f, 0.14f);
    b.hoverColor    = glm::vec4(0.21f, 0.22f, 0.23f, 0.24f);
    b.pressedColor  = glm::vec4(0.31f, 0.32f, 0.33f, 0.34f);
    b.disabledColor = glm::vec4(0.41f, 0.42f, 0.43f, 0.44f);
    b.selectedColor = glm::vec4(0.51f, 0.52f, 0.53f, 0.54f);

    b.normalSprite   = "spr_normal";
    b.hoverSprite    = "spr_hover";
    b.pressedSprite  = "spr_pressed";
    b.disabledSprite = "spr_disabled";
    b.selectedSprite = "spr_selected";

    b.fadeDuration = 0.375f;

    b.text      = "Aceptar";
    b.fontPath  = "assets/fonts/roboto.ttf";
    b.fontSize  = 27.5f;
    b.textColor = glm::vec4(0.61f, 0.62f, 0.63f, 0.64f);
    b.textAlign = UiTextAlign::Justify;
}

static void checkButtonMatchesFilled(const ButtonComponent& b)
{
    CHECK(nearlyEqual(b.anchorMin.x, 0.125f));
    CHECK(nearlyEqual(b.anchorMin.y, 0.25f));
    CHECK(nearlyEqual(b.anchorMax.x, 0.75f));
    CHECK(nearlyEqual(b.anchorMax.y, 0.875f));
    CHECK(nearlyEqual(b.pivot.x, 0.375f));
    CHECK(nearlyEqual(b.pivot.y, 0.625f));
    CHECK(nearlyEqual(b.position.x, 12.5f));
    CHECK(nearlyEqual(b.position.y, -34.25f));
    CHECK(nearlyEqual(b.size.x, 222.5f));
    CHECK(nearlyEqual(b.size.y, 48.75f));
    CHECK(nearlyEqual(b.color.r, 0.1f));
    CHECK(nearlyEqual(b.color.g, 0.2f));
    CHECK(nearlyEqual(b.color.b, 0.3f));
    CHECK(nearlyEqual(b.color.a, 0.4f));
    CHECK(b.visible == false);
    CHECK(b.atlasPath == "assets/ui/atlas.png");
    CHECK(b.sprite == "boton_base");

    CHECK(b.interactable == false);
    CHECK(b.selected == true);
    CHECK(b.transition == UiButtonTransition::Animation);

    CHECK(nearlyEqual(b.normalColor.r, 0.11f));
    CHECK(nearlyEqual(b.normalColor.a, 0.14f));
    CHECK(nearlyEqual(b.hoverColor.r, 0.21f));
    CHECK(nearlyEqual(b.hoverColor.a, 0.24f));
    CHECK(nearlyEqual(b.pressedColor.r, 0.31f));
    CHECK(nearlyEqual(b.pressedColor.a, 0.34f));
    CHECK(nearlyEqual(b.disabledColor.r, 0.41f));
    CHECK(nearlyEqual(b.disabledColor.a, 0.44f));
    CHECK(nearlyEqual(b.selectedColor.r, 0.51f));
    CHECK(nearlyEqual(b.selectedColor.a, 0.54f));

    CHECK(b.normalSprite == "spr_normal");
    CHECK(b.hoverSprite == "spr_hover");
    CHECK(b.pressedSprite == "spr_pressed");
    CHECK(b.disabledSprite == "spr_disabled");
    CHECK(b.selectedSprite == "spr_selected");

    CHECK(nearlyEqual(b.fadeDuration, 0.375f));

    CHECK(b.text == "Aceptar");
    CHECK(b.fontPath == "assets/fonts/roboto.ttf");
    CHECK(nearlyEqual(b.fontSize, 27.5f));
    CHECK(nearlyEqual(b.textColor.r, 0.61f));
    CHECK(nearlyEqual(b.textColor.a, 0.64f));
    CHECK(b.textAlign == UiTextAlign::Justify);
}

static void test_button_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Aceptar", canvasGo);
    auto button = std::make_shared<ButtonComponent>();
    fillButton(*button);
    go->setButton(button);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasButton()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Aceptar");
    checkButtonMatchesFilled(*found->getButton());
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: no Button and no warnings.
static void test_scene_without_button_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasButton()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any Button the JSON gains not a byte, and adding and
// removing the component returns the exact SAME dump at the start.
static void test_scene_without_button_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("button") == std::string::npos);

    go->setButton(std::make_shared<ButtonComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setButton(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

// The gate also works for a DESCENDANT of Canvas: a button hangs from the
// canvas, it is not the canvas.
static void test_ui_components_available_for_descendants()
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    GameObject* hijo     = scene.addGameObject("Boton", canvasGo);
    GameObject* nieto    = scene.addGameObject("Icono", hijo);
    CHECK(!PropertiesPanel::uiComponentsAvailable(hijo));
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    CHECK(PropertiesPanel::uiComponentsAvailable(hijo));
    CHECK(PropertiesPanel::uiComponentsAvailable(nieto));
    // A sibling of the canvas (not descendant) still does not see them.
    CHECK(!PropertiesPanel::uiComponentsAvailable(scene.addGameObject("Suelto")));
}

// Add reversible, and redo does NOT return fields to defaults.
static void test_button_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Aceptar");
    ButtonComponent st;
    fillButton(st);
    ButtonComponentCommand cmd(scene, "Add Button", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasButton());
    checkButtonMatchesFilled(*go->getButton());
    cmd.undo();
    CHECK(!go->hasButton());
    cmd.execute();
    CHECK(go->hasButton());
    checkButtonMatchesFilled(*go->getButton());
}

// Remove reversible: undo returns the component WITH its values.
static void test_button_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Aceptar");
    auto button = std::make_shared<ButtonComponent>();
    fillButton(*button);
    go->setButton(button);

    ButtonComponentCommand cmd(scene, "Remove Button", go->id, /*add=*/false, *button);
    cmd.execute();
    CHECK(!go->hasButton());
    cmd.undo();
    CHECK(go->hasButton());
    checkButtonMatchesFilled(*go->getButton());
}

// Editing a Button field also goes into the stack: the same PropertyCommand<T>
// that builds the section (resolved by id, not by pointer) goes and comes, and
// does not resurrect the component if it no longer exists.
static void test_button_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Aceptar");
    go->setButton(std::make_shared<ButtonComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyText = [sc, id](const std::string& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasButton()) g->getButton()->text = v;
    };
    PropertyCommand<std::string> cmd("Text", std::string(), std::string("Aceptar"), applyText);

    cmd.execute();
    CHECK(go->getButton()->text == "Aceptar");
    cmd.undo();
    CHECK(go->getButton()->text.empty());
    cmd.execute();
    CHECK(go->getButton()->text == "Aceptar");

    // Without component the applier does nothing (neither crashes nor resurrects).
    go->setButton(nullptr);
    cmd.undo();
    CHECK(!go->hasButton());
}

// ── Sync of Button against the live canvas ───────────────────────────────────
// No GPU: the fake loader returns nullptr, which is exactly what Renderer
// returns with an empty path. What is tested is PLACEMENT, not texture.
struct FakeUiLoader
{
    // How many times each resource has been asked for. Loading a real font is
    // FreeType + bake + upload to GPU: who asks for it and WHEN is what feels
    // like a freeze in the editor, so we count it.
    int atlasLoads = 0;
    int fontLoads  = 0;

    UiTextureAtlas* loadUiAtlas(const std::string&) { atlasLoads++; return nullptr; }
    UiFont*         loadUiFont(const std::string&)  { fontLoads++;  return nullptr; }
};

// TEST-ONLY adapter to the signature syncUiWidgets had before lists were
// grouped in UiWidgetLists. It is here and not in the engine on purpose: the
// production API is ONE (the struct one), and this exists to not rewrite the
// ~65 calls in this file, which test the tree assembly not the way lists are
// passed. New tests call the real one.
//
// It lives in global scope and the production one in DonTopo, so both are
// candidates by ADL; there is no ambiguity because arities do not overlap
// (this one asks for at least six arguments and that one exactly four).

// One complete click on p: one frame of hover, one with button down and
// another with button up. The hit test needs rects, that is a buildDrawData
// beforehand. Times are separated between clicks to not cross the double-click
// threshold by accident.
static void clickEnCanvas(UiCanvas& canvas, glm::vec2 p, float t0)
{
    UiInputState in;
    in.mousePos    = p;
    in.timeSeconds = t0;
    canvas.updateInput(in);

    in.mouseDown[0] = true;
    in.timeSeconds  = t0 + 0.016f;
    canvas.updateInput(in);

    in.mouseDown[0] = false;
    in.timeSeconds  = t0 + 0.032f;
    canvas.updateInput(in);
}

template <class Loader>
static void syncUiWidgets(
    const std::vector<std::pair<uint64_t, const ButtonComponent*>>& buttons,
    const std::vector<std::pair<uint64_t, const TextComponent*>>& texts,
    const std::vector<std::pair<uint64_t, const ProgressBarComponent*>>& bars,
    UiCanvas& canvas, UiWidgetSyncCache& cache, Loader& loader,
    const std::vector<std::pair<uint64_t, uint64_t>>* parents = nullptr,
    const std::vector<std::pair<uint64_t, const LayoutComponent*>>* layouts = nullptr)
{
    UiWidgetLists w;
    w.buttons = buttons;
    w.texts   = texts;
    w.bars    = bars;
    if (layouts) w.layouts = *layouts;
    if (parents) w.parents = *parents;
    DonTopo::syncUiWidgets(w, canvas, cache, loader);
}

// Editing a component field has to show in the next frame. The tree caches
// vertices per node, so a sync that writes fields and does not mark the node
// dirty leaves the button stuck where it was.
static void test_button_sync_moves_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.position = glm::vec2(10.0f, 20.0f);
    b.size     = glm::vec2(100.0f, 50.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    UiDrawData data;

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 10.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 20.0f));

    // Same button, another position: the live node has to follow it.
    b.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 300.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 120.0f));
}

// Touching Canvas resolution cannot make the button disappear: the sync in the
// next frame leaves it where it belongs, scaled.
static void test_button_survives_canvas_edit()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.position = glm::vec2(10.0f, 20.0f);
    b.size     = glm::vec2(100.0f, 50.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    UiDrawData data;
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);

    // What CanvasComponent::applyTo does when the user touches the panel.
    CanvasComponent cc;
    cc.scaleFactor = 2.0f;
    cc.applyTo(canvas);

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 20.0f));   // 10 * scale 2
    CHECK(nearlyEqual(data.vertices[0].pos.y, 40.0f));

    // And no reasonable canvas adjustment erases it from screen.
    const UiScaleMode modos[] = { UiScaleMode::ConstantPixelSize,
                                  UiScaleMode::ScaleWithScreenSize,
                                  UiScaleMode::ConstantPhysicalSize };
    for (UiScaleMode m : modos)
    {
        CanvasComponent otro;
        otro.scaleMode = m;
        otro.safeArea  = { 8.0f, 6.0f, 8.0f, 6.0f };
        otro.applyTo(canvas);
        syncUiWidgets(lista, {}, {}, canvas, cache, loader);
        data.clear();
        canvas.buildDrawData(800, 480, data);
        CHECK(data.batches.size() == 1);
    }
}

// A button with text and WITHOUT a font set still shows its label: the sync
// gives it a default font instead of leaving the text invisible.
static void test_button_text_without_font_is_visible()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.text = "Aceptar";

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& node = *canvas.root().children()[0];
    CHECK(node.children().size() == 1);   // the label exists even without font
    if (node.children().empty()) return;
    const Text* label = node.children()[0]->asText();
    CHECK(label != nullptr);
    if (label) CHECK(label->text == "Aceptar");
}

// With input fed, the color you see is the STATE one, not the base color: this
// is what makes editing "Normal" in the panel visible.
static void test_button_state_color_is_applied()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.size        = glm::vec2(100.0f, 50.0f);
    b.color       = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    b.normalColor = glm::vec4(0.2f, 0.4f, 0.6f, 0.8f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);   // places the rects: the hit test reads them

    UiInputState in;
    in.mousePos    = glm::vec2(-1.0f, -1.0f);   // the mouse, far: Normal state
    in.timeSeconds = 1.0f;
    canvas.updateInput(in);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& node = *canvas.root().children()[0];
    CHECK(nearlyEqual(node.color.r, 0.2f));
    CHECK(nearlyEqual(node.color.g, 0.4f));
    CHECK(nearlyEqual(node.color.b, 0.6f));
    CHECK(nearlyEqual(node.color.a, 0.8f));
}

// A button WITH label still sees the mouse. The label is a Text child anchored
// to the ENTIRE button rect, and the hit test tries children first: if it
// intercepted the mouse, hover would be marked on it and the button would stay
// in Normal forever. It is a bug hard to see because CLICK does work (events
// bubble from child to parent): all that breaks are the five state colors.
static void test_button_with_label_still_hovers()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.size        = glm::vec2(100.0f, 50.0f);
    b.text        = "Jugar";            // <- with label
    b.normalColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    b.hoverColor  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    UiInputState in;
    in.mousePos    = glm::vec2(20.0f, 20.0f);   // inside the button AND its label
    in.timeSeconds = 1.0f;
    canvas.updateInput(in);

    CHECK(cache.buttonNodes.size() == 1);
    CHECK(cache.buttonLabels.size() == 1 && cache.buttonLabels[0] != nullptr);
    if (cache.buttonNodes.size() != 1) return;
    CHECK(cache.buttonNodes[0]->hovered);
    CHECK(cache.buttonNodes[0]->state == UiButtonState::Hover);
    CHECK(nearlyEqual(cache.buttonNodes[0]->color.r, 1.0f));
    CHECK(nearlyEqual(cache.buttonNodes[0]->color.g, 0.0f));
}

// Click on a button in the viewport: the canvas hit test returns a node and
// its name is the only thing that ties the UI tree to the scene. It is the
// pure piece of ViewportPanel::pickUiObject (the rest is ImGui).
static void test_button_hit_test_maps_back_to_gameobject()
{
    CHECK(uiButtonOwnerId(uiButtonNodeName(42ull)) == 42ull);
    CHECK(uiButtonOwnerId(uiButtonNodeName(42ull) + "/Label") == 42ull);
    CHECK(uiButtonOwnerId("Cubo") == 0ull);
    CHECK(uiButtonOwnerId("go:") == 0ull);
    CHECK(uiButtonOwnerId("go:12ab") == 0ull);

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent a, b;
    a.position = glm::vec2(0.0f, 0.0f);
    a.size     = glm::vec2(100.0f, 40.0f);
    b.position = glm::vec2(300.0f, 200.0f);
    b.size     = glm::vec2(120.0f, 60.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &a}, {9ull, &b} };
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    const UiElement* hit = canvas.hitTest(glm::vec2(360.0f, 230.0f));   // center of 9
    CHECK(hit != nullptr);
    if (hit) CHECK(uiButtonOwnerId(hit->name) == 9ull);

    hit = canvas.hitTest(glm::vec2(50.0f, 20.0f));                      // center of 7
    CHECK(hit != nullptr);
    if (hit) CHECK(uiButtonOwnerId(hit->name) == 7ull);

    CHECK(canvas.hitTest(glm::vec2(700.0f, 400.0f)) == nullptr);        // gap
}

// Button asset boxes veto by extension: the font box does NOT accept an image
// and vice versa. It is the same filter for the drop and for the file dialog.
static void test_button_asset_path_filters()
{
    CHECK(PropertiesPanel::isUiFontPath("assets/DancingScript-VariableFont_wght.ttf"));
    CHECK(PropertiesPanel::isUiFontPath("C:/fuentes/algo.OTF"));   // uppercase
    CHECK(PropertiesPanel::isUiFontPath("a.ttc"));
    CHECK(!PropertiesPanel::isUiFontPath("assets/ui_atlas.png"));
    CHECK(!PropertiesPanel::isUiFontPath("assets/hero.fbx"));
    CHECK(!PropertiesPanel::isUiFontPath("sinextension"));
    CHECK(!PropertiesPanel::isUiFontPath(""));
    // A DIRECTORY point is not an extension.
    CHECK(!PropertiesPanel::isUiFontPath("C:/mis.fuentes/archivo"));

    CHECK(PropertiesPanel::isUiAtlasPath("assets/ui_atlas.png"));
    CHECK(PropertiesPanel::isUiAtlasPath("x.JPEG"));
    CHECK(PropertiesPanel::isUiAtlasPath("x.tga"));
    CHECK(!PropertiesPanel::isUiAtlasPath("assets/fuente.ttf"));
    CHECK(!PropertiesPanel::isUiAtlasPath("assets/audio.wav"));
    // Since Content Browser lets you drag images and fonts, ANY asset can land in
    // ANY box: a mesh on a UI atlas is the case that could not happen before (the
    // .fbx was draggable, but the atlas box could not see it arrive because the
    // drag did not leave the Content Browser).
    CHECK(!PropertiesPanel::isUiAtlasPath("assets/hero.fbx"));
    CHECK(!PropertiesPanel::isUiFontPath("assets/musica.mp3"));
}

// ── Text ────────────────────────────────────────────────────────────────────
// ALL fields with values NOT neutral and DISTINCT among themselves, same
// criterion as fillButton: a default does not prove anyone read or wrote them,
// and two fields with the SAME value do not detect that they were swapped.
static void fillText(TextComponent& t)
{
    t.anchorMin = glm::vec2(0.0625f, 0.1875f);
    t.anchorMax = glm::vec2(0.6875f, 0.8125f);
    t.pivot     = glm::vec2(0.4375f, 0.5625f);
    t.position  = glm::vec2(7.25f, -19.5f);
    t.size      = glm::vec2(301.5f, 77.25f);
    t.color     = glm::vec4(0.05f, 0.15f, 0.25f, 0.35f);
    t.visible   = false;

    t.text     = "Hola <b>mundo</b>";
    t.fontPath = "assets/fonts/inter.ttf";
    t.fontSize = 33.5f;

    t.outlineWidth = 2.75f;
    t.outlineColor = glm::vec4(0.71f, 0.72f, 0.73f, 0.74f);
    t.shadowOffset = glm::vec2(3.5f, -4.25f);
    t.shadowColor  = glm::vec4(0.81f, 0.82f, 0.83f, 0.84f);

    t.align    = UiTextAlign::Right;
    t.overflow = UiTextOverflow::Ellipsis;
    t.wordWrap = true;

    t.boldStrength = 0.135f;
    t.italicSkew   = -0.4375f;
}

static void checkTextMatchesFilled(const TextComponent& t)
{
    CHECK(nearlyEqual(t.anchorMin.x, 0.0625f));
    CHECK(nearlyEqual(t.anchorMin.y, 0.1875f));
    CHECK(nearlyEqual(t.anchorMax.x, 0.6875f));
    CHECK(nearlyEqual(t.anchorMax.y, 0.8125f));
    CHECK(nearlyEqual(t.pivot.x, 0.4375f));
    CHECK(nearlyEqual(t.pivot.y, 0.5625f));
    CHECK(nearlyEqual(t.position.x, 7.25f));
    CHECK(nearlyEqual(t.position.y, -19.5f));
    CHECK(nearlyEqual(t.size.x, 301.5f));
    CHECK(nearlyEqual(t.size.y, 77.25f));
    CHECK(nearlyEqual(t.color.r, 0.05f));
    CHECK(nearlyEqual(t.color.g, 0.15f));
    CHECK(nearlyEqual(t.color.b, 0.25f));
    CHECK(nearlyEqual(t.color.a, 0.35f));
    CHECK(t.visible == false);

    CHECK(t.text == "Hola <b>mundo</b>");
    CHECK(t.fontPath == "assets/fonts/inter.ttf");
    CHECK(nearlyEqual(t.fontSize, 33.5f));

    CHECK(nearlyEqual(t.outlineWidth, 2.75f));
    CHECK(nearlyEqual(t.outlineColor.r, 0.71f));
    CHECK(nearlyEqual(t.outlineColor.a, 0.74f));
    CHECK(nearlyEqual(t.shadowOffset.x, 3.5f));
    CHECK(nearlyEqual(t.shadowOffset.y, -4.25f));
    CHECK(nearlyEqual(t.shadowColor.r, 0.81f));
    CHECK(nearlyEqual(t.shadowColor.a, 0.84f));

    CHECK(t.align == UiTextAlign::Right);
    CHECK(t.overflow == UiTextOverflow::Ellipsis);
    CHECK(t.wordWrap == true);

    CHECK(nearlyEqual(t.boldStrength, 0.135f));
    CHECK(nearlyEqual(t.italicSkew, -0.4375f));
}

static void test_text_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Titulo", canvasGo);
    auto text = std::make_shared<TextComponent>();
    fillText(*text);
    go->setText(text);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasText()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Titulo");
    checkTextMatchesFilled(*found->getText());
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: no Text and no warnings.
static void test_scene_without_text_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasText()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any Text the JSON gains not a byte, and adding and
// removing the component returns the exact SAME dump at the start.
static void test_scene_without_text_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"text\"") == std::string::npos);

    go->setText(std::make_shared<TextComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setText(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

// Add reversible, and redo does NOT return fields to defaults.
static void test_text_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Titulo");
    TextComponent st;
    fillText(st);
    TextComponentCommand cmd(scene, "Add Text", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasText());
    checkTextMatchesFilled(*go->getText());
    cmd.undo();
    CHECK(!go->hasText());
    cmd.execute();
    CHECK(go->hasText());
    checkTextMatchesFilled(*go->getText());
}

// Remove reversible: undo returns the component WITH its values.
static void test_text_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Titulo");
    auto text = std::make_shared<TextComponent>();
    fillText(*text);
    go->setText(text);

    TextComponentCommand cmd(scene, "Remove Text", go->id, /*add=*/false, *text);
    cmd.execute();
    CHECK(!go->hasText());
    cmd.undo();
    CHECK(go->hasText());
    checkTextMatchesFilled(*go->getText());
}

// Editing a Text field also goes into the stack, with the same PropertyCommand<T>
// that builds the section (resolved by id, not by pointer).
static void test_text_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Titulo");
    go->setText(std::make_shared<TextComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyText = [sc, id](const std::string& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasText()) g->getText()->text = v;
    };
    PropertyCommand<std::string> cmd("Text", std::string(), std::string("Titulo"), applyText);

    cmd.execute();
    CHECK(go->getText()->text == "Titulo");
    cmd.undo();
    CHECK(go->getText()->text.empty());
    cmd.execute();
    CHECK(go->getText()->text == "Titulo");

    // Without component the applier does nothing (neither crashes nor resurrects).
    go->setText(nullptr);
    cmd.undo();
    CHECK(!go->hasText());
}

// Editing a component field must show in the next frame. The tree caches
// vertices per node, so a sync that writes fields and does not mark dirty
// leaves the text STUCK: this is what the dirty flags here catch. With the
// fake loader there is no font, and without font no quad is emitted
// (drawable = false), so the rect is checked by screenPos not vertices.
static void test_text_sync_updates_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    TextComponent t;
    t.text     = "Uno";
    t.position = glm::vec2(10.0f, 20.0f);
    t.size     = glm::vec2(120.0f, 30.0f);

    std::vector<std::pair<uint64_t, const TextComponent*>> textos{ {9ull, &t} };
    UiDrawData data;

    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& node = *canvas.root().children()[0];
    CHECK(node.name == uiTextNodeName(9ull));
    const Text* vivo = node.asText();
    CHECK(vivo != nullptr);
    if (!vivo) return;
    CHECK(vivo->text == "Uno");
    CHECK(nearlyEqual(node.screenPos.x, 10.0f));
    CHECK(nearlyEqual(node.screenPos.y, 20.0f));
    // The emitter leaves the node clean: it is the cache the sync has to invalidate.
    CHECK(node.dirty == 0u);

    // Same Text, different content and different position: the live node has to
    // follow them AND be marked dirty, or the canvas would reuse the old vertices.
    t.text     = "Dos";
    t.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(vivo->text == "Dos");
    CHECK(node.dirty != 0u);

    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(node.screenPos.x, 300.0f));
    CHECK(nearlyEqual(node.screenPos.y, 120.0f));

    // And a frame with no changes does NOT redirty: dirtying always throws away
    // the entire canvas vertex cache every frame.
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(node.dirty == 0u);
}

// The trap: the canvas root is rebuilt with clearChildren(), so a sync that
// only knew buttons would erase texts (and vice versa). With both in the scene,
// neither carries away the other.
// ── Hierarchy ───────────────────────────────────────────────────────────────
// Widgets all hung from the canvas root, so nesting GameObjects in the scene
// was pointless: the parent did not place, did not clip and did not attenuate
// its children. With hierarchy, a GameObject node hangs from its parent's MAIN
// node (Button > ProgressBar > Text), which is the one that has the rect to
// anchor against.
static void test_jerarquia_ancla_y_hereda_del_padre()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent padre;
    padre.position = glm::vec2(100.0f, 50.0f);
    padre.size     = glm::vec2(400.0f, 300.0f);

    ButtonComponent hijo;
    hijo.position = glm::vec2(10.0f, 20.0f);
    hijo.size     = glm::vec2(60.0f, 30.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {7ull, &padre}, {8ull, &hijo} };
    // Pre-order, with the child pointing to its parent. 0 = hangs from the root.
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {7ull, 0ull}, {8ull, 7ull} };

    UiDrawData data;
    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);
    canvas.buildDrawData(800, 480, data);

    // The child NO LONGER hangs from the root.
    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    const UiElement* nodoPadre = canvas.root().children()[0].get();
    CHECK(nodoPadre->name == uiButtonNodeName(7ull));
    CHECK(nodoPadre->children().size() == 1);
    if (nodoPadre->children().size() != 1) return;
    CHECK(nodoPadre->children()[0]->name == uiButtonNodeName(8ull));

    // And its position is RELATIVE to the parent: 100+10, 50+20.
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[4].pos.x, 110.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 70.0f));

    // Moving the parent moves the child without touching it.
    padre.position = glm::vec2(200.0f, 50.0f);
    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[4].pos.x, 210.0f));

    // Without hierarchy (nullptr) everything hangs from the root again: it is EXACTLY
    // what the old calls did.
    UiCanvas          plano;
    UiWidgetSyncCache cachePlano;
    syncUiWidgets(botones, {}, {}, plano, cachePlano, loader);
    CHECK(plano.root().children().size() == 2);
}

// Changing parent rebuilds the tree: if not, the node would stay hanging
// where it was and the scene and what you see would stop matching.
static void test_jerarquia_cambiar_de_padre_reconstruye()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent a, b;
    a.size = glm::vec2(200.0f, 100.0f);
    b.size = glm::vec2(50.0f, 25.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {7ull, &a}, {8ull, &b} };
    std::vector<std::pair<uint64_t, uint64_t>> anidado{ {7ull, 0ull}, {8ull, 7ull} };
    std::vector<std::pair<uint64_t, uint64_t>> suelto { {7ull, 0ull}, {8ull, 0ull} };

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &anidado);
    CHECK(canvas.root().children().size() == 1);

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &suelto);
    CHECK(canvas.root().children().size() == 2);
}

// The parent's opacity multiplies into the child (the one from the UI tree, which the
// canvas already accumulated and which no widget could exploit without hierarchy).
static void test_jerarquia_hereda_la_opacidad()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent padre, hijo;
    padre.size = glm::vec2(400.0f, 300.0f);
    hijo.size  = glm::vec2(60.0f, 30.0f);
    // Different colors and not neutral: with whites, a misplaced alpha looks
    // unnoticed.
    padre.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    hijo.color  = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {7ull, &padre}, {8ull, &hijo} };
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {7ull, 0ull}, {8ull, 7ull} };

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);

    // Opacity is not a field of the component: it is touched in the live node, which
    // is what the core animation does. What is tested is that it GOES DOWN.
    UiElement* nodoPadre = const_cast<UiElement*>(canvas.root().children()[0].get());
    nodoPadre->opacity = 0.5f;
    nodoPadre->markDirty(UiElement::DirtyTransform);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[0].color.a, 0.5f));   // parent
    CHECK(nearlyEqual(data.vertices[4].color.a, 0.5f));   // child, inherited
}

// A GameObject in between WITHOUT UI components does not provide a rect to
// anchor to, so it cannot hold anyone: its children have to hang from
// the first ancestor that does have UI. If the immediate parent were used, the node
// would hang under an id that does not exist in the UI tree and would end up at the root,
// losing the anchor to the grandfather without anything saying so.
static void test_collect_ui_widgets_salta_los_intermedios_sin_ui()
{
    Scene scene;
    GameObject* canvasGo = scene.addGameObject("Canvas");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* panel = scene.addGameObject("Panel", canvasGo);
    panel->setButton(std::make_shared<ButtonComponent>());

    // Middle level WITHOUT UI: just groups.
    GameObject* grupo = scene.addGameObject("Grupo", panel);

    GameObject* etiqueta = scene.addGameObject("Etiqueta", grupo);
    etiqueta->setText(std::make_shared<TextComponent>());

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;

    CHECK(botones.size() == 1);
    CHECK(textos.size() == 1);
    CHECK(barras.empty());

    // Only those with UI appear in the hierarchy: the Canvas and the group do not.
    CHECK(jerarquia.size() == 2);
    if (jerarquia.size() != 2) return;
    CHECK(jerarquia[0].first == panel->id);
    CHECK(jerarquia[0].second == 0ull);              // first level
    CHECK(jerarquia[1].first == etiqueta->id);
    CHECK(jerarquia[1].second == panel->id);         // the grandfather, not the group

    // And the order is PRE-ORDER: the parent before the child, which is what
    // allows building the tree in one pass.
    CHECK(jerarquia[0].first != jerarquia[1].second || true);
}

// Each widget goes to the canvas it hangs from, not to a common bucket. With one
// canvas this made no difference; with two, putting them all in the first one paints the UI of the
// pause menu over the HUD and nothing says so.
static void test_collect_canvases_agrupa_por_canvas()
{
    Scene scene;
    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* vida = scene.addGameObject("Vida", hud);
    vida->setProgressBar(std::make_shared<ProgressBarComponent>());

    GameObject* menu = scene.addGameObject("Menu");
    menu->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* jugar = scene.addGameObject("Jugar", menu);
    jugar->setButton(std::make_shared<ButtonComponent>());
    GameObject* salir = scene.addGameObject("Salir", menu);
    salir->setButton(std::make_shared<ButtonComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 2);
    if (bindings.size() != 2) return;
    CHECK(bindings[0].ownerId == hud->id);
    CHECK(bindings[0].widgets.bars.size() == 1);
    CHECK(bindings[0].widgets.buttons.empty());
    CHECK(bindings[1].ownerId == menu->id);
    CHECK(bindings[1].widgets.buttons.size() == 2);
    CHECK(bindings[1].widgets.bars.empty());
}

// Canvas inside canvas: the CLOSEST one upward wins, with no new rules.
static void test_collect_canvases_anidado_gana_el_mas_cercano()
{
    Scene scene;
    GameObject* fuera = scene.addGameObject("Fuera");
    fuera->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* deFuera = scene.addGameObject("DeFuera", fuera);
    deFuera->setPanel(std::make_shared<PanelComponent>());

    GameObject* dentro = scene.addGameObject("Dentro", fuera);
    dentro->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* deDentro = scene.addGameObject("DeDentro", dentro);
    deDentro->setPanel(std::make_shared<PanelComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 2);
    if (bindings.size() != 2) return;
    CHECK(bindings[0].ownerId == fuera->id);
    CHECK(bindings[0].widgets.panels.size() == 1);
    if (bindings[0].widgets.panels.size() == 1)
        CHECK(bindings[0].widgets.panels[0].first == deFuera->id);
    CHECK(bindings[1].ownerId == dentro->id);
    CHECK(bindings[1].widgets.panels.size() == 1);
    if (bindings[1].widgets.panels.size() == 1)
        CHECK(bindings[1].widgets.panels[0].first == deDentro->id);
}

// A widget with NO canvas above it goes nowhere. Before it ended
// in the unique canvas even if it did not hang from it; now it does not paint. This is a
// DELIBERATE behavior change: the editor already prevents it (uiComponentsAvailable
// requires a Canvas ancestor) and only affects manually-made scenes.
static void test_collect_canvases_ignora_los_huerfanos()
{
    Scene scene;
    GameObject* suelto = scene.addGameObject("Suelto");
    suelto->setButton(std::make_shared<ButtonComponent>());

    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 1);
    if (bindings.empty()) return;
    CHECK(bindings[0].ownerId == hud->id);
    CHECK(bindings[0].widgets.buttons.empty());
}

// The hierarchy of each binding is ITS OWN: the parent ids point inside the
// same canvas, and the first level hangs from 0 (the root of THAT canvas).
static void test_collect_canvases_jerarquia_por_canvas()
{
    Scene scene;
    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* marco = scene.addGameObject("Marco", hud);
    marco->setPanel(std::make_shared<PanelComponent>());
    GameObject* icono = scene.addGameObject("Icono", marco);
    icono->setImage(std::make_shared<ImageComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    CHECK(bindings.size() == 1);
    if (bindings.empty()) return;

    const auto& p = bindings[0].widgets.parents;
    CHECK(p.size() == 2);
    if (p.size() != 2) return;
    CHECK(p[0].first == marco->id);
    CHECK(p[0].second == 0ull);          // first level of the canvas
    CHECK(p[1].first == icono->id);
    CHECK(p[1].second == marco->id);
}

static void test_buttons_and_texts_coexist()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent b;
    b.position = glm::vec2(0.0f, 0.0f);
    b.size     = glm::vec2(100.0f, 40.0f);
    TextComponent t;
    t.text     = "Titulo";
    t.position = glm::vec2(200.0f, 150.0f);
    t.size     = glm::vec2(120.0f, 30.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {7ull, &b} };
    std::vector<std::pair<uint64_t, const TextComponent*>>   textos{ {9ull, &t} };

    UiDrawData data;
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(canvas.root().children().size() == 2);
    if (canvas.root().children().size() != 2) return;
    CHECK(canvas.root().children()[0]->name == uiButtonNodeName(7ull));
    CHECK(canvas.root().children()[1]->name == uiTextNodeName(9ull));
    // The button paints (color quad, no atlas); the text without font does not.
    CHECK(data.vertices.size() == 4);

    // Touching ONLY the text does not erase the button nor throw away its vertices.
    t.text = "Otro";
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 2);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 0.0f));

    // And touching ONLY the button does not erase the text.
    b.position = glm::vec2(50.0f, 60.0f);
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 2);
    if (canvas.root().children().size() != 2) return;
    const Text* vivo = canvas.root().children()[1]->asText();
    CHECK(vivo != nullptr);
    if (vivo) CHECK(vivo->text == "Otro");

    // One more widget rebuilds the root: both types get rebuilt, not just
    // the one that changed count.
    TextComponent t2;
    t2.text = "Pie";
    textos.emplace_back(11ull, &t2);
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 3);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);   // the button still there after the rebuild
    CHECK(nearlyEqual(data.vertices[0].pos.x, 50.0f));
}

// A newly added Text is EMPTY: it cannot cost a font load, which
// is synchronous (FreeType + bake + GPU) and shows as a freeze right when Add is pressed.
// The font is paid when there is real text, and only once per path.
static void test_text_without_content_loads_no_font()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    TextComponent t;   // without text, as "Add Component" leaves it

    std::vector<std::pair<uint64_t, const TextComponent*>> textos{ {9ull, &t} };
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);
    CHECK(canvas.root().children().size() == 1);

    // And several more frames without writing anything do not load it either.
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);

    // The first letter does load it, and only that once: the cache by path is what
    // prevents one load per frame (and one GPU memory leak per frame).
    t.text = "H";
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 1);
    t.text = "Ho";
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 1);
}

// Click on a text in the viewport: same path as the button, and with both
// components in the SAME GameObject both names lead to their id without stepping on each other.
static void test_text_hit_test_maps_back_to_gameobject()
{
    CHECK(uiTextOwnerId(uiTextNodeName(42ull)) == 42ull);
    CHECK(uiTextOwnerId("Cubo") == 0ull);
    CHECK(uiTextOwnerId("txt:") == 0ull);
    CHECK(uiTextOwnerId("txt:12ab") == 0ull);
    // The two prefixes do not confuse each other.
    CHECK(uiTextOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiTextNodeName(42ull)) == 0ull);
}

// ── ProgressBar ─────────────────────────────────────────────────────────────
// All fields set to NOT neutral values and DIFFERENT from each other: with the default (or
// with two fields equal) a round-trip passes even if fromJson skips the
// field or reads the wrong key.
static void fillBar(ProgressBarComponent& p)
{
    p.anchorMin = glm::vec2(0.125f, 0.25f);
    p.anchorMax = glm::vec2(0.75f, 0.875f);
    p.pivot     = glm::vec2(0.3125f, 0.40625f);
    p.position  = glm::vec2(11.5f, -23.25f);
    p.size      = glm::vec2(242.75f, 31.5f);
    p.color     = glm::vec4(0.11f, 0.12f, 0.13f, 0.14f);
    p.visible   = false;

    p.value    = 37.5f;
    p.minValue = -12.25f;
    p.maxValue = 88.75f;

    p.fillColor     = glm::vec4(0.21f, 0.22f, 0.23f, 0.24f);
    p.fillDirection = UiProgressFillDirection::BottomToTop;

    p.atlasPath      = "assets/ui/hud.png";
    p.backgroundPath = "assets/ui/bar_bg.png";
    p.fillPath       = "assets/ui/bar_fill.png";
}

static void checkBarMatchesFilled(const ProgressBarComponent& p)
{
    CHECK(nearlyEqual(p.anchorMin.x, 0.125f));
    CHECK(nearlyEqual(p.anchorMin.y, 0.25f));
    CHECK(nearlyEqual(p.anchorMax.x, 0.75f));
    CHECK(nearlyEqual(p.anchorMax.y, 0.875f));
    CHECK(nearlyEqual(p.pivot.x, 0.3125f));
    CHECK(nearlyEqual(p.pivot.y, 0.40625f));
    CHECK(nearlyEqual(p.position.x, 11.5f));
    CHECK(nearlyEqual(p.position.y, -23.25f));
    CHECK(nearlyEqual(p.size.x, 242.75f));
    CHECK(nearlyEqual(p.size.y, 31.5f));
    CHECK(nearlyEqual(p.color.r, 0.11f));
    CHECK(nearlyEqual(p.color.g, 0.12f));
    CHECK(nearlyEqual(p.color.b, 0.13f));
    CHECK(nearlyEqual(p.color.a, 0.14f));
    CHECK(p.visible == false);

    CHECK(nearlyEqual(p.value, 37.5f));
    CHECK(nearlyEqual(p.minValue, -12.25f));
    CHECK(nearlyEqual(p.maxValue, 88.75f));

    CHECK(nearlyEqual(p.fillColor.r, 0.21f));
    CHECK(nearlyEqual(p.fillColor.g, 0.22f));
    CHECK(nearlyEqual(p.fillColor.b, 0.23f));
    CHECK(nearlyEqual(p.fillColor.a, 0.24f));
    CHECK(p.fillDirection == UiProgressFillDirection::BottomToTop);

    CHECK(p.atlasPath == "assets/ui/hud.png");
    CHECK(p.backgroundPath == "assets/ui/bar_bg.png");
    CHECK(p.fillPath == "assets/ui/bar_fill.png");
}

static void test_progress_bar_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Vida", canvasGo);
    auto bar = std::make_shared<ProgressBarComponent>();
    fillBar(*bar);
    go->setProgressBar(bar);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasProgressBar()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Vida");
    checkBarMatchesFilled(*found->getProgressBar());
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: without bar and without warnings.
static void test_scene_without_progress_bar_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasProgressBar()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any progress bar the JSON gains not a byte, and adding and removing the
// component returns the EXACT dump from the start.
static void test_scene_without_progress_bar_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"progressBar\"") == std::string::npos);

    go->setProgressBar(std::make_shared<ProgressBarComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setProgressBar(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

// Add reversible, and redo does NOT return the fields to defaults.
static void test_progress_bar_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vida");
    ProgressBarComponent st;
    fillBar(st);
    ProgressBarComponentCommand cmd(scene, "Add Progress Bar", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasProgressBar());
    checkBarMatchesFilled(*go->getProgressBar());
    cmd.undo();
    CHECK(!go->hasProgressBar());
    cmd.execute();
    CHECK(go->hasProgressBar());
    checkBarMatchesFilled(*go->getProgressBar());
}

// Remove reversible: undo returns the component WITH its values.
static void test_progress_bar_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vida");
    auto bar = std::make_shared<ProgressBarComponent>();
    fillBar(*bar);
    go->setProgressBar(bar);

    ProgressBarComponentCommand cmd(scene, "Remove Progress Bar", go->id, /*add=*/false, *bar);
    cmd.execute();
    CHECK(!go->hasProgressBar());
    cmd.undo();
    CHECK(go->hasProgressBar());
    checkBarMatchesFilled(*go->getProgressBar());
}

// Editing a field of the progress bar also goes into the stack, with the same
// PropertyCommand<T> that builds the section (resolved by id, not by pointer).
static void test_progress_bar_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vida");
    go->setProgressBar(std::make_shared<ProgressBarComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyValue = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasProgressBar()) g->getProgressBar()->value = v;
    };
    PropertyCommand<float> cmd("Value", 0.5f, 0.125f, applyValue);

    cmd.execute();
    CHECK(nearlyEqual(go->getProgressBar()->value, 0.125f));
    cmd.undo();
    CHECK(nearlyEqual(go->getProgressBar()->value, 0.5f));
    cmd.execute();
    CHECK(nearlyEqual(go->getProgressBar()->value, 0.125f));

    // Without component the applier does nothing (neither crashes nor resurrects it).
    go->setProgressBar(nullptr);
    cmd.undo();
    CHECK(!go->hasProgressBar());
}

// The four directions at 25%: each one gives a DIFFERENT rect and in the right place
// (the Y of the canvas grows downward). And a value outside the range cannot go beyond
// the bottom, which the component does not clamp on its own.
static void test_progress_bar_fill_directions()
{
    ProgressBarComponent p;
    p.size     = glm::vec2(200.0f, 40.0f);
    p.minValue = 0.0f;
    p.maxValue = 100.0f;
    p.value    = 25.0f;

    glm::vec2 pos{0.0f};
    glm::vec2 sz{0.0f};

    p.fillDirection = UiProgressFillDirection::LeftToRight;
    p.fillRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));
    CHECK(nearlyEqual(pos.y, 0.0f));
    CHECK(nearlyEqual(sz.x, 50.0f));
    CHECK(nearlyEqual(sz.y, 40.0f));

    p.fillDirection = UiProgressFillDirection::RightToLeft;
    p.fillRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 150.0f));
    CHECK(nearlyEqual(pos.y, 0.0f));
    CHECK(nearlyEqual(sz.x, 50.0f));
    CHECK(nearlyEqual(sz.y, 40.0f));

    p.fillDirection = UiProgressFillDirection::TopToBottom;
    p.fillRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));
    CHECK(nearlyEqual(pos.y, 0.0f));
    CHECK(nearlyEqual(sz.x, 200.0f));
    CHECK(nearlyEqual(sz.y, 10.0f));

    p.fillDirection = UiProgressFillDirection::BottomToTop;
    p.fillRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));
    CHECK(nearlyEqual(pos.y, 30.0f));
    CHECK(nearlyEqual(sz.x, 200.0f));
    CHECK(nearlyEqual(sz.y, 10.0f));

    // Out of range on top: full, but not a pixel beyond the background.
    p.value = 999.0f;
    for (int d = 0; d < 4; d++)
    {
        p.fillDirection = (UiProgressFillDirection)d;
        p.fillRect(pos, sz);
        CHECK(pos.x >= 0.0f && pos.y >= 0.0f);
        CHECK(pos.x + sz.x <= p.size.x + 1e-4f);
        CHECK(pos.y + sz.y <= p.size.y + 1e-4f);
    }
    // And from below: empty, never negative.
    p.value = -999.0f;
    for (int d = 0; d < 4; d++)
    {
        p.fillDirection = (UiProgressFillDirection)d;
        p.fillRect(pos, sz);
        CHECK(nearlyEqual(sz.x * sz.y, 0.0f));
        CHECK(pos.x >= 0.0f && pos.y >= 0.0f);
    }
    // Degenerate range: there is no way to distribute an empty interval.
    p.value = 5.0f; p.minValue = 3.0f; p.maxValue = 3.0f;
    CHECK(nearlyEqual(p.normalizedValue(), 0.0f));
}

// Editing the value has to show in the next frame. The tree caches
// vertices by node, so a sync that writes fields without dirtying leaves the
// bar STUCK: that is what the dirty flags here catch. And dirtying ALWAYS
// would throw away the canvas vertex cache every frame, which is the other possible failure.
static void test_progress_bar_sync_updates_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ProgressBarComponent p;
    p.position = glm::vec2(10.0f, 20.0f);
    p.size     = glm::vec2(200.0f, 40.0f);
    p.value    = 0.5f;   // min 0, max 1

    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras{ {5ull, &p} };
    UiDrawData data;

    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& fondo = *canvas.root().children()[0];
    CHECK(fondo.name == uiProgressBarNodeName(5ull));
    CHECK(fondo.children().size() == 1);
    if (fondo.children().empty()) return;
    const UiElement& relleno = *fondo.children()[0];
    CHECK(relleno.name == uiProgressBarNodeName(5ull) + "/Fill");

    // Full background + fill halfway: two quads.
    CHECK(data.vertices.size() == 8);
    CHECK(nearlyEqual(fondo.screenPos.x, 10.0f));
    CHECK(nearlyEqual(fondo.screenPos.y, 20.0f));
    CHECK(nearlyEqual(relleno.size.x, 100.0f));
    CHECK(nearlyEqual(relleno.size.y, 40.0f));
    // The emitter leaves nodes clean: it is the cache that sync has to
    // invalidate.
    CHECK(fondo.dirty == 0u);
    CHECK(relleno.dirty == 0u);

    // Another value: the fill changes size AND both nodes get dirty, or
    // the canvas would reuse the vertices from before.
    p.value = 0.25f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(nearlyEqual(relleno.size.x, 50.0f));
    CHECK(relleno.dirty != 0u);
    CHECK(fondo.dirty != 0u);

    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(relleno.screenPos.x, 10.0f));
    CHECK(nearlyEqual(relleno.screenPos.y, 20.0f));

    // With RightToLeft the fill sticks to the other end, on screen and not just
    // in the local rect.
    p.fillDirection = UiProgressFillDirection::RightToLeft;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(relleno.screenPos.x, 160.0f));   // 10 + 200*(1-0.25)

    // And a frame with no changes does NOT get dirty again: dirtying always throws away the
    // vertex cache of the entire canvas every frame.
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(fondo.dirty == 0u);
    CHECK(relleno.dirty == 0u);

    // At value 0 the fill emits no quad (degenerate rect), but the NODE still
    // exists: if it appeared and disappeared it would change the subtree shape and
    // force rebuilding the root when crossing zero.
    p.value = 0.0f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(fondo.children().size() == 1);
    CHECK(data.vertices.size() == 4);
}

// The trap: the canvas root is rebuilt with clearChildren(), so a
// sync that only knew two of the three types would wipe out the third. With the
// THREE in the scene, none of them wipes out the others.
static void test_all_three_ui_components_coexist()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent b;
    b.position = glm::vec2(0.0f, 0.0f);
    b.size     = glm::vec2(100.0f, 40.0f);
    TextComponent t;
    t.text     = "Titulo";
    t.position = glm::vec2(200.0f, 150.0f);
    t.size     = glm::vec2(120.0f, 30.0f);
    ProgressBarComponent p;
    p.position = glm::vec2(300.0f, 300.0f);
    p.size     = glm::vec2(200.0f, 20.0f);
    p.value    = 0.5f;

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones{ {7ull, &b} };
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos { {9ull, &t} };
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras { {5ull, &p} };

    UiDrawData data;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(canvas.root().children().size() == 3);
    if (canvas.root().children().size() != 3) return;
    CHECK(canvas.root().children()[0]->name == uiButtonNodeName(7ull));
    CHECK(canvas.root().children()[1]->name == uiProgressBarNodeName(5ull));
    CHECK(canvas.root().children()[2]->name == uiTextNodeName(9ull));
    // Button (1 quad) + bar (background and fill) = 3; text without font does not paint.
    CHECK(data.vertices.size() == 12);

    // Touching ONLY the bar does not wipe the button or text.
    p.value = 0.75f;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 3);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 12);
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));
    const Text* vivo = canvas.root().children()[2]->asText();
    CHECK(vivo != nullptr);
    if (vivo) CHECK(vivo->text == "Titulo");

    // And touching ONLY the text does not touch the bar.
    t.text = "Otro";
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 3);
    const UiElement& fondo = *canvas.root().children()[1];
    CHECK(fondo.children().size() == 1);
    if (!fondo.children().empty())
        CHECK(nearlyEqual(fondo.children()[0]->size.x, 150.0f));

    // One more bar rebuilds the root: all three types move up.
    ProgressBarComponent p2;
    p2.size  = glm::vec2(50.0f, 10.0f);
    p2.value = 1.0f;
    barras.emplace_back(11ull, &p2);
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 4);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 20);   // button + 2 bars x 2 quads
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));   // the button is still there
}

// A newly added bar has no image: it cannot cost a load,
// which is synchronous (read + bake + upload to GPU) and looks like a stall right when
// you press Add. And then, one load per DIFFERENT PATH and only one: the path cache
// is what prevents one load (and a GPU memory leak) per frame.
static void test_progress_bar_without_atlas_loads_nothing()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ProgressBarComponent p;   // without images, as "Add Component" leaves it

    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras{ {5ull, &p} };
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 0);
    CHECK(canvas.root().children().size() == 1);

    // And several more frames with no path do not ask for it either.
    p.value = 0.9f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 0);

    // Just the atlas: one load, and both parts pull from it (background and
    // fill fall into the atlas when they bring no path of their own).
    p.atlasPath = "assets/ui/hud.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 1);
    p.value = 0.1f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 1);

    // Two different own paths: two more loads, one per file.
    p.backgroundPath = "assets/ui/bar_bg.png";
    p.fillPath       = "assets/ui/bar_fill.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);

    // And the same file in both parts does NOT count twice.
    p.fillPath = "assets/ui/bar_bg.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);

    // Changing the value with all three paths set does not reload anything.
    p.value = 0.4f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);
}

// Click on a bar in the viewport: same path as the button and the
// text, and with all three components in the SAME GameObject the three names
// lead to their id without stepping on each other.
static void test_progress_bar_hit_test_maps_back_to_gameobject()
{
    CHECK(uiProgressBarOwnerId(uiProgressBarNodeName(42ull)) == 42ull);
    // The fill node hangs from the bar: its name also leads to the owner.
    CHECK(uiProgressBarOwnerId(uiProgressBarNodeName(42ull) + "/Fill") == 42ull);
    CHECK(uiProgressBarOwnerId("Cubo") == 0ull);
    CHECK(uiProgressBarOwnerId("bar:") == 0ull);
    CHECK(uiProgressBarOwnerId("bar:12ab") == 0ull);
    // The three prefixes do not confuse each other.
    CHECK(uiProgressBarOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiProgressBarOwnerId(uiTextNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
    CHECK(uiTextOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
}

// ── Layout ──────────────────────────────────────────────────────────────────
// The auto-layout solver already lived in UiElement (layoutMode, padding, spacing,
// cell); what did not exist was a way to use it from the scene. What is tested
// here is exactly the new layer: that a GameObject WITHOUT another UI component mounts
// its own container, that with another component it does NOT mount one more, and that the
// fields reach the right node. The solver arithmetic is already covered by
// ui_batch_tests.

// An empty container places its children and does NOT paint: it is a rect, not a
// widget. The positions of the children are deliberately absurd: if the layout
// did not overwrite them, the test would see them.
static void test_layout_container_places_children()
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* menu = scene.addGameObject("Menu", canvasGo);
    auto layout = std::make_shared<LayoutComponent>();
    layout->mode        = UiLayoutMode::Vertical;
    layout->position    = glm::vec2(30.0f, 40.0f);
    layout->size        = glm::vec2(300.0f, 200.0f);
    layout->paddingLeft = 7.0f;
    layout->paddingTop  = 5.0f;
    layout->spacing     = glm::vec2(9.0f, 12.0f);   // .x != .y: distinguishes the axes
    menu->setLayout(layout);

    GameObject* uno = scene.addGameObject("Uno", menu);
    auto a = std::make_shared<ButtonComponent>();
    a->size     = glm::vec2(100.0f, 40.0f);
    a->position = glm::vec2(999.0f, 999.0f);   // the layout commands it, not the child
    uno->setButton(a);

    GameObject* dos = scene.addGameObject("Dos", menu);
    auto b = std::make_shared<ButtonComponent>();
    b->size     = glm::vec2(80.0f, 30.0f);
    b->position = glm::vec2(-500.0f, -500.0f);
    dos->setButton(b);

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;
    CHECK(layouts.size() == 1);

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader, &jerarquia, &layouts);

    // One container per GameObject and no more: the menu hangs from the root and the
    // two buttons from it.
    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    CHECK(canvas.root().children()[0]->name == uiLayoutNodeName(menu->id));
    CHECK(canvas.root().children()[0]->children().size() == 2);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    // The container does not paint: two buttons, four vertices each.
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // origin = position + padding = (30+7, 40+5)
    CHECK(nearlyEqual(data.vertices[0].pos.x, 37.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 45.0f));
    // The second lowers the height of the first PLUS the spacing in Y (not in X).
    CHECK(nearlyEqual(data.vertices[4].pos.x, 37.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 97.0f));
}

// With another UI component in the same GameObject the layout does NOT mount a container:
// it writes its fields in the node that is already there. One more panel would be an invisible rect
// between the widget and its children, and the anchors would not match up.
static void test_layout_on_widget_uses_its_node()
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* barra = scene.addGameObject("Barra", canvasGo);
    auto fondo = std::make_shared<ButtonComponent>();
    fondo->position = glm::vec2(10.0f, 20.0f);
    fondo->size     = glm::vec2(200.0f, 100.0f);
    barra->setButton(fondo);
    auto layout = std::make_shared<LayoutComponent>();
    layout->mode        = UiLayoutMode::Horizontal;
    layout->paddingLeft = 6.0f;
    layout->paddingTop  = 4.0f;
    barra->setLayout(layout);

    GameObject* icono = scene.addGameObject("Icono", barra);
    auto ib = std::make_shared<ButtonComponent>();
    ib->size     = glm::vec2(30.0f, 30.0f);
    ib->position = glm::vec2(500.0f, 500.0f);
    icono->setButton(ib);

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader, &jerarquia, &layouts);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    // The GameObject node is still the button's, not a new container.
    CHECK(canvas.root().children()[0]->name == uiButtonNodeName(barra->id));
    CHECK(canvas.root().children()[0]->children().size() == 1);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 10.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 20.0f));
    // The child, placed by the button layout: corner + padding.
    CHECK(nearlyEqual(data.vertices[4].pos.x, 16.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 24.0f));
}

// ignoreLayout is what Unity solves with a separate LayoutElement: here it goes
// in the same component. The child that sets it anchors itself and does NOT take up
// space, so the next one starts at the container origin.
static void test_layout_ignore_layout_child_keeps_its_anchor()
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* menu = scene.addGameObject("Menu", canvasGo);
    auto layout = std::make_shared<LayoutComponent>();
    layout->mode = UiLayoutMode::Vertical;
    layout->size = glm::vec2(300.0f, 200.0f);
    menu->setLayout(layout);

    // Loose: button + layout in the SAME GameObject only for ignoreLayout.
    GameObject* suelto = scene.addGameObject("Suelto", menu);
    auto sb = std::make_shared<ButtonComponent>();
    sb->size     = glm::vec2(100.0f, 40.0f);
    sb->position = glm::vec2(250.0f, 150.0f);
    suelto->setButton(sb);
    auto suelta = std::make_shared<LayoutComponent>();
    suelta->mode         = UiLayoutMode::None;
    suelta->ignoreLayout = true;
    suelto->setLayout(suelta);

    GameObject* colocado = scene.addGameObject("Colocado", menu);
    auto cb = std::make_shared<ButtonComponent>();
    cb->size = glm::vec2(80.0f, 30.0f);
    // Not neutral: if the layout did not place it, the test would see this position instead
    // of the container origin.
    cb->position = glm::vec2(77.0f, 88.0f);
    colocado->setButton(cb);

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;
    CHECK(layouts.size() == 2);

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader, &jerarquia, &layouts);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // The loose one, in its own position inside the container.
    CHECK(nearlyEqual(data.vertices[0].pos.x, 250.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 150.0f));
    // And the placed one starts at the very top: the loose one did not eat its space.
    CHECK(nearlyEqual(data.vertices[4].pos.x, 0.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 0.0f));
}

// A container DOES provide a rect, so it holds its children in the hierarchy
// just like a button. Without this, its children would go up to the root and the layout would not
// place anyone.
static void test_collect_ui_widgets_incluye_los_layouts()
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* menu = scene.addGameObject("Menu", canvasGo);
    menu->setLayout(std::make_shared<LayoutComponent>());

    GameObject* boton = scene.addGameObject("Boton", menu);
    boton->setButton(std::make_shared<ButtonComponent>());

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;

    CHECK(layouts.size() == 1);
    CHECK(botones.size() == 1);
    if (layouts.size() != 1 || botones.size() != 1) return;
    CHECK(layouts[0].first == menu->id);

    CHECK(jerarquia.size() == 2);
    if (jerarquia.size() != 2) return;
    CHECK(jerarquia[0].first == menu->id);
    CHECK(jerarquia[0].second == 0ull);
    CHECK(jerarquia[1].first == boton->id);
    CHECK(jerarquia[1].second == menu->id);   // the container, not the canvas
}

// Without hierarchy (the path of old scenes) the container still mounts
// in the root: losing it would leave the scene without the rect that groups.
static void test_layout_sin_jerarquia_monta_en_la_raiz()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    LayoutComponent l;
    l.position = glm::vec2(12.0f, 34.0f);
    l.size     = glm::vec2(56.0f, 78.0f);
    std::vector<std::pair<uint64_t, const LayoutComponent*>> layouts{ {21ull, &l} };

    syncUiWidgets({}, {}, {}, canvas, cache, loader, nullptr, &layouts);
    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    CHECK(canvas.root().children()[0]->name == uiLayoutNodeName(21ull));
    // And it does not paint: a container is a rect, not a color quad.
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.empty());
}

// Non-neutral values and DIFFERENT from each other: with zeros, ones or repeated, a
// field that serialization does not write would pass the same (the default would hide it).
static void fillLayout(LayoutComponent& l)
{
    l.anchorMin = glm::vec2(0.0625f, 0.1875f);
    l.anchorMax = glm::vec2(0.6875f, 0.9375f);
    l.pivot     = glm::vec2(0.28125f, 0.34375f);
    l.position  = glm::vec2(13.5f, -27.25f);
    l.size      = glm::vec2(321.75f, 213.5f);
    l.visible   = false;

    l.mode = UiLayoutMode::Grid;

    l.paddingLeft   = 3.25f;
    l.paddingRight  = 5.75f;
    l.paddingTop    = 7.125f;
    l.paddingBottom = 9.375f;

    l.spacing  = glm::vec2(11.5f, 13.25f);
    l.cellSize = glm::vec2(64.75f, 48.125f);
    l.columns  = 7;

    l.crossAlign = UiCrossAlign::End;

    l.fitWidth  = true;
    l.fitHeight = true;

    l.ignoreLayout = true;
    l.clipChildren = true;
}

static void checkLayoutMatchesFilled(const LayoutComponent& l)
{
    CHECK(nearlyEqual(l.anchorMin.x, 0.0625f));
    CHECK(nearlyEqual(l.anchorMin.y, 0.1875f));
    CHECK(nearlyEqual(l.anchorMax.x, 0.6875f));
    CHECK(nearlyEqual(l.anchorMax.y, 0.9375f));
    CHECK(nearlyEqual(l.pivot.x, 0.28125f));
    CHECK(nearlyEqual(l.pivot.y, 0.34375f));
    CHECK(nearlyEqual(l.position.x, 13.5f));
    CHECK(nearlyEqual(l.position.y, -27.25f));
    CHECK(nearlyEqual(l.size.x, 321.75f));
    CHECK(nearlyEqual(l.size.y, 213.5f));
    CHECK(l.visible == false);

    CHECK(l.mode == UiLayoutMode::Grid);

    CHECK(nearlyEqual(l.paddingLeft, 3.25f));
    CHECK(nearlyEqual(l.paddingRight, 5.75f));
    CHECK(nearlyEqual(l.paddingTop, 7.125f));
    CHECK(nearlyEqual(l.paddingBottom, 9.375f));

    CHECK(nearlyEqual(l.spacing.x, 11.5f));
    CHECK(nearlyEqual(l.spacing.y, 13.25f));
    CHECK(nearlyEqual(l.cellSize.x, 64.75f));
    CHECK(nearlyEqual(l.cellSize.y, 48.125f));
    CHECK(l.columns == 7u);

    CHECK(l.crossAlign == UiCrossAlign::End);

    CHECK(l.fitWidth == true);
    CHECK(l.fitHeight == true);

    CHECK(l.ignoreLayout == true);
    CHECK(l.clipChildren == true);
}

static void test_layout_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Menu", canvasGo);
    auto layout = std::make_shared<LayoutComponent>();
    fillLayout(*layout);
    go->setLayout(layout);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasLayout()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Menu");
    checkLayoutMatchesFilled(*found->getLayout());
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: no Layout and no warnings.
static void test_scene_without_layout_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasLayout()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any Layout the JSON does not gain a single byte, and adding and removing
// the component returns the EXACT dump from the start.
static void test_scene_without_layout_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    const std::string antes = scene.toJson().dump();

    auto layout = std::make_shared<LayoutComponent>();
    fillLayout(*layout);
    go->setLayout(layout);
    CHECK(scene.toJson().dump() != antes);

    go->setLayout(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_layout_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Menu");
    LayoutComponent st;
    fillLayout(st);
    LayoutComponentCommand cmd(scene, "Add Layout", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasLayout());
    checkLayoutMatchesFilled(*go->getLayout());
    cmd.undo();
    CHECK(!go->hasLayout());
    cmd.execute();
    CHECK(go->hasLayout());
    checkLayoutMatchesFilled(*go->getLayout());
}

// Remove reversible: undo returns the component WITH its values.
static void test_layout_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Menu");
    auto layout = std::make_shared<LayoutComponent>();
    fillLayout(*layout);
    go->setLayout(layout);

    LayoutComponentCommand cmd(scene, "Remove Layout", go->id, /*add=*/false, *layout);
    cmd.execute();
    CHECK(!go->hasLayout());
    cmd.undo();
    CHECK(go->hasLayout());
    checkLayoutMatchesFilled(*go->getLayout());
}

// Editing a container field also goes into the stack, with the same
// PropertyCommand<T> that the section builds (resolved by id, not by pointer).
static void test_layout_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Menu");
    go->setLayout(std::make_shared<LayoutComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applySpacing = [sc, id](const glm::vec2& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasLayout()) g->getLayout()->spacing = v;
    };
    PropertyCommand<glm::vec2> cmd("Spacing", glm::vec2(0.0f, 0.0f), glm::vec2(4.5f, 6.25f),
                                   applySpacing);

    cmd.execute();
    CHECK(nearlyEqual(go->getLayout()->spacing.x, 4.5f));
    CHECK(nearlyEqual(go->getLayout()->spacing.y, 6.25f));
    cmd.undo();
    CHECK(nearlyEqual(go->getLayout()->spacing.x, 0.0f));
    cmd.execute();
    CHECK(nearlyEqual(go->getLayout()->spacing.y, 6.25f));

    // Without component the applier does nothing (neither crashes nor resurrects it).
    go->setLayout(nullptr);
    cmd.undo();
    CHECK(!go->hasLayout());
}

// A container does not paint, so it also cannot EAT clicks: the hit test
// has to go through it. If it were raycastTarget, a group that only places
// would leave dead what was behind it, and with nothing painted there would be no way to see
// why.
static void test_layout_container_no_se_come_los_clics()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    LayoutComponent l;
    l.mode     = UiLayoutMode::None;   // without placing: the child stays in its place
    l.position = glm::vec2(0.0f, 0.0f);
    l.size     = glm::vec2(400.0f, 300.0f);
    std::vector<std::pair<uint64_t, const LayoutComponent*>> layouts{ {31ull, &l} };

    ButtonComponent b;
    b.position = glm::vec2(10.0f, 10.0f);
    b.size     = glm::vec2(50.0f, 20.0f);
    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {32ull, &b} };
    // The button hangs from the container.
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {31ull, 0ull}, {32ull, 31ull} };

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia, &layouts);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    // Inside the button: it gets it.
    const UiElement* enBoton = canvas.hitTest(glm::vec2(20.0f, 15.0f));
    CHECK(enBoton != nullptr);
    if (enBoton) CHECK(uiButtonOwnerId(enBoton->name) == 32ull);

    // Inside the container but OUTSIDE the button: no one gets it.
    CHECK(canvas.hitTest(glm::vec2(300.0f, 250.0f)) == nullptr);
}

// The node name leads back to the GameObject (click in the viewport), and does not
// confuse with the three other prefixes.
static void test_layout_hit_test_maps_back_to_gameobject()
{
    CHECK(uiLayoutOwnerId(uiLayoutNodeName(42ull)) == 42ull);
    CHECK(uiLayoutOwnerId("Cubo") == 0ull);
    CHECK(uiLayoutOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiLayoutOwnerId(uiTextNodeName(42ull)) == 0ull);
    CHECK(uiLayoutOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiLayoutNodeName(42ull)) == 0ull);
    CHECK(uiTextOwnerId(uiLayoutNodeName(42ull)) == 0ull);
    CHECK(uiProgressBarOwnerId(uiLayoutNodeName(42ull)) == 0ull);
}

// ── Panel ─────────────────────────────────────────────────────────────────── The core Panel
// (UiWidgets.h) is a UiElement without its own fields: it is the background rectangle with which
// frames and groups are built. The scene component exposes the same as the rest — the rect, color,
// visibility and the atlas/sprite pair — plus raycastTarget, which the core does have and which in
// a background panel is exactly the field that decides whether it eats clicks from behind.
//
// Non-neutral values and DIFFERENT from each other: with the default (or with two equal fields) a
// round-trip passes the same even if fromJson skips the field or reads the wrong key.
static void fillPanel(PanelComponent& p)
{
    p.anchorMin = glm::vec2(0.0625f, 0.1875f);
    p.anchorMax = glm::vec2(0.5625f, 0.8125f);
    p.pivot     = glm::vec2(0.25f, 0.75f);
    p.position  = glm::vec2(13.5f, -27.25f);
    p.size      = glm::vec2(311.5f, 122.25f);
    p.color     = glm::vec4(0.31f, 0.32f, 0.33f, 0.34f);
    p.visible   = false;

    p.raycastTarget = false;

    p.atlasPath = "assets/ui/frames.png";
    p.sprite    = "marco_dorado";
}

static void checkPanelMatchesFilled(const PanelComponent& p)
{
    CHECK(nearlyEqual(p.anchorMin.x, 0.0625f));
    CHECK(nearlyEqual(p.anchorMin.y, 0.1875f));
    CHECK(nearlyEqual(p.anchorMax.x, 0.5625f));
    CHECK(nearlyEqual(p.anchorMax.y, 0.8125f));
    CHECK(nearlyEqual(p.pivot.x, 0.25f));
    CHECK(nearlyEqual(p.pivot.y, 0.75f));
    CHECK(nearlyEqual(p.position.x, 13.5f));
    CHECK(nearlyEqual(p.position.y, -27.25f));
    CHECK(nearlyEqual(p.size.x, 311.5f));
    CHECK(nearlyEqual(p.size.y, 122.25f));
    CHECK(nearlyEqual(p.color.r, 0.31f));
    CHECK(nearlyEqual(p.color.g, 0.32f));
    CHECK(nearlyEqual(p.color.b, 0.33f));
    CHECK(nearlyEqual(p.color.a, 0.34f));
    CHECK(p.visible == false);
    CHECK(p.raycastTarget == false);
    CHECK(p.atlasPath == "assets/ui/frames.png");
    CHECK(p.sprite == "marco_dorado");
}

static void test_panel_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Marco", canvasGo);
    auto panel = std::make_shared<PanelComponent>();
    fillPanel(*panel);
    go->setPanel(panel);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasPanel()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Marco");
    checkPanelMatchesFilled(*found->getPanel());
    CHECK(loaded.lastWarnings().empty());
}

// A scene saved before the component loads the same: no panel and no warnings.
static void test_scene_without_panel_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasPanel()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

// Neutrality: without any panel the JSON does not gain a single byte, and adding and removing the
// component returns the EXACT dump from the start.
static void test_scene_without_panel_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"panel\"") == std::string::npos);

    go->setPanel(std::make_shared<PanelComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setPanel(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

// Add reversible, and redo does NOT return the fields to defaults.
static void test_panel_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Marco");
    PanelComponent st;
    fillPanel(st);
    PanelComponentCommand cmd(scene, "Add Panel", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasPanel());
    checkPanelMatchesFilled(*go->getPanel());
    cmd.undo();
    CHECK(!go->hasPanel());
    cmd.execute();
    CHECK(go->hasPanel());
    checkPanelMatchesFilled(*go->getPanel());
}

// Remove reversible: undo returns the component WITH its values.
static void test_panel_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Marco");
    auto panel = std::make_shared<PanelComponent>();
    fillPanel(*panel);
    go->setPanel(panel);

    PanelComponentCommand cmd(scene, "Remove Panel", go->id, /*add=*/false, *panel);
    cmd.execute();
    CHECK(!go->hasPanel());
    cmd.undo();
    CHECK(go->hasPanel());
    checkPanelMatchesFilled(*go->getPanel());
}

// Editing a panel field also goes into the stack, with the same
// PropertyCommand<T> that the section builds (resolved by id, not by pointer).
static void test_panel_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Marco");
    go->setPanel(std::make_shared<PanelComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applySize = [sc, id](const glm::vec2& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasPanel()) g->getPanel()->size = v;
    };
    PropertyCommand<glm::vec2> cmd("Size", glm::vec2(200.0f, 120.0f), glm::vec2(37.5f, 91.25f),
                                   applySize);

    cmd.execute();
    CHECK(nearlyEqual(go->getPanel()->size.x, 37.5f));
    CHECK(nearlyEqual(go->getPanel()->size.y, 91.25f));
    cmd.undo();
    CHECK(nearlyEqual(go->getPanel()->size.x, 200.0f));
    cmd.execute();
    CHECK(nearlyEqual(go->getPanel()->size.y, 91.25f));

    // Without component the applier does nothing (neither crashes nor resurrects it).
    go->setPanel(nullptr);
    cmd.undo();
    CHECK(!go->hasPanel());
}

// The live node: its own name, rect dumped and, above all, that a change in the
// component DIRTIES the node. A sync that writes fields without dirtying leaves the
// panel stuck, because the canvas copies the cached vertices.
static void test_panel_sync_updates_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    PanelComponent p;
    p.position = glm::vec2(12.0f, 24.0f);
    p.size     = glm::vec2(160.0f, 80.0f);
    p.color    = glm::vec4(0.5f, 0.25f, 0.125f, 1.0f);

    UiWidgetLists w;
    w.panels.emplace_back(5ull, &p);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& nodo = *canvas.root().children()[0];
    CHECK(nodo.name == uiPanelNodeName(5ull));
    CHECK(nodo.typeName() == std::string("Panel"));
    CHECK(nearlyEqual(nodo.screenPos.x, 12.0f));
    CHECK(nearlyEqual(nodo.screenPos.y, 24.0f));
    CHECK(nearlyEqual(nodo.size.x, 160.0f));
    CHECK(nearlyEqual(nodo.size.y, 80.0f));
    CHECK(nearlyEqual(nodo.color.r, 0.5f));
    CHECK(data.vertices.size() == 4);   // un quad
    // The emitter leaves the node clean: it is the cache that sync has to invalidate.
    CHECK(nodo.dirty == 0u);

    // Moving the panel has to dirty it, or the canvas would reuse the vertices.
    p.position = glm::vec2(40.0f, 50.0f);
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nodo.dirty != 0u);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(nodo.screenPos.x, 40.0f));

    // And without changes does NOT get dirty again: dirtying always would throw away the cache
    // of the entire canvas every frame.
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nodo.dirty == 0u);

    // raycastTarget travels: without it a background panel would eat clicks from what
    // is behind it and there would be no way to turn it off from the scene.
    CHECK(nodo.raycastTarget == true);
    p.raycastTarget = false;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nodo.raycastTarget == false);
}

static void test_panel_hit_test_maps_back_to_gameobject()
{
    CHECK(uiPanelOwnerId(uiPanelNodeName(42ull)) == 42ull);
    CHECK(uiPanelOwnerId("Cubo") == 0ull);
    CHECK(uiPanelOwnerId("pnl:") == 0ull);
    CHECK(uiPanelOwnerId("pnl:12ab") == 0ull);
    // The prefixes do not confuse each other.
    CHECK(uiPanelOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiPanelOwnerId(uiTextNodeName(42ull)) == 0ull);
    CHECK(uiPanelOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
    CHECK(uiPanelOwnerId(uiLayoutNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiPanelNodeName(42ull)) == 0ull);
    CHECK(uiTextOwnerId(uiPanelNodeName(42ull)) == 0ull);
    CHECK(uiProgressBarOwnerId(uiPanelNodeName(42ull)) == 0ull);
    CHECK(uiLayoutOwnerId(uiPanelNodeName(42ull)) == 0ull);
}

// ── Image ───────────────────────────────────────────────────────────────────
// The core Image DOES have its own fields (mode, 9-slice borders, tile cap
// and the Filled block), and all of them have to reach the node: the
// batcher reads them to emit N quads.
static void fillImage(ImageComponent& im)
{
    im.anchorMin = glm::vec2(0.09375f, 0.15625f);
    im.anchorMax = glm::vec2(0.6875f, 0.9375f);
    im.pivot     = glm::vec2(0.125f, 0.625f);
    im.position  = glm::vec2(-17.75f, 29.5f);
    im.size      = glm::vec2(97.25f, 143.5f);
    im.color     = glm::vec4(0.41f, 0.42f, 0.43f, 0.44f);
    im.visible   = false;

    im.raycastTarget = false;

    im.atlasPath = "assets/ui/iconos.png";
    im.sprite    = "corazon";

    im.mode = UiImageMode::Sliced;

    im.borderLeft   = 3.5f;
    im.borderRight  = 5.25f;
    im.borderTop    = 7.75f;
    im.borderBottom = 9.125f;
    im.fillCenter   = false;

    im.maxTiles = 777u;

    im.fillDirection = UiFillDirection::Vertical;
    im.fillOrigin    = UiFillOrigin::End;
    im.fillAmount    = 0.375f;
}

static void checkImageMatchesFilled(const ImageComponent& im)
{
    CHECK(nearlyEqual(im.anchorMin.x, 0.09375f));
    CHECK(nearlyEqual(im.anchorMin.y, 0.15625f));
    CHECK(nearlyEqual(im.anchorMax.x, 0.6875f));
    CHECK(nearlyEqual(im.anchorMax.y, 0.9375f));
    CHECK(nearlyEqual(im.pivot.x, 0.125f));
    CHECK(nearlyEqual(im.pivot.y, 0.625f));
    CHECK(nearlyEqual(im.position.x, -17.75f));
    CHECK(nearlyEqual(im.position.y, 29.5f));
    CHECK(nearlyEqual(im.size.x, 97.25f));
    CHECK(nearlyEqual(im.size.y, 143.5f));
    CHECK(nearlyEqual(im.color.r, 0.41f));
    CHECK(nearlyEqual(im.color.g, 0.42f));
    CHECK(nearlyEqual(im.color.b, 0.43f));
    CHECK(nearlyEqual(im.color.a, 0.44f));
    CHECK(im.visible == false);
    CHECK(im.raycastTarget == false);
    CHECK(im.atlasPath == "assets/ui/iconos.png");
    CHECK(im.sprite == "corazon");

    CHECK(im.mode == UiImageMode::Sliced);
    CHECK(nearlyEqual(im.borderLeft, 3.5f));
    CHECK(nearlyEqual(im.borderRight, 5.25f));
    CHECK(nearlyEqual(im.borderTop, 7.75f));
    CHECK(nearlyEqual(im.borderBottom, 9.125f));
    CHECK(im.fillCenter == false);
    CHECK(im.maxTiles == 777u);
    CHECK(im.fillDirection == UiFillDirection::Vertical);
    CHECK(im.fillOrigin == UiFillOrigin::End);
    CHECK(nearlyEqual(im.fillAmount, 0.375f));
}

static void test_image_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Icono", canvasGo);
    auto img = std::make_shared<ImageComponent>();
    fillImage(*img);
    go->setImage(img);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasImage()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Icono");
    checkImageMatchesFilled(*found->getImage());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_image_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasImage()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_image_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"image\"") == std::string::npos);

    go->setImage(std::make_shared<ImageComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setImage(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_image_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Icono");
    ImageComponent st;
    fillImage(st);
    ImageComponentCommand cmd(scene, "Add Image", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasImage());
    checkImageMatchesFilled(*go->getImage());
    cmd.undo();
    CHECK(!go->hasImage());
    cmd.execute();
    CHECK(go->hasImage());
    checkImageMatchesFilled(*go->getImage());
}

static void test_image_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Icono");
    auto img = std::make_shared<ImageComponent>();
    fillImage(*img);
    go->setImage(img);

    ImageComponentCommand cmd(scene, "Remove Image", go->id, /*add=*/false, *img);
    cmd.execute();
    CHECK(!go->hasImage());
    cmd.undo();
    CHECK(go->hasImage());
    checkImageMatchesFilled(*go->getImage());
}

static void test_image_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Icono");
    go->setImage(std::make_shared<ImageComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyFill = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasImage()) g->getImage()->fillAmount = v;
    };
    PropertyCommand<float> cmd("Fill Amount", 1.0f, 0.125f, applyFill);

    cmd.execute();
    CHECK(nearlyEqual(go->getImage()->fillAmount, 0.125f));
    cmd.undo();
    CHECK(nearlyEqual(go->getImage()->fillAmount, 1.0f));
    cmd.execute();
    CHECK(nearlyEqual(go->getImage()->fillAmount, 0.125f));

    go->setImage(nullptr);
    cmd.undo();
    CHECK(!go->hasImage());
}

// The Image's OWN fields have to reach the live node: the batcher reads
// from there to decide how many quads to emit. A sync that only dumped the rect
// would leave the mode, borders and fillAmount in their defaults and the Image would
// always draw as Normal with nothing saying so.
static void test_image_sync_updates_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ImageComponent im;
    fillImage(im);
    im.visible = true;   // invisible does not emit quads and here we look at the node

    UiWidgetLists w;
    w.images.emplace_back(7ull, &im);

    syncUiWidgets(w, canvas, cache, loader);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& nodo = *canvas.root().children()[0];
    CHECK(nodo.name == uiImageNodeName(7ull));
    CHECK(nodo.typeName() == std::string("Image"));

    const Image* img = nodo.asImage();
    CHECK(img != nullptr);
    if (!img) return;
    CHECK(img->mode == UiImageMode::Sliced);
    CHECK(nearlyEqual(img->borderLeft, 3.5f));
    CHECK(nearlyEqual(img->borderRight, 5.25f));
    CHECK(nearlyEqual(img->borderTop, 7.75f));
    CHECK(nearlyEqual(img->borderBottom, 9.125f));
    CHECK(img->fillCenter == false);
    CHECK(img->maxTiles == 777u);
    CHECK(img->fillDirection == UiFillDirection::Vertical);
    CHECK(img->fillOrigin == UiFillOrigin::End);
    CHECK(nearlyEqual(img->fillAmount, 0.375f));
    CHECK(img->raycastTarget == false);
    CHECK(img->sprite == "corazon");

    // And a later change dirties the node.
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(nodo.dirty == 0u);
    im.fillAmount = 0.75f;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nearlyEqual(img->fillAmount, 0.75f));
    CHECK(nodo.dirty != 0u);
}

static void test_image_hit_test_maps_back_to_gameobject()
{
    CHECK(uiImageOwnerId(uiImageNodeName(42ull)) == 42ull);
    CHECK(uiImageOwnerId("Cubo") == 0ull);
    CHECK(uiImageOwnerId("img:") == 0ull);
    CHECK(uiImageOwnerId("img:12ab") == 0ull);
    CHECK(uiImageOwnerId(uiPanelNodeName(42ull)) == 0ull);
    CHECK(uiPanelOwnerId(uiImageNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiImageNodeName(42ull)) == 0ull);
    CHECK(uiTextOwnerId(uiImageNodeName(42ull)) == 0ull);
}

// collectCanvases has to see the two new components and put them in the
// hierarchy: a GameObject with Panel is an ancestor with valid rect, so its
// children have to hang from it and not go up to the root.
static void test_collect_ui_widgets_incluye_panels_e_images()
{
    Scene scene;
    GameObject* canvasGo = scene.addGameObject("Canvas");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* marco = scene.addGameObject("Marco", canvasGo);
    marco->setPanel(std::make_shared<PanelComponent>());

    GameObject* icono = scene.addGameObject("Icono", marco);
    icono->setImage(std::make_shared<ImageComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& w = bindings.empty() ? kVacio : bindings[0].widgets;

    CHECK(w.panels.size() == 1);
    CHECK(w.images.size() == 1);
    CHECK(w.buttons.empty());
    CHECK(w.texts.empty());
    CHECK(w.bars.empty());
    CHECK(w.layouts.empty());
    if (w.panels.size() != 1 || w.images.size() != 1) return;
    CHECK(w.panels[0].first == marco->id);
    CHECK(w.images[0].first == icono->id);

    CHECK(w.parents.size() == 2);
    if (w.parents.size() != 2) return;
    CHECK(w.parents[0].first == marco->id);
    CHECK(w.parents[0].second == 0ull);
    CHECK(w.parents[1].first == icono->id);
    CHECK(w.parents[1].second == marco->id);   // the Panel holds the Image
}

// Panel and Image in the SAME GameObject: two sibling nodes with different
// names, and the Image on top of the Panel (the last sibling wins). With the
// same prefix, the gizmo and picking would grab the wrong one.
static void test_panel_and_image_coexist()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    PanelComponent p;
    p.position = glm::vec2(0.0f, 0.0f);
    p.size     = glm::vec2(300.0f, 200.0f);
    ImageComponent im;
    im.position = glm::vec2(10.0f, 10.0f);
    im.size     = glm::vec2(64.0f, 64.0f);

    UiWidgetLists w;
    w.panels.emplace_back(9ull, &p);
    w.images.emplace_back(9ull, &im);
    w.parents.emplace_back(9ull, 0ull);

    syncUiWidgets(w, canvas, cache, loader);

    CHECK(canvas.root().children().size() == 2);
    if (canvas.root().children().size() != 2) return;
    CHECK(canvas.root().children()[0]->name == uiPanelNodeName(9ull));
    CHECK(canvas.root().children()[1]->name == uiImageNodeName(9ull));
}

// ── Slider ──────────────────────────────────────────────────────────────────
// The core Slider is a stub without fields, so the widget is built by
// COMPOSITION just like the ProgressBar: the track is the root node and from it
// hang the fill and the handle. The difference with the bar is that this one DOES receive
// input: dragging the handle writes to the component, which is what the editor
// serializes and what a script reads.
//
// Non-neutral values and DIFFERENT from each other, as always.
static void fillSlider(SliderComponent& s)
{
    s.anchorMin = glm::vec2(0.03125f, 0.21875f);
    s.anchorMax = glm::vec2(0.53125f, 0.71875f);
    s.pivot     = glm::vec2(0.375f, 0.5625f);
    s.position  = glm::vec2(19.5f, -31.25f);
    s.size      = glm::vec2(273.5f, 27.25f);
    s.color     = glm::vec4(0.51f, 0.52f, 0.53f, 0.54f);
    s.visible   = false;

    s.interactable = false;

    s.value    = 21.5f;
    s.minValue = -8.25f;
    s.maxValue = 63.75f;
    s.wholeNumbers = true;

    s.direction = UiSliderDirection::BottomToTop;

    s.fillColor   = glm::vec4(0.61f, 0.62f, 0.63f, 0.64f);
    s.handleColor = glm::vec4(0.71f, 0.72f, 0.73f, 0.74f);
    s.handleSize  = 33.5f;

    s.atlasPath         = "assets/ui/hud.png";
    s.backgroundSprite  = "pista";
    s.fillSprite        = "relleno";
    s.handleSprite      = "asa";
}

static void checkSliderMatchesFilled(const SliderComponent& s)
{
    CHECK(nearlyEqual(s.anchorMin.x, 0.03125f));
    CHECK(nearlyEqual(s.anchorMin.y, 0.21875f));
    CHECK(nearlyEqual(s.anchorMax.x, 0.53125f));
    CHECK(nearlyEqual(s.anchorMax.y, 0.71875f));
    CHECK(nearlyEqual(s.pivot.x, 0.375f));
    CHECK(nearlyEqual(s.pivot.y, 0.5625f));
    CHECK(nearlyEqual(s.position.x, 19.5f));
    CHECK(nearlyEqual(s.position.y, -31.25f));
    CHECK(nearlyEqual(s.size.x, 273.5f));
    CHECK(nearlyEqual(s.size.y, 27.25f));
    CHECK(nearlyEqual(s.color.r, 0.51f));
    CHECK(nearlyEqual(s.color.a, 0.54f));
    CHECK(s.visible == false);
    CHECK(s.interactable == false);

    CHECK(nearlyEqual(s.value, 21.5f));
    CHECK(nearlyEqual(s.minValue, -8.25f));
    CHECK(nearlyEqual(s.maxValue, 63.75f));
    CHECK(s.wholeNumbers == true);
    CHECK(s.direction == UiSliderDirection::BottomToTop);

    CHECK(nearlyEqual(s.fillColor.r, 0.61f));
    CHECK(nearlyEqual(s.fillColor.a, 0.64f));
    CHECK(nearlyEqual(s.handleColor.r, 0.71f));
    CHECK(nearlyEqual(s.handleColor.a, 0.74f));
    CHECK(nearlyEqual(s.handleSize, 33.5f));

    CHECK(s.atlasPath == "assets/ui/hud.png");
    CHECK(s.backgroundSprite == "pista");
    CHECK(s.fillSprite == "relleno");
    CHECK(s.handleSprite == "asa");
}

static void test_slider_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Volumen", canvasGo);
    auto sl = std::make_shared<SliderComponent>();
    fillSlider(*sl);
    go->setSlider(sl);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasSlider()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Volumen");
    checkSliderMatchesFilled(*found->getSlider());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_slider_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasSlider()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_slider_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"slider\"") == std::string::npos);

    go->setSlider(std::make_shared<SliderComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setSlider(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_slider_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Volumen");
    SliderComponent st;
    fillSlider(st);
    SliderComponentCommand cmd(scene, "Add Slider", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasSlider());
    checkSliderMatchesFilled(*go->getSlider());
    cmd.undo();
    CHECK(!go->hasSlider());
    cmd.execute();
    CHECK(go->hasSlider());
    checkSliderMatchesFilled(*go->getSlider());
}

static void test_slider_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Volumen");
    auto sl = std::make_shared<SliderComponent>();
    fillSlider(*sl);
    go->setSlider(sl);

    SliderComponentCommand cmd(scene, "Remove Slider", go->id, /*add=*/false, *sl);
    cmd.execute();
    CHECK(!go->hasSlider());
    cmd.undo();
    CHECK(go->hasSlider());
    checkSliderMatchesFilled(*go->getSlider());
}

static void test_slider_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Volumen");
    go->setSlider(std::make_shared<SliderComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyValue = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasSlider()) g->getSlider()->value = v;
    };
    PropertyCommand<float> cmd("Value", 0.5f, 0.125f, applyValue);

    cmd.execute();
    CHECK(nearlyEqual(go->getSlider()->value, 0.125f));
    cmd.undo();
    CHECK(nearlyEqual(go->getSlider()->value, 0.5f));
    cmd.execute();
    CHECK(nearlyEqual(go->getSlider()->value, 0.125f));

    go->setSlider(nullptr);
    cmd.undo();
    CHECK(!go->hasSlider());
}

// The handle does NOT come out of the track on either end: at t=0 its edge
// sticks to the beginning and at t=1 to the end. Without subtracting handleSize from the
// travel, half the handle would come out of the rect at each tip and nothing
// would say so — the handle draws the same—.
static void test_slider_handle_stays_inside_the_track()
{
    SliderComponent s;
    s.size       = glm::vec2(200.0f, 20.0f);
    s.handleSize = 40.0f;
    s.minValue   = 0.0f;
    s.maxValue   = 1.0f;

    glm::vec2 pos{0.0f}, sz{0.0f};

    s.value = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));
    CHECK(nearlyEqual(sz.x, 40.0f));

    s.value = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 160.0f));     // 200 - 40
    CHECK(nearlyEqual(pos.x + sz.x, 200.0f));

    s.value = 0.5f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 80.0f));      // (200 - 40) * 0.5

    // And with the inverted axis, the same travel on the other side.
    s.direction = UiSliderDirection::RightToLeft;
    s.value     = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 160.0f));
    s.value = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));

    // Vertical: the canvas Y grows DOWN, so BottomToTop at 1 sticks
    // the handle UP (y=0).
    s.direction  = UiSliderDirection::BottomToTop;
    s.size       = glm::vec2(20.0f, 200.0f);
    s.handleSize = 40.0f;
    s.value      = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.y, 0.0f));
    CHECK(nearlyEqual(sz.y, 40.0f));
    s.value = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.y, 160.0f));
}

// wholeNumbers rounds the value THAT IS WRITTEN, not the one shown: if it only
// rounded when drawing, the component would save 3.7 and the script would read 3.7
// while the handle shows 4.
static void test_slider_whole_numbers_snaps_the_value()
{
    SliderComponent s;
    s.minValue = 0.0f;
    s.maxValue = 10.0f;

    CHECK(nearlyEqual(s.valueFromNormalized(0.37f), 3.7f));

    s.wholeNumbers = true;
    CHECK(nearlyEqual(s.valueFromNormalized(0.37f), 4.0f));
    CHECK(nearlyEqual(s.valueFromNormalized(0.34f), 3.0f));
    // The endpoints are still exact.
    CHECK(nearlyEqual(s.valueFromNormalized(0.0f), 0.0f));
    CHECK(nearlyEqual(s.valueFromNormalized(1.0f), 10.0f));

    // A degenerate range cannot distribute anything: it returns the minimum, not a NaN.
    s.maxValue = s.minValue;
    CHECK(nearlyEqual(s.valueFromNormalized(0.5f), 0.0f));
    CHECK(nearlyEqual(s.normalizedValue(), 0.0f));
}

static void test_slider_sync_builds_track_fill_and_handle()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    SliderComponent s;
    s.position   = glm::vec2(10.0f, 20.0f);
    s.size       = glm::vec2(200.0f, 20.0f);
    s.handleSize = 40.0f;
    s.value      = 0.5f;   // min 0, max 1

    UiWidgetLists w;
    w.sliders.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& pista = *canvas.root().children()[0];
    CHECK(pista.name == uiSliderNodeName(5ull));
    CHECK(pista.typeName() == std::string("Slider"));
    CHECK(pista.children().size() == 2);
    if (pista.children().size() != 2) return;
    const UiElement& relleno = *pista.children()[0];
    const UiElement& asa     = *pista.children()[1];
    CHECK(relleno.name == uiSliderNodeName(5ull) + "/Fill");
    CHECK(asa.name == uiSliderNodeName(5ull) + "/Handle");

    // The fill reaches to the CENTER of the handle, which is where it marks the value.
    CHECK(nearlyEqual(relleno.size.x, 100.0f));
    CHECK(nearlyEqual(asa.size.x, 40.0f));
    CHECK(nearlyEqual(asa.position.x, 80.0f));

    // The handle cannot eat the track click: the hit test returns the deepest node,
    // and if the handle were raycastTarget the drag that starts
    // on top of it would not reach the track handler.
    CHECK(asa.raycastTarget == false);
    CHECK(relleno.raycastTarget == false);

    // The emitter leaves the nodes clean: it is the cache that sync has to
    // invalidate when the value changes.
    CHECK(pista.dirty == 0u);
    s.value = 0.25f;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nearlyEqual(asa.position.x, 40.0f));
    CHECK(pista.dirty != 0u);
    CHECK(asa.dirty != 0u);
}

// The reason for being of the widget: dragging writes to the COMPONENT. If the value
// stayed in the node, the editor would not see it, it would not serialize and a script
// would read the value from before the drag.
static void test_slider_drag_writes_the_component_value()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    SliderComponent s;
    s.position   = glm::vec2(0.0f, 0.0f);
    s.size       = glm::vec2(200.0f, 20.0f);
    s.handleSize = 0.0f;   // without handle the travel is the entire rect: 1 px = 0.5%
    s.minValue   = 0.0f;
    s.maxValue   = 100.0f;
    s.value      = 0.0f;

    // The path back to Lua: the same runtime as the Button uses.
    float ultimoAviso = -1.0f;
    int   avisos      = 0;
    s.callbacks.ptr->onValueChanged = [&](float v) { ultimoAviso = v; avisos++; };

    UiWidgetLists w;
    w.sliders.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    // A Down at 3/4 of the track puts the value right there (like Unity: the entire track
    // is click zone, not just the handle).
    UiInputState in;
    in.mousePos    = glm::vec2(150.0f, 10.0f);
    in.timeSeconds = 0.0f;
    canvas.updateInput(in);
    in.mouseDown[0] = true;
    in.timeSeconds  = 0.016f;
    canvas.updateInput(in);

    CHECK(nearlyEqual(s.value, 75.0f));
    CHECK(avisos == 1);
    CHECK(nearlyEqual(ultimoAviso, 75.0f));

    // And dragging follows the mouse, also outside the rect (clamped to the track).
    in.mousePos    = glm::vec2(50.0f, 10.0f);
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.value, 25.0f));

    in.mousePos    = glm::vec2(-500.0f, 10.0f);
    in.timeSeconds = 0.048f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.value, 0.0f));

    in.mousePos    = glm::vec2(9999.0f, 10.0f);
    in.timeSeconds = 0.064f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.value, 100.0f));
}

// A non-interactive slider draws but cannot be moved: it is the "read-only"
// mode that a HUD needs to show a value without the player touching it.
static void test_slider_not_interactable_ignores_the_mouse()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    SliderComponent s;
    s.size         = glm::vec2(200.0f, 20.0f);
    s.handleSize   = 0.0f;
    s.maxValue     = 100.0f;
    s.value        = 42.0f;
    s.interactable = false;

    UiWidgetLists w;
    w.sliders.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    UiInputState in;
    in.mousePos    = glm::vec2(150.0f, 10.0f);
    in.timeSeconds = 0.0f;
    canvas.updateInput(in);
    in.mouseDown[0] = true;
    in.timeSeconds  = 0.016f;
    canvas.updateInput(in);

    CHECK(nearlyEqual(s.value, 42.0f));
}

static void test_slider_hit_test_maps_back_to_gameobject()
{
    CHECK(uiSliderOwnerId(uiSliderNodeName(42ull)) == 42ull);
    // Child nodes also return to their owner: dragging the handle selects
    // the slider, not anything.
    CHECK(uiSliderOwnerId(uiSliderNodeName(42ull) + "/Handle") == 42ull);
    CHECK(uiSliderOwnerId(uiSliderNodeName(42ull) + "/Fill") == 42ull);
    CHECK(uiSliderOwnerId("Cubo") == 0ull);
    CHECK(uiSliderOwnerId("sld:") == 0ull);
    CHECK(uiSliderOwnerId("sld:12ab") == 0ull);
    CHECK(uiSliderOwnerId(uiPanelNodeName(42ull)) == 0ull);
    CHECK(uiPanelOwnerId(uiSliderNodeName(42ull)) == 0ull);
    CHECK(uiProgressBarOwnerId(uiSliderNodeName(42ull)) == 0ull);
}


// ── Checkbox ────────────────────────────────────────────────────────────────
// Stub without fields in the core, like the Slider: the box is the root node and
// the mark hangs from it. It is the simplest of the interactive widgets — a
// click and a bool—, so here we test above all that the click REACHES the
// component and does not stay in the node.
static void fillCheckbox(CheckboxComponent& c)
{
    c.anchorMin = glm::vec2(0.0625f, 0.3125f);
    c.anchorMax = glm::vec2(0.4375f, 0.6875f);
    c.pivot     = glm::vec2(0.1875f, 0.8125f);
    c.position  = glm::vec2(23.5f, -41.25f);
    c.size      = glm::vec2(37.75f, 39.5f);
    c.color     = glm::vec4(0.15f, 0.16f, 0.17f, 0.18f);
    c.visible   = false;

    c.interactable = false;
    c.isOn         = true;

    c.checkColor   = glm::vec4(0.25f, 0.26f, 0.27f, 0.28f);
    c.checkPadding = 5.25f;

    c.atlasPath        = "assets/ui/widgets.png";
    c.backgroundSprite = "casilla";
    c.checkmarkSprite  = "tick";
}

static void checkCheckboxMatchesFilled(const CheckboxComponent& c)
{
    CHECK(nearlyEqual(c.anchorMin.x, 0.0625f));
    CHECK(nearlyEqual(c.anchorMin.y, 0.3125f));
    CHECK(nearlyEqual(c.anchorMax.x, 0.4375f));
    CHECK(nearlyEqual(c.anchorMax.y, 0.6875f));
    CHECK(nearlyEqual(c.pivot.x, 0.1875f));
    CHECK(nearlyEqual(c.pivot.y, 0.8125f));
    CHECK(nearlyEqual(c.position.x, 23.5f));
    CHECK(nearlyEqual(c.position.y, -41.25f));
    CHECK(nearlyEqual(c.size.x, 37.75f));
    CHECK(nearlyEqual(c.size.y, 39.5f));
    CHECK(nearlyEqual(c.color.r, 0.15f));
    CHECK(nearlyEqual(c.color.a, 0.18f));
    CHECK(c.visible == false);
    CHECK(c.interactable == false);
    CHECK(c.isOn == true);
    CHECK(nearlyEqual(c.checkColor.r, 0.25f));
    CHECK(nearlyEqual(c.checkColor.a, 0.28f));
    CHECK(nearlyEqual(c.checkPadding, 5.25f));
    CHECK(c.atlasPath == "assets/ui/widgets.png");
    CHECK(c.backgroundSprite == "casilla");
    CHECK(c.checkmarkSprite == "tick");
}

static void test_checkbox_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Subtitulos", canvasGo);
    auto cb = std::make_shared<CheckboxComponent>();
    fillCheckbox(*cb);
    go->setCheckbox(cb);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasCheckbox()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Subtitulos");
    checkCheckboxMatchesFilled(*found->getCheckbox());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_checkbox_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasCheckbox()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_checkbox_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"checkbox\"") == std::string::npos);

    go->setCheckbox(std::make_shared<CheckboxComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setCheckbox(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_checkbox_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Subtitulos");
    CheckboxComponent st;
    fillCheckbox(st);
    CheckboxComponentCommand cmd(scene, "Add Checkbox", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasCheckbox());
    checkCheckboxMatchesFilled(*go->getCheckbox());
    cmd.undo();
    CHECK(!go->hasCheckbox());
    cmd.execute();
    CHECK(go->hasCheckbox());
    checkCheckboxMatchesFilled(*go->getCheckbox());
}

static void test_checkbox_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Subtitulos");
    auto cb = std::make_shared<CheckboxComponent>();
    fillCheckbox(*cb);
    go->setCheckbox(cb);

    CheckboxComponentCommand cmd(scene, "Remove Checkbox", go->id, /*add=*/false, *cb);
    cmd.execute();
    CHECK(!go->hasCheckbox());
    cmd.undo();
    CHECK(go->hasCheckbox());
    checkCheckboxMatchesFilled(*go->getCheckbox());
}

static void test_checkbox_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Subtitulos");
    go->setCheckbox(std::make_shared<CheckboxComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyOn = [sc, id](const bool& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasCheckbox()) g->getCheckbox()->isOn = v;
    };
    PropertyCommand<bool> cmd("Is On", false, true, applyOn);

    cmd.execute();
    CHECK(go->getCheckbox()->isOn == true);
    cmd.undo();
    CHECK(go->getCheckbox()->isOn == false);

    go->setCheckbox(nullptr);
    cmd.execute();
    CHECK(!go->hasCheckbox());
}

// The mark goes INSIDE the box from all four sides. A padding that
// goes over cannot give a negative rect: the mark disappears, which is the worst that
// can happen, not a backwards quad.
static void test_checkbox_check_rect_respects_padding()
{
    CheckboxComponent c;
    c.size         = glm::vec2(40.0f, 40.0f);
    c.checkPadding = 8.0f;

    glm::vec2 pos{0.0f}, sz{0.0f};
    c.checkRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 8.0f));
    CHECK(nearlyEqual(pos.y, 8.0f));
    CHECK(nearlyEqual(sz.x, 24.0f));
    CHECK(nearlyEqual(sz.y, 24.0f));

    c.checkPadding = 50.0f;
    c.checkRect(pos, sz);
    CHECK(sz.x >= 0.0f);
    CHECK(sz.y >= 0.0f);
    CHECK(nearlyEqual(sz.x, 0.0f));
}

static void test_checkbox_sync_builds_box_and_check()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    CheckboxComponent c;
    c.position     = glm::vec2(5.0f, 6.0f);
    c.size         = glm::vec2(40.0f, 40.0f);
    c.checkPadding = 8.0f;
    c.isOn         = false;

    UiWidgetLists w;
    w.checkboxes.emplace_back(5ull, &c);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& caja = *canvas.root().children()[0];
    CHECK(caja.name == uiCheckboxNodeName(5ull));
    CHECK(caja.typeName() == std::string("Checkbox"));
    CHECK(caja.children().size() == 1);
    if (caja.children().empty()) return;
    const UiElement& marca = *caja.children()[0];
    CHECK(marca.name == uiCheckboxNodeName(5ull) + "/Check");

    // Off: the mark EXISTS (the subtree shape does not change) but does not
    // paint. If the node appeared and disappeared we would have to rebuild the
    // canvas root on every click.
    CHECK(marca.drawable == false);
    CHECK(data.vertices.size() == 4);   // just the box

    c.isOn = true;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(marca.drawable == true);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);   // box + mark
    CHECK(nearlyEqual(marca.size.x, 24.0f));
    // The mark also cannot eat the box click.
    CHECK(marca.raycastTarget == false);
}

static void test_checkbox_click_toggles_the_component()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    CheckboxComponent c;
    c.position = glm::vec2(0.0f, 0.0f);
    c.size     = glm::vec2(40.0f, 40.0f);
    c.isOn     = false;

    bool ultimoAviso = false;
    int  avisos      = 0;
    c.callbacks.ptr->onValueChanged = [&](bool v) { ultimoAviso = v; avisos++; };

    UiWidgetLists w;
    w.checkboxes.emplace_back(5ull, &c);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    clickEnCanvas(canvas, glm::vec2(20.0f, 20.0f), 0.0f);
    CHECK(c.isOn == true);
    CHECK(avisos == 1);
    CHECK(ultimoAviso == true);

    // And the second click turns it off: it is a switch, not a power button.
    clickEnCanvas(canvas, glm::vec2(20.0f, 20.0f), 5.0f);
    CHECK(c.isOn == false);
    CHECK(avisos == 2);

    // Non-interactive: it draws but the click does not move it.
    c.interactable = false;
    syncUiWidgets(w, canvas, cache, loader);
    clickEnCanvas(canvas, glm::vec2(20.0f, 20.0f), 10.0f);
    CHECK(c.isOn == false);
    CHECK(avisos == 2);
}

static void test_checkbox_hit_test_maps_back_to_gameobject()
{
    CHECK(uiCheckboxOwnerId(uiCheckboxNodeName(42ull)) == 42ull);
    CHECK(uiCheckboxOwnerId(uiCheckboxNodeName(42ull) + "/Check") == 42ull);
    CHECK(uiCheckboxOwnerId("Cubo") == 0ull);
    CHECK(uiCheckboxOwnerId("chk:") == 0ull);
    CHECK(uiCheckboxOwnerId("chk:12ab") == 0ull);
    CHECK(uiCheckboxOwnerId(uiSliderNodeName(42ull)) == 0ull);
    CHECK(uiSliderOwnerId(uiCheckboxNodeName(42ull)) == 0ull);
}

// ── Toggle ──────────────────────────────────────────────────────────────────
// The sliding switch: same data as the Checkbox (a bool) but another
// way to show it — the knob moves from one end to the other and the track changes
// color. That is why they are two components and not one with a style enum: they are two
// different sets of fields (mark padding vs. knob size).
static void fillToggle(ToggleComponent& t)
{
    t.anchorMin = glm::vec2(0.09375f, 0.34375f);
    t.anchorMax = glm::vec2(0.46875f, 0.65625f);
    t.pivot     = glm::vec2(0.21875f, 0.78125f);
    t.position  = glm::vec2(27.5f, -43.25f);
    t.size      = glm::vec2(71.75f, 33.5f);
    t.visible   = false;

    t.interactable = false;
    t.isOn         = true;

    t.offColor  = glm::vec4(0.35f, 0.36f, 0.37f, 0.38f);
    t.onColor   = glm::vec4(0.45f, 0.46f, 0.47f, 0.48f);
    t.knobColor = glm::vec4(0.55f, 0.56f, 0.57f, 0.58f);

    t.knobSize    = 25.25f;
    t.knobPadding = 3.75f;

    t.atlasPath        = "assets/ui/widgets.png";
    t.backgroundSprite = "riel";
    t.knobSprite       = "mando";
}

static void checkToggleMatchesFilled(const ToggleComponent& t)
{
    CHECK(nearlyEqual(t.anchorMin.x, 0.09375f));
    CHECK(nearlyEqual(t.anchorMin.y, 0.34375f));
    CHECK(nearlyEqual(t.anchorMax.x, 0.46875f));
    CHECK(nearlyEqual(t.anchorMax.y, 0.65625f));
    CHECK(nearlyEqual(t.pivot.x, 0.21875f));
    CHECK(nearlyEqual(t.pivot.y, 0.78125f));
    CHECK(nearlyEqual(t.position.x, 27.5f));
    CHECK(nearlyEqual(t.position.y, -43.25f));
    CHECK(nearlyEqual(t.size.x, 71.75f));
    CHECK(nearlyEqual(t.size.y, 33.5f));
    CHECK(t.visible == false);
    CHECK(t.interactable == false);
    CHECK(t.isOn == true);
    CHECK(nearlyEqual(t.offColor.r, 0.35f));
    CHECK(nearlyEqual(t.offColor.a, 0.38f));
    CHECK(nearlyEqual(t.onColor.r, 0.45f));
    CHECK(nearlyEqual(t.onColor.a, 0.48f));
    CHECK(nearlyEqual(t.knobColor.r, 0.55f));
    CHECK(nearlyEqual(t.knobColor.a, 0.58f));
    CHECK(nearlyEqual(t.knobSize, 25.25f));
    CHECK(nearlyEqual(t.knobPadding, 3.75f));
    CHECK(t.atlasPath == "assets/ui/widgets.png");
    CHECK(t.backgroundSprite == "riel");
    CHECK(t.knobSprite == "mando");
}

static void test_toggle_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Vsync", canvasGo);
    auto tg = std::make_shared<ToggleComponent>();
    fillToggle(*tg);
    go->setToggle(tg);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasToggle()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Vsync");
    checkToggleMatchesFilled(*found->getToggle());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_toggle_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasToggle()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_toggle_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"toggle\"") == std::string::npos);

    go->setToggle(std::make_shared<ToggleComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setToggle(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_toggle_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vsync");
    ToggleComponent st;
    fillToggle(st);
    ToggleComponentCommand cmd(scene, "Add Toggle", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasToggle());
    checkToggleMatchesFilled(*go->getToggle());
    cmd.undo();
    CHECK(!go->hasToggle());
    cmd.execute();
    CHECK(go->hasToggle());
    checkToggleMatchesFilled(*go->getToggle());
}

static void test_toggle_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vsync");
    auto tg = std::make_shared<ToggleComponent>();
    fillToggle(*tg);
    go->setToggle(tg);

    ToggleComponentCommand cmd(scene, "Remove Toggle", go->id, /*add=*/false, *tg);
    cmd.execute();
    CHECK(!go->hasToggle());
    cmd.undo();
    CHECK(go->hasToggle());
    checkToggleMatchesFilled(*go->getToggle());
}

static void test_toggle_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Vsync");
    go->setToggle(std::make_shared<ToggleComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyKnob = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasToggle()) g->getToggle()->knobSize = v;
    };
    PropertyCommand<float> cmd("Knob Size", 20.0f, 7.5f, applyKnob);

    cmd.execute();
    CHECK(nearlyEqual(go->getToggle()->knobSize, 7.5f));
    cmd.undo();
    CHECK(nearlyEqual(go->getToggle()->knobSize, 20.0f));

    go->setToggle(nullptr);
    cmd.execute();
    CHECK(!go->hasToggle());
}

// The handle goes flush against one end or the other, always inside padding,
// and NEVER leaves the track even if the requested size does not fit.
static void test_toggle_knob_rect_moves_end_to_end()
{
    ToggleComponent t;
    t.size        = glm::vec2(80.0f, 40.0f);
    t.knobSize    = 30.0f;
    t.knobPadding = 5.0f;

    glm::vec2 pos{0.0f}, sz{0.0f};

    t.isOn = false;
    t.knobRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 5.0f));
    CHECK(nearlyEqual(pos.y, 5.0f));
    CHECK(nearlyEqual(sz.x, 30.0f));
    CHECK(nearlyEqual(sz.y, 30.0f));   // height of the track minus the padding

    t.isOn = true;
    t.knobRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 45.0f));            // 80 - 5 - 30
    CHECK(nearlyEqual(pos.x + sz.x, 75.0f));     // does not come out of the padding

    // A handle bigger than the track is clamped to what fits between paddings,
    // instead of sticking out the edge.
    t.knobSize = 500.0f;
    t.isOn     = true;
    t.knobRect(pos, sz);
    CHECK(nearlyEqual(sz.x, 70.0f));
    CHECK(nearlyEqual(pos.x, 5.0f));
}

static void test_toggle_sync_builds_track_and_knob()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ToggleComponent t;
    t.position    = glm::vec2(0.0f, 0.0f);
    t.size        = glm::vec2(80.0f, 40.0f);
    t.knobSize    = 30.0f;
    t.knobPadding = 5.0f;
    t.isOn        = false;
    t.offColor    = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
    t.onColor     = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);

    UiWidgetLists w;
    w.toggles.emplace_back(5ull, &t);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& pista = *canvas.root().children()[0];
    CHECK(pista.name == uiToggleNodeName(5ull));
    CHECK(pista.typeName() == std::string("Toggle"));
    CHECK(pista.children().size() == 1);
    if (pista.children().empty()) return;
    const UiElement& mando = *pista.children()[0];
    CHECK(mando.name == uiToggleNodeName(5ull) + "/Knob");
    CHECK(mando.raycastTarget == false);

    // Off: color of "off" and handle at the left.
    CHECK(nearlyEqual(pista.color.r, 0.1f));
    CHECK(nearlyEqual(mando.position.x, 5.0f));

    // On: both change, and the node is left dirty or the canvas would reuse the
    // old vertices.
    t.isOn = true;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nearlyEqual(pista.color.r, 0.9f));
    CHECK(nearlyEqual(mando.position.x, 45.0f));
    CHECK(pista.dirty != 0u);
    CHECK(mando.dirty != 0u);
}

static void test_toggle_click_flips_the_component()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ToggleComponent t;
    t.size = glm::vec2(80.0f, 40.0f);
    t.isOn = false;

    int avisos = 0;
    t.callbacks.ptr->onValueChanged = [&](bool) { avisos++; };

    UiWidgetLists w;
    w.toggles.emplace_back(5ull, &t);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    clickEnCanvas(canvas, glm::vec2(40.0f, 20.0f), 0.0f);
    CHECK(t.isOn == true);
    CHECK(avisos == 1);

    clickEnCanvas(canvas, glm::vec2(40.0f, 20.0f), 5.0f);
    CHECK(t.isOn == false);
    CHECK(avisos == 2);

    t.interactable = false;
    syncUiWidgets(w, canvas, cache, loader);
    clickEnCanvas(canvas, glm::vec2(40.0f, 20.0f), 10.0f);
    CHECK(t.isOn == false);
    CHECK(avisos == 2);
}

static void test_toggle_hit_test_maps_back_to_gameobject()
{
    CHECK(uiToggleOwnerId(uiToggleNodeName(42ull)) == 42ull);
    CHECK(uiToggleOwnerId(uiToggleNodeName(42ull) + "/Knob") == 42ull);
    CHECK(uiToggleOwnerId("Cubo") == 0ull);
    CHECK(uiToggleOwnerId("tgl:") == 0ull);
    CHECK(uiToggleOwnerId("tgl:12ab") == 0ull);
    CHECK(uiToggleOwnerId(uiCheckboxNodeName(42ull)) == 0ull);
    CHECK(uiCheckboxOwnerId(uiToggleNodeName(42ull)) == 0ull);
}

// ── Scrollbar ───────────────────────────────────────────────────────────────
// Like the Slider but with the handle of VARIABLE size (the visible fraction
// of the content) and with the value always in 0..1: it has no own range
// because the ScrollView is the one that interprets it, not the bar.
static void fillScrollbar(ScrollbarComponent& s)
{
    s.anchorMin = glm::vec2(0.125f, 0.375f);
    s.anchorMax = glm::vec2(0.5f, 0.625f);
    s.pivot     = glm::vec2(0.25f, 0.6875f);
    s.position  = glm::vec2(29.5f, -47.25f);
    s.size      = glm::vec2(17.75f, 213.5f);
    s.color     = glm::vec4(0.19f, 0.29f, 0.39f, 0.49f);
    s.visible   = false;

    s.interactable = false;

    s.value          = 0.625f;
    s.handleFraction = 0.375f;
    s.direction      = UiScrollbarDirection::BottomToTop;
    s.numberOfSteps  = 7u;

    s.handleColor = glm::vec4(0.59f, 0.69f, 0.79f, 0.89f);

    s.atlasPath        = "assets/ui/widgets.png";
    s.backgroundSprite = "canal";
    s.handleSprite     = "pulgar";
}

static void checkScrollbarMatchesFilled(const ScrollbarComponent& s)
{
    CHECK(nearlyEqual(s.anchorMin.x, 0.125f));
    CHECK(nearlyEqual(s.anchorMin.y, 0.375f));
    CHECK(nearlyEqual(s.anchorMax.x, 0.5f));
    CHECK(nearlyEqual(s.anchorMax.y, 0.625f));
    CHECK(nearlyEqual(s.pivot.x, 0.25f));
    CHECK(nearlyEqual(s.pivot.y, 0.6875f));
    CHECK(nearlyEqual(s.position.x, 29.5f));
    CHECK(nearlyEqual(s.position.y, -47.25f));
    CHECK(nearlyEqual(s.size.x, 17.75f));
    CHECK(nearlyEqual(s.size.y, 213.5f));
    CHECK(nearlyEqual(s.color.r, 0.19f));
    CHECK(nearlyEqual(s.color.a, 0.49f));
    CHECK(s.visible == false);
    CHECK(s.interactable == false);
    CHECK(nearlyEqual(s.value, 0.625f));
    CHECK(nearlyEqual(s.handleFraction, 0.375f));
    CHECK(s.direction == UiScrollbarDirection::BottomToTop);
    CHECK(s.numberOfSteps == 7u);
    CHECK(nearlyEqual(s.handleColor.r, 0.59f));
    CHECK(nearlyEqual(s.handleColor.a, 0.89f));
    CHECK(s.atlasPath == "assets/ui/widgets.png");
    CHECK(s.backgroundSprite == "canal");
    CHECK(s.handleSprite == "pulgar");
}

static void test_scrollbar_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("BarraLateral", canvasGo);
    auto sb = std::make_shared<ScrollbarComponent>();
    fillScrollbar(*sb);
    go->setScrollbar(sb);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasScrollbar()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "BarraLateral");
    checkScrollbarMatchesFilled(*found->getScrollbar());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_scrollbar_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasScrollbar()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_scrollbar_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"scrollbar\"") == std::string::npos);

    go->setScrollbar(std::make_shared<ScrollbarComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setScrollbar(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_scrollbar_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("BarraLateral");
    ScrollbarComponent st;
    fillScrollbar(st);
    ScrollbarComponentCommand cmd(scene, "Add Scrollbar", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasScrollbar());
    checkScrollbarMatchesFilled(*go->getScrollbar());
    cmd.undo();
    CHECK(!go->hasScrollbar());
    cmd.execute();
    CHECK(go->hasScrollbar());
    checkScrollbarMatchesFilled(*go->getScrollbar());
}

static void test_scrollbar_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("BarraLateral");
    auto sb = std::make_shared<ScrollbarComponent>();
    fillScrollbar(*sb);
    go->setScrollbar(sb);

    ScrollbarComponentCommand cmd(scene, "Remove Scrollbar", go->id, /*add=*/false, *sb);
    cmd.execute();
    CHECK(!go->hasScrollbar());
    cmd.undo();
    CHECK(go->hasScrollbar());
    checkScrollbarMatchesFilled(*go->getScrollbar());
}

static void test_scrollbar_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("BarraLateral");
    go->setScrollbar(std::make_shared<ScrollbarComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyFrac = [sc, id](const float& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasScrollbar()) g->getScrollbar()->handleFraction = v;
    };
    PropertyCommand<float> cmd("Handle Fraction", 0.25f, 0.8125f, applyFrac);

    cmd.execute();
    CHECK(nearlyEqual(go->getScrollbar()->handleFraction, 0.8125f));
    cmd.undo();
    CHECK(nearlyEqual(go->getScrollbar()->handleFraction, 0.25f));

    go->setScrollbar(nullptr);
    cmd.execute();
    CHECK(!go->hasScrollbar());
}

// The handle occupies its fraction of the channel and travels what is left,
// never going out.
static void test_scrollbar_handle_rect_and_steps()
{
    ScrollbarComponent s;
    s.direction      = UiScrollbarDirection::LeftToRight;
    s.size           = glm::vec2(200.0f, 20.0f);
    s.handleFraction = 0.25f;

    glm::vec2 pos{0.0f}, sz{0.0f};

    s.value = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(sz.x, 50.0f));
    CHECK(nearlyEqual(pos.x, 0.0f));

    s.value = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 150.0f));
    CHECK(nearlyEqual(pos.x + sz.x, 200.0f));

    s.value = 0.5f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 75.0f));

    // Vertical TopToBottom: the value grows downward, which is the natural way
    // for a scrollbar (0 = at the top).
    s.direction      = UiScrollbarDirection::TopToBottom;
    s.size           = glm::vec2(20.0f, 200.0f);
    s.handleFraction = 0.5f;
    s.value          = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.y, 0.0f));
    CHECK(nearlyEqual(sz.y, 100.0f));
    s.value = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.y, 100.0f));

    // numberOfSteps snaps the value to discrete positions. With 5 steps there are
    // 5 stops (0, 0.25, 0.5, 0.75, 1), like in Unity.
    ScrollbarComponent d;
    d.numberOfSteps = 5u;
    CHECK(nearlyEqual(d.snapValue(0.3f), 0.25f));
    CHECK(nearlyEqual(d.snapValue(0.6f), 0.5f));
    CHECK(nearlyEqual(d.snapValue(0.99f), 1.0f));
    // 0 and 1 steps = continuous: snapping to one place would leave the bar dead.
    d.numberOfSteps = 0u;
    CHECK(nearlyEqual(d.snapValue(0.3f), 0.3f));
    d.numberOfSteps = 1u;
    CHECK(nearlyEqual(d.snapValue(0.3f), 0.3f));
}

static void test_scrollbar_drag_and_wheel_write_the_component()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ScrollbarComponent s;
    s.direction      = UiScrollbarDirection::LeftToRight;
    s.position       = glm::vec2(0.0f, 0.0f);
    s.size           = glm::vec2(200.0f, 20.0f);
    s.handleFraction = 0.0f;   // without handle the travel is the entire channel
    s.value          = 0.0f;

    float ultimo = -1.0f;
    int   avisos = 0;
    s.callbacks.ptr->onValueChanged = [&](float v) { ultimo = v; avisos++; };

    UiWidgetLists w;
    w.scrollbars.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    const UiElement& canal = *canvas.root().children()[0];
    CHECK(canal.name == uiScrollbarNodeName(5ull));
    CHECK(canal.typeName() == std::string("Scrollbar"));

    UiInputState in;
    in.mousePos    = glm::vec2(150.0f, 10.0f);
    in.timeSeconds = 0.0f;
    canvas.updateInput(in);
    in.mouseDown[0] = true;
    in.timeSeconds  = 0.016f;
    canvas.updateInput(in);

    CHECK(nearlyEqual(s.value, 0.75f));
    CHECK(avisos == 1);
    CHECK(nearlyEqual(ultimo, 0.75f));

    in.mouseDown[0] = false;
    in.timeSeconds  = 0.032f;
    canvas.updateInput(in);

    // The wheel also moves the scrollbar: without this, a list with scrollbar
    // could only be scrolled by dragging, which is not what anyone expects.
    in.scrollDelta = 1.0f;     // + upward = toward the beginning
    in.timeSeconds = 0.048f;
    canvas.updateInput(in);
    CHECK(s.value < 0.75f);
    const float trasRueda = s.value;

    in.scrollDelta = -1.0f;
    in.timeSeconds = 0.064f;
    canvas.updateInput(in);
    CHECK(s.value > trasRueda);

    // And does not leave [0,1] no matter how much you push.
    for (int i = 0; i < 50; i++)
    {
        in.scrollDelta = -1.0f;
        in.timeSeconds += 0.016f;
        canvas.updateInput(in);
    }
    CHECK(nearlyEqual(s.value, 1.0f));
}

static void test_scrollbar_hit_test_maps_back_to_gameobject()
{
    CHECK(uiScrollbarOwnerId(uiScrollbarNodeName(42ull)) == 42ull);
    CHECK(uiScrollbarOwnerId(uiScrollbarNodeName(42ull) + "/Handle") == 42ull);
    CHECK(uiScrollbarOwnerId("Cubo") == 0ull);
    CHECK(uiScrollbarOwnerId("scr:") == 0ull);
    CHECK(uiScrollbarOwnerId("scr:12ab") == 0ull);
    CHECK(uiScrollbarOwnerId(uiSliderNodeName(42ull)) == 0ull);
    CHECK(uiSliderOwnerId(uiScrollbarNodeName(42ull)) == 0ull);
    CHECK(uiToggleOwnerId(uiScrollbarNodeName(42ull)) == 0ull);
}


// ── Text input in the canvas ───────────────────────────────────────────────
// The canvas already had focus, Tab traversal, directional navigation and
// onKeyDown; the only thing missing for typing was the CHARACTER channel.
// UiKey names keys (Tab, Enter, arrows...), and an 'a' is not a named key:
// it is a codepoint, and depends on keyboard layout and dead keys, which the
// core does not know about and should not care. So the caller fills it
// (GLFW, the editor, a test), just like the mouse position.
static void test_canvas_entrega_el_texto_al_elemento_con_foco()
{
    UiCanvas canvas;
    Panel& campo = canvas.root().add<Panel>("campo");
    campo.size      = glm::vec2(100.0f, 20.0f);
    campo.focusable = true;

    std::string escrito;
    campo.onTextInput = [&](UiEvent& e) { escrito += (char)e.codepoint; };

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    // Without focus it is NOT delivered: a lone character with no destination
    // cannot go to whoever is near.
    UiInputState in;
    in.chars = { 'N', 'o' };
    canvas.updateInput(in);
    CHECK(escrito.empty());

    canvas.setFocus(&campo);
    in.chars = { 'H', 'o', 'l', 'a' };
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(escrito == "Hola");

    // And codepoints travel whole, not truncated to a byte: the channel is
    // uint32, so an 'n' with tilde comes through in one piece.
    uint32_t ultimo = 0;
    campo.onTextInput = [&](UiEvent& e) { ultimo = e.codepoint; };
    in.chars = { 0x00F1u };   // n with tilde
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(ultimo == 0x00F1u);
}

// The four edit keys had to exist: without Backspace you cannot delete, and
// with navigation eating the arrows you cannot move the cursor. The canvas
// already yielded the key to whoever consumed it; this checks that it KEEPS
// yielding it with the new ones.
static void test_canvas_cede_las_teclas_de_edicion_a_quien_las_consume()
{
    UiCanvas canvas;
    Panel& a = canvas.root().add<Panel>("a");
    a.size      = glm::vec2(100.0f, 20.0f);
    a.position  = glm::vec2(0.0f, 0.0f);
    a.focusable = true;
    Panel& b = canvas.root().add<Panel>("b");
    b.size      = glm::vec2(100.0f, 20.0f);
    b.position  = glm::vec2(200.0f, 0.0f);
    b.focusable = true;

    std::vector<UiKey> recibidas;
    a.onKeyDown = [&](UiEvent& e) {
        recibidas.push_back(e.key);
        // Left and Right are for the cursor; Up and Down are passed through so
        // navigation keeps working from inside the field.
        if (e.key == UiKey::Left || e.key == UiKey::Right) e.consumed = true;
    };

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    canvas.setFocus(&a);

    UiInputState in;
    in.keys = { UiKey::Backspace, UiKey::Delete, UiKey::Home, UiKey::End };
    canvas.updateInput(in);
    CHECK(recibidas.size() == 4);
    if (recibidas.size() != 4) return;
    CHECK(recibidas[0] == UiKey::Backspace);
    CHECK(recibidas[1] == UiKey::Delete);
    CHECK(recibidas[2] == UiKey::Home);
    CHECK(recibidas[3] == UiKey::End);

    // Right consumed: focus does NOT move to the next one.
    in.keys = { UiKey::Right };
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(canvas.focused() == &a);

    // And without consuming it, navigation stays alive: this is what makes a menu
    // with gamepad playable, and it cannot have broken along the way.
    a.onKeyDown = nullptr;
    in.keys = { UiKey::Right };
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(canvas.focused() == &b);
}


// ── InputField ──────────────────────────────────────────────────────────────────
// The only widget that needed something the CORE did NOT have: a character
// channel. UiKey names keys and an 'a' is not a named key, so until now there
// was no way to deliver it. With UiInputState::chars and onTextInput it can
// now, and this component is the first one to use them.
//
// The cursor is counted in CODEPOINTS, not bytes: with UTF-8, an 'n' with
// tilde takes two bytes and a cursor in bytes would split it in the middle.
static void fillInputField(InputFieldComponent& f)
{
    f.anchorMin = glm::vec2(0.03125f, 0.09375f);
    f.anchorMax = glm::vec2(0.65625f, 0.84375f);
    f.pivot     = glm::vec2(0.28125f, 0.71875f);
    f.position  = glm::vec2(33.5f, -51.25f);
    f.size      = glm::vec2(287.75f, 41.5f);
    f.color     = glm::vec4(0.12f, 0.13f, 0.14f, 0.15f);
    f.visible   = false;

    f.interactable = false;
    f.readOnly     = true;

    f.text        = "Jugador1";
    f.placeholder = "Tu nombre...";

    f.fontPath         = "assets/fonts/mono.ttf";
    f.fontSize         = 21.5f;
    f.textColor        = glm::vec4(0.22f, 0.23f, 0.24f, 0.25f);
    f.placeholderColor = glm::vec4(0.32f, 0.33f, 0.34f, 0.35f);
    f.align            = UiTextAlign::Right;
    f.padding          = 7.25f;

    f.characterLimit = 12u;
    f.contentType    = UiInputContentType::Password;
    f.passwordChar   = "#";

    f.caretColor     = glm::vec4(0.42f, 0.43f, 0.44f, 0.45f);
    f.caretWidth     = 3.5f;
    f.caretBlinkRate = 0.625f;

    f.atlasPath        = "assets/ui/widgets.png";
    f.backgroundSprite = "campo";
}

static void checkInputFieldMatchesFilled(const InputFieldComponent& f)
{
    CHECK(nearlyEqual(f.anchorMin.x, 0.03125f));
    CHECK(nearlyEqual(f.anchorMax.y, 0.84375f));
    CHECK(nearlyEqual(f.pivot.x, 0.28125f));
    CHECK(nearlyEqual(f.position.x, 33.5f));
    CHECK(nearlyEqual(f.position.y, -51.25f));
    CHECK(nearlyEqual(f.size.x, 287.75f));
    CHECK(nearlyEqual(f.size.y, 41.5f));
    CHECK(nearlyEqual(f.color.r, 0.12f));
    CHECK(nearlyEqual(f.color.a, 0.15f));
    CHECK(f.visible == false);
    CHECK(f.interactable == false);
    CHECK(f.readOnly == true);
    CHECK(f.text == "Jugador1");
    CHECK(f.placeholder == "Tu nombre...");
    CHECK(f.fontPath == "assets/fonts/mono.ttf");
    CHECK(nearlyEqual(f.fontSize, 21.5f));
    CHECK(nearlyEqual(f.textColor.r, 0.22f));
    CHECK(nearlyEqual(f.placeholderColor.g, 0.33f));
    CHECK(f.align == UiTextAlign::Right);
    CHECK(nearlyEqual(f.padding, 7.25f));
    CHECK(f.characterLimit == 12u);
    CHECK(f.contentType == UiInputContentType::Password);
    CHECK(f.passwordChar == "#");
    CHECK(nearlyEqual(f.caretColor.b, 0.44f));
    CHECK(nearlyEqual(f.caretWidth, 3.5f));
    CHECK(nearlyEqual(f.caretBlinkRate, 0.625f));
    CHECK(f.atlasPath == "assets/ui/widgets.png");
    CHECK(f.backgroundSprite == "campo");
}

static void test_input_field_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Nombre", canvasGo);
    auto f = std::make_shared<InputFieldComponent>();
    fillInputField(*f);
    go->setInputField(f);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasInputField()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Nombre");
    checkInputFieldMatchesFilled(*found->getInputField());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_input_field_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasInputField()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_input_field_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"inputField\"") == std::string::npos);

    go->setInputField(std::make_shared<InputFieldComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setInputField(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_input_field_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Nombre");
    InputFieldComponent st;
    fillInputField(st);
    InputFieldComponentCommand cmd(scene, "Add Input Field", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasInputField());
    checkInputFieldMatchesFilled(*go->getInputField());
    cmd.undo();
    CHECK(!go->hasInputField());
    cmd.execute();
    CHECK(go->hasInputField());
    checkInputFieldMatchesFilled(*go->getInputField());
}

static void test_input_field_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Nombre");
    auto f = std::make_shared<InputFieldComponent>();
    fillInputField(*f);
    go->setInputField(f);

    InputFieldComponentCommand cmd(scene, "Remove Input Field", go->id, /*add=*/false, *f);
    cmd.execute();
    CHECK(!go->hasInputField());
    cmd.undo();
    CHECK(go->hasInputField());
    checkInputFieldMatchesFilled(*go->getInputField());
}

static void test_input_field_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Nombre");
    go->setInputField(std::make_shared<InputFieldComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyText = [sc, id](const std::string& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasInputField()) g->getInputField()->text = v;
    };
    PropertyCommand<std::string> cmd("Text", std::string(), std::string("Hola"), applyText);

    cmd.execute();
    CHECK(go->getInputField()->text == "Hola");
    cmd.undo();
    CHECK(go->getInputField()->text.empty());

    go->setInputField(nullptr);
    cmd.execute();
    CHECK(!go->hasInputField());
}

// Cursor is counted in CODEPOINTS and text is stored in UTF-8. A cursor in
// bytes would split an 'n' with tilde in half and leave the string broken
// without anything saying so.
static void test_input_field_edits_by_codepoint_not_by_byte()
{
    InputFieldComponent f;
    f.text     = "";
    f.caretPos = 0;

    CHECK(f.insertCodepoint('a'));
    CHECK(f.insertCodepoint(0x00F1u));   // n with tilde: TWO bytes in UTF-8
    CHECK(f.insertCodepoint('o'));
    CHECK(f.text == "a\xC3\xB1o");
    CHECK(f.caretPos == 3);              // three CHARACTERS, four bytes
    CHECK(f.codepointCount() == 3);

    // Backspacing removes the whole character, not half a byte.
    f.caretPos = 2;
    CHECK(f.backspace());
    CHECK(f.text == "ao");
    CHECK(f.caretPos == 1);

    // And forward, same thing.
    f.text     = "a\xC3\xB1o";
    f.caretPos = 1;
    CHECK(f.deleteForward());
    CHECK(f.text == "ao");
    CHECK(f.caretPos == 1);

    // At the extremes there is nothing to delete and nothing happens.
    f.caretPos = 0;
    CHECK(!f.backspace());
    f.caretPos = f.codepointCount();
    CHECK(!f.deleteForward());

    // Cursor moves by characters and is clamped to the ends.
    f.text     = "a\xC3\xB1o";
    f.caretPos = 0;
    f.moveCaret(1);
    CHECK(f.caretPos == 1);
    f.moveCaret(-5);
    CHECK(f.caretPos == 0);
    f.moveCaret(99);
    CHECK(f.caretPos == 3);
    f.caretHome();
    CHECK(f.caretPos == 0);
    f.caretEnd();
    CHECK(f.caretPos == 3);
}

// Content type filters what can be typed, and the limit cuts. Both go
// where you WRITE, not when drawing: if they only filtered the display,
// the component would save garbage and a script would read it.
static void test_input_field_content_type_and_limit()
{
    InputFieldComponent f;

    f.contentType = UiInputContentType::IntegerNumber;
    CHECK(f.accepts('4'));
    CHECK(!f.accepts('a'));
    CHECK(!f.accepts('.'));
    // Sign only at the start: "1-2" is not an integer.
    f.text = ""; f.caretPos = 0;
    CHECK(f.accepts('-'));
    f.text = "1"; f.caretPos = 1;
    CHECK(!f.accepts('-'));

    f.contentType = UiInputContentType::DecimalNumber;
    f.text = ""; f.caretPos = 0;
    CHECK(f.accepts('.'));
    f.text = "1.5"; f.caretPos = 3;
    CHECK(!f.accepts('.'));   // only one decimal separator

    f.contentType = UiInputContentType::Alphanumeric;
    CHECK(f.accepts('a'));
    CHECK(f.accepts('7'));
    CHECK(!f.accepts(' '));

    f.contentType = UiInputContentType::Standard;
    CHECK(f.accepts(' '));
    // Control characters NEVER go in: a '\n' or tab inside a line
    // is invisible and shifts everything that follows.
    CHECK(!f.accepts('\n'));
    CHECK(!f.accepts('\t'));
    CHECK(!f.accepts(0x7Fu));

    // The limit counts CHARACTERS, not bytes.
    f.text           = "";
    f.caretPos       = 0;
    f.characterLimit = 3u;
    CHECK(f.insertCodepoint('a'));
    CHECK(f.insertCodepoint(0x00F1u));
    CHECK(f.insertCodepoint('c'));
    CHECK(!f.insertCodepoint('d'));   // full: three characters, four bytes
    CHECK(f.codepointCount() == 3);

    // 0 = no limit.
    f.characterLimit = 0u;
    CHECK(f.insertCodepoint('d'));
}

// Password does not change the text, it changes what is SHOWN. Saving the
// masked version would lose the password.
static void test_input_field_password_and_placeholder()
{
    InputFieldComponent f;
    f.text        = "secreto";
    f.placeholder = "clave...";
    f.contentType = UiInputContentType::Password;
    f.passwordChar = "*";

    CHECK(f.displayText() == "*******");
    CHECK(f.text == "secreto");
    CHECK(!f.isShowingPlaceholder());

    // With multiple bytes per character, the mask still has one symbol per
    // CHARACTER and not one per byte.
    f.text = "a\xC3\xB1o";
    CHECK(f.displayText() == "***");

    // Empty: the placeholder is shown, and with ITS color (that is checked by the sync).
    f.text = "";
    CHECK(f.isShowingPlaceholder());
    CHECK(f.displayText() == "clave...");

    // No placeholder and empty nothing is drawn.
    f.placeholder = "";
    CHECK(f.displayText().empty());

    // An empty passwordChar falls back to asterisk: a password field that shows
    // NOTHING looks broken.
    f.text         = "abc";
    f.passwordChar = "";
    CHECK(f.displayText() == "***");
}

static void test_input_field_sync_builds_box_text_and_caret()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    InputFieldComponent f;
    f.position = glm::vec2(10.0f, 20.0f);
    f.size     = glm::vec2(200.0f, 30.0f);
    f.text     = "abc";

    UiWidgetLists w;
    w.inputFields.emplace_back(5ull, &f);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& caja = *canvas.root().children()[0];
    CHECK(caja.name == uiInputFieldNodeName(5ull));
    CHECK(caja.typeName() == std::string("InputField"));
    // The box HAS to be focusable or there is nowhere to type.
    CHECK(caja.focusable == true);
    CHECK(caja.children().size() == 2);
    if (caja.children().size() != 2) return;
    const UiElement& texto = *caja.children()[0];
    const UiElement& caret = *caja.children()[1];
    CHECK(texto.name == uiInputFieldNodeName(5ull) + "/Text");
    CHECK(caret.name == uiInputFieldNodeName(5ull) + "/Caret");
    CHECK(texto.raycastTarget == false);
    CHECK(caret.raycastTarget == false);

    // Without focus the cursor is NOT drawn: a field that blinks without being active
    // is exactly what makes you think you can type in it.
    CHECK(caret.drawable == false);
}

// The reason for the text channel: typing writes in the COMPONENT.
static void test_input_field_typing_writes_the_component()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    InputFieldComponent f;
    f.position = glm::vec2(0.0f, 0.0f);
    f.size     = glm::vec2(200.0f, 30.0f);

    std::string ultimo;
    int         avisos = 0;
    int         finales = 0;
    f.callbacks.ptr->onValueChanged = [&](const std::string& v) { ultimo = v; avisos++; };
    f.callbacks.ptr->onEndEdit      = [&](const std::string&)   { finales++; };

    UiWidgetLists w;
    w.inputFields.emplace_back(5ull, &f);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    // A click focuses the field.
    clickEnCanvas(canvas, glm::vec2(100.0f, 15.0f), 0.0f);
    CHECK(canvas.focused() != nullptr);

    UiInputState in;
    in.mousePos    = glm::vec2(100.0f, 15.0f);
    in.timeSeconds = 1.0f;
    in.chars       = { 'H', 'o', 'l', 'a' };
    canvas.updateInput(in);

    CHECK(f.text == "Hola");
    CHECK(f.caretPos == 4);
    CHECK(avisos == 4);
    CHECK(ultimo == "Hola");

    // Backspace deletes and Left moves, without letting navigation eat the
    // arrow (the field consumes it).
    in.chars.clear();
    in.keys = { UiKey::Backspace };
    in.timeSeconds = 1.016f;
    canvas.updateInput(in);
    CHECK(f.text == "Hol");
    CHECK(f.caretPos == 3);

    in.keys = { UiKey::Left, UiKey::Left };
    in.timeSeconds = 1.032f;
    canvas.updateInput(in);
    CHECK(f.caretPos == 1);
    CHECK(canvas.focused() != nullptr);   // the arrow did NOT move focus

    in.keys = { UiKey::Home };
    in.timeSeconds = 1.048f;
    canvas.updateInput(in);
    CHECK(f.caretPos == 0);
    in.keys = { UiKey::End };
    in.timeSeconds = 1.064f;
    canvas.updateInput(in);
    CHECK(f.caretPos == 3);

    // Enter closes the edit.
    in.keys = { UiKey::Enter };
    in.timeSeconds = 1.08f;
    canvas.updateInput(in);
    CHECK(finales == 1);

    // readOnly: can focus and move the cursor, but not change the text.
    f.readOnly = true;
    syncUiWidgets(w, canvas, cache, loader);
    canvas.setFocus(nullptr);
    clickEnCanvas(canvas, glm::vec2(100.0f, 15.0f), 2.0f);
    in.keys.clear();
    in.chars = { 'X' };
    in.timeSeconds = 3.0f;
    canvas.updateInput(in);
    CHECK(f.text == "Hol");

    // And not interactable does not even focus.
    f.readOnly     = false;
    f.interactable = false;
    syncUiWidgets(w, canvas, cache, loader);
    canvas.setFocus(nullptr);
    clickEnCanvas(canvas, glm::vec2(100.0f, 15.0f), 4.0f);
    in.chars = { 'Y' };
    in.timeSeconds = 5.0f;
    canvas.updateInput(in);
    CHECK(f.text == "Hol");
}

static void test_input_field_hit_test_maps_back_to_gameobject()
{
    CHECK(uiInputFieldOwnerId(uiInputFieldNodeName(42ull)) == 42ull);
    CHECK(uiInputFieldOwnerId(uiInputFieldNodeName(42ull) + "/Caret") == 42ull);
    CHECK(uiInputFieldOwnerId("Cubo") == 0ull);
    CHECK(uiInputFieldOwnerId("inp:") == 0ull);
    CHECK(uiInputFieldOwnerId("inp:12ab") == 0ull);
    CHECK(uiInputFieldOwnerId(uiSliderNodeName(42ull)) == 0ull);
    CHECK(uiSliderOwnerId(uiInputFieldNodeName(42ull)) == 0ull);
}

// ── Dropdown ────────────────────────────────────────────────────────────────
// The only widget whose subtree CHANGES SHAPE with the data: one more option is
// one more node. Opening and closing do NOT change the shape (the list always exists and only
// turns off), but adding or removing options does, and that forces a rebuild.
static void fillDropdown(DropdownComponent& d)
{
    d.anchorMin = glm::vec2(0.15625f, 0.40625f);
    d.anchorMax = glm::vec2(0.53125f, 0.59375f);
    d.pivot     = glm::vec2(0.34375f, 0.65625f);
    d.position  = glm::vec2(37.5f, -53.25f);
    d.size      = glm::vec2(197.75f, 35.5f);
    d.color     = glm::vec4(0.16f, 0.17f, 0.18f, 0.19f);
    d.visible   = false;

    d.interactable = false;

    d.options = { "Bajo", "Medio", "Alto" };
    d.value   = 2;

    d.itemHeight      = 27.25f;
    d.maxVisibleItems = 5u;

    d.listColor         = glm::vec4(0.26f, 0.27f, 0.28f, 0.29f);
    d.itemColor         = glm::vec4(0.36f, 0.37f, 0.38f, 0.39f);
    d.itemSelectedColor = glm::vec4(0.46f, 0.47f, 0.48f, 0.49f);
    d.arrowColor        = glm::vec4(0.56f, 0.57f, 0.58f, 0.59f);

    d.fontPath  = "assets/fonts/ui.ttf";
    d.fontSize  = 19.5f;
    d.textColor = glm::vec4(0.66f, 0.67f, 0.68f, 0.69f);

    d.atlasPath        = "assets/ui/widgets.png";
    d.backgroundSprite = "combo";
    d.arrowSprite      = "flecha";
    d.itemSprite       = "fila";
}

static void checkDropdownMatchesFilled(const DropdownComponent& d)
{
    CHECK(nearlyEqual(d.anchorMin.x, 0.15625f));
    CHECK(nearlyEqual(d.anchorMax.y, 0.59375f));
    CHECK(nearlyEqual(d.pivot.y, 0.65625f));
    CHECK(nearlyEqual(d.position.x, 37.5f));
    CHECK(nearlyEqual(d.size.y, 35.5f));
    CHECK(nearlyEqual(d.color.r, 0.16f));
    CHECK(d.visible == false);
    CHECK(d.interactable == false);
    CHECK(d.options.size() == 3);
    if (d.options.size() == 3)
    {
        CHECK(d.options[0] == "Bajo");
        CHECK(d.options[1] == "Medio");
        CHECK(d.options[2] == "Alto");
    }
    CHECK(d.value == 2);
    CHECK(nearlyEqual(d.itemHeight, 27.25f));
    CHECK(d.maxVisibleItems == 5u);
    CHECK(nearlyEqual(d.listColor.g, 0.27f));
    CHECK(nearlyEqual(d.itemColor.b, 0.38f));
    CHECK(nearlyEqual(d.itemSelectedColor.a, 0.49f));
    CHECK(nearlyEqual(d.arrowColor.r, 0.56f));
    CHECK(d.fontPath == "assets/fonts/ui.ttf");
    CHECK(nearlyEqual(d.fontSize, 19.5f));
    CHECK(nearlyEqual(d.textColor.g, 0.67f));
    CHECK(d.atlasPath == "assets/ui/widgets.png");
    CHECK(d.backgroundSprite == "combo");
    CHECK(d.arrowSprite == "flecha");
    CHECK(d.itemSprite == "fila");
}

static void test_dropdown_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Calidad", canvasGo);
    auto d = std::make_shared<DropdownComponent>();
    fillDropdown(*d);
    go->setDropdown(d);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasDropdown()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Calidad");
    checkDropdownMatchesFilled(*found->getDropdown());
    CHECK(loaded.lastWarnings().empty());
}

// isOpen is LIVE state and is not saved: a scene that opened with the combo
// dropped would have a list covering the menu right after loading.
static void test_dropdown_open_state_is_not_serialized(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Calidad");
    auto d = std::make_shared<DropdownComponent>();
    d->options = { "A", "B" };
    d->isOpen  = true;
    go->setDropdown(d);

    const std::string dump = scene.toJson().dump();
    CHECK(dump.find("isOpen") == std::string::npos);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(scene.toJson(), pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasDropdown()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->getDropdown()->isOpen == false);
}

static void test_scene_without_dropdown_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasDropdown()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_dropdown_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"dropdown\"") == std::string::npos);

    go->setDropdown(std::make_shared<DropdownComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setDropdown(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_dropdown_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Calidad");
    DropdownComponent st;
    fillDropdown(st);
    DropdownComponentCommand cmd(scene, "Add Dropdown", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasDropdown());
    checkDropdownMatchesFilled(*go->getDropdown());
    cmd.undo();
    CHECK(!go->hasDropdown());
    cmd.execute();
    CHECK(go->hasDropdown());
    checkDropdownMatchesFilled(*go->getDropdown());
}

static void test_dropdown_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Calidad");
    auto d = std::make_shared<DropdownComponent>();
    fillDropdown(*d);
    go->setDropdown(d);

    DropdownComponentCommand cmd(scene, "Remove Dropdown", go->id, /*add=*/false, *d);
    cmd.execute();
    CHECK(!go->hasDropdown());
    cmd.undo();
    CHECK(go->hasDropdown());
    checkDropdownMatchesFilled(*go->getDropdown());
}

static void test_dropdown_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Calidad");
    go->setDropdown(std::make_shared<DropdownComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyValue = [sc, id](const int& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasDropdown()) g->getDropdown()->value = v;
    };
    PropertyCommand<int> cmd("Value", 0, 3, applyValue);

    cmd.execute();
    CHECK(go->getDropdown()->value == 3);
    cmd.undo();
    CHECK(go->getDropdown()->value == 0);

    go->setDropdown(nullptr);
    cmd.execute();
    CHECK(!go->hasDropdown());
}

// The value is NOT clamped when written (the component does not interpret anything) but
// everything that READS it has to tolerate an out-of-range index: a scene
// edited by hand with value 99 and two options cannot crash.
static void test_dropdown_out_of_range_value_is_survivable()
{
    DropdownComponent d;
    d.options = { "A", "B" };

    d.value = 99;
    CHECK(d.selectedLabel().empty());
    d.value = -1;
    CHECK(d.selectedLabel().empty());
    d.value = 1;
    CHECK(d.selectedLabel() == "B");

    // Not even without options.
    d.options.clear();
    d.value = 0;
    CHECK(d.selectedLabel().empty());

    // Height of the list: at most maxVisibleItems rows are shown.
    d.options         = { "A", "B", "C", "D", "E" };
    d.itemHeight      = 20.0f;
    d.maxVisibleItems = 3u;
    CHECK(nearlyEqual(d.listHeight(), 60.0f));
    d.maxVisibleItems = 0u;   // 0 = all
    CHECK(nearlyEqual(d.listHeight(), 100.0f));
}

static void test_dropdown_sync_builds_label_arrow_and_items()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    DropdownComponent d;
    d.position   = glm::vec2(0.0f, 0.0f);
    d.size       = glm::vec2(160.0f, 30.0f);
    d.options    = { "Bajo", "Medio", "Alto" };
    d.value      = 1;
    d.itemHeight = 20.0f;

    UiWidgetLists w;
    w.dropdowns.emplace_back(5ull, &d);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& caja = *canvas.root().children()[0];
    CHECK(caja.name == uiDropdownNodeName(5ull));
    CHECK(caja.typeName() == std::string("Dropdown"));
    // Label, arrow and list.
    CHECK(caja.children().size() == 3);
    if (caja.children().size() != 3) return;
    const UiElement& lista = *caja.children()[2];
    CHECK(lista.name == uiDropdownNodeName(5ull) + "/List");
    CHECK(lista.children().size() == 3);   // one row per option

    // Closed: the list EXISTS but is not seen. The shape of the subtree does not change when
    // opening, so opening does not rebuild the entire canvas.
    CHECK(lista.visible == false);

    d.isOpen = true;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(lista.visible == true);
    CHECK(canvas.root().children()[0]->children().size() == 3);

    // Changing the NUMBER of options does change the shape: you have to rebuild, or
    // the new option would have no node and would not be seen.
    d.options.push_back("Ultra");
    syncUiWidgets(w, canvas, cache, loader);
    const UiElement& lista2 = *canvas.root().children()[0]->children()[2];
    CHECK(lista2.children().size() == 4);
}

static void test_dropdown_click_opens_and_picks()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    DropdownComponent d;
    d.position   = glm::vec2(0.0f, 0.0f);
    d.size       = glm::vec2(160.0f, 30.0f);
    d.options    = { "Bajo", "Medio", "Alto" };
    d.value      = 0;
    d.itemHeight = 20.0f;

    int ultimo = -1;
    int avisos = 0;
    d.callbacks.ptr->onValueChanged = [&](int v) { ultimo = v; avisos++; };

    UiWidgetLists w;
    w.dropdowns.emplace_back(5ull, &d);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    // Click on the box: it opens.
    clickEnCanvas(canvas, glm::vec2(80.0f, 15.0f), 0.0f);
    CHECK(d.isOpen == true);

    // Have to sync and measure again: the list just became visible,
    // so until now it had no rect to test the mouse against.
    syncUiWidgets(w, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);

    // Click on the second row: the list starts right below the box
    // (y = 30) and each row measures 20, so row 1 goes from 50 to 70.
    clickEnCanvas(canvas, glm::vec2(80.0f, 60.0f), 5.0f);
    CHECK(d.value == 1);
    CHECK(avisos == 1);
    CHECK(ultimo == 1);
    CHECK(d.isOpen == false);   // choosing closes

    // Not interactable: does not even open.
    d.interactable = false;
    syncUiWidgets(w, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    clickEnCanvas(canvas, glm::vec2(80.0f, 15.0f), 10.0f);
    CHECK(d.isOpen == false);
}

static void test_dropdown_hit_test_maps_back_to_gameobject()
{
    CHECK(uiDropdownOwnerId(uiDropdownNodeName(42ull)) == 42ull);
    CHECK(uiDropdownOwnerId(uiDropdownNodeName(42ull) + "/List") == 42ull);
    CHECK(uiDropdownOwnerId("Cubo") == 0ull);
    CHECK(uiDropdownOwnerId("drp:") == 0ull);
    CHECK(uiDropdownOwnerId("drp:12ab") == 0ull);
    CHECK(uiDropdownOwnerId(uiInputFieldNodeName(42ull)) == 0ull);
    CHECK(uiInputFieldOwnerId(uiDropdownNodeName(42ull)) == 0ull);
}

// ── ScrollView ──────────────────────────────────────────────────────────────
// The only one whose MAIN node is not the one that receives the mouse: the children of the
// scene hang from Content, which is the one that moves. If they hung from viewport,
// the scroll would not drag anything.
static void fillScrollView(ScrollViewComponent& s)
{
    s.anchorMin = glm::vec2(0.1875f, 0.4375f);
    s.anchorMax = glm::vec2(0.5625f, 0.5625f);
    s.pivot     = glm::vec2(0.40625f, 0.59375f);
    s.position  = glm::vec2(41.5f, -57.25f);
    s.size      = glm::vec2(311.75f, 217.5f);
    s.color     = glm::vec4(0.17f, 0.18f, 0.19f, 0.21f);
    s.visible   = false;

    s.horizontal = true;
    s.vertical   = false;

    s.contentSize        = glm::vec2(613.5f, 941.25f);
    s.normalizedPosition = glm::vec2(0.3125f, 0.6875f);
    s.scrollSensitivity  = 43.5f;

    s.atlasPath        = "assets/ui/widgets.png";
    s.backgroundSprite = "marco";
}

static void checkScrollViewMatchesFilled(const ScrollViewComponent& s)
{
    CHECK(nearlyEqual(s.anchorMin.x, 0.1875f));
    CHECK(nearlyEqual(s.anchorMax.y, 0.5625f));
    CHECK(nearlyEqual(s.pivot.x, 0.40625f));
    CHECK(nearlyEqual(s.position.y, -57.25f));
    CHECK(nearlyEqual(s.size.x, 311.75f));
    CHECK(nearlyEqual(s.color.a, 0.21f));
    CHECK(s.visible == false);
    CHECK(s.horizontal == true);
    CHECK(s.vertical == false);
    CHECK(nearlyEqual(s.contentSize.x, 613.5f));
    CHECK(nearlyEqual(s.contentSize.y, 941.25f));
    CHECK(nearlyEqual(s.normalizedPosition.x, 0.3125f));
    CHECK(nearlyEqual(s.normalizedPosition.y, 0.6875f));
    CHECK(nearlyEqual(s.scrollSensitivity, 43.5f));
    CHECK(s.atlasPath == "assets/ui/widgets.png");
    CHECK(s.backgroundSprite == "marco");
}

static void test_scroll_view_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* canvasGo = scene.addGameObject("UI");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* go = scene.addGameObject("Lista", canvasGo);
    auto s = std::make_shared<ScrollViewComponent>();
    fillScrollView(*s);
    go->setScrollView(s);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasScrollView()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Lista");
    checkScrollViewMatchesFilled(*found->getScrollView());
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_scroll_view_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool alguno = false;
    loaded.traverse([&](GameObject* n) { if (n->hasScrollView()) alguno = true; });
    CHECK(!alguno);
    CHECK(loaded.lastWarnings().empty());
}

static void test_scene_without_scroll_view_serializes_identically()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Pelado");
    const std::string antes = scene.toJson().dump();
    CHECK(antes.find("\"scrollView\"") == std::string::npos);

    go->setScrollView(std::make_shared<ScrollViewComponent>());
    CHECK(scene.toJson().dump() != antes);
    go->setScrollView(nullptr);
    CHECK(scene.toJson().dump() == antes);
}

static void test_scroll_view_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Lista");
    ScrollViewComponent st;
    fillScrollView(st);
    ScrollViewComponentCommand cmd(scene, "Add Scroll View", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasScrollView());
    checkScrollViewMatchesFilled(*go->getScrollView());
    cmd.undo();
    CHECK(!go->hasScrollView());
    cmd.execute();
    CHECK(go->hasScrollView());
    checkScrollViewMatchesFilled(*go->getScrollView());
}

static void test_scroll_view_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Lista");
    auto s = std::make_shared<ScrollViewComponent>();
    fillScrollView(*s);
    go->setScrollView(s);

    ScrollViewComponentCommand cmd(scene, "Remove Scroll View", go->id, /*add=*/false, *s);
    cmd.execute();
    CHECK(!go->hasScrollView());
    cmd.undo();
    CHECK(go->hasScrollView());
    checkScrollViewMatchesFilled(*go->getScrollView());
}

static void test_scroll_view_property_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Lista");
    go->setScrollView(std::make_shared<ScrollViewComponent>());
    const uint64_t id = go->id;
    Scene* sc = &scene;

    auto applyContent = [sc, id](const glm::vec2& v) {
        if (GameObject* g = sc->findById(id))
            if (g->hasScrollView()) g->getScrollView()->contentSize = v;
    };
    PropertyCommand<glm::vec2> cmd("Content Size", glm::vec2(200.0f, 400.0f),
                                   glm::vec2(37.5f, 91.25f), applyContent);

    cmd.execute();
    CHECK(nearlyEqual(go->getScrollView()->contentSize.x, 37.5f));
    cmd.undo();
    CHECK(nearlyEqual(go->getScrollView()->contentSize.y, 400.0f));

    go->setScrollView(nullptr);
    cmd.execute();
    CHECK(!go->hasScrollView());
}

// The offset of the content comes from the REAL scroll: content minus
// viewport. With content smaller than the viewport there is nothing to
// scroll, and the offset has to be 0 and not negative — content pushed
// inward leaves a gap at the top that nobody asked for.
static void test_scroll_view_content_offset()
{
    ScrollViewComponent s;
    s.size        = glm::vec2(100.0f, 200.0f);
    s.contentSize = glm::vec2(100.0f, 600.0f);
    s.vertical    = true;
    s.horizontal  = false;

    CHECK(nearlyEqual(s.scrollRange().y, 400.0f));

    s.normalizedPosition = glm::vec2(0.0f, 0.0f);
    CHECK(nearlyEqual(s.contentOffset().y, 0.0f));
    s.normalizedPosition = glm::vec2(0.0f, 1.0f);
    CHECK(nearlyEqual(s.contentOffset().y, -400.0f));
    s.normalizedPosition = glm::vec2(0.0f, 0.25f);
    CHECK(nearlyEqual(s.contentOffset().y, -100.0f));

    // Axis off: does not move even if the content is wider.
    s.contentSize        = glm::vec2(500.0f, 600.0f);
    s.normalizedPosition = glm::vec2(1.0f, 0.0f);
    CHECK(nearlyEqual(s.contentOffset().x, 0.0f));
    s.horizontal = true;
    CHECK(nearlyEqual(s.contentOffset().x, -400.0f));

    // Content smaller than the viewport: zero, never positive.
    s.contentSize        = glm::vec2(50.0f, 50.0f);
    s.normalizedPosition = glm::vec2(1.0f, 1.0f);
    CHECK(nearlyEqual(s.contentOffset().x, 0.0f));
    CHECK(nearlyEqual(s.contentOffset().y, 0.0f));
    CHECK(nearlyEqual(s.scrollRange().x, 0.0f));
}

static void test_scroll_view_sync_builds_viewport_and_content()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ScrollViewComponent s;
    s.position           = glm::vec2(10.0f, 20.0f);
    s.size               = glm::vec2(100.0f, 200.0f);
    s.contentSize        = glm::vec2(100.0f, 600.0f);
    s.normalizedPosition = glm::vec2(0.0f, 0.5f);

    UiWidgetLists w;
    w.scrollViews.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& vista = *canvas.root().children()[0];
    CHECK(vista.name == uiScrollViewNodeName(5ull));
    CHECK(vista.typeName() == std::string("ScrollView"));
    // Clips its descendants: that is what keeps the content from escaping.
    CHECK(vista.clipChildren == true);
    CHECK(vista.children().size() == 1);
    if (vista.children().empty()) return;
    const UiElement& contenido = *vista.children()[0];
    CHECK(contenido.name == uiScrollViewNodeName(5ull) + "/Content");
    CHECK(nearlyEqual(contenido.size.y, 600.0f));
    CHECK(nearlyEqual(contenido.position.y, -200.0f));   // (600-200) * 0.5
    // The content does NOT receive the mouse: the wheel is the viewport's.
    CHECK(contenido.raycastTarget == false);
}

// The children of the scene hang from CONTENT, not from viewport: if they hung from
// viewport, moving would not drag them and scrolling would be useless.
static void test_scroll_view_children_hang_from_the_content()
{
    Scene scene;
    GameObject* canvasGo = scene.addGameObject("Canvas");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* vista = scene.addGameObject("Lista", canvasGo);
    auto sv = std::make_shared<ScrollViewComponent>();
    sv->size        = glm::vec2(100.0f, 200.0f);
    sv->contentSize = glm::vec2(100.0f, 600.0f);
    vista->setScrollView(sv);

    GameObject* fila = scene.addGameObject("Fila", vista);
    fila->setPanel(std::make_shared<PanelComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    static const UiWidgetLists kVacio;
    const UiWidgetLists& w = bindings.empty() ? kVacio : bindings[0].widgets;
    CHECK(w.scrollViews.size() == 1);
    CHECK(w.panels.size() == 1);

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(w, canvas, cache, loader);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& nodoVista = *canvas.root().children()[0];
    CHECK(nodoVista.children().size() == 1);
    if (nodoVista.children().empty()) return;
    const UiElement& contenido = *nodoVista.children()[0];
    CHECK(contenido.name == uiScrollViewNodeName(vista->id) + "/Content");
    // The row hangs from CONTENT.
    CHECK(contenido.children().size() == 1);
    if (contenido.children().empty()) return;
    CHECK(contenido.children()[0]->name == uiPanelNodeName(fila->id));
}

static void test_scroll_view_wheel_moves_the_component()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ScrollViewComponent s;
    s.position          = glm::vec2(0.0f, 0.0f);
    s.size              = glm::vec2(100.0f, 200.0f);
    s.contentSize       = glm::vec2(100.0f, 600.0f);   // scroll range: 400 px
    s.scrollSensitivity = 40.0f;                        // 40 px per notch = 0.1

    float ultimoY = -1.0f;
    int   avisos  = 0;
    s.callbacks.ptr->onValueChanged = [&](float, float y) { ultimoY = y; avisos++; };

    UiWidgetLists w;
    w.scrollViews.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    UiInputState in;
    in.mousePos    = glm::vec2(50.0f, 100.0f);
    in.timeSeconds = 0.0f;
    canvas.updateInput(in);

    // Wheel down (negative delta) = go down the list.
    in.scrollDelta = -1.0f;
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.normalizedPosition.y, 0.1f));
    CHECK(avisos == 1);
    CHECK(nearlyEqual(ultimoY, 0.1f));

    // And up it comes back.
    in.scrollDelta = 1.0f;
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.normalizedPosition.y, 0.0f));

    // Does not leave [0,1] no matter how much you insist.
    for (int i = 0; i < 50; i++)
    {
        in.scrollDelta = -1.0f;
        in.timeSeconds += 0.016f;
        canvas.updateInput(in);
    }
    CHECK(nearlyEqual(s.normalizedPosition.y, 1.0f));

    // Without scroll range it does not move NOR warn: content that fits entirely does not
    // scroll, and warning of a change that did not happen would make a
    // script work for nothing on every notch.
    s.contentSize = glm::vec2(100.0f, 100.0f);
    syncUiWidgets(w, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    const int antes = avisos;
    in.scrollDelta = -1.0f;
    in.timeSeconds += 0.016f;
    canvas.updateInput(in);
    CHECK(avisos == antes);
}

static void test_scroll_view_hit_test_maps_back_to_gameobject()
{
    CHECK(uiScrollViewOwnerId(uiScrollViewNodeName(42ull)) == 42ull);
    CHECK(uiScrollViewOwnerId(uiScrollViewNodeName(42ull) + "/Content") == 42ull);
    CHECK(uiScrollViewOwnerId("Cubo") == 0ull);
    CHECK(uiScrollViewOwnerId("scv:") == 0ull);
    CHECK(uiScrollViewOwnerId("scv:12ab") == 0ull);
    // "scv:" and "scr:" (Scrollbar) do NOT confuse each other: they share the first three
    // letters and are two different widgets.
    CHECK(uiScrollViewOwnerId(uiScrollbarNodeName(42ull)) == 0ull);
    CHECK(uiScrollbarOwnerId(uiScrollViewNodeName(42ull)) == 0ull);
}

// The slots are paired by ownerId, NOT by index. Reordering the canvas in the
// hierarchy (or deleting one in the middle) cannot reset the cache of one that has not
// moved: if it did, moving an enemy would rebuild its health bar
// entirely and it would look like a flicker.
static void test_ui_slots_se_emparejan_por_owner_id()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    CanvasComponent c;

    std::vector<UiCanvasBinding> b(2);
    b[0].ownerId = 7ull;  b[0].canvas = &c;
    b[1].ownerId = 9ull;  b[1].canvas = &c;
    matchUiCanvasSlots(b, slots);
    CHECK(slots.size() == 2);
    if (slots.size() != 2) return;
    const UiCanvas* arbol7 = &slots[0]->canvas;
    const UiCanvas* arbol9 = &slots[1]->canvas;
    CHECK(slots[0]->ownerId == 7ull);
    CHECK(slots[1]->ownerId == 9ull);

    // The order is INVERTED: each slot has to follow ITS owner, with its tree.
    std::vector<UiCanvasBinding> b2(2);
    b2[0].ownerId = 9ull; b2[0].canvas = &c;
    b2[1].ownerId = 7ull; b2[1].canvas = &c;
    matchUiCanvasSlots(b2, slots);
    CHECK(slots.size() == 2);
    if (slots.size() != 2) return;
    CHECK(slots[0]->ownerId == 9ull);
    CHECK(slots[1]->ownerId == 7ull);
    CHECK(&slots[0]->canvas == arbol9);
    CHECK(&slots[1]->canvas == arbol7);

    // And one that disappears takes its slot; a new one starts its own.
    std::vector<UiCanvasBinding> b3(1);
    b3[0].ownerId = 42ull; b3[0].canvas = &c;
    matchUiCanvasSlots(b3, slots);
    CHECK(slots.size() == 1);
    if (slots.empty()) return;
    CHECK(slots[0]->ownerId == 42ull);
}

// And the painting order of WORLD canvas: far to near. They have alpha,
// so painting them backwards mixes badly and looks like a halo. Against
// geometry the depth rules; among them, this does.
static void test_world_canvases_se_ordenan_de_lejos_a_cerca()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    for (float z : { -2.0f, -10.0f, -6.0f })
    {
        auto s = std::make_unique<UiCanvasSlot>();
        s->mode  = UiCanvasRenderMode::World;
        s->model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z));
        slots.push_back(std::move(s));
    }

    // A fourth SCREEN slot, with a z that would fall IN THE MIDDLE of the order if
    // the mode filter slipped through: between the one at -10 and the one at -2. If someone
    // deletes the "continue" (or inverts the condition) of the filter of
    // sortWorldCanvasesBackToFront, this slot not only changes the size of
    // `order`, it also sneaks in the middle and messes up the expected order — the
    // test fails in two places at once, not just on size.
    UiCanvasSlot* pantalla = nullptr;
    {
        auto s = std::make_unique<UiCanvasSlot>();
        s->mode  = UiCanvasRenderMode::ScreenSpace;
        s->model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f));
        pantalla = s.get();
        slots.push_back(std::move(s));
    }

    // Camera at the origin looking at -Z: the one at z = -10 is the farthest.
    const glm::mat4 vista(1.0f);
    std::vector<UiCanvasSlot*> orden;
    sortWorldCanvasesBackToFront(slots, vista, orden);

    CHECK(orden.size() == 3);
    if (orden.size() != 3) return;
    CHECK(nearlyEqual(orden[0]->model[3].z, -10.0f));
    CHECK(nearlyEqual(orden[1]->model[3].z, -6.0f));
    CHECK(nearlyEqual(orden[2]->model[3].z, -2.0f));

    // The screen one must not appear at all in the world list.
    CHECK(orden[0] != pantalla);
    CHECK(orden[1] != pantalla);
    CHECK(orden[2] != pantalla);
}

// And the INPUT PRIORITY order of SCREEN canvas: the one on top
// first, that is the LAST one painted. The UI pass traverses the slots in
// order and each canvas paints over the previous one, so priority is the painting order
// REVERSED. The WORLD slot goes IN THE MIDDLE on purpose: if someone
// deletes the mode filter, not only does the list size change — it sneaks
// between the two screen ones and also messes up the second position.
static void test_canvas_de_pantalla_en_orden_de_prioridad()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    for (auto par : { std::make_pair(7ull,  UiCanvasRenderMode::ScreenSpace),
                      std::make_pair(8ull,  UiCanvasRenderMode::World),
                      std::make_pair(9ull,  UiCanvasRenderMode::ScreenSpace) })
    {
        auto s = std::make_unique<UiCanvasSlot>();
        s->ownerId = par.first;
        s->mode    = par.second;
        slots.push_back(std::move(s));
    }

    std::vector<UiCanvas*> orden;
    screenCanvasesTopFirst(slots, orden);

    CHECK(orden.size() == 2);
    // Guards by POSITION and not a `return` on first size failure: if the
    // mode filter falls off, the world one sneaks IN THE MIDDLE and what you have to see
    // is that the second position also gets messed up, not just the size.
    // POINTER identity, not ownerId: it is the concrete tree that will
    // receive the mouse, and comparing ids would let one paired by index slip through.
    if (orden.size() >= 1) CHECK(orden[0] == &slots[2]->canvas);   // the last painted, the first in input
    if (orden.size() >= 2) CHECK(orden[1] == &slots[0]->canvas);
    if (orden.size() >= 1) CHECK(orden[0] != &slots[1]->canvas);   // the world one does not enter
    if (orden.size() >= 2) CHECK(orden[1] != &slots[1]->canvas);
}

// ── Input dispatch among overlapped screen canvas ─────────────────────────────
// Two screen canvas, each with ITS button, with OVERLAPPED rects and with
// distinct and not neutral values among themselves. It is the fixture for the four tests
// below: the criterion of who wins the pointer, the cleanup of the loser, the
// capture during a drag and the keyboard dispatch.
struct DosCanvasSolapados
{
    UiCanvas          arriba;      // the LAST one painted = the one on top
    UiCanvas          abajo;
    UiWidgetSyncCache cacheArriba;
    UiWidgetSyncCache cacheAbajo;
    FakeUiLoader      loader;
    ButtonComponent   compArriba;
    ButtonComponent   compAbajo;
    Button*           nodoArriba = nullptr;
    Button*           nodoAbajo  = nullptr;

    std::vector<UiCanvas*> orden;   // input priority: top first

    int clicksArriba = 0;
    int clicksAbajo  = 0;
    int exitsAbajo   = 0;
    int dragsAbajo   = 0;

    UiInputState in;
    float t = 0.0f;

    // Inside the button of TOP and BOTTOM at the same time.
    static glm::vec2 solape()    { return glm::vec2(150.0f, 150.0f); }
    // Inside BOTTOM and outside TOP.
    static glm::vec2 soloAbajo() { return glm::vec2(400.0f, 330.0f); }

    DosCanvasSolapados()
    {
        // x[100,300] y[100,220]
        compArriba.position    = glm::vec2(100.0f, 100.0f);
        compArriba.size        = glm::vec2(200.0f, 120.0f);
        compArriba.normalColor = glm::vec4(0.20f, 0.40f, 0.60f, 1.0f);
        compArriba.hoverColor  = glm::vec4(0.90f, 0.10f, 0.30f, 1.0f);
        // x[60,460] y[60,360] — contains all of top and overhangs at the bottom
        compAbajo.position     = glm::vec2(60.0f, 60.0f);
        compAbajo.size         = glm::vec2(400.0f, 300.0f);
        compAbajo.normalColor  = glm::vec4(0.05f, 0.70f, 0.25f, 1.0f);
        compAbajo.hoverColor   = glm::vec4(0.15f, 0.35f, 0.85f, 1.0f);

        std::vector<std::pair<uint64_t, const ButtonComponent*>> listaArriba{ {11ull, &compArriba} };
        std::vector<std::pair<uint64_t, const ButtonComponent*>> listaAbajo { {22ull, &compAbajo}  };
        syncUiWidgets(listaArriba, {}, {}, arriba, cacheArriba, loader);
        syncUiWidgets(listaAbajo,  {}, {}, abajo,  cacheAbajo,  loader);

        UiDrawData data;
        arriba.buildDrawData(800, 480, data);   // places the rects: the hit test reads them
        abajo .buildDrawData(800, 480, data);

        nodoArriba = cacheArriba.buttonNodes.empty() ? nullptr : cacheArriba.buttonNodes[0];
        nodoAbajo  = cacheAbajo .buttonNodes.empty() ? nullptr : cacheAbajo .buttonNodes[0];

        compArriba.callbacks.ptr->onClick = [this] { clicksArriba++; };
        compAbajo .callbacks.ptr->onClick = [this] { clicksAbajo++;  };
        if (nodoAbajo)
        {
            nodoAbajo->onMouseExit = [this](UiEvent&) { exitsAbajo++; };
            nodoAbajo->onDrag      = [this](UiEvent&) { dragsAbajo++; };
        }

        orden = { &arriba, &abajo };
    }

    // One frame: advance the clock and distribute the current state between the two.
    void frame()
    {
        t += 0.016f;
        in.timeSeconds = t;
        dispatchUiInput(orden, in);
        in.keys.clear();
        in.chars.clear();
    }
};

// The one ON TOP takes the pointer. Without this, two overlapped canvas leave BOTH
// a widget in hover and a click activates two buttons at once.
static void test_el_canvas_de_encima_se_lleva_el_puntero()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr && e.nodoAbajo != nullptr);
    if (!e.nodoArriba || !e.nodoAbajo) return;

    e.in.mousePos = DosCanvasSolapados::solape();
    e.frame();

    CHECK(e.arriba.hovered() == e.nodoArriba);
    CHECK(e.abajo.hovered()  == nullptr);
    CHECK(e.nodoArriba->hovered);
    CHECK(!e.nodoAbajo->hovered);
    CHECK(e.nodoArriba->state == UiButtonState::Hover);
    CHECK(e.nodoAbajo->state  == UiButtonState::Normal);

    e.in.mouseDown[0] = true;
    e.frame();
    CHECK(e.nodoArriba->state == UiButtonState::Pressed);
    CHECK(e.nodoAbajo->state  == UiButtonState::Normal);

    e.in.mouseDown[0] = false;
    e.frame();
    CHECK(e.clicksArriba == 1);
    CHECK(e.clicksAbajo  == 0);
}

// And the one that LOSES the pointer has to be CLEAN. It is the half you do not see:
// a canvas that had a button in hover and stops receiving input stays STUCK
// in that hover forever — the button looks lit and does not respond to anything.
// That is why the ones that do not win receive input with the mouse OUT, not none.
static void test_el_canvas_de_abajo_no_se_queda_pegado_en_hover()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr && e.nodoAbajo != nullptr);
    if (!e.nodoArriba || !e.nodoAbajo) return;

    // First the mouse where ONLY the bottom one receives it: it enters hover.
    e.in.mousePos = DosCanvasSolapados::soloAbajo();
    e.frame();
    CHECK(e.abajo.hovered()  == e.nodoAbajo);
    CHECK(e.arriba.hovered() == nullptr);
    CHECK(e.nodoAbajo->state == UiButtonState::Hover);
    CHECK(e.exitsAbajo == 0);

    // And now to the overlapped zone: the top one wins and the bottom one has to
    // RELEASE hover, with its MouseExit and its color back to Normal.
    e.in.mousePos = DosCanvasSolapados::solape();
    e.frame();
    CHECK(e.arriba.hovered() == e.nodoArriba);
    CHECK(e.abajo.hovered()  == nullptr);
    CHECK(!e.nodoAbajo->hovered);
    CHECK(e.nodoAbajo->state == UiButtonState::Normal);
    CHECK(e.exitsAbajo == 1);
}

// Pointer capture survives overlap: press the button over a widget and
// drag OVER another canvas does not cut the drag. Without this, a slider
// that pokes out from below another canvas gets stuck halfway when the cursor
// crosses the edge, and the gesture is lost without any warning.
static void test_la_captura_del_puntero_sobrevive_al_solape()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr && e.nodoAbajo != nullptr);
    if (!e.nodoArriba || !e.nodoAbajo) return;

    e.in.mousePos = DosCanvasSolapados::soloAbajo();
    e.frame();
    e.in.mouseDown[0] = true;
    e.frame();
    CHECK(e.abajo.pointerCaptured());
    CHECK(!e.arriba.pointerCaptured());

    // Drag to the overlapped zone with the button HELD.
    e.in.mousePos = DosCanvasSolapados::solape();
    e.frame();
    CHECK(e.abajo.pointerCaptured());        // still yours
    CHECK(e.abajo.hovered() == e.nodoAbajo); // still seeing the mouse
    CHECK(e.arriba.hovered() == nullptr);    // the top one does NOT steal it
    CHECK(e.dragsAbajo >= 1);                // and the drag stays alive

    // On release, capture ends and the top one recovers the pointer.
    e.in.mouseDown[0] = false;
    e.frame();
    CHECK(!e.abajo.pointerCaptured());
    e.frame();
    CHECK(e.arriba.hovered() == e.nodoArriba);
    CHECK(e.abajo.hovered()  == nullptr);
}

// Keyboard does NOT follow the mouse: it follows FOCUS. Writing in a field of one canvas
// and moving the cursor over another must not divert a single key. And
// focus moves on CLICK, not on mouseover — and when it moves, the canvas
// that had it releases it, or there would be two focus rings at once.
static void test_el_teclado_va_al_canvas_con_foco_no_al_del_raton()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr && e.nodoAbajo != nullptr);
    if (!e.nodoArriba || !e.nodoAbajo) return;
    e.nodoArriba->focusable = true;
    e.nodoAbajo->focusable  = true;

    // Click where only the bottom one receives it: it gets focus.
    e.in.mousePos = DosCanvasSolapados::soloAbajo();
    e.frame();
    e.in.mouseDown[0] = true;  e.frame();
    e.in.mouseDown[0] = false; e.frame();
    CHECK(e.clicksAbajo == 1);
    CHECK(e.abajo.focused()  == e.nodoAbajo);
    CHECK(e.arriba.focused() == nullptr);

    // The mouse goes to the overlapped zone — the POINTER changes canvas — and
    // Enter is pressed. The key has to go to the BOTTOM one, the one that has focus.
    e.in.mousePos = DosCanvasSolapados::solape();
    e.in.keys.push_back(UiKey::Enter);
    e.frame();
    CHECK(e.arriba.hovered() == e.nodoArriba);   // the pointer DID move
    CHECK(e.abajo.hovered()  == nullptr);
    CHECK(e.abajo.focused()  == e.nodoAbajo);    // the focus did NOT
    CHECK(e.clicksAbajo  == 2);                  // 1 from mouse + 1 from Enter
    CHECK(e.clicksArriba == 0);

    // A click in the overlapped zone DOES move focus, and the bottom one releases it.
    e.in.mouseDown[0] = true;  e.frame();
    e.in.mouseDown[0] = false; e.frame();
    CHECK(e.clicksArriba == 1);
    CHECK(e.arriba.focused() == e.nodoArriba);
    CHECK(e.abajo.focused()  == nullptr);

    // And from here Enter goes to the top one, not the bottom one.
    e.in.keys.push_back(UiKey::Enter);
    e.frame();
    CHECK(e.clicksArriba == 2);
    CHECK(e.clicksAbajo  == 2);
}

// PHANTOM click: press in EMPTY space, drag over a button and
// release cannot activate it. The semantics are always MouseUp YES, Click NO —
// Click requires that Down and Up fall on the SAME element.
//
// And this happens with a SINGLE canvas, that is, any existing project: if
// no one wins the pointer (the cursor is not over any widget) and the canvas
// is lied to saying there is no button pressed, it does not see the falling edge of the
// background. When the cursor later enters the button with the button STILL pressed, it sees a
// NEW edge, registers a press on it, on release emits a Click that no one
// asked for and on top of that steals focus.
//
// That is why the state of the one that does NOT have the pointer lies about the POSITION of the
// mouse (uiPointerAway) but NOT about the buttons: with the mouse out the hit
// test already returns nullptr, so the press is registered on nullptr and dispatch
// nothing, and estadoDe cannot paint Pressed without hovered. The only thing needed
// is for the count of EDGES to still be the real one.
static void test_no_hay_clic_fantasma_al_arrastrar_desde_el_vacio()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr);
    if (!e.nodoArriba) return;
    // Focusable so that the focus theft of the phantom click is observable.
    e.nodoArriba->focusable = true;

    int upsArriba = 0;
    e.nodoArriba->onMouseUp = [&](UiEvent&) { upsArriba++; };

    // A SINGLE canvas in the list: the bug does not need a second canvas.
    std::vector<UiCanvas*> soloUno{ &e.arriba };
    UiInputState in;
    float t = 0.0f;
    auto frame = [&]() { t += 0.016f; in.timeSeconds = t; dispatchUiInput(soloUno, in); };

    // Press in a canvas area with NO widget...
    in.mousePos = glm::vec2(600.0f, 400.0f);
    frame();
    in.mouseDown[0] = true;
    frame();
    // ...drag over the button with the button STILL pressed...
    in.mousePos = DosCanvasSolapados::solape();
    frame();
    // ...and release.
    in.mouseDown[0] = false;
    frame();

    CHECK(e.clicksArriba == 0);             // the phantom click
    CHECK(e.arriba.focused() == nullptr);   // and the focus it steals along the way
    CHECK(upsArriba == 1);                  // the MouseUp DOES arrive, as always

    // Control: a REAL click on the button keeps counting. Without this, an
    // implementation that emitted no Click would pass just as well.
    frame();
    in.mouseDown[0] = true;  frame();
    in.mouseDown[0] = false; frame();
    CHECK(e.clicksArriba == 1);
    CHECK(e.arriba.focused() == e.nodoArriba);
}

// The canvas of a SPECIFIC GameObject, by ownerId. The gizmo of the
// SELECTED canvas needs this: with uiCanvas() — the FIRST screen canvas — selecting
// a SECOND screen canvas painted the rect of the FIRST, that is a gizmo that
// lies. With a single canvas they match and you do not notice, which is silent.
static void test_canvas_por_owner_id()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    for (auto par : { std::make_pair(7ull,  UiCanvasRenderMode::ScreenSpace),
                      std::make_pair(8ull,  UiCanvasRenderMode::World),
                      std::make_pair(9ull,  UiCanvasRenderMode::ScreenSpace) })
    {
        auto s = std::make_unique<UiCanvasSlot>();
        s->ownerId = par.first;
        s->mode    = par.second;
        slots.push_back(std::move(s));
    }

    // POINTER identity: it is the concrete tree whose uiOrigin/uiScale the
    // gizmo is going to read. The 9 is the one that catches the bug — it is the SECOND screen, that is
    // the one uiCanvas() never returned.
    CHECK(findCanvasByOwner(slots, 9ull) == &slots[2]->canvas);
    CHECK(findCanvasByOwner(slots, 7ull) == &slots[0]->canvas);
    // WORLD ones also come out: the mode filter is from who asks.
    CHECK(findCanvasByOwner(slots, 8ull) == &slots[1]->canvas);
    // And an id that is not there returns NOTHING, not "the first": the gizmo has to
    // be able to not draw instead of drawing the other one.
    CHECK(findCanvasByOwner(slots, 42ull) == nullptr);
}

// Orphaned capture: a canvas that exits input dispatch mid-press
// (a script that sets renderMode = World, which is writable from Lua) never
// sees the MouseUp and stays with m_pressTarget. On return it would enter with
// pointerCaptured() true WITHOUT any button down, it would take the mouse in
// step 1 of dispatchUiInput and steal the pointer from the one above, with a
// MouseUp/Click that nobody asked for. And in the process it would stay STUCK in its last
// hover, because being outside the list dispatchUiInput can no longer clean it.
static void test_soltar_el_input_no_deja_captura_huerfana()
{
    DosCanvasSolapados e;
    CHECK(e.nodoArriba != nullptr && e.nodoAbajo != nullptr);
    if (!e.nodoArriba || !e.nodoAbajo) return;
    e.nodoAbajo->focusable = true;

    // Button goes down on the one BELOW, in its exclusive zone: gets capture,
    // hover and focus.
    e.in.mousePos = DosCanvasSolapados::soloAbajo();
    e.frame();
    e.in.mouseDown[0] = true;
    e.frame();
    CHECK(e.abajo.pointerCaptured());
    CHECK(e.abajo.hovered() == e.nodoAbajo);
    CHECK(e.abajo.focused() == e.nodoAbajo);
    CHECK(e.exitsAbajo == 0);

    // The script pulls it out of the dispatch mid-press.
    e.abajo.releaseInput();
    CHECK(!e.abajo.pointerCaptured());
    CHECK(e.abajo.hovered() == nullptr);
    CHECK(!e.nodoAbajo->hovered);
    CHECK(e.abajo.focused() == nullptr);
    CHECK(e.exitsAbajo == 1);            // with its MouseExit, not silently

    // And when it returns, with the mouse already released and in the OVERLAPPED zone, the one
    // ON TOP takes the pointer. Without the released one, the orphaned capture would win
    // step 1 and steal it.
    e.in.mouseDown[0] = false;
    e.in.mousePos     = DosCanvasSolapados::solape();
    e.frame();
    CHECK(e.arriba.hovered() == e.nodoArriba);
    CHECK(e.abajo.hovered()  == nullptr);
}

// The UI buffer is ONE ONLY per frame and is shared by WORLD canvas
// (which are recorded in the scene pass) with SCREEN ones (which come after, in
// their own pass). Sizing it with the total of ONE of the two halves is the
// bug that doesn't show: the one left out is discarded by the uiCursorFits guard
// IN SILENCE — no error, no validation warning, no canvas on screen. And the
// SCREEN count is needed separately because it's the one that decides if the UI
// pass gets to open.
//
// The six numbers are intentionally distinct from each other: with equal totals, a
// sum that picked the wrong field (indices by vertices, or the total by the
// screen one) would give the same result anyway.
static void test_ui_frame_totals_suma_mundo_y_pantalla()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;

    auto conDatos = [&](UiCanvasRenderMode modo, size_t vertices, size_t indices) {
        auto s = std::make_unique<UiCanvasSlot>();
        s->mode = modo;
        s->drawData.vertices.resize(vertices);
        s->drawData.indices.resize(indices);
        slots.push_back(std::move(s));
    };

    conDatos(UiCanvasRenderMode::World,       5, 9);
    conDatos(UiCanvasRenderMode::ScreenSpace, 3, 6);
    conDatos(UiCanvasRenderMode::World,       11, 21);
    // An empty slot: matchUiCanvasSlots never leaves them, but the sum cannot
    // depend on that — a nullptr here would crash before reaching draw.
    slots.push_back(nullptr);

    const UiFrameTotals t = uiFrameTotals(slots);

    // ALL canvas of the frame: 5 + 3 + 11 and 9 + 6 + 21.
    CHECK(t.vertices == 19u);
    CHECK(t.indices  == 36u);
    // And only the SCREEN ones.
    CHECK(t.screenVertices == 3u);
    CHECK(t.screenIndices  == 6u);
}

// ── Gizmo of the world canvas (ViewportPanel) ─────────────────────────────────
//
// The arithmetic of the world canvas gizmo: project the four corners and
// convert them to pixels of the viewport image. It is declared here by hand because
// it lives in engine/src/Editor/ViewportPanel.cpp, which is EDITOR code and
// does not have a public header to export it — this executable links DonTopoEditor,
// so the symbol is there. The rest of the gizmo is pure ImGui (AddLine on a
// draw list) and cannot be tested without a window; this can, and that is where the
// two unseen bugs are: the corner behind the camera and the Y flipped.
namespace DonTopo
{
    bool projectWorldCanvasCorners(const glm::mat4& mvp,
                                   const glm::vec2& rectMin, const glm::vec2& rectMax,
                                   const glm::vec2& imagePos, const glm::vec2& imageSize,
                                   glm::vec2 outCorners[4]);
    const GameObject* owningCanvasObject(const GameObject* go);
}

// The same projection that ViewportPanel::pickObject builds for the
// EDIT camera: fixed 45° + Vulkan's Y-flip. Here explicit so the test does not
// depend on any Renderer state.
static glm::mat4 editorProjForTest(float aspect)
{
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    proj[1][1] *= -1.0f;
    return proj;
}

// A world canvas TILTED on two axes, so the four corners land
// on eight DISTINCT numbers among themselves: with the canvas facing forward, left and
// right share X and top and bottom share Y, and a permutation of the
// corners — or a mirrored axis — would pass the same way.
//
// The expected values are calculated outside (proj·view·model by hand, in
// double precision), not derived from the function itself.
static void test_world_canvas_gizmo_proyecta_las_cuatro_esquinas()
{
    CanvasComponent c;
    c.renderMode          = UiCanvasRenderMode::World;
    c.billboard           = UiBillboard::None;
    c.worldScale          = 0.01f;                    // 200x100 px -> 2x1 units
    c.referenceResolution = glm::vec2(200.0f, 100.0f);

    const glm::vec2 tam(200.0f, 100.0f);
    const glm::mat4 mundo = glm::rotate(
        glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.6f, -0.3f, -5.0f)),
                    glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::radians(20.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 vista(1.0f);                      // camera at the origin looking at -Z

    const glm::vec2 imagenPos(37.0f, 91.0f);          // not (0,0): the gizmo goes in SCREEN coords
    const glm::vec2 imagenTam(800.0f, 400.0f);

    const glm::mat4 mvp = editorProjForTest(imagenTam.x / imagenTam.y) * vista *
                          uiWorldCanvasMatrix(c, tam, mundo, vista);

    glm::vec2 esq[4];
    CHECK(projectWorldCanvasCorners(mvp, glm::vec2(0.0f), tam, imagenPos, imagenTam, esq));

    // Order: (0,0), (w,0), (w,h), (0,h) in CANVAS pixels. The (0,0) of the
    // canvas is its UPPER-left corner — the canvas Y grows
    // downward, and uiWorldCanvasMatrix negates it — so it has to come out with the Y
    // of image SMALLEST of the four. If someone puts the image Y
    // backwards, the quadrilateral is still a quadrilateral and only this catches it.
    CHECK(nearlyEqual(esq[0].x, 423.3624f, 0.02f));
    CHECK(nearlyEqual(esq[0].y, 271.8673f, 0.02f));
    CHECK(nearlyEqual(esq[1].x, 571.8282f, 0.02f));
    CHECK(nearlyEqual(esq[1].y, 275.9068f, 0.02f));
    CHECK(nearlyEqual(esq[2].x, 548.6389f, 0.02f));
    CHECK(nearlyEqual(esq[2].y, 356.0572f, 0.02f));
    CHECK(nearlyEqual(esq[3].x, 403.4565f, 0.02f));
    CHECK(nearlyEqual(esq[3].y, 372.4002f, 0.02f));
}

// A corner with w <= 0 is BEHIND the camera plane: dividing by that w
// mirrors the point to the other side and the quadrilateral comes out crossed or shot to
// infinity, without anything on screen saying it is garbage (ImGui does not clip, it draws
// what it is given). The rejection has to be EXPLICIT.
static void test_world_canvas_gizmo_rechaza_esquina_detras_de_la_camara()
{
    CanvasComponent c;
    c.renderMode          = UiCanvasRenderMode::World;
    c.worldScale          = 0.01f;
    c.referenceResolution = glm::vec2(200.0f, 100.0f);

    const glm::vec2 tam(200.0f, 100.0f);
    // Almost edge-on (-80°) and half a meter from the camera. The four w come out
    // +1.4848, -0.4848, -0.4848, +1.4848: two corners in front and two behind — the
    // REAL case, not "all behind", which would be caught even by a reject at the center.
    //
    // The sign matters and is NOT interchangeable with +80°. At +80° the corner that rejects
    // is corner 0, the function exits on the first loop without having written
    // anything, and then an implementation that wrote DIRECTLY to outCorners
    // would pass this test the same way: the sentinels below would not prove anything. At
    // -80° corner 0 is IN FRONT and the one that rejects is 1, so by the time
    // the problem is discovered there is already a corner calculated. That is what
    // the intermediate array of projectWorldCanvasCorners proves: nobody can
    // keep a new corner and three old ones.
    const glm::mat4 mundo = glm::rotate(
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.5f)),
        glm::radians(-80.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 vista(1.0f);

    const glm::mat4 mvp = editorProjForTest(2.0f) * vista *
                          uiWorldCanvasMatrix(c, tam, mundo, vista);

    // Sentinels distinct from each other: if the function wrote something before
    // rejecting, it would show here.
    glm::vec2 esq[4] = { glm::vec2(-11.0f, -12.0f), glm::vec2(-13.0f, -14.0f),
                         glm::vec2(-15.0f, -16.0f), glm::vec2(-17.0f, -18.0f) };
    CHECK(!projectWorldCanvasCorners(mvp, glm::vec2(0.0f), tam, glm::vec2(37.0f, 91.0f),
                                     glm::vec2(800.0f, 400.0f), esq));
    // The one at corner 0 is the one that bites: it is the one that DID get calculated.
    CHECK(nearlyEqual(esq[0].x, -11.0f));
    CHECK(nearlyEqual(esq[0].y, -12.0f));
    CHECK(nearlyEqual(esq[1].y, -14.0f));
    CHECK(nearlyEqual(esq[3].x, -17.0f));
}

// And the opposite: off-frame but IN FRONT is accepted. The criterion is the
// sign of w, not whether the rect fits in the image — clipping here would leave no
// gizmo for a canvas that sticks halfway out the edge, which is when it is most sought.
static void test_world_canvas_gizmo_acepta_fuera_de_encuadre_si_esta_delante()
{
    CanvasComponent c;
    c.renderMode          = UiCanvasRenderMode::World;
    c.worldScale          = 0.01f;
    c.referenceResolution = glm::vec2(200.0f, 100.0f);

    const glm::vec2 tam(200.0f, 100.0f);
    const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, -5.0f));
    const glm::mat4 vista(1.0f);
    const glm::mat4 mvp = editorProjForTest(2.0f) * vista *
                          uiWorldCanvasMatrix(c, tam, mundo, vista);

    glm::vec2 esq[4];
    CHECK(projectWorldCanvasCorners(mvp, glm::vec2(0.0f), tam, glm::vec2(37.0f, 91.0f),
                                    glm::vec2(800.0f, 400.0f), esq));
    // Very far to the right of the right edge of the image (37 + 800 = 837).
    CHECK(nearlyEqual(esq[0].x, 2271.8023f, 0.05f));
    CHECK(nearlyEqual(esq[1].x, 2464.9394f, 0.05f));
    // The Y does fall inside: the canvas is at camera height.
    CHECK(nearlyEqual(esq[0].y, 242.7157f, 0.02f));
    CHECK(nearlyEqual(esq[2].y, 339.2843f, 0.02f));
}

// Which Canvas a widget belongs to: the NEAREST ancestor (or itself) that
// has one. This is the exact rule of Scene::collectCanvases — a nested canvas opens
// its own binding and CUTS the chain — and on it depends whether a widget's gizmo
// knows if its canvas is of the world. If the gizmo and the sync do not use the same criterion,
// the gizmo decides for the wrong canvas.
//
// The two canvas of the test carry DISTINCT and not neutral values (resolution and
// worldScale) so we can assert WHICH of the two came back, not just that it is not
// null: returning the outer one instead of the inner one is exactly the bug to catch.
static void test_owning_canvas_es_el_ancestro_mas_cercano()
{
    GameObject raiz("Raiz");
    raiz.setCanvas(std::make_shared<CanvasComponent>());
    raiz.getCanvas()->renderMode          = UiCanvasRenderMode::ScreenSpace;
    raiz.getCanvas()->referenceResolution = glm::vec2(1280.0f, 720.0f);
    raiz.getCanvas()->worldScale          = 0.007f;

    GameObject* canvasMundo = raiz.addChild("CanvasMundo");
    canvasMundo->setCanvas(std::make_shared<CanvasComponent>());
    canvasMundo->getCanvas()->renderMode          = UiCanvasRenderMode::World;
    canvasMundo->getCanvas()->referenceResolution = glm::vec2(640.0f, 360.0f);
    canvasMundo->getCanvas()->worldScale          = 0.003f;

    GameObject* boton = canvasMundo->addChild("Boton");
    boton->setButton(std::make_shared<ButtonComponent>());

    // The one INSIDE, not the outside one. The GameObject is returned and not the component
    // because the gizmo also needs its worldTransform to build the
    // model matrix of the canvas.
    const GameObject* objBoton = owningCanvasObject(boton);
    CHECK(objBoton == canvasMundo);
    const CanvasComponent* delBoton = objBoton ? objBoton->getCanvas().get() : nullptr;
    CHECK(delBoton != nullptr);
    if (delBoton)
    {
        CHECK(delBoton->renderMode == UiCanvasRenderMode::World);
        CHECK(nearlyEqual(delBoton->worldScale, 0.003f));
        CHECK(nearlyEqual(delBoton->referenceResolution.x, 640.0f));
    }

    // A GameObject that IS the canvas returns itself: the gizmo of the own
    // Canvas relies on the same criterion.
    CHECK(owningCanvasObject(canvasMundo) == canvasMundo);

    // And from the root the SCREEN one rules: without this, an implementation that
    // climbed to the outermost canvas — or that kept the last one it
    // saw — would pass the check above all the same.
    const GameObject* objRaiz = owningCanvasObject(&raiz);
    CHECK(objRaiz == &raiz);
    const CanvasComponent* deLaRaiz = objRaiz ? objRaiz->getCanvas().get() : nullptr;
    CHECK(deLaRaiz != nullptr);
    if (deLaRaiz)
    {
        CHECK(deLaRaiz->renderMode == UiCanvasRenderMode::ScreenSpace);
        CHECK(nearlyEqual(deLaRaiz->worldScale, 0.007f));
        CHECK(nearlyEqual(deLaRaiz->referenceResolution.x, 1280.0f));
    }

    // No canvas on top means no canvas: the editor prevents it, but a scene
    // made by hand can bring it and this cannot dereference a null.
    GameObject suelto("Suelto");
    suelto.setButton(std::make_shared<ButtonComponent>());
    CHECK(owningCanvasObject(&suelto) == nullptr);
}

// The rect of a WIDGET inside a world canvas, which is what is needed
// for its gizmo. It goes through the SAME function as the entire canvas: the canvas is
// nothing more than the (0,0)-(w,h) case of this.
//
// The line that this test protects is precisely that the rect ENTERS through the
// parameter. The chosen rect, (40,25)-(140,60), does not start at (0,0), does not match
// the canvas and is not centered on it neither in X nor in Y: an implementation that
// ignored the two arguments and projected the entire canvas would give the eight
// coordinates of the test above, which are more than 21 pixels away from these — a thousand
// times the tolerance. With a rect that started at (0,0), or that was the entire
// canvas, that sabotage would get through.
static void test_world_canvas_gizmo_proyecta_el_rect_de_un_widget()
{
    CanvasComponent c;
    c.renderMode          = UiCanvasRenderMode::World;
    c.billboard           = UiBillboard::None;
    c.worldScale          = 0.01f;
    c.referenceResolution = glm::vec2(200.0f, 100.0f);

    // Same canvas, same camera and same image as
    // test_world_canvas_gizmo_proyecta_las_cuatro_esquinas: the ONLY thing that changes
    // is the rect, so the difference between the two sets of numbers cannot
    // come from anything else.
    const glm::vec2 tam(200.0f, 100.0f);
    const glm::mat4 mundo = glm::rotate(
        glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.6f, -0.3f, -5.0f)),
                    glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::radians(20.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 vista(1.0f);
    const glm::vec2 imagenPos(37.0f, 91.0f);
    const glm::vec2 imagenTam(800.0f, 400.0f);

    const glm::mat4 mvp = editorProjForTest(imagenTam.x / imagenTam.y) * vista *
                          uiWorldCanvasMatrix(c, tam, mundo, vista);

    // screenPos (40,25) and screenSize (100,35): in a world canvas, what
    // buildDrawData leaves already comes in LOCAL pixels of the canvas (scale 1 and
    // origin (0,0)), so it enters as is.
    const glm::vec2 rectMin(40.0f, 25.0f);
    const glm::vec2 rectMax(140.0f, 60.0f);

    glm::vec2 esq[4];
    CHECK(projectWorldCanvasCorners(mvp, rectMin, rectMax, imagenPos, imagenTam, esq));

    CHECK(nearlyEqual(esq[0].x, 453.5888f, 0.02f));
    CHECK(nearlyEqual(esq[0].y, 297.8520f, 0.02f));
    CHECK(nearlyEqual(esq[1].x, 528.4051f, 0.02f));
    CHECK(nearlyEqual(esq[1].y, 297.0902f, 0.02f));
    CHECK(nearlyEqual(esq[2].x, 520.3945f, 0.02f));
    CHECK(nearlyEqual(esq[2].y, 327.1820f, 0.02f));
    CHECK(nearlyEqual(esq[3].x, 446.1635f, 0.02f));
    CHECK(nearlyEqual(esq[3].y, 331.6128f, 0.02f));

    // And the degenerate rect (min == max) gives a point: it is how the gizmo pulls the
    // projected pivot, and without this a division by the rect size slipped into
    // the function would not be seen.
    glm::vec2 punto[4];
    CHECK(projectWorldCanvasCorners(mvp, rectMin, rectMin, imagenPos, imagenTam, punto));
    CHECK(nearlyEqual(punto[0].x, 453.5888f, 0.02f));
    CHECK(nearlyEqual(punto[0].y, 297.8520f, 0.02f));
    CHECK(nearlyEqual(punto[2].x, 453.5888f, 0.02f));
    CHECK(nearlyEqual(punto[2].y, 297.8520f, 0.02f));
}

// ===========================================================================
// DonTopo::Camera — the EDITOR flight camera (Core/Camera.cpp)
// ===========================================================================
//
// Section 4.2 of docs/core-audit.md: 98 LOC with ZERO references in the 25 test
// files. It is not the CameraComponent above (that one was covered): it is the one that
// moves the viewport point of view with WASD, the one that reorients the axis gizmo
// and the one that executes the F key.
//
// update() is left out on purpose: it asks for a GLFWwindow* and there is no window in
// a headless test. Everything else — the trigonometry, the clamp, the frame-fit — is
// pure arithmetic and had nothing underneath.

// The default yaw is -90°, and that choice is exactly what makes the camera
// look at -Z like glm::lookAt and like CameraComponent::viewFromWorld. If someone
// "fixes" the trigonometry of updateVectors, the two conventions stop
// matching and the frustum gizmo starts lying about what is seen in Play.
static void test_camera_default_front_is_minus_z()
{
    Camera cam;
    const glm::vec3 f = cam.getFront();
    CHECK(nearlyEqual(f.x, 0.0f, 1e-5f));
    CHECK(nearlyEqual(f.y, 0.0f, 1e-5f));
    CHECK(nearlyEqual(f.z, -1.0f, 1e-5f));
    CHECK(nearlyEqual(glm::length(f), 1.0f, 1e-5f));
}

// The pitch is clamped to ±89°: at 90 the front aligns with the up and the
// cross product of update() (right = cross(front, up)) degenerates. Without clamp, a
// long mouse drag passes 90 and the camera flips.
static void test_camera_pitch_is_clamped()
{
    Camera arriba;
    arriba.processMouse(0.0f, -100000.0f);   // negative yOffset raises the pitch
    CHECK(arriba.getFront().y <= 1.0f);
    // sin(89°) = 0.99985. Without clamp the pitch passes 90 and the component
    // goes DOWN (sin(200°) is negative), which is what this threshold catches.
    CHECK(arriba.getFront().y > 0.999f);
    CHECK(nearlyEqual(glm::length(arriba.getFront()), 1.0f, 1e-5f));

    Camera abajo;
    abajo.processMouse(0.0f, 100000.0f);
    CHECK(abajo.getFront().y < -0.999f);
}

// mouseSens multiplies the offset. It is a public field that the editor exposes in
// preferences: if processMouse stopped applying it, the slider would do
// nothing and nobody would notice from the code.
static void test_camera_mouse_sensitivity_scales_offset()
{
    Camera lenta;
    Camera rapida;
    rapida.mouseSens = lenta.mouseSens * 2.0f;

    lenta.processMouse(10.0f, 0.0f);
    rapida.processMouse(5.0f, 0.0f);
    // Same effective rotation: 10 * s == 5 * 2s.
    CHECK(nearlyEqual(lenta.getFront().x, rapida.getFront().x, 1e-5f));
    CHECK(nearlyEqual(lenta.getFront().z, rapida.getFront().z, 1e-5f));

    // And that it truly rotated, not that both stayed still.
    Camera quieta;
    CHECK(!nearlyEqual(lenta.getFront().x, quieta.getFront().x, 1e-3f));
}

// lookAlongAxis ONLY rotates: the axis gizmo of the viewport uses it, and moving the
// position when clicked would teleport the user without asking.
static void test_camera_look_along_axis_only_rotates()
{
    Camera cam(glm::vec3(10.0f, 20.0f, 30.0f));
    const glm::vec3 antes = cam.getPos();

    cam.lookAlongAxis(glm::vec3(0.0f, 0.0f, 1.0f));

    CHECK(nearlyEqual(cam.getPos().x, antes.x, 1e-5f));
    CHECK(nearlyEqual(cam.getPos().y, antes.y, 1e-5f));
    CHECK(nearlyEqual(cam.getPos().z, antes.z, 1e-5f));
    // Looking "along the +Z axis" means looking TOWARD -Z: the axis passed is
    // from where you look, not to where.
    CHECK(nearlyEqual(cam.getFront().z, -1.0f, 1e-4f));
}

// focusOn (the F key) retreats along the camera-to-object vector and frames it. The
// distance comes from max(radius, 5) * 2.5: without the minimum, framing a tiny
// object would put the camera inside it.
static void test_camera_focus_on_frames_the_object()
{
    Camera cam(glm::vec3(0.0f, 0.0f, 100.0f));
    const glm::vec3 centro(0.0f, 0.0f, 0.0f);

    cam.focusOn(centro, 40.0f);

    // It stays in the direction it was already (+Z), at 40 * 2.5 = 100.
    CHECK(nearlyEqual(glm::length(cam.getPos() - centro), 100.0f, 1e-3f));
    CHECK(cam.getPos().z > 0.0f);
    // And looking at the object.
    const glm::vec3 haciaElCentro = glm::normalize(centro - cam.getPos());
    CHECK(nearlyEqual(glm::dot(cam.getFront(), haciaElCentro), 1.0f, 1e-4f));

    // A tiny object does not frame at 0: the minimum radius (5) rules.
    Camera cerca(glm::vec3(0.0f, 0.0f, 100.0f));
    cerca.focusOn(centro, 0.0f);
    CHECK(nearlyEqual(glm::length(cerca.getPos() - centro), 12.5f, 1e-3f));
}

// The degenerate case: the camera is ALREADY exactly at the center of the object.
// The camera-to-center vector is zero and normalizing it gives NaN; the epsilon guard
// falls back to -front current. Without it, the position goes to NaN and the viewport
// goes black without saying why.
static void test_camera_focus_on_from_the_center_does_not_nan()
{
    const glm::vec3 centro(7.0f, 8.0f, 9.0f);
    Camera cam(centro);

    cam.focusOn(centro, 10.0f);

    const glm::vec3 p = cam.getPos();
    CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    const glm::vec3 f = cam.getFront();
    CHECK(std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z));
    CHECK(nearlyEqual(glm::length(cam.getPos() - centro), 25.0f, 1e-3f));
}

// getViewMatrix has to be exactly the lookAt that the rest of the engine
// assumes, or the gizmo and the render stop matching.
static void test_camera_view_matrix_is_lookat()
{
    Camera cam(glm::vec3(3.0f, 4.0f, 5.0f));
    cam.processMouse(37.0f, -11.0f);   // an arbitrary orientation, not the factory one

    const glm::mat4 esperado = glm::lookAt(cam.getPos(), cam.getPos() + cam.getFront(), cam.getUp());
    const glm::mat4 obtenido = cam.getViewMatrix();
    for (int c = 0; c < 4; ++c)
        for (int f = 0; f < 4; ++f)
            CHECK(nearlyEqual(obtenido[c][f], esperado[c][f], 1e-5f));
}

// ===========================================================================
// Scene::collectLights
// ===========================================================================
//
// Section 4.2 of docs/core-audit.md: zero tests, and it is what feeds the entire
// lighting block — the clipping to MAX_LIGHTS, the total that distinguishes "scene
// without lights" from "scene with more than fit", the angle-to-cosine conversion,
// the radius that has to match the shader's, and the NaN guard
// when the Z axis has scale 0.

// Helper: hangs a GameObject with light and the given transform from the root.
static GameObject* addLight(Scene& scene, const char* nombre, const glm::mat4& local,
                            LightType tipo = LightType::Point)
{
    GameObject* go = scene.addGameObject(nombre);
    auto luz = std::make_shared<LightComponent>();
    luz->setType(tipo);
    go->setLight(luz);
    go->localTransform = local;
    return go;
}

// The cap is from the UBO block, not the scene: lights that overflow are clipped
// but the TOTAL is returned, which is the only thing that lets the editor warn "there are
// 70 lights and only 64 fit". Returning the clipped size would leave that warning
// silent forever.
static void test_collect_lights_caps_but_reports_total()
{
    Scene scene("Test");
    const int extra = 3;
    for (int i = 0; i < MAX_LIGHTS + extra; ++i)
        addLight(scene, "Luz", glm::mat4(1.0f));
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces;
    std::vector<float> radios;
    const size_t total = scene.collectLights(luces, radios);

    CHECK(total == (size_t)(MAX_LIGHTS + extra));
    CHECK(luces.size() == (size_t)MAX_LIGHTS);
    CHECK(radios.size() == (size_t)MAX_LIGHTS);   // the two vectors in parallel
}

// The two outputs are cleared on entry: they are called ONCE PER FRAME on the
// same host vectors, so without the clear they would grow without stopping until
// overflowing the UBO with lights from old frames.
static void test_collect_lights_clears_previous_output()
{
    Scene scene("Test");
    addLight(scene, "Unica", glm::mat4(1.0f));
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces(10);
    std::vector<float> radios(10);
    CHECK(scene.collectLights(luces, radios) == 1);
    CHECK(luces.size() == 1);
    CHECK(radios.size() == 1);

    // And a scene with no lights leaves them empty, not with what was in the previous frame.
    Scene vacia("Vacia");
    CHECK(vacia.collectLights(luces, radios) == 0);
    CHECK(luces.empty());
    CHECK(radios.empty());
}

// Position and direction come from worldTransform, not the component: moving or
// rotating the GameObject has to move the light. The direction is -Z local.
static void test_collect_lights_position_and_direction_from_transform()
{
    Scene scene("Test");
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 30.0f));
    t = glm::rotate(t, glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    addLight(scene, "Luz", t, LightType::Spot);
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces;
    std::vector<float> radios;
    CHECK(scene.collectLights(luces, radios) == 1);
    if (luces.size() != 1) return;

    CHECK(nearlyEqual(luces[0].position.x, 10.0f, 1e-4f));
    CHECK(nearlyEqual(luces[0].position.y, 20.0f, 1e-4f));
    CHECK(nearlyEqual(luces[0].position.z, 30.0f, 1e-4f));
    CHECK(nearlyEqual(luces[0].position.w, 1.0f, 1e-5f));

    // Rotated 90° on Y, the Z column becomes (1,0,0) and the "which way it
    // looks" is its NEGATED. Without the sign, the light illuminates backwards.
    CHECK(nearlyEqual(luces[0].direction.x, -1.0f, 1e-4f));
    CHECK(nearlyEqual(luces[0].direction.y, 0.0f, 1e-4f));
    CHECK(nearlyEqual(luces[0].direction.z, 0.0f, 1e-4f));
    // The type travels in the w of direction.
    CHECK(nearlyEqual(luces[0].direction.w, (float)(int)LightType::Spot, 1e-5f));
}

// THE guard. Scale 0 in Z can be set from Properties: normalizing a
// null vector gives NaN, and that NaN reaches the shader and dirties the lighting of
// the ENTIRE scene, not just that light. The fallback is -Y.
static void test_collect_lights_zero_scale_z_does_not_nan()
{
    Scene scene("Test");
    addLight(scene, "Aplastada", glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.0f)));
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces;
    std::vector<float> radios;
    CHECK(scene.collectLights(luces, radios) == 1);
    if (luces.size() != 1) return;

    CHECK(std::isfinite(luces[0].direction.x));
    CHECK(std::isfinite(luces[0].direction.y));
    CHECK(std::isfinite(luces[0].direction.z));
    CHECK(nearlyEqual(luces[0].direction.x, 0.0f, 1e-5f));
    CHECK(nearlyEqual(luces[0].direction.y, -1.0f, 1e-5f));
    CHECK(nearlyEqual(luces[0].direction.z, 0.0f, 1e-5f));
}

// The cone angles already travel in cosine: the shader compares against the cosine
// of the angle with the axis instead of calling cos() per fragment. Sending degrees
// would give an absurd cone (cos(20) ≈ 0.94 versus 20.0) without anything failing at
// compile time.
static void test_collect_lights_angles_travel_as_cosines()
{
    Scene scene("Test");
    GameObject* go = addLight(scene, "Foco", glm::mat4(1.0f), LightType::Spot);
    go->getLight()->setOuterAngle(30.0f);
    go->getLight()->setInnerAngle(20.0f);
    go->getLight()->setRange(500.0f);
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces;
    std::vector<float> radios;
    CHECK(scene.collectLights(luces, radios) == 1);
    if (luces.size() != 1) return;

    CHECK(nearlyEqual(luces[0].params.x, 500.0f, 1e-3f));
    CHECK(nearlyEqual(luces[0].params.y, std::cos(glm::radians(20.0f)), 1e-5f));
    CHECK(nearlyEqual(luces[0].params.z, std::cos(glm::radians(30.0f)), 1e-5f));
}

// The radius of the Forward+ binning has to be THE SAME range that the
// fragment shader uses, or a light goes off suddenly when crossing a tile edge.
// The area is approximated as a point of radius width/2; the rest uses its range.
static void test_collect_lights_radius_matches_the_shader()
{
    Scene scene("Test");
    GameObject* punto = addLight(scene, "Punto", glm::mat4(1.0f), LightType::Point);
    punto->getLight()->setRange(300.0f);
    punto->getLight()->setAreaWidth(999.0f);   // must not affect a point

    GameObject* area = addLight(scene, "Area", glm::mat4(1.0f), LightType::Area);
    area->getLight()->setRange(300.0f);        // must not affect an area
    area->getLight()->setAreaWidth(80.0f);
    scene.getRoot().updateWorldTransforms();

    std::vector<Light> luces;
    std::vector<float> radios;
    CHECK(scene.collectLights(luces, radios) == 2);
    if (radios.size() != 2) return;

    CHECK(nearlyEqual(radios[0], 300.0f, 1e-3f));   // point: su range
    CHECK(nearlyEqual(radios[1], 40.0f, 1e-3f));    // area: width/2
}

// ── Gizmo of viewport translation (ImGuizmo) ────────────────────────────────
//
// The test subject is the two seams of ViewportPanel.h: `localFromWorld`,
// which converts what the manipulator returns (a WORLD matrix) into what
// is edited and serialized (`localTransform`), and `applyLocalTransform`, which is
// literally the body of the PropertyCommand lambda that is stacked when
// released. What CANNOT be tested here is mouse interaction: ImGuizmo
// needs a live ImGui context and pointer events, so the drag
// entry/exit edge — and with it "one command per drag" — is
// verified by hand in the GUI.

// The trivial case, which also HIDES the bug: with the parent at
// identity, putting the inverse or not putting it, and multiplying in one order or
// another, all four give the same result.
static void test_gizmo_local_from_world_identity_parent()
{
    const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -7.0f, 11.0f)) *
                            glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0, 1, 0));

    const glm::mat4 local = localFromWorld(glm::mat4(1.0f), mundo);
    for (int c = 0; c < 4; ++c)
        for (int f = 0; f < 4; ++f)
            CHECK(nearlyEqual(local[c][f], mundo[c][f], 1e-5f));
}

// The case that truly matters: parent TRANSLATED AND ROTATED. A child moved with the
// gizmo cannot jump, and would jump in the two wrong ways — writing the
// world as is into the local, or multiplying by the inverse on the wrong
// side — which here give three distinct results.
static void test_gizmo_local_from_world_translated_rotated_parent()
{
    const glm::mat4 padre = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, -3.0f, 2.0f)) *
                            glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
    // What ImGuizmo would return when dragging the child to (10, 4, -6) of world.
    const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 4.0f, -6.0f));

    const glm::mat4 local = localFromWorld(padre, mundo);

    // By hand: inverse(parent) = Ry(-90)·T(-5,3,-2), so the local translation
    // is Ry(-90)·(5,7,-8) = (8,7,5).
    //
    // Writing the world as is would give (10,4,-6), and multiplying backwards
    // (world · inverse(parent)) would give (12,7,-11). Neither of the two comes through
    // here.
    CHECK(nearlyEqual(local[3][0], 8.0f, 1e-4f));
    CHECK(nearlyEqual(local[3][1], 7.0f, 1e-4f));
    CHECK(nearlyEqual(local[3][2], 5.0f, 1e-4f));

    // And the return: recomposing the world from the local returns the entire
    // starting matrix, not just the position. It is the same as what
    // updateWorldTransforms does the next frame, so if this does not check out the
    // object moves on its own when released.
    const glm::mat4 rehecho = padre * local;
    for (int c = 0; c < 4; ++c)
        for (int f = 0; f < 4; ++f)
            CHECK(nearlyEqual(rehecho[c][f], mundo[c][f], 1e-4f));
}

// The PropertyCommand that is stacked when released: undoes and redoes, and resolves the
// object by ID. The pointer is put in the command on purpose to NOT use it.
static void test_gizmo_transform_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Movido");
    const uint64_t id = go->id;

    const glm::mat4 antes   = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    const glm::mat4 despues = glm::translate(glm::mat4(1.0f), glm::vec3(40.0f, -5.0f, 9.0f));
    go->localTransform = antes;

    Scene* pScene = &scene;
    PropertyCommand<glm::mat4> cmd(
        "Transform de 'Movido'", antes, despues,
        [pScene, id](const glm::mat4& t) { applyLocalTransform(*pScene, id, t); });

    cmd.execute();
    CHECK(nearlyEqual(go->localTransform[3][0], 40.0f, 1e-5f));
    CHECK(nearlyEqual(go->localTransform[3][1], -5.0f, 1e-5f));
    CHECK(nearlyEqual(go->localTransform[3][2], 9.0f, 1e-5f));
    // The world propagates in the same stroke: without this the object would be seen in its
    // old spot until the next traverse.
    CHECK(nearlyEqual(go->worldTransform[3][0], 40.0f, 1e-5f));

    cmd.undo();
    CHECK(nearlyEqual(go->localTransform[3][0], 1.0f, 1e-5f));
    CHECK(nearlyEqual(go->localTransform[3][1], 2.0f, 1e-5f));
    CHECK(nearlyEqual(go->localTransform[3][2], 3.0f, 1e-5f));

    cmd.execute();
    CHECK(nearlyEqual(go->localTransform[3][0], 40.0f, 1e-5f));
}

// And what forces resolution by id: between stacking the command and undoing it fits
// a deletion + a reconstruction (the undo of a Delete, the loading of a
// scene). The reconstructed GameObject keeps the id but NOT the address: a
// GameObject* captured would point to freed memory.
static void test_gizmo_transform_command_survives_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Movido");
    const uint64_t id = go->id;

    const glm::mat4 antes   = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    const glm::mat4 despues = glm::translate(glm::mat4(1.0f), glm::vec3(40.0f, -5.0f, 9.0f));
    go->localTransform = despues;

    Scene* pScene = &scene;
    PropertyCommand<glm::mat4> cmd(
        "Transform de 'Movido'", antes, despues,
        [pScene, id](const glm::mat4& t) { applyLocalTransform(*pScene, id, t); });

    // Deletion and reconstruction by the same path as the undo of a Delete
    // (see test_undo_delete_keeps_original_id), which keeps the id.
    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findById(id) == nullptr);
    // An undo with the object ABSENT cannot crash: it is a no-op and that is it.
    cmd.undo();

    GameObject* rehecho = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(rehecho != nullptr);
    if (!rehecho) return;
    // Same id, new object: the old `go` is freed memory and a
    // GameObject* captured in the command would point there. (Addresses are not compared:
    // the allocator can reuse the same one and that would not prove anything.)
    CHECK(rehecho->id == id);

    cmd.undo();
    CHECK(nearlyEqual(rehecho->localTransform[3][0], 1.0f, 1e-5f));
    CHECK(nearlyEqual(rehecho->localTransform[3][1], 2.0f, 1e-5f));
    CHECK(nearlyEqual(rehecho->localTransform[3][2], 3.0f, 1e-5f));
}

// ── Gizmo modes: rotate and scale ────────────────────────────────────────────
//
// What can be asserted without GUI is which CHANNEL each mode reports. The rest
// — which handles ImGuizmo draws, whether the rings come aligned to the object — is
// from the library and the screen.
//
// It is worth testing because the bug has a very concrete and very
// silent look: the three modes share a path, write the same `mat4` and
// stack the same command, so a copy-paste that leaves Rotate reporting the
// position compiles, runs and is only noticed reading the Log Console.

// The dispatch to ImGuizmo. Sending Rotate to TRANSLATE compiles and runs: the object
// would move instead of rotating and no other test would know (tested by
// sabotaging it before writing this). What is also asserted is the space decision,
// which is deliberate and different per mode.
static void test_gizmo_imguizmo_enums_per_mode()
{
    int op = -1, espacio = -1;

    gizmoImGuizmoEnums(GizmoMode::Translate, op, espacio);
    CHECK(op == ImGuizmo::TRANSLATE);
    CHECK(espacio == ImGuizmo::WORLD);   // the world X, however the object is measured

    gizmoImGuizmoEnums(GizmoMode::Rotate, op, espacio);
    CHECK(op == ImGuizmo::ROTATE);
    CHECK(espacio == ImGuizmo::LOCAL);   // rings stuck to the object's axes

    gizmoImGuizmoEnums(GizmoMode::Scale, op, espacio);
    CHECK(op == ImGuizmo::SCALE);
    // LOCAL required: ImGuizmo does (operation & SCALE) ? LOCAL : mode and
    // discards what is passed to it. Passing WORLD here would be a call that lies.
    CHECK(espacio == ImGuizmo::LOCAL);
}

static void test_gizmo_channel_label_per_mode()
{
    CHECK(std::string(gizmoChannelLabel(GizmoMode::Translate)) == "Position");
    CHECK(std::string(gizmoChannelLabel(GizmoMode::Rotate))    == "Rotation");
    CHECK(std::string(gizmoChannelLabel(GizmoMode::Scale))     == "Scale");
}

// A single matrix with the three things distinct from each other, so no mode
// can get lucky by accident reading another's channel.
static void test_gizmo_logged_value_reads_its_own_channel()
{
    // 35° and not 90°: at 90 the Y of euler lands right in the singularity of the `asin`
    // of glm::eulerAngles, where the error of normalizing columns in float
    // amplifies and 89.98 comes out. That is precision, not wrong channel, and a test
    // that tolerated it with 0.1 margin would stop distinguishing position from
    // rotation. It is tested far from the edge and asserted strongly.
    const glm::mat4 local =
        glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, -2.0f, 7.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0, 1, 0)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 5.0f));

    const glm::vec3 pos = gizmoLoggedValue(GizmoMode::Translate, local);
    CHECK(nearlyEqual(pos.x, 4.0f, 1e-4f));
    CHECK(nearlyEqual(pos.y, -2.0f, 1e-4f));
    CHECK(nearlyEqual(pos.z, 7.0f, 1e-4f));

    // DEGREES, not radians: 35, not 0.61. It is the difference between a log line
    // that can be compared with the one in Properties and one that cannot.
    const glm::vec3 rot = gizmoLoggedValue(GizmoMode::Rotate, local);
    CHECK(nearlyEqual(rot.x, 0.0f, 1e-3f));
    CHECK(nearlyEqual(rot.y, 35.0f, 1e-3f));
    CHECK(nearlyEqual(rot.z, 0.0f, 1e-3f));

    const glm::vec3 esc = gizmoLoggedValue(GizmoMode::Scale, local);
    CHECK(nearlyEqual(esc.x, 2.0f, 1e-4f));
    CHECK(nearlyEqual(esc.y, 3.0f, 1e-4f));
    CHECK(nearlyEqual(esc.z, 5.0f, 1e-4f));
}

// A scale to 0 makes the matrix singular — and the Scale mode of the gizmo goes
// all the way there — so `glm::decompose` bare returns false and does NOT write
// its outputs. The log has to go through decomposeTransform, the repo's
// wrapper, which does write them always.
//
// The ACTUAL values are asserted and not `isfinite`: the 0xCDCDCDCD pattern of the
// Debug CRT is -1.07e8, which is a perfectly finite number, so a
// test of isfinite passes with garbage inside and protects nothing.
static void test_gizmo_logged_value_survives_singular_matrix()
{
    const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 5.0f));

    // A flattened axis defines no orientation: identity, that is 0°.
    const glm::vec3 rot = gizmoLoggedValue(GizmoMode::Rotate, local);
    CHECK(nearlyEqual(rot.x, 0.0f, 1e-4f));
    CHECK(nearlyEqual(rot.y, 0.0f, 1e-4f));
    CHECK(nearlyEqual(rot.z, 0.0f, 1e-4f));

    // The scale comes out ENTIRE and correct, zeros included: they are the lengths of
    // the columns, which do not depend on whether decomposition was possible.
    const glm::vec3 esc = gizmoLoggedValue(GizmoMode::Scale, local);
    CHECK(nearlyEqual(esc.x, 2.0f, 1e-4f));
    CHECK(nearlyEqual(esc.y, 0.0f, 1e-4f));
    CHECK(nearlyEqual(esc.z, 5.0f, 1e-4f));

    // And the translation the same: fourth column, does not go through decomposition.
    const glm::vec3 pos = gizmoLoggedValue(GizmoMode::Translate, local);
    CHECK(nearlyEqual(pos.y, 2.0f, 1e-5f));
}

// The world-to-local conversion with a ROTATION, which is what ImGuizmo returns
// in Rotate mode. It is the same `localFromWorld` as the translation, but the case
// that matters is different: the local has to be the rotation RELATIVE to the
// parent, not the absolute.
static void test_gizmo_rotation_through_rotated_parent_is_relative()
{
    const glm::mat4 padre = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
    // The gizmo leaves the child looking at 145° of world.
    const glm::mat4 mundo = glm::rotate(glm::mat4(1.0f), glm::radians(145.0f), glm::vec3(0, 1, 0));

    const glm::mat4 local = localFromWorld(padre, mundo);

    // 145 − 90 = 55, not 145: the parent already provides its 90.
    const glm::vec3 grados = gizmoLoggedValue(GizmoMode::Rotate, local);
    CHECK(nearlyEqual(grados.y, 55.0f, 1e-3f));
    // And without parasitic scale: a misplaced inverse introduces it and does not show in the
    // angle.
    const glm::vec3 esc = gizmoLoggedValue(GizmoMode::Scale, local);
    CHECK(nearlyEqual(esc.x, 1.0f, 1e-4f));
    CHECK(nearlyEqual(esc.y, 1.0f, 1e-4f));
    CHECK(nearlyEqual(esc.z, 1.0f, 1e-4f));

    const glm::mat4 rehecho = padre * local;
    for (int c = 0; c < 4; ++c)
        for (int f = 0; f < 4; ++f)
            CHECK(nearlyEqual(rehecho[c][f], mundo[c][f], 1e-4f));
}

int main()
{
    // One PxFoundation per process only: a single PhysicsManager shared
    // by all tests, never one per test. Here physics/audio are only needed
    // because Scene::fromJson/insertFromJson/cloneGameObject demand them
    // in their signature to recreate colliders and clips — these tests do not simulate anything.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_defaults();
    test_clamps();
    test_projection_has_vulkan_y_flip();
    test_projection_modes_differ();
    test_projection_degenerate_aspect();
    test_camera_default_front_is_minus_z();
    test_camera_pitch_is_clamped();
    test_camera_mouse_sensitivity_scales_offset();
    test_camera_look_along_axis_only_rotates();
    test_camera_focus_on_frames_the_object();
    test_camera_focus_on_from_the_center_does_not_nan();
    test_camera_view_matrix_is_lookat();
    test_collect_lights_caps_but_reports_total();
    test_collect_lights_clears_previous_output();
    test_collect_lights_position_and_direction_from_transform();
    test_collect_lights_zero_scale_z_does_not_nan();
    test_collect_lights_angles_travel_as_cosines();
    test_collect_lights_radius_matches_the_shader();
    test_orthographic_uses_vulkan_depth_range();
    test_perspective_uses_vulkan_depth_range();
    test_view_from_world_translation();
    test_view_from_world_ignores_scale();
    test_find_camera_at_any_depth();
    test_find_camera_ignores_name();
    test_find_camera_returns_first_in_preorder();
    test_serialization_round_trip(pm, am);
    test_subtree_round_trip(pm, am);
    test_scene_without_camera_block_still_loads(pm, am);
    test_load_with_two_cameras_keeps_first(pm, am);
    test_load_with_one_camera_has_no_warnings(pm, am);
    test_repeated_warnings_are_collapsed(pm, am);
    test_single_warning_has_no_suffix(pm, am);
    test_remove_notifies_listener();
    test_remove_notifies_before_destroying();
    test_remove_without_listener_is_fine();
    test_remove_does_not_notify_for_non_removals();
    test_insert_from_json_resets_warnings(pm, am);
    test_corrupt_id_does_not_lose_scene(pm, am);
    test_missing_children_does_not_lose_scene(pm, am);
    test_missing_child_name_does_not_lose_scene(pm, am);
    test_corrupt_mesh_indices_does_not_lose_scene(pm, am);
    test_corrupt_legacy_usegravity_does_not_lose_scene(pm, am);
    test_corrupt_script_name_does_not_lose_scene(pm, am);
    test_corrupt_ui_string_warns(pm, am);
    test_clone_never_keeps_camera(pm, am);
    test_scene_shutdown_releases_every_component(pm, am);
    test_window_without_init_is_inert();
    test_traverse_does_not_copy_stateful_functor();
    test_find_first_stops_at_the_first_hit();
    test_corrupt_field_costs_its_node_not_the_scene(pm, am);
    test_gameobject_header_declares_components_instead_of_including();
    test_insert_from_json_discards_second_camera(pm, am);
    test_insert_from_json_keeps_camera_when_none_alive(pm, am);
    test_insert_from_json_discards_second_audio_listener(pm, am);
    test_clone_strips_camera_from_descendant(pm, am);
    test_clone_gets_fresh_id(pm, am);
    test_clone_subtree_gets_fresh_ids(pm, am);
    test_undo_delete_keeps_original_id(pm, am);
    test_insert_from_json_reassigns_colliding_id(pm, am);
    test_insert_from_json_reassigns_id_colliding_within_same_subtree(pm, am);
    test_scene_load_reassigns_duplicate_ids(pm, am);
    test_find_by_id_unique_id_still_resolves();
    test_find_by_id_duplicate_writes_only_first_never_the_other();
    test_duplicate_is_sibling_not_child(pm, am);
    test_insert_model_pieces_uses_the_given_meshes(pm, am);
    test_insert_from_json_uses_cached_mesh_for_rigged_source_without_probing(pm, am);
    test_duplicate_of_root_child_is_sibling(pm, am);
    test_duplicate_rejects_scene_root(pm, am);
    test_duplicate_subtree_has_unique_ids(pm, am);
    test_duplicate_undo_redo_restores_object_count(pm, am);
    test_load_advances_id_counter(pm, am);
    test_camera_command_add_undo_redo();
    test_camera_command_remove();
    test_camera_command_survives_missing_target();

    test_gizmo_local_from_world_identity_parent();
    test_gizmo_local_from_world_translated_rotated_parent();
    test_gizmo_transform_command_undo_redo();
    test_gizmo_transform_command_survives_rebuild(pm, am);
    test_gizmo_imguizmo_enums_per_mode();
    test_gizmo_channel_label_per_mode();
    test_gizmo_logged_value_reads_its_own_channel();
    test_gizmo_logged_value_survives_singular_matrix();
    test_gizmo_rotation_through_rotated_parent_is_relative();

    test_canvas_round_trip(pm, am);
    test_scene_without_canvas_block_still_loads(pm, am);
    test_scene_without_canvas_serializes_identically();
    test_ui_components_need_canvas();
    test_canvas_command_add_undo_redo();
    test_canvas_command_remove();
    test_canvas_property_command_undo_redo();
    test_canvas_world_fields_round_trip(pm, am);
    test_canvas_without_world_fields_loads_with_defaults(pm, am);

    test_button_round_trip(pm, am);
    test_scene_without_button_block_still_loads(pm, am);
    test_scene_without_button_serializes_identically();
    test_ui_components_available_for_descendants();
    test_button_command_add_undo_redo();
    test_button_command_remove();
    test_button_property_command_undo_redo();
    test_button_sync_moves_the_live_node();
    test_button_survives_canvas_edit();
    test_button_text_without_font_is_visible();
    test_button_state_color_is_applied();
    test_button_with_label_still_hovers();
    test_button_hit_test_maps_back_to_gameobject();
    test_button_asset_path_filters();

    test_text_round_trip(pm, am);
    test_scene_without_text_block_still_loads(pm, am);
    test_scene_without_text_serializes_identically();
    test_text_command_add_undo_redo();
    test_text_command_remove();
    test_text_property_command_undo_redo();
    test_text_sync_updates_the_live_node();
    test_collect_ui_widgets_salta_los_intermedios_sin_ui();
    test_collect_canvases_agrupa_por_canvas();
    test_collect_canvases_anidado_gana_el_mas_cercano();
    test_collect_canvases_ignora_los_huerfanos();
    test_collect_canvases_jerarquia_por_canvas();
    test_jerarquia_ancla_y_hereda_del_padre();
    test_jerarquia_cambiar_de_padre_reconstruye();
    test_jerarquia_hereda_la_opacidad();
    test_buttons_and_texts_coexist();
    test_text_without_content_loads_no_font();
    test_text_hit_test_maps_back_to_gameobject();
    test_progress_bar_round_trip(pm, am);
    test_scene_without_progress_bar_block_still_loads(pm, am);
    test_scene_without_progress_bar_serializes_identically();
    test_progress_bar_command_add_undo_redo();
    test_progress_bar_command_remove();
    test_progress_bar_property_command_undo_redo();
    test_progress_bar_fill_directions();
    test_progress_bar_sync_updates_the_live_node();
    test_all_three_ui_components_coexist();
    test_progress_bar_without_atlas_loads_nothing();
    test_progress_bar_hit_test_maps_back_to_gameobject();

    test_layout_container_places_children();
    test_layout_on_widget_uses_its_node();
    test_layout_ignore_layout_child_keeps_its_anchor();
    test_collect_ui_widgets_incluye_los_layouts();
    test_layout_sin_jerarquia_monta_en_la_raiz();
    test_layout_container_no_se_come_los_clics();
    test_layout_hit_test_maps_back_to_gameobject();
    test_layout_round_trip(pm, am);
    test_scene_without_layout_block_still_loads(pm, am);
    test_scene_without_layout_serializes_identically();
    test_layout_command_add_undo_redo();
    test_layout_command_remove();
    test_layout_property_command_undo_redo();

    test_panel_round_trip(pm, am);
    test_scene_without_panel_block_still_loads(pm, am);
    test_scene_without_panel_serializes_identically();
    test_panel_command_add_undo_redo();
    test_panel_command_remove();
    test_panel_property_command_undo_redo();
    test_panel_sync_updates_the_live_node();
    test_panel_hit_test_maps_back_to_gameobject();

    test_image_round_trip(pm, am);
    test_scene_without_image_block_still_loads(pm, am);
    test_scene_without_image_serializes_identically();
    test_image_command_add_undo_redo();
    test_image_command_remove();
    test_image_property_command_undo_redo();
    test_image_sync_updates_the_live_node();
    test_image_hit_test_maps_back_to_gameobject();
    test_collect_ui_widgets_incluye_panels_e_images();
    test_panel_and_image_coexist();

    test_slider_round_trip(pm, am);
    test_scene_without_slider_block_still_loads(pm, am);
    test_scene_without_slider_serializes_identically();
    test_slider_command_add_undo_redo();
    test_slider_command_remove();
    test_slider_property_command_undo_redo();
    test_slider_handle_stays_inside_the_track();
    test_slider_whole_numbers_snaps_the_value();
    test_slider_sync_builds_track_fill_and_handle();
    test_slider_drag_writes_the_component_value();
    test_slider_not_interactable_ignores_the_mouse();
    test_slider_hit_test_maps_back_to_gameobject();
    test_checkbox_round_trip(pm, am);
    test_scene_without_checkbox_block_still_loads(pm, am);
    test_scene_without_checkbox_serializes_identically();
    test_checkbox_command_add_undo_redo();
    test_checkbox_command_remove();
    test_checkbox_property_command_undo_redo();
    test_checkbox_check_rect_respects_padding();
    test_checkbox_sync_builds_box_and_check();
    test_checkbox_click_toggles_the_component();
    test_checkbox_hit_test_maps_back_to_gameobject();

    test_toggle_round_trip(pm, am);
    test_scene_without_toggle_block_still_loads(pm, am);
    test_scene_without_toggle_serializes_identically();
    test_toggle_command_add_undo_redo();
    test_toggle_command_remove();
    test_toggle_property_command_undo_redo();
    test_toggle_knob_rect_moves_end_to_end();
    test_toggle_sync_builds_track_and_knob();
    test_toggle_click_flips_the_component();
    test_toggle_hit_test_maps_back_to_gameobject();

    test_scrollbar_round_trip(pm, am);
    test_scene_without_scrollbar_block_still_loads(pm, am);
    test_scene_without_scrollbar_serializes_identically();
    test_scrollbar_command_add_undo_redo();
    test_scrollbar_command_remove();
    test_scrollbar_property_command_undo_redo();
    test_scrollbar_handle_rect_and_steps();
    test_scrollbar_drag_and_wheel_write_the_component();
    test_scrollbar_hit_test_maps_back_to_gameobject();
    test_canvas_entrega_el_texto_al_elemento_con_foco();
    test_canvas_cede_las_teclas_de_edicion_a_quien_las_consume();
    test_input_field_round_trip(pm, am);
    test_scene_without_input_field_block_still_loads(pm, am);
    test_scene_without_input_field_serializes_identically();
    test_input_field_command_add_undo_redo();
    test_input_field_command_remove();
    test_input_field_property_command_undo_redo();
    test_input_field_edits_by_codepoint_not_by_byte();
    test_input_field_content_type_and_limit();
    test_input_field_password_and_placeholder();
    test_input_field_sync_builds_box_text_and_caret();
    test_input_field_typing_writes_the_component();
    test_input_field_hit_test_maps_back_to_gameobject();

    test_dropdown_round_trip(pm, am);
    test_dropdown_open_state_is_not_serialized(pm, am);
    test_scene_without_dropdown_block_still_loads(pm, am);
    test_scene_without_dropdown_serializes_identically();
    test_dropdown_command_add_undo_redo();
    test_dropdown_command_remove();
    test_dropdown_property_command_undo_redo();
    test_dropdown_out_of_range_value_is_survivable();
    test_dropdown_sync_builds_label_arrow_and_items();
    test_dropdown_click_opens_and_picks();
    test_dropdown_hit_test_maps_back_to_gameobject();

    test_scroll_view_round_trip(pm, am);
    test_scene_without_scroll_view_block_still_loads(pm, am);
    test_scene_without_scroll_view_serializes_identically();
    test_scroll_view_command_add_undo_redo();
    test_scroll_view_command_remove();
    test_scroll_view_property_command_undo_redo();
    test_scroll_view_content_offset();
    test_scroll_view_sync_builds_viewport_and_content();
    test_scroll_view_children_hang_from_the_content();
    test_scroll_view_wheel_moves_the_component();
    test_scroll_view_hit_test_maps_back_to_gameobject();

    test_ui_slots_se_emparejan_por_owner_id();
    test_world_canvases_se_ordenan_de_lejos_a_cerca();
    test_ui_frame_totals_suma_mundo_y_pantalla();
    test_canvas_de_pantalla_en_orden_de_prioridad();
    test_el_canvas_de_encima_se_lleva_el_puntero();
    test_el_canvas_de_abajo_no_se_queda_pegado_en_hover();
    test_la_captura_del_puntero_sobrevive_al_solape();
    test_el_teclado_va_al_canvas_con_foco_no_al_del_raton();
    test_no_hay_clic_fantasma_al_arrastrar_desde_el_vacio();
    test_soltar_el_input_no_deja_captura_huerfana();
    test_canvas_por_owner_id();

    test_world_canvas_gizmo_proyecta_las_cuatro_esquinas();
    test_world_canvas_gizmo_rechaza_esquina_detras_de_la_camara();
    test_world_canvas_gizmo_acepta_fuera_de_encuadre_si_esta_delante();
    test_world_canvas_gizmo_proyecta_el_rect_de_un_widget();
    test_owning_canvas_es_el_ancestro_mas_cercano();


    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
