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
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <stdexcept>

namespace DonTopo::ScriptBindings
{
    namespace
    {
        using DonTopo::GameObject;
        using DonTopo::LuaEntity;

        // Deref validado: entity muerta -> excepción C++ que sol2 convierte en
        // error Lua (capturado por la protected_function del callback).
        GameObject* deref(const LuaEntity& e)
        {
            if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
                throw std::runtime_error("Entity destruida o inválida");
            return e.go;
        }

        struct LuaTransform { LuaEntity e; };
        struct LuaBoxCollider { LuaEntity e; };
        struct LuaSphereCollider { LuaEntity e; };
        struct LuaCapsuleCollider { LuaEntity e; };
        struct LuaPlaneCollider { LuaEntity e; };
        struct LuaAudioClip { LuaEntity e; };
        struct LuaRigidbody { LuaEntity e; };
        struct LuaAnimator { LuaEntity e; };

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

        void registerInput(sol::state& lua)
        {
            sol::table input = lua.create_named_table("Input");
            input["IsKeyDown"]          = [](int k) { return Input::isKeyDown(k); };
            input["IsKeyPressed"]       = [](int k) { return Input::isKeyPressed(k); };
            input["IsKeyReleased"]      = [](int k) { return Input::isKeyReleased(k); };
            input["IsMouseButtonDown"]  = [](int b) { return Input::isMouseButtonDown(b); };

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

        void registerTransform(sol::state& lua)
        {
            lua.new_usertype<LuaTransform>("Transform",
                sol::no_constructor,
                "GetPosition", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return p;
                },
                "SetPosition", [](const LuaTransform& t, const glm::vec3& np) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, np, r, s);
                },
                "GetRotation", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return r;
                },
                "SetRotation", [](const LuaTransform& t, const glm::vec3& nr) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, nr, s);
                },
                "GetScale", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return s;
                },
                "SetScale", [](const LuaTransform& t, const glm::vec3& ns) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, r, ns);
                },
                "GetWorldPosition", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return glm::vec3(go->worldTransform[3]);
                },
                "Translate", [](const LuaTransform& t, const glm::vec3& d) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p + d, r, s);
                },
                "Rotate", [](const LuaTransform& t, const glm::vec3& dEuler) {
                    // Rotación incremental compuesta como quaternion, NUNCA
                    // sumando eulers: extractEulerAngleXYZ acota el ángulo
                    // medio a ±90°, y acumular sobre esa representación hace
                    // que una rotación continua se "atasque" al llegar al
                    // límite (gira y luego se queda casi quieta).
                    GameObject* go = deref(t.e);
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

        void registerComponents(sol::state& lua)
        {
            lua.new_usertype<LuaBoxCollider>("BoxCollider",
                sol::no_constructor,
                "GetHalfExtents", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->getHalfExtents();
                },
                "SetHalfExtents", [](const LuaBoxCollider& c, const glm::vec3& he) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    go->getBoxCollider()->setHalfExtents(he);
                },
                "GetCenter", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->getCenter();
                },
                "SetCenter", [](const LuaBoxCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    go->getBoxCollider()->setCenter(ctr);
                });

            lua.new_usertype<LuaSphereCollider>("SphereCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->getRadius();
                },
                "SetRadius", [](const LuaSphereCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    go->getSphereCollider()->setRadius(r);
                },
                "GetCenter", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->getCenter();
                },
                "SetCenter", [](const LuaSphereCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    go->getSphereCollider()->setCenter(ctr);
                });

            lua.new_usertype<LuaCapsuleCollider>("CapsuleCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getRadius();
                },
                "SetRadius", [](const LuaCapsuleCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    go->getCapsuleCollider()->setRadius(r);
                },
                "GetHalfHeight", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getHalfHeight();
                },
                "SetHalfHeight", [](const LuaCapsuleCollider& c, float h) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    go->getCapsuleCollider()->setHalfHeight(h);
                },
                "GetCenter", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getCenter();
                },
                "SetCenter", [](const LuaCapsuleCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    go->getCapsuleCollider()->setCenter(ctr);
                });

            lua.new_usertype<LuaPlaneCollider>("PlaneCollider",
                sol::no_constructor,
                "GetCenter", [](const LuaPlaneCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
                    return go->getPlaneCollider()->getCenter();
                },
                "SetCenter", [](const LuaPlaneCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("El GameObject ya no tiene Plane Collider");
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
                    [rbOf](const LuaRigidbody& c, float v) { rbOf(c)->setMass(v); }),
                "useGravity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getUseGravity(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setUseGravity(v); }),
                "isKinematic", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getIsKinematic(); },
                    [rbOf](const LuaRigidbody& c, bool v) { rbOf(c)->setIsKinematic(v); }),
                "drag", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getDrag(); },
                    [rbOf](const LuaRigidbody& c, float v) { rbOf(c)->setDrag(v); }),
                "angularDrag", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getAngularDrag(); },
                    [rbOf](const LuaRigidbody& c, float v) { rbOf(c)->setAngularDrag(v); }),
                "velocity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getVelocity(); },
                    [rbOf](const LuaRigidbody& c, const glm::vec3& v) { rbOf(c)->setVelocity(v); }),
                "angularVelocity", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getAngularVelocity(); },
                    [rbOf](const LuaRigidbody& c, const glm::vec3& v) { rbOf(c)->setAngularVelocity(v); }),
                "AddForce",   [rbOf](const LuaRigidbody& c, float x, float y, float z) { rbOf(c)->addForce({x, y, z}); },
                "AddTorque",  [rbOf](const LuaRigidbody& c, float x, float y, float z) { rbOf(c)->addTorque({x, y, z}); },
                "AddImpulse", [rbOf](const LuaRigidbody& c, float x, float y, float z) { rbOf(c)->addImpulse({x, y, z}); });

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
                "SetFloat",   [animOf](const LuaAnimator& c, const std::string& n, float v) { animOf(c)->setFloat(n, v); },
                "GetFloat",   [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getFloat(n); },
                "GetState",   [animOf](const LuaAnimator& c) { return animOf(c)->currentStateName(); });
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

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerLog(mgr);
        registerInput(mgr.lua());
        registerTransform(mgr.lua());
        registerComponents(mgr.lua());
        registerEntity(mgr);
        registerScene(mgr);   // Task 7
    }
}
