#include "DonTopo/ScriptManager.h"
#include "DonTopo/ScriptBindings.h"
#include "DonTopo/Scene.h"
#include <algorithm>

namespace DonTopo
{
    ScriptManager::ScriptManager()  = default;
    ScriptManager::~ScriptManager() = default;

    void ScriptManager::init(const std::string& scriptsDir)
    {
        // Solo libs sin acceso a proceso/filesystem: los scripts de gameplay
        // no necesitan io/os, y así un script no puede tocar disco.
        m_lua.open_libraries(sol::lib::base, sol::lib::math,
                             sol::lib::string, sol::lib::table);

        ScriptBindings::registerAll(*this);

        m_scriptsDir = scriptsDir;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_scriptsDir, ec))
        {
            log("Scripts: carpeta '" + scriptsDir + "' no encontrada — sin scripts");
            return;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_scriptsDir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".lua")
                loadScript(entry.path());
        }
    }

    bool ScriptManager::loadScript(const std::filesystem::path& path)
    {
        const std::string className = path.stem().string();

        auto result = m_lua.safe_script_file(path.string(), sol::script_pass_on_error);
        if (!result.valid())
        {
            sol::error err = result;
            m_compileErrors[className] = err.what();
            log("Script '" + className + "': error de compilación: " + err.what());
            return false;
        }

        sol::object classObj = m_lua[className];
        if (classObj.get_type() != sol::type::table)
        {
            m_compileErrors[className] =
                "el archivo no define una tabla global '" + className + "'";
            log("Script '" + className + "': no define la tabla global '" + className + "'");
            return false;
        }

        ScriptClass cls;
        cls.classTable = classObj.as<sol::table>();
        cls.path       = path;
        std::error_code ec;
        cls.mtime      = std::filesystem::last_write_time(path, ec);
        cls.props      = detectProps(cls.classTable);

        m_registry[className] = std::move(cls);
        m_compileErrors.erase(className);
        log("Script '" + className + "' registrado (" +
            std::to_string(m_registry[className].props.size()) + " props)");
        return true;
    }

    const std::string* ScriptManager::getCompileError(const std::string& name) const
    {
        auto it = m_compileErrors.find(name);
        return it != m_compileErrors.end() ? &it->second : nullptr;
    }

    std::vector<ScriptProp> ScriptManager::detectProps(const sol::table& classTable)
    {
        std::vector<ScriptProp> props;
        for (const auto& [key, value] : classTable)
        {
            if (key.get_type() != sol::type::string) continue;

            ScriptProp p;
            p.name = key.as<std::string>();
            switch (value.get_type())
            {
                case sol::type::number:
                {
                    // lua_isinteger distingue 5 (integer) de 5.0 (float)
                    value.push(m_lua.lua_state());
                    p.isInteger = lua_isinteger(m_lua.lua_state(), -1) != 0;
                    lua_pop(m_lua.lua_state(), 1);
                    p.defaultValue = value.as<double>();
                    break;
                }
                case sol::type::boolean:
                    p.defaultValue = value.as<bool>();
                    break;
                case sol::type::string:
                    p.defaultValue = value.as<std::string>();
                    break;
                default:
                    continue; // funciones/tablas anidadas: no son props
            }
            props.push_back(std::move(p));
        }
        std::sort(props.begin(), props.end(),
                  [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
        return props;
    }

    sol::table ScriptManager::createInstance(
        const std::string& name, const std::map<std::string, ScriptValue>& overrides)
    {
        auto it = m_registry.find(name);
        if (it == m_registry.end()) return sol::table();

        const ScriptClass& cls = it->second;
        sol::table inst = m_lua.create_table();

        // Copia de props a la instancia: cada instancia tiene las suyas,
        // editar una no toca las demás. Funciones se heredan vía metatable.
        for (const ScriptProp& p : cls.props)
        {
            const ScriptValue* v = &p.defaultValue;
            auto ov = overrides.find(p.name);
            if (ov != overrides.end()) v = &ov->second;

            std::visit([&](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, double>)
                {
                    if (p.isInteger) inst[p.name] = static_cast<int64_t>(val);
                    else             inst[p.name] = val;
                }
                else inst[p.name] = val;
            }, *v);
        }

        sol::table mt = m_lua.create_table();
        mt["__index"] = cls.classTable;
        inst[sol::metatable_key] = mt;
        return inst;
    }

    void ScriptManager::rebuildAliveSet()
    {
        m_alive.clear();
        if (!m_scene) return;
        m_scene->traverse([this](GameObject* go) { m_alive.insert(go); });
    }

    void ScriptManager::instantiateComponent(ScriptComponent& comp)
    {
        comp.instance = createInstance(comp.scriptName, comp.overrides);
        comp.started  = false;
        comp.hasError = false;
        if (!comp.instance.valid()) return;   // clase no registrada (missing)

        comp.instance["entity"] = LuaEntity{ comp.owner, this };

        auto isFn = [&](const char* n) {
            return comp.instance[n].get_type() == sol::type::function;
        };
        comp.hasAwake       = isFn("Awake");
        comp.hasStart       = isFn("Start");
        comp.hasUpdate      = isFn("Update");
        comp.hasFixedUpdate = isFn("FixedUpdate");
        comp.hasLateUpdate  = isFn("LateUpdate");
        comp.hasOnDestroy   = isFn("OnDestroy");
    }
}
