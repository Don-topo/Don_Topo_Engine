// Tests headless de ScriptBindings: el guard ensureFinite (NaN/Inf desde Lua,
// ver ScriptBindings.cpp) y su contrato de "silencio + aviso". Plain main +
// asserts, sin framework — mismo patrón que camera_tests.cpp/audio_tests.cpp/
// physics_tests.cpp.
//
// Antes de este fichero, ensureFinite (131 líneas cambiadas en
// ScriptBindings.cpp) no lo ejercitaba ningún test: podía romperse en
// silencio sin que ningún exe rojo lo delatara.
//
// PhysX solo admite UNA PxFoundation por proceso: se comparte un único
// PhysicsManager (y AudioManager) entre todos los tests, creados en main() y
// pasados por referencia — nunca uno por test (ver physics_tests.cpp).
//
// ScriptManager se ejercita headless de verdad: init() con una carpeta que no
// existe registra igualmente los bindings de Lua (solo loguea "carpeta no
// encontrada" y sigue, ver ScriptManager::init) — no hace falta ningún .lua
// en disco para llamar a Transform/Collider/Rigidbody directamente. Cada test
// empuja una LuaEntity ya resuelta a un global Lua ("e") y ejecuta Lua REAL
// contra ella (mismo mecanismo que instantiateComponentWith usa para inyectar
// self.entity, solo que aquí sin ScriptComponent de por medio) y captura el
// Log en un std::vector<std::string> vía setLogCallback.
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Audio/AudioManager.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// true si alguna línea logueada contiene needle (p.ej. el nombre del método o
// "WARN").
static bool logContains(const std::vector<std::string>& log, const std::string& needle)
{
    for (const auto& l : log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// SetPosition con un Vec3 que trae un componente NaN (0/0 en Lua): la
// posición NO cambia (se conserva la de antes) y el Log recibe un aviso que
// nombra el método. Ejercita el guard tal cual lo ve un script real, que es
// justo lo que el review señaló sin cobertura.
static void test_set_position_rejects_nan(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Objetivo");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 6.0f, 7.0f));
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetTransform():SetPosition(Vec3.new(0/0, 1, 2))");

    glm::vec3 pos(go->localTransform[3]);
    CHECK(nearlyEqual(pos.x, 5.0f));
    CHECK(nearlyEqual(pos.y, 6.0f));
    CHECK(nearlyEqual(pos.z, 7.0f));
    CHECK(logContains(log, "SetPosition"));
    CHECK(logContains(log, "WARN"));
}

// CASO DE CONTROL: el MISMO setter con un Vec3 finito de verdad SÍ cambia la
// posición y NO deja ningún aviso en el Log. Sin este test, un ensureFinite
// que devolviera siempre false pasaría igual el test de arriba (posición sin
// cambiar "porque nunca aplica nada" en vez de "porque el valor era NaN").
static void test_set_position_applies_finite_value(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Objetivo");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetTransform():SetPosition(Vec3.new(11, 22, 33))");

    glm::vec3 pos(go->localTransform[3]);
    CHECK(nearlyEqual(pos.x, 11.0f));
    CHECK(nearlyEqual(pos.y, 22.0f));
    CHECK(nearlyEqual(pos.z, 33.0f));
    CHECK(log.empty());
}

// Caso de un FLOAT SUELTO (no un Vec3): SphereCollider.SetRadius con NaN.
// Mismo contrato que SetPosition: el radio no cambia y el Log avisa.
static void test_set_radius_rejects_nan(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Bola");
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), go->worldTransform, /*dynamic=*/false);
    go->setSphereCollider(col);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('SphereCollider'):SetRadius(0/0)");

    CHECK(nearlyEqual(go->getSphereCollider()->getRadius(), 10.0f));
    CHECK(logContains(log, "SetRadius"));
    CHECK(logContains(log, "WARN"));
}

// Control: el mismo SetRadius con un valor finito SÍ cambia el radio y NO
// loguea nada.
static void test_set_radius_applies_finite_value(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Bola");
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), go->worldTransform, /*dynamic=*/false);
    go->setSphereCollider(col);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('SphereCollider'):SetRadius(42)");

    CHECK(nearlyEqual(go->getSphereCollider()->getRadius(), 42.0f));
    CHECK(log.empty());
}

// AddForce recibe x,y,z SUELTOS (no un Vec3 ya construido): con un NaN entre
// ellos, la fuerza NO se aplica (la velocidad se queda a 0 tras avanzar la
// física) y el Log recibe un aviso.
static void test_add_force_rejects_nan(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    // El actor PhysX vive en el collider (Rigidbody no lo posee, ver
    // Rigidbody.h) — col tiene que seguir vivo mientras se usa rb, igual que
    // en physics_tests.cpp.
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('Rigidbody'):AddForce(0/0, 100, 0)");
    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);

    CHECK(nearlyEqual(rb->getVelocity().x, 0.0f));
    CHECK(nearlyEqual(rb->getVelocity().y, 0.0f));
    CHECK(logContains(log, "AddForce"));
    CHECK(logContains(log, "WARN"));
}

// Control: el mismo AddForce con x,y,z finitos SÍ mueve el cuerpo (velocidad
// no nula tras avanzar la física) y NO loguea nada. Sin este test, un
// ensureFinite que devolviera siempre false pasaría igual el test de arriba.
static void test_add_force_applies_finite_value(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('Rigidbody'):AddForce(1000, 0, 0)");
    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);

    CHECK(rb->getVelocity().x > 0.1f);
    CHECK(log.empty());
}

// El ORDEN importa, y este test es lo único que lo protege: ensureFinite
// tiene que correr DESPUÉS del deref, no antes.
//
// Con el guard delante, un script que toca una entity ya destruida pasándole
// además un NaN recibía un aviso de "valor no finito" y un return silencioso
// — enmascarando el use-after-destroy, que es el fallo grave de los dos. Con
// el orden correcto, deref lanza error de Lua y del NaN no se llega a hablar.
//
// Sin este test, revertir ese reordenamiento entero deja los 7 ejecutables en
// verde: el caso feliz no lo distingue, porque cuando la entity está viva los
// dos órdenes se comportan igual.
static void test_dead_entity_wins_over_nan(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Condenado");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    // El Transform se obtiene MIENTRAS la entity vive y se guarda. Si se
    // pidiera después del borrado, el deref que hace el propio GetTransform
    // lanzaría antes de llegar a SetPosition y este test pasaría sin ejercitar
    // nada — comprobado: así no distinguía el orden correcto del invertido.
    sm.lua().script("t = e:GetTransform()");

    // Ahora sí: la entity de Lua sostiene un GameObject que ya no está en la
    // escena, y el Transform guardado apunta a ella.
    scene.removeGameObject(go);
    sm.rebuildAliveSet();

    // pcall: se espera que LANCE. Con script() a secas el error se propagaría
    // y abortaría el test en vez de comprobarlo.
    sm.lua().script(
        "ok, err = pcall(function() t:SetPosition(Vec3.new(0/0, 1, 2)) end)");

    const bool ok = sm.lua()["ok"];
    CHECK(!ok);                              // tiene que fallar, no avisar

    // Y el motivo debe ser la entity muerta, no el NaN. El early-return es
    // necesario: si la llamada NO lanzó, "err" es nil y leerlo como string
    // hace panic a sol2, que abortaría el proceso en vez de dejar un FAIL
    // legible.
    if (!ok)
    {
        const std::string err = sm.lua()["err"];
        CHECK(err.find("destruida") != std::string::npos);
        CHECK(err.find("SetPosition") == std::string::npos);
    }
    CHECK(!logContains(log, "SetPosition"));
}

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    // Carpeta inexistente a propósito: init() registra los bindings de Lua
    // igual (solo loguea el aviso de "carpeta no encontrada" y sigue, ver
    // ScriptManager::init) — estos tests no cargan ningún .lua de disco.
    ScriptManager sm;
    sm.init("__scripting_tests_sin_carpeta_de_scripts__");
    sm.setPhysicsManager(&pm);
    sm.setAudioManager(&am);

    test_set_position_rejects_nan(sm);
    test_set_position_applies_finite_value(sm);
    test_set_radius_rejects_nan(sm, pm);
    test_set_radius_applies_finite_value(sm, pm);
    test_add_force_rejects_nan(sm, pm);
    test_add_force_applies_finite_value(sm, pm);
    test_dead_entity_wins_over_nan(sm);

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL SCRIPTING TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
