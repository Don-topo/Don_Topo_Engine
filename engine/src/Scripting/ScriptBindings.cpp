#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Core/TransformDecompose.h"
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
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/CameraComponent.h"
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
#include "DonTopo/Audio/ReverbZoneComponent.h"

namespace DonTopo::ScriptBindings
{
    namespace
    {
        using DonTopo::GameObject;
        using DonTopo::LuaEntity;

        // Mailbox for DonTopo.loadScene (see ScriptBindings.h). A single slot:
        // two requests in the same frame -> the last one wins. It lives here and not in
        // ScriptManager because the consumer (EditorUI / runtime) only needs
        // the path, not the VM. Only one thread touches it: the scripts thread and the
        // frame loop thread are the same.
        std::string g_pendingSceneLoad;
        bool        g_hasPendingSceneLoad = false;

        // Validated deref: dead entity -> C++ exception that sol2 turns into a
        // Lua error (caught by the callback's protected_function).
        GameObject* deref(const LuaEntity& e)
        {
            if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
                throw std::runtime_error("Entity destroyed or invalid");
            return e.go;
        }

        // std::clamp(NaN, lo, hi) returns NaN: every comparison with NaN is
        // false, so the range clamp (e.g. the [0,1] of volume) does not
        // stop it. Infinities DO clamp fine (clamp(+inf,0,1) == 1.0),
        // so the truly dangerous one is NaN, not infinity: a NaN sneaks
        // into the scene JSON (nlohmann serializes it as
        // "null"), and when that "null" is read back with .get<float>() nlohmann throws
        // json::exception, which made the WHOLE Scene::fromJson fail because of a
        // single corrupt field. It is stopped here, at the entry point from
        // Lua: the value is ignored (the previous one is kept) and a warning goes to the Log
        // Console, without raising a Lua error: a broken computation in a script
        // (e.g. a 0/0) must not kill the game.
        //
        // IMPORTANT at every call site: call ensureFinite AFTER
        // deref() and after any has*Collider()/hasAudioClip()/hasRigidbody(),
        // never before. Otherwise an already destroyed entity with a NaN as a gift
        // (deadEntity:GetTransform():SetPosition(Vec3(0/0,0,0))) just
        // warns about the NaN and returns, when the real and more serious bug,
        // use-after-destroy, should keep raising a Lua error
        // as always (finding 4 of this fix's review).
        bool ensureFinite(ScriptManager& mgr, const char* metodo, float v)
        {
            if (std::isfinite(v)) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": non-finite value (NaN/Inf) ignored, the previous one is kept");
            return false;
        }

        bool ensureFinite(ScriptManager& mgr, const char* metodo, const glm::vec3& v)
        {
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": vector with a non-finite component (NaN/Inf) ignored, the previous one is kept");
            return false;
        }

        // Optional force mode of AddForce/AddTorque. It arrives from Lua as an
        // integer (ForceMode table) and is validated against the range of the enum in
        // Rigidbody.h. Out of range does NOT throw: same channel as ensureFinite,
        // a warning through the Log Console and the whole call is ignored. Absent
        // (three-argument call) is always valid and means Force.
        constexpr int kForceModeMax = static_cast<int>(ForceMode::VelocityChange);

        bool ensureForceMode(ScriptManager& mgr, const char* metodo, const sol::optional<int>& mode)
        {
            if (!mode) return true;
            if (*mode >= 0 && *mode <= kForceModeMax) return true;
            mgr.log(std::string("[Lua][WARN] ") + metodo +
                    ": ForceMode out of range (" + std::to_string(*mode) +
                    "), force ignored; use the ForceMode table");
            return false;
        }

        // Only called after a successful ensureForceMode, so the value is already
        // inside the enum.
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
        struct LuaReverbZone { LuaEntity e; };
        struct LuaRigidbody { LuaEntity e; };
        struct LuaAnimator { LuaEntity e; };
        struct LuaLight { LuaEntity e; };
        struct LuaCamera { LuaEntity e; };
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

        // Decomposes localTransform into T/R/S (degrees for Lua). The angle extraction
        // uses extractEulerAngleXYZ, the exact inverse of the eulerAngleXYZ in
        // recomposeLocal; mixing conventions (e.g.
        // glm::eulerAngles on the quat) corrupts the rotation on any
        // object rotated on more than one axis.
        void decomposeLocal(GameObject* go, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
        {
            glm::quat rot;
            // Without checking what decompose returns, a scale of 0 left the three
            // outputs UNINITIALIZED and Lua read garbage through Transform.position,
            // .rotation and .scale.
            decomposeTransform(go->localTransform, &pos, &rot, &scale);
            glm::mat4 rotOnly = glm::mat4_cast(rot);
            float t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
            glm::extractEulerAngleXYZ(rotOnly, t1, t2, t3);
            eulerDeg = glm::degrees(glm::vec3(t1, t2, t3));
        }

        // Normalizes a column of the worldTransform. An object with scale 0 on
        // that axis leaves the column at zero and normalize() would return NaN, which
        // travels from Lua to a setter and from there to the scene JSON: with a
        // degenerate scale the canonical axis is returned, which is at least a valid
        // direction.
        glm::vec3 safeAxis(const glm::vec3& column, const glm::vec3& fallback)
        {
            const float len = glm::length(column);
            return len > 1e-6f ? column / len : fallback;
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
                // Algebra that previously had to be written by hand with math.sqrt.
                // All of them return new values: none mutates the receiver, so
                // 'local d = a:Normalized()' leaves 'a' intact.
                "Length", [](const glm::vec3& v) { return glm::length(v); },
                // Unit length vector. The zero vector has no direction: zero is
                // returned as is instead of a NaN, which would sneak into
                // the scene JSON (see ensureFinite).
                "Normalized", [](const glm::vec3& v) {
                    const float len = glm::length(v);
                    return len > 0.0f ? v / len : glm::vec3(0.0f);
                },
                "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
                "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); },
                "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); },
                // Linear interpolation WITHOUT clamping t: with t outside [0,1]
                // it extrapolates, like glm::mix and unlike Unity.
                "Lerp", [](const glm::vec3& a, const glm::vec3& b, float t) {
                    return a + (b - a) * t;
                },
                sol::meta_function::addition,
                    [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
                sol::meta_function::subtraction,
                    [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
                // Lua tries the __mul of the LEFT operand and, if that does not fit,
                // the one of the right operand, but with the arguments in the written order.
                // Without the second overload, '2 * v' arrived here as
                // (float, vec3) and blew up with a type error.
                sol::meta_function::multiplication, sol::overload(
                    [](const glm::vec3& v, float s) { return v * s; },
                    [](float s, const glm::vec3& v) { return v * s; }),
                // Dividing by zero gives inf/NaN, which ensureFinite stops as soon as
                // the result tries to enter an engine setter.
                sol::meta_function::division,
                    [](const glm::vec3& v, float s) { return v / s; },
                sol::meta_function::unary_minus,
                    [](const glm::vec3& v) { return -v; },
                // Exact component-wise equality. Lua only calls
                // __eq when both operands are of the same type, so
                // 'v == nil' is still false without going through here.
                sol::meta_function::equal_to,
                    [](const glm::vec3& a, const glm::vec3& b) { return a == b; },
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
            // native print -> same destination as Log.Info. Each argument goes through
            // Lua's tostring (it handles numbers, nil, tables and the
            // __tostring metamethod), like the native print.
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

        // DonTopo.loadScene(path) -> bool. It does NOT load: it validates and enqueues (see the
        // mailbox above). The bool is the result of the validation (readable file,
        // parseable JSON, v1 scene structure), the same one EditorUI::loadSceneFile does
        // before touching the GPU; the outcome of the load itself arrives one frame
        // later and cannot be returned here. No exceptions towards Lua: readJson
        // already returns an optional.
        void registerEngineTable(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table engine = lua.create_named_table("DonTopo");

            engine["loadScene"] = [&mgr](const std::string& path) -> bool {
                if (path.empty())
                {
                    mgr.log("[Lua][ERROR] DonTopo.loadScene: empty path");
                    return false;
                }
                auto parsed = FileManager::readJson(path);
                bool structureOk = parsed.has_value() &&
                                   parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                   (*parsed)["version"].get<int>() == 1 &&
                                   parsed->contains("root") && (*parsed)["root"].is_object();
                if (!structureOk)
                {
                    mgr.log("[Lua][ERROR] DonTopo.loadScene: could not read the scene '" + path + "'");
                    return false;
                }
                // Last request of the frame wins: the previous one is overwritten without warning.
                g_pendingSceneLoad    = path;
                g_hasPendingSceneLoad = true;
                return true;
            };
        }

        // Script clock. The three accumulated values live here and not in the
        // Lua table because the table is writable from a script: if
        // someone did Time.time = 0, the C++ accumulator would still be
        // the good one and the next frame would restore the correct value.
        float g_timeSincePlay = 0.0f;
        int   g_frameCount    = 0;

        void registerTime(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table time = lua.create_named_table("Time");
            // fixedDeltaTime is constant (ScriptManager's fixed step) and is
            // written only once; the other three are overwritten by tickTime.
            time["fixedDeltaTime"] = ScriptManager::kFixedStep;
            time["deltaTime"]      = 0.0f;
            time["time"]           = 0.0f;
            time["frameCount"]     = 0;
        }

        // Game light and camera. Both components existed in the core for a long
        // time and did not reach Lua: turning on a light, changing its color
        // or widening the FOV could only be done by hand in the
        // inspector. Neither stores position or orientation (they come
        // from the GameObject's worldTransform), so here there are only settings.
        //
        // All the core setters already clamp (LightComponent.h,
        // CameraComponent.cpp): the clamp is not repeated here, but the NaN filter is,
        // because std::clamp(NaN,...) returns NaN and it would sneak through entirely.
        void registerLighting(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();

            lua["LightType"] = lua.create_table_with(
                "Point", static_cast<int>(LightType::Point),
                "Spot", static_cast<int>(LightType::Spot),
                "Directional", static_cast<int>(LightType::Directional),
                "Area", static_cast<int>(LightType::Area));
            lua["CameraProjection"] = lua.create_table_with(
                "Perspective", static_cast<int>(CameraComponent::ProjectionMode::Perspective),
                "Orthographic", static_cast<int>(CameraComponent::ProjectionMode::Orthographic));

            auto lightOf = [](const LuaLight& c) -> LightComponent* {
                GameObject* go = deref(c.e);
                if (!go->hasLight()) throw std::runtime_error("The GameObject no longer has a Light");
                return go->getLight().get();
            };
            lua.new_usertype<LuaLight>("Light",
                sol::no_constructor,
                // Type outside the enum: a warning is issued and it is ignored, same criterion as
                // ForceMode. Any value would travel to direction.w of the UBO
                // and the shader would pick a branch that does not exist.
                "type", sol::property(
                    [lightOf](const LuaLight& c) { return static_cast<int>(lightOf(c)->getType()); },
                    [lightOf, &mgr](const LuaLight& c, int v) {
                        LightComponent* l = lightOf(c);
                        if (v < static_cast<int>(LightType::Point) ||
                            v > static_cast<int>(LightType::Area))
                        {
                            mgr.log("[Lua][WARN] Light.type: value out of range (" +
                                    std::to_string(v) + "), use the LightType table");
                            return;
                        }
                        l->setType(static_cast<LightType>(v));
                    }),
                "intensity", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getIntensity(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.intensity", v)) return;
                        l->setIntensity(v);
                    }),
                "range", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getRange(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.range", v)) return;
                        l->setRange(v);
                    }),
                "innerAngle", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getInnerAngle(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.innerAngle", v)) return;
                        l->setInnerAngle(v);
                    }),
                "outerAngle", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getOuterAngle(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.outerAngle", v)) return;
                        l->setOuterAngle(v);
                    }),
                "areaWidth", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getAreaWidth(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.areaWidth", v)) return;
                        l->setAreaWidth(v);
                    }),
                "areaHeight", sol::property(
                    [lightOf](const LuaLight& c) { return lightOf(c)->getAreaHeight(); },
                    [lightOf, &mgr](const LuaLight& c, float v) {
                        LightComponent* l = lightOf(c);
                        if (!ensureFinite(mgr, "Light.areaHeight", v)) return;
                        l->setAreaHeight(v);
                    }),
                // The color goes through a method and not a property because it is a Vec3:
                // 'light.color.x = 1' on a property would write to a temporary
                // COPY and be lost without warning.
                "GetColor", [lightOf](const LuaLight& c) { return lightOf(c)->getColor(); },
                "SetColor", [lightOf, &mgr](const LuaLight& c, const glm::vec3& col) {
                    LightComponent* l = lightOf(c);
                    if (!ensureFinite(mgr, "Light.SetColor", col)) return;
                    l->setColor(col);
                });

            auto camOf = [](const LuaCamera& c) -> CameraComponent* {
                GameObject* go = deref(c.e);
                if (!go->hasCameraComponent()) throw std::runtime_error("The GameObject no longer has a Camera");
                return go->getCameraComponent().get();
            };
            lua.new_usertype<LuaCamera>("Camera",
                sol::no_constructor,
                "mode", sol::property(
                    [camOf](const LuaCamera& c) { return static_cast<int>(camOf(c)->getMode()); },
                    [camOf, &mgr](const LuaCamera& c, int v) {
                        CameraComponent* cam = camOf(c);
                        if (v < static_cast<int>(CameraComponent::ProjectionMode::Perspective) ||
                            v > static_cast<int>(CameraComponent::ProjectionMode::Orthographic))
                        {
                            mgr.log("[Lua][WARN] Camera.mode: value out of range (" +
                                    std::to_string(v) + "), use the CameraProjection table");
                            return;
                        }
                        cam->setMode(static_cast<CameraComponent::ProjectionMode>(v));
                    }),
                // Only used by perspective mode; orthographic ignores it.
                "fov", sol::property(
                    [camOf](const LuaCamera& c) { return camOf(c)->getFov(); },
                    [camOf, &mgr](const LuaCamera& c, float v) {
                        CameraComponent* cam = camOf(c);
                        if (!ensureFinite(mgr, "Camera.fov", v)) return;
                        cam->setFov(v);
                    }),
                // Half-height in world units; only used by orthographic.
                "orthographicSize", sol::property(
                    [camOf](const LuaCamera& c) { return camOf(c)->getOrthographicSize(); },
                    [camOf, &mgr](const LuaCamera& c, float v) {
                        CameraComponent* cam = camOf(c);
                        if (!ensureFinite(mgr, "Camera.orthographicSize", v)) return;
                        cam->setOrthographicSize(v);
                    }),
                "near", sol::property(
                    [camOf](const LuaCamera& c) { return camOf(c)->getNear(); },
                    [camOf, &mgr](const LuaCamera& c, float v) {
                        CameraComponent* cam = camOf(c);
                        if (!ensureFinite(mgr, "Camera.near", v)) return;
                        cam->setNear(v);
                    }),
                "far", sol::property(
                    [camOf](const LuaCamera& c) { return camOf(c)->getFar(); },
                    [camOf, &mgr](const LuaCamera& c, float v) {
                        CameraComponent* cam = camOf(c);
                        if (!ensureFinite(mgr, "Camera.far", v)) return;
                        cam->setFar(v);
                    }));
        }

        void registerInput(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table input = lua.create_named_table("Input");
            input["IsKeyDown"]          = [](int k) { return Input::isKeyDown(k); };
            input["IsKeyPressed"]       = [](int k) { return Input::isKeyPressed(k); };
            input["IsKeyReleased"]      = [](int k) { return Input::isKeyReleased(k); };
            input["IsMouseButtonDown"]  = [](int b) { return Input::isMouseButtonDown(b); };

            // Named actions of the Input Actions panel. An unknown name
            // returns false and warns ONCE per name and session: the typical call
            // lives in Update() and a warning per frame would drown the Log.
            auto warned = std::make_shared<std::set<std::string>>();
            auto known  = [&mgr, warned](const std::string& name) {
                const bool ok = Input::hasAction(name);   // forces the lazy load of the map
                // Load warnings (ignored gamepad bindings): the list is
                // emptied when read, so they come out only once.
                for (const std::string& d : Input::takeActionDiagnostics())
                    mgr.log("[Lua][WARN] " + d);
                if (!ok && warned->insert(name).second)
                    mgr.log("[Lua][WARN] Input: there is no action '" + name +
                            "' (define it in the Input Actions panel)");
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

            // Raw gamepad. The normal way is to use named actions (the Input
            // Actions panel already knows about gamepads), but the core exposes the gamepad
            // directly and the panel uses it to capture bindings: we do not cap in
            // Lua what the engine gives. With no gamepad connected everything returns false,
            // never an error.
            input["IsPadButtonDown"]    = [](int b) { return Input::isPadButtonDown(b); };
            input["IsPadButtonPressed"] = [](int b) { return Input::isPadButtonPressed(b); };
            // Axes are queried by CODE, not by axis: an axis is two
            // directions (up/down) and each one counts as a separate binding.
            // The code is composed by PadAxis.Code(axis, negative) or, more conveniently,
            // read from the precomposed constants of the PadAxis table.
            input["IsPadAxisDown"]      = [](int code) { return Input::isPadAxisDown(code); };
            input["IsPadAxisPressed"]   = [](int code) { return Input::isPadAxisPressed(code); };

            sol::table pad = lua.create_named_table("PadButton");
            pad["A"] = GLFW_GAMEPAD_BUTTON_A;  pad["B"] = GLFW_GAMEPAD_BUTTON_B;
            pad["X"] = GLFW_GAMEPAD_BUTTON_X;  pad["Y"] = GLFW_GAMEPAD_BUTTON_Y;
            pad["LeftBumper"]  = GLFW_GAMEPAD_BUTTON_LEFT_BUMPER;
            pad["RightBumper"] = GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER;
            pad["Back"]  = GLFW_GAMEPAD_BUTTON_BACK;
            pad["Start"] = GLFW_GAMEPAD_BUTTON_START;
            pad["Guide"] = GLFW_GAMEPAD_BUTTON_GUIDE;
            pad["LeftThumb"]  = GLFW_GAMEPAD_BUTTON_LEFT_THUMB;
            pad["RightThumb"] = GLFW_GAMEPAD_BUTTON_RIGHT_THUMB;
            pad["DpadUp"]    = GLFW_GAMEPAD_BUTTON_DPAD_UP;
            pad["DpadRight"] = GLFW_GAMEPAD_BUTTON_DPAD_RIGHT;
            pad["DpadDown"]  = GLFW_GAMEPAD_BUTTON_DPAD_DOWN;
            pad["DpadLeft"]  = GLFW_GAMEPAD_BUTTON_DPAD_LEFT;

            // Precomposed codes, one per direction. The names describe
            // which way it is pushed, not the sign of the axis: in GLFW the Y axis of
            // the sticks grows DOWNWARDS, so "Up" is the negative one.
            sol::table axis = lua.create_named_table("PadAxis");
            axis["Code"] = [](int a, bool negative) { return Input::padAxisCode(a, negative); };
            axis["LeftStickRight"] = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, false);
            axis["LeftStickLeft"]  = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, true);
            axis["LeftStickDown"]  = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y, false);
            axis["LeftStickUp"]    = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y, true);
            axis["RightStickRight"] = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_X, false);
            axis["RightStickLeft"]  = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_X, true);
            axis["RightStickDown"]  = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_Y, false);
            axis["RightStickUp"]    = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_Y, true);
            // The triggers are analog with rest at -1: Input renormalizes
            // them to [0,1], so they only have a positive direction.
            axis["LeftTrigger"]  = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, false);
            axis["RightTrigger"] = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, false);
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
                    // deref BEFORE ensureFinite: a destroyed entity has
                    // to give the usual Lua error (use-after-destroy,
                    // the big bug), not a silent NaN warning that lets it
                    // through (finding 4 of the review).
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
                // Places the object at a WORLD position: converts to local
                // by undoing the parent's worldTransform. Without a parent it is the
                // same as SetPosition. A parent with scale 0 gives a singular matrix
                // and its inverse is infinities: ensureFinite catches the
                // result before writing it, since otherwise it would leave the object
                // out of the universe with no way back.
                "SetWorldPosition", [&mgr](const LuaTransform& t, const glm::vec3& wp) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.SetWorldPosition", wp)) return;
                    glm::vec3 local = wp;
                    if (go->parent)
                        local = glm::vec3(glm::inverse(go->parent->worldTransform) * glm::vec4(wp, 1.0f));
                    if (!ensureFinite(mgr, "Transform.SetWorldPosition", local)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, local, r, s);
                },
                // Object axes in WORLD space, already normalized (a scaled object
                // would give vectors longer than 1 if read raw). The
                // convention is that of CameraComponent and glm::lookAt: it looks
                // towards local -Z, +Y is up and +X is right.
                "GetForward", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return safeAxis(-glm::vec3(go->worldTransform[2]), glm::vec3(0.0f, 0.0f, -1.0f));
                },
                "GetRight", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return safeAxis(glm::vec3(go->worldTransform[0]), glm::vec3(1.0f, 0.0f, 0.0f));
                },
                "GetUp", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return safeAxis(glm::vec3(go->worldTransform[1]), glm::vec3(0.0f, 1.0f, 0.0f));
                },
                // Orients the object so its forward (-Z) points at the given world
                // point, keeping position and scale. The optional up
                // (+Y by default) resolves the leftover twist around the
                // forward. Degenerate cases (looking at itself, or an up
                // parallel to the forward) have no answer: a warning is issued and the
                // rotation is left as it was, instead of installing a matrix with
                // NaN that would drag the children along.
                "LookAt", [&mgr](const LuaTransform& t, const glm::vec3& target,
                                 sol::optional<glm::vec3> up) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.LookAt", target)) return;
                    const glm::vec3 upVec = up ? *up : glm::vec3(0.0f, 1.0f, 0.0f);
                    if (!ensureFinite(mgr, "Transform.LookAt", upVec)) return;

                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    const glm::vec3 worldPos = glm::vec3(go->worldTransform[3]);
                    const glm::vec3 dir = target - worldPos;
                    if (glm::length(dir) <= 1e-6f || glm::length(upVec) <= 1e-6f)
                    {
                        mgr.log("[Lua][WARN] Transform.LookAt: target at its own position "
                                "or null 'up', rotation unchanged");
                        return;
                    }
                    const glm::vec3 fwd = glm::normalize(dir);
                    if (std::abs(glm::dot(fwd, glm::normalize(upVec))) > 0.9999f)
                    {
                        mgr.log("[Lua][WARN] Transform.LookAt: 'up' parallel to the view "
                                "direction, rotation unchanged");
                        return;
                    }
                    // lookAt returns a VIEW (world -> camera); the object's pose
                    // is its inverse. It is composed in world space and, if there is a
                    // parent, converted to local: otherwise an object with a rotated parent
                    // would look anywhere but at the target.
                    glm::mat4 world = glm::inverse(glm::lookAt(worldPos, target, upVec));
                    if (go->parent)
                        world = glm::inverse(go->parent->worldTransform) * world;
                    float t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
                    glm::extractEulerAngleXYZ(world, t1, t2, t3);
                    const glm::vec3 eulerDeg = glm::degrees(glm::vec3(t1, t2, t3));
                    if (!ensureFinite(mgr, "Transform.LookAt", eulerDeg)) return;
                    recomposeLocal(go, p, eulerDeg, s);
                },
                "Translate", [&mgr](const LuaTransform& t, const glm::vec3& d) {
                    GameObject* go = deref(t.e);
                    if (!ensureFinite(mgr, "Transform.Translate", d)) return;
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p + d, r, s);
                },
                "Rotate", [&mgr](const LuaTransform& t, const glm::vec3& dEuler) {
                    GameObject* go = deref(t.e);
                    // Incremental rotation composed as a quaternion, NEVER
                    // by adding eulers: extractEulerAngleXYZ clamps the middle
                    // angle to ±90°, and accumulating on that representation makes
                    // a continuous rotation get "stuck" on reaching the
                    // limit (it turns and then stays almost still).
                    if (!ensureFinite(mgr, "Transform.Rotate", dEuler)) return;
                    glm::vec3 scale, pos; glm::quat rot;
                    // Here the oversight was the most expensive of the four: with the
                    // outputs uninitialized, the localTransform was REWRITTEN
                    // with that garbage and the object was wrecked
                    // forever, not just drawn wrong.
                    decomposeTransform(go->localTransform, &pos, &rot, &scale);
                    rot = rot * glm::quat(glm::radians(dEuler));
                    go->localTransform = glm::translate(glm::mat4(1.0f), pos) *
                                         glm::mat4_cast(rot) *
                                         glm::scale(glm::mat4(1.0f), scale);
                    go->updateWorldTransforms(go->parent ? go->parent->worldTransform
                                                         : glm::mat4(1.0f));
                });
        }

        // Valid collision layer index, or a Lua error. Used by the four
        // colliders (.layer) and the Physics matrix. It throws instead of clamping
        // on purpose: with a clamp the script would believe it is filtering by the
        // layer it asked for when it is actually in another, and that is not visible until
        // something goes through something.
        void requireLayer(const char* what, int layer)
        {
            if (!DonTopo::PhysicsManager::isValidLayer(layer))
                throw std::runtime_error(std::string(what) + ": layer out of range (0-" +
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
                    if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                    return go->getBoxCollider()->getHalfExtents();
                },
                "SetHalfExtents", [&mgr](const LuaBoxCollider& c, const glm::vec3& he) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                    if (!ensureFinite(mgr, "BoxCollider.SetHalfExtents", he)) return;
                    go->getBoxCollider()->setHalfExtents(he);
                },
                "GetCenter", [](const LuaBoxCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                    return go->getBoxCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaBoxCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                    if (!ensureFinite(mgr, "BoxCollider.SetCenter", ctr)) return;
                    go->getBoxCollider()->setCenter(ctr);
                },
                // Physics material of the collider. Properties (not Get/Set)
                // as in Rigidbody. As in mass: deref + has BEFORE the
                // finiteness guard.
                "staticFriction", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        return go->getBoxCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.staticFriction", v)) return;
                        go->getBoxCollider()->setFriction(v, go->getBoxCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        return go->getBoxCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.dynamicFriction", v)) return;
                        go->getBoxCollider()->setFriction(go->getBoxCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        return go->getBoxCollider()->getBounciness();
                    },
                    [&mgr](const LuaBoxCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        if (!ensureFinite(mgr, "BoxCollider.bounciness", v)) return;
                        go->getBoxCollider()->setBounciness(v);
                    }),
                // Collision layer (0-31). What it collides with is decided by the
                // global matrix (Physics.SetLayerCollision). The setter rewrites
                // the shape's filter, so it also works in the middle of Play.
                "layer", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        return go->getBoxCollider()->getLayer();
                    },
                    [](const LuaBoxCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        requireLayer("BoxCollider.layer", v);
                        go->getBoxCollider()->setLayer(v);
                    }),
                // Is Trigger. The setter does NOT touch the collider directly: it goes through
                // PhysicsManager::setTrigger, which besides the flag flip
                // adds/removes the collider in the onTriggerStay registry.
                // Outside Play there is no PhysicsManager (same as in the raycast):
                // silent no-op, not a Lua error.
                "isTrigger", sol::property(
                    [](const LuaBoxCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        return go->getBoxCollider()->isTrigger();
                    },
                    [&mgr](const LuaBoxCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasBoxCollider()) throw std::runtime_error("The GameObject no longer has a Box Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getBoxCollider(), v);
                    }));

            lua.new_usertype<LuaSphereCollider>("SphereCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                    return go->getSphereCollider()->getRadius();
                },
                "SetRadius", [&mgr](const LuaSphereCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                    if (!ensureFinite(mgr, "SphereCollider.SetRadius", r)) return;
                    go->getSphereCollider()->setRadius(r);
                },
                "GetCenter", [](const LuaSphereCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                    return go->getSphereCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaSphereCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                    if (!ensureFinite(mgr, "SphereCollider.SetCenter", ctr)) return;
                    go->getSphereCollider()->setCenter(ctr);
                },
                // Physics material of the collider; see the note in BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        return go->getSphereCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.staticFriction", v)) return;
                        go->getSphereCollider()->setFriction(v, go->getSphereCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        return go->getSphereCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.dynamicFriction", v)) return;
                        go->getSphereCollider()->setFriction(go->getSphereCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        return go->getSphereCollider()->getBounciness();
                    },
                    [&mgr](const LuaSphereCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        if (!ensureFinite(mgr, "SphereCollider.bounciness", v)) return;
                        go->getSphereCollider()->setBounciness(v);
                    }),
                // Collision layer; see the note in BoxCollider.
                "layer", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        return go->getSphereCollider()->getLayer();
                    },
                    [](const LuaSphereCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        requireLayer("SphereCollider.layer", v);
                        go->getSphereCollider()->setLayer(v);
                    }),
                // Is Trigger; see the note in BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaSphereCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        return go->getSphereCollider()->isTrigger();
                    },
                    [&mgr](const LuaSphereCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasSphereCollider()) throw std::runtime_error("The GameObject no longer has a Sphere Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getSphereCollider(), v);
                    }));

            lua.new_usertype<LuaCapsuleCollider>("CapsuleCollider",
                sol::no_constructor,
                "GetRadius", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    return go->getCapsuleCollider()->getRadius();
                },
                "SetRadius", [&mgr](const LuaCapsuleCollider& c, float r) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetRadius", r)) return;
                    go->getCapsuleCollider()->setRadius(r);
                },
                "GetHalfHeight", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    return go->getCapsuleCollider()->getHalfHeight();
                },
                "SetHalfHeight", [&mgr](const LuaCapsuleCollider& c, float h) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetHalfHeight", h)) return;
                    go->getCapsuleCollider()->setHalfHeight(h);
                },
                "GetCenter", [](const LuaCapsuleCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    return go->getCapsuleCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaCapsuleCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                    if (!ensureFinite(mgr, "CapsuleCollider.SetCenter", ctr)) return;
                    go->getCapsuleCollider()->setCenter(ctr);
                },
                // Physics material of the collider; see the note in BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        return go->getCapsuleCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.staticFriction", v)) return;
                        go->getCapsuleCollider()->setFriction(v, go->getCapsuleCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        return go->getCapsuleCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.dynamicFriction", v)) return;
                        go->getCapsuleCollider()->setFriction(go->getCapsuleCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        return go->getCapsuleCollider()->getBounciness();
                    },
                    [&mgr](const LuaCapsuleCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        if (!ensureFinite(mgr, "CapsuleCollider.bounciness", v)) return;
                        go->getCapsuleCollider()->setBounciness(v);
                    }),
                // Collision layer; see the note in BoxCollider.
                "layer", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        return go->getCapsuleCollider()->getLayer();
                    },
                    [](const LuaCapsuleCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        requireLayer("CapsuleCollider.layer", v);
                        go->getCapsuleCollider()->setLayer(v);
                    }),
                // Is Trigger; see the note in BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaCapsuleCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        return go->getCapsuleCollider()->isTrigger();
                    },
                    [&mgr](const LuaCapsuleCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasCapsuleCollider()) throw std::runtime_error("The GameObject no longer has a Capsule Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getCapsuleCollider(), v);
                    }));

            lua.new_usertype<LuaPlaneCollider>("PlaneCollider",
                sol::no_constructor,
                "GetCenter", [](const LuaPlaneCollider& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                    return go->getPlaneCollider()->getCenter();
                },
                "SetCenter", [&mgr](const LuaPlaneCollider& c, const glm::vec3& ctr) {
                    GameObject* go = deref(c.e);
                    if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                    if (!ensureFinite(mgr, "PlaneCollider.SetCenter", ctr)) return;
                    go->getPlaneCollider()->setCenter(ctr);
                },
                // Physics material of the collider; see the note in BoxCollider.
                "staticFriction", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        return go->getPlaneCollider()->getStaticFriction();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.staticFriction", v)) return;
                        go->getPlaneCollider()->setFriction(v, go->getPlaneCollider()->getDynamicFriction());
                    }),
                "dynamicFriction", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        return go->getPlaneCollider()->getDynamicFriction();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.dynamicFriction", v)) return;
                        go->getPlaneCollider()->setFriction(go->getPlaneCollider()->getStaticFriction(), v);
                    }),
                "bounciness", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        return go->getPlaneCollider()->getBounciness();
                    },
                    [&mgr](const LuaPlaneCollider& c, float v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        if (!ensureFinite(mgr, "PlaneCollider.bounciness", v)) return;
                        go->getPlaneCollider()->setBounciness(v);
                    }),
                // Collision layer; see the note in BoxCollider.
                "layer", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        return go->getPlaneCollider()->getLayer();
                    },
                    [](const LuaPlaneCollider& c, int v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        requireLayer("PlaneCollider.layer", v);
                        go->getPlaneCollider()->setLayer(v);
                    }),
                // Is Trigger; see the note in BoxCollider.
                "isTrigger", sol::property(
                    [](const LuaPlaneCollider& c) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        return go->getPlaneCollider()->isTrigger();
                    },
                    [&mgr](const LuaPlaneCollider& c, bool v) {
                        GameObject* go = deref(c.e);
                        if (!go->hasPlaneCollider()) throw std::runtime_error("The GameObject no longer has a Plane Collider");
                        PhysicsManager* pm = mgr.physics();
                        if (!pm) return;
                        pm->setTrigger(go->getPlaneCollider(), v);
                    }));

            lua.new_usertype<LuaAudioClip>("AudioClip",
                sol::no_constructor,
                "Play", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
                },
                "Stop", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->stop();
                },
                // It OVERLAPS with whatever is already playing, unlike Play, which cuts the
                // previous voice of the same clip. This is what keeps two footsteps or
                // two consecutive shots from stepping on each other. The voice it fires is out
                // of reach: Stop, SetVolume and IsPlaying do not see it, and it does not
                // follow the object. For short clips, never for loops.
                "PlayOneShot", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->playOneShot(glm::vec3(go->worldTransform[3]));
                },
                "SetLoop", [](const LuaAudioClip& c, bool l) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->setLoop(l);
                },
                "GetLoop", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getLoop();
                },
                "SetVolume", [&mgr](const LuaAudioClip& c, float v) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetVolume", v)) return;
                    go->getAudioClip()->setVolume(v);
                },
                "GetVolume", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getVolume();
                },
                "SetPitch", [&mgr](const LuaAudioClip& c, float p) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetPitch", p)) return;
                    go->getAudioClip()->setPitch(p);
                },
                "GetPitch", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getPitch();
                },
                // Careful: setIs3D RELOADS the sound (unloadSound + loadSound
                // because is3D is baked into the FMOD_MODE) and cuts whatever was
                // playing. It is configuration, not something to call per
                // frame, unlike SetVolume/SetPitch.
                "SetIs3D", [](const LuaAudioClip& c, bool b) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->setIs3D(b);
                },
                "GetIs3D", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getIs3D();
                },
                // 3D attenuation distances. They were in the component and in the
                // Inspector from the start, but not in Lua: a script could not,
                // for example, widen an engine's radius when accelerating.
                // Like SetVolume/SetPitch, they do not reload the sound. The clamp and the
                // min <= max invariant are enforced by the component.
                "SetMinDistance", [&mgr](const LuaAudioClip& c, float d) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetMinDistance", d)) return;
                    go->getAudioClip()->setMinDistance(d);
                },
                "GetMinDistance", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getMinDistance();
                },
                "SetMaxDistance", [&mgr](const LuaAudioClip& c, float d) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetMaxDistance", d)) return;
                    go->getAudioClip()->setMaxDistance(d);
                },
                "GetMaxDistance", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getMaxDistance();
                },
                "SetPlayOnAwake", [](const LuaAudioClip& c, bool b) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->setPlayOnAwake(b);
                },
                "GetPlayOnAwake", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getPlayOnAwake();
                },
                // Bus by NAME ("master"/"music"/"sfx"), not by index: it is the
                // same thing that is stored in the scene, and a magic number in a
                // script would be unreadable. An unknown name warns and does not
                // change anything, instead of falling back to an arbitrary bus.
                "SetBus", [&mgr](const LuaAudioClip& c, const std::string& name) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    AudioBus bus;
                    if (!audioBusFromStr(name, bus))
                    {
                        mgr.log("[Lua][WARN] AudioClip.SetBus: unknown bus '" + name +
                                 "' (use 'master', 'music' or 'sfx'), the previous one is kept");
                        return;
                    }
                    go->getAudioClip()->setBus(bus);
                },
                "GetBus", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return std::string(audioBusToStr(go->getAudioClip()->getBus()));
                },
                // Load mode by name ("sample"/"stream"), like the bus.
                // CAREFUL: it reloads the sound and cuts whatever is playing. It is startup
                // configuration, not something to call per frame.
                "SetLoadMode", [&mgr](const LuaAudioClip& c, const std::string& name) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    AudioLoadMode mode;
                    if (!audioLoadModeFromStr(name, mode))
                    {
                        mgr.log("[Lua][WARN] AudioClip.SetLoadMode: unknown mode '" + name +
                                 "' (use 'sample' or 'stream'), the previous one is kept");
                        return;
                    }
                    go->getAudioClip()->setLoadMode(mode);
                },
                "GetLoadMode", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return std::string(audioLoadModeToStr(go->getAudioClip()->getLoadMode()));
                },
                // Attenuation curve by name. Like SetLoadMode, it RELOADS the
                // sound: it is configuration, not something to touch per frame.
                "SetRolloff", [&mgr](const LuaAudioClip& c, const std::string& name) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    AudioRolloff r;
                    if (!audioRolloffFromStr(name, r))
                    {
                        mgr.log("[Lua][WARN] AudioClip.SetRolloff: unknown curve '" + name +
                                 "' (use 'inverse', 'linear' or 'linearSquare'), the previous one is kept");
                        return;
                    }
                    go->getAudioClip()->setRolloff(r);
                },
                "GetRolloff", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return std::string(audioRolloffToStr(go->getAudioClip()->getRolloff()));
                },
                // The three of the voice: they do not reload, but they are read when playback
                // starts, so changing them while something is playing has no effect
                // until the next Play.
                "SetSpread", [&mgr](const LuaAudioClip& c, float d) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetSpread", d)) return;
                    go->getAudioClip()->setSpread(d);
                },
                "GetSpread", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getSpread();
                },
                "SetStereoPan", [&mgr](const LuaAudioClip& c, float p) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetStereoPan", p)) return;
                    go->getAudioClip()->setStereoPan(p);
                },
                "GetStereoPan", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getStereoPan();
                },
                "SetDopplerLevel", [&mgr](const LuaAudioClip& c, float l) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetDopplerLevel", l)) return;
                    go->getAudioClip()->setDopplerLevel(l);
                },
                "GetDopplerLevel", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getDopplerLevel();
                },
                // Mute: silence without losing the volume. Unlike Pause,
                // this IS serialized, so an object can be born muted.
                "SetMute", [](const LuaAudioClip& c, bool m) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->setMute(m);
                },
                "GetMute", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getMute();
                },
                // Playback position in seconds. GetTime returns -1 if
                // nothing is playing: 0 would be the start of the clip, which is
                // a different answer.
                "GetTime", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getTime();
                },
                "SetTime", [&mgr](const LuaAudioClip& c, float t) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    if (!ensureFinite(mgr, "AudioClip.SetTime", t)) return;
                    go->getAudioClip()->setTime(t);
                },
                "GetPath", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->getPath();
                },
                // State of the VOICE, not of the component. IsPlaying follows the
                // criterion of FMOD and Unity: a paused voice counts as
                // playing, and IsPaused is what tells them apart. Without this a script
                // had no way to wait for a sound to finish.
                "IsPlaying", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->isPlaying();
                },
                "IsPaused", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    return go->getAudioClip()->isPaused();
                },
                // Pause keeps the playback position; Stop discards it.
                "Pause", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->pause();
                },
                "Resume", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("The GameObject no longer has an AudioClip");
                    go->getAudioClip()->resume();
                });

            // Rigidbody: Unity-style dynamics. Properties (mass/useGravity/
            // isKinematic/drag/angularDrag/constraints/velocity/angularVelocity)
            // + methods AddForce/AddTorque/AddImpulse. Obtained with
            // GetComponent("Rigidbody").
            //
            // The 6 valid bits of the constraints bitmask (Rigidbody.h). Anything
            // else that arrives from Lua is cut against this mask.
            constexpr uint32_t kRigidbodyConstraintsMask =
                RB_FreezePositionX | RB_FreezePositionY | RB_FreezePositionZ |
                RB_FreezeRotationX | RB_FreezeRotationY | RB_FreezeRotationZ;
            auto rbOf = [](const LuaRigidbody& c) -> Rigidbody* {
                GameObject* go = deref(c.e);
                if (!go->hasRigidbody()) throw std::runtime_error("The GameObject no longer has a Rigidbody");
                return go->getRigidbody().get();
            };
            lua.new_usertype<LuaRigidbody>("Rigidbody",
                sol::no_constructor,
                "mass", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getMass(); },
                    [rbOf, &mgr](const LuaRigidbody& c, float v) {
                        Rigidbody* rb = rbOf(c); // deref + hasRigidbody BEFORE the guard (finding 4)
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
                // constraints is a BITMASK (RigidbodyConstraints table), not a
                // float: no ensureFinite here. Bits that are not
                // defined in Rigidbody.h are masked out silently instead of
                // throwing: an extra OR must not bring the script down.
                "constraints", sol::property(
                    [rbOf](const LuaRigidbody& c) { return rbOf(c)->getConstraints(); },
                    [rbOf](const LuaRigidbody& c, uint32_t v) {
                        Rigidbody* rb = rbOf(c);
                        rb->setConstraints(v & kRigidbodyConstraintsMask);
                    }),
                // ccd/interpolate: two booleans independent of each other and
                // off by default. No ensureFinite (they are not floats) and no
                // masking (they are not a bitmask): a Lua bool is always valid.
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
                // The 4th argument (mode) is OPTIONAL: without it ForceMode.Force
                // is applied, that is, the same as the three-argument calls
                // always did. An out-of-range mode is reported through the Log and applies
                // NO force, like a NaN: a badly computed index in a script
                // must not bring the game down.
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

            // Constants of the Rigidbody.constraints bitmask. They are combined with
            // the bitwise OR of Lua 5.3+ (rb.constraints = RigidbodyConstraints.
            // FreezePositionX | RigidbodyConstraints.FreezeRotationY).
            sol::table rbc = lua.create_named_table("RigidbodyConstraints");
            rbc["None"]            = RB_None;
            rbc["FreezePositionX"] = RB_FreezePositionX;
            rbc["FreezePositionY"] = RB_FreezePositionY;
            rbc["FreezePositionZ"] = RB_FreezePositionZ;
            rbc["FreezeRotationX"] = RB_FreezeRotationX;
            rbc["FreezeRotationY"] = RB_FreezeRotationY;
            rbc["FreezeRotationZ"] = RB_FreezeRotationZ;

            // AddForce/AddTorque modes (optional 4th argument). The values
            // are the indices of the ForceMode enum in Rigidbody.h, in that order.
            sol::table fm = lua.create_named_table("ForceMode");
            fm["Force"]          = static_cast<int>(ForceMode::Force);
            fm["Acceleration"]   = static_cast<int>(ForceMode::Acceleration);
            fm["Impulse"]        = static_cast<int>(ForceMode::Impulse);
            fm["VelocityChange"] = static_cast<int>(ForceMode::VelocityChange);

            // Animator: animation state machine. Obtained with
            // GetComponent("Animator"). No properties: the parameters are
            // declared in the graph and queried by name, they are not fields.
            auto animOf = [](const LuaAnimator& c) -> AnimatorComponent* {
                GameObject* go = deref(c.e);
                if (!go->hasAnimator()) throw std::runtime_error("The GameObject no longer has an Animator");
                return go->getAnimator().get();
            };
            // Optional layer of the per-layer calls: with no argument, the base one.
            // Out of range returns -1 and the call does nothing (instead of
            // falling into another layer, which is what the component's clamping would do).
            auto capaDe = [](AnimatorComponent* a, sol::optional<int> capa) {
                const int li = capa.value_or(0);
                return (li >= 0 && li < a->layerCount()) ? li : -1;
            };
            lua.new_usertype<LuaAnimator>("Animator",
                sol::no_constructor,
                "SetBool",    [animOf](const LuaAnimator& c, const std::string& n, bool v) { animOf(c)->setBool(n, v); },
                "GetBool",    [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getBool(n); },
                "SetTrigger", [animOf](const LuaAnimator& c, const std::string& n) { animOf(c)->setTrigger(n); },
                "ResetTrigger", [animOf](const LuaAnimator& c, const std::string& n) { animOf(c)->resetTrigger(n); },
                // Direct control of the graph. A state that does not exist returns
                // false and leaves a warning in the log, without throwing: like the
                // parameters, a misspelled name does not bring the script down.
                "Play", [animOf, capaDe, &mgr](const LuaAnimator& c, const std::string& estado, sol::optional<int> capa) {
                    AnimatorComponent* anim = animOf(c);
                    const int li = capaDe(anim, capa);
                    if (li < 0) { mgr.log("[Lua][WARN] Animator.Play: there is no layer " + std::to_string(capa.value_or(0))); return false; }
                    const bool ok = anim->play(estado, li);
                    if (!ok) mgr.log("[Lua][WARN] Animator.Play: there is no state '" + estado + "'");
                    return ok;
                },
                "CrossFade", [animOf, capaDe, &mgr](const LuaAnimator& c, const std::string& estado, float segundos,
                                                    sol::optional<int> capa) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.CrossFade", segundos)) return false;
                    const int li = capaDe(anim, capa);
                    if (li < 0) { mgr.log("[Lua][WARN] Animator.CrossFade: there is no layer " + std::to_string(capa.value_or(0))); return false; }
                    const bool ok = anim->crossFade(estado, segundos, li);
                    if (!ok) mgr.log("[Lua][WARN] Animator.CrossFade: there is no state '" + estado + "'");
                    return ok;
                },
                "GetNormalizedTime", [animOf, capaDe](const LuaAnimator& c, sol::optional<int> capa) {
                    AnimatorComponent* anim = animOf(c);
                    const int li = capaDe(anim, capa);
                    return li < 0 ? 0.0f : anim->normalizedTime(li);
                },
                // Layers: the weight of the upper ones is driven from here (the base
                // is always 1); mode and mask are graph authoring.
                "SetLayerWeight", [animOf, &mgr](const LuaAnimator& c, int capa, float peso) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.SetLayerWeight", peso)) return;
                    anim->setLayerWeight(capa, peso);
                },
                "GetLayerWeight", [animOf](const LuaAnimator& c, int capa) {
                    AnimatorComponent* anim = animOf(c);
                    return (capa >= 0 && capa < anim->layerCount()) ? anim->layerWeight(capa) : 0.0f;
                },
                "GetLayerCount", [animOf](const LuaAnimator& c) { return animOf(c)->layerCount(); },
                // IK: the weight and the targets are driven from the game; the
                // bone, the type and the axis are graph authoring (Animator panel).
                "SetIkWeight", [animOf, &mgr](const LuaAnimator& c, const std::string& n, float peso) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.SetIkWeight", peso)) return;
                    anim->setIkWeight(n, peso);
                },
                "GetIkWeight", [animOf](const LuaAnimator& c, const std::string& n) {
                    return animOf(c)->ikWeight(n);
                },
                // The entity can be nil: that removes the target and the
                // constraint stops being applied.
                "SetIkTarget", [animOf](const LuaAnimator& c, const std::string& n, sol::optional<LuaEntity> e) {
                    animOf(c)->setIkTarget(n, (e && e->go) ? e->go->id : 0);
                },
                "SetIkPole", [animOf](const LuaAnimator& c, const std::string& n, sol::optional<LuaEntity> e) {
                    animOf(c)->setIkPole(n, (e && e->go) ? e->go->id : 0);
                },
                "GetIkCount", [animOf](const LuaAnimator& c) { return (int)animOf(c)->ikConstraints().size(); },
                // Global speed of the Animator (runtime, not saved). The
                // per-state speed is graph authoring: it is driven with
                // SetFloat on its multiplier parameter.
                "SetSpeed", [animOf, &mgr](const LuaAnimator& c, float v) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.SetSpeed", v)) return;
                    anim->setSpeed(v);
                },
                "GetSpeed", [animOf](const LuaAnimator& c) { return animOf(c)->speed(); },
                // Numeric: same contract as the bools. An undeclared name
                // (or one of another type) is ignored in the setter and returns 0
                // in the getter, it never throws.
                "SetInt",     [animOf](const LuaAnimator& c, const std::string& n, int v) { animOf(c)->setInt(n, v); },
                "GetInt",     [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getInt(n); },
                "SetFloat",   [animOf, &mgr](const LuaAnimator& c, const std::string& n, float v) {
                    AnimatorComponent* anim = animOf(c);
                    if (!ensureFinite(mgr, "Animator.SetFloat", v)) return;
                    anim->setFloat(n, v);
                },
                "GetFloat",   [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getFloat(n); },
                "GetState",   [animOf, capaDe](const LuaAnimator& c, sol::optional<int> capa) {
                    AnimatorComponent* anim = animOf(c);
                    const int li = capaDe(anim, capa);
                    return li < 0 ? std::string() : anim->currentStateName(li);
                },
                // Cross-fade in progress. They are READ-ONLY: the blend duration
                // is graph authoring (edited in the Animator panel), like
                // the conditions of a transition.
                "IsBlending",     [animOf, capaDe](const LuaAnimator& c, sol::optional<int> capa) {
                    AnimatorComponent* anim = animOf(c);
                    const int li = capaDe(anim, capa);
                    return li >= 0 && anim->fading(li);
                },
                "GetBlendWeight", [animOf](const LuaAnimator& c) { return animOf(c)->blendWeight(); },
                "GetPreviousState", [animOf](const LuaAnimator& c) { return animOf(c)->previousStateName(); },
                // The weight that ends up going to the GPU: the cross-fade's if there is
                // one in flight, otherwise the state's per-parameter blend, and
                // 1 if there is no blend at all. The per-parameter blend is DRIVEN
                // with SetFloat on its parameter, so here it is only read.
                "GetPoseWeight", [animOf](const LuaAnimator& c) { return animOf(c)->poseWeight(); });
        }

        // ── UI: Canvas, Button, Text and ProgressBar ──────────────────────────
        //
        // All four are scene DATA ONLY and whoever draws them is
        // syncUiWidgets, which every frame dumps the component onto the live canvas
        // node. That is why the setters ALWAYS write to the component and
        // never to the node: a write to the node would be erased by the next
        // dump. Nothing is marked dirty by hand either, since the sync compares with its
        // own snapshot and already knows what has changed.
        //
        // Per-access resolution (deref + has*) like the rest of the
        // components: a wrapper stored in a Lua variable cannot
        // hold a pointer that another frame has freed.
        CanvasComponent* canvasOf(const LuaCanvas& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasCanvas()) throw std::runtime_error("The GameObject no longer has a Canvas");
            return go->getCanvas().get();
        }
        ButtonComponent* buttonOf(const LuaButton& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasButton()) throw std::runtime_error("The GameObject no longer has a Button");
            return go->getButton().get();
        }
        TextComponent* textOf(const LuaText& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasText()) throw std::runtime_error("The GameObject no longer has a Text");
            return go->getText().get();
        }
        ProgressBarComponent* barOf(const LuaProgressBar& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasProgressBar()) throw std::runtime_error("The GameObject no longer has a ProgressBar");
            return go->getProgressBar().get();
        }
        LayoutComponent* layoutOf(const LuaLayout& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasLayout()) throw std::runtime_error("The GameObject no longer has a Layout");
            return go->getLayout().get();
        }
        PanelComponent* panelOf(const LuaPanel& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasPanel()) throw std::runtime_error("The GameObject no longer has a Panel");
            return go->getPanel().get();
        }
        ImageComponent* imageOf(const LuaImage& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasImage()) throw std::runtime_error("The GameObject no longer has an Image");
            return go->getImage().get();
        }
        SliderComponent* sliderOf(const LuaSlider& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasSlider()) throw std::runtime_error("The GameObject no longer has a Slider");
            return go->getSlider().get();
        }
        CheckboxComponent* checkboxOf(const LuaCheckbox& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasCheckbox()) throw std::runtime_error("The GameObject no longer has a Checkbox");
            return go->getCheckbox().get();
        }
        ToggleComponent* toggleOf(const LuaToggle& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasToggle()) throw std::runtime_error("The GameObject no longer has a Toggle");
            return go->getToggle().get();
        }
        ScrollbarComponent* scrollbarOf(const LuaScrollbar& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasScrollbar()) throw std::runtime_error("The GameObject no longer has a Scrollbar");
            return go->getScrollbar().get();
        }
        InputFieldComponent* inputFieldOf(const LuaInputField& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasInputField()) throw std::runtime_error("The GameObject no longer has an InputField");
            return go->getInputField().get();
        }
        DropdownComponent* dropdownOf(const LuaDropdown& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasDropdown()) throw std::runtime_error("The GameObject no longer has a Dropdown");
            return go->getDropdown().get();
        }
        ScrollViewComponent* scrollViewOf(const LuaScrollView& c)
        {
            GameObject* go = deref(c.e);
            if (!go->hasScrollView()) throw std::runtime_error("The GameObject no longer has a ScrollView");
            return go->getScrollView().get();
        }

        // Accessor factories. They are templates and not a hand-written list of lambdas
        // because the four components add up to more than a hundred fields and writing the
        // get/set pair of each one would multiply tenfold the chances of
        // typing the wrong field on one side of the pair.
        //
        // The resolver comes in as a FUNCTION POINTER (that is why the four above
        // capture nothing): this way the accessor can be copied inside the
        // lambdas without dragging state along.
        template <class W, class Comp, class T>
        auto uiProp(Comp* (*res)(const W&), T Comp::*campo)
        {
            return sol::property(
                [res, campo](const W& w) -> T { return res(w)->*campo; },
                [res, campo](const W& w, T v) { res(w)->*campo = v; });
        }

        // Same, but going through the NaN/Inf filter: a broken computation in a
        // script cannot leave a UI field with a value that blows up the
        // layout (same contract as Transform.SetPosition).
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

        // Enums travel as INTEGERS (the UiTextAlign, UiButtonState and
        // similar tables that registerUi registers). An out-of-range value is ignored:
        // converting it to an enum as is would put an impossible value in the component
        // and the sync's switch would fall into the default without anyone noticing.
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

        // Vectors as METHODS and not as properties: in Lua there is no vec2 or
        // vec4 (only Vec3), and returning a new table per read would create garbage
        // on every frame of every script. They return multiple values, which is the
        // natural form in Lua: local x, y = b:GetPosition().
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

        // The Lua functions of the UI callbacks live in THIS table of the
        // lua_State itself, referenced by an integer key, and what is
        // stored in the component is a std::function that goes to fetch them.
        //
        // Storing the sol::protected_function inside the component would put
        // a reference to the Lua registry in an object that OUTLIVES the
        // lua_State: its destructor would do luaL_unref on an already closed state.
        // This way the component stores nothing from Lua.
        constexpr const char* kUiCallbackTable = "__uiCallbacks";
        long long g_nextUiCallbackKey = 0;

        void setUiCallback(ScriptManager& mgr, std::function<void()>& destino,
                           const char* nombre, const sol::object& fn)
        {
            if (!fn.valid() || fn.get_type() != sol::type::function)
            {
                destino = nullptr;   // passing nil (or anything else) removes it
                return;
            }

            sol::state_view lua(mgr.lua());
            sol::table tabla = lua[kUiCallbackTable];
            const long long clave = ++g_nextUiCallbackKey;
            tabla[clave] = fn;

            // The epoch is what prevents calling a dead lua_State: it expires when
            // the ScriptManager is destroyed and on hot-reloading a script.
            // It is checked BEFORE touching mgr, which by then may also
            // be dead.
            std::weak_ptr<char> epoca = mgr.callbackEpoch();
            ScriptManager* m = &mgr;
            const std::string etiqueta = nombre;
            destino = [m, clave, epoca, etiqueta]() {
                if (epoca.expired()) return;

                sol::state_view lua(m->lua());
                sol::table tabla = lua[kUiCallbackTable];
                sol::object f = tabla[clave];
                if (f.get_type() != sol::type::function) return;

                // protected_function: an error inside the callback is logged
                // and execution continues. A button with a broken script cannot bring down the
                // frame nor swallow the rest of the UI.
                sol::protected_function pf = f;
                sol::protected_function_result r = pf();
                if (!r.valid())
                {
                    sol::error err = r;
                    m->log(std::string("[Lua][ERROR] ") + etiqueta + ": " + err.what());
                }
            };
        }

        // Same as setUiCallback but for the handlers that carry a VALUE (the
        // new one of the slider, checkbox, toggle, bar). It is a separate
        // template and not a generalization of that one because the
        // Button's OnClick/OnDoubleClick takes no argument and its signature has
        // no reason to change.
        template <class... Args>
        void setUiValueCallback(ScriptManager& mgr, std::function<void(Args...)>& destino,
                                const char* nombre, const sol::object& fn)
        {
            if (!fn.valid() || fn.get_type() != sol::type::function)
            {
                destino = nullptr;   // passing nil (or anything else) removes it
                return;
            }

            sol::state_view lua(mgr.lua());
            sol::table tabla = lua[kUiCallbackTable];
            const long long clave = ++g_nextUiCallbackKey;
            tabla[clave] = fn;

            // Same epoch as setUiCallback: it is what prevents calling a dead
            // lua_State after destroying the ScriptManager or hot-reloading.
            // It is checked BEFORE touching mgr.
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

            // Enums as tables of integers: they are the SAME values as in C++
            // (the order of the enum class), so UiTextAlign.Center is worth
            // what UiTextAlign::Center is.
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
                // The safe area is four separate insets (not a vec4): they are passed
                // in the same order in which UiSafeArea declares them.
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
                // State: the canvas writes it on the live node and the sync
                // publishes it to the component. Read-only, as in C++.
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
                // Same as normalizedValue() in C++: the already clamped 0..1 that
                // the sync uses for the fill rect.
                "GetNormalizedValue", [](const LuaProgressBar& b) {
                    return barOf(b)->normalizedValue();
                });

            // ── Layout ──────────────────────────────────────────────────────
            // The only one of the four that does not draw: it positions. That is why it has neither
            // color nor sprite, but does have the mode, the padding and the cell.
            lua.new_usertype<LuaLayout>("Layout",
                sol::no_constructor,
                "visible",       uiProp(layoutOf, &LayoutComponent::visible),
                "mode",          uiEnumProp(layoutOf, &LayoutComponent::mode, 3),
                "crossAlign",    uiEnumProp(layoutOf, &LayoutComponent::crossAlign, 2),
                "paddingLeft",   uiFloatProp(layoutOf, &LayoutComponent::paddingLeft, &mgr, "Layout.paddingLeft"),
                "paddingRight",  uiFloatProp(layoutOf, &LayoutComponent::paddingRight, &mgr, "Layout.paddingRight"),
                "paddingTop",    uiFloatProp(layoutOf, &LayoutComponent::paddingTop, &mgr, "Layout.paddingTop"),
                "paddingBottom", uiFloatProp(layoutOf, &LayoutComponent::paddingBottom, &mgr, "Layout.paddingBottom"),
                // columns is an integer: without uiFloatProp's NaN guard rail and
                // without decimals to round behind the script's back.
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
            // The background rectangle. No fields of its own beyond the rect, the
            // color and the sprite: the core's Panel does not have them either.
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
            // With the NINE fields of the core widget's own: the mode, the
            // four 9-slice borders with their fillCenter, the tile cap and the
            // Filled block.
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
                // maxTiles is an integer: without uiFloatProp's NaN guard rail and
                // without decimals to round behind the script's back.
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
            // The first of the interactive ones: what the player moves is written
            // to the COMPONENT, so reading `value` here gives the real value
            // without polling the canvas node.
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
                // Same as normalizedValue() in C++: the already clamped 0..1.
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
            // No `color`: the track is painted by the sync with offColor or onColor depending on
            // the state, so a loose color field would be one that the first
            // dump overwrites and that seems to do nothing.
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
                // numberOfSteps is an integer: without uiFloatProp's NaN guard rail and
                // without decimals to round behind the script's back.
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
                // The same snap to discrete stops that dragging applies.
                "SnapValue", [](const LuaScrollbar& s, float v) {
                    return scrollbarOf(s)->snapValue(v);
                },
                "OnValueChanged", [&mgr](const LuaScrollbar& s, sol::object fn) {
                    setUiValueCallback<float>(mgr, scrollbarOf(s)->callbacks.ptr->onValueChanged,
                                              "Scrollbar.OnValueChanged", fn);
                });

            // ── InputField ──────────────────────────────────────────────────
            // The only one in which the PLAYER types. `text` is the real text:
            // in Password it is stored as is and only what is SHOWN changes,
            // which is what GetDisplayText returns.
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
                // characterLimit is an integer: without the NaN guard rail and without
                // decimals to round behind the script's back.
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
                // What is DRAWN: the placeholder if empty, or the mask if
                // it is Password. Never the password.
                "GetDisplayText", [](const LuaInputField& f) {
                    return inputFieldOf(f)->displayText();
                },
                // Cursor position in CHARACTERS (not bytes), 0 = before the
                // first. It is clamped on write: a cursor outside the text would
                // split the string on the next delete.
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
            // The options are read and written with 1-BASED indices, which is
            // natural in Lua; `value` is still the 0-based index of the
            // component, as in C++ and in the inspector.
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
                // 1-based and out of range returns an empty string: a bad index
                // cannot bring down a menu's script.
                "GetOption", [](const LuaDropdown& d, int i) {
                    DropdownComponent* c = dropdownOf(d);
                    if (i < 1 || i > (int)c->options.size()) return std::string();
                    return c->options[(size_t)(i - 1)];
                },
                "GetSelectedLabel", [](const LuaDropdown& d) {
                    return dropdownOf(d)->selectedLabel();
                },
                // Replaces the whole list. Anything that is not a string is DISCARDED
                // instead of throwing away the table: losing the combo because of one bad entry
                // would be worse than losing that entry.
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
            // No reference to a Scrollbar: linking them is one line of script
            // (bar:OnValueChanged -> view:SetNormalizedPosition), and a
            // reference between scene components would have to be serialized
            // and kept alive across clone, undo and delete.
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
                // How much it can scroll per axis, in pixels. A disabled axis
                // gives 0 even if the content is larger.
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
                // Hides or shows the mesh without destroying anything: the object stays
                // alive, keeps colliding and its scripts keep running, it just
                // stops being drawn. It is the "SetActive of the visible" that every
                // game needs and that until now could only be touched from the
                // inspector. An object without a mesh accepts the flag anyway (it is not an
                // error): if one is added later, it is born with this visibility.
                "meshVisible", sol::property(
                    [](const LuaEntity& e) { return deref(e)->meshVisible; },
                    [](const LuaEntity& e, bool v) { deref(e)->meshVisible = v; }),
                "IsValid", [](const LuaEntity& e) {
                    return e.go && e.mgr && e.mgr->isAlive(e.go);
                },
                // Light and camera: same named shortcuts as the UI. The Gets
                // return nil if the component is not there (checking it is the
                // first thing a script does) and the Adds are idempotent.
                // Changes parent. With no argument (or with nil) it hangs it from the
                // scene root. The second argument decides what is
                // kept: by default the WORLD pose, like Unity's
                // transform.parent: the object stays where it is and what
                // is recomputed is its local transform. With false the LOCAL is kept
                // and the object jumps along with its new parent, which is what
                // dragging in the editor hierarchy does.
                //
                // Returns false, touching nothing, if the destination is inside its
                // own subtree: that would detach the subtree from the tree.
                "SetParent", [&mgr](const LuaEntity& e, sol::optional<LuaEntity> parent,
                                    sol::optional<bool> keepWorld) -> bool {
                    GameObject* go = deref(e);
                    if (!mgr.scene()) return false;
                    // deref of the parent BEFORE anything else: an already destroyed parent has
                    // to give a Lua error like in any other method, not a silent
                    // false (same criterion as finding 4 of
                    // ensureFinite).
                    GameObject* newParent = parent ? deref(*parent) : nullptr;

                    const bool mantenerMundo = keepWorld.value_or(true);
                    const glm::mat4 mundoAntes = go->worldTransform;

                    if (!mgr.scene()->reparent(go, newParent))
                    {
                        mgr.log("[Lua][WARN] Entity:SetParent: invalid target "
                                "(the root, or a descendant of the object itself)");
                        return false;
                    }

                    if (mantenerMundo)
                    {
                        const glm::mat4 padreMundo =
                            go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                        const glm::mat4 nuevoLocal = glm::inverse(padreMundo) * mundoAntes;
                        // A parent with scale 0 gives a singular matrix and its
                        // inverse is infinities. The existing local is kept
                        // (the object jumps, but it is still a valid
                        // transform) instead of baking NaN into the matrix, which
                        // would take all the children down with it.
                        bool finito = true;
                        for (int c = 0; c < 4 && finito; ++c)
                            for (int r = 0; r < 4 && finito; ++r)
                                if (!std::isfinite(nuevoLocal[c][r])) finito = false;
                        if (finito)
                            go->localTransform = nuevoLocal;
                        else
                            mgr.log("[Lua][WARN] Entity:SetParent: the parent has a "
                                    "degenerate transform, the local pose is kept");
                    }

                    // The subtree's world matrices are recomputed right now, not on the next
                    // frame: a script that calls GetWorldPosition on the next line
                    // would read the old position.
                    go->updateWorldTransforms(
                        go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
                    return true;
                },
                "GetLight", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasLight()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaLight{e});
                },
                "AddLight", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasLight()) go->setLight(std::make_shared<LightComponent>());
                    return LuaLight{e};
                },
                "RemoveLight", [](const LuaEntity& e) { deref(e)->setLight(nullptr); },
                "GetCamera", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->hasCameraComponent()) return sol::nil;
                    return sol::make_object(e.mgr->lua(), LuaCamera{e});
                },
                "AddCamera", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    if (!go->hasCameraComponent())
                        go->setCameraComponent(std::make_shared<CameraComponent>());
                    return LuaCamera{e};
                },
                "RemoveCamera", [](const LuaEntity& e) { deref(e)->setCameraComponent(nullptr); },
                "GetTransform", [](const LuaEntity& e) { deref(e); return LuaTransform{e}; },
                // UI: named shortcuts for the four components, in addition to the usual
                // GetComponent("Button"). The getter returns nil if
                // the component is not there (checking it is the first thing a
                // UI script does, and a Lua error is not acceptable as an answer), and the
                // Add returns the wrapper, which is ready to chain.
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
                    if (!go->parent || !go->parent->parent) return sol::nil; // root is not exposed
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
                    if (name == "ReverbZone"      && go->hasReverbZone())      return sol::make_object(lua, LuaReverbZone{e});
                    if (name == "Rigidbody"       && go->hasRigidbody())       return sol::make_object(lua, LuaRigidbody{e});
                    if (name == "Animator"        && go->hasAnimator())        return sol::make_object(lua, LuaAnimator{e});
                    if (name == "Light"           && go->hasLight())           return sol::make_object(lua, LuaLight{e});
                    if (name == "Camera"          && go->hasCameraComponent()) return sol::make_object(lua, LuaCamera{e});
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
                    // Same defaults as EditorUI::drawAddComponentButton;
                    // mutually exclusive colliders, same rule as the UI.
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
                    if (name == "ReverbZone" && !go->hasReverbZone())
                    {
                        go->setReverbZone(std::make_shared<ReverbZoneComponent>());
                        return sol::make_object(lua, LuaReverbZone{e});
                    }
                    if (name == "AudioClip" && !go->hasAudioClip() && mgr->audioManager() && arg)
                    {
                        // Same whitelist as the inspector and scene loading:
                        // without it, a path with any extension still
                        // created the component and the failure was only noticed
                        // as silence (FMOD loads lazily).
                        std::string ext = std::filesystem::path(*arg).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char ch) { return (char)std::tolower(ch); });
                        if (!isSupportedAudioExtension(ext))
                        {
                            mgr->log("[Lua][WARN] AddComponent(\"AudioClip\"): unsupported format '" +
                                      ext + "' (use .wav, .mp3, .ogg or .flac)");
                            return sol::nil;
                        }
                        auto clip = mgr->audioManager()->createAudioClipComponent(*arg, false, false);
                        if (clip) { go->setAudioClip(std::move(clip)); return sol::make_object(lua, LuaAudioClip{e}); }
                    }
                    // Rigidbody: needs a collider that provides the shape and that does not
                    // already exist. attachRigidbody promotes the actor to dynamic.
                    if (name == "Rigidbody" && go->hasAnyCollider() && !go->hasRigidbody() && mgr->physics())
                    {
                        auto rb = std::make_shared<Rigidbody>();
                        go->setRigidbody(rb);
                        if (auto col = go->anyCollider()) mgr->physics()->attachRigidbody(col, rb);
                        return sol::make_object(lua, LuaRigidbody{e});
                    }
                    // Light and camera: pure data, nothing to resolve against
                    // the managers. The camera does NOT check that there is no other one in
                    // the scene: the uniqueness invariant is enforced by
                    // Scene::findCamera by keeping the first one, same as
                    // with the AudioListener.
                    if (name == "Light")
                    {
                        if (!go->hasLight()) go->setLight(std::make_shared<LightComponent>());
                        return sol::make_object(lua, LuaLight{e});
                    }
                    if (name == "Camera")
                    {
                        if (!go->hasCameraComponent())
                            go->setCameraComponent(std::make_shared<CameraComponent>());
                        return sol::make_object(lua, LuaCamera{e});
                    }
                    // UI: no dependencies to resolve (they are DATA ONLY) and no
                    // exclusion gate: all four coexist on the same
                    // GameObject, as in the Properties panel. Asking for one
                    // that is already there returns the existing one, it does not replace it: the
                    // editor's Add does not overwrite what already exists either.
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
                        // The instantiation + Awake/Start of the new comp is
                        // done by the lifecycle on the next update (started
                        // == false gives it away). Task 8.
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
                    else if (name == "ReverbZone")      go->setReverbZone(nullptr);
                    else if (name == "Light")           go->setLight(nullptr);
                    else if (name == "Camera")          go->setCameraComponent(nullptr);
                    // Removing a UI component takes its callbacks with it:
                    // the runtime dies with the component and the node's handler,
                    // which only has a weak_ptr, stops firing.
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
                        // Rebuilds the actor as static before releasing the Rigidbody.
                        if (auto col = go->anyCollider(); col && e.mgr && e.mgr->physics())
                            e.mgr->physics()->detachRigidbody(col);
                        go->setRigidbody(nullptr);
                    }
                    else if (name.rfind("Script:", 0) == 0)
                    {
                        // Deferred: the lifecycle processes it at the end of the frame
                        // (removing in the middle of the Update iteration would break
                        // the traversal). Task 8.
                        const std::string scriptName = name.substr(7);
                        for (auto& s : go->getScripts())
                            if (s->scriptName == scriptName) s->pendingRemove = true;
                    }
                });
        }

#ifdef DT_PHYSX_ENABLED
        // The GameObject behind a PhysX actor: actor userData =
        // Collider* (set by PhysicsManager when creating the collider) and the
        // collider's owner = GameObject* (set by Scene when deserializing / by the editor when
        // adding it). Same path TriggerDispatcher uses for the trigger
        // callbacks. nullptr if the actor does not hang from any GameObject.
        GameObject* actorOwner(const physx::PxRigidActor* actor)
        {
            if (!actor) return nullptr;
            auto* col = static_cast<DonTopo::Collider*>(actor->userData);
            return col ? static_cast<GameObject*>(col->getOwner()) : nullptr;
        }

        // Query prefilter. Collider::applyTriggerFlag only turns off
        // eSIMULATION_SHAPE: a trigger's shape keeps eSCENE_QUERY_SHAPE,
        // so without this filter a trigger would block the ray. Here the
        // actor of the GameObject to ignore is also discarded.
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

        // Already validated arguments of Physics.Raycast / Physics.RaycastHit.
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

        // Warn/argAt/given: shared by all the query parsers.
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

        // 'options' table common to ALL the queries (raycast, sweep and
        // overlap): { hitTriggers, static, dynamic, ignore }. Absent or nil =>
        // the RaycastArgs defaults stay. The queries without a ray
        // (overlaps) only use these four fields of the struct.
        bool parseQueryOptions(ScriptManager& mgr, const char* fn,
                               const sol::object& oOpts, RaycastArgs& out)
        {
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            if (!queryGiven(oOpts)) return true;
            if (oOpts.get_type() != sol::type::table)
            {
                warn("options must be a table");
                return false;
            }
            sol::table opts = oOpts.as<sol::table>();

            auto readBool = [&](const char* key, bool& dst) {
                const sol::object v = opts[key];
                if (!queryGiven(v)) return true;
                if (v.get_type() != sol::type::boolean)
                {
                    warn(std::string(key) + " must be a boolean");
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
                    warn("ignore must be an Entity");
                    return false;
                }
                const LuaEntity e = oIgnore.as<LuaEntity>();
                if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
                {
                    warn("ignore points to a destroyed Entity");
                    return false;
                }
                out.ignore = e.go;
            }
            return true;
        }

        // Reads the arguments by hand (sol::variadic_args, not typed parameters)
        // because a wrong type has to return nil and a warning, not sol2's
        // conversion exception, which would bring the script down.
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
                warn("origin and direction must be Vec3");
                return false;
            }
            out.origin = oOrigin.as<glm::vec3>();
            out.dir    = oDir.as<glm::vec3>();

            const sol::object oMax = argAt(2);
            if (given(oMax))
            {
                if (oMax.get_type() != sol::type::number)
                {
                    warn("maxDistance must be a number");
                    return false;
                }
                // Absent or <= 0 -> the default of 1000 stays.
                const float m = oMax.as<float>();
                if (m > 0.0f) out.maxDistance = m;
            }

            return parseQueryOptions(mgr, fn, argAt(3), out);
        }

        // Launches the query. false = no hit, no PhysicsManager (outside
        // Play), degenerate direction or a filter that leaves no actor: in
        // all those cases PhysX is not touched and hit is left unwritten.
        bool doRaycast(ScriptManager& mgr, const RaycastArgs& a, physx::PxRaycastBuffer& hit)
        {
            PhysicsManager* pm = mgr.physics();
            if (!pm) return false;
            if (!a.queryStatic && !a.queryDynamic) return false;
            if (!std::isfinite(a.origin.x) || !std::isfinite(a.origin.y) || !std::isfinite(a.origin.z))
                return false;

            // PhysX requires a unit direction (with an unnormalized one the
            // distance comes out scaled); length 0 or NaN -> nothing to trace.
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

        // Hit ceiling of Physics.RaycastAll. PhysX silently truncates when the
        // buffer fills up (see PxQueryReport.h: "Overflow does not trigger
        // warnings or errors"), so the binding detects it and warns.
        constexpr physx::PxU32 kRaycastAllMaxHits = 64;

        // Same query as doRaycast but multi-hit; the touches come out
        // sorted by distance (done by PhysicsManager::raycastAll).
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

        // Table shape of a hit: { entity, point, normal, distance }.
        // Shared by Physics.Raycast (a single hit) and Physics.RaycastAll (one
        // per array element), so they cannot diverge. entity is omitted if the
        // actor does not hang from any GameObject.
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

        // Query filters common to sweeps and overlaps: the same ones
        // doRaycast builds (ePREFILTER + eSTATIC/eDYNAMIC according to options).
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
        // The radius goes BEFORE maxDistance, so the ray parsing cannot
        // be reused as is (the indices shift); what is reused
        // is the options table.
        bool parseSphereCastArgs(ScriptManager& mgr, sol::variadic_args va,
                                 RaycastArgs& out, float& radius)
        {
            const char* fn = "SphereCast";
            auto warn = [&mgr, fn](const std::string& m) { queryWarn(mgr, fn, m); };

            const sol::object oOrigin = queryArgAt(va, 0);
            const sol::object oDir    = queryArgAt(va, 1);
            if (!oOrigin.is<glm::vec3>() || !oDir.is<glm::vec3>())
            {
                warn("origin and direction must be Vec3");
                return false;
            }
            out.origin = oOrigin.as<glm::vec3>();
            out.dir    = oDir.as<glm::vec3>();

            // queryGiven BEFORE get_type: on a sol::object without a lua_State
            // (an argument that was not passed) get_type dereferences a null pointer.
            const sol::object oRadius = queryArgAt(va, 2);
            if (!queryGiven(oRadius) || oRadius.get_type() != sol::type::number)
            {
                warn("radius must be a number");
                return false;
            }
            // PxSphereGeometry with radius <= 0 (or NaN) is invalid geometry:
            // PhysX complains through its error channel and the query is not valid.
            const float r = oRadius.as<float>();
            if (!std::isfinite(r) || r <= 0.0f)
            {
                warn("radius must be greater than 0");
                return false;
            }
            radius = r;

            const sol::object oMax = queryArgAt(va, 3);
            if (queryGiven(oMax))
            {
                if (oMax.get_type() != sol::type::number)
                {
                    warn("maxDistance must be a number");
                    return false;
                }
                const float m = oMax.as<float>();
                if (m > 0.0f) out.maxDistance = m;
            }

            return parseQueryOptions(mgr, fn, queryArgAt(va, 4), out);
        }

        // Sphere sweep. Same false outputs as doRaycast (no
        // PhysicsManager, a filter that leaves no actors, degenerate origin/direction)
        // and the same direction normalization.
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

        // A sweep's hit table has the same fields as a raycast's,
        // but PxSweepHit and PxRaycastHit are different types: it is
        // filled in separately (same names and same order on purpose).
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

        // Overlap ceiling of Physics.OverlapSphere / OverlapBox. Like
        // kRaycastAllMaxHits: PhysX silently truncates when the buffer fills up
        // (PxQueryReport.h, "Overflow does not trigger warnings or errors"), so
        // the binding detects it with getNbTouches() == getMaxNbTouches().
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
                warn("center must be a Vec3");
                return false;
            }
            center = oCenter.as<glm::vec3>();

            const sol::object oRadius = queryArgAt(va, 1);
            if (!queryGiven(oRadius) || oRadius.get_type() != sol::type::number)
            {
                warn("radius must be a number");
                return false;
            }
            const float r = oRadius.as<float>();
            if (!std::isfinite(r) || r <= 0.0f)
            {
                warn("radius must be greater than 0");
                return false;
            }
            radius = r;

            return parseQueryOptions(mgr, fn, queryArgAt(va, 2), out);
        }

        // Physics.OverlapBox(center, halfExtents [, rotation] [, options]).
        // rotation is Euler degrees in a Vec3, same convention as
        // transform.rotation (eulerAngleXYZ). Since it is optional and comes before
        // options, the third argument is disambiguated by type: Vec3 => rotation,
        // table => options.
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
                warn("center and halfExtents must be Vec3");
                return false;
            }
            center      = oCenter.as<glm::vec3>();
            halfExtents = oHalf.as<glm::vec3>();
            if (!finite3(halfExtents) ||
                halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
            {
                warn("halfExtents must have all three components greater than 0");
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
                    warn("rotation must be finite");
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

            // Same composition as recomposeLocal (eulerAngleXYZ), so that
            // passing it an entity's transform.rotation orients the box the same way
            // that entity is oriented.
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

        // 1-indexed Entity array with the overlaps. An overlap has no point,
        // normal or distance, so there is no hit table to return: only the
        // GameObject. Actors with no GameObject behind them are omitted (there is nothing
        // to hand to Lua) and the same GameObject comes out ONCE even if several
        // of its shapes overlap.
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

        // Warning shared by the two overlaps when the buffer fills up.
        void warnOverlapOverflow(ScriptManager& mgr, const char* fn,
                                 const physx::PxOverlapBufferN<kOverlapMaxHits>& hits)
        {
            if (hits.getNbTouches() >= hits.getMaxNbTouches())
                mgr.log(std::string("[Lua][WARN] Physics.") + fn + ": limit of " +
                        std::to_string(kOverlapMaxHits) +
                        " overlaps reached, some results were discarded");
        }
#endif

        void registerPhysics(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            // Global Audio table: the volume controls the player expects
            // in an options menu. They existed in AudioManager from the
            // start and NOBODY called them, neither the UI nor the scripts, so
            // an exported game had no way to lower the volume.
            //
            // They are passed by bus name, like AudioClip:SetBus, so as not to
            // have two different vocabularies for the same thing.
            sol::table audio = lua.create_named_table("Audio");

            audio["SetBusVolume"] = [&mgr](const std::string& name, float v) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                AudioBus bus;
                if (!audioBusFromStr(name, bus))
                {
                    mgr.log("[Lua][WARN] Audio.SetBusVolume: unknown bus '" + name +
                             "' (use 'master', 'music' or 'sfx')");
                    return;
                }
                // Same treatment as the clip setters: a NaN here would leave the group's
                // volume unusable for the rest of the game, and
                // there is no .scene where it would show up to debug it later.
                if (!ensureFinite(mgr, "Audio.SetBusVolume", v)) return;
                am->setBusVolume(bus, std::clamp(v, 0.0f, 1.0f));
            };

            // Audio.PlayClipAtPoint(path, x, y, z [, volume, pitch, bus]): a
            // sound at a world position WITHOUT creating a GameObject. It is the
            // gap PlayOneShot left: for an impact or an explosion there is
            // no object to hang the clip on, and often the
            // object that causes it is destroyed in that same frame.
            //
            // The sound is kept in the cache after the first use. Careful: because of
            // FMOD's lazy loading, that first shot almost surely is not
            // heard; Audio.Preload(path) in Start is what fixes it.
            audio["PlayClipAtPoint"] = [&mgr](const std::string& path, float x, float y, float z,
                                               sol::optional<float> volume,
                                               sol::optional<float> pitch,
                                               sol::optional<std::string> busName) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                std::string ext = std::filesystem::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                if (!isSupportedAudioExtension(ext))
                {
                    mgr.log("[Lua][WARN] Audio.PlayClipAtPoint: unsupported format '" + ext +
                             "' (use .wav, .mp3, .ogg or .flac)");
                    return;
                }
                const float v = volume.value_or(1.0f);
                const float p = pitch.value_or(1.0f);
                if (!ensureFinite(mgr, "Audio.PlayClipAtPoint", v)) return;
                if (!ensureFinite(mgr, "Audio.PlayClipAtPoint", p)) return;
                if (!ensureFinite(mgr, "Audio.PlayClipAtPoint", x)) return;
                if (!ensureFinite(mgr, "Audio.PlayClipAtPoint", y)) return;
                if (!ensureFinite(mgr, "Audio.PlayClipAtPoint", z)) return;
                AudioBus bus = AudioBus::Sfx;
                if (busName && !audioBusFromStr(*busName, bus))
                {
                    mgr.log("[Lua][WARN] Audio.PlayClipAtPoint: unknown bus '" + *busName +
                             "' (use 'master', 'music' or 'sfx')");
                    return;
                }
                am->playClipAtPoint(path, glm::vec3(x, y, z),
                                    std::clamp(v, 0.0f, 1.0f), std::clamp(p, 0.5f, 2.0f), bus);
            };

            audio["Preload"] = [&mgr](const std::string& path) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                std::string ext = std::filesystem::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                if (!isSupportedAudioExtension(ext))
                {
                    mgr.log("[Lua][WARN] Audio.Preload: unsupported format '" + ext + "'");
                    return;
                }
                am->preloadClip(path);
            };

            // Per-bus effects: Audio.SetBusEffect("music", "lowPass", 0.2).
            // They apply to everything that goes out through that bus, which is the real use
            // case ("everything muffled underwater"), and not per clip: a per-voice
            // filter is paid for per voice.
            audio["SetBusEffect"] = [&mgr](const std::string& busName,
                                            const std::string& effectName, float amount) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                AudioBus bus;
                if (!audioBusFromStr(busName, bus))
                {
                    mgr.log("[Lua][WARN] Audio.SetBusEffect: unknown bus '" + busName + "'");
                    return;
                }
                AudioEffect effect;
                if (!audioEffectFromStr(effectName, effect))
                {
                    mgr.log("[Lua][WARN] Audio.SetBusEffect: unknown effect '" + effectName +
                             "' (use 'lowPass', 'highPass', 'echo' or 'reverb')");
                    return;
                }
                if (!ensureFinite(mgr, "Audio.SetBusEffect", amount)) return;
                am->setBusEffect(bus, effect, std::clamp(amount, 0.0f, 1.0f));
            };

            audio["ClearBusEffect"] = [&mgr](const std::string& busName,
                                              sol::optional<std::string> effectName) {
                AudioManager* am = mgr.audioManager();
                if (!am) return;
                AudioBus bus;
                if (!audioBusFromStr(busName, bus))
                {
                    mgr.log("[Lua][WARN] Audio.ClearBusEffect: unknown bus '" + busName + "'");
                    return;
                }
                // Without a second argument the whole bus is cleared: which is what is
                // wanted when leaving the water or closing the pause menu.
                if (!effectName) { am->clearBusEffects(bus); return; }
                AudioEffect effect;
                if (!audioEffectFromStr(*effectName, effect))
                {
                    mgr.log("[Lua][WARN] Audio.ClearBusEffect: unknown effect '" +
                             *effectName + "'");
                    return;
                }
                am->clearBusEffect(bus, effect);
            };

            // Global pause: freezes EVERYTHING that is playing, keeping the position.
            // It is what a pause menu wants. Careful: the engine has no simulation
            // pause, so this silences the audio but the scene keeps running.
            audio["SetPaused"] = [&mgr](bool paused) {
                if (AudioManager* am = mgr.audioManager()) am->setAudioPaused(paused);
            };
            audio["IsPaused"] = [&mgr]() {
                AudioManager* am = mgr.audioManager();
                return am && am->isAudioPaused();
            };

            audio["GetBusVolume"] = [&mgr](const std::string& name) -> float {
                AudioManager* am = mgr.audioManager();
                if (!am) return 1.0f;
                AudioBus bus;
                if (!audioBusFromStr(name, bus))
                {
                    mgr.log("[Lua][WARN] Audio.GetBusVolume: unknown bus '" + name + "'");
                    return 1.0f;
                }
                return am->getBusVolume(bus);
            };

            // ReverbZone per GameObject: same pattern as AudioClip, with the
            // preset and the radii. The live FMOD zone is handled by AudioManager;
            // here only the data is touched, and the per-frame sync does the rest.
            lua.new_usertype<LuaReverbZone>("ReverbZone",
                sol::no_constructor,
                "SetPreset", [&mgr](const LuaReverbZone& z, const std::string& name) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    const auto& known = AudioManager::reverbPresetNames();
                    if (std::find(known.begin(), known.end(), name) == known.end())
                    {
                        mgr.log("[Lua][WARN] ReverbZone.SetPreset: unknown preset '" + name +
                                 "', the previous one is kept");
                        return;
                    }
                    go->getReverbZone()->setPreset(name);
                },
                "GetPreset", [](const LuaReverbZone& z) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    return go->getReverbZone()->getPreset();
                },
                "SetMinDistance", [&mgr](const LuaReverbZone& z, float d) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    if (!ensureFinite(mgr, "ReverbZone.SetMinDistance", d)) return;
                    go->getReverbZone()->setMinDistance(d);
                },
                "GetMinDistance", [](const LuaReverbZone& z) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    return go->getReverbZone()->getMinDistance();
                },
                "SetMaxDistance", [&mgr](const LuaReverbZone& z, float d) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    if (!ensureFinite(mgr, "ReverbZone.SetMaxDistance", d)) return;
                    go->getReverbZone()->setMaxDistance(d);
                },
                "GetMaxDistance", [](const LuaReverbZone& z) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    return go->getReverbZone()->getMaxDistance();
                },
                "SetEnabled", [](const LuaReverbZone& z, bool e) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    go->getReverbZone()->setEnabled(e);
                },
                "GetEnabled", [](const LuaReverbZone& z) {
                    GameObject* go = deref(z.e);
                    if (!go->hasReverbZone()) throw std::runtime_error("The GameObject no longer has a ReverbZone");
                    return go->getReverbZone()->getEnabled();
                });

            sol::table physics = lua.create_named_table("Physics");

            // Physics.Raycast(origin, direction, maxDistance, options) -> table
            // { entity, point, normal, distance } or nil. entity is nil if the
            // hit actor does not hang from any GameObject; the rest of the
            // fields are always there.
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
            // 1-indexed array table of hits, each one with the SAME shape
            // that Physics.Raycast returns, sorted by ascending distance.
            // It always returns a table: with no hits (or with invalid
            // arguments, which also warn) it comes out empty, never nil, so the
            // caller can do ipairs/# without checking first.
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

                // PhysX does not warn about the overflow: the hits that did not
                // fit are lost and on top of that the discarded ones are arbitrary
                // (the order arrives unsorted), not "the farthest ones".
                if (hits.getNbTouches() >= hits.getMaxNbTouches())
                    mgr.log("[Lua][WARN] Physics.RaycastAll: limit of " +
                            std::to_string(kRaycastAllMaxHits) +
                            " hits reached, some results were discarded");
#else
                (void)va;
#endif
                return sol::make_object(mgr.lua(), out);
            };

            // Same but without building the table: for the "I just want to know if it
            // hits".
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
            // options) -> the SAME table { entity, point, normal, distance } as
            // Physics.Raycast, or nil if it hits nothing. It is the raycast "with
            // thickness": the sphere starts centered at origin and sweeps along
            // direction. If it already overlaps something at the origin, distance is 0 and
            // point/normal mean nothing (PhysX does not compute the separation
            // without eMTD).
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

            // Physics.OverlapSphere(center, radius, options) -> 1-indexed
            // array table of Entity (NOT of hit tables: an overlap has no
            // point, normal or distance). Empty if nothing overlaps or if the
            // arguments are invalid, never nil.
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
            // same as OverlapSphere but with an oriented box. rotation is
            // optional (Vec3 of Euler degrees, same convention as
            // transform.rotation) and is told apart from options by type.
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

            // Physics.SetLayerCollision(a, b, enabled) / GetLayerCollision(a, b):
            // GLOBAL 32x32 matrix, symmetric: enabling (a,b) enables (b,a).
            // The change propagates to the colliders already in the scene, that
            // is, it works in the middle of a game.
            //
            // Index outside [0,31]: Lua error (see requireLayer). Outside
            // Play there is no PhysicsManager: Set is a no-op and Get returns true, which
            // is what the default matrix says.
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

            // Unity-style global Destroy(): destroys the GameObject and its whole
            // subtree during Play. Same deferred queue as Scene.Destroy; the
            // teardown (OnDestroy in scripts, GPU release via
            // Scene::setOnNodeRemoved, GameObject destructor that releases colliders/
            // audio and removes it from the managers) is processed by the lifecycle at the end
            // of the frame. Deferred on purpose: destroying in the middle of Update
            // would break the lifecycle iteration. deref validates that the entity
            // is still alive (Lua error if it was already destroyed).
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
                // The clone's scripts are instantiated right away; Awake immediately,
                // Start is fired by the lifecycle before its first Update
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
    } // namespace (anonymous)

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
        // A new table, not entry-by-entry deletion: the old keys are no longer useful to
        // anyone (the std::functions that held them are mute because of the
        // epoch) and this way Lua's GC takes the functions away all at once.
        mgr.lua()[kUiCallbackTable] = mgr.lua().create_table();
    }

    void tickTime(ScriptManager& mgr, float dt)
    {
        // A non-finite dt (a degenerate frame, or a test that passes a NaN) must not
        // poison the accumulated value: the whole frame is ignored instead of leaving
        // Time.time at NaN forever.
        if (!std::isfinite(dt)) return;
        g_timeSincePlay += dt;
        ++g_frameCount;
        sol::table time = mgr.lua()["Time"];
        if (!time.valid()) return;
        time["deltaTime"]  = dt;
        time["time"]       = g_timeSincePlay;
        time["frameCount"] = g_frameCount;
    }

    void resetTime(ScriptManager& mgr)
    {
        g_timeSincePlay = 0.0f;
        g_frameCount    = 0;
        sol::table time = mgr.lua()["Time"];
        if (!time.valid()) return;
        time["deltaTime"]  = 0.0f;
        time["time"]       = 0.0f;
        time["frameCount"] = 0;
    }

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerTime(mgr);
        registerLog(mgr);
        registerInput(mgr);
        registerTransform(mgr);
        registerComponents(mgr);
        registerLighting(mgr); // before Entity: GetLight/GetCamera return these types
        registerUi(mgr);      // before Entity: its getters return these types
        registerEntity(mgr);
        registerScene(mgr);   // Task 7
        registerPhysics(mgr);
        registerEngineTable(mgr);
    }
}
