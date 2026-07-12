#include "DonTopo/ScriptBindings.h"
#include "DonTopo/ScriptManager.h"
#include "DonTopo/Input.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace DonTopo::ScriptBindings
{
    namespace
    {
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
            // print nativo -> mismo destino que Log.Info
            lua["print"] = [&mgr](sol::variadic_args args) {
                std::string out;
                for (auto a : args)
                {
                    if (!out.empty()) out += "\t";
                    out += a.get<sol::object>().as<std::string>();
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
    }

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerLog(mgr);
        registerInput(mgr.lua());
        // Tasks 6-7 añaden aquí: registerEntity, registerTransform,
        // registerComponents, registerScene.
    }
}
