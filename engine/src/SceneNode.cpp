#include "DonTopo/SceneNode.h"

namespace DonTopo{
    SceneNode* SceneNode::addChild(std::string name, int meshIndex)
    {
        auto node = std::make_unique<SceneNode>();
        node->name      = std::move(name);
        node->meshIndex = meshIndex;
        node->parent    = this;
        SceneNode* raw  = node.get();
        children.push_back(std::move(node));
        return raw;
    }

    void SceneNode::updateWorldTransforms(const glm::mat4& parentWorld)
    {
        worldTransform = parentWorld * localTransform;
        for(auto& c : children)
        {
            c->updateWorldTransforms(worldTransform);
        }
    }
}