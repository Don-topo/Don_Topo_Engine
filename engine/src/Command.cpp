#include "DonTopo/Command.h"
#include "DonTopo/Scene.h"
#include "DonTopo/GameObject.h"
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

} // namespace DonTopo
