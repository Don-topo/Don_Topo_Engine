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
#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
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

// Vulkan clipea 0 <= z_clip <= w_clip, así que la proyección tiene que mapear
// near->0 y far->1. El default de glm (sin GLM_FORCE_DEPTH_ZERO_TO_ONE) mapea
// near->-1 pensando en OpenGL: en ortográfica eso tiraba la mitad cercana del
// rango entero (con near=1/far=2000 sólo se veía de 1000.5 en adelante).
static void test_orthographic_uses_vulkan_depth_range()
{
    CameraComponent c; // near=1, far=2000
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 p = c.projectionMatrix(1.0f);

    glm::vec4 atNear = p * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f);
    CHECK(nearlyEqual(atNear.z / atNear.w, 0.0f));
    glm::vec4 atFar = p * glm::vec4(0.0f, 0.0f, -2000.0f, 1.0f);
    CHECK(nearlyEqual(atFar.z / atFar.w, 1.0f));
    // Un objeto a la escala de este repo (cámara del sandbox a z=300) tiene que
    // quedar DENTRO del rango visible, no clipeado.
    glm::vec4 mid = p * glm::vec4(0.0f, 0.0f, -300.0f, 1.0f);
    CHECK(mid.z / mid.w > 0.0f);
    CHECK(mid.z / mid.w < 1.0f);
}

// Mismo contrato en perspectiva (ahí el fallo sólo recortaba los primeros ~2
// units, por eso pasaba desapercibido).
static void test_perspective_uses_vulkan_depth_range()
{
    CameraComponent c; // perspectiva por defecto, near=1, far=2000
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);

    glm::vec4 atNear = p * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f);
    CHECK(nearlyEqual(atNear.z / atNear.w, 0.0f));
    glm::vec4 atFar = p * glm::vec4(0.0f, 0.0f, -2000.0f, 1.0f);
    CHECK(nearlyEqual(atFar.z / atFar.w, 1.0f));
    glm::vec4 mid = p * glm::vec4(0.0f, 0.0f, -300.0f, 1.0f);
    CHECK(mid.z / mid.w > 0.0f);
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

// Cuenta cuántos avisos de loaded contienen needle.
static int countWarnings(const Scene& scene, const char* needle)
{
    int n = 0;
    for (const auto& w : scene.lastWarnings())
        if (w.find(needle) != std::string::npos) ++n;
    return n;
}

// Un aviso que se repite se colapsa a UNA entrada con "(xN)". Sin esto, una
// malla corrupta escribe un aviso idéntico por vértice y sepulta en el Log los
// demás avisos de la misma carga.
//
// Se montan tres objetos con el MISMO nombre (el contexto del aviso es el
// nombre, no el índice, así que los tres avisos salen byte a byte iguales) y a
// los tres se les corrompe camera.far. Como además las tres traen cámara, el
// prune deja dos avisos también idénticos entre sí: eso fija de paso que
// collapseWarnings corre DESPUÉS de pruneExtraCameras, no antes.
static void test_repeated_warnings_are_collapsed(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    for (int i = 0; i < 3; i++)
        scene.addGameObject("Cam")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();
    for (auto& child : j["root"]["children"])
        child["camera"]["far"] = nullptr; // corrupto: readFloat avisa y cae al default

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    // Una sola entrada por mensaje, no tres ni dos.
    CHECK(countWarnings(loaded, "far") == 1);
    CHECK(countWarnings(loaded, "más de una cámara") == 1);
    CHECK(loaded.lastWarnings().size() == 2);
    // Y el recuento real va en el texto.
    CHECK(countWarnings(loaded, "far: valor corrupto en la escena, se usa el valor por defecto (x3)") == 1);
    CHECK(countWarnings(loaded, "(x2)") == 1);
}

// El sufijo solo aparece cuando hay repetición: un aviso único se queda tal cual.
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

// insertFromJson (el undo de un Delete) limpia los avisos de la operación
// anterior en vez de apilar los suyos encima. Sin el clear, m_warnings crecía
// durante toda la sesión de editor y lastWarnings() dejaba de significar "la
// última operación", que es lo que su contrato promete.
static void test_insert_from_json_resets_warnings(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    for (int i = 0; i < 2; i++)
        scene.addGameObject("Cam")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am)); // deja el aviso del prune
    CHECK(!loaded.lastWarnings().empty());

    // Un insert limpio detrás: sus avisos son los suyos, ninguno.
    GameObject* go = loaded.addGameObject("Otro");
    nlohmann::json snapshot = loaded.subtreeToJson(go);
    loaded.removeGameObject(go);
    CHECK(loaded.insertFromJson(snapshot, nullptr, 0, pm, am) != nullptr);
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

// Un clon necesita id PROPIO. cloneGameObject serializa el origen con
// nodeToJson (que emite "id") y lo reconstruye con nodeFromJson, que reusa ese
// id a propósito: es justo lo que hace falta en el Undo de un Delete, para que
// los comandos que quedan en el stack sigan resolviendo el objeto
// reconstruido. Pero al clonar el ORIGINAL SIGUE VIVO, así que reusarlo deja
// dos GameObjects con el mismo id y findById devuelve el último del recorrido:
// el clon. Cualquier comando de undo resuelto por id (Transform, Rigidbody,
// Audio Clip, Camera...) acabaría escribiendo en el objeto equivocado.
static void test_clone_gets_fresh_id(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Original");
    const uint64_t originalId = go->id;

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    CHECK(clone->id != originalId);
    // Y el id del original tiene que seguir resolviendo AL ORIGINAL, que es lo
    // que de verdad rompía: findById devolvía el clon.
    CHECK(scene.findById(originalId) == go);
    CHECK(scene.findById(clone->id) == clone);
}

// Mismo invariante en un subárbol: los descendientes del clon también tienen
// que estrenar id, no solo su raíz.
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

    // Ningún id repetido en toda la escena.
    std::vector<uint64_t> ids;
    scene.traverse([&](GameObject* n) { ids.push_back(n->id); });
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

// La contrapartida de los dos tests de arriba, y la razón de que el strip de
// ids viva en cloneGameObject y NO en nodeFromJson: insertFromJson (el camino
// del Undo de un Delete) tiene que SEGUIR reusando el id del snapshot. Ahí el
// original ya no existe, así que no hay colisión posible, y conservarlo es lo
// que permite que los comandos que quedan en el stack sigan resolviendo el
// objeto reconstruido.
//
// Sin este test, mover el strip a nodeFromJson —que parece la simplificación
// obvia— rompería el undo en silencio: ningún otro test lo notaría.
static void test_undo_delete_keeps_original_id(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Borrado");
    const uint64_t originalId = go->id;

    // Snapshot + borrado, que es lo que hace DeleteGameObjectCommand.
    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findById(originalId) == nullptr);

    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->id == originalId);
    CHECK(scene.findById(originalId) == restored);
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

// ── Canvas ──────────────────────────────────────────────────────────────────
// Los 10 campos con valores NO neutros y DISTINTOS entre sí: un default no
// prueba que nadie los haya leído ni escrito.
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

// Una escena guardada antes del componente carga igual: sin Canvas y sin avisos.
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

// Neutralidad: sin ningún Canvas el JSON no gana ni un byte, y añadir y quitar
// el componente devuelve el dump EXACTO de partida.
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

// El gate de los componentes de UI: un GameObject solo los ofrece con Canvas.
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

// Add reversible, y el redo NO devuelve los campos a los defaults.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo del Canvas también entra en el stack: el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero) va y
// vuelve, y no revive el componente si ya no está.
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setCanvas(nullptr);
    cmd.undo();
    CHECK(!go->hasCanvas());
}

// ── Button ──────────────────────────────────────────────────────────────────
// TODOS los campos con valores NO neutros y DISTINTOS entre sí (colores,
// rutas, tamaños): un default no prueba que nadie los haya leído ni escrito, y
// dos campos con el MISMO valor no detectan que se hayan cruzado.
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

// Una escena guardada antes del componente carga igual: sin Button y sin avisos.
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

// Neutralidad: sin ningún Button el JSON no gana ni un byte, y añadir y quitar
// el componente devuelve el dump EXACTO de partida.
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

// El gate también vale para un DESCENDIENTE del Canvas: un botón cuelga del
// canvas, no es el canvas.
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
    // Un hermano del canvas (no descendiente) sigue sin verlos.
    CHECK(!PropertiesPanel::uiComponentsAvailable(scene.addGameObject("Suelto")));
}

// Add reversible, y el redo NO devuelve los campos a los defaults.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo del Button también entra en el stack: el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero) va y
// vuelve, y no revive el componente si ya no está.
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setButton(nullptr);
    cmd.undo();
    CHECK(!go->hasButton());
}

// ── Sync del Button contra el canvas vivo ───────────────────────────────────
// Sin GPU: el loader falso devuelve nullptr, que es exactamente lo que devuelve
// el Renderer con una ruta vacía. Lo que se prueba es la COLOCACIÓN, no la
// textura.
struct FakeUiLoader
{
    // Cuántas veces se ha pedido cada recurso. Cargar una fuente de verdad es
    // FreeType + bake + subida a GPU: quién la pide y CUÁNDO es lo que se nota
    // como un parón en el editor, así que se cuenta.
    int atlasLoads = 0;
    int fontLoads  = 0;

    UiTextureAtlas* loadUiAtlas(const std::string&) { atlasLoads++; return nullptr; }
    UiFont*         loadUiFont(const std::string&)  { fontLoads++;  return nullptr; }
};

// Editar un campo del componente tiene que verse en el siguiente frame. El
// árbol cachea los vértices por nodo, así que un sync que escribe los campos y
// no ensucia el nodo deja el botón clavado donde estaba.
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

    syncUiWidgets(lista, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 10.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 20.0f));

    // Mismo botón, otra posición: el nodo vivo tiene que seguirla.
    b.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets(lista, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 300.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 120.0f));
}

// Tocar la resolución del Canvas no puede hacer desaparecer el botón: el sync
// del frame siguiente lo deja donde toca, escalado.
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
    syncUiWidgets(lista, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);

    // Lo que hace CanvasComponent::applyTo cuando el usuario toca el panel.
    CanvasComponent cc;
    cc.scaleFactor = 2.0f;
    cc.applyTo(canvas);

    syncUiWidgets(lista, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 20.0f));   // 10 * escala 2
    CHECK(nearlyEqual(data.vertices[0].pos.y, 40.0f));

    // Y ningún ajuste razonable del canvas lo borra de la pantalla.
    const UiScaleMode modos[] = { UiScaleMode::ConstantPixelSize,
                                  UiScaleMode::ScaleWithScreenSize,
                                  UiScaleMode::ConstantPhysicalSize };
    for (UiScaleMode m : modos)
    {
        CanvasComponent otro;
        otro.scaleMode = m;
        otro.safeArea  = { 8.0f, 6.0f, 8.0f, 6.0f };
        otro.applyTo(canvas);
        syncUiWidgets(lista, {}, canvas, cache, loader);
        data.clear();
        canvas.buildDrawData(800, 480, data);
        CHECK(data.batches.size() == 1);
    }
}

// Un botón con texto y SIN fuente configurada sigue mostrando su etiqueta: el
// sync le pone una fuente por defecto en vez de dejar el texto invisible.
static void test_button_text_without_font_is_visible()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.text = "Aceptar";

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    syncUiWidgets(lista, {}, canvas, cache, loader);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().empty()) return;
    const UiElement& node = *canvas.root().children()[0];
    CHECK(node.children().size() == 1);   // la etiqueta existe aunque no haya fuente
    if (node.children().empty()) return;
    const Text* label = node.children()[0]->asText();
    CHECK(label != nullptr);
    if (label) CHECK(label->text == "Aceptar");
}

// Con el input alimentado, el color que se ve es el del ESTADO, no el color
// base: es lo que hace que editar "Normal" en el panel se note.
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
    syncUiWidgets(lista, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);   // coloca los rects: el hit test los lee

    UiInputState in;
    in.mousePos    = glm::vec2(-1.0f, -1.0f);   // el ratón, lejos: estado Normal
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

// Clic sobre un botón en el viewport: el hit test del canvas devuelve un nodo y
// su nombre es lo único que ata el árbol de UI con la escena. Es la pieza pura
// de ViewportPanel::pickUiObject (lo demás es ImGui).
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
    syncUiWidgets(lista, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    const UiElement* hit = canvas.hitTest(glm::vec2(360.0f, 230.0f));   // centro del 9
    CHECK(hit != nullptr);
    if (hit) CHECK(uiButtonOwnerId(hit->name) == 9ull);

    hit = canvas.hitTest(glm::vec2(50.0f, 20.0f));                      // centro del 7
    CHECK(hit != nullptr);
    if (hit) CHECK(uiButtonOwnerId(hit->name) == 7ull);

    CHECK(canvas.hitTest(glm::vec2(700.0f, 400.0f)) == nullptr);        // hueco
}

// Las cajas de asset del Button vetan por extensión: la de fuentes NO traga una
// imagen ni al revés. Es el mismo filtro para el drop y para el file dialog.
static void test_button_asset_path_filters()
{
    CHECK(PropertiesPanel::isUiFontPath("assets/DancingScript-VariableFont_wght.ttf"));
    CHECK(PropertiesPanel::isUiFontPath("C:/fuentes/algo.OTF"));   // mayúsculas
    CHECK(PropertiesPanel::isUiFontPath("a.ttc"));
    CHECK(!PropertiesPanel::isUiFontPath("assets/ui_atlas.png"));
    CHECK(!PropertiesPanel::isUiFontPath("assets/hero.fbx"));
    CHECK(!PropertiesPanel::isUiFontPath("sinextension"));
    CHECK(!PropertiesPanel::isUiFontPath(""));
    // Un punto del DIRECTORIO no es una extensión.
    CHECK(!PropertiesPanel::isUiFontPath("C:/mis.fuentes/archivo"));

    CHECK(PropertiesPanel::isUiAtlasPath("assets/ui_atlas.png"));
    CHECK(PropertiesPanel::isUiAtlasPath("x.JPEG"));
    CHECK(PropertiesPanel::isUiAtlasPath("x.tga"));
    CHECK(!PropertiesPanel::isUiAtlasPath("assets/fuente.ttf"));
    CHECK(!PropertiesPanel::isUiAtlasPath("assets/audio.wav"));
}

// ── Text ────────────────────────────────────────────────────────────────────
// TODOS los campos con valores NO neutros y DISTINTOS entre sí, mismo criterio
// que fillButton: un default no prueba que nadie los haya leído ni escrito, y
// dos campos con el MISMO valor no detectan que se hayan cruzado.
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

// Una escena guardada antes del componente carga igual: sin Text y sin avisos.
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

// Neutralidad: sin ningún Text el JSON no gana ni un byte, y añadir y quitar el
// componente devuelve el dump EXACTO de partida.
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

// Add reversible, y el redo NO devuelve los campos a los defaults.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo del Text también entra en el stack, con el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero).
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setText(nullptr);
    cmd.undo();
    CHECK(!go->hasText());
}

// Editar un campo del componente tiene que verse en el siguiente frame. El árbol
// cachea los vértices por nodo, así que un sync que escribe los campos y no
// ensucia el nodo deja el texto CLAVADO: es lo que cazan los dirty flags de
// aquí. Con el loader falso no hay fuente, y sin fuente no se emite ni un quad
// (drawable = false), así que el rect se comprueba por screenPos y no por
// vértices.
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

    syncUiWidgets({}, textos, canvas, cache, loader);
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
    // El emisor deja el nodo limpio: es la caché que el sync tiene que invalidar.
    CHECK(node.dirty == 0u);

    // Mismo Text, otro contenido y otra posición: el nodo vivo tiene que
    // seguirlos Y quedar sucio, o el canvas reusaría los vértices de antes.
    t.text     = "Dos";
    t.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(vivo->text == "Dos");
    CHECK(node.dirty != 0u);

    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(node.screenPos.x, 300.0f));
    CHECK(nearlyEqual(node.screenPos.y, 120.0f));

    // Y un frame sin cambios NO vuelve a ensuciar: ensuciar siempre tira la
    // caché de vértices del canvas entero cada frame.
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(node.dirty == 0u);
}

// La trampa: la raíz del canvas se reconstruye con clearChildren(), así que un
// sync que solo conociera los botones borraría los textos (y al revés). Con los
// dos en la escena, ninguno se lleva por delante al otro.
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
    syncUiWidgets(botones, textos, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(canvas.root().children().size() == 2);
    if (canvas.root().children().size() != 2) return;
    CHECK(canvas.root().children()[0]->name == uiButtonNodeName(7ull));
    CHECK(canvas.root().children()[1]->name == uiTextNodeName(9ull));
    // El botón se dibuja (quad de color, sin atlas); el texto sin fuente no.
    CHECK(data.vertices.size() == 4);

    // Tocar SOLO el texto no borra el botón ni le tira sus vértices.
    t.text = "Otro";
    syncUiWidgets(botones, textos, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 2);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 0.0f));

    // Y tocar SOLO el botón no borra el texto.
    b.position = glm::vec2(50.0f, 60.0f);
    syncUiWidgets(botones, textos, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 2);
    if (canvas.root().children().size() != 2) return;
    const Text* vivo = canvas.root().children()[1]->asText();
    CHECK(vivo != nullptr);
    if (vivo) CHECK(vivo->text == "Otro");

    // Un widget de más reconstruye la raíz: los dos tipos se remontan, no solo
    // el que cambió de cuenta.
    TextComponent t2;
    t2.text = "Pie";
    textos.emplace_back(11ull, &t2);
    syncUiWidgets(botones, textos, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 3);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);   // el botón sigue ahí tras el rebuild
    CHECK(nearlyEqual(data.vertices[0].pos.x, 50.0f));
}

// Un Text recién añadido está VACÍO: no puede costar una carga de fuente, que
// es síncrona (FreeType + bake + GPU) y se ve como un parón justo al pulsar Add.
// La fuente se paga cuando hay texto de verdad, y una sola vez por ruta.
static void test_text_without_content_loads_no_font()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    TextComponent t;   // sin texto, como lo deja "Add Component"

    std::vector<std::pair<uint64_t, const TextComponent*>> textos{ {9ull, &t} };
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);
    CHECK(canvas.root().children().size() == 1);

    // Y varios frames más sin escribir nada tampoco la piden.
    syncUiWidgets({}, textos, canvas, cache, loader);
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);

    // La primera letra sí la carga, y solo esa vez: la caché por ruta es lo que
    // impide una carga por frame (y una fuga de memoria de GPU por frame).
    t.text = "H";
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(loader.fontLoads == 1);
    t.text = "Ho";
    syncUiWidgets({}, textos, canvas, cache, loader);
    CHECK(loader.fontLoads == 1);
}

// Clic sobre un texto en el viewport: mismo camino que el del botón, y con los
// dos componentes en el MISMO GameObject los dos nombres llevan a su id sin
// pisarse.
static void test_text_hit_test_maps_back_to_gameobject()
{
    CHECK(uiTextOwnerId(uiTextNodeName(42ull)) == 42ull);
    CHECK(uiTextOwnerId("Cubo") == 0ull);
    CHECK(uiTextOwnerId("txt:") == 0ull);
    CHECK(uiTextOwnerId("txt:12ab") == 0ull);
    // Los dos prefijos no se confunden entre sí.
    CHECK(uiTextOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiTextNodeName(42ull)) == 0ull);
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
    test_insert_from_json_resets_warnings(pm, am);
    test_clone_never_keeps_camera(pm, am);
    test_clone_strips_camera_from_descendant(pm, am);
    test_clone_gets_fresh_id(pm, am);
    test_clone_subtree_gets_fresh_ids(pm, am);
    test_undo_delete_keeps_original_id(pm, am);
    test_camera_command_add_undo_redo();
    test_camera_command_remove();
    test_camera_command_survives_missing_target();

    test_canvas_round_trip(pm, am);
    test_scene_without_canvas_block_still_loads(pm, am);
    test_scene_without_canvas_serializes_identically();
    test_ui_components_need_canvas();
    test_canvas_command_add_undo_redo();
    test_canvas_command_remove();
    test_canvas_property_command_undo_redo();

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
    test_button_hit_test_maps_back_to_gameobject();
    test_button_asset_path_filters();

    test_text_round_trip(pm, am);
    test_scene_without_text_block_still_loads(pm, am);
    test_scene_without_text_serializes_identically();
    test_text_command_add_undo_redo();
    test_text_command_remove();
    test_text_property_command_undo_redo();
    test_text_sync_updates_the_live_node();
    test_buttons_and_texts_coexist();
    test_text_without_content_loads_no_font();
    test_text_hit_test_maps_back_to_gameobject();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
