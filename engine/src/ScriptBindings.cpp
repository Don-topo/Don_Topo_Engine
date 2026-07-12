#include "DonTopo/ScriptBindings.h"
#include "DonTopo/ScriptManager.h"
#include "DonTopo/Input.h"
#include "DonTopo/Scene.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

        // Descompone localTransform en T/R/S (grados pa Lua).
        void decomposeLocal(GameObject* go, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
        {
            glm::quat rot; glm::vec3 skew; glm::vec4 persp;
            glm::decompose(go->localTransform, scale, rot, pos, skew, persp);
            eulerDeg = glm::degrees(glm::eulerAngles(rot));
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
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, r + dEuler, s);
                });
        }

        void registerComponents(sol::state& lua)
        {
            lua.new_usertype<LuaBoxCollider>("BoxCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaBoxCollider& c, bool g) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    go->getBoxCollider()->setUseGravity(g);
                },
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
                },
                "IsDynamic", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("El GameObject ya no tiene Box Collider");
                    return go->getBoxCollider()->isDynamic();
                });

            lua.new_usertype<LuaSphereCollider>("SphereCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaSphereCollider& c, bool g) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    go->getSphereCollider()->setUseGravity(g);
                },
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
                },
                "IsDynamic", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("El GameObject ya no tiene Sphere Collider");
                    return go->getSphereCollider()->isDynamic();
                });

            lua.new_usertype<LuaCapsuleCollider>("CapsuleCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaCapsuleCollider& c, bool g) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    go->getCapsuleCollider()->setUseGravity(g);
                },
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
                },
                "IsDynamic", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("El GameObject ya no tiene Capsule Collider");
                    return go->getCapsuleCollider()->isDynamic();
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
                        auto clip = mgr->audioManager()->createAudioClipComponent(*arg, true, false);
                        if (clip) { go->setAudioClip(std::move(clip)); return sol::make_object(lua, LuaAudioClip{e}); }
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
    }

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerLog(mgr);
        registerInput(mgr.lua());
        registerTransform(mgr.lua());
        registerComponents(mgr.lua());
        registerEntity(mgr);
    }
}
