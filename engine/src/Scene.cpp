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

    glm::mat4 jsonToMat4(const nlohmann::json& j)
    {
        glm::mat4 m(1.0f);
        float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            p[i] = j.at(i).get<float>();
        return m;
    }

    glm::vec3 jsonToVec3(const nlohmann::json& j)
    {
        return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>());
    }

    // Crea el Mesh procedural correspondiente a meshName (case-insensitive),
    // con los mismos parámetros fijos que EditorUI::createBasicShape. nullptr
    // si meshName no matchea ninguna de las 4 formas básicas.
    std::shared_ptr<DonTopo::Mesh> proceduralMeshByName(const std::string& meshName)
    {
        std::string lower = meshName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower == "cube")    return std::make_shared<DonTopo::Mesh>(DonTopo::Cube::create(50.0f));
        if (lower == "sphere")  return std::make_shared<DonTopo::Mesh>(DonTopo::Sphere::create(50.0f));
        if (lower == "plane")   return std::make_shared<DonTopo::Mesh>(DonTopo::Plane::create(50.0f, 0.0f));
        if (lower == "capsule") return std::make_shared<DonTopo::Mesh>(DonTopo::Capsule::create(25.0f, 50.0f));
        return nullptr;
    }

    // Reconstruye node (ya insertado en el árbol) desde j, y recursivamente
    // sus hijos. parentWorld es el worldTransform ya resuelto del padre —
    // necesario para pasar un worldTransform correcto a las factories de
    // collider (que fijan la pose inicial del actor PhysX a partir de él).
    void nodeFromJson(const nlohmann::json& j, GameObject* node, const glm::mat4& parentWorld,
                       DonTopo::PhysicsManager& physics, DonTopo::AudioManager& audio)
    {
        node->localTransform = jsonToMat4(j.at("localTransform"));
        node->worldTransform = parentWorld * node->localTransform;

        if (j.contains("mesh"))
        {
            std::string sourcePath = j["mesh"].value("sourcePath", "");
            std::string meshName   = j["mesh"].value("name", "");
            try
            {
                if (!sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath));
                    mesh->sourcePath = sourcePath;
                    node->setMesh(std::move(mesh));
                }
                else if (auto mesh = proceduralMeshByName(meshName))
                {
                    node->setMesh(std::move(mesh));
                }
            }
            catch (const std::exception&)
            {
                // Asset roto (movido/borrado) o formato no soportado: node
                // queda sin mesh, el resto de la escena sigue cargando.
            }
        }

        if (j.contains("boxCollider"))
        {
            const auto& c = j["boxCollider"];
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.at("halfExtents")), jsonToVec3(c.at("center")),
                node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            node->setSphereCollider(physics.createSphereColliderComponent(
                c.at("radius").get<float>(), jsonToVec3(c.at("center")),
                node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                c.at("radius").get<float>(), c.at("halfHeight").get<float>(),
                jsonToVec3(c.at("center")), node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("planeCollider"))
        {
            const auto& c = j["planeCollider"];
            node->setPlaneCollider(physics.createPlaneColliderComponent(
                jsonToVec3(c.at("center")), node->worldTransform));
        }
        if (j.contains("audioClip"))
        {
            const auto& c = j["audioClip"];
            auto clip = audio.createAudioClipComponent(
                c.at("path").get<std::string>(), c.at("is3D").get<bool>(), c.at("loop").get<bool>());
            if (clip)
                node->setAudioClip(std::move(clip));
            // clip nullptr (asset roto/formato no soportado): node queda sin
            // audio, el resto de la escena sigue cargando.
        }

        for (const auto& childJson : j.at("children"))
        {
            GameObject* child = node->addChild(childJson.at("name").get<std::string>());
            nodeFromJson(childJson, child, node->worldTransform, physics, audio);
        }
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

    bool Scene::load(const std::string& path, PhysicsManager& physics, AudioManager& audio)
    {
        auto parsed = FileManager::readJson(path);
        if (!parsed)
            return false;

        const nlohmann::json& j = *parsed;
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1 ||
            !j.contains("root") || !j["root"].is_object())
            return false;

        shutdown(physics, audio);
        m_root.children.clear();

        const nlohmann::json& rootJson = j["root"];
        m_root.name = rootJson.value("name", "root");

        try
        {
            nodeFromJson(rootJson, &m_root, glm::mat4(1.0f), physics, audio);
        }
        catch (const nlohmann::json::exception&)
        {
            // Nodo interno malformado (campo requerido ausente/tipo incorrecto):
            // la escena queda parcialmente reconstruida en vez de crashear. Caso
            // no cubierto por el spec de forma explícita — se prioriza no-crash
            // sobre atomicidad total de la carga.
            return false;
        }

        m_root.updateWorldTransforms();
        return true;
    }
}
