#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include <algorithm>

namespace DonTopo {

ReparentCommand::ReparentCommand(Scene& scene, std::string label, uint64_t id,
                                  uint64_t oldParentId, size_t oldIndex,
                                  uint64_t newParentId, size_t newIndex)
    : m_scene(scene), m_label(std::move(label)), m_id(id),
      m_oldParentId(oldParentId), m_oldIndex(oldIndex),
      m_newParentId(newParentId), m_newIndex(newIndex) {}

void ReparentCommand::execute() { moveTo(m_newParentId, m_newIndex); }
void ReparentCommand::undo()    { moveTo(m_oldParentId, m_oldIndex); }

void ReparentCommand::moveTo(uint64_t parentId, size_t index)
{
    GameObject* node = m_scene.findById(m_id);
    GameObject* newParent = m_scene.findById(parentId);
    if (!node || !newParent || !node->parent) return;

    auto& oldSiblings = node->parent->children;
    auto it = std::find_if(oldSiblings.begin(), oldSiblings.end(),
        [node](const std::unique_ptr<GameObject>& c) { return c.get() == node; });
    if (it == oldSiblings.end()) return;

    std::unique_ptr<GameObject> moved = std::move(*it);
    oldSiblings.erase(it);

    moved->parent = newParent;
    auto& newSiblings = newParent->children;
    size_t clampedIndex = std::min(index, newSiblings.size());
    newSiblings.insert(newSiblings.begin() + static_cast<long>(clampedIndex), std::move(moved));
}

DeleteGameObjectCommand::DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio,
                                                  Renderer& renderer, std::string label,
                                                  uint64_t parentId, size_t index, nlohmann::json snapshot)
    : m_scene(scene), m_physics(physics), m_audio(audio), m_renderer(renderer),
      m_label(std::move(label)), m_parentId(parentId), m_index(index), m_snapshot(std::move(snapshot)) {}

void DeleteGameObjectCommand::execute()
{
    uint64_t id = m_snapshot.value("id", uint64_t{0});
    GameObject* node = m_scene.findById(id);
    if (!node) return;
    m_renderer.removeGameObject(node);
    m_scene.removeGameObject(node);
}

void DeleteGameObjectCommand::undo()
{
    GameObject* parent = m_scene.findById(m_parentId);
    GameObject* node = m_scene.insertFromJson(m_snapshot, parent, m_index, m_physics, m_audio);
    if (node)
        m_renderer.registerGameObject(node);
}

CreateGameObjectCommand::CreateGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio,
                                                  Renderer& renderer, std::string label,
                                                  uint64_t parentId, size_t index, nlohmann::json snapshot)
    : m_scene(scene), m_physics(physics), m_audio(audio), m_renderer(renderer),
      m_label(std::move(label)), m_parentId(parentId), m_index(index), m_snapshot(std::move(snapshot)) {}

void CreateGameObjectCommand::execute()
{
    GameObject* parent = m_scene.findById(m_parentId);
    GameObject* node = m_scene.insertFromJson(m_snapshot, parent, m_index, m_physics, m_audio);
    if (node)
        m_renderer.registerGameObject(node);
}

void CreateGameObjectCommand::undo()
{
    uint64_t id = m_snapshot.value("id", uint64_t{0});
    GameObject* node = m_scene.findById(id);
    if (!node) return;
    m_renderer.removeGameObject(node);
    m_scene.removeGameObject(node);
}

CameraComponentCommand::CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, CameraState state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(state) {}

void CameraComponentCommand::execute() { apply(m_add); }
void CameraComponentCommand::undo()    { apply(!m_add); }

void CameraComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setCameraComponent(nullptr);
        return;
    }

    auto cam = std::make_shared<CameraComponent>();
    cam->setMode(m_state.mode);
    // far antes que near: setNear clampa contra el far actual (ver
    // CameraComponent::setNear).
    cam->setFar(m_state.farPlane);
    cam->setNear(m_state.nearPlane);
    cam->setFov(m_state.fov);
    cam->setOrthographicSize(m_state.orthographicSize);
    go->setCameraComponent(cam);
}

AnimatorComponentCommand::AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                    bool add, AnimatorComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void AnimatorComponentCommand::execute() { apply(m_add); }
void AnimatorComponentCommand::undo()    { apply(!m_add); }

void AnimatorComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setAnimator(nullptr);
        return;
    }
    go->setAnimator(std::make_shared<AnimatorComponent>(m_state));
}

AnimationSourceCommand::AnimationSourceCommand(Scene& scene, Renderer* renderer,
                                                std::string label, uint64_t id, bool add,
                                                std::string path,
                                                std::vector<std::string> clipNames,
                                                size_t pathOccurrence)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_add(add), m_path(std::move(path)), m_clipNames(std::move(clipNames)),
      m_pathOccurrence(pathOccurrence) {}

void AnimationSourceCommand::execute() { m_add ? applyAdd() : applyRemove(); }
void AnimationSourceCommand::undo()    { m_add ? applyRemove() : applyAdd(); }

void AnimationSourceCommand::applyAdd()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    std::vector<std::string> warnings;
    // m_clipNames vacío = primera vez (el usuario acaba de elegir el
    // fichero): los nombres los decide addAnimationSource y se guardan aquí
    // para que un redo posterior reproduzca exactamente los mismos.
    const std::vector<std::string>* forced = m_clipNames.empty() ? nullptr : &m_clipNames;
    if (!addAnimationSource(*mesh, m_path, warnings, forced)) return;

    m_clipNames = mesh->animationSources.back().clipNames;
    // addAnimationSource siempre añade al final: la fuente que acabamos de
    // (re)meter es, por definición, la última con ese path — occurrence 0
    // contado desde el final. Esto se recalcula en cada applyAdd (tanto el
    // Add real como el undo de un Remove que reinserta al final) para que un
    // applyRemove posterior siga apuntando a ELLA y no a la ordinal que se
    // capturó en el clic original, que tras el reinsert ya no describe su
    // posición (ver comentario largo en applyRemove).
    m_pathOccurrence = 0;

    // bindClips resuelve por nombre en Scene::nodeFromJson, pero eso solo
    // corre en una carga de escena: aquí el mesh muta en caliente (puede que
    // a mitad de Play Mode) y nadie más re-resuelve clipIndex. Sin esto, los
    // estados quedan con el índice VIEJO, que tras el add sigue siendo
    // válido como índice pero puede apuntar a un clip distinto (Finding 1 de
    // la revisión: "Remove" desplaza el resto de la lista). rebindClips (sin
    // el reset() de bindClips) preserva m_currentState y los parámetros del
    // usuario, que bindClips destruiría.
    if (go->hasAnimator())
    {
        std::vector<std::string> bindWarnings;
        go->getAnimator()->rebindClips(*mesh, &bindWarnings);
        // Sin canal de log desde Command.cpp (ICommand no conoce
        // EditorContext/pushLog, a diferencia de AnimatorPanel): se
        // descartan, mismo precedente que ya sienta este mismo método unas
        // líneas arriba con los warnings de addAnimationSource.
    }

    if (m_renderer && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
}

void AnimationSourceCommand::applyRemove()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    // Se localiza por path Y por posición, no solo por path: dos fuentes
    // pueden compartir path a propósito (reimportar el mismo fichero, ver
    // AnimatorPanel::drawAnimationSources) y coexistir como filas
    // independientes. m_pathOccurrence cuenta, ESCANEANDO DESDE EL FINAL,
    // cuántas fuentes no-builtin con ese path hay que saltarse antes de
    // llegar a la que se quiere quitar (0 = la primera que se encuentra
    // yendo desde el final, o sea la más reciente).
    //
    // Por qué desde el final y no un índice absoluto: undo() de un Remove
    // reinserta la fuente al FINAL del vector (applyAdd), no en su posición
    // original — así que su índice absoluto cambia entre el borrado y un
    // redo posterior. Contar desde el final sí es estable: applyAdd
    // recalcula m_pathOccurrence a 0 cada vez que reinserta (porque lo que
    // acaba de reinsertar es, por definición, lo último), así que un
    // redo inmediato vuelve a encontrar exactamente esa misma fuente aquí.
    // El único caso en que m_pathOccurrence vale algo distinto de 0 es el
    // clic ORIGINAL del usuario sobre una fila que no es la última con ese
    // path (el bug que corrige este fix): AnimatorPanel::drawAnimationSources
    // lo calcula en ese momento.
    bool removed = false;
    size_t skipped = 0;
    for (size_t i = mesh->animationSources.size(); i-- > 0; )
    {
        const auto& src = mesh->animationSources[i];
        if (src.builtin || src.path != m_path) continue;
        if (skipped != m_pathOccurrence) { skipped++; continue; }
        m_clipNames = src.clipNames;
        removeAnimationSource(*mesh, i);
        removed = true;
        break;
    }

    // Nada que quitar (redo tras recarga de escena, o mesh reemplazado entre
    // execute() y undo()): rebuildSkinnedMesh es un vkDeviceWaitIdle + un
    // destroy/recreate completo de buffers/texturas/descriptor sets, y
    // pagarlo por una malla que no cambió es puro desperdicio (applyAdd ya
    // hacía este mismo early-return con su "if (!addAnimationSource(...))
    // return;").
    if (!removed) return;

    // Mismo motivo que en applyAdd: el mesh mutó en caliente y clipIndex
    // apunta a índices que ya no describen los mismos clips (el hueco que
    // deja el clip quitado desplaza a los de detrás). reset() no: se
    // preserva el estado runtime del Animator.
    if (go->hasAnimator())
    {
        std::vector<std::string> bindWarnings;
        go->getAnimator()->rebindClips(*mesh, &bindWarnings);
        // Se descartan por el mismo motivo que en applyAdd: no hay canal de
        // log disponible desde un ICommand.
    }

    if (m_renderer && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
}

ClipRenameCommand::ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                                      std::string oldName, std::string newName)
    : m_scene(scene), m_label(std::move(label)), m_id(id),
      m_oldName(std::move(oldName)), m_newName(std::move(newName)) {}

void ClipRenameCommand::execute() { apply(m_oldName, m_newName); }
void ClipRenameCommand::undo()    { apply(m_newName, m_oldName); }

void ClipRenameCommand::apply(const std::string& from, const std::string& to)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;
    if (!renameClip(*mesh, from, to)) return;
    if (go->hasAnimator())
        go->getAnimator()->renameClipReferences(from, to);
}

} // namespace DonTopo
