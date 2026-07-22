#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"

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
// La gravedad ya no vive en el collider (pasó al Rigidbody): ver RigidbodyState.
struct BoxColliderState     { glm::vec3 center; glm::vec3 size; bool isTrigger; };
struct SphereColliderState  { glm::vec3 center; float radius; bool isTrigger; };
struct CapsuleColliderState { glm::vec3 center; float radius; float height; bool isTrigger; };
struct PlaneColliderState   { glm::vec3 center; bool isTrigger; };

// Snapshot value-type del Rigidbody — T de PropertyCommand<T> en la sección
// Rigidbody del panel Properties.
struct RigidbodyState {
    float    mass;
    bool     useGravity;
    bool     isKinematic;
    float    drag;
    float    angularDrag;
    uint32_t constraints;
};

// Snapshot value-type del AudioClipComponent — T de PropertyCommand<T> en la
// sección Audio Clip del panel Properties. Sólo volumen y pitch: loop, is3D y
// playOnAwake se escriben directos y no tienen undo.
struct AudioClipState {
    float volume;
    float pitch;
};

// Snapshot value-type del CameraComponent — T de PropertyCommand<T> en la
// sección Camera del panel Properties.
struct CameraState {
    CameraComponent::ProjectionMode mode;
    float fov;
    float orthographicSize;
    float nearPlane;
    float farPlane;
};

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

// Añade (add=true) o quita (add=false) el CameraComponent del GameObject id;
// undo() hace lo contrario.
//
// A diferencia de los Add de collider/Rigidbody (que no pasan por el stack),
// el de cámara SÍ: sin esto se puede llegar a dos cámaras en escena — Add a X,
// Delete X (el snapshot se lleva la cámara), Add a Z (permitido, findCamera()
// es nullptr), Ctrl+Z resucita X CON su cámara. Con el Add en el stack, para
// deshacer el Delete de X hay que deshacer antes el Add de Z, y el orden impone
// el invariante sin descartar nada.
//
// Resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// mismo contrato que PropertyCommand. m_state conserva los valores pa que un
// Add-undo-redo no los devuelva a los defaults.
class CameraComponentCommand : public ICommand {
public:
    CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CameraState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CameraState m_state;
};

// Add/Remove del AnimatorComponent, mismo contrato que CameraComponentCommand:
// resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// y m_state conserva el grafo pa que un Add-undo-redo no lo devuelva vacío.
//
// El estado es una COPIA del componente entero, no un POD de campos como
// CameraState: el "estado" de un Animator es el grafo completo, y
// AnimatorComponent es copiable (solo vectores, mapas y PODs). Serializarlo a
// JSON pa esto no compraría nada — las funciones de JSON viven en el anon
// namespace de Scene.cpp y no son accesibles desde aquí.
class AnimatorComponentCommand : public ICommand {
public:
    AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                              bool add, AnimatorComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    AnimatorComponent m_state;
};

// Añade (add=true) o quita (add=false) una fuente de animación del SkinnedMesh
// del GameObject id; undo() hace lo contrario. Mismo contrato que el resto:
// resuelve el GameObject por id en cada execute()/undo().
//
// m_clipNames guarda los nombres que la fuente aportó, y hace dos trabajos.
// Uno: sin él, deshacer un Remove reimportaría el fichero con los nombres del
// FBX y se perdería cualquier rename — dejando huérfanos los estados del grafo
// que los usaban. Dos: es la IDENTIDAD con la que applyRemove localiza su
// fuente. Los nombres de clip son únicos dentro del mesh (uniqueClipName lo
// garantiza) y viajan CON la fuente, mientras que una posición describe dónde
// estaba: applyAdd re-añade al final, así que cualquier ordinal guardado por
// otro comando del stack cambia de significado en cuanto se deshace un Remove.
//
// renderer puede ser nullptr (tests headless). Cuando no lo es, los SSBOs del
// objeto skinned se rehacen: la lista de clips ha cambiado y la GPU tiene la
// vieja.
class AnimationSourceCommand : public ICommand {
public:
    // pathOccurrence: FALLBACK posicional para applyRemove — qué fuente
    // no-builtin con ese path quitar, CONTADO DESDE EL FINAL del vector
    // (0 = la más reciente/última). Solo se usa cuando la búsqueda por
    // identidad (m_clipNames) no encuentra nada, p.ej. con m_clipNames vacío.
    // Importar el mismo FBX dos veces es legal, y AnimatorPanel distingue las
    // filas con este mismo ordinal. 0 por defecto vale tanto para un Add real
    // (nada que desambiguar, la fuente nueva siempre va al final) como para el
    // undo de un Add.
    AnimationSourceCommand(Scene& scene, Renderer* renderer, std::string label,
                            uint64_t id, bool add, std::string path,
                            std::vector<std::string> clipNames,
                            size_t pathOccurrence = 0);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void applyAdd();
    void applyRemove();

    Scene& m_scene;
    Renderer* m_renderer;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    std::string m_path;
    std::vector<std::string> m_clipNames;
    size_t m_pathOccurrence;
};

// Renombra un clip del mesh y arrastra los estados del Animator que lo usaban.
// No toca la GPU: los buffers van por índice de clip, y renombrar no reordena
// nada.
class ClipRenameCommand : public ICommand {
public:
    ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                       std::string oldName, std::string newName);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& from, const std::string& to);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    std::string m_oldName;
    std::string m_newName;
};

} // namespace DonTopo
