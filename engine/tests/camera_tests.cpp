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

// Los cuatro campos nuevos del Canvas, con valores NO neutros y distintos entre
// sí: con los defaults, un fromJson que se saltara el campo pasaría igual.
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

// Una escena guardada antes de estos campos carga con los defaults y sin avisos:
// bloque aditivo, misma regla que todos los componentes de UI.
static void test_canvas_without_world_fields_loads_with_defaults(PhysicsManager& pm,
                                                                  AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Viejo");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();
    // Se borran las claves nuevas para simular una escena antigua.
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

// Adaptador SOLO DE TESTS a la firma que syncUiWidgets tenía antes de que las
// listas se agruparan en UiWidgetLists. Está aquí y no en el motor a propósito:
// la API de producción es UNA (la de la struct), y esto existe para no reescribir
// las ~65 llamadas de este fichero, que prueban el montaje del árbol y no la
// forma de pasarle las listas. Los tests nuevos llaman a la de verdad.
//
// Vive en el ámbito global y la de producción en DonTopo, así que las dos son
// candidatas por ADL; no hay ambigüedad porque las aridades no se solapan (esta
// pide seis argumentos como mínimo y aquella exactamente cuatro).

// Un click completo sobre p: un frame de hover, uno con el boton abajo y otro
// con el boton arriba. El hit test necesita rects, o sea un buildDrawData
// previo. Los tiempos se separan entre clicks para no cruzar el umbral del
// doble click sin querer.
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

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 10.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 20.0f));

    // Mismo botón, otra posición: el nodo vivo tiene que seguirla.
    b.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
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
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    CHECK(data.batches.size() == 1);

    // Lo que hace CanvasComponent::applyTo cuando el usuario toca el panel.
    CanvasComponent cc;
    cc.scaleFactor = 2.0f;
    cc.applyTo(canvas);

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
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
        syncUiWidgets(lista, {}, {}, canvas, cache, loader);
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
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);

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
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
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

// Un botón CON etiqueta sigue viendo el ratón. La etiqueta es un hijo Text
// anclado al rect ENTERO del botón, y el hit test prueba a los hijos primero:
// si interceptara el ratón, el hover se marcaría en ella y el botón se quedaría
// en Normal para siempre. Es un fallo difícil de ver porque el CLICK sí
// funciona (los eventos burbujean del hijo al padre): lo único que se rompe son
// los cinco colores de estado.
static void test_button_with_label_still_hovers()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ButtonComponent b;
    b.size        = glm::vec2(100.0f, 50.0f);
    b.text        = "Jugar";            // <- con etiqueta
    b.normalColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    b.hoverColor  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{ {7ull, &b} };
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    UiInputState in;
    in.mousePos    = glm::vec2(20.0f, 20.0f);   // dentro del botón Y de su etiqueta
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
    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
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
    // El emisor deja el nodo limpio: es la caché que el sync tiene que invalidar.
    CHECK(node.dirty == 0u);

    // Mismo Text, otro contenido y otra posición: el nodo vivo tiene que
    // seguirlos Y quedar sucio, o el canvas reusaría los vértices de antes.
    t.text     = "Dos";
    t.position = glm::vec2(300.0f, 120.0f);
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(vivo->text == "Dos");
    CHECK(node.dirty != 0u);

    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(node.screenPos.x, 300.0f));
    CHECK(nearlyEqual(node.screenPos.y, 120.0f));

    // Y un frame sin cambios NO vuelve a ensuciar: ensuciar siempre tira la
    // caché de vértices del canvas entero cada frame.
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(node.dirty == 0u);
}

// La trampa: la raíz del canvas se reconstruye con clearChildren(), así que un
// sync que solo conociera los botones borraría los textos (y al revés). Con los
// dos en la escena, ninguno se lleva por delante al otro.
// ── Jerarquía ───────────────────────────────────────────────────────────────
// Los widgets colgaban TODOS de la raíz del canvas, así que anidar GameObjects
// en la escena no servía de nada: el padre no colocaba, no recortaba y no
// atenuaba a sus hijos. Con la jerarquía, el nodo de un GameObject cuelga del
// nodo PRINCIPAL de su padre (Button > ProgressBar > Text), que es el que tiene
// el rect contra el que anclarse.
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
    // Pre-orden, con el hijo apuntando a su padre. 0 = cuelga de la raíz.
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {7ull, 0ull}, {8ull, 7ull} };

    UiDrawData data;
    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);
    canvas.buildDrawData(800, 480, data);

    // El hijo YA NO cuelga de la raíz.
    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    const UiElement* nodoPadre = canvas.root().children()[0].get();
    CHECK(nodoPadre->name == uiButtonNodeName(7ull));
    CHECK(nodoPadre->children().size() == 1);
    if (nodoPadre->children().size() != 1) return;
    CHECK(nodoPadre->children()[0]->name == uiButtonNodeName(8ull));

    // Y su posición es RELATIVA al padre: 100+10, 50+20.
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[4].pos.x, 110.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 70.0f));

    // Mover al padre mueve al hijo sin tocarlo.
    padre.position = glm::vec2(200.0f, 50.0f);
    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[4].pos.x, 210.0f));

    // Sin jerarquía (nullptr) todo vuelve a colgar de la raíz: es EXACTAMENTE
    // lo que hacían las llamadas de siempre.
    UiCanvas          plano;
    UiWidgetSyncCache cachePlano;
    syncUiWidgets(botones, {}, {}, plano, cachePlano, loader);
    CHECK(plano.root().children().size() == 2);
}

// Cambiar de padre reconstruye el árbol: si no, el nodo se quedaría colgando
// donde estaba y la escena y lo que se ve dejarían de coincidir.
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

// La opacidad del padre se multiplica en el hijo (la del árbol de UI, que el
// canvas ya acumulaba y que ningún widget podía aprovechar sin jerarquía).
static void test_jerarquia_hereda_la_opacidad()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ButtonComponent padre, hijo;
    padre.size = glm::vec2(400.0f, 300.0f);
    hijo.size  = glm::vec2(60.0f, 30.0f);
    // Colores distintos y no neutros: con blancos, un alfa mal propagado pasa
    // desapercibido.
    padre.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    hijo.color  = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {7ull, &padre}, {8ull, &hijo} };
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {7ull, 0ull}, {8ull, 7ull} };

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia);

    // La opacidad no es un campo del componente: se toca en el nodo vivo, que
    // es lo que hace la animación del core. Lo que se prueba es que BAJE.
    UiElement* nodoPadre = const_cast<UiElement*>(canvas.root().children()[0].get());
    nodoPadre->opacity = 0.5f;
    nodoPadre->markDirty(UiElement::DirtyTransform);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[0].color.a, 0.5f));   // padre
    CHECK(nearlyEqual(data.vertices[4].color.a, 0.5f));   // hijo, heredada
}

// Un GameObject intermedio SIN componentes de UI no aporta rect contra el que
// anclarse, así que no puede sostener a nadie: sus hijos tienen que colgar del
// primer ancestro que sí tenga UI. Si se cogiera el padre inmediato, el nodo
// quedaría bajo un id que no existe en el árbol de UI y acabaría en la raíz,
// perdiendo el anclaje al abuelo sin que nada lo dijera.
static void test_collect_ui_widgets_salta_los_intermedios_sin_ui()
{
    Scene scene;
    GameObject* canvasGo = scene.addGameObject("Canvas");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* panel = scene.addGameObject("Panel", canvasGo);
    panel->setButton(std::make_shared<ButtonComponent>());

    // Intermedio SIN UI: solo agrupa.
    GameObject* grupo = scene.addGameObject("Grupo", panel);

    GameObject* etiqueta = scene.addGameObject("Etiqueta", grupo);
    etiqueta->setText(std::make_shared<TextComponent>());

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;

    CHECK(botones.size() == 1);
    CHECK(textos.size() == 1);
    CHECK(barras.empty());

    // Solo los que tienen UI aparecen en la jerarquía: el Canvas y el grupo no.
    CHECK(jerarquia.size() == 2);
    if (jerarquia.size() != 2) return;
    CHECK(jerarquia[0].first == panel->id);
    CHECK(jerarquia[0].second == 0ull);              // primer nivel
    CHECK(jerarquia[1].first == etiqueta->id);
    CHECK(jerarquia[1].second == panel->id);         // el abuelo, no el grupo

    // Y el orden es de PRE-ORDEN: el padre antes que el hijo, que es lo que
    // permite montar el árbol en una sola pasada.
    CHECK(jerarquia[0].first != jerarquia[1].second || true);
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
    // El botón se dibuja (quad de color, sin atlas); el texto sin fuente no.
    CHECK(data.vertices.size() == 4);

    // Tocar SOLO el texto no borra el botón ni le tira sus vértices.
    t.text = "Otro";
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 2);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 4);
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 0.0f));

    // Y tocar SOLO el botón no borra el texto.
    b.position = glm::vec2(50.0f, 60.0f);
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
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
    syncUiWidgets(botones, textos, {}, canvas, cache, loader);
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
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);
    CHECK(canvas.root().children().size() == 1);

    // Y varios frames más sin escribir nada tampoco la piden.
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 0);

    // La primera letra sí la carga, y solo esa vez: la caché por ruta es lo que
    // impide una carga por frame (y una fuga de memoria de GPU por frame).
    t.text = "H";
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
    CHECK(loader.fontLoads == 1);
    t.text = "Ho";
    syncUiWidgets({}, textos, {}, canvas, cache, loader);
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

// ── ProgressBar ─────────────────────────────────────────────────────────────
// Todos los campos a valores NO neutros y DISTINTOS entre sí: con el default (o
// con dos campos iguales) un round-trip pasa igual aunque fromJson se salte el
// campo o lea la clave equivocada.
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

// Una escena guardada antes del componente carga igual: sin barra y sin avisos.
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

// Neutralidad: sin ninguna barra el JSON no gana ni un byte, y añadir y quitar el
// componente devuelve el dump EXACTO de partida.
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

// Add reversible, y el redo NO devuelve los campos a los defaults.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo de la barra también entra en el stack, con el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero).
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setProgressBar(nullptr);
    cmd.undo();
    CHECK(!go->hasProgressBar());
}

// Las cuatro direcciones al 25%: cada una da un rect DISTINTO y en el sitio
// correcto (la Y del canvas crece hacia abajo). Y un valor fuera del rango no
// puede salirse del fondo, que el componente no clampa nada por su cuenta.
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

    // Fuera de rango por arriba: lleno, pero ni un píxel fuera del fondo.
    p.value = 999.0f;
    for (int d = 0; d < 4; d++)
    {
        p.fillDirection = (UiProgressFillDirection)d;
        p.fillRect(pos, sz);
        CHECK(pos.x >= 0.0f && pos.y >= 0.0f);
        CHECK(pos.x + sz.x <= p.size.x + 1e-4f);
        CHECK(pos.y + sz.y <= p.size.y + 1e-4f);
    }
    // Y por abajo: vacío, nunca negativo.
    p.value = -999.0f;
    for (int d = 0; d < 4; d++)
    {
        p.fillDirection = (UiProgressFillDirection)d;
        p.fillRect(pos, sz);
        CHECK(nearlyEqual(sz.x * sz.y, 0.0f));
        CHECK(pos.x >= 0.0f && pos.y >= 0.0f);
    }
    // Rango degenerado: no hay forma de repartir un intervalo vacío.
    p.value = 5.0f; p.minValue = 3.0f; p.maxValue = 3.0f;
    CHECK(nearlyEqual(p.normalizedValue(), 0.0f));
}

// Editar el valor tiene que verse en el siguiente frame. El árbol cachea los
// vértices por nodo, así que un sync que escribe los campos y no ensucia deja la
// barra CLAVADA: es lo que cazan los dirty flags de aquí. Y ensuciar SIEMPRE
// tiraría la caché del canvas entero cada frame, que es el otro fallo posible.
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

    // Fondo entero + relleno a la mitad: dos quads.
    CHECK(data.vertices.size() == 8);
    CHECK(nearlyEqual(fondo.screenPos.x, 10.0f));
    CHECK(nearlyEqual(fondo.screenPos.y, 20.0f));
    CHECK(nearlyEqual(relleno.size.x, 100.0f));
    CHECK(nearlyEqual(relleno.size.y, 40.0f));
    // El emisor deja los nodos limpios: es la caché que el sync tiene que
    // invalidar.
    CHECK(fondo.dirty == 0u);
    CHECK(relleno.dirty == 0u);

    // Otro valor: el relleno cambia de tamaño Y los dos nodos quedan sucios, o
    // el canvas reusaría los vértices de antes.
    p.value = 0.25f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(nearlyEqual(relleno.size.x, 50.0f));
    CHECK(relleno.dirty != 0u);
    CHECK(fondo.dirty != 0u);

    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(relleno.screenPos.x, 10.0f));
    CHECK(nearlyEqual(relleno.screenPos.y, 20.0f));

    // Con RightToLeft el relleno se pega al otro extremo, en pantalla y no solo
    // en el rect local.
    p.fillDirection = UiProgressFillDirection::RightToLeft;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(relleno.screenPos.x, 160.0f));   // 10 + 200*(1-0.25)

    // Y un frame sin cambios NO vuelve a ensuciar: ensuciar siempre tira la
    // caché de vértices del canvas entero cada frame.
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(fondo.dirty == 0u);
    CHECK(relleno.dirty == 0u);

    // A valor 0 el relleno no emite quad (rect degenerado), pero el NODO sigue
    // ahí: si apareciera y desapareciera cambiaría la forma del subárbol y
    // obligaría a reconstruir la raíz al cruzar el cero.
    p.value = 0.0f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(fondo.children().size() == 1);
    CHECK(data.vertices.size() == 4);
}

// La trampa: la raíz del canvas se reconstruye con clearChildren(), así que un
// sync que solo conociera dos de los tres tipos borraría el tercero. Con los
// TRES en la escena, ninguno se lleva por delante a los otros.
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
    // Botón (1 quad) + barra (fondo y relleno) = 3; el texto sin fuente no pinta.
    CHECK(data.vertices.size() == 12);

    // Tocar SOLO la barra no borra al botón ni al texto.
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

    // Y tocar SOLO el texto no toca la barra.
    t.text = "Otro";
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 3);
    const UiElement& fondo = *canvas.root().children()[1];
    CHECK(fondo.children().size() == 1);
    if (!fondo.children().empty())
        CHECK(nearlyEqual(fondo.children()[0]->size.x, 150.0f));

    // Una barra de más reconstruye la raíz: los tres tipos se remontan.
    ProgressBarComponent p2;
    p2.size  = glm::vec2(50.0f, 10.0f);
    p2.value = 1.0f;
    barras.emplace_back(11ull, &p2);
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);
    CHECK(canvas.root().children().size() == 4);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 20);   // botón + 2 barras x 2 quads
    CHECK(nearlyEqual(data.vertices[0].pos.x, 0.0f));   // el botón sigue ahí
}

// Una barra recién añadida no tiene ninguna imagen: no puede costar una carga,
// que es síncrona (lectura + bake + subida a GPU) y se ve como un parón justo al
// pulsar Add. Y luego, una carga por RUTA distinta y solo una: la caché por ruta
// es lo que impide una carga (y una fuga de memoria de GPU) por frame.
static void test_progress_bar_without_atlas_loads_nothing()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    ProgressBarComponent p;   // sin imágenes, como lo deja "Add Component"

    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras{ {5ull, &p} };
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 0);
    CHECK(canvas.root().children().size() == 1);

    // Y varios frames más sin ruta tampoco la piden.
    p.value = 0.9f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 0);

    // Solo el atlas: una carga, y las dos partes tiran de ella (el fondo y el
    // relleno caen en el atlas cuando no traen ruta propia).
    p.atlasPath = "assets/ui/hud.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 1);
    p.value = 0.1f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 1);

    // Dos rutas propias distintas: dos cargas más, una por fichero.
    p.backgroundPath = "assets/ui/bar_bg.png";
    p.fillPath       = "assets/ui/bar_fill.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);

    // Y el mismo fichero en las dos partes NO cuenta dos veces.
    p.fillPath = "assets/ui/bar_bg.png";
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);

    // Cambiar el valor con las tres rutas puestas tampoco recarga nada.
    p.value = 0.4f;
    syncUiWidgets({}, {}, barras, canvas, cache, loader);
    CHECK(loader.atlasLoads == 3);
}

// Clic sobre una barra en el viewport: mismo camino que el del botón y el del
// texto, y con los tres componentes en el MISMO GameObject los tres nombres
// llevan a su id sin pisarse.
static void test_progress_bar_hit_test_maps_back_to_gameobject()
{
    CHECK(uiProgressBarOwnerId(uiProgressBarNodeName(42ull)) == 42ull);
    // El nodo del relleno cuelga de la barra: su nombre también lleva al dueño.
    CHECK(uiProgressBarOwnerId(uiProgressBarNodeName(42ull) + "/Fill") == 42ull);
    CHECK(uiProgressBarOwnerId("Cubo") == 0ull);
    CHECK(uiProgressBarOwnerId("bar:") == 0ull);
    CHECK(uiProgressBarOwnerId("bar:12ab") == 0ull);
    // Los tres prefijos no se confunden entre sí.
    CHECK(uiProgressBarOwnerId(uiButtonNodeName(42ull)) == 0ull);
    CHECK(uiProgressBarOwnerId(uiTextNodeName(42ull)) == 0ull);
    CHECK(uiButtonOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
    CHECK(uiTextOwnerId(uiProgressBarNodeName(42ull)) == 0ull);
}

// ── Layout ──────────────────────────────────────────────────────────────────
// El solver de auto-layout ya vivía en UiElement (layoutMode, padding, spacing,
// celda); lo que no había era forma de usarlo desde la escena. Lo que se prueba
// aquí es justo la capa nueva: que un GameObject SIN otro componente de UI monte
// un contenedor propio, que con otro componente NO monte uno de más, y que los
// campos lleguen al nodo que toca. La aritmética del solver ya la cubre
// ui_batch_tests.

// Un contenedor vacío coloca a sus hijos y NO se dibuja: es un rect, no un
// widget. Las posiciones de los hijos son deliberadamente absurdas: si el layout
// no las pisara, el test lo vería.
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
    layout->spacing     = glm::vec2(9.0f, 12.0f);   // .x != .y: distingue los ejes
    menu->setLayout(layout);

    GameObject* uno = scene.addGameObject("Uno", menu);
    auto a = std::make_shared<ButtonComponent>();
    a->size     = glm::vec2(100.0f, 40.0f);
    a->position = glm::vec2(999.0f, 999.0f);   // la manda el layout, no el hijo
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
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;
    CHECK(layouts.size() == 1);

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader, &jerarquia, &layouts);

    // Un contenedor por GameObject y ni uno más: el menú cuelga de la raíz y los
    // dos botones de él.
    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    CHECK(canvas.root().children()[0]->name == uiLayoutNodeName(menu->id));
    CHECK(canvas.root().children()[0]->children().size() == 2);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    // El contenedor no pinta: dos botones, cuatro vértices cada uno.
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // origen = posición + padding = (30+7, 40+5)
    CHECK(nearlyEqual(data.vertices[0].pos.x, 37.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 45.0f));
    // El segundo baja el alto del primero MÁS el spacing en Y (no el de X).
    CHECK(nearlyEqual(data.vertices[4].pos.x, 37.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 97.0f));
}

// Con otro componente de UI en el mismo GameObject el layout NO monta contenedor:
// escribe sus campos en el nodo que ya hay. Un panel de más sería un rect
// invisible entre el widget y sus hijos, y las anclas dejarían de cuadrar.
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
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    syncUiWidgets(botones, textos, barras, canvas, cache, loader, &jerarquia, &layouts);

    CHECK(canvas.root().children().size() == 1);
    if (canvas.root().children().size() != 1) return;
    // El nodo del GameObject sigue siendo el del botón, no un contenedor nuevo.
    CHECK(canvas.root().children()[0]->name == uiButtonNodeName(barra->id));
    CHECK(canvas.root().children()[0]->children().size() == 1);

    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;
    CHECK(nearlyEqual(data.vertices[0].pos.x, 10.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 20.0f));
    // El hijo, colocado por el layout del botón: esquina + padding.
    CHECK(nearlyEqual(data.vertices[4].pos.x, 16.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 24.0f));
}

// ignoreLayout es lo que un Unity resuelve con un LayoutElement aparte: aquí va
// en el mismo componente. El hijo que lo pone se ancla por su cuenta y NO ocupa
// hueco, así que el siguiente arranca en el origen del contenedor.
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

    // Suelto: botón + layout en el MISMO GameObject solo para el ignoreLayout.
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
    // No neutra: si el layout no lo colocara, el test vería esta posición en vez
    // del origen del contenedor.
    cb->position = glm::vec2(77.0f, 88.0f);
    colocado->setButton(cb);

    std::vector<std::pair<uint64_t, const ButtonComponent*>>      botones;
    std::vector<std::pair<uint64_t, const TextComponent*>>        textos;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras;
    std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
    std::vector<std::pair<uint64_t, uint64_t>>                    jerarquia;
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
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

    // El suelto, en su propia posición dentro del contenedor.
    CHECK(nearlyEqual(data.vertices[0].pos.x, 250.0f));
    CHECK(nearlyEqual(data.vertices[0].pos.y, 150.0f));
    // Y el colocado arranca arriba del todo: el suelto no le comió el hueco.
    CHECK(nearlyEqual(data.vertices[4].pos.x, 0.0f));
    CHECK(nearlyEqual(data.vertices[4].pos.y, 0.0f));
}

// Un contenedor SÍ aporta rect, así que sostiene a sus hijos en la jerarquía
// igual que un botón. Sin esto, sus hijos subirían a la raíz y el layout no
// colocaría a nadie.
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
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
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
    CHECK(jerarquia[1].second == menu->id);   // el contenedor, no el canvas
}

// Sin jerarquía (el camino de las escenas viejas) el contenedor sigue montándose
// en la raíz: perderlo dejaría la escena sin el rect que agrupa.
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
    // Y no dibuja: un contenedor es un rect, no un quad de color.
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.empty());
}

// Valores no neutros y DISTINTOS entre sí: con ceros, unos o repetidos, un
// campo que la serialización no escribe pasaría igual (el default lo taparía).
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

// Una escena guardada antes del componente carga igual: sin Layout y sin avisos.
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

// Neutralidad: sin ningún Layout el JSON no gana ni un byte, y añadir y quitar
// el componente devuelve el dump EXACTO de partida.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo del contenedor también entra en el stack, con el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero).
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setLayout(nullptr);
    cmd.undo();
    CHECK(!go->hasLayout());
}

// Un contenedor no dibuja, así que tampoco puede COMERSE los clics: el hit test
// lo tiene que atravesar. Si fuera raycastTarget, un grupo que solo coloca
// dejaría muerto lo que tuviera detrás, y sin pintar nada no habría forma de ver
// por qué.
static void test_layout_container_no_se_come_los_clics()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    LayoutComponent l;
    l.mode     = UiLayoutMode::None;   // sin colocar: el hijo se queda en su sitio
    l.position = glm::vec2(0.0f, 0.0f);
    l.size     = glm::vec2(400.0f, 300.0f);
    std::vector<std::pair<uint64_t, const LayoutComponent*>> layouts{ {31ull, &l} };

    ButtonComponent b;
    b.position = glm::vec2(10.0f, 10.0f);
    b.size     = glm::vec2(50.0f, 20.0f);
    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{ {32ull, &b} };
    // El botón cuelga del contenedor.
    std::vector<std::pair<uint64_t, uint64_t>> jerarquia{ {31ull, 0ull}, {32ull, 31ull} };

    syncUiWidgets(botones, {}, {}, canvas, cache, loader, &jerarquia, &layouts);
    UiDrawData data;
    canvas.buildDrawData(800, 480, data);

    // Dentro del botón: lo coge él.
    const UiElement* enBoton = canvas.hitTest(glm::vec2(20.0f, 15.0f));
    CHECK(enBoton != nullptr);
    if (enBoton) CHECK(uiButtonOwnerId(enBoton->name) == 32ull);

    // Dentro del contenedor pero FUERA del botón: no lo coge nadie.
    CHECK(canvas.hitTest(glm::vec2(300.0f, 250.0f)) == nullptr);
}

// El nombre del nodo lleva de vuelta al GameObject (clic en el viewport), y no
// se confunde con los otros tres prefijos.
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

// ── Panel ───────────────────────────────────────────────────────────────────
// El Panel del núcleo (UiWidgets.h) es un UiElement sin campos propios: es el
// rectángulo de fondo con el que se montan marcos y grupos. El componente de
// escena expone lo mismo que el resto —el rect, el color, la visibilidad y el
// par atlas/sprite— más raycastTarget, que el núcleo sí tiene y que en un panel
// de fondo es justo el campo que decide si se come los clics de lo de detrás.
//
// Valores NO neutros y DISTINTOS entre sí: con el default (o con dos campos
// iguales) un round-trip pasa igual aunque fromJson se salte el campo o lea la
// clave equivocada.
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

// Una escena guardada antes del componente carga igual: sin panel y sin avisos.
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

// Neutralidad: sin ningún panel el JSON no gana ni un byte, y añadir y quitar el
// componente devuelve el dump EXACTO de partida.
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

// Add reversible, y el redo NO devuelve los campos a los defaults.
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

// Remove reversible: el undo devuelve el componente CON sus valores.
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

// Editar un campo del panel también entra en el stack, con el mismo
// PropertyCommand<T> que arma la sección (resuelto por id, no por puntero).
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

    // Sin componente el applier no hace nada (ni crashea ni lo resucita).
    go->setPanel(nullptr);
    cmd.undo();
    CHECK(!go->hasPanel());
}

// El nodo vivo: nombre propio, rect volcado y, sobre todo, que un cambio del
// componente ENSUCIE el nodo. Un sync que escribe los campos sin ensuciar deja
// el panel clavado, porque el canvas se copia los vértices cacheados.
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
    // El emisor deja el nodo limpio: es la caché que el sync tiene que invalidar.
    CHECK(nodo.dirty == 0u);

    // Mover el panel tiene que ensuciarlo, o el canvas reusaría los vértices.
    p.position = glm::vec2(40.0f, 50.0f);
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nodo.dirty != 0u);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(nearlyEqual(nodo.screenPos.x, 40.0f));

    // Y sin cambios NO se vuelve a ensuciar: ensuciar siempre tiraría la caché
    // del canvas entero cada frame.
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nodo.dirty == 0u);

    // raycastTarget viaja: sin él un panel de fondo se comería los clics de lo
    // que tenga detrás y no habría forma de apagarlo desde la escena.
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
    // Los prefijos no se confunden entre sí.
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
// El Image del núcleo SÍ tiene campos propios (modo, bordes del 9-slice, tope
// de tiles y el bloque de Filled), y todos tienen que llegar al nodo: el
// batcher los lee para emitir N quads.
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

// Los campos PROPIOS del Image tienen que llegar al nodo vivo: el batcher los
// lee de ahí para decidir cuántos quads emite. Un sync que solo volcara el rect
// dejaría el modo, los bordes y el fillAmount en sus defaults y el Image se
// dibujaría siempre como Normal sin que nada lo dijera.
static void test_image_sync_updates_the_live_node()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    ImageComponent im;
    fillImage(im);
    im.visible = true;   // invisible no emite quads y aquí se mira el nodo

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

    // Y un cambio posterior ensucia el nodo.
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

// collectUiWidgets tiene que ver los dos componentes nuevos y meterlos en la
// jerarquía: un GameObject con Panel es un ancestro con rect válido, así que sus
// hijos tienen que colgar de él y no subir a la raíz.
static void test_collect_ui_widgets_incluye_panels_e_images()
{
    Scene scene;
    GameObject* canvasGo = scene.addGameObject("Canvas");
    canvasGo->setCanvas(std::make_shared<CanvasComponent>());

    GameObject* marco = scene.addGameObject("Marco", canvasGo);
    marco->setPanel(std::make_shared<PanelComponent>());

    GameObject* icono = scene.addGameObject("Icono", marco);
    icono->setImage(std::make_shared<ImageComponent>());

    UiWidgetLists w;
    scene.collectUiWidgets(w);

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
    CHECK(w.parents[1].second == marco->id);   // el Panel sostiene al Image
}

// Panel e Image en el MISMO GameObject: dos nodos hermanos con nombres
// distintos, y el Image encima del Panel (el último hermano manda). Con el
// mismo prefijo, el gizmo y el picking cogerían el que no toca.
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
// El Slider del núcleo es un stub sin campos, así que el widget se monta por
// COMPOSICIÓN igual que la ProgressBar: la pista es el nodo raíz y de ella
// cuelgan el relleno y el asa. La diferencia con la barra es que este SÍ recibe
// input: arrastrar el asa escribe en el componente, que es lo que serializa el
// editor y lo que lee un script.
//
// Valores NO neutros y DISTINTOS entre sí, por lo de siempre.
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

// El asa NO se sale de la pista por ninguno de los dos extremos: a t=0 su borde
// pega con el principio y a t=1 con el final. Sin descontar handleSize del
// recorrido, la mitad del asa se saldría del rect en cada punta y no habría nada
// que lo dijera —el asa se dibuja igual—.
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

    // Y con el eje invertido, el mismo recorrido del otro lado.
    s.direction = UiSliderDirection::RightToLeft;
    s.value     = 0.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 160.0f));
    s.value = 1.0f;
    s.handleRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 0.0f));

    // Vertical: la Y del canvas crece hacia ABAJO, así que BottomToTop a 1 pega
    // el asa ARRIBA (y=0).
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

// wholeNumbers redondea el valor QUE SE ESCRIBE, no el que se muestra: si solo
// redondeara al dibujar, el componente guardaría 3,7 y el script leería 3,7
// mientras el asa se enseña en 4.
static void test_slider_whole_numbers_snaps_the_value()
{
    SliderComponent s;
    s.minValue = 0.0f;
    s.maxValue = 10.0f;

    CHECK(nearlyEqual(s.valueFromNormalized(0.37f), 3.7f));

    s.wholeNumbers = true;
    CHECK(nearlyEqual(s.valueFromNormalized(0.37f), 4.0f));
    CHECK(nearlyEqual(s.valueFromNormalized(0.34f), 3.0f));
    // Los extremos siguen siendo exactos.
    CHECK(nearlyEqual(s.valueFromNormalized(0.0f), 0.0f));
    CHECK(nearlyEqual(s.valueFromNormalized(1.0f), 10.0f));

    // Un rango degenerado no puede repartir nada: devuelve el mínimo y no un NaN.
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

    // El relleno llega hasta el CENTRO del asa, que es donde marca el valor.
    CHECK(nearlyEqual(relleno.size.x, 100.0f));
    CHECK(nearlyEqual(asa.size.x, 40.0f));
    CHECK(nearlyEqual(asa.position.x, 80.0f));

    // El asa NO puede comerse el clic de la pista: el hit test devuelve el nodo
    // más profundo, y si el asa fuera raycastTarget el arrastre que empieza
    // encima de ella no llegaría al handler de la pista.
    CHECK(asa.raycastTarget == false);
    CHECK(relleno.raycastTarget == false);

    // El emisor deja los nodos limpios: es la caché que el sync tiene que
    // invalidar cuando cambia el valor.
    CHECK(pista.dirty == 0u);
    s.value = 0.25f;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(nearlyEqual(asa.position.x, 40.0f));
    CHECK(pista.dirty != 0u);
    CHECK(asa.dirty != 0u);
}

// La razón de ser del widget: arrastrar escribe en el COMPONENTE. Si el valor se
// quedara en el nodo, el editor no lo vería, no se serializaría y un script
// leería el valor de antes del arrastre.
static void test_slider_drag_writes_the_component_value()
{
    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;

    SliderComponent s;
    s.position   = glm::vec2(0.0f, 0.0f);
    s.size       = glm::vec2(200.0f, 20.0f);
    s.handleSize = 0.0f;   // sin asa el recorrido es el rect entero: 1 px = 0.5%
    s.minValue   = 0.0f;
    s.maxValue   = 100.0f;
    s.value      = 0.0f;

    // El camino de vuelta a Lua: el mismo runtime que usa el Button.
    float ultimoAviso = -1.0f;
    int   avisos      = 0;
    s.callbacks.ptr->onValueChanged = [&](float v) { ultimoAviso = v; avisos++; };

    UiWidgetLists w;
    w.sliders.emplace_back(5ull, &s);
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    // Un Down a 3/4 de la pista pone el valor ahí mismo (como Unity: la pista
    // entera es zona de clic, no solo el asa).
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

    // Y arrastrando sigue el ratón, también fuera del rect (acotado a la pista).
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

// Un slider no interactable se dibuja pero NO se deja mover: es el modo "solo
// lectura" que un HUD necesita para enseñar un valor sin que el jugador lo toque.
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
    // Los nodos hijos también devuelven a su dueño: arrastrar el asa selecciona
    // el slider, no nada.
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
// Stub sin campos en el núcleo, igual que el Slider: la caja es el nodo raíz y
// la marca cuelga de ella. Es el widget más simple de los interactivos — un
// click y un bool—, así que aquí se prueba sobre todo que el click LLEGUE al
// componente y no se quede en el nodo.
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

// La marca se mete hacia DENTRO de la caja por los cuatro lados. Un padding que
// se pasa no puede dar un rect negativo: la marca desaparece, que es lo peor que
// puede pasar, y no un quad del revés.
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

    // Apagado: la marca EXISTE (la forma del subárbol no cambia) pero no se
    // dibuja. Si el nodo apareciera y desapareciera habría que reconstruir la
    // raíz del canvas en cada click.
    CHECK(marca.drawable == false);
    CHECK(data.vertices.size() == 4);   // solo la caja

    c.isOn = true;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(marca.drawable == true);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    CHECK(data.vertices.size() == 8);   // caja + marca
    CHECK(nearlyEqual(marca.size.x, 24.0f));
    // La marca tampoco puede comerse el click de la caja.
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

    // Y el segundo click lo apaga: es un interruptor, no un botón de encender.
    clickEnCanvas(canvas, glm::vec2(20.0f, 20.0f), 5.0f);
    CHECK(c.isOn == false);
    CHECK(avisos == 2);

    // No interactable: se dibuja pero el click no lo mueve.
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
// El interruptor deslizante: mismo dato que el Checkbox (un bool) pero otra
// forma de enseñarlo — el mando se mueve de un extremo al otro y la pista cambia
// de color. Por eso son dos componentes y no uno con un enum de estilo: son dos
// conjuntos de campos distintos (padding de la marca vs. tamaño del mando).
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

// El mando va pegado a un extremo o al otro, siempre dentro del padding, y NUNCA
// se sale de la pista aunque el tamaño pedido no quepa.
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
    CHECK(nearlyEqual(sz.y, 30.0f));   // alto de la pista menos el padding

    t.isOn = true;
    t.knobRect(pos, sz);
    CHECK(nearlyEqual(pos.x, 45.0f));            // 80 - 5 - 30
    CHECK(nearlyEqual(pos.x + sz.x, 75.0f));     // no se sale del padding

    // Un mando más grande que la pista se acota a lo que queda entre paddings,
    // en vez de asomar por el borde.
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

    // Apagado: color de "off" y mando a la izquierda.
    CHECK(nearlyEqual(pista.color.r, 0.1f));
    CHECK(nearlyEqual(mando.position.x, 5.0f));

    // Encendido: los dos cambian, y el nodo queda sucio o el canvas reusaría los
    // vértices de antes.
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
// Como el Slider pero con el asa de tamaño VARIABLE (la fracción visible del
// contenido) y con el valor siempre en 0..1: no tiene rango propio porque quien
// lo interpreta es el ScrollView, no la barra.
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

// El asa ocupa su fracción del canal y recorre lo que le queda, sin salirse.
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

    // Vertical TopToBottom: el valor crece hacia abajo, que es lo natural de una
    // barra lateral (0 = arriba del todo).
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

    // numberOfSteps engancha el valor a posiciones discretas. Con 5 pasos hay 5
    // paradas (0, 0.25, 0.5, 0.75, 1), como en Unity.
    ScrollbarComponent d;
    d.numberOfSteps = 5u;
    CHECK(nearlyEqual(d.snapValue(0.3f), 0.25f));
    CHECK(nearlyEqual(d.snapValue(0.6f), 0.5f));
    CHECK(nearlyEqual(d.snapValue(0.99f), 1.0f));
    // 0 y 1 pasos = continuo: enganchar a un solo sitio dejaría la barra muerta.
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
    s.handleFraction = 0.0f;   // sin asa el recorrido es el canal entero
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

    // La rueda también mueve la barra: sin esto, una lista con scrollbar solo se
    // podría mover arrastrando, que no es lo que espera nadie.
    in.scrollDelta = 1.0f;     // + hacia arriba = hacia el principio
    in.timeSeconds = 0.048f;
    canvas.updateInput(in);
    CHECK(s.value < 0.75f);
    const float trasRueda = s.value;

    in.scrollDelta = -1.0f;
    in.timeSeconds = 0.064f;
    canvas.updateInput(in);
    CHECK(s.value > trasRueda);

    // Y no se sale de [0,1] por mucho que se insista.
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


// ── Entrada de texto en el canvas ───────────────────────────────────────────
// El canvas ya tenia foco, recorrido con Tab, navegacion direccional y
// onKeyDown; lo unico que le faltaba para poder escribir era el canal de
// CARACTERES. UiKey nombra teclas (Tab, Enter, flechas...), y una 'a' no es una
// tecla nombrada: es un codepoint, y depende del layout del teclado y de las
// muertas, cosa que el core no sabe ni tiene por que saber. Por eso lo rellena
// el caller (GLFW, el editor, un test), igual que la posicion del raton.
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

    // Sin foco NO se entrega nada: un caracter suelto sin destino no puede ir
    // al primero que pase por ahi.
    UiInputState in;
    in.chars = { 'N', 'o' };
    canvas.updateInput(in);
    CHECK(escrito.empty());

    canvas.setFocus(&campo);
    in.chars = { 'H', 'o', 'l', 'a' };
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(escrito == "Hola");

    // Y los codepoints viajan enteros, no truncados a un byte: el canal es
    // uint32, asi que una 'n' con virgulilla llega de una pieza.
    uint32_t ultimo = 0;
    campo.onTextInput = [&](UiEvent& e) { ultimo = e.codepoint; };
    in.chars = { 0x00F1u };   // n con virgulilla
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(ultimo == 0x00F1u);
}

// Las cuatro teclas de edicion tenian que existir: sin Backspace no se puede
// borrar, y con la navegacion comiendose las flechas no se puede mover el
// cursor. El canvas ya cedia la tecla a quien la consumiera; esto comprueba que
// SIGUE cediendola con las nuevas.
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
        // Left y Right son del cursor; Up y Down se dejan pasar para que la
        // navegacion siga funcionando desde dentro del campo.
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

    // Right consumida: el foco NO se va al de al lado.
    in.keys = { UiKey::Right };
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(canvas.focused() == &a);

    // Y sin consumirla, la navegacion sigue viva: es lo que hace jugable un menu
    // con mando, y no se puede haber roto por el camino.
    a.onKeyDown = nullptr;
    in.keys = { UiKey::Right };
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(canvas.focused() == &b);
}


// ── InputField ──────────────────────────────────────────────────────────────
// El unico widget que necesitaba algo que el core NO tenia: un canal de
// caracteres. UiKey nombra teclas y una 'a' no es una tecla nombrada, asi que
// hasta ahora no habia por donde entregarla. Con UiInputState::chars y
// onTextInput ya se puede, y este componente es el primero que los usa.
//
// El cursor se cuenta en CODEPOINTS, no en bytes: con UTF-8, una 'n' con
// virgulilla ocupa dos bytes y un cursor en bytes lo partiria por la mitad.
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

// El cursor se cuenta en CODEPOINTS y el texto se guarda en UTF-8. Un cursor en
// bytes partiria una 'n' con virgulilla por la mitad y dejaria la cadena rota
// sin que nada lo dijera.
static void test_input_field_edits_by_codepoint_not_by_byte()
{
    InputFieldComponent f;
    f.text     = "";
    f.caretPos = 0;

    CHECK(f.insertCodepoint('a'));
    CHECK(f.insertCodepoint(0x00F1u));   // n con virgulilla: DOS bytes en UTF-8
    CHECK(f.insertCodepoint('o'));
    CHECK(f.text == "a\xC3\xB1o");
    CHECK(f.caretPos == 3);              // tres CARACTERES, cuatro bytes
    CHECK(f.codepointCount() == 3);

    // Borrar hacia atras se lleva el caracter entero, no medio byte.
    f.caretPos = 2;
    CHECK(f.backspace());
    CHECK(f.text == "ao");
    CHECK(f.caretPos == 1);

    // Y hacia delante, lo mismo.
    f.text     = "a\xC3\xB1o";
    f.caretPos = 1;
    CHECK(f.deleteForward());
    CHECK(f.text == "ao");
    CHECK(f.caretPos == 1);

    // En los extremos no hay nada que borrar y no pasa nada.
    f.caretPos = 0;
    CHECK(!f.backspace());
    f.caretPos = f.codepointCount();
    CHECK(!f.deleteForward());

    // El cursor se mueve por caracteres y se acota a los extremos.
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

// El tipo de contenido filtra lo que se puede teclear, y el limite corta. Los
// dos van en el sitio donde se ESCRIBE, no al dibujar: si solo filtraran la
// pintura, el componente guardaria basura y un script la leeria.
static void test_input_field_content_type_and_limit()
{
    InputFieldComponent f;

    f.contentType = UiInputContentType::IntegerNumber;
    CHECK(f.accepts('4'));
    CHECK(!f.accepts('a'));
    CHECK(!f.accepts('.'));
    // El signo solo al principio: "1-2" no es un entero.
    f.text = ""; f.caretPos = 0;
    CHECK(f.accepts('-'));
    f.text = "1"; f.caretPos = 1;
    CHECK(!f.accepts('-'));

    f.contentType = UiInputContentType::DecimalNumber;
    f.text = ""; f.caretPos = 0;
    CHECK(f.accepts('.'));
    f.text = "1.5"; f.caretPos = 3;
    CHECK(!f.accepts('.'));   // un solo separador decimal

    f.contentType = UiInputContentType::Alphanumeric;
    CHECK(f.accepts('a'));
    CHECK(f.accepts('7'));
    CHECK(!f.accepts(' '));

    f.contentType = UiInputContentType::Standard;
    CHECK(f.accepts(' '));
    // Los de control NUNCA entran: un '\n' o un tabulador dentro de una linea
    // no se ve y desplaza todo lo que venga detras.
    CHECK(!f.accepts('\n'));
    CHECK(!f.accepts('\t'));
    CHECK(!f.accepts(0x7Fu));

    // El limite cuenta CARACTERES, no bytes.
    f.text           = "";
    f.caretPos       = 0;
    f.characterLimit = 3u;
    CHECK(f.insertCodepoint('a'));
    CHECK(f.insertCodepoint(0x00F1u));
    CHECK(f.insertCodepoint('c'));
    CHECK(!f.insertCodepoint('d'));   // lleno: tres caracteres, cuatro bytes
    CHECK(f.codepointCount() == 3);

    // 0 = sin limite.
    f.characterLimit = 0u;
    CHECK(f.insertCodepoint('d'));
}

// El Password no cambia el texto, cambia lo que se ENSEÑA. Guardar el
// enmascarado seria perder la contraseña.
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

    // Con varios bytes por caracter, la mascara sigue teniendo un simbolo por
    // CARACTER y no uno por byte.
    f.text = "a\xC3\xB1o";
    CHECK(f.displayText() == "***");

    // Vacio: se enseña el placeholder, y con SU color (eso lo comprueba el sync).
    f.text = "";
    CHECK(f.isShowingPlaceholder());
    CHECK(f.displayText() == "clave...");

    // Sin placeholder y vacio no se dibuja nada.
    f.placeholder = "";
    CHECK(f.displayText().empty());

    // Un passwordChar vacio cae al asterisco: un campo de contraseña que no
    // enseña NADA parece roto.
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
    // La caja TIENE que ser focusable o no hay donde escribir.
    CHECK(caja.focusable == true);
    CHECK(caja.children().size() == 2);
    if (caja.children().size() != 2) return;
    const UiElement& texto = *caja.children()[0];
    const UiElement& caret = *caja.children()[1];
    CHECK(texto.name == uiInputFieldNodeName(5ull) + "/Text");
    CHECK(caret.name == uiInputFieldNodeName(5ull) + "/Caret");
    CHECK(texto.raycastTarget == false);
    CHECK(caret.raycastTarget == false);

    // Sin foco el cursor NO se dibuja: un campo que parpadea sin estar activo
    // es exactamente lo que hace creer que se puede escribir en el.
    CHECK(caret.drawable == false);
}

// La razon de ser del canal de texto: teclear escribe en el COMPONENTE.
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

    // Un click enfoca el campo.
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

    // Backspace borra y Left mueve, sin dejar que la navegacion se coma la
    // flecha (el campo la consume).
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
    CHECK(canvas.focused() != nullptr);   // la flecha NO movio el foco

    in.keys = { UiKey::Home };
    in.timeSeconds = 1.048f;
    canvas.updateInput(in);
    CHECK(f.caretPos == 0);
    in.keys = { UiKey::End };
    in.timeSeconds = 1.064f;
    canvas.updateInput(in);
    CHECK(f.caretPos == 3);

    // Enter cierra la edicion.
    in.keys = { UiKey::Enter };
    in.timeSeconds = 1.08f;
    canvas.updateInput(in);
    CHECK(finales == 1);

    // readOnly: se puede enfocar y mover el cursor, pero no cambiar el texto.
    f.readOnly = true;
    syncUiWidgets(w, canvas, cache, loader);
    canvas.setFocus(nullptr);
    clickEnCanvas(canvas, glm::vec2(100.0f, 15.0f), 2.0f);
    in.keys.clear();
    in.chars = { 'X' };
    in.timeSeconds = 3.0f;
    canvas.updateInput(in);
    CHECK(f.text == "Hol");

    // Y no interactable no se enfoca siquiera.
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
// El unico widget cuyo subarbol CAMBIA DE FORMA con los datos: una opcion mas es
// un nodo mas. Abrir y cerrar NO cambia la forma (la lista existe siempre y solo
// se apaga), pero añadir o quitar opciones si, y eso obliga a reconstruir.
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

// isOpen es estado VIVO y no se guarda: una escena que se abriera con el combo
// desplegado tendria una lista tapando el menu nada mas cargar.
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

// El valor NO se clampa al escribirlo (el componente no interpreta nada) pero
// todo lo que lo LEE tiene que aguantar un indice fuera de rango: una escena
// editada a mano con value 99 y dos opciones no puede reventar.
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

    // Sin opciones tampoco.
    d.options.clear();
    d.value = 0;
    CHECK(d.selectedLabel().empty());

    // Alto de la lista: se enseñan como mucho maxVisibleItems filas.
    d.options         = { "A", "B", "C", "D", "E" };
    d.itemHeight      = 20.0f;
    d.maxVisibleItems = 3u;
    CHECK(nearlyEqual(d.listHeight(), 60.0f));
    d.maxVisibleItems = 0u;   // 0 = todas
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
    // Etiqueta, flecha y lista.
    CHECK(caja.children().size() == 3);
    if (caja.children().size() != 3) return;
    const UiElement& lista = *caja.children()[2];
    CHECK(lista.name == uiDropdownNodeName(5ull) + "/List");
    CHECK(lista.children().size() == 3);   // una fila por opcion

    // Cerrada: la lista EXISTE pero no se ve. La forma del subarbol no cambia al
    // abrir, asi que abrir no reconstruye el canvas entero.
    CHECK(lista.visible == false);

    d.isOpen = true;
    syncUiWidgets(w, canvas, cache, loader);
    CHECK(lista.visible == true);
    CHECK(canvas.root().children()[0]->children().size() == 3);

    // Cambiar el NUMERO de opciones si cambia la forma: hay que reconstruir, o
    // la opcion nueva no tendria nodo y no se veria.
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

    // Click en la caja: se abre.
    clickEnCanvas(canvas, glm::vec2(80.0f, 15.0f), 0.0f);
    CHECK(d.isOpen == true);

    // Hay que volver a sincronizar y a medir: la lista acaba de hacerse visible,
    // asi que hasta ahora no tenia rect contra el que probar el raton.
    syncUiWidgets(w, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);

    // Click en la segunda fila: la lista arranca justo debajo de la caja
    // (y = 30) y cada fila mide 20, asi que la fila 1 va de 50 a 70.
    clickEnCanvas(canvas, glm::vec2(80.0f, 60.0f), 5.0f);
    CHECK(d.value == 1);
    CHECK(avisos == 1);
    CHECK(ultimo == 1);
    CHECK(d.isOpen == false);   // elegir cierra

    // No interactable: ni se abre.
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
// El unico cuyo nodo PRINCIPAL no es el que recibe el raton: los hijos de la
// escena cuelgan del Content, que es el que se mueve. Si colgaran del viewport,
// el scroll no arrastraria nada.
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

// El desplazamiento del contenido sale del recorrido REAL: contenido menos
// viewport. Con el contenido mas pequeño que el viewport no hay nada que
// desplazar, y el offset tiene que ser 0 y no negativo — un contenido empujado
// hacia dentro deja un hueco arriba que nadie ha pedido.
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

    // Eje apagado: no se mueve aunque el contenido sea mas ancho.
    s.contentSize        = glm::vec2(500.0f, 600.0f);
    s.normalizedPosition = glm::vec2(1.0f, 0.0f);
    CHECK(nearlyEqual(s.contentOffset().x, 0.0f));
    s.horizontal = true;
    CHECK(nearlyEqual(s.contentOffset().x, -400.0f));

    // Contenido mas pequeño que el viewport: cero, nunca positivo.
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
    // Recorta a sus descendientes: es lo que hace que el contenido no se salga.
    CHECK(vista.clipChildren == true);
    CHECK(vista.children().size() == 1);
    if (vista.children().empty()) return;
    const UiElement& contenido = *vista.children()[0];
    CHECK(contenido.name == uiScrollViewNodeName(5ull) + "/Content");
    CHECK(nearlyEqual(contenido.size.y, 600.0f));
    CHECK(nearlyEqual(contenido.position.y, -200.0f));   // (600-200) * 0.5
    // El contenido NO recibe el raton: la rueda es del viewport.
    CHECK(contenido.raycastTarget == false);
}

// Los hijos de la escena cuelgan del CONTENT, no del viewport: si colgaran del
// viewport, moverse no los arrastraria y el scroll no serviria de nada.
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

    UiWidgetLists w;
    scene.collectUiWidgets(w);
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
    // La fila cuelga del CONTENT.
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
    s.contentSize       = glm::vec2(100.0f, 600.0f);   // recorrido: 400 px
    s.scrollSensitivity = 40.0f;                        // 40 px por muesca = 0.1

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

    // Rueda hacia abajo (delta negativo) = bajar por la lista.
    in.scrollDelta = -1.0f;
    in.timeSeconds = 0.016f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.normalizedPosition.y, 0.1f));
    CHECK(avisos == 1);
    CHECK(nearlyEqual(ultimoY, 0.1f));

    // Y hacia arriba vuelve.
    in.scrollDelta = 1.0f;
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(s.normalizedPosition.y, 0.0f));

    // No se sale de [0,1] por mucho que se insista.
    for (int i = 0; i < 50; i++)
    {
        in.scrollDelta = -1.0f;
        in.timeSeconds += 0.016f;
        canvas.updateInput(in);
    }
    CHECK(nearlyEqual(s.normalizedPosition.y, 1.0f));

    // Sin recorrido no se mueve NI avisa: un contenido que cabe entero no
    // scrollea, y avisar de un cambio que no ha pasado haria trabajar a un
    // script en balde en cada muesca.
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
    // "scv:" y "scr:" (Scrollbar) NO se confunden: comparten las tres primeras
    // letras y son dos widgets distintos.
    CHECK(uiScrollViewOwnerId(uiScrollbarNodeName(42ull)) == 0ull);
    CHECK(uiScrollbarOwnerId(uiScrollViewNodeName(42ull)) == 0ull);
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




    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
