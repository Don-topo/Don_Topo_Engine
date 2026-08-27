#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/Files/FileManager.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace DonTopo::ScriptBindings
{
    namespace
    {
        using DonTopo::GameObject;
        using DonTopo::LuaEntity;

        // Buzón de DonTopo.loadScene (ver ScriptBindings.h). Una sola casilla:
        // dos peticiones en el mismo frame -> gana la última. Vive aquí y no en
        // ScriptManager porque el consumidor (EditorUI / runtime) solo necesita
        // la ruta, no la VM. Un único hilo lo toca: el de los scripts y el del
        // bucle de frame son el mismo.
        std::string g_pendingSceneLoad;
        bool        g_hasPendingSceneLoad = false;

        // Deref validado: entity muerta -> excepción C++ que sol2 convierte en
        // error Lua (capturado por la protected_function del callback).
        GameObject* deref(const LuaEntity& e)
        {
            if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
                throw std::runtime_error("Entity destruida o inválida");
            return e.go;
        }

        // std::clamp(NaN, lo, hi) devuelve NaN: toda comparación con NaN es
        // falsa, así que el clamp de rango (p.ej. el [0,1] de volume) no lo
        // detiene. Los infinitos SÍ se clampan bien (clamp(+inf,0,1) == 1.0),
        // así que el peligroso de verdad es el NaN, no el infinito: un NaN se
        // cuela hasta el JSON de la escena (nlohmann lo serializa como
        // "null"), y al releer ese "null" con .get<float>() nlohmann lanza
        // json::exception, lo que hacía fallar Scene::fromJson ENTERO por un
        // solo campo corrupto. Se ataja aquí, en el punto de entrada desde
        // Lua: se ignora el valor (se deja el anterior) y se avisa por el Log
        // Console, sin lanzar error de Lua — un cálculo roto en un script
        // (p.ej. un 0/0) no debe matar la partida.
        //
        // IMPORTANTE en cada call-site: llamar a ensureFinite DESPUÉS de
        // deref() y de cualquier has*Collider()/hasAudioClip()/hasRigidbody(),
        // nunca antes. Si no, una entity ya destruida con un NaN de regalo
        // (deadEntity:GetTransform():SetPosition(Vec3(0/0,0,0))) se limita a
        // avisar del NaN y hacer return, cuando el bug real y más grave —
        // use-after-destroy — debería seguir lanzando error de Lua como
        // siempre (hallazgo 4 del review de este fix).
        bool ensureFinite(ScriptManager& mgr, const char* metodo, float v)
        {
            if (std::isfinite(v)) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": valor no finito (NaN/Inf) ignorado, se conserva el anterior");
            return false;
        }

        bool ensureFinite(ScriptManager& mgr, const char* metodo, const glm::vec3& v)
        {
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": vector con componente no finito (NaN/Inf) ignorado, se conserva el anterior");
            return false;
        }

        // Modo de fuerza opcional de AddForce/AddTorque. Llega de Lua como un
        // entero (tabla ForceMode) y se valida contra el rango del enum de
        // Rigidbody.h. Fuera de rango NO lanza: mismo canal que ensureFinite —
        // aviso por el Log Console y la llamada se ignora entera. Ausente
        // (llamada de tres argumentos) es siempre válido y vale Force.
        constexpr int kForceModeMax = static_cast<int>(ForceMode::VelocityChange);

        bool ensureForceMode(ScriptManager& mgr, const char* metodo, const sol::optional<int>& mode)
        {
            if (!mode) return true;
            if (*mode >= 0 && *mode <= kForceModeMax) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": ForceMode fuera de rango (" + std::to_string(*mode) +
                    "), fuerza ignorada — usa la tabla ForceMode");
            return false;
        }

        // Solo se llama tras un ensureForceMode en verde, así que el valor ya
        // está dentro del enum.
        ForceMode toForceMode(const sol::optional<int>& mode)
        {
            return mode ? static_cast<ForceMode>(*mode) : ForceMode::Force;
        }

        struct LuaTransform { LuaEntity e; };
        struct LuaBoxCollider { LuaEntity e; };
        struct LuaSphereCollider { LuaEntity e; };
        struct LuaCapsuleCollider { LuaEntity e; };
        struct LuaPlaneCollider { LuaEntity e; };
        struct LuaAudioClip { LuaEntity e; };
        struct LuaRigidbody { LuaEntity e; };
        struct LuaAnimator { LuaEntity e; };
        struct LuaCanvas { LuaEntity e; };
        struct LuaButton { LuaEntity e; };
        struct LuaText { LuaEntity e; };
        struct LuaProgressBar { LuaEntity e; };
        struct LuaLayout { LuaEntity e; };
        struct LuaPanel { LuaEntity e; };
        struct LuaImage { LuaEntity e; };
        struct LuaSlider { LuaEntity e; };
        struct LuaCheckbox { LuaEntity e; };
        struct LuaToggle { LuaEntity e; };
        struct LuaScrollbar { LuaEntity e; };
        struct LuaInputField { LuaEntity e; };
        struct LuaDropdown { LuaEntity e; };
        struct LuaScrollView { LuaEntity e; };

        // Descompone localTransform en T/R/S (grados pa Lua). La extracción de
        // ángulos usa extractEulerAngleXYZ — el inverso exacto del
        // eulerAngleXYZ de recomposeLocal; mezclar convenciones (p.ej.
        // glm::eulerAngles sobre el quat) corrompe la rotación en cualquier
        // objeto rotado en más de un eje.
        void decomposeLocal(GameObject* go, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
        {
            glm::quat rot; glm::vec3 skew; glm::vec4 persp;
            glm::decompose(go->localTransform, scale, rot, pos, skew, persp);
            glm::mat4 rotOnly = glm::mat4_cast(rot);
            float t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
            glm::extractEulerAngleXYZ(rotOnly, t1, t2, t3);
            eulerDeg = glm::degrees(glm::vec3(t1, t2, t3));
        }

        void recomposeLocal(GameObject* go, const glm::vec3& pos, const glm::vec3& eulerDeg, const glm::vec3& scale)
        {
            glm::mat4 r = glm::eulerAngleXYZ(glm::radians(eulerDeg.x),
                                              glm::radians(eulerDeg.y),
                                              glm::radians(eulerDeg.z));
            go->localTransform = glm::translate(glm::mat4(1.0f), pos) * r *
                                 glm::scale(glm::mat4(1.0f), scale);
            go->updateWorldTransforms(go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
        }
        void registerVec3(sol::state& lua)
        {
            lua.new_usertype<glm::vec3>("Vec3",
                sol::call_constructor, sol::factories(
                    []() { return glm::vec3(0.0f); },
                    [](float x, float y, float z) { return glm::vec3(x, y, z); }),
                "new", sol::factories(
                    []() { return glm::vec3(0.0f); },
                    [](float x, float y, float z) { return glm::vec3(x, y, z); }),
                "x", &glm::vec3::x,
                "y", &glm::vec3::y,
                "z", &glm::vec3::z,
                sol::meta_function::addition,
                    [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
                sol::meta_function::subtraction,
                    [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
                sol::meta_function::multiplication,
                    [](const glm::vec3& v, float s) { return v * s; },
                sol::meta_function::to_string,
                    [](const glm::vec3& v) {
                        return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) +
                               ", " + std::to_string(v.z) + ")";
                    });
        }

        void registerLog(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table logTable = lua.create_named_table("Log");
            logTable["Info"]  = [&mgr](const std::string& m) { mgr.log("[Lua] " + m); };
            logTable["Warn"]  = [&mgr](const std::string& m) { mgr.log("[Lua][WARN] " + m); };
            logTable["Error"] = [&mgr](const std::string& m) { mgr.log("[Lua][ERROR] " + m); };
            // print nativo -> mismo destino que Log.Info. Cada argumento pasa
            // por el tostring de Lua (maneja números, nil, tablas y el
            // metamétodo __tostring), igual que el print nativo.
            lua["print"] = [&mgr](sol::variadic_args args) {
                sol::state_view lua(args.lua_state());
                sol::protected_function tostring = lua["tostring"];
                std::string out;
                for (auto a : args)
                {
                    if (!out.empty()) out += "\t";
                    sol::protected_function_result r = tostring(a.get<sol::object>());
                    if (r.valid() && r.get_type() == sol::type::string)
                        out += r.get<std::string>();
                    else
                        out += "?";
                }
                mgr.log("[Lua] " + out);
            };
        }

        // DonTopo.loadScene(path) -> bool. NO carga: valida y encola (ver el
        // buzón arriba). El bool es el resultado de la validación —fichero
        // legible, JSON parseable, estructura de escena v1—, la misma que hace
        // EditorUI::loadSceneFile antes de tocar GPU; el desenlace de la carga
        // en sí llega un frame después y no puede devolverse aquí. Nada de
        // excepciones hacia Lua: readJson ya devuelve optional.
        void registerEngineTable(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table engine = lua.create_named_table("DonTopo");

            engine["loadScene"] = [&mgr](const std::string& path) -> bool {
                if (path.empty())
                {
                    mgr.log("[Lua][ERROR] DonTopo.loadScene: ruta vacía");
                    return false;
                }
                auto parsed = FileManager::readJson(path);
                bool structureOk = parsed.has_value() &&
                                   parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                   (*parsed)["version"].get<int>() == 1 &&
                                   parsed->contains("root") && (*parsed)["root"].is_object();
                if (!structureOk)
                {
                    mgr.log("[Lua][ERROR] DonTopo.loadScene: no se pudo leer la escena '" + path + "'");
                    return false;
                }
                // Última petición del frame gana: se pisa la anterior sin avisar.
                g_pendingSceneLoad    = path;
                g_hasPendingSceneLoad = true;
                return true;
            };
        }

        void registerInput(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table input = lua.create_named_table("Input");
            input["IsKeyDown"]          = [](int k) { return Input::isKeyDown(k); };
            input["IsKeyPressed"]       = [](int k) { return Input::isKeyPressed(k); };
            input["IsKeyReleased"]      = [](int k) { return Input::isKeyReleased(k); };
            input["IsMouseButtonDown"]  = [](int b) { return Input::isMouseButtonDown(b); };

            // Acciones con nombre del panel Input Actions. Un nombre desconocido
            // devuelve false y avisa UNA vez por nombre y sesión: la llamada
            // típica vive en Update() y un aviso por frame ahogaría el Log.
            auto warned = std::make_shared<std::set<std::string>>();
            auto known  = [&mgr, warned](const std::string& name) {
                const bool ok = Input::hasAction(name);   // fuerza la carga perezosa del mapa
                // Avisos de la carga (bindings de mando ignorados): la lista se
                // vacía al leerla, así que salen una sola vez.
                for (const std::string& d : Input::takeActionDiagnostics())
                    mgr.log("[Lua][WARN] " + d);
                if (!ok && warned->insert(name).second)
                    mgr.log("[Lua][WARN] Input: no existe la accion '" + name +
                            "' (definela en el panel Input Actions)");
                return ok;
            };
            input["IsActionDown"]     = [known](const std::string& n) { return known(n) && Input::isActionDown(n); };
            input["IsActionPressed"]  = [known](const std::string& n) { return known(n) && Input::isActionPressed(n); };
            input["IsActionReleased"] = [known](const std::string& n) { return known(n) && Input::isActionReleased(n); };

            sol::table key = lua.create_named_table("Key");
            key["Space"]  = GLFW_KEY_SPACE;  key["Enter"] = GLFW_KEY_ENTER;
            key["Escape"] = GLFW_KEY_ESCAPE; key["Tab"]   = GLFW_KEY_TAB;
            key["LeftShift"]  = GLFW_KEY_LEFT_SHIFT;
            key["LeftControl"] = GLFW_KEY_LEFT_CONTROL;
            key["Up"]   = GLFW_KEY_UP;   key["Down"]  = GLFW_KEY_DOWN;
            key["Left"] = GLFW_KEY_LEFT; key["Right"] = GLFW_KEY_RIGHT;
            for (int i = 0; i < 26; ++i)
                key[std::string(1, char('A' + i))] = GLFW_KEY_A + i;
            for (int i = 0; i <= 9; ++i)
                key["Num" + std::to_string(i)] = GLFW_KEY_0 + i;

            sol::table mb = lua.create_named_table("MouseButton");
            mb["Left"]   = GLFW_MOUSE_BUTTON_LEFT;
            mb["Right"]  = GLFW_MOUSE_BUTTON_RIGHT;
            mb["Middle"] = GLFW_MOUSE_BUTTON_MIDDLE;
        }

        void registerTransform(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            lua.new_usertype<LuaTransform>("Transform",
                sol::no_constructor,
                "GetPosition", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return p;
                },
                "SetPosition", [&mgr](const LuaTransform& t, const glm::vec3& np) {
                    // deref ANTES que ensureFinite: una entity destruida tiene
                    // que dar el error de Lua de siempre (use-after-destroy,
                    // el bug gordo), no un aviso de NaN silencioso que la deja
                    // pasar (hallazgo 4 del review).
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.SetPosition", np)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, np, r, s);
                },
                "GetRotation", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return r;
                },
                "SetRotation", [&mgr](const LuaTransform& t, const glm::vec3& nr) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.SetRotation", nr)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, nr, s);
                },
                "GetScale", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return s;
                },
                "SetScale", [&mgr](const LuaTransform& t, const glm::vec3& ns) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.SetScale", ns)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, r, ns);
                },
                "GetWorldPosition", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return glm::vec3(go->worldTransform[3]);
                },
                "Translate", [&mgr](const LuaTransform& t, const glm::vec3& d) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.Translate", d)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p + d, r, s);
                },
                "Rotate", [&mgr](const LuaTransform& t, const glm::vec3& dEuler) {
                    GameObject* go = deref(t.e);
                    // Rotación incremental compuesta como quaternion, NUNCA
                    // sumando eulers: extractEulerAngleXYZ acota el ángulo
                    // medio a ±90°, y acumular sobre esa representación hace
                    // que una rotación continua se "atasque" al llegar al
                    // límite (gira y luego se queda casi quieta).
                    if (!ensureFinite(mgr, "Transform.Rotate", dEuler)) return;
                    glm::vec3 scale, pos, skew; glm::quat rot; glm::vec4 persp;
                    glm::decompose(go->localTransform, scale, rot, pos, skew, persp);
                    rot = rot * glm::quat(glm::radians(dEuler));
                    go->localTransform = glm::translate(glm::mat4(1.0f), pos) *
                                         glm::mat4_cast(rot) *
                                         glm::scale(glm::mat4(1.0f), scale);
                    go->updateWorldTransforms(go->parent ? go->parent->worldTransform
                                                         : glm::mat4(1.0f));
                });
        }

        // Índice de capa de colisión válido, o error de Lua. Lo usan los cuatro
        // colliders (.layer) y la matriz de Physics. Se lanza en vez de clampear
        // a propósito: con un clamp el script creería estar filtrando por la
        // capa que pidió cuando en realidad está en otra, y eso no se ve hasta
        // que algo atraviesa algo.
        void requireLayer(const char* what, int layer)
        {
            if (!DonTopo::PhysicsManager::isValidLayer(layer))
                throw std::runtime_error(std::string(what) + ": capa fuera de rango (0-" +
                                         std::to_string(DonTopo::PhysicsManager::kLayerCount - 1) +
                                         "): " + std::to_string(layer));
        }

        void registerComponents(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            lua.new_usertype<LuaBoxCollider>("BoxCollider",
                sol::no_constructor,
                "GetHalfExtents", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->getHalfExtents();
                },
                "SetHalfExtents", [&mgr](const LuaBoxCollider& c, const glm::vec3& he) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    if (!ensureFinite(mgr, "BoxCollider.SetHalfExtents", he)) return;
                    go->getBoxCollider()->setHalfExtents(he);
                },
                "GetCenter", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaBoxCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    if (!ensureFinite(mgr, "BoxCollider.SetCenter", ctr)) return;
                    go->getBoxCollider()->setCenter(ctr);
                },
                // Material de física del collider. Propiedades (no Get/Set)
                // igual que en Rigidbody. Como en mass: deref + has ANTES del
                // guard de finitud.
                "staticFriction", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        return go->getBoxCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.staticFriction", v)) return;
                        go->getBoxCollider()->setFriction(v, go->getBoxCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        return go->getBoxCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.dynamicFriction", v)) return;
                        go->getBoxCollider()->setFriction(go->getBoxCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        return go->getBoxCollider()->getBounciness();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.bounciness", v)) return;
                        go->getBoxCollider()->setBounciness(v);
                    }),
                // Capa de colisión (0-31). Con quién colisiona la decide la
                // matriz global (Physics.SetLayerCollision). El setter reescribe
                // el filtro de la shape, así que vale también en mitad de Play.
                "layer", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        return go->getBoxCollider()->getLayer();
                    },
                    [](const LuaBoxCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        requireLayer("BoxCollider.layer", v);
                        go->getBoxCollider()->setLayer(v);
                    }),
                // Is Trigger. El setter NO toca el collider a pelo: pasa por
                // PhysicsManager::setTrigger, que además del flip de flags da
                // de alta/baja el collider en el registro de onTriggerStay.
                // Fuera de Play no hay PhysicsManager (igual que en el raycast):
                // no-op silencioso, no error de Lua.
                "isTrigger", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        return go->getBoxCollider()->isTrigger();
                    },
                    [&mgr](const LuaBoxCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getBoxCollider(), v);
                    }));

            lua.new_usertype<LuaSphereCollider>("SphereCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->getRadius();
                },
                "SetRadius", [&mgr](const LuaSphereCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    if (!ensureFinite(mgr, "SphereCollider.SetRadius", r)) return;
                    go->getSphereCollider()->setRadius(r);
                },
                "GetCenter", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaSphereCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    if (!ensureFinite(mgr, "SphereCollider.SetCenter", ctr)) return;
                    go->getSphereCollider()->setCenter(ctr);
                },
                // Material de física del collider; ver nota en BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        return go->getSphereCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.staticFriction", v)) return;
                        go->getSphereCollider()->setFriction(v, go->getSphereCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        return go->getSphereCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.dynamicFriction", v)) return;
                        go->getSphereCollider()->setFriction(go->getSphereCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        return go->getSphereCollider()->getBounciness();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.bounciness", v)) return;
                        go->getSphereCollider()->setBounciness(v);
                    }),
                // Capa de colisión; ver nota en BoxCollider.
                "layer", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        return go->getSphereCollider()->getLayer();
                    },
                    [](const LuaSphereCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        requireLayer("SphereCollider.layer", v);
                        go->getSphereCollider()->setLayer(v);
                    }),
                // Is Trigger; ver nota en BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        return go->getSphereCollider()->isTrigger();
                    },
                    [&mgr](const LuaSphereCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getSphereCollider(), v);
                    }));

            lua.new_usertype<LuaCapsuleCollider>("CapsuleCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getRadius();
                },
                "SetRadius", [&mgr](const LuaCapsuleCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetRadius", r)) return;
                    go->getCapsuleCollider()->setRadius(r);
                },
                "GetHalfHeight", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getHalfHeight();
                },
                "SetHalfHeight", [&mgr](const LuaCapsuleCollider& c, float h) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetHalfHeight", h)) return;
                    go->getCapsuleCollider()->setHalfHeight(h);
                },
                "GetCenter", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaCapsuleCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetCenter", ctr)) return;
                    go->getCapsuleCollider()->setCenter(ctr);
                },
                // Material de física del collider; ver nota en BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        return go->getCapsuleCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.staticFriction", v)) return;
                        go->getCapsuleCollider()->setFriction(v, go->getCapsuleCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        return go->getCapsuleCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.dynamicFriction", v)) return;
                        go->getCapsuleCollider()->setFriction(go->getCapsuleCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        return go->getCapsuleCollider()->getBounciness();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.bounciness", v)) return;
                        go->getCapsuleCollider()->setBounciness(v);
                    }),
                // Capa de colisión; ver nota en BoxCollider.
                "layer", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        return go->getCapsuleCollider()->getLayer();
                    },
                    [](const LuaCapsuleCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        requireLayer("CapsuleCollider.layer", v);
                        go->getCapsuleCollider()->setLayer(v);
                    }),
                // Is Trigger; ver nota en BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        return go->getCapsuleCollider()->isTrigger();
                    },
                    [&mgr](const LuaCapsuleCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getCapsuleCollider(), v);
                    }));

            lua.new_usertype<LuaPlaneCollider>("PlaneCollider",
                sol::no_constructor,
                "GetCenter", [](const LuaPlaneCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                    return go->getPlaneCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaPlaneCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                    if (!ensureFinite(mgr, "PlaneCollider.SetCenter", ctr)) return;
                    go->getPlaneCollider()->setCenter(ctr);
                },
                // Material de física del collider; ver nota en BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        return go->getPlaneCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.staticFriction", v)) return;
                        go->getPlaneCollider()->setFriction(v, go->getPlaneCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        return go->getPlaneCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.dynamicFriction", v)) return;
                        go->getPlaneCollider()->setFriction(go->getPlaneCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        return go->getPlaneCollider()->getBounciness();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.bounciness", v)) return;
                        go->getPlaneCollider()->setBounciness(v);
                    }),
                // Capa de colisión; ver nota en BoxCollider.
                "layer", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        return go->getPlaneCollider()->getLayer();
                    },
                    [](const LuaPlaneCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        requireLayer("PlaneCollider.layer", v);
                        go->getPlaneCollider()->setLayer(v);
                    }),
                // Is Trigger; ver nota en BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        return go->getPlaneCollider()->isTrigger();
                    },
                    [&mgr](const LuaPlaneCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getPlaneCollider(), v);
                    }));

            lua.new_usertype<LuaAudioClip>("AudioClip",
                sol::no_constructor,
                "Play", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
                },
                "Stop", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->stop();
                },
                // Se SOLAPA con lo que ya suene, al revés que Play, que corta la
                // voz anterior del mismo clip. Es lo que hace que dos pasos o
                // dos disparos seguidos no se pisen. La voz que dispara queda
                // fuera de alcance: Stop, SetVolume e IsPlaying no la ven, y no
                // sigue al objeto. Para clips cortos, nunca para loops.
                "PlayOneShot", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->playOneShot(glm::vec3(go->worldTransform[3]));
                },
                "SetLoop", [](const LuaAudioClip& c, bool l) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setLoop(l);
                },
                "GetLoop", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getLoop();
                },
                "SetVolume", [&mgr](const LuaAudioClip& c, float v) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetVolume", v)) return;
                    go->getAudioClip()->setVolume(v);
                },
                "GetVolume", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getVolume();
                },
                "SetPitch", [&mgr](const LuaAudioClip& c, float p) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetPitch", p)) return;
                    go->getAudioClip()->setPitch(p);
                },
                "GetPitch", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getPitch();
                },
                // Ojo: setIs3D RECARGA el sonido (unloadSound + loadSound
                // porque is3D va horneado en el FMOD_MODE) y corta lo que
                // estuviera sonando. Es configuración, no algo de llamar por
                // frame — al revés que SetVolume/SetPitch.
                "SetIs3D", [](const LuaAudioClip& c, bool b) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setIs3D(b);
                },
                "GetIs3D", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getIs3D();
                },
                // Distancias de atenuación 3D. Estaban en el componente y en el
                // Inspector desde el principio, pero no en Lua: un script no
                // podía, por ejemplo, ensanchar el radio de un motor al acelerar.
                // Como SetVolume/SetPitch, no recargan el sonido. El clamp y el
                // invariante min <= max los impone el componente.
                "SetMinDistance", [&mgr](const LuaAudioClip& c, float d) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetMinDistance", d)) return;
                    go->getAudioClip()->setMinDistance(d);
                },
                "GetMinDistance", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getMinDistance();
                },
                "SetMaxDistance", [&mgr](const LuaAudioClip& c, float d) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetMaxDistance", d)) return;
                    go->getAudioClip()->setMaxDistance(d);
                },
                "GetMaxDistance", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getMaxDistance();
                },
                "SetPlayOnAwake", [](const LuaAudioClip& c, bool b) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setPlayOnAwake(b);
                },
                "GetPlayOnAwake", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getPlayOnAwake();
                },
                // Bus por NOMBRE ("master"/"music"/"sfx"), no por índice: es lo
                // mismo que se guarda en la escena, y un número mágico en un
                // script sería ilegible. Un nombre desconocido avisa y no
                // cambia nada, en vez de caer a un bus arbitrario.
                "SetBus", [&mgr](const LuaAudioClip& c, const std::string& name) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    AudioBus bus;
                    if (!audioBusFromStr(name, bus))
                    {
                        mgr.log("[Lua][WARN] AudioClip.SetBus: bus desconocido '" + name +
                                 "' (usa 'master', 'music' o 'sfx'), se conserva el anterior");
                        return;
                    }
                    go->getAudioClip()->setBus(bus);
                },
                "GetBus", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return std::string(audioBusToStr(go->getAudioClip()->getBus()));
                },
                // Modo de carga por nombre ("sample"/"stream"), como el bus.
                // OJO: recarga el sonido y corta lo que suene. Es configuracion
                // de arranque, no algo de llamar por frame.
                "SetLoadMode", [&mgr](const LuaAudioClip& c, const std::string& name) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    AudioLoadMode mode;
                    if (!audioLoadModeFromStr(name, mode))
                    {
                        mgr.log("[Lua][WARN] AudioClip.SetLoadMode: modo desconocido '" + name +
                                 "' (usa 'sample' o 'stream'), se conserva el anterior");
                        return;
                    }
                    go->getAudioClip()->setLoadMode(mode);
                },
                "GetLoadMode", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return std::string(audioLoadModeToStr(go->getAudioClip()->getLoadMode()));
                },
                "GetPath", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getPath();
                },
                // Estado de la VOZ, no del componente. IsPlaying sigue el
                // criterio de FMOD y de Unity: una voz pausada cuenta como
                // sonando, y IsPaused es lo que las separa. Sin esto un script
                // no tenía forma de esperar a que un sonido terminara.
                "IsPlaying", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->isPlaying();
                },
                "IsPaused", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->isPaused();
                },
                // Pause conserva la posición de reproducción; Stop la tira.
                "Pause", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->pause();
                },
                "Resume", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->resume();
                });

            // Rigidbody: dinámica estilo Unity. Propiedades (mass/useGravity/
            // isKinematic/drag/angularDrag/constraints/velocity/angularVelocity)
            // + métodos AddForce/AddTorque/AddImpulse. Se obtiene con
            // GetComponent("Rigidbody").
            //
            // Los 6 bits válidos del bitmask de constraints (Rigidbody.h). Todo
            // lo demás que llegue de Lua se recorta contra esta máscara.
            constexpr uint32_t kRigidbodyConstraintsMask =
                RB_FreezePositionX | RB_FreezePositionY | RB_FreezePositionZ |
                RB_FreezeRotationX | RB_FreezeRotationY | RB_FreezeRotationZ;
            auto rbOf = [](const LuaRigidbody& c) -> Rigidbody* {
                GameObject* go = deref(c.e);
                if (!go->hasRigidbody()) throw std::runtime_error("El GameObject ya no tiene Rigidbody");
                return go->getRigidbody().get();
            };
            lua.new_usertype<LuaRigidbody>("Rigidbody",
                sol::no_constructor,
                "mass", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getMass(); },
                    [rbOf, &mgr](const LuaRigidbody& c, float v) {
                        Rigidbody* rb = rbOf(c); // deref + hasRigidbody ANTES del guard (hallazgo 4)
                        if (!ensureFinite(mgr, "Rigidbody.mass", v)) return;
                        rb->setMass(v);
                    }),
                "useGravity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getUseGravity(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setUseGravity(v); }),
                "isKinematic", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getIsKinematic(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setIsKinematic(v); }),
                "drag", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getDrag(); },
                    [rbOf, &mgr](const LuaRigidbody& c, float v) {
                        Rigidbody* rb = rbOf(c);
                        if (!ensureFinite(mgr, "Rigidbody.drag", v)) return;
                        rb->setDrag(v);
                    }),
                "angularDrag", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getAngularDrag(); },
                    [rbOf, &mgr](const LuaRigidbody& c, float v) {
                        Rigidbody* rb = rbOf(c);
                        if (!ensureFinite(mgr, "Rigidbody.angularDrag", v)) return;
                        rb->setAngularDrag(v);
                    }),
                // constraints es un BITMASK (tabla RigidbodyConstraints), no un
                // float: nada de ensureFinite aquí. Los bits que no están
                // definidos en Rigidbody.h se enmascaran en silencio en vez de
                // lanzar — un OR de más no debe tumbar el script.
                "constraints", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getConstraints(); },
                    [rbOf](const LuaRigidbody& c, uint32_t v) {
                        Rigidbody* rb = rbOf(c);
                        rb->setConstraints(v & kRigidbodyConstraintsMask);
                    }),
                // ccd/interpolate: dos booleanos independientes entre sí y
                // apagados por defecto. Sin ensureFinite (no son floats) y sin
                // enmascarar (no son bitmask): un bool de Lua es siempre válido.
                "ccd", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getCcd(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setCcd(v); }),
                "interpolate", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getInterpolate(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setInterpolate(v); }),
                "velocity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getVelocity(); },
                    [rbOf, &mgr](const LuaRigidbody& c, const glm::vec3& v) {
                        Rigidbody* rb = rbOf(c);
                        if (!ensureFinite(mgr, "Rigidbody.velocity", v)) return;
                        rb->setVelocity(v);
                    }),
                "angularVelocity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getAngularVelocity(); },
                    [rbOf, &mgr](const LuaRigidbody& c, const glm::vec3& v) {
                        Rigidbody* rb = rbOf(c);
                        if (!ensureFinite(mgr, "Rigidbody.angularVelocity", v)) return;
                        rb->setAngularVelocity(v);
                    }),
                // El 4º argumento (modo) es OPCIONAL: sin él se aplica
                // ForceMode.Force, o sea lo mismo que hacían las llamadas de
                // tres argumentos de siempre. Un modo fuera de rango se avisa
                // por el Log y NO aplica fuerza, igual que un NaN: un índice
                // mal calculado en un script no debe tumbar la partida.
                "AddForce",   [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z, sol::optional<int> mode) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 f(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddForce", f)) return;
                    if (!ensureForceMode(mgr, "Rigidbody.AddForce", mode)) return;
                    rb->addForce(f, toForceMode(mode));
                },
                "AddTorque",  [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z, sol::optional<int> mode) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 t(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddTorque", t)) return;
                    if (!ensureForceMode(mgr, "Rigidbody.AddTorque", mode)) return;
                    rb->addTorque(t, toForceMode(mode));
                },
                "AddImpulse", [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 f(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddImpulse", f)) return;
                    rb->addImpulse(f);
                });

            // Constantes del bitmask de Rigidbody.constraints. Se combinan con
            // el OR bit a bit de Lua 5.3+ (rb.constraints = RigidbodyConstraints.
            // FreezePositionX | RigidbodyConstraints.FreezeRotationY).
            sol::table rbc = lua.create_named_table("RigidbodyConstraints");
            rbc["None"]            = RB_None;
            rbc["FreezePositionX"] = RB_FreezePositionX;
            rbc["FreezePositionY"] = RB_FreezePositionY;
            rbc["FreezePositionZ"] = RB_FreezePositionZ;
            rbc["FreezeRotationX"] = RB_FreezeRotationX;
            rbc["FreezeRotationY"] = RB_FreezeRotationY;
            rbc["FreezeRotationZ"] = RB_FreezeRotationZ;

            // Modos de AddForce/AddTorque (4º argumento opcional). Los valores
            // son los índices del enum ForceMode de Rigidbody.h, en ese orden.
            sol::table fm = lua.create_named_table("ForceMode");
            fm["Force"]          = static_cast<int>(ForceMode::Force);
            fm["Acceleration"]   = static_cast<int>(ForceMode::Acceleration);
            fm["Impulse"]        = static_cast<int>(ForceMode::Impulse);
            fm["VelocityChange"] = static_cast<int>(ForceMode::VelocityChange);

            // Animator: máquina de estados de animación. Se obtiene con
            // GetComponent("Animator"). Sin propiedades: los parámetros se
            // declaran en el grafo y se consultan por nombre, no son campos.
            auto animOf = [](const LuaAnimator& c) -> AnimatorComponent* {
                GameObject* go = deref(c.e);
                if (!go->hasAnimator()) throw std::runtime_error("El GameObject ya no tiene Animator");
                return go->getAnimator().get();
            };
            lua.new_usertype<LuaAnimator>("Animator",
                sol::no_constructor,
                "SetBool",    [animOf](const LuaAnimator& c, const std::string& n, bool v) { animOf(c)->setBool(n, v); },
                "GetBool",    [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getBool(n); },
                "SetTrigger", [animOf](const LuaAnimator& c, const std::string& n) { animOf(c)->setTrigger(n); },
                // Numéricos: mismo contrato que los bools — un nombre no
                // declarado (o de otro tipo) se ignora en el setter y devuelve 0
                // en el getter, nunca lanza.
                "SetInt",     [animOf](const LuaAnimator& c, const std::string& n, int v) { animOf(c)->setInt(n, v); },
                "GetInt",     [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getInt(n); },
                "SetFloat",   [animOf, &mgr](const LuaAnimator& c, const std::string& n, float v) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.SetFloat", v)) return;
                    anim->setFloat(n, v);
                },
                "GetFloat",   [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getFloat(n); },
                "GetState",   [animOf](const LuaAnimator& c) { return animOf(c)->currentStateName(); },
                // Cross-fade en curso. Son de LECTURA: la duración de la mezcla
                // es autoría del grafo (se edita en el panel Animator), igual
                // que las condiciones de una transición.
                "IsBlending",     [animOf](const LuaAnimator& c) { return animOf(c)->blending(); },
                "GetBlendWeight", [animOf](const LuaAnimator& c) { return animOf(c)->blendWeight(); },
                "GetPreviousState", [animOf](const LuaAnimator& c) { return animOf(c)->previousStateName(); },
                // El peso que acaba yendo a la GPU: el del cross-fade si hay
                // uno en vuelo, si no el del blend por parámetro del estado, y
                // 1 si no hay mezcla ninguna. El blend por parámetro se CONDUCE
                // con SetFloat sobre su parámetro, así que aquí solo se lee.
                "GetPoseWeight", [animOf](const LuaAnimator& c) { return animOf(c)->poseWeight(); });
        }

        // ── UI: Canvas, Button, Text y ProgressBar ──────────────────────────
        //
        // Los cuatro son SOLO DATOS de la escena y quien los pinta es
        // syncUiWidgets, que cada frame vuelca el componente sobre el nodo vivo
        // del canvas. Por eso los setters escriben SIEMPRE en el componente y
        // nunca en el nodo: una escritura al nodo la borraría el siguiente
        // volcado. Tampoco se ensucia nada a mano — el sync compara con su
        // propio snapshot y ya sabe qué ha cambiado.
        //
        // Resolución por acceso (deref + has*) igual que el resto de
        // componentes: un wrapper guardado en una variable de Lua no puede
        // quedarse con un puntero que otro frame haya liberado.
        CanvasComponent* canvasOf(const LuaCanvas& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasCanvas()) throw std::runtime_error("El GameObject ya no tiene Canvas");
            return go->getCanvas().get();
        }
        ButtonComponent* buttonOf(const LuaButton& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasButton()) throw std::runtime_error("El GameObject ya no tiene Button");
            return go->getButton().get();
        }
        TextComponent* textOf(const LuaText& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasText()) throw std::runtime_error("El GameObject ya no tiene Text");
            return go->getText().get();
        }
        ProgressBarComponent* barOf(const LuaProgressBar& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasProgressBar()) throw std::runtime_error("El GameObject ya no tiene ProgressBar");
            return go->getProgressBar().get();
        }
        LayoutComponent* layoutOf(const LuaLayout& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasLayout()) throw std::runtime_error("El GameObject ya no tiene Layout");
            return go->getLayout().get();
        }
        PanelComponent* panelOf(const LuaPanel& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasPanel()) throw std::runtime_error("El GameObject ya no tiene Panel");
            return go->getPanel().get();
        }
        ImageComponent* imageOf(const LuaImage& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasImage()) throw std::runtime_error("El GameObject ya no tiene Image");
            return go->getImage().get();
        }
        SliderComponent* sliderOf(const LuaSlider& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasSlider()) throw std::runtime_error("El GameObject ya no tiene Slider");
            return go->getSlider().get();
        }
        CheckboxComponent* checkboxOf(const LuaCheckbox& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasCheckbox()) throw std::runtime_error("El GameObject ya no tiene Checkbox");
            return go->getCheckbox().get();
        }
        ToggleComponent* toggleOf(const LuaToggle& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasToggle()) throw std::runtime_error("El GameObject ya no tiene Toggle");
            return go->getToggle().get();
        }
        ScrollbarComponent* scrollbarOf(const LuaScrollbar& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasScrollbar()) throw std::runtime_error("El GameObject ya no tiene Scrollbar");
            return go->getScrollbar().get();
        }
        InputFieldComponent* inputFieldOf(const LuaInputField& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasInputField()) throw std::runtime_error("El GameObject ya no tiene InputField");
            return go->getInputField().get();
        }
        DropdownComponent* dropdownOf(const LuaDropdown& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasDropdown()) throw std::runtime_error("El GameObject ya no tiene Dropdown");
            return go->getDropdown().get();
        }
        ScrollViewComponent* scrollViewOf(const LuaScrollView& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasScrollView()) throw std::runtime_error("El GameObject ya no tiene ScrollView");
            return go->getScrollView().get();
        }

        // Fábricas de accesores. Son plantillas y no una lista de lambdas a mano
        // porque los cuatro componentes suman más de cien campos y escribir el
        // par get/set de cada uno multiplicaría por diez las ocasiones de
        // teclear el campo equivocado en un lado del par.
        //
        // El resolutor entra como PUNTERO A FUNCIÓN (por eso los cuatro de
        // arriba no capturan nada): así el accesor se puede copiar dentro de las
        // lambdas sin arrastrar estado.
        template <class W, class Comp, class T>
        auto uiProp(Comp* (*res)(const W&), T Comp::*campo)
        {
            return sol::property(
                [res, campo](const W& w) -> T { return res(w)->*campo; },
                [res, campo](const W& w, T v) { res(w)->*campo = v; });
        }

        // Igual, pero pasando por el filtro de NaN/Inf: un cálculo roto en un
        // script no puede dejar un campo de la UI con un valor que reviente el
        // layout (mismo contrato que Transform.SetPosition).
        template <class W, class Comp>
        auto uiFloatProp(Comp* (*res)(const W&), float Comp::*campo,
                         ScriptManager* mgr, const char* nombre)
        {
            return sol::property(
                [res, campo](const W& w) { return res(w)->*campo; },
                [res, campo, mgr, nombre](const W& w, float v) {
                    Comp* c = res(w);
                    if (!ensureFinite(*mgr, nombre, v)) return;
                    c->*campo = v;
                });
        }

        // Los enums viajan como ENTEROS (las tablas UiTextAlign, UiButtonState y
        // compañía que registra registerUi). Un valor fuera de rango se ignora:
        // convertirlo a enum sin más metería un valor imposible en el componente
        // y el switch del sync caería en el default sin que nadie se enterase.
        template <class W, class Comp, class E>
        auto uiEnumProp(Comp* (*res)(const W&), E Comp::*campo, int maximo)
        {
            return sol::property(
                [res, campo](const W& w) { return static_cast<int>(res(w)->*campo); },
                [res, campo, maximo](const W& w, int v) {
                    Comp* c = res(w);
                    if (v < 0 || v > maximo) return;
                    c->*campo = static_cast<E>(v);
                });
        }

        // Vectores como MÉTODOS y no como propiedades: en Lua no hay vec2 ni
        // vec4 (solo Vec3), y devolver una tabla nueva por lectura haría basura
        // en cada frame de cada script. Devuelven varios valores, que es la
        // forma natural en Lua: local x, y = b:GetPosition().
        template <class W, class Comp>
        auto uiVec2Get(Comp* (*res)(const W&), glm::vec2 Comp::*campo)
        {
            return [res, campo](const W& w) {
                const glm::vec2 v = res(w)->*campo;
                return std::make_tuple(v.x, v.y);
            };
        }
        template <class W, class Comp>
        auto uiVec2Set(Comp* (*res)(const W&), glm::vec2 Comp::*campo,
                       ScriptManager* mgr, const char* nombre)
        {
            return [res, campo, mgr, nombre](const W& w, float x, float y) {
                Comp* c = res(w);
                if (!ensureFinite(*mgr, nombre, glm::vec3(x, y, 0.0f))) return;
                c->*campo = glm::vec2(x, y);
            };
        }
        template <class W, class Comp>
        auto uiVec4Get(Comp* (*res)(const W&), glm::vec4 Comp::*campo)
        {
            return [res, campo](const W& w) {
                const glm::vec4 v = res(w)->*campo;
                return std::make_tuple(v.x, v.y, v.z, v.w);
            };
        }
        template <class W, class Comp>
        auto uiVec4Set(Comp* (*res)(const W&), glm::vec4 Comp::*campo,
                       ScriptManager* mgr, const char* nombre)
        {
            return [res, campo, mgr, nombre](const W& w, float x, float y, float z, float a) {
                Comp* c = res(w);
                if (!ensureFinite(*mgr, nombre, glm::vec3(x, y, z)) ||
                    !ensureFinite(*mgr, nombre, a)) return;
                c->*campo = glm::vec4(x, y, z, a);
            };
        }

        // Las funciones Lua de los callbacks de UI viven en ESTA tabla del
        // propio lua_State, referenciadas por una clave entera, y lo que se
        // guarda en el componente es un std::function que va a buscarlas.
        //
        // Guardar el sol::protected_function dentro del componente sería meter
        // una referencia al registro de Lua en un objeto que SOBREVIVE al
        // lua_State: su destructor haría luaL_unref sobre un estado ya cerrado.
        // Así el componente no guarda nada de Lua.
        constexpr const char* kUiCallbackTable = "__uiCallbacks";
        long long g_nextUiCallbackKey = 0;

        void setUiCallback(ScriptManager& mgr, std::function<void()>& destino,
                           const char* nombre, const sol::object& fn)
        {
            if (!fn.valid() || fn.get_type() != sol::type::function)
            {
                destino = nullptr;   // pasar nil (o cualquier otra cosa) lo quita
                return;
            }

            sol::state_view lua(mgr.lua());
            sol::table tabla = lua[kUiCallbackTable];
            const long long clave = ++g_nextUiCallbackKey;
            tabla[clave] = fn;

            // La época es lo que impide llamar a un lua_State muerto: expira al
            // destruirse el ScriptManager y al recargar en caliente un script.
            // Se comprueba ANTES de tocar mgr, que para entonces también puede
            // haber muerto.
            std::weak_ptr<char> epoca = mgr.callbackEpoch();
            ScriptManager* m = &mgr;
            const std::string etiqueta = nombre;
            destino = [m, clave, epoca, etiqueta]() {
                if (epoca.expired()) return;

                sol::state_view lua(m->lua());
                sol::table tabla = lua[kUiCallbackTable];
                sol::object f = tabla[clave];
                if (f.get_type() != sol::type::function) return;

                // protected_function: un error dentro del callback se registra
                // y se sigue. Un botón con un script roto no puede tumbar el
                // frame ni comerse el resto de la UI.
                sol::protected_function pf = f;
                sol::protected_function_result r = pf();
                if (!r.valid())
                {
                    sol::error err = r;
                    m->log(std::string("[Lua][ERROR] ") + etiqueta + ": " + err.what());
                }
            };
        }

        // Igual que setUiCallback pero para los handlers que traen un VALOR (el
        // nuevo del slider, del checkbox, del toggle, de la barra). Es una
        // plantilla aparte y no una generalizacion de aquella porque el
        // OnClick/OnDoubleClick del Button no lleva argumento y su firma no
        // tiene por que cambiar.
        template <class... Args>
        void setUiValueCallback(ScriptManager& mgr, std::function<void(Args...)>& destino,
                                const char* nombre, const sol::object& fn)
        {
            if (!fn.valid() || fn.get_type() != sol::type::function)
            {
                destino = nullptr;   // pasar nil (o cualquier otra cosa) lo quita
                return;
            }

            sol::state_view lua(mgr.lua());
            sol::table tabla = lua[kUiCallbackTable];
            const long long clave = ++g_nextUiCallbackKey;
            tabla[clave] = fn;

            // Misma epoca que setUiCallback: es lo que impide llamar a un
            // lua_State muerto tras destruir el ScriptManager o recargar en
            // caliente. Se comprueba ANTES de tocar mgr.
            std::weak_ptr<char> epoca = mgr.callbackEpoch();
            ScriptManager* m = &mgr;
            const std::string etiqueta = nombre;
            destino = [m, clave, epoca, etiqueta](Args... v) {
                if (epoca.expired()) return;

                sol::state_view lua(m->lua());
                sol::table tabla = lua[kUiCallbackTable];
                sol::object f = tabla[clave];
                if (f.get_type() != sol::type::function) return;

                sol::protected_function pf = f;
                sol::protected_function_result r = pf(v...);
                if (!r.valid())
                {
                    sol::error err = r;
                    m->log(std::string("[Lua][ERROR] ") + etiqueta + ": " + err.what());
                }
            };
        }

        void registerUi(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();

            lua[kUiCallbackTable] = lua.create_table();

            // Enums como tablas de enteros: son los MISMOS valores que el C++
            // (el orden de los enum class), así que UiTextAlign.Center vale lo
            // que UiTextAlign::Center.
            lua["UiScaleMode"] = lua.create_table_with(
                "ConstantPixelSize", 0, "ScaleWithScreenSize", 1, "ConstantPhysicalSize", 2);
            lua["UiScreenMatch"] = lua.create_table_with(
                "MatchWidthOrHeight", 0, "Expand", 1, "Shrink", 2);
            lua["UiCanvasRenderMode"] = lua.create_table_with("ScreenSpace", 0, "World", 1);
            lua["UiBillboard"] = lua.create_table_with("None", 0, "YawOnly", 1, "Full", 2);
            lua["UiTextAlign"] = lua.create_table_with(
                "Left", 0, "Center", 1, "Right", 2, "Justify", 3);
            lua["UiTextVAlign"] = lua.create_table_with(
                "Top", 0, "Middle", 1, "Bottom", 2);
            lua["UiTextOverflow"] = lua.create_table_with(
                "Overflow", 0, "Clip", 1, "Ellipsis", 2);
            lua["UiProgressFillDirection"] = lua.create_table_with(
                "LeftToRight", 0, "RightToLeft", 1, "BottomToTop", 2, "TopToBottom", 3);
            lua["UiLayoutMode"] = lua.create_table_with(
                "None", 0, "Horizontal", 1, "Vertical", 2, "Grid", 3);
            lua["UiCrossAlign"] = lua.create_table_with(
                "Start", 0, "Center", 1, "End", 2);
            lua["UiInputContentType"] = lua.create_table_with(
                "Standard", 0, "IntegerNumber", 1, "DecimalNumber", 2,
                "Alphanumeric", 3, "Password", 4);
            lua["UiSliderDirection"] = lua.create_table_with(
                "LeftToRight", 0, "RightToLeft", 1, "BottomToTop", 2, "TopToBottom", 3);
            lua["UiScrollbarDirection"] = lua.create_table_with(
                "LeftToRight", 0, "RightToLeft", 1, "TopToBottom", 2, "BottomToTop", 3);
            lua["UiImageMode"] = lua.create_table_with(
                "Normal", 0, "Tiled", 1, "Sliced", 2, "Filled", 3);
            lua["UiFillDirection"] = lua.create_table_with(
                "Horizontal", 0, "Vertical", 1);
            lua["UiFillOrigin"] = lua.create_table_with(
                "Start", 0, "End", 1);
            lua["UiButtonTransition"] = lua.create_table_with(
                "ColorTint", 0, "SpriteSwap", 1, "Animation", 2);
            lua["UiButtonState"] = lua.create_table_with(
                "Normal", 0, "Hover", 1, "Pressed", 2, "Disabled", 3, "Selected", 4);

            // ── Canvas ──────────────────────────────────────────────────────
            lua.new_usertype<LuaCanvas>("Canvas",
                sol::no_constructor,
                "scaleMode",          uiEnumProp(canvasOf, &CanvasComponent::scaleMode, 2),
                "scaleFactor",        uiFloatProp(canvasOf, &CanvasComponent::scaleFactor, &mgr, "Canvas.scaleFactor"),
                "screenMatch",        uiEnumProp(canvasOf, &CanvasComponent::screenMatch, 2),
                "matchWidthOrHeight", uiFloatProp(canvasOf, &CanvasComponent::matchWidthOrHeight, &mgr, "Canvas.matchWidthOrHeight"),
                "screenDpi",          uiFloatProp(canvasOf, &CanvasComponent::screenDpi, &mgr, "Canvas.screenDpi"),
                "fallbackDpi",        uiFloatProp(canvasOf, &CanvasComponent::fallbackDpi, &mgr, "Canvas.fallbackDpi"),
                "referenceDpi",       uiFloatProp(canvasOf, &CanvasComponent::referenceDpi, &mgr, "Canvas.referenceDpi"),
                "aspectRatio",        uiFloatProp(canvasOf, &CanvasComponent::aspectRatio, &mgr, "Canvas.aspectRatio"),
                "renderMode", uiEnumProp(canvasOf, &CanvasComponent::renderMode, 1),
                "worldScale", uiFloatProp(canvasOf, &CanvasComponent::worldScale, &mgr, "Canvas.worldScale"),
                "billboard",  uiEnumProp(canvasOf, &CanvasComponent::billboard, 2),
                "depthTest",  uiProp(canvasOf, &CanvasComponent::depthTest),
                "GetReferenceResolution", uiVec2Get(canvasOf, &CanvasComponent::referenceResolution),
                "SetReferenceResolution", uiVec2Set(canvasOf, &CanvasComponent::referenceResolution, &mgr, "Canvas.SetReferenceResolution"),
                // El safe area son cuatro insets sueltos (no un vec4): se pasan
                // en el mismo orden que los declara UiSafeArea.
                "GetSafeArea", [](const LuaCanvas& c) {
                    const UiSafeArea& s = canvasOf(c)->safeArea;
                    return std::make_tuple(s.left, s.top, s.right, s.bottom);
                },
                "SetSafeArea", [&mgr](const LuaCanvas& c, float l, float t, float r, float b) {
                    CanvasComponent* comp = canvasOf(c);
                    if (!ensureFinite(mgr, "Canvas.SetSafeArea", glm::vec3(l, t, r)) ||
                        !ensureFinite(mgr, "Canvas.SetSafeArea", b)) return;
                    comp->safeArea = UiSafeArea{l, t, r, b};
                });

            // ── Button ──────────────────────────────────────────────────────
            lua.new_usertype<LuaButton>("Button",
                sol::no_constructor,
                "visible",      uiProp(buttonOf, &ButtonComponent::visible),
                "atlasPath",    uiProp(buttonOf, &ButtonComponent::atlasPath),
                "sprite",       uiProp(buttonOf, &ButtonComponent::sprite),
                "interactable", uiProp(buttonOf, &ButtonComponent::interactable),
                "selected",     uiProp(buttonOf, &ButtonComponent::selected),
                "transition",   uiEnumProp(buttonOf, &ButtonComponent::transition, 2),
                "normalSprite",   uiProp(buttonOf, &ButtonComponent::normalSprite),
                "hoverSprite",    uiProp(buttonOf, &ButtonComponent::hoverSprite),
                "pressedSprite",  uiProp(buttonOf, &ButtonComponent::pressedSprite),
                "disabledSprite", uiProp(buttonOf, &ButtonComponent::disabledSprite),
                "selectedSprite", uiProp(buttonOf, &ButtonComponent::selectedSprite),
                "fadeDuration", uiFloatProp(buttonOf, &ButtonComponent::fadeDuration, &mgr, "Button.fadeDuration"),
                "text",         uiProp(buttonOf, &ButtonComponent::text),
                "fontPath",     uiProp(buttonOf, &ButtonComponent::fontPath),
                "fontSize",     uiFloatProp(buttonOf, &ButtonComponent::fontSize, &mgr, "Button.fontSize"),
                "textAlign",    uiEnumProp(buttonOf, &ButtonComponent::textAlign, 3),
                "textVAlign",   uiEnumProp(buttonOf, &ButtonComponent::textVAlign, 2),
                "GetAnchorMin", uiVec2Get(buttonOf, &ButtonComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(buttonOf, &ButtonComponent::anchorMin, &mgr, "Button.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(buttonOf, &ButtonComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(buttonOf, &ButtonComponent::anchorMax, &mgr, "Button.SetAnchorMax"),
                "GetPivot",     uiVec2Get(buttonOf, &ButtonComponent::pivot),
                "SetPivot",     uiVec2Set(buttonOf, &ButtonComponent::pivot, &mgr, "Button.SetPivot"),
                "GetPosition",  uiVec2Get(buttonOf, &ButtonComponent::position),
                "SetPosition",  uiVec2Set(buttonOf, &ButtonComponent::position, &mgr, "Button.SetPosition"),
                "GetSize",      uiVec2Get(buttonOf, &ButtonComponent::size),
                "SetSize",      uiVec2Set(buttonOf, &ButtonComponent::size, &mgr, "Button.SetSize"),
                "GetColor",     uiVec4Get(buttonOf, &ButtonComponent::color),
                "SetColor",     uiVec4Set(buttonOf, &ButtonComponent::color, &mgr, "Button.SetColor"),
                "GetNormalColor",   uiVec4Get(buttonOf, &ButtonComponent::normalColor),
                "SetNormalColor",   uiVec4Set(buttonOf, &ButtonComponent::normalColor, &mgr, "Button.SetNormalColor"),
                "GetHoverColor",    uiVec4Get(buttonOf, &ButtonComponent::hoverColor),
                "SetHoverColor",    uiVec4Set(buttonOf, &ButtonComponent::hoverColor, &mgr, "Button.SetHoverColor"),
                "GetPressedColor",  uiVec4Get(buttonOf, &ButtonComponent::pressedColor),
                "SetPressedColor",  uiVec4Set(buttonOf, &ButtonComponent::pressedColor, &mgr, "Button.SetPressedColor"),
                "GetDisabledColor", uiVec4Get(buttonOf, &ButtonComponent::disabledColor),
                "SetDisabledColor", uiVec4Set(buttonOf, &ButtonComponent::disabledColor, &mgr, "Button.SetDisabledColor"),
                "GetSelectedColor", uiVec4Get(buttonOf, &ButtonComponent::selectedColor),
                "SetSelectedColor", uiVec4Set(buttonOf, &ButtonComponent::selectedColor, &mgr, "Button.SetSelectedColor"),
                "GetTextColor",     uiVec4Get(buttonOf, &ButtonComponent::textColor),
                "SetTextColor",     uiVec4Set(buttonOf, &ButtonComponent::textColor, &mgr, "Button.SetTextColor"),
                // Estado: lo escribe el canvas en el nodo vivo y el sync lo
                // publica en el componente. Solo lectura, como en C++.
                "GetState", [](const LuaButton& b) {
                    return static_cast<int>(buttonOf(b)->callbacks.ptr->state);
                },
                "OnClick", [&mgr](const LuaButton& b, sol::object fn) {
                    setUiCallback(mgr, buttonOf(b)->callbacks.ptr->onClick, "Button.OnClick", fn);
                },
                "OnDoubleClick", [&mgr](const LuaButton& b, sol::object fn) {
                    setUiCallback(mgr, buttonOf(b)->callbacks.ptr->onDoubleClick, "Button.OnDoubleClick", fn);
                });

            // ── Text ────────────────────────────────────────────────────────
            lua.new_usertype<LuaText>("Text",
                sol::no_constructor,
                "visible",      uiProp(textOf, &TextComponent::visible),
                "text",         uiProp(textOf, &TextComponent::text),
                "fontPath",     uiProp(textOf, &TextComponent::fontPath),
                "fontSize",     uiFloatProp(textOf, &TextComponent::fontSize, &mgr, "Text.fontSize"),
                "outlineWidth", uiFloatProp(textOf, &TextComponent::outlineWidth, &mgr, "Text.outlineWidth"),
                "align",        uiEnumProp(textOf, &TextComponent::align, 3),
                "vAlign",       uiEnumProp(textOf, &TextComponent::vAlign, 2),
                "overflow",     uiEnumProp(textOf, &TextComponent::overflow, 2),
                "wordWrap",     uiProp(textOf, &TextComponent::wordWrap),
                "boldStrength", uiFloatProp(textOf, &TextComponent::boldStrength, &mgr, "Text.boldStrength"),
                "italicSkew",   uiFloatProp(textOf, &TextComponent::italicSkew, &mgr, "Text.italicSkew"),
                "GetAnchorMin", uiVec2Get(textOf, &TextComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(textOf, &TextComponent::anchorMin, &mgr, "Text.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(textOf, &TextComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(textOf, &TextComponent::anchorMax, &mgr, "Text.SetAnchorMax"),
                "GetPivot",     uiVec2Get(textOf, &TextComponent::pivot),
                "SetPivot",     uiVec2Set(textOf, &TextComponent::pivot, &mgr, "Text.SetPivot"),
                "GetPosition",  uiVec2Get(textOf, &TextComponent::position),
                "SetPosition",  uiVec2Set(textOf, &TextComponent::position, &mgr, "Text.SetPosition"),
                "GetSize",      uiVec2Get(textOf, &TextComponent::size),
                "SetSize",      uiVec2Set(textOf, &TextComponent::size, &mgr, "Text.SetSize"),
                "GetShadowOffset", uiVec2Get(textOf, &TextComponent::shadowOffset),
                "SetShadowOffset", uiVec2Set(textOf, &TextComponent::shadowOffset, &mgr, "Text.SetShadowOffset"),
                "GetColor",        uiVec4Get(textOf, &TextComponent::color),
                "SetColor",        uiVec4Set(textOf, &TextComponent::color, &mgr, "Text.SetColor"),
                "GetOutlineColor", uiVec4Get(textOf, &TextComponent::outlineColor),
                "SetOutlineColor", uiVec4Set(textOf, &TextComponent::outlineColor, &mgr, "Text.SetOutlineColor"),
                "GetShadowColor",  uiVec4Get(textOf, &TextComponent::shadowColor),
                "SetShadowColor",  uiVec4Set(textOf, &TextComponent::shadowColor, &mgr, "Text.SetShadowColor"));

            // ── ProgressBar ─────────────────────────────────────────────────
            lua.new_usertype<LuaProgressBar>("ProgressBar",
                sol::no_constructor,
                "visible",        uiProp(barOf, &ProgressBarComponent::visible),
                "value",          uiFloatProp(barOf, &ProgressBarComponent::value, &mgr, "ProgressBar.value"),
                "minValue",       uiFloatProp(barOf, &ProgressBarComponent::minValue, &mgr, "ProgressBar.minValue"),
                "maxValue",       uiFloatProp(barOf, &ProgressBarComponent::maxValue, &mgr, "ProgressBar.maxValue"),
                "fillDirection",  uiEnumProp(barOf, &ProgressBarComponent::fillDirection, 3),
                "atlasPath",      uiProp(barOf, &ProgressBarComponent::atlasPath),
                "backgroundPath", uiProp(barOf, &ProgressBarComponent::backgroundPath),
                "fillPath",       uiProp(barOf, &ProgressBarComponent::fillPath),
                "GetAnchorMin", uiVec2Get(barOf, &ProgressBarComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(barOf, &ProgressBarComponent::anchorMin, &mgr, "ProgressBar.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(barOf, &ProgressBarComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(barOf, &ProgressBarComponent::anchorMax, &mgr, "ProgressBar.SetAnchorMax"),
                "GetPivot",     uiVec2Get(barOf, &ProgressBarComponent::pivot),
                "SetPivot",     uiVec2Set(barOf, &ProgressBarComponent::pivot, &mgr, "ProgressBar.SetPivot"),
                "GetPosition",  uiVec2Get(barOf, &ProgressBarComponent::position),
                "SetPosition",  uiVec2Set(barOf, &ProgressBarComponent::position, &mgr, "ProgressBar.SetPosition"),
                "GetSize",      uiVec2Get(barOf, &ProgressBarComponent::size),
                "SetSize",      uiVec2Set(barOf, &ProgressBarComponent::size, &mgr, "ProgressBar.SetSize"),
                "GetColor",     uiVec4Get(barOf, &ProgressBarComponent::color),
                "SetColor",     uiVec4Set(barOf, &ProgressBarComponent::color, &mgr, "ProgressBar.SetColor"),
                "GetFillColor", uiVec4Get(barOf, &ProgressBarComponent::fillColor),
                "SetFillColor", uiVec4Set(barOf, &ProgressBarComponent::fillColor, &mgr, "ProgressBar.SetFillColor"),
                // Lo mismo que normalizedValue() en C++: el 0..1 ya acotado que
                // usa el sync para el rect del relleno.
                "GetNormalizedValue", [](const LuaProgressBar& b) {
                    return barOf(b)->normalizedValue();
                });

            // ── Layout ──────────────────────────────────────────────────────
            // El único de los cuatro que no dibuja: coloca. Por eso no tiene ni
            // color ni sprite, y sí el modo, el padding y la celda.
            lua.new_usertype<LuaLayout>("Layout",
                sol::no_constructor,
                "visible",       uiProp(layoutOf, &LayoutComponent::visible),
                "mode",          uiEnumProp(layoutOf, &LayoutComponent::mode, 3),
                "crossAlign",    uiEnumProp(layoutOf, &LayoutComponent::crossAlign, 2),
                "paddingLeft",   uiFloatProp(layoutOf, &LayoutComponent::paddingLeft, &mgr, "Layout.paddingLeft"),
                "paddingRight",  uiFloatProp(layoutOf, &LayoutComponent::paddingRight, &mgr, "Layout.paddingRight"),
                "paddingTop",    uiFloatProp(layoutOf, &LayoutComponent::paddingTop, &mgr, "Layout.paddingTop"),
                "paddingBottom", uiFloatProp(layoutOf, &LayoutComponent::paddingBottom, &mgr, "Layout.paddingBottom"),
                // columns es entero: sin el guardarraíl de NaN de uiFloatProp y
                // sin decimales que redondear a espaldas del script.
                "columns",       uiProp(layoutOf, &LayoutComponent::columns),
                "fitWidth",      uiProp(layoutOf, &LayoutComponent::fitWidth),
                "fitHeight",     uiProp(layoutOf, &LayoutComponent::fitHeight),
                "ignoreLayout",  uiProp(layoutOf, &LayoutComponent::ignoreLayout),
                "clipChildren",  uiProp(layoutOf, &LayoutComponent::clipChildren),
                "GetAnchorMin", uiVec2Get(layoutOf, &LayoutComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(layoutOf, &LayoutComponent::anchorMin, &mgr, "Layout.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(layoutOf, &LayoutComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(layoutOf, &LayoutComponent::anchorMax, &mgr, "Layout.SetAnchorMax"),
                "GetPivot",     uiVec2Get(layoutOf, &LayoutComponent::pivot),
                "SetPivot",     uiVec2Set(layoutOf, &LayoutComponent::pivot, &mgr, "Layout.SetPivot"),
                "GetPosition",  uiVec2Get(layoutOf, &LayoutComponent::position),
                "SetPosition",  uiVec2Set(layoutOf, &LayoutComponent::position, &mgr, "Layout.SetPosition"),
                "GetSize",      uiVec2Get(layoutOf, &LayoutComponent::size),
                "SetSize",      uiVec2Set(layoutOf, &LayoutComponent::size, &mgr, "Layout.SetSize"),
                "GetSpacing",   uiVec2Get(layoutOf, &LayoutComponent::spacing),
                "SetSpacing",   uiVec2Set(layoutOf, &LayoutComponent::spacing, &mgr, "Layout.SetSpacing"),
                "GetCellSize",  uiVec2Get(layoutOf, &LayoutComponent::cellSize),
                "SetCellSize",  uiVec2Set(layoutOf, &LayoutComponent::cellSize, &mgr, "Layout.SetCellSize"));

            // ── Panel ───────────────────────────────────────────────────────
            // El rectángulo de fondo. Sin campos propios más allá del rect, el
            // color y el sprite: el Panel del núcleo tampoco los tiene.
            lua.new_usertype<LuaPanel>("Panel",
                sol::no_constructor,
                "visible",       uiProp(panelOf, &PanelComponent::visible),
                "raycastTarget", uiProp(panelOf, &PanelComponent::raycastTarget),
                "atlasPath",     uiProp(panelOf, &PanelComponent::atlasPath),
                "sprite",        uiProp(panelOf, &PanelComponent::sprite),
                "GetAnchorMin", uiVec2Get(panelOf, &PanelComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(panelOf, &PanelComponent::anchorMin, &mgr, "Panel.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(panelOf, &PanelComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(panelOf, &PanelComponent::anchorMax, &mgr, "Panel.SetAnchorMax"),
                "GetPivot",     uiVec2Get(panelOf, &PanelComponent::pivot),
                "SetPivot",     uiVec2Set(panelOf, &PanelComponent::pivot, &mgr, "Panel.SetPivot"),
                "GetPosition",  uiVec2Get(panelOf, &PanelComponent::position),
                "SetPosition",  uiVec2Set(panelOf, &PanelComponent::position, &mgr, "Panel.SetPosition"),
                "GetSize",      uiVec2Get(panelOf, &PanelComponent::size),
                "SetSize",      uiVec2Set(panelOf, &PanelComponent::size, &mgr, "Panel.SetSize"),
                "GetColor",     uiVec4Get(panelOf, &PanelComponent::color),
                "SetColor",     uiVec4Set(panelOf, &PanelComponent::color, &mgr, "Panel.SetColor"));

            // ── Image ───────────────────────────────────────────────────────
            // Con los NUEVE campos propios del widget del núcleo: el modo, los
            // cuatro bordes del 9-slice con su fillCenter, el tope de tiles y el
            // bloque de Filled.
            lua.new_usertype<LuaImage>("Image",
                sol::no_constructor,
                "visible",       uiProp(imageOf, &ImageComponent::visible),
                "raycastTarget", uiProp(imageOf, &ImageComponent::raycastTarget),
                "atlasPath",     uiProp(imageOf, &ImageComponent::atlasPath),
                "sprite",        uiProp(imageOf, &ImageComponent::sprite),
                "mode",          uiEnumProp(imageOf, &ImageComponent::mode, 3),
                "borderLeft",    uiFloatProp(imageOf, &ImageComponent::borderLeft, &mgr, "Image.borderLeft"),
                "borderRight",   uiFloatProp(imageOf, &ImageComponent::borderRight, &mgr, "Image.borderRight"),
                "borderTop",     uiFloatProp(imageOf, &ImageComponent::borderTop, &mgr, "Image.borderTop"),
                "borderBottom",  uiFloatProp(imageOf, &ImageComponent::borderBottom, &mgr, "Image.borderBottom"),
                "fillCenter",    uiProp(imageOf, &ImageComponent::fillCenter),
                // maxTiles es entero: sin el guardarraíl de NaN de uiFloatProp y
                // sin decimales que redondear a espaldas del script.
                "maxTiles",      uiProp(imageOf, &ImageComponent::maxTiles),
                "fillDirection", uiEnumProp(imageOf, &ImageComponent::fillDirection, 1),
                "fillOrigin",    uiEnumProp(imageOf, &ImageComponent::fillOrigin, 1),
                "fillAmount",    uiFloatProp(imageOf, &ImageComponent::fillAmount, &mgr, "Image.fillAmount"),
                "GetAnchorMin", uiVec2Get(imageOf, &ImageComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(imageOf, &ImageComponent::anchorMin, &mgr, "Image.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(imageOf, &ImageComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(imageOf, &ImageComponent::anchorMax, &mgr, "Image.SetAnchorMax"),
                "GetPivot",     uiVec2Get(imageOf, &ImageComponent::pivot),
                "SetPivot",     uiVec2Set(imageOf, &ImageComponent::pivot, &mgr, "Image.SetPivot"),
                "GetPosition",  uiVec2Get(imageOf, &ImageComponent::position),
                "SetPosition",  uiVec2Set(imageOf, &ImageComponent::position, &mgr, "Image.SetPosition"),
                "GetSize",      uiVec2Get(imageOf, &ImageComponent::size),
                "SetSize",      uiVec2Set(imageOf, &ImageComponent::size, &mgr, "Image.SetSize"),
                "GetColor",     uiVec4Get(imageOf, &ImageComponent::color),
                "SetColor",     uiVec4Set(imageOf, &ImageComponent::color, &mgr, "Image.SetColor"));

            // ── Slider ──────────────────────────────────────────────────────
            // El primero de los interactivos: lo que el jugador mueve se escribe
            // en el COMPONENTE, asi que leer `value` aqui da el valor de verdad
            // sin sondear el nodo del canvas.
            lua.new_usertype<LuaSlider>("Slider",
                sol::no_constructor,
                "visible",          uiProp(sliderOf, &SliderComponent::visible),
                "interactable",     uiProp(sliderOf, &SliderComponent::interactable),
                "value",            uiFloatProp(sliderOf, &SliderComponent::value, &mgr, "Slider.value"),
                "minValue",         uiFloatProp(sliderOf, &SliderComponent::minValue, &mgr, "Slider.minValue"),
                "maxValue",         uiFloatProp(sliderOf, &SliderComponent::maxValue, &mgr, "Slider.maxValue"),
                "wholeNumbers",     uiProp(sliderOf, &SliderComponent::wholeNumbers),
                "direction",        uiEnumProp(sliderOf, &SliderComponent::direction, 3),
                "handleSize",       uiFloatProp(sliderOf, &SliderComponent::handleSize, &mgr, "Slider.handleSize"),
                "atlasPath",        uiProp(sliderOf, &SliderComponent::atlasPath),
                "backgroundSprite", uiProp(sliderOf, &SliderComponent::backgroundSprite),
                "fillSprite",       uiProp(sliderOf, &SliderComponent::fillSprite),
                "handleSprite",     uiProp(sliderOf, &SliderComponent::handleSprite),
                "GetAnchorMin", uiVec2Get(sliderOf, &SliderComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(sliderOf, &SliderComponent::anchorMin, &mgr, "Slider.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(sliderOf, &SliderComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(sliderOf, &SliderComponent::anchorMax, &mgr, "Slider.SetAnchorMax"),
                "GetPivot",     uiVec2Get(sliderOf, &SliderComponent::pivot),
                "SetPivot",     uiVec2Set(sliderOf, &SliderComponent::pivot, &mgr, "Slider.SetPivot"),
                "GetPosition",  uiVec2Get(sliderOf, &SliderComponent::position),
                "SetPosition",  uiVec2Set(sliderOf, &SliderComponent::position, &mgr, "Slider.SetPosition"),
                "GetSize",      uiVec2Get(sliderOf, &SliderComponent::size),
                "SetSize",      uiVec2Set(sliderOf, &SliderComponent::size, &mgr, "Slider.SetSize"),
                "GetColor",     uiVec4Get(sliderOf, &SliderComponent::color),
                "SetColor",     uiVec4Set(sliderOf, &SliderComponent::color, &mgr, "Slider.SetColor"),
                "GetFillColor", uiVec4Get(sliderOf, &SliderComponent::fillColor),
                "SetFillColor", uiVec4Set(sliderOf, &SliderComponent::fillColor, &mgr, "Slider.SetFillColor"),
                "GetHandleColor", uiVec4Get(sliderOf, &SliderComponent::handleColor),
                "SetHandleColor", uiVec4Set(sliderOf, &SliderComponent::handleColor, &mgr, "Slider.SetHandleColor"),
                // Lo mismo que normalizedValue() en C++: el 0..1 ya acotado.
                "GetNormalizedValue", [](const LuaSlider& s) {
                    return sliderOf(s)->normalizedValue();
                },
                "OnValueChanged", [&mgr](const LuaSlider& s, sol::object fn) {
                    setUiValueCallback<float>(mgr, sliderOf(s)->callbacks.ptr->onValueChanged,
                                              "Slider.OnValueChanged", fn);
                });

            // ── Checkbox ────────────────────────────────────────────────────
            lua.new_usertype<LuaCheckbox>("Checkbox",
                sol::no_constructor,
                "visible",          uiProp(checkboxOf, &CheckboxComponent::visible),
                "interactable",     uiProp(checkboxOf, &CheckboxComponent::interactable),
                "isOn",             uiProp(checkboxOf, &CheckboxComponent::isOn),
                "checkPadding",     uiFloatProp(checkboxOf, &CheckboxComponent::checkPadding, &mgr, "Checkbox.checkPadding"),
                "atlasPath",        uiProp(checkboxOf, &CheckboxComponent::atlasPath),
                "backgroundSprite", uiProp(checkboxOf, &CheckboxComponent::backgroundSprite),
                "checkmarkSprite",  uiProp(checkboxOf, &CheckboxComponent::checkmarkSprite),
                "GetAnchorMin", uiVec2Get(checkboxOf, &CheckboxComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(checkboxOf, &CheckboxComponent::anchorMin, &mgr, "Checkbox.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(checkboxOf, &CheckboxComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(checkboxOf, &CheckboxComponent::anchorMax, &mgr, "Checkbox.SetAnchorMax"),
                "GetPivot",     uiVec2Get(checkboxOf, &CheckboxComponent::pivot),
                "SetPivot",     uiVec2Set(checkboxOf, &CheckboxComponent::pivot, &mgr, "Checkbox.SetPivot"),
                "GetPosition",  uiVec2Get(checkboxOf, &CheckboxComponent::position),
                "SetPosition",  uiVec2Set(checkboxOf, &CheckboxComponent::position, &mgr, "Checkbox.SetPosition"),
                "GetSize",      uiVec2Get(checkboxOf, &CheckboxComponent::size),
                "SetSize",      uiVec2Set(checkboxOf, &CheckboxComponent::size, &mgr, "Checkbox.SetSize"),
                "GetColor",     uiVec4Get(checkboxOf, &CheckboxComponent::color),
                "SetColor",     uiVec4Set(checkboxOf, &CheckboxComponent::color, &mgr, "Checkbox.SetColor"),
                "GetCheckColor", uiVec4Get(checkboxOf, &CheckboxComponent::checkColor),
                "SetCheckColor", uiVec4Set(checkboxOf, &CheckboxComponent::checkColor, &mgr, "Checkbox.SetCheckColor"),
                "OnValueChanged", [&mgr](const LuaCheckbox& c, sol::object fn) {
                    setUiValueCallback<bool>(mgr, checkboxOf(c)->callbacks.ptr->onValueChanged,
                                             "Checkbox.OnValueChanged", fn);
                });

            // ── Toggle ──────────────────────────────────────────────────────
            // Sin `color`: la pista la pinta el sync con offColor u onColor segun
            // el estado, asi que un campo de color suelto seria uno que el primer
            // volcado pisa y que parece no hacer nada.
            lua.new_usertype<LuaToggle>("Toggle",
                sol::no_constructor,
                "visible",          uiProp(toggleOf, &ToggleComponent::visible),
                "interactable",     uiProp(toggleOf, &ToggleComponent::interactable),
                "isOn",             uiProp(toggleOf, &ToggleComponent::isOn),
                "knobSize",         uiFloatProp(toggleOf, &ToggleComponent::knobSize, &mgr, "Toggle.knobSize"),
                "knobPadding",      uiFloatProp(toggleOf, &ToggleComponent::knobPadding, &mgr, "Toggle.knobPadding"),
                "atlasPath",        uiProp(toggleOf, &ToggleComponent::atlasPath),
                "backgroundSprite", uiProp(toggleOf, &ToggleComponent::backgroundSprite),
                "knobSprite",       uiProp(toggleOf, &ToggleComponent::knobSprite),
                "GetAnchorMin", uiVec2Get(toggleOf, &ToggleComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(toggleOf, &ToggleComponent::anchorMin, &mgr, "Toggle.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(toggleOf, &ToggleComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(toggleOf, &ToggleComponent::anchorMax, &mgr, "Toggle.SetAnchorMax"),
                "GetPivot",     uiVec2Get(toggleOf, &ToggleComponent::pivot),
                "SetPivot",     uiVec2Set(toggleOf, &ToggleComponent::pivot, &mgr, "Toggle.SetPivot"),
                "GetPosition",  uiVec2Get(toggleOf, &ToggleComponent::position),
                "SetPosition",  uiVec2Set(toggleOf, &ToggleComponent::position, &mgr, "Toggle.SetPosition"),
                "GetSize",      uiVec2Get(toggleOf, &ToggleComponent::size),
                "SetSize",      uiVec2Set(toggleOf, &ToggleComponent::size, &mgr, "Toggle.SetSize"),
                "GetOffColor",  uiVec4Get(toggleOf, &ToggleComponent::offColor),
                "SetOffColor",  uiVec4Set(toggleOf, &ToggleComponent::offColor, &mgr, "Toggle.SetOffColor"),
                "GetOnColor",   uiVec4Get(toggleOf, &ToggleComponent::onColor),
                "SetOnColor",   uiVec4Set(toggleOf, &ToggleComponent::onColor, &mgr, "Toggle.SetOnColor"),
                "GetKnobColor", uiVec4Get(toggleOf, &ToggleComponent::knobColor),
                "SetKnobColor", uiVec4Set(toggleOf, &ToggleComponent::knobColor, &mgr, "Toggle.SetKnobColor"),
                "OnValueChanged", [&mgr](const LuaToggle& t, sol::object fn) {
                    setUiValueCallback<bool>(mgr, toggleOf(t)->callbacks.ptr->onValueChanged,
                                             "Toggle.OnValueChanged", fn);
                });

            // ── Scrollbar ───────────────────────────────────────────────────
            lua.new_usertype<LuaScrollbar>("Scrollbar",
                sol::no_constructor,
                "visible",          uiProp(scrollbarOf, &ScrollbarComponent::visible),
                "interactable",     uiProp(scrollbarOf, &ScrollbarComponent::interactable),
                "value",            uiFloatProp(scrollbarOf, &ScrollbarComponent::value, &mgr, "Scrollbar.value"),
                "handleFraction",   uiFloatProp(scrollbarOf, &ScrollbarComponent::handleFraction, &mgr, "Scrollbar.handleFraction"),
                "direction",        uiEnumProp(scrollbarOf, &ScrollbarComponent::direction, 3),
                // numberOfSteps es entero: sin el guardarrail de NaN de
                // uiFloatProp y sin decimales que redondear a espaldas del script.
                "numberOfSteps",    uiProp(scrollbarOf, &ScrollbarComponent::numberOfSteps),
                "scrollStep",       uiFloatProp(scrollbarOf, &ScrollbarComponent::scrollStep, &mgr, "Scrollbar.scrollStep"),
                "atlasPath",        uiProp(scrollbarOf, &ScrollbarComponent::atlasPath),
                "backgroundSprite", uiProp(scrollbarOf, &ScrollbarComponent::backgroundSprite),
                "handleSprite",     uiProp(scrollbarOf, &ScrollbarComponent::handleSprite),
                "GetAnchorMin", uiVec2Get(scrollbarOf, &ScrollbarComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(scrollbarOf, &ScrollbarComponent::anchorMin, &mgr, "Scrollbar.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(scrollbarOf, &ScrollbarComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(scrollbarOf, &ScrollbarComponent::anchorMax, &mgr, "Scrollbar.SetAnchorMax"),
                "GetPivot",     uiVec2Get(scrollbarOf, &ScrollbarComponent::pivot),
                "SetPivot",     uiVec2Set(scrollbarOf, &ScrollbarComponent::pivot, &mgr, "Scrollbar.SetPivot"),
                "GetPosition",  uiVec2Get(scrollbarOf, &ScrollbarComponent::position),
                "SetPosition",  uiVec2Set(scrollbarOf, &ScrollbarComponent::position, &mgr, "Scrollbar.SetPosition"),
                "GetSize",      uiVec2Get(scrollbarOf, &ScrollbarComponent::size),
                "SetSize",      uiVec2Set(scrollbarOf, &ScrollbarComponent::size, &mgr, "Scrollbar.SetSize"),
                "GetColor",     uiVec4Get(scrollbarOf, &ScrollbarComponent::color),
                "SetColor",     uiVec4Set(scrollbarOf, &ScrollbarComponent::color, &mgr, "Scrollbar.SetColor"),
                "GetHandleColor", uiVec4Get(scrollbarOf, &ScrollbarComponent::handleColor),
                "SetHandleColor", uiVec4Set(scrollbarOf, &ScrollbarComponent::handleColor, &mgr, "Scrollbar.SetHandleColor"),
                // El mismo enganche a paradas discretas que aplica el arrastre.
                "SnapValue", [](const LuaScrollbar& s, float v) {
                    return scrollbarOf(s)->snapValue(v);
                },
                "OnValueChanged", [&mgr](const LuaScrollbar& s, sol::object fn) {
                    setUiValueCallback<float>(mgr, scrollbarOf(s)->callbacks.ptr->onValueChanged,
                                              "Scrollbar.OnValueChanged", fn);
                });

            // ── InputField ──────────────────────────────────────────────────
            // El unico en el que el JUGADOR escribe. `text` es el texto de
            // verdad: en Password se guarda tal cual y solo cambia lo que se
            // ENSENA, que es lo que devuelve GetDisplayText.
            lua.new_usertype<LuaInputField>("InputField",
                sol::no_constructor,
                "visible",          uiProp(inputFieldOf, &InputFieldComponent::visible),
                "interactable",     uiProp(inputFieldOf, &InputFieldComponent::interactable),
                "readOnly",         uiProp(inputFieldOf, &InputFieldComponent::readOnly),
                "text",             uiProp(inputFieldOf, &InputFieldComponent::text),
                "placeholder",      uiProp(inputFieldOf, &InputFieldComponent::placeholder),
                "fontPath",         uiProp(inputFieldOf, &InputFieldComponent::fontPath),
                "fontSize",         uiFloatProp(inputFieldOf, &InputFieldComponent::fontSize, &mgr, "InputField.fontSize"),
                "align",            uiEnumProp(inputFieldOf, &InputFieldComponent::align, 3),
                "padding",          uiFloatProp(inputFieldOf, &InputFieldComponent::padding, &mgr, "InputField.padding"),
                // characterLimit es entero: sin el guardarrail de NaN y sin
                // decimales que redondear a espaldas del script.
                "characterLimit",   uiProp(inputFieldOf, &InputFieldComponent::characterLimit),
                "contentType",      uiEnumProp(inputFieldOf, &InputFieldComponent::contentType, 4),
                "passwordChar",     uiProp(inputFieldOf, &InputFieldComponent::passwordChar),
                "caretWidth",       uiFloatProp(inputFieldOf, &InputFieldComponent::caretWidth, &mgr, "InputField.caretWidth"),
                "caretBlinkRate",   uiFloatProp(inputFieldOf, &InputFieldComponent::caretBlinkRate, &mgr, "InputField.caretBlinkRate"),
                "atlasPath",        uiProp(inputFieldOf, &InputFieldComponent::atlasPath),
                "backgroundSprite", uiProp(inputFieldOf, &InputFieldComponent::backgroundSprite),
                "GetAnchorMin", uiVec2Get(inputFieldOf, &InputFieldComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(inputFieldOf, &InputFieldComponent::anchorMin, &mgr, "InputField.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(inputFieldOf, &InputFieldComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(inputFieldOf, &InputFieldComponent::anchorMax, &mgr, "InputField.SetAnchorMax"),
                "GetPivot",     uiVec2Get(inputFieldOf, &InputFieldComponent::pivot),
                "SetPivot",     uiVec2Set(inputFieldOf, &InputFieldComponent::pivot, &mgr, "InputField.SetPivot"),
                "GetPosition",  uiVec2Get(inputFieldOf, &InputFieldComponent::position),
                "SetPosition",  uiVec2Set(inputFieldOf, &InputFieldComponent::position, &mgr, "InputField.SetPosition"),
                "GetSize",      uiVec2Get(inputFieldOf, &InputFieldComponent::size),
                "SetSize",      uiVec2Set(inputFieldOf, &InputFieldComponent::size, &mgr, "InputField.SetSize"),
                "GetColor",     uiVec4Get(inputFieldOf, &InputFieldComponent::color),
                "SetColor",     uiVec4Set(inputFieldOf, &InputFieldComponent::color, &mgr, "InputField.SetColor"),
                "GetTextColor", uiVec4Get(inputFieldOf, &InputFieldComponent::textColor),
                "SetTextColor", uiVec4Set(inputFieldOf, &InputFieldComponent::textColor, &mgr, "InputField.SetTextColor"),
                "GetPlaceholderColor", uiVec4Get(inputFieldOf, &InputFieldComponent::placeholderColor),
                "SetPlaceholderColor", uiVec4Set(inputFieldOf, &InputFieldComponent::placeholderColor, &mgr, "InputField.SetPlaceholderColor"),
                "GetCaretColor", uiVec4Get(inputFieldOf, &InputFieldComponent::caretColor),
                "SetCaretColor", uiVec4Set(inputFieldOf, &InputFieldComponent::caretColor, &mgr, "InputField.SetCaretColor"),
                // Lo que se DIBUJA: el placeholder si esta vacio, o la mascara si
                // es Password. Nunca la contrasena.
                "GetDisplayText", [](const LuaInputField& f) {
                    return inputFieldOf(f)->displayText();
                },
                // Posicion del cursor en CARACTERES (no en bytes), 0 = antes del
                // primero. Se acota al escribirla: un cursor fuera del texto
                // partiria la cadena en el siguiente borrado.
                "GetCaretPos", [](const LuaInputField& f) {
                    return inputFieldOf(f)->caretPos;
                },
                "SetCaretPos", [](const LuaInputField& f, int v) {
                    InputFieldComponent* c = inputFieldOf(f);
                    c->caretPos = 0;
                    c->moveCaret(v);
                },
                "OnValueChanged", [&mgr](const LuaInputField& f, sol::object fn) {
                    setUiValueCallback<const std::string&>(
                        mgr, inputFieldOf(f)->callbacks.ptr->onValueChanged,
                        "InputField.OnValueChanged", fn);
                },
                "OnEndEdit", [&mgr](const LuaInputField& f, sol::object fn) {
                    setUiValueCallback<const std::string&>(
                        mgr, inputFieldOf(f)->callbacks.ptr->onEndEdit,
                        "InputField.OnEndEdit", fn);
                });

            // ── Dropdown ────────────────────────────────────────────────────
            // Las opciones se leen y se escriben con indices 1-BASED, que es lo
            // natural en Lua; `value` sigue siendo el indice 0-based del
            // componente, igual que en C++ y que en el inspector.
            lua.new_usertype<LuaDropdown>("Dropdown",
                sol::no_constructor,
                "visible",          uiProp(dropdownOf, &DropdownComponent::visible),
                "interactable",     uiProp(dropdownOf, &DropdownComponent::interactable),
                "value",            uiProp(dropdownOf, &DropdownComponent::value),
                "isOpen",           uiProp(dropdownOf, &DropdownComponent::isOpen),
                "itemHeight",       uiFloatProp(dropdownOf, &DropdownComponent::itemHeight, &mgr, "Dropdown.itemHeight"),
                "maxVisibleItems",  uiProp(dropdownOf, &DropdownComponent::maxVisibleItems),
                "fontPath",         uiProp(dropdownOf, &DropdownComponent::fontPath),
                "fontSize",         uiFloatProp(dropdownOf, &DropdownComponent::fontSize, &mgr, "Dropdown.fontSize"),
                "padding",          uiFloatProp(dropdownOf, &DropdownComponent::padding, &mgr, "Dropdown.padding"),
                "atlasPath",        uiProp(dropdownOf, &DropdownComponent::atlasPath),
                "backgroundSprite", uiProp(dropdownOf, &DropdownComponent::backgroundSprite),
                "arrowSprite",      uiProp(dropdownOf, &DropdownComponent::arrowSprite),
                "itemSprite",       uiProp(dropdownOf, &DropdownComponent::itemSprite),
                "GetAnchorMin", uiVec2Get(dropdownOf, &DropdownComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(dropdownOf, &DropdownComponent::anchorMin, &mgr, "Dropdown.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(dropdownOf, &DropdownComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(dropdownOf, &DropdownComponent::anchorMax, &mgr, "Dropdown.SetAnchorMax"),
                "GetPivot",     uiVec2Get(dropdownOf, &DropdownComponent::pivot),
                "SetPivot",     uiVec2Set(dropdownOf, &DropdownComponent::pivot, &mgr, "Dropdown.SetPivot"),
                "GetPosition",  uiVec2Get(dropdownOf, &DropdownComponent::position),
                "SetPosition",  uiVec2Set(dropdownOf, &DropdownComponent::position, &mgr, "Dropdown.SetPosition"),
                "GetSize",      uiVec2Get(dropdownOf, &DropdownComponent::size),
                "SetSize",      uiVec2Set(dropdownOf, &DropdownComponent::size, &mgr, "Dropdown.SetSize"),
                "GetColor",     uiVec4Get(dropdownOf, &DropdownComponent::color),
                "SetColor",     uiVec4Set(dropdownOf, &DropdownComponent::color, &mgr, "Dropdown.SetColor"),
                "GetListColor", uiVec4Get(dropdownOf, &DropdownComponent::listColor),
                "SetListColor", uiVec4Set(dropdownOf, &DropdownComponent::listColor, &mgr, "Dropdown.SetListColor"),
                "GetItemColor", uiVec4Get(dropdownOf, &DropdownComponent::itemColor),
                "SetItemColor", uiVec4Set(dropdownOf, &DropdownComponent::itemColor, &mgr, "Dropdown.SetItemColor"),
                "GetItemSelectedColor", uiVec4Get(dropdownOf, &DropdownComponent::itemSelectedColor),
                "SetItemSelectedColor", uiVec4Set(dropdownOf, &DropdownComponent::itemSelectedColor, &mgr, "Dropdown.SetItemSelectedColor"),
                "GetArrowColor", uiVec4Get(dropdownOf, &DropdownComponent::arrowColor),
                "SetArrowColor", uiVec4Set(dropdownOf, &DropdownComponent::arrowColor, &mgr, "Dropdown.SetArrowColor"),
                "GetTextColor", uiVec4Get(dropdownOf, &DropdownComponent::textColor),
                "SetTextColor", uiVec4Set(dropdownOf, &DropdownComponent::textColor, &mgr, "Dropdown.SetTextColor"),
                "GetOptionCount", [](const LuaDropdown& d) {
                    return (int)dropdownOf(d)->options.size();
                },
                // 1-based y fuera de rango devuelve cadena vacia: un indice malo
                // no puede tumbar el script de un menu.
                "GetOption", [](const LuaDropdown& d, int i) {
                    DropdownComponent* c = dropdownOf(d);
                    if (i < 1 || i > (int)c->options.size()) return std::string();
                    return c->options[(size_t)(i - 1)];
                },
                "GetSelectedLabel", [](const LuaDropdown& d) {
                    return dropdownOf(d)->selectedLabel();
                },
                // Reemplaza la lista entera. Lo que no sea cadena se DESCARTA en
                // vez de tirar la tabla: perder el combo por una entrada mala
                // seria peor que perder esa entrada.
                "SetOptions", [](const LuaDropdown& d, sol::table t) {
                    DropdownComponent* c = dropdownOf(d);
                    c->options.clear();
                    for (size_t i = 1; i <= t.size(); i++)
                    {
                        sol::object o = t[i];
                        if (o.get_type() == sol::type::string)
                            c->options.push_back(o.as<std::string>());
                    }
                },
                "AddOption", [](const LuaDropdown& d, const std::string& s) {
                    dropdownOf(d)->options.push_back(s);
                },
                "ClearOptions", [](const LuaDropdown& d) {
                    dropdownOf(d)->options.clear();
                },
                "OnValueChanged", [&mgr](const LuaDropdown& d, sol::object fn) {
                    setUiValueCallback<int>(mgr, dropdownOf(d)->callbacks.ptr->onValueChanged,
                                            "Dropdown.OnValueChanged", fn);
                });

            // ── ScrollView ──────────────────────────────────────────────────
            // Sin referencia a un Scrollbar: enlazarlos es una linea de script
            // (barra:OnValueChanged -> vista:SetNormalizedPosition), y una
            // referencia entre componentes de la escena habria que serializarla
            // y mantenerla viva en el clone, el undo y el borrado.
            lua.new_usertype<LuaScrollView>("ScrollView",
                sol::no_constructor,
                "visible",           uiProp(scrollViewOf, &ScrollViewComponent::visible),
                "horizontal",        uiProp(scrollViewOf, &ScrollViewComponent::horizontal),
                "vertical",          uiProp(scrollViewOf, &ScrollViewComponent::vertical),
                "scrollSensitivity", uiFloatProp(scrollViewOf, &ScrollViewComponent::scrollSensitivity, &mgr, "ScrollView.scrollSensitivity"),
                "atlasPath",         uiProp(scrollViewOf, &ScrollViewComponent::atlasPath),
                "backgroundSprite",  uiProp(scrollViewOf, &ScrollViewComponent::backgroundSprite),
                "GetAnchorMin", uiVec2Get(scrollViewOf, &ScrollViewComponent::anchorMin),
                "SetAnchorMin", uiVec2Set(scrollViewOf, &ScrollViewComponent::anchorMin, &mgr, "ScrollView.SetAnchorMin"),
                "GetAnchorMax", uiVec2Get(scrollViewOf, &ScrollViewComponent::anchorMax),
                "SetAnchorMax", uiVec2Set(scrollViewOf, &ScrollViewComponent::anchorMax, &mgr, "ScrollView.SetAnchorMax"),
                "GetPivot",     uiVec2Get(scrollViewOf, &ScrollViewComponent::pivot),
                "SetPivot",     uiVec2Set(scrollViewOf, &ScrollViewComponent::pivot, &mgr, "ScrollView.SetPivot"),
                "GetPosition",  uiVec2Get(scrollViewOf, &ScrollViewComponent::position),
                "SetPosition",  uiVec2Set(scrollViewOf, &ScrollViewComponent::position, &mgr, "ScrollView.SetPosition"),
                "GetSize",      uiVec2Get(scrollViewOf, &ScrollViewComponent::size),
                "SetSize",      uiVec2Set(scrollViewOf, &ScrollViewComponent::size, &mgr, "ScrollView.SetSize"),
                "GetColor",     uiVec4Get(scrollViewOf, &ScrollViewComponent::color),
                "SetColor",     uiVec4Set(scrollViewOf, &ScrollViewComponent::color, &mgr, "ScrollView.SetColor"),
                "GetContentSize", uiVec2Get(scrollViewOf, &ScrollViewComponent::contentSize),
                "SetContentSize", uiVec2Set(scrollViewOf, &ScrollViewComponent::contentSize, &mgr, "ScrollView.SetContentSize"),
                "GetNormalizedPosition", uiVec2Get(scrollViewOf, &ScrollViewComponent::normalizedPosition),
                "SetNormalizedPosition", uiVec2Set(scrollViewOf, &ScrollViewComponent::normalizedPosition, &mgr, "ScrollView.SetNormalizedPosition"),
                // Cuanto se puede desplazar por eje, en pixeles. Un eje apagado
                // da 0 aunque el contenido sea mas grande.
                "GetScrollRange", [](const LuaScrollView& v) {
                    const glm::vec2 r = scrollViewOf(v)->scrollRange();
                    return std::make_tuple(r.x, r.y);
                },
                "GetContentOffset", [](const LuaScrollView& v) {
                    const glm::vec2 o = scrollViewOf(v)->contentOffset();
                    return std::make_tuple(o.x, o.y);
                },
                "OnValueChanged", [&mgr](const LuaScrollView& v, sol::object fn) {
                    setUiValueCallback<float, float>(
                        mgr, scrollViewOf(v)->callbacks.ptr->onValueChanged,
                        "ScrollView.OnValueChanged", fn);
                });
        }

        void registerEntity(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            lua.new_usertype<LuaEntity>("Entity",
                sol::no_constructor,
                "name", sol::property(
                    [](const LuaEntity& e) { return deref(e)->name; },
                    [](const LuaEntity& e, const std::string& n) { deref(e)->name = n; }),
                "IsValid", [](const LuaEntity& e) {
                    return e.go && e.mgr && e.mgr->isAlive(e.go);
                },
                "GetTransform", [](const LuaEntity& e) { deref(e); return LuaTransform{e}; },
                // UI: atajos con nombre para los cuatro componentes, además del
                // GetComponent("Button") de siempre. El getter devuelve nil si
                // el componente no está —comprobarlo es lo primero que hace un
                // script de UI, y un error de Lua no vale como respuesta—, y el
                // Add devuelve el wrapper, que ya está listo para encadenar.
                "GetCanvas", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasCanvas()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaCanvas{e});
                },
                "GetButton", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasButton()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaButton{e});
                },
                "GetText", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasText()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaText{e});
                },
                "GetProgressBar", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasProgressBar()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaProgressBar{e});
                },
                "GetLayout", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasLayout()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaLayout{e});
                },
                "GetInputField", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasInputField()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaInputField{e});
                },
                "GetDropdown", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasDropdown()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaDropdown{e});
                },
                "GetScrollView", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasScrollView()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaScrollView{e});
                },
                "GetSlider", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasSlider()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaSlider{e});
                },
                "GetCheckbox", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasCheckbox()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaCheckbox{e});
                },
                "GetToggle", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasToggle()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaToggle{e});
                },
                "GetScrollbar", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasScrollbar()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaScrollbar{e});
                },
                "GetPanel", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasPanel()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaPanel{e});
                },
                "GetImage", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasImage()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaImage{e});
                },
                "AddCanvas", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasCanvas()) go->setCanvas(std::make_shared<CanvasComponent>());
                    return LuaCanvas{e};
                },
                "AddButton", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasButton()) go->setButton(std::make_shared<ButtonComponent>());
                    return LuaButton{e};
                },
                "AddText", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasText()) go->setText(std::make_shared<TextComponent>());
                    return LuaText{e};
                },
                "AddProgressBar", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasProgressBar()) go->setProgressBar(std::make_shared<ProgressBarComponent>());
                    return LuaProgressBar{e};
                },
                "AddLayout", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasLayout()) go->setLayout(std::make_shared<LayoutComponent>());
                    return LuaLayout{e};
                },
                "AddInputField", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasInputField()) go->setInputField(std::make_shared<InputFieldComponent>());
                    return LuaInputField{e};
                },
                "AddDropdown", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasDropdown()) go->setDropdown(std::make_shared<DropdownComponent>());
                    return LuaDropdown{e};
                },
                "AddScrollView", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasScrollView()) go->setScrollView(std::make_shared<ScrollViewComponent>());
                    return LuaScrollView{e};
                },
                "AddSlider", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasSlider()) go->setSlider(std::make_shared<SliderComponent>());
                    return LuaSlider{e};
                },
                "AddCheckbox", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasCheckbox()) go->setCheckbox(std::make_shared<CheckboxComponent>());
                    return LuaCheckbox{e};
                },
                "AddToggle", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasToggle()) go->setToggle(std::make_shared<ToggleComponent>());
                    return LuaToggle{e};
                },
                "AddScrollbar", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasScrollbar()) go->setScrollbar(std::make_shared<ScrollbarComponent>());
                    return LuaScrollbar{e};
                },
                "AddPanel", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasPanel()) go->setPanel(std::make_shared<PanelComponent>());
                    return LuaPanel{e};
                },
                "AddImage", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasImage()) go->setImage(std::make_shared<ImageComponent>());
                    return LuaImage{e};
                },
                "RemoveCanvas",      [](const LuaEntity& e) { deref(e)->setCanvas(nullptr); },
                "RemoveButton",      [](const LuaEntity& e) { deref(e)->setButton(nullptr); },
                "RemoveText",        [](const LuaEntity& e) { deref(e)->setText(nullptr); },
                "RemoveProgressBar", [](const LuaEntity& e) { deref(e)->setProgressBar(nullptr); },
                "RemoveLayout",      [](const LuaEntity& e) { deref(e)->setLayout(nullptr); },
                "RemoveInputField",  [](const LuaEntity& e) { deref(e)->setInputField(nullptr); },
                "RemoveDropdown",    [](const LuaEntity& e) { deref(e)->setDropdown(nullptr); },
                "RemoveScrollView",  [](const LuaEntity& e) { deref(e)->setScrollView(nullptr); },
                "RemoveSlider",      [](const LuaEntity& e) { deref(e)->setSlider(nullptr); },
                "RemoveCheckbox",    [](const LuaEntity& e) { deref(e)->setCheckbox(nullptr); },
                "RemoveToggle",      [](const LuaEntity& e) { deref(e)->setToggle(nullptr); },
                "RemoveScrollbar",   [](const LuaEntity& e) { deref(e)->setScrollbar(nullptr); },
                "RemovePanel",       [](const LuaEntity& e) { deref(e)->setPanel(nullptr); },
                "RemoveImage",       [](const LuaEntity& e) { deref(e)->setImage(nullptr); },
                "GetParent", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->parent || !go->parent->parent) return sol::nil; // root no se expone
                    return sol::make_object(e.mgr->lua(), LuaEntity{go->parent, e.mgr});
                },
                "GetChildren", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    sol::table result = e.mgr->lua().create_table();
                    int i = 1;
                    for (auto& c : go->children)
                        result[i++] = LuaEntity{c.get(), e.mgr};
                    return result;
                },
                "GetComponent", [](const LuaEntity& e, const std::string& name) -> sol::object {
                    GameObject* go = deref(e);
                    sol::state_view lua(e.mgr->lua());
                    if (name == "BoxCollider"     && go->hasBoxCollider())     return sol::make_object(lua, LuaBoxCollider{e});
                    if (name == "SphereCollider"  && go->hasSphereCollider())  return sol::make_object(lua, LuaSphereCollider{e});
                    if (name == "CapsuleCollider" && go->hasCapsuleCollider()) return sol::make_object(lua, LuaCapsuleCollider{e});
                    if (name == "PlaneCollider"   && go->hasPlaneCollider())   return sol::make_object(lua, LuaPlaneCollider{e});
                    if (name == "AudioClip"       && go->hasAudioClip())       return sol::make_object(lua, LuaAudioClip{e});
                    if (name == "Rigidbody"       && go->hasRigidbody())       return sol::make_object(lua, LuaRigidbody{e});
                    if (name == "Animator"        && go->hasAnimator())        return sol::make_object(lua, LuaAnimator{e});
                    if (name == "Canvas"          && go->hasCanvas())          return sol::make_object(lua, LuaCanvas{e});
                    if (name == "Button"          && go->hasButton())          return sol::make_object(lua, LuaButton{e});
                    if (name == "Text"            && go->hasText())            return sol::make_object(lua, LuaText{e});
                    if (name == "ProgressBar"     && go->hasProgressBar())     return sol::make_object(lua, LuaProgressBar{e});
                    if (name == "Layout"          && go->hasLayout())          return sol::make_object(lua, LuaLayout{e});
                    if (name == "Panel"           && go->hasPanel())           return sol::make_object(lua, LuaPanel{e});
                    if (name == "Image"           && go->hasImage())           return sol::make_object(lua, LuaImage{e});
                    if (name == "Slider"          && go->hasSlider())          return sol::make_object(lua, LuaSlider{e});
                    if (name == "Checkbox"        && go->hasCheckbox())        return sol::make_object(lua, LuaCheckbox{e});
                    if (name == "Toggle"          && go->hasToggle())          return sol::make_object(lua, LuaToggle{e});
                    if (name == "Scrollbar"       && go->hasScrollbar())       return sol::make_object(lua, LuaScrollbar{e});
                    if (name == "InputField"      && go->hasInputField())      return sol::make_object(lua, LuaInputField{e});
                    if (name == "Dropdown"        && go->hasDropdown())        return sol::make_object(lua, LuaDropdown{e});
                    if (name == "ScrollView"      && go->hasScrollView())      return sol::make_object(lua, LuaScrollView{e});
                    if (name.rfind("Script:", 0) == 0)
                    {
                        const std::string scriptName = name.substr(7);
                        for (auto& s : go->getScripts())
                            if (s->scriptName == scriptName && s->instance.valid())
                                return s->instance;
                    }
                    return sol::nil;
                },
                "AddComponent", [](const LuaEntity& e, const std::string& name,
                                   sol::optional<std::string> arg) -> sol::object {
                    GameObject* go = deref(e);
                    auto* mgr = e.mgr;
                    sol::state_view lua(mgr->lua());
                    // Mismos defaults que EditorUI::drawAddComponentButton;
                    // colliders mutuamente excluyentes, misma regla que la UI.
                    if (name == "BoxCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setBoxCollider(mgr->physics()->createBoxColliderComponent(
                            glm::vec3(25.0f), glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaBoxCollider{e});
                    }
                    if (name == "SphereCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setSphereCollider(mgr->physics()->createSphereColliderComponent(
                            25.0f, glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaSphereCollider{e});
                    }
                    if (name == "CapsuleCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setCapsuleCollider(mgr->physics()->createCapsuleColliderComponent(
                            15.0f, 25.0f, glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaCapsuleCollider{e});
                    }
                    if (name == "PlaneCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setPlaneCollider(mgr->physics()->createPlaneColliderComponent(
                            glm::vec3(0.0f), go->worldTransform));
                        return sol::make_object(lua, LuaPlaneCollider{e});
                    }
                    if (name == "AudioClip" && !go->hasAudioClip() && mgr->audioManager() && arg)
                    {
                        // Misma whitelist que el inspector y que la carga de
                        // escena: sin ella, un path con cualquier extensión
                        // creaba el componente igual y el fallo solo se notaba
                        // como silencio (FMOD carga en diferido).
                        std::string ext = std::filesystem::path(*arg).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char ch) { return (char)std::tolower(ch); });
                        if (!isSupportedAudioExtension(ext))
                        {
                            mgr->log("[Lua][WARN] AddComponent(\"AudioClip\"): formato no soportado '" +
                                      ext + "' (usa .wav, .mp3, .ogg o .flac)");
                            return sol::nil;
                        }
                        auto clip = mgr->audioManager()->createAudioClipComponent(*arg, false, false);
                        if (clip) { go->setAudioClip(std::move(clip)); return sol::make_object(lua, LuaAudioClip{e}); }
                    }
                    // Rigidbody: necesita un collider que aporte la forma y que no
                    // exista ya. attachRigidbody promociona el actor a dynamic.
                    if (name == "Rigidbody" && go->hasAnyCollider() && !go->hasRigidbody() && mgr->physics())
                    {
                        auto rb = std::make_shared<Rigidbody>();
                        go->setRigidbody(rb);
                        if (auto col = go->anyCollider()) mgr->physics()->attachRigidbody(col, rb);
                        return sol::make_object(lua, LuaRigidbody{e});
                    }
                    // UI: sin dependencias que resolver (son SOLO datos) y sin
                    // gate de exclusión — los cuatro conviven en el mismo
                    // GameObject, igual que en el panel Properties. Pedir uno
                    // que ya está devuelve el que hay, no lo reemplaza: el
                    // Add del editor tampoco pisa lo que ya existe.
                    if (name == "Canvas")
                    {
                        if (!go->hasCanvas()) go->setCanvas(std::make_shared<CanvasComponent>());
                        return sol::make_object(lua, LuaCanvas{e});
                    }
                    if (name == "Button")
                    {
                        if (!go->hasButton()) go->setButton(std::make_shared<ButtonComponent>());
                        return sol::make_object(lua, LuaButton{e});
                    }
                    if (name == "Text")
                    {
                        if (!go->hasText()) go->setText(std::make_shared<TextComponent>());
                        return sol::make_object(lua, LuaText{e});
                    }
                    if (name == "ProgressBar")
                    {
                        if (!go->hasProgressBar()) go->setProgressBar(std::make_shared<ProgressBarComponent>());
                        return sol::make_object(lua, LuaProgressBar{e});
                    }
                    if (name == "Layout")
                    {
                        if (!go->hasLayout()) go->setLayout(std::make_shared<LayoutComponent>());
                        return sol::make_object(lua, LuaLayout{e});
                    }
                    if (name == "Panel")
                    {
                        if (!go->hasPanel()) go->setPanel(std::make_shared<PanelComponent>());
                        return sol::make_object(lua, LuaPanel{e});
                    }
                    if (name == "Image")
                    {
                        if (!go->hasImage()) go->setImage(std::make_shared<ImageComponent>());
                        return sol::make_object(lua, LuaImage{e});
                    }
                    if (name == "Slider")
                    {
                        if (!go->hasSlider()) go->setSlider(std::make_shared<SliderComponent>());
                        return sol::make_object(lua, LuaSlider{e});
                    }
                    if (name == "Checkbox")
                    {
                        if (!go->hasCheckbox()) go->setCheckbox(std::make_shared<CheckboxComponent>());
                        return sol::make_object(lua, LuaCheckbox{e});
                    }
                    if (name == "Toggle")
                    {
                        if (!go->hasToggle()) go->setToggle(std::make_shared<ToggleComponent>());
                        return sol::make_object(lua, LuaToggle{e});
                    }
                    if (name == "Scrollbar")
                    {
                        if (!go->hasScrollbar()) go->setScrollbar(std::make_shared<ScrollbarComponent>());
                        return sol::make_object(lua, LuaScrollbar{e});
                    }
                    if (name == "InputField")
                    {
                        if (!go->hasInputField()) go->setInputField(std::make_shared<InputFieldComponent>());
                        return sol::make_object(lua, LuaInputField{e});
                    }
                    if (name == "Dropdown")
                    {
                        if (!go->hasDropdown()) go->setDropdown(std::make_shared<DropdownComponent>());
                        return sol::make_object(lua, LuaDropdown{e});
                    }
                    if (name == "ScrollView")
                    {
                        if (!go->hasScrollView()) go->setScrollView(std::make_shared<ScrollViewComponent>());
                        return sol::make_object(lua, LuaScrollView{e});
                    }
                    if (name.rfind("Script:", 0) == 0)
                    {
                        auto comp = std::make_unique<DonTopo::ScriptComponent>(name.substr(7), go);
                        go->addScript(std::move(comp));
                        // La instanciación + Awake/Start del comp nuevo la
                        // hace el lifecycle en el siguiente update (started
                        // == false lo delata). Task 8.
                        return sol::make_object(lua, true);
                    }
                    return sol::nil;
                },
                "RemoveComponent", [](const LuaEntity& e, const std::string& name) {
                    GameObject* go = deref(e);
                    if (name == "BoxCollider")     go->setBoxCollider(nullptr);
                    else if (name == "SphereCollider")  go->setSphereCollider(nullptr);
                    else if (name == "CapsuleCollider") go->setCapsuleCollider(nullptr);
                    else if (name == "PlaneCollider")   go->setPlaneCollider(nullptr);
                    else if (name == "AudioClip")       go->setAudioClip(nullptr);
                    // Quitar un componente de UI se lleva por delante sus
                    // callbacks: el runtime muere con el componente y el handler
                    // del nodo, que solo tiene un weak_ptr, deja de disparar.
                    else if (name == "Canvas")          go->setCanvas(nullptr);
                    else if (name == "Button")          go->setButton(nullptr);
                    else if (name == "Text")            go->setText(nullptr);
                    else if (name == "ProgressBar")     go->setProgressBar(nullptr);
                    else if (name == "Layout")          go->setLayout(nullptr);
                    else if (name == "Panel")           go->setPanel(nullptr);
                    else if (name == "Image")           go->setImage(nullptr);
                    else if (name == "Slider")          go->setSlider(nullptr);
                    else if (name == "Checkbox")        go->setCheckbox(nullptr);
                    else if (name == "Toggle")          go->setToggle(nullptr);
                    else if (name == "Scrollbar")       go->setScrollbar(nullptr);
                    else if (name == "InputField")      go->setInputField(nullptr);
                    else if (name == "Dropdown")        go->setDropdown(nullptr);
                    else if (name == "ScrollView")      go->setScrollView(nullptr);
                    else if (name == "Rigidbody")
                    {
                        // Reconstruye el actor como static antes de soltar el Rigidbody.
                        if (auto col = go->anyCollider(); col && e.mgr && e.mgr->physics())
                            e.mgr->physics()->detachRigidbody(col);
                        go->setRigidbody(nullptr);
                    }
                    else if (name.rfind("Script:", 0) == 0)
                    {
                        // Diferido: el lifecycle lo procesa al final del frame
                        // (quitar en mitad de la iteración de Update rompería
                        // el recorrido). Task 8.
                        const std::string scriptName = name.substr(7);
                        for (auto& s : go->getScripts())
                            if (s->scriptName == scriptName) s->pendingRemove = true;
                    }
                });
        }

#ifdef DT_PHYSX_ENABLED
        // El GameObject detrás de un actor de PhysX: userData del actor =
        // Collider* (lo pone PhysicsManager al crear el collider) y el owner del
        // collider = GameObject* (lo pone Scene al deserializar / el editor al
        // añadirlo). Mismo camino que usa TriggerDispatcher para los callbacks
        // de trigger. nullptr si el actor no cuelga de ningún GameObject.
        GameObject* actorOwner(const physx::PxRigidActor* actor)
        {
            if (!actor) return nullptr;
            auto* col = static_cast<DonTopo::Collider*>(actor->userData);
            return col ? static_cast<GameObject*>(col->getOwner()) : nullptr;
        }

        // Prefiltro de la consulta. Collider::applyTriggerFlag solo apaga
        // eSIMULATION_SHAPE: la shape de un trigger conserva eSCENE_QUERY_SHAPE,
        // así que sin este filtro un trigger bloquearía el rayo. Aquí se
        // descarta también el actor del GameObject a ignorar.
        class RaycastFilter : public physx::PxQueryFilterCallback
        {
        public:
            RaycastFilter(bool hitTriggers, GameObject* ignore)
                : m_hitTriggers(hitTriggers), m_ignore(ignore) {}

            physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData&,
                                                  const physx::PxShape* shape,
                                                  const physx::PxRigidActor* actor,
                                                  physx::PxHitFlags&) override
            {
                if (!m_hitTriggers && shape &&
                    (shape->getFlags() & physx::PxShapeFlag::eTRIGGER_SHAPE))
                    return physx::PxQueryHitType::eNONE;
                if (m_ignore && actorOwner(actor) == m_ignore)
                    return physx::PxQueryHitType::eNONE;
                return physx::PxQueryHitType::eBLOCK;
            }

            physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData&,
                                                   const physx::PxQueryHit&,
                                                   const physx::PxShape*,
                                                   const physx::PxRigidActor*) override
            {
                return physx::PxQueryHitType::eBLOCK;
            }

        private:
            bool        m_hitTriggers;
            GameObject* m_ignore;
        };

        // Argumentos ya validados de Physics.Raycast / Physics.RaycastHit.
        struct RaycastArgs
        {
            glm::vec3   origin{ 0.0f };
            glm::vec3   dir{ 0.0f, 0.0f, 1.0f };
            float       maxDistance  = 1000.0f;
            bool        hitTriggers  = false;
            bool        queryStatic  = true;
            bool        queryDynamic = true;
            GameObject* ignore       = nullptr;
        };

        // Warn/argAt/given: los comparten todos los parseos de consulta.
        void queryWarn(ScriptManager& mgr, const char* fn, const std::string& m)
        {
            mgr.log(std::string("[Lua][WARN] Physics.") + fn + ": " + m);
        }
        sol::object queryArgAt(sol::variadic_args va, std::size_t i)
        {
            return i < va.size() ? va[i].get<sol::object>() : sol::object();
        }
        bool queryGiven(const sol::object& o)
        {
            return o.valid() && o.get_type() != sol::type::lua_nil;
        }

        // Tabla 'options' común a TODAS las consultas (raycast, sweep y
        // overlap): { hitTriggers, static, dynamic, ignore }. Ausente o nil =>
        // se quedan los defaults de RaycastArgs. Las consultas sin rayo
        // (overlaps) sólo usan estos cuatro campos de la struct.
        bool parseQueryOptions(ScriptManager& mgr, const char* fn,
                               const sol::object& oOpts, RaycastArgs& out)
        {
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            if (!queryGiven(oOpts)) return true;
            if (oOpts.get_type() != sol::type::table)
            {
                warn("options tiene que ser una tabla");
                return false;
            }
            sol::table opts = oOpts.as<sol::table>();

            auto readBool = [&](const char* key, bool& dst) {
                const sol::object v = opts[key];
                if (!queryGiven(v)) return true;
                if (v.get_type() != sol::type::boolean)
                {
                    warn(std::string(key) + " tiene que ser booleano");
                    return false;
                }
                dst = v.as<bool>();
                return true;
            };
            if (!readBool("hitTriggers", out.hitTriggers)) return false;
            if (!readBool("static",      out.queryStatic)) return false;
            if (!readBool("dynamic",     out.queryDynamic)) return false;

            const sol::object oIgnore = opts["ignore"];
            if (queryGiven(oIgnore))
            {
                if (!oIgnore.is<LuaEntity>())
                {
                    warn("ignore tiene que ser una Entity");
                    return false;
                }
                const LuaEntity e = oIgnore.as<LuaEntity>();
                if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
                {
                    warn("ignore apunta a una Entity destruida");
                    return false;
                }
                out.ignore = e.go;
            }
            return true;
        }

        // Lee los argumentos a mano (sol::variadic_args, no parámetros tipados)
        // porque un tipo equivocado tiene que devolver nil y un aviso, no la
        // excepción de conversión de sol2 que tumbaría el script.
        bool parseRaycastArgs(ScriptManager& mgr, const char* fn,
                              sol::variadic_args va, RaycastArgs& out)
        {
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };
            auto argAt = [&va](std::size_t i) { return queryArgAt(va, i); };
            auto given = [](const sol::object& o) { return queryGiven(o); };

            const sol::object oOrigin = argAt(0);
            const sol::object oDir    = argAt(1);
            if (!oOrigin.is<glm::vec3>() || !oDir.is<glm::vec3>())
            {
                warn("origin y direction tienen que ser Vec3");
                return false;
            }
            out.origin = oOrigin.as<glm::vec3>();
            out.dir    = oDir.as<glm::vec3>();

            const sol::object oMax = argAt(2);
            if (given(oMax))
            {
                if (oMax.get_type() != sol::type::number)
                {
                    warn("maxDistance tiene que ser un numero");
                    return false;
                }
                // Ausente o <= 0 -> se queda el default de 1000.
                const float m = oMax.as<float>();
                if (m > 0.0f) out.maxDistance = m;
            }

            return parseQueryOptions(mgr, fn, argAt(3), out);
        }

        // Lanza la consulta. false = sin impacto, sin PhysicsManager (fuera de
        // Play), dirección degenerada o filtro que no deja ningún actor: en
        // todos esos casos no se toca PhysX y hit se queda sin escribir.
        bool doRaycast(ScriptManager& mgr, const RaycastArgs& a, physx::PxRaycastBuffer& hit)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!std::isfinite(a.origin.x) || !std::isfinite(a.origin.y) || !std::isfinite(a.origin.z))
                return false;

            // PhysX exige dirección unitaria (con una sin normalizar la
            // distancia sale escalada); longitud 0 o NaN -> nada que trazar.
            const float len = glm::length(a.dir);
            if (!std::isfinite(len) || len <= 0.0f) return false;
            const glm::vec3 dir = a.dir / len;

            physx::PxQueryFilterData filterData;
            filterData.flags = physx::PxQueryFlag::ePREFILTER;
            if (a.queryStatic)  filterData.flags |= physx::PxQueryFlag::eSTATIC;
            if (a.queryDynamic) filterData.flags |= physx::PxQueryFlag::eDYNAMIC;

            RaycastFilter filter(a.hitTriggers, a.ignore);
            return pm->raycast(physx::PxVec3(a.origin.x, a.origin.y, a.origin.z),
                               physx::PxVec3(dir.x, dir.y, dir.z),
                               a.maxDistance, hit, filterData, &filter);
        }

        // Techo de impactos de Physics.RaycastAll. PhysX trunca en silencio al
        // llenarse el buffer (ver PxQueryReport.h: "Overflow does not trigger
        // warnings or errors"), así que el binding lo detecta y avisa.
        constexpr physx::PxU32 kRaycastAllMaxHits = 64;

        // Misma consulta que doRaycast pero multi-hit; los touches salen
        // ordenados por distancia (lo hace PhysicsManager::raycastAll).
        bool doRaycastAll(ScriptManager& mgr, const RaycastArgs& a,
                          physx::PxRaycastBufferN<kRaycastAllMaxHits>& hits)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!std::isfinite(a.origin.x) || !std::isfinite(a.origin.y) || !std::isfinite(a.origin.z))
                return false;

            const float len = glm::length(a.dir);
            if (!std::isfinite(len) || len <= 0.0f) return false;
            const glm::vec3 dir = a.dir / len;

            physx::PxQueryFilterData filterData;
            filterData.flags = physx::PxQueryFlag::ePREFILTER;
            if (a.queryStatic)  filterData.flags |= physx::PxQueryFlag::eSTATIC;
            if (a.queryDynamic) filterData.flags |= physx::PxQueryFlag::eDYNAMIC;

            RaycastFilter filter(a.hitTriggers, a.ignore);
            return pm->raycastAll(physx::PxVec3(a.origin.x, a.origin.y, a.origin.z),
                                  physx::PxVec3(dir.x, dir.y, dir.z),
                                  a.maxDistance, hits, filterData, &filter);
        }

        // Forma de tabla de un impacto: { entity, point, normal, distance }.
        // La comparten Physics.Raycast (un solo hit) y Physics.RaycastAll (uno
        // por elemento del array), así no pueden divergir. entity se omite si el
        // actor no cuelga de ningún GameObject.
        sol::table makeHitTable(ScriptManager& mgr, const physx::PxRaycastHit& hit)
        {
            sol::table t = mgr.lua().create_table();
            if (GameObject* go = actorOwner(hit.actor))
                t["entity"] = LuaEntity{ go, &mgr };
            t["point"]    = glm::vec3(hit.position.x, hit.position.y, hit.position.z);
            t["normal"]   = glm::vec3(hit.normal.x, hit.normal.y, hit.normal.z);
            t["distance"] = hit.distance;
            return t;
        }

        // Filtros de la consulta comunes a sweeps y overlaps: los mismos que
        // arma doRaycast (ePREFILTER + eSTATIC/eDYNAMIC según options).
        physx::PxQueryFilterData queryFilterData(const RaycastArgs& a)
        {
            physx::PxQueryFilterData filterData;
            filterData.flags = physx::PxQueryFlag::ePREFILTER;
            if (a.queryStatic)  filterData.flags |= physx::PxQueryFlag::eSTATIC;
            if (a.queryDynamic) filterData.flags |= physx::PxQueryFlag::eDYNAMIC;
            return filterData;
        }

        bool finite3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Physics.SphereCast(origin, direction, radius, maxDistance [, options]).
        // El radio va ANTES de maxDistance, así que el parseo del rayo no se
        // puede reutilizar tal cual (los índices bailan); lo que sí se reutiliza
        // es la tabla options.
        bool parseSphereCastArgs(ScriptManager& mgr, sol::variadic_args va,
                                 RaycastArgs& out, float& radius)
        {
            const char* fn = "SphereCast";
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            const sol::object oOrigin = queryArgAt(va, 0);
            const sol::object oDir    = queryArgAt(va, 1);
            if (!oOrigin.is<glm::vec3>() || !oDir.is<glm::vec3>())
            {
                warn("origin y direction tienen que ser Vec3");
                return false;
            }
            out.origin = oOrigin.as<glm::vec3>();
            out.dir    = oDir.as<glm::vec3>();

            // queryGiven ANTES de get_type: sobre un sol::object sin lua_State
            // (argumento que no se pasó) get_type deref un puntero nulo.
            const sol::object oRadius = queryArgAt(va, 2);
            if (!queryGiven(oRadius) || oRadius.get_type() != sol::type::number)
            {
                warn("radius tiene que ser un numero");
                return false;
            }
            // PxSphereGeometry con radio <= 0 (o NaN) es geometría inválida:
            // PhysX se queja por su canal de errores y la consulta no vale.
            const float r = oRadius.as<float>();
            if (!std::isfinite(r) || r <= 0.0f)
            {
                warn("radius tiene que ser mayor que 0");
                return false;
            }
            radius = r;

            const sol::object oMax = queryArgAt(va, 3);
            if (queryGiven(oMax))
            {
                if (oMax.get_type() != sol::type::number)
                {
                    warn("maxDistance tiene que ser un numero");
                    return false;
                }
                const float m = oMax.as<float>();
                if (m > 0.0f) out.maxDistance = m;
            }

            return parseQueryOptions(mgr, fn, queryArgAt(va, 4), out);
        }

        // Barrido de la esfera. Mismas salidas en falso que doRaycast (sin
        // PhysicsManager, filtro que no deja actores, origen/dirección
        // degenerados) y misma normalización de la dirección.
        bool doSphereCast(ScriptManager& mgr, const RaycastArgs& a, float radius,
                          physx::PxSweepBuffer& hit)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!finite3(a.origin)) return false;

            const float len = glm::length(a.dir);
            if (!std::isfinite(len) || len <= 0.0f) return false;
            const glm::vec3 dir = a.dir / len;

            physx::PxQueryFilterData filterData = queryFilterData(a);
            RaycastFilter filter(a.hitTriggers, a.ignore);
            return pm->sphereCast(physx::PxVec3(a.origin.x, a.origin.y, a.origin.z),
                                  physx::PxVec3(dir.x, dir.y, dir.z),
                                  radius, a.maxDistance, hit, filterData, &filter);
        }

        // La tabla de impacto de un sweep tiene los mismos campos que la de un
        // raycast, pero PxSweepHit y PxRaycastHit son tipos distintos: se
        // rellena aparte (mismos nombres y mismo orden a propósito).
        sol::table makeSweepHitTable(ScriptManager& mgr, const physx::PxSweepHit& hit)
        {
            sol::table t = mgr.lua().create_table();
            if (GameObject* go = actorOwner(hit.actor))
                t["entity"] = LuaEntity{ go, &mgr };
            t["point"]    = glm::vec3(hit.position.x, hit.position.y, hit.position.z);
            t["normal"]   = glm::vec3(hit.normal.x, hit.normal.y, hit.normal.z);
            t["distance"] = hit.distance;
            return t;
        }

        // Techo de solapes de Physics.OverlapSphere / OverlapBox. Igual que
        // kRaycastAllMaxHits: PhysX trunca en silencio al llenarse el buffer
        // (PxQueryReport.h, "Overflow does not trigger warnings or errors"), así
        // que el binding lo detecta con getNbTouches() == getMaxNbTouches().
        constexpr physx::PxU32 kOverlapMaxHits = 64;

        // Physics.OverlapSphere(center, radius [, options]).
        bool parseOverlapSphereArgs(ScriptManager& mgr, sol::variadic_args va,
                                    glm::vec3& center, float& radius, RaycastArgs& out)
        {
            const char* fn = "OverlapSphere";
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            const sol::object oCenter = queryArgAt(va, 0);
            if (!oCenter.is<glm::vec3>())
            {
                warn("center tiene que ser un Vec3");
                return false;
            }
            center = oCenter.as<glm::vec3>();

            const sol::object oRadius = queryArgAt(va, 1);
            if (!queryGiven(oRadius) || oRadius.get_type() != sol::type::number)
            {
                warn("radius tiene que ser un numero");
                return false;
            }
            const float r = oRadius.as<float>();
            if (!std::isfinite(r) || r <= 0.0f)
            {
                warn("radius tiene que ser mayor que 0");
                return false;
            }
            radius = r;

            return parseQueryOptions(mgr, fn, queryArgAt(va, 2), out);
        }

        // Physics.OverlapBox(center, halfExtents [, rotation] [, options]).
        // rotation son grados de Euler en un Vec3, misma convención que
        // transform.rotation (eulerAngleXYZ). Como es opcional y va delante de
        // options, el tercer argumento se desambigua por tipo: Vec3 => rotación,
        // tabla => options.
        bool parseOverlapBoxArgs(ScriptManager& mgr, sol::variadic_args va,
                                 glm::vec3& center, glm::vec3& halfExtents,
                                 glm::vec3& eulerDeg, RaycastArgs& out)
        {
            const char* fn = "OverlapBox";
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            const sol::object oCenter = queryArgAt(va, 0);
            const sol::object oHalf   = queryArgAt(va, 1);
            if (!oCenter.is<glm::vec3>() || !oHalf.is<glm::vec3>())
            {
                warn("center y halfExtents tienen que ser Vec3");
                return false;
            }
            center      = oCenter.as<glm::vec3>();
            halfExtents = oHalf.as<glm::vec3>();
            if (!finite3(halfExtents) ||
                halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
            {
                warn("halfExtents tiene que tener las tres componentes mayores que 0");
                return false;
            }

            eulerDeg = glm::vec3(0.0f);
            const sol::object oThird = queryArgAt(va, 2);
            std::size_t optsIndex = 2;
            if (queryGiven(oThird) && oThird.is<glm::vec3>())
            {
                eulerDeg = oThird.as<glm::vec3>();
                if (!finite3(eulerDeg))
                {
                    warn("rotation tiene que ser finita");
                    return false;
                }
                optsIndex = 3;
            }

            return parseQueryOptions(mgr, fn, queryArgAt(va, optsIndex), out);
        }

        bool doOverlapSphere(ScriptManager& mgr, const glm::vec3& center, float radius,
                             const RaycastArgs& a,
                             physx::PxOverlapBufferN<kOverlapMaxHits>& hits)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!finite3(center)) return false;

            physx::PxQueryFilterData filterData = queryFilterData(a);
            RaycastFilter filter(a.hitTriggers, a.ignore);
            return pm->overlapSphere(physx::PxVec3(center.x, center.y, center.z),
                                     radius, hits, filterData, &filter);
        }

        bool doOverlapBox(ScriptManager& mgr, const glm::vec3& center,
                          const glm::vec3& halfExtents, const glm::vec3& eulerDeg,
                          const RaycastArgs& a,
                          physx::PxOverlapBufferN<kOverlapMaxHits>& hits)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!finite3(center)) return false;

            // Misma composición que recomposeLocal (eulerAngleXYZ), para que
            // pasarle transform.rotation de una entidad oriente la caja igual
            // que está orientada esa entidad.
            const glm::quat q(glm::quat_cast(glm::eulerAngleXYZ(glm::radians(eulerDeg.x),
                                                                glm::radians(eulerDeg.y),
                                                                glm::radians(eulerDeg.z))));

            physx::PxQueryFilterData filterData = queryFilterData(a);
            RaycastFilter filter(a.hitTriggers, a.ignore);
            return pm->overlapBox(physx::PxVec3(center.x, center.y, center.z),
                                  physx::PxVec3(halfExtents.x, halfExtents.y, halfExtents.z),
                                  physx::PxQuat(q.x, q.y, q.z, q.w),
                                  hits, filterData, &filter);
        }

        // Array 1-indexado de Entity con los solapes. Un overlap no tiene punto,
        // normal ni distancia, así que no hay tabla de hit que devolver: sólo el
        // GameObject. Los actores sin GameObject detrás se omiten (no hay nada
        // que entregar a Lua) y un mismo GameObject sale UNA vez aunque solapen
        // varias de sus shapes.
        sol::table makeOverlapArray(ScriptManager& mgr,
                                    const physx::PxOverlapBufferN<kOverlapMaxHits>& hits)
        {
            sol::table out = mgr.lua().create_table();
            std::vector<GameObject*> seen;
            int n = 0;
            for (physx::PxU32 i = 0; i < hits.getNbTouches(); ++i)
            {
                GameObject* go = actorOwner(hits.getTouch(i).actor);
                if (!go) continue;
                if (std::find(seen.begin(), seen.end(), go) != seen.end()) continue;
                seen.push_back(go);
                out[++n] = LuaEntity{ go, &mgr };
            }
            return out;
        }

        // Aviso compartido por los dos overlaps al llenarse el buffer.
        void warnOverlapOverflow(ScriptManager& mgr, const char* fn,
                                 const physx::PxOverlapBufferN<kOverlapMaxHits>& hits)
        {
            if (hits.getNbTouches() >= hits.getMaxNbTouches())
                mgr.log(std::string("[Lua][WARN] Physics.") + fn + ": limite de " +
                        std::to_string(kOverlapMaxHits) +
                        " solapes alcanzado, hay resultados descartados");
        }
#endif

        void registerPhysics(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            // Tabla global Audio: los mandos de volumen que el jugador espera
            // en un menú de opciones. Existían en AudioManager desde el
            // principio y no los llamaba NADIE — ni la UI ni los scripts—, así
            // que un juego exportado no tenía forma de bajar el volumen.
            //
            // Se pasan por nombre de bus, igual que AudioClip:SetBus, para no
            // tener dos vocabularios distintos para lo mismo.
            sol::table audio = lua.create_named_table("Audio");

            audio["SetBusVolume"] = [&mgr](const std::string& name, float v) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                AudioBus bus;
                if (!audioBusFromStr(name, bus))
                {
                    mgr.log("[Lua][WARN] Audio.SetBusVolume: bus desconocido '" + name +
                             "' (usa 'master', 'music' o 'sfx')");
                    return;
                }
                // Mismo trato que los setters del clip: un NaN aquí dejaría el
                // volumen del grupo inutilizable para el resto de la partida, y
                // no hay ningún .scene donde se note para depurarlo después.
                if (!ensureFinite(mgr, "Audio.SetBusVolume", v)) return;
                am->setBusVolume(bus, std::clamp(v, 0.0f, 1.0f));
            };

            audio["GetBusVolume"] = [&mgr](const std::string& name) -> float {
                AudioManager* am = mgr.audioManager();
                if (!am) return 1.0f;
                AudioBus bus;
                if (!audioBusFromStr(name, bus))
                {
                    mgr.log("[Lua][WARN] Audio.GetBusVolume: bus desconocido '" + name + "'");
                    return 1.0f;
                }
                return am->getBusVolume(bus);
            };

            sol::table physics = lua.create_named_table("Physics");

            // Physics.Raycast(origin, direction, maxDistance, options) -> tabla
            // { entity, point, normal, distance } o nil. entity es nil si el
            // actor impactado no cuelga de ningún GameObject; el resto de
            // campos vienen siempre.
            physics["Raycast"] = [&mgr](sol::variadic_args va) -> sol::object {
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                if (!parseRaycastArgs(mgr, "Raycast", va, args)) return sol::nil;

                physx::PxRaycastBuffer hit;
                if (!doRaycast(mgr, args, hit)) return sol::nil;

                return sol::make_object(mgr.lua(), makeHitTable(mgr, hit.block));
#else
                (void)va;
                return sol::nil;
#endif
            };

            // Physics.RaycastAll(origin, direction, maxDistance, options) ->
            // tabla-array 1-indexada de impactos, cada uno con la MISMA forma
            // que devuelve Physics.Raycast, ordenados por distancia ascendente.
            // Siempre devuelve una tabla: sin impactos (o con argumentos
            // inválidos, que además avisan) sale vacía, nunca nil, así el
            // caller puede hacer ipairs/# sin comprobar antes.
            physics["RaycastAll"] = [&mgr](sol::variadic_args va) -> sol::object {
                sol::table out = mgr.lua().create_table();
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                if (!parseRaycastArgs(mgr, "RaycastAll", va, args))
                    return sol::make_object(mgr.lua(), out);

                physx::PxRaycastBufferN<kRaycastAllMaxHits> hits;
                if (!doRaycastAll(mgr, args, hits))
                    return sol::make_object(mgr.lua(), out);

                for (physx::PxU32 i = 0; i < hits.getNbTouches(); ++i)
                    out[i + 1] = makeHitTable(mgr, hits.getTouch(i));

                // PhysX no avisa del desbordamiento: los impactos que no
                // cupieron se pierden y encima los descartados son arbitrarios
                // (el orden llega sin ordenar), no "los más lejanos".
                if (hits.getNbTouches() >= hits.getMaxNbTouches())
                    mgr.log("[Lua][WARN] Physics.RaycastAll: limite de " +
                            std::to_string(kRaycastAllMaxHits) +
                            " impactos alcanzado, hay resultados descartados");
#else
                (void)va;
#endif
                return sol::make_object(mgr.lua(), out);
            };

            // Igual pero sin construir la tabla: para el "solo quiero saber si
            // choca".
            physics["RaycastHit"] = [&mgr](sol::variadic_args va) -> bool {
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                if (!parseRaycastArgs(mgr, "RaycastHit", va, args)) return false;
                physx::PxRaycastBuffer hit;
                return doRaycast(mgr, args, hit);
#else
                (void)va;
                return false;
#endif
            };

            // Physics.SphereCast(origin, direction, radius, maxDistance,
            // options) -> la MISMA tabla { entity, point, normal, distance } que
            // Physics.Raycast, o nil si no toca nada. Es el raycast "con
            // grosor": la esfera parte centrada en origin y barre a lo largo de
            // direction. Si ya solapa algo en el origen, distance sale 0 y
            // point/normal no significan nada (PhysX no calcula la separación
            // sin eMTD).
            physics["SphereCast"] = [&mgr](sol::variadic_args va) -> sol::object {
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                float       radius = 0.0f;
                if (!parseSphereCastArgs(mgr, va, args, radius)) return sol::nil;

                physx::PxSweepBuffer hit;
                if (!doSphereCast(mgr, args, radius, hit)) return sol::nil;

                return sol::make_object(mgr.lua(), makeSweepHitTable(mgr, hit.block));
#else
                (void)va;
                return sol::nil;
#endif
            };

            // Physics.OverlapSphere(center, radius, options) -> tabla-array
            // 1-indexada de Entity (NO de tablas de impacto: un solape no tiene
            // punto, normal ni distancia). Vacía si no solapa nada o si los
            // argumentos son inválidos, nunca nil.
            physics["OverlapSphere"] = [&mgr](sol::variadic_args va) -> sol::object {
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                glm::vec3   center{ 0.0f };
                float       radius = 0.0f;
                if (!parseOverlapSphereArgs(mgr, va, center, radius, args))
                    return sol::make_object(mgr.lua(), mgr.lua().create_table());

                physx::PxOverlapBufferN<kOverlapMaxHits> hits;
                if (!doOverlapSphere(mgr, center, radius, args, hits))
                    return sol::make_object(mgr.lua(), mgr.lua().create_table());

                sol::table out = makeOverlapArray(mgr, hits);
                warnOverlapOverflow(mgr, "OverlapSphere", hits);
                return sol::make_object(mgr.lua(), out);
#else
                (void)va;
                return sol::make_object(mgr.lua(), mgr.lua().create_table());
#endif
            };

            // Physics.OverlapBox(center, halfExtents, rotation, options) ->
            // igual que OverlapSphere pero con una caja orientada. rotation es
            // opcional (Vec3 de grados Euler, misma convención que
            // transform.rotation) y se distingue de options por el tipo.
            physics["OverlapBox"] = [&mgr](sol::variadic_args va) -> sol::object {
#ifdef DT_PHYSX_ENABLED
                RaycastArgs args;
                glm::vec3   center{ 0.0f }, halfExtents{ 0.0f }, eulerDeg{ 0.0f };
                if (!parseOverlapBoxArgs(mgr, va, center, halfExtents, eulerDeg, args))
                    return sol::make_object(mgr.lua(), mgr.lua().create_table());

                physx::PxOverlapBufferN<kOverlapMaxHits> hits;
                if (!doOverlapBox(mgr, center, halfExtents, eulerDeg, args, hits))
                    return sol::make_object(mgr.lua(), mgr.lua().create_table());

                sol::table out = makeOverlapArray(mgr, hits);
                warnOverlapOverflow(mgr, "OverlapBox", hits);
                return sol::make_object(mgr.lua(), out);
#else
                (void)va;
                return sol::make_object(mgr.lua(), mgr.lua().create_table());
#endif
            };

            // Physics.SetLayerCollision(a, b, activo) / GetLayerCollision(a, b):
            // matriz GLOBAL de 32x32, simétrica — activar (a,b) activa (b,a).
            // El cambio se propaga a los colliders que ya estén en la escena, o
            // sea que vale en mitad de una partida.
            //
            // Índice fuera de [0,31]: error de Lua (ver requireLayer). Fuera de
            // Play no hay PhysicsManager: Set es no-op y Get devuelve true, que
            // es lo que dice la matriz por defecto.
            physics["SetLayerCollision"] = [&mgr](int a, int b, bool enabled) {
                requireLayer("Physics.SetLayerCollision", a);
                requireLayer("Physics.SetLayerCollision", b);
                PhysicsManager* pm = mgr.physics();
                if (!pm) return;
                pm->setLayerCollision(a, b, enabled);
            };

            physics["GetLayerCollision"] = [&mgr](int a, int b) -> bool {
                requireLayer("Physics.GetLayerCollision", a);
                requireLayer("Physics.GetLayerCollision", b);
                PhysicsManager* pm = mgr.physics();
                if (!pm) return true;
                return pm->getLayerCollision(a, b);
            };
        }

        void registerScene(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table sceneTable = lua.create_named_table("Scene");

            sceneTable["Find"] = [&mgr](const std::string& name) -> sol::object {
                if (!mgr.scene()) return sol::nil;
                GameObject* found = nullptr;
                mgr.scene()->traverse([&](GameObject* go) {
                    if (!found && go->parent && go->name == name) found = go;
                });
                if (!found) return sol::nil;
                return sol::make_object(mgr.lua(), LuaEntity{found, &mgr});
            };

            sceneTable["CreateGameObject"] = [&mgr](const std::string& name,
                                                    sol::optional<LuaEntity> parent) -> sol::object {
                if (!mgr.scene()) return sol::nil;
                GameObject* p = parent ? deref(*parent) : nullptr;
                GameObject* go = mgr.scene()->addGameObject(name, p);
                mgr.rebuildAliveSet();
                return sol::make_object(mgr.lua(), LuaEntity{go, &mgr});
            };

            sceneTable["Destroy"] = [&mgr](const LuaEntity& e) {
                mgr.queueDestroy(deref(e));
            };

            // Global estilo Unity Destroy(): destruye el GameObject y todo su
            // subtree durante Play. Misma cola diferida que Scene.Destroy — el
            // teardown (OnDestroy en scripts, liberación de GPU vía
            // m_onDestroying, destructor de GameObject que suelta colliders/
            // audio y lo saca de los managers) lo procesa el lifecycle al final
            // del frame. Diferido a propósito: destruir en mitad de Update
            // rompería la iteración del lifecycle. deref valida que la entity
            // siga viva (error Lua si ya fue destruida).
            lua["DestroyGameObject"] = [&mgr](const LuaEntity& e) {
                mgr.queueDestroy(deref(e));
            };

            sceneTable["Instantiate"] = [&mgr](const LuaEntity& src,
                                               sol::optional<LuaEntity> parent) -> sol::object {
                if (!mgr.scene() || !mgr.physics() || !mgr.audioManager()) return sol::nil;
                GameObject* srcGo = deref(src);
                GameObject* p = parent ? deref(*parent) : nullptr;
                GameObject* clone = mgr.scene()->cloneGameObject(
                    srcGo, p, *mgr.physics(), *mgr.audioManager());
                if (!clone) return sol::nil;

                if (mgr.onInstantiated()) mgr.onInstantiated()(clone);
                mgr.rebuildAliveSet();
                // Los scripts del clon se instancian ya; Awake inmediato,
                // Start lo dispara el lifecycle antes de su primer Update
                // (started == false).
                clone->traverse([&mgr](GameObject* n) {
                    for (auto& s : n->getScripts())
                    {
                        mgr.instantiateComponent(*s);
                        if (s->instance.valid() && s->hasAwake)
                        {
                            sol::protected_function f = s->instance["Awake"];
                            auto r = f(s->instance);
                            if (!r.valid())
                            {
                                sol::error err = r;
                                mgr.log("Script '" + s->scriptName + "' Awake: " + std::string(err.what()));
                                s->hasError = true;
                            }
                        }
                    }
                });
                return sol::make_object(mgr.lua(), LuaEntity{clone, &mgr});
            };
        }
    } // namespace (anónimo)

    bool takePendingSceneLoad(std::string& outPath)
    {
        if (!g_hasPendingSceneLoad) return false;
        outPath = g_pendingSceneLoad;
        g_pendingSceneLoad.clear();
        g_hasPendingSceneLoad = false;
        return true;
    }

    void clearUiCallbacks(ScriptManager& mgr)
    {
        // Tabla nueva, no borrado entrada a entrada: las claves viejas ya no le
        // sirven a nadie (los std::function que las guardaban están mudos por
        // la época) y así el GC de Lua se lleva las funciones de golpe.
        mgr.lua()[kUiCallbackTable] = mgr.lua().create_table();
    }

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerLog(mgr);
        registerInput(mgr);
        registerTransform(mgr);
        registerComponents(mgr);
        registerUi(mgr);      // antes que Entity: sus getters devuelven estos tipos
        registerEntity(mgr);
        registerScene(mgr);   // Task 7
        registerPhysics(mgr);
        registerEngineTable(mgr);
    }
}
