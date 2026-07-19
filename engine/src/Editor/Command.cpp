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
                                                std::vector<std::string> clipNames)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_add(add), m_path(std::move(path)), m_clipNames(std::move(clipNames)) {}

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

    if (m_renderer && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
}

void AnimationSourceCommand::applyRemove()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    // Se localiza por path Y por no-builtin: dos fuentes pueden compartir
    // path (reimportar el mismo fichero), así que se quita la última que
    // coincida, que es la que este comando añadió.
    for (size_t i = mesh->animationSources.size(); i-- > 0; )
    {
        const auto& src = mesh->animationSources[i];
        if (src.builtin || src.path != m_path) continue;
        m_clipNames = src.clipNames;
        removeAnimationSource(*mesh, i);
        break;
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
