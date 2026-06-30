#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>

namespace DonTopo 
{
    struct SceneNode
    {
        std::string                             name;
        int                                     meshIndex = -1; // -1 = nodo sin mesh
        glm::mat4                               localTransform {1.0f};
        glm::mat4                               worldTransform {1.0f};
        SceneNode*                              parent = nullptr;
        std::vector<std::unique_ptr<SceneNode>> children;    

        SceneNode* addChild(std::string name, int meshIndex = -1);
        void updateWorldTransforms(const glm::mat4& parentWorld = glm::mat4(1.0f));

        template <typename Fn>
        void traverse(Fn fn)
        {
            fn(this);
            for(auto& c : children) c->traverse(fn);
        }
    };
}