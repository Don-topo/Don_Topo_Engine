#include "DonTopo/Scene.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Mesh.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/FileManager.h"
#include <algorithm>
#include <cctype>
#include <memory>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>

namespace
{
    using DonTopo::GameObject;

    nlohmann::json mat4ToJson(const glm::mat4& m)
    {
        auto arr = nlohmann::json::array();
        const float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            arr.push_back(p[i]);
        return arr;
    }

    nlohmann::json vec3ToJson(const glm::vec3& v)
    {
        return nlohmann::json::array({ v.x, v.y, v.z });
    }

    nlohmann::json nodeToJson(const GameObject& node)
    {
        nlohmann::json j;
        j["name"] = node.name;
        j["localTransform"] = mat4ToJson(node.localTransform);

        if (node.hasMesh())
        {
            const auto& mesh = node.getMesh();
            j["mesh"] = { {"sourcePath", mesh->sourcePath}, {"name", mesh->name} };
        }
        if (node.hasBoxCollider())
        {
            const auto& c = node.getBoxCollider();
            j["boxCollider"] = { {"halfExtents", vec3ToJson(c->getHalfExtents())},
                                  {"center", vec3ToJson(c->getCenter())},
                                  {"useGravity", c->getUseGravity()} };
        }
        if (node.hasSphereCollider())
        {
            const auto& c = node.getSphereCollider();
            j["sphereCollider"] = { {"radius", c->getRadius()},
                                     {"center", vec3ToJson(c->getCenter())},
                                     {"useGravity", c->getUseGravity()} };
        }
        if (node.hasCapsuleCollider())
        {
            const auto& c = node.getCapsuleCollider();
            j["capsuleCollider"] = { {"radius", c->getRadius()},
                                      {"halfHeight", c->getHalfHeight()},
                                      {"center", vec3ToJson(c->getCenter())},
                                      {"useGravity", c->getUseGravity()} };
        }
        if (node.hasPlaneCollider())
        {
            const auto& c = node.getPlaneCollider();
            j["planeCollider"] = { {"center", vec3ToJson(c->getCenter())} };
        }
        if (node.hasAudioClip())
        {
            const auto& clip = node.getAudioClip();
            j["audioClip"] = { {"path", clip->getPath()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()} };
        }

        j["children"] = nlohmann::json::array();
        for (const auto& child : node.children)
            j["children"].push_back(nodeToJson(*child));

        return j;
    }
}

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

    bool Scene::save(const std::string& path) const
    {
        nlohmann::json root;
        root["version"] = 1;
        root["root"] = nodeToJson(m_root);
        return FileManager::writeJson(path, root);
    }
}
