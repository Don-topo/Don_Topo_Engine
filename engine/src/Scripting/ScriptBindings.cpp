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
#include "DonTopo/Files/FileManager.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <stdexcept>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <tuple>

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
                });

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
                });

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
                });

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
                });

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
                });

            // Rigidbody: dinámica estilo Unity. Propiedades (mass/useGravity/
            // isKinematic/drag/angularDrag/velocity/angularVelocity) + métodos
            // AddForce/AddTorque/AddImpulse. Se obtiene con GetComponent("Rigidbody").
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
                "AddForce",   [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 f(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddForce", f)) return;
                    rb->addForce(f);
                },
                "AddTorque",  [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 t(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddTorque", t)) return;
                    rb->addTorque(t);
                },
                "AddImpulse", [rbOf, &mgr](const LuaRigidbody& c, float x, float y, float z) {
                    Rigidbody* rb = rbOf(c);
                    glm::vec3 f(x, y, z);
                    if (!ensureFinite(mgr, "Rigidbody.AddImpulse", f)) return;
                    rb->addImpulse(f);
                });

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
                "GetState",   [animOf](const LuaAnimator& c) { return animOf(c)->currentStateName(); });
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
            lua["UiTextAlign"] = lua.create_table_with(
                "Left", 0, "Center", 1, "Right", 2, "Justify", 3);
            lua["UiTextOverflow"] = lua.create_table_with(
                "Overflow", 0, "Clip", 1, "Ellipsis", 2);
            lua["UiProgressFillDirection"] = lua.create_table_with(
                "LeftToRight", 0, "RightToLeft", 1, "BottomToTop", 2, "TopToBottom", 3);
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
                "RemoveCanvas",      [](const LuaEntity& e) { deref(e)->setCanvas(nullptr); },
                "RemoveButton",      [](const LuaEntity& e) { deref(e)->setButton(nullptr); },
                "RemoveText",        [](const LuaEntity& e) { deref(e)->setText(nullptr); },
                "RemoveProgressBar", [](const LuaEntity& e) { deref(e)->setProgressBar(nullptr); },
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
        registerEngineTable(mgr);
    }
}
