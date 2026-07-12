#pragma once
#include <sol/sol.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "DonTopo/ScriptComponent.h"

namespace DonTopo {

class Scene;
class GameObject;
class PhysicsManager;
class AudioManager;

// Una prop serializable detectada en la tabla clase de un script.
struct ScriptProp {
    std::string name;
    ScriptValue defaultValue;
    // Lua 5.4 distingue 5 (integer) de 5.0 (float); la UI usa DragInt vs
    // DragFloat según esto.
    bool isInteger = false;
};

struct ScriptClass {
    sol::table classTable;
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    std::vector<ScriptProp> props;   // orden alfabético, estable pa la UI
};

// Dueño de la única VM Lua del motor. Carga/registra/instancia scripts.
// El motor nunca llama a Lua fuera de esta clase.
class ScriptManager {
public:
    ScriptManager();
    ~ScriptManager();
    ScriptManager(const ScriptManager&)            = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;

    // Abre libs seguras (base/math/string/table), registra bindings y carga
    // todos los .lua de scriptsDir (recursivo). Los punteros son
    // no-propietarios, mismo patrón que EditorUI::setPhysicsManager.
    void init(const std::string& scriptsDir);
    void setScene(Scene* scene)                    { m_scene = scene; }
    void setPhysicsManager(PhysicsManager* p)      { m_physics = p; }
    void setAudioManager(AudioManager* a)          { m_audio = a; }
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }

    bool loadScript(const std::filesystem::path& path);
    bool hasClass(const std::string& name) const { return m_registry.count(name) > 0; }
    const std::map<std::string, ScriptClass>& getRegistry() const { return m_registry; }
    const std::string* getCompileError(const std::string& name) const;

    // Tabla instancia nueva: copia de props (defaults + overrides) +
    // metatable __index -> tabla clase. Tabla inválida si name no existe.
    sol::table createInstance(const std::string& name,
                              const std::map<std::string, ScriptValue>& overrides);

    sol::state& lua() { return m_lua; }
    Scene* scene() const              { return m_scene; }
    PhysicsManager* physics() const   { return m_physics; }
    AudioManager* audioManager() const { return m_audio; }

    void log(const std::string& msg) { if (m_log) m_log(msg); }

private:
    // Extrae las props serializables (number/boolean/string) de classTable.
    std::vector<ScriptProp> detectProps(const sol::table& classTable);

    sol::state m_lua;
    std::filesystem::path m_scriptsDir;
    std::map<std::string, ScriptClass> m_registry;
    // Último error de compilación por nombre de script (persiste aunque el
    // script siga registrado con su versión anterior válida).
    std::map<std::string, std::string> m_compileErrors;
    std::function<void(const std::string&)> m_log;

    Scene*          m_scene   = nullptr;
    PhysicsManager* m_physics = nullptr;
    AudioManager*   m_audio   = nullptr;
};

} // namespace DonTopo
