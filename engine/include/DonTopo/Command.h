#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace DonTopo {

class Scene;
class Renderer;
class PhysicsManager;
class AudioManager;

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;   // aplica "after" (redo)
    virtual void undo() = 0;      // aplica "before"
    virtual std::string label() const = 0;   // pa Log Console
};

// Comando genérico pa cualquier propiedad value-type de un GameObject o de
// uno de sus componentes. apply() resuelve el objeto en vivo cada vez que se
// invoca (nunca captura un GameObject* crudo) — sobrevive a que el
// GameObject se haya reconstruido entretanto por un Undo de Delete.
template <typename T>
class PropertyCommand : public ICommand {
public:
    PropertyCommand(std::string label, T before, T after,
                     std::function<void(const T&)> apply)
        : m_label(std::move(label)), m_before(std::move(before)),
          m_after(std::move(after)), m_apply(std::move(apply)) {}

    void execute() override { m_apply(m_after); }
    void undo()    override { m_apply(m_before); }
    std::string label() const override { return m_label; }

private:
    std::string m_label;
    T m_before;
    T m_after;
    std::function<void(const T&)> m_apply;
};

// Snapshots value-type pa cada tipo de collider — T de PropertyCommand<T>
// en las secciones Box/Sphere/Capsule/Plane Collider del panel Properties.
struct BoxColliderState     { glm::vec3 center; glm::vec3 size; bool useGravity; };
struct SphereColliderState  { glm::vec3 center; float radius; bool useGravity; };
struct CapsuleColliderState { glm::vec3 center; float radius; float height; bool useGravity; };
struct PlaneColliderState   { glm::vec3 center; };

class ReparentCommand : public ICommand {
public:
    ReparentCommand(Scene& scene, std::string label, uint64_t id,
                     uint64_t oldParentId, size_t oldIndex,
                     uint64_t newParentId, size_t newIndex);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void moveTo(uint64_t parentId, size_t index);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    uint64_t m_oldParentId;
    size_t m_oldIndex;
    uint64_t m_newParentId;
    size_t m_newIndex;
};

// Borra un GameObject ya existente (execute) / lo reconstruye desde un
// snapshot JSON tomado ANTES de borrarlo (undo). El snapshot conserva el id
// original (Scene::subtreeToJson/nodeToJson serializan "id"), así que
// comandos posteriores en el stack que referencien ese id lo siguen
// resolviendo tras un undo() de este comando.
class DeleteGameObjectCommand : public ICommand {
public:
    DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, Renderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    Renderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
};

// Inverso de DeleteGameObjectCommand: reconstruye desde snapshot (execute) /
// borra (undo). snapshot ya incluye el subárbol completo tal y como quedó
// justo después de crearlo (mismo formato que DeleteGameObjectCommand).
class CreateGameObjectCommand : public ICommand {
public:
    CreateGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, Renderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    Renderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
};

} // namespace DonTopo
