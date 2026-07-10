#include "DonTopo/Scene.h"
#include <algorithm>
#include <memory>

namespace DonTopo
{
    Scene::Scene(std::string name) : m_name(std::move(name)), m_root("root") {}

    GameObject* Scene::addGameObject(const std::string& name, GameObject* parent)
    {
        GameObject* target = parent ? parent : &m_root;
        return target->addChild(name);
    }

    void Scene::removeGameObject(GameObject* node)
    {
        if (!node || !node->parent) return;

        auto& siblings = node->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [node](const std::unique_ptr<GameObject>& c) { return c.get() == node; }),
            siblings.end());
    }

    void Scene::update(float /*dt*/, PhysicsManager& /*physics*/)
    {
        m_root.traverse([](GameObject* go) {
            if (go->hasBoxCollider())
            {
                if (go->getBoxCollider()->isDynamic())
                {
                    go->worldTransform = go->getBoxCollider()->getWorldTransform();
                    glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                    go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                }
                else
                    go->getBoxCollider()->syncTransform(go->worldTransform);
            }

            if (go->hasSphereCollider())
            {
                if (go->getSphereCollider()->isDynamic())
                {
                    go->worldTransform = go->getSphereCollider()->getWorldTransform();
                    glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                    go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                }
                else
                    go->getSphereCollider()->syncTransform(go->worldTransform);
            }

            if (go->hasCapsuleCollider())
            {
                if (go->getCapsuleCollider()->isDynamic())
                {
                    go->worldTransform = go->getCapsuleCollider()->getWorldTransform();
                    glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                    go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                }
                else
                    go->getCapsuleCollider()->syncTransform(go->worldTransform);
            }

            // Plane Collider siempre es kinematic (isDynamic()==false hardcoded):
            // nunca lee pose de PhysX, solo empuja la del GameObject.
            if (go->hasPlaneCollider())
                go->getPlaneCollider()->syncTransform(go->worldTransform);
        });

        // Sync física-transform corre antes de propagar transforms locales:
        // los colliders ya escriben worldTransform/localTransform directamente,
        // así que updateWorldTransforms() solo necesita recalcular los nodos
        // sin collider (hijos de un padre cuyo worldTransform pudo cambiar).
        m_root.updateWorldTransforms();
    }
    void Scene::shutdown(PhysicsManager& /*physics*/, AudioManager& /*audio*/)
    {
        m_root.traverse([](GameObject* go) {
            go->setBoxCollider(nullptr);
            go->setSphereCollider(nullptr);
            go->setCapsuleCollider(nullptr);
            go->setPlaneCollider(nullptr);
            go->setAudioClip(nullptr);
        });
    }
}
