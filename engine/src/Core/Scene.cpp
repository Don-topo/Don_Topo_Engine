#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/Sphere.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Renderer/Capsule.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include <algorithm>
#include <cctype>
#include <cmath>
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

    nlohmann::json vertexToJson(const DonTopo::Vertex& v)
    {
        return { {"pos", vec3ToJson(v.pos)},
                 {"color", vec3ToJson(v.color)},
                 {"uv", nlohmann::json::array({v.uv.x, v.uv.y})},
                 {"normal", vec3ToJson(v.normal)},
                 {"tangent", vec3ToJson(v.tangent)} };
    }

    nlohmann::json nodeToJson(const GameObject& node)
    {
        nlohmann::json j;
        j["id"] = node.id;
        j["name"] = node.name;
        j["localTransform"] = mat4ToJson(node.localTransform);

        if (node.hasMesh())
        {
            const auto& mesh = node.getMesh();
            nlohmann::json meshJson = { {"sourcePath", mesh->sourcePath}, {"name", mesh->name}, {"skinned", node.isSkinned()} };
            if (mesh->sourcePath.empty())
            {
                // Procedural (Cube/Sphere/Plane/Capsule): no hay fichero de
                // origen que recargar. Regenerar vía los parámetros fijos de
                // ScenePanel::createBasicShape asumiría que el mesh se creó con
                // esos defaults — falso para meshes procedurales con
                // parámetros custom (ej. el floor, Plane::create(1000.0f,
                // floorY) en main.cpp, muy distinto del Plane 50/0 del menú
                // Basic Shapes). Se serializa la geometría real para
                // reconstruir el mesh exacto sin depender de qué parámetros
                // lo generaron.
                nlohmann::json verts = nlohmann::json::array();
                for (const auto& v : mesh->vertices)
                    verts.push_back(vertexToJson(v));
                meshJson["vertices"] = std::move(verts);
                meshJson["indices"]  = mesh->indices;
            }
            j["mesh"] = std::move(meshJson);
        }
        if (node.hasBoxCollider())
        {
            const auto& c = node.getBoxCollider();
            j["boxCollider"] = { {"halfExtents", vec3ToJson(c->getHalfExtents())},
                                  {"center", vec3ToJson(c->getCenter())},
                                  {"useGravity", c->getUseGravity()},
                                  {"isTrigger", c->isTrigger()} };
        }
        if (node.hasSphereCollider())
        {
            const auto& c = node.getSphereCollider();
            j["sphereCollider"] = { {"radius", c->getRadius()},
                                     {"center", vec3ToJson(c->getCenter())},
                                     {"useGravity", c->getUseGravity()},
                                     {"isTrigger", c->isTrigger()} };
        }
        if (node.hasCapsuleCollider())
        {
            const auto& c = node.getCapsuleCollider();
            j["capsuleCollider"] = { {"radius", c->getRadius()},
                                      {"halfHeight", c->getHalfHeight()},
                                      {"center", vec3ToJson(c->getCenter())},
                                      {"useGravity", c->getUseGravity()},
                                      {"isTrigger", c->isTrigger()} };
        }
        if (node.hasPlaneCollider())
        {
            const auto& c = node.getPlaneCollider();
            j["planeCollider"] = { {"center", vec3ToJson(c->getCenter())},
                                    {"isTrigger", c->isTrigger()} };
        }
        if (node.hasAudioClip())
        {
            const auto& clip = node.getAudioClip();
            j["audioClip"] = { {"path", clip->getPath()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()},
                                {"playOnAwake", clip->getPlayOnAwake()} };
        }
        if (node.hasScripts())
        {
            auto arr = nlohmann::json::array();
            for (const auto& s : node.getScripts())
            {
                nlohmann::json ov = nlohmann::json::object();
                for (const auto& [key, val] : s->overrides)
                {
                    std::visit([&](auto&& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, double>)
                        {
                            // Preserva enteros como enteros en el JSON
                            if (v == std::floor(v) && std::abs(v) < 1e15)
                                ov[key] = static_cast<int64_t>(v);
                            else
                                ov[key] = v;
                        }
                        else
                            ov[key] = v;
                    }, val);
                }
                arr.push_back({ {"name", s->scriptName}, {"overrides", std::move(ov)} });
            }
            j["scripts"] = std::move(arr);
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

    DonTopo::Vertex jsonToVertex(const nlohmann::json& j)
    {
        DonTopo::Vertex v{};
        v.pos     = jsonToVec3(j.at("pos"));
        v.color   = jsonToVec3(j.at("color"));
        v.uv      = glm::vec2(j.at("uv").at(0).get<float>(), j.at("uv").at(1).get<float>());
        v.normal  = jsonToVec3(j.at("normal"));
        v.tangent = jsonToVec3(j.at("tangent"));
        return v;
    }

    // Crea el Mesh procedural correspondiente a meshName (case-insensitive),
    // con los mismos parámetros fijos que ScenePanel::createBasicShape. nullptr
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
        // "id" no existe en ficheros .scene guardados antes de este campo —
        // se deja el id que el constructor de GameObject ya asignó (contador
        // atómico), backward-compatible. Cuando sí existe (snapshots propios
        // de Undo/Redo o escenas re-guardadas), se reusa el mismo id: así un
        // Undo de Delete reconstruye el GameObject con el id original y los
        // comandos siguientes en el stack lo siguen resolviendo bien.
        if (j.contains("id"))
            node->id = j.at("id").get<uint64_t>();
        node->localTransform = jsonToMat4(j.at("localTransform"));
        node->worldTransform = parentWorld * node->localTransform;

        if (j.contains("mesh"))
        {
            std::string sourcePath = j["mesh"].value("sourcePath", "");
            std::string meshName   = j["mesh"].value("name", "");
            // "skinned" no existe en ficheros guardados antes de este campo
            // — default false, se reconstruyen como mesh estático (mismo
            // comportamiento que tenían antes de soportar skinned).
            bool skinned = j["mesh"].value("skinned", false);
            try
            {
                if (skinned && !sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));
                    node->setMesh(std::move(mesh));
                }
                else if (!sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath));
                    node->setMesh(std::move(mesh));
                }
                else if (j["mesh"].contains("vertices") && j["mesh"].contains("indices"))
                {
                    // Procedural con geometría serializada (ficheros
                    // guardados con este fix o posteriores): reconstruye el
                    // mesh exacto, sin depender de qué parámetros lo
                    // generaron originalmente.
                    auto mesh = std::make_shared<DonTopo::Mesh>();
                    mesh->name = meshName;
                    for (const auto& vj : j["mesh"]["vertices"])
                        mesh->vertices.push_back(jsonToVertex(vj));
                    mesh->indices = j["mesh"]["indices"].get<std::vector<uint32_t>>();
                    node->setMesh(std::move(mesh));
                }
                else if (auto mesh = proceduralMeshByName(meshName))
                {
                    // Fallback para ficheros guardados ANTES de este fix
                    // (sin vertices/indices) — best-effort con los
                    // parámetros fijos de Basic Shapes, mismo comportamiento
                    // (potencialmente incorrecto para tamaños custom) que
                    // tenían antes.
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
            node->getBoxCollider()->setOwner(node);
            physics.setTrigger(node->getBoxCollider(), c.value("isTrigger", false));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            node->setSphereCollider(physics.createSphereColliderComponent(
                c.at("radius").get<float>(), jsonToVec3(c.at("center")),
                node->worldTransform, c.at("useGravity").get<bool>()));
            node->getSphereCollider()->setOwner(node);
            physics.setTrigger(node->getSphereCollider(), c.value("isTrigger", false));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                c.at("radius").get<float>(), c.at("halfHeight").get<float>(),
                jsonToVec3(c.at("center")), node->worldTransform, c.at("useGravity").get<bool>()));
            node->getCapsuleCollider()->setOwner(node);
            physics.setTrigger(node->getCapsuleCollider(), c.value("isTrigger", false));
        }
        if (j.contains("planeCollider"))
        {
            const auto& c = j["planeCollider"];
            node->setPlaneCollider(physics.createPlaneColliderComponent(
                jsonToVec3(c.at("center")), node->worldTransform));
            node->getPlaneCollider()->setOwner(node);
            physics.setTrigger(node->getPlaneCollider(), c.value("isTrigger", false));
        }
        if (j.contains("audioClip"))
        {
            const auto& c = j["audioClip"];
            auto clip = audio.createAudioClipComponent(
                c.at("path").get<std::string>(), c.at("is3D").get<bool>(), c.at("loop").get<bool>());
            if (clip)
            {
                // .value() con default false: compat con escenas guardadas
                // antes de que existiera este campo.
                clip->setPlayOnAwake(c.value("playOnAwake", false));
                node->setAudioClip(std::move(clip));
            }
            // clip nullptr (asset roto/formato no soportado): node queda sin
            // audio, el resto de la escena sigue cargando.
        }
        if (j.contains("scripts"))
        {
            for (const auto& sj : j["scripts"])
            {
                auto comp = std::make_unique<DonTopo::ScriptComponent>(
                    sj.at("name").get<std::string>(), node);
                if (sj.contains("overrides"))
                {
                    for (const auto& [key, val] : sj["overrides"].items())
                    {
                        if (val.is_boolean())     comp->overrides[key] = val.get<bool>();
                        else if (val.is_string()) comp->overrides[key] = val.get<std::string>();
                        else if (val.is_number()) comp->overrides[key] = val.get<double>();
                        // Otros tipos: ignorados (no son props serializables)
                    }
                }
                // Nota: si el script ya no existe en Scripts/, el componente
                // se conserva igual ("missing script", spec) — la UI lo
                // señala; los overrides no se pierden al re-guardar.
                node->addScript(std::move(comp));
            }
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

    GameObject* Scene::cloneGameObject(GameObject* src, GameObject* parent,
                                       PhysicsManager& physics, AudioManager& audio)
    {
        if (!src || src == &m_root) return nullptr;

        GameObject* target = parent ? parent : (src->parent ? src->parent : &m_root);
        nlohmann::json j = nodeToJson(*src);

        GameObject* clone = target->addChild(src->name + " (Clone)");
        try
        {
            nodeFromJson(j, clone, target->worldTransform, physics, audio);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(clone);
            return nullptr;
        }

        clone->traverse([](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
        });
        return clone;
    }

    GameObject* Scene::findById(uint64_t id)
    {
        GameObject* found = nullptr;
        m_root.traverse([&](GameObject* n) { if (n->id == id) found = n; });
        return found;
    }

    nlohmann::json Scene::subtreeToJson(const GameObject* node) const
    {
        return nodeToJson(*node);
    }

    GameObject* Scene::insertFromJson(const nlohmann::json& j, GameObject* parent, size_t index,
                                       PhysicsManager& physics, AudioManager& audio)
    {
        GameObject* target = parent ? parent : &m_root;
        GameObject* node = target->addChild(j.value("name", std::string()));
        try
        {
            nodeFromJson(j, node, target->worldTransform, physics, audio);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(node);
            return nullptr;
        }

        node->traverse([](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
        });

        // addChild() insertó al final; reposicionar a index si no es ya ahí.
        auto& siblings = target->children;
        size_t insertedAt = siblings.size() - 1;
        if (index < insertedAt)
        {
            auto last = siblings.begin() + static_cast<long>(insertedAt);
            std::rotate(siblings.begin() + static_cast<long>(index), last, last + 1);
        }
        return node;
    }

    void Scene::update(float /*dt*/, PhysicsManager& /*physics*/)
    {
        m_root.traverse([](GameObject* go) {
            auto col = go->anyCollider();
            if (!col) return;

            const bool hasRb     = go->hasRigidbody();
            const bool kinematic = hasRb && go->getRigidbody()->getIsKinematic();
            const bool simulated = hasRb && !kinematic; // cuerpo dinámico real

            if (simulated)
            {
                // PhysX manda: leer pose actor -> GameObject.
                go->worldTransform = col->getWorldTransform();
                glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
            }
            else if (kinematic)
            {
                // Kinematic: empujar pose GameObject -> actor (setKinematicTarget).
                col->syncTransform(go->worldTransform);
            }
            else
            {
                // Solo collider (static): empujar pose por si el editor la movió.
                col->teleport(go->worldTransform);
            }
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
            go->getScripts().clear();
        });
    }

    nlohmann::json Scene::toJson() const
    {
        nlohmann::json root;
        root["version"] = 1;
        root["root"] = nodeToJson(m_root);
        return root;
    }

    bool Scene::save(const std::string& path) const
    {
        return FileManager::writeJson(path, toJson());
    }

    bool Scene::fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio)
    {
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1 ||
            !j.contains("root") || !j["root"].is_object())
            return false;

        const nlohmann::json& rootJson = j["root"];

        // Construye el árbol nuevo en un GameObject temporal, desconectado de
        // m_root: si nodeFromJson lanza a mitad de un nodo interno malformado,
        // el temporal se destruye solo al salir de scope (liberando los
        // colliders/audio ya creados en él — physics/audio siguen vivos) y
        // m_root queda intacto. Garantiza que una carga fallida nunca deja la
        // escena a medio reconstruir, no solo en el chequeo de version/root de
        // arriba sino también ante malformación anidada más abajo en el árbol
        // (spec: "carga fallida no modifica la escena").
        GameObject newRoot(rootJson.value("name", "root"));
        try
        {
            nodeFromJson(rootJson, &newRoot, glm::mat4(1.0f), physics, audio);
        }
        catch (const nlohmann::json::exception&)
        {
            return false;
        }

        shutdown(physics, audio);
        m_root = std::move(newRoot);
        // addChild() (llamado dentro de nodeFromJson vía newRoot.addChild/
        // node->addChild) apunta el parent de cada hijo directo al objeto
        // newRoot original — que era una variable local a esta función. Tras
        // el move-assignment, m_root vive en su propia dirección estable (es
        // un miembro de Scene), así que hay que re-apuntar el parent de los
        // hijos directos a &m_root. Los nietos y descendientes más profundos NO
        // necesitan este arreglo: su parent apunta a su padre inmediato, que
        // vive en el heap vía unique_ptr y no se mueve de dirección con este
        // move-assignment.
        m_root.parent = nullptr;
        for (auto& child : m_root.children)
            child->parent = &m_root;

        m_root.updateWorldTransforms();
        return true;
    }

    bool Scene::load(const std::string& path, PhysicsManager& physics, AudioManager& audio)
    {
        auto parsed = FileManager::readJson(path);
        if (!parsed)
            return false;
        return fromJson(*parsed, physics, audio);
    }
}
