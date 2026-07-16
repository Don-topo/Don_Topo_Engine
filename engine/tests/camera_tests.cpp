// Test headless del CameraComponent y de la serialización/invariante de cámara
// en Scene (sin GUI). Plain main + asserts, sin framework — coherente con
// physics_tests.cpp.
//
// PhysX sólo admite UNA PxFoundation por proceso (crearla dos veces, aunque se
// libere entremedias, crashea). Por eso se crea un único PhysicsManager en
// main() y se pasa por referencia: aquí sólo hace falta porque Scene::fromJson/
// insertFromJson/cloneGameObject lo exigen en su firma para recrear colliders,
// no porque estos tests simulen física.
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Editor/Command.h"
#include <nlohmann/json.hpp>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Defaults a la escala de este repo, no a los de Unity.
static void test_defaults()
{
    CameraComponent c;
    CHECK(c.getMode() == CameraComponent::ProjectionMode::Perspective);
    CHECK(nearlyEqual(c.getFov(), 45.0f));
    CHECK(nearlyEqual(c.getOrthographicSize(), 100.0f));
    CHECK(nearlyEqual(c.getNear(), 1.0f));
    CHECK(nearlyEqual(c.getFar(), 2000.0f));
}

// Los clamps viven en el componente: un JSON editado a mano no puede instalar
// una proyección degenerada.
static void test_clamps()
{
    CameraComponent c;
    c.setFov(0.0f);      CHECK(c.getFov() >= 1.0f);
    c.setFov(500.0f);    CHECK(c.getFov() <= 179.0f);
    c.setOrthographicSize(-5.0f); CHECK(c.getOrthographicSize() > 0.0f);
    c.setNear(-5.0f);    CHECK(c.getNear() > 0.0f);
    // far nunca queda por debajo de near.
    c.setNear(10.0f);
    c.setFar(5.0f);
    CHECK(c.getFar() > c.getNear());
    // near nunca sobrepasa far, y hacerlo no debe mover far.
    CameraComponent d;
    d.setFar(100.0f);
    d.setNear(500.0f);
    CHECK(d.getNear() < d.getFar());
    CHECK(nearlyEqual(d.getFar(), 100.0f));
}

// El Y-flip de Vulkan va DENTRO de projectionMatrix: sus dos consumidores (UBO
// del Renderer y Gizmos::drawFrustum) lo necesitan.
static void test_projection_has_vulkan_y_flip()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);
    CHECK(p[1][1] < 0.0f);
}

// Perspectiva y ortográfica no pueden dar la misma matriz.
static void test_projection_modes_differ()
{
    CameraComponent c;
    glm::mat4 persp = c.projectionMatrix(1.0f);
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 ortho = c.projectionMatrix(1.0f);
    CHECK(persp != ortho);
    // En ortográfica, w del punto proyectado es 1 (sin división perspectiva).
    glm::vec4 clip = ortho * glm::vec4(0.0f, 0.0f, -50.0f, 1.0f);
    CHECK(nearlyEqual(clip.w, 1.0f));
}

// Un aspect degenerado (viewport de ancho 0 al minimizar) no debe producir NaN.
static void test_projection_degenerate_aspect()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(0.0f);
    CHECK(!std::isnan(p[0][0]));
}

// La cámara mira a -Z local (convención de glm/lookAt y de DonTopo::Camera,
// cuyo yaw por defecto de -90° da front = (0,0,-1)).
static void test_view_from_world_translation()
{
    glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 view  = CameraComponent::viewFromWorld(world);
    // El origen del mundo queda 10 unidades delante de la cámara, o sea en -Z.
    glm::vec4 p = view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(p.x, 0.0f));
    CHECK(nearlyEqual(p.y, 0.0f));
    CHECK(nearlyEqual(p.z, -10.0f));
}

// La escala del GameObject NO debe entrar en la view (deformaría la imagen).
static void test_view_from_world_ignores_scale()
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 unscaled = CameraComponent::viewFromWorld(t);
    glm::mat4 scaled   = CameraComponent::viewFromWorld(t * glm::scale(glm::mat4(1.0f), glm::vec3(5.0f)));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(nearlyEqual(unscaled[col][row], scaled[col][row]));
}

// findCamera() es la ÚNICA fuente de verdad del invariante "una cámara por
// escena": tiene que encontrarla esté donde esté, no solo colgando de la raíz.
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

// La cámara puede vivir en CUALQUIER GameObject, no solo en uno llamado
// "Camera".
static void test_find_camera_ignores_name()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("CualquierNombre");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == go);
    CHECK(scene.findCamera()->hasCameraComponent());
}

// Pre-orden: gana la primera en el recorrido, no una cualquiera.
static void test_find_camera_returns_first_in_preorder()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == a);
}

// Round-trip completo por toJson/fromJson. Los valores NO son los defaults a
// propósito: unos defaults se "preservarían" solos aunque el bloque no se
// serializara. near/far grandes cubren además el orden de carga (setNear clampa
// contra el far actual, así que far tiene que cargarse antes).
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

// Camino de subtreeToJson/insertFromJson — el que usan los comandos de
// Undo/Redo. Sin él, un Undo de Delete devolvería el GameObject sin su cámara.
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

// Back-compat: las escenas guardadas antes de este cambio no traen bloque
// "camera" y tienen que cargar igual (version sigue en 1).
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

// Escena con DOS cámaras (JSON editado a mano): gana la primera en pre-orden,
// la otra pierde SOLO el componente (su GameObject se conserva) y queda aviso.
// Así un .scene recuperable se abre igual, en vez de fallar la carga.
static void test_load_with_two_cameras_keeps_first(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("Primera");
    GameObject* b = scene.addGameObject("Segunda");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    // toJson serializa las dos: el invariante lo impone la carga, que es donde
    // puede llegar un fichero editado a mano.
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    int cameraCount = 0;
    loaded.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);

    GameObject* cam = loaded.findCamera();
    CHECK(cam != nullptr);
    if (cam) CHECK(cam->name == "Primera");
    // Los dos GameObjects siguen ahí: solo se cae el componente sobrante.
    CHECK(loaded.getRoot().children.size() == 2);
    CHECK(!loaded.lastWarnings().empty());
}

// Una escena con UNA cámara no genera avisos (el prune no es un falso positivo).
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

// Clonar un GameObject con cámara NO puede dar dos cámaras. Su único caller es
// Instantiate de Lua, que corre en Play: ningún gate de UI puede evitarlo, así
// que la regla vive en Scene.
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
    // El original conserva la suya y sigue siendo LA cámara de la escena.
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    int cameraCount = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);
}

// La cámara puede estar en un descendiente del subárbol clonado, no solo en su
// raíz.
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

// El Add/Remove de cámara pasa por el stack de Undo (a diferencia de los Add de
// collider/Rigidbody): si no, un Undo de Delete podría resucitar una cámara
// borrada estando ya otra en escena. Ver spec, "The One-Camera Invariant".
static void test_camera_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");

    // near > el far por defecto (2000) a propósito: apply() tiene que llamar a
    // setFar ANTES que a setNear (setNear clampa contra el far ACTUAL). Con
    // valores pequeños los dos órdenes dan el mismo resultado y la regresión
    // pasaría desapercibida; con near=3000 el orden inverso lo truncaría a
    // 1999.999 y este test cae.
    CameraState st{ CameraComponent::ProjectionMode::Orthographic, 60.0f, 300.0f, 3000.0f, 8000.0f };
    CameraComponentCommand cmd(scene, "Add Camera", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    cmd.undo();
    CHECK(!go->hasCameraComponent());
    CHECK(scene.findCamera() == nullptr);

    // Redo: los valores del state se conservan, no vuelve a los defaults.
    cmd.execute();
    CHECK(go->hasCameraComponent());
    const auto& c = go->getCameraComponent();
    CHECK(c->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(c->getFov(), 60.0f));
    CHECK(nearlyEqual(c->getOrthographicSize(), 300.0f));
    CHECK(nearlyEqual(c->getNear(), 3000.0f));
    CHECK(nearlyEqual(c->getFar(), 8000.0f));
}

// add=false invierte el sentido: execute quita, undo devuelve.
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

// El comando resuelve el GameObject por id en cada execute()/undo(), nunca
// guarda un puntero crudo: sobrevive a que el objeto se reconstruya entretanto.
static void test_camera_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");
    uint64_t id = go->id;
    CameraState st{ CameraComponent::ProjectionMode::Perspective, 45.0f, 100.0f, 1.0f, 2000.0f };
    CameraComponentCommand cmd(scene, "Add Camera", id, /*add=*/true, st);

    scene.removeGameObject(go);
    cmd.execute(); // no debe crashear: findById devuelve nullptr y sale
    CHECK(scene.findCamera() == nullptr);
}

int main()
{
    // Una sola PxFoundation por proceso: un único PhysicsManager compartido
    // por todos los tests, nunca uno por test. Aquí physics/audio solo hacen
    // falta porque Scene::fromJson/insertFromJson/cloneGameObject los exigen
    // en su firma pa recrear colliders y clips — estos tests no simulan nada.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_defaults();
    test_clamps();
    test_projection_has_vulkan_y_flip();
    test_projection_modes_differ();
    test_projection_degenerate_aspect();
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
    test_clone_never_keeps_camera(pm, am);
    test_clone_strips_camera_from_descendant(pm, am);
    test_camera_command_add_undo_redo();
    test_camera_command_remove();
    test_camera_command_survives_missing_target();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
