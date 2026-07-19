#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
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
#include <filesystem>
#include <memory>
#include <unordered_map>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>

namespace
{
    using DonTopo::GameObject;
    using DonTopo::Rigidbody;
    using DonTopo::CameraComponent;
    using DonTopo::AnimatorComponent;

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

    // Los enums van como string y no como int: legible en un .scene editado a
    // mano y estable si el enum crece por el medio. Mismo criterio que el "mode"
    // de la cámara.
    const char* paramTypeToStr(AnimatorComponent::ParamType t)
    {
        switch (t)
        {
            case AnimatorComponent::ParamType::Trigger: return "trigger";
            case AnimatorComponent::ParamType::Int:     return "int";
            case AnimatorComponent::ParamType::Float:   return "float";
            default:                                    return "bool";
        }
    }

    AnimatorComponent::ParamType paramTypeFromStr(const std::string& s)
    {
        if (s == "trigger") return AnimatorComponent::ParamType::Trigger;
        if (s == "int")     return AnimatorComponent::ParamType::Int;
        if (s == "float")   return AnimatorComponent::ParamType::Float;
        return AnimatorComponent::ParamType::Bool;
    }

    const char* condTypeToStr(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animationFinished";
            case AnimatorComponent::ConditionType::Int:               return "int";
            case AnimatorComponent::ConditionType::Float:             return "float";
            default:                                                  return "bool";
        }
    }

    AnimatorComponent::ConditionType condTypeFromStr(const std::string& s)
    {
        if (s == "trigger")           return AnimatorComponent::ConditionType::Trigger;
        if (s == "animationFinished") return AnimatorComponent::ConditionType::AnimationFinished;
        if (s == "int")               return AnimatorComponent::ConditionType::Int;
        if (s == "float")             return AnimatorComponent::ConditionType::Float;
        return AnimatorComponent::ConditionType::Bool;
    }

    const char* compareToStr(AnimatorComponent::Compare c)
    {
        switch (c)
        {
            case AnimatorComponent::Compare::Less:      return "less";
            case AnimatorComponent::Compare::Equals:    return "equals";
            case AnimatorComponent::Compare::NotEquals: return "notEquals";
            default:                                    return "greater";
        }
    }

    AnimatorComponent::Compare compareFromStr(const std::string& s)
    {
        if (s == "less")      return AnimatorComponent::Compare::Less;
        if (s == "equals")    return AnimatorComponent::Compare::Equals;
        if (s == "notEquals") return AnimatorComponent::Compare::NotEquals;
        return AnimatorComponent::Compare::Greater;
    }

    nlohmann::json animatorToJson(const AnimatorComponent& a)
    {
        auto states = nlohmann::json::array();
        for (const auto& s : a.states())
        {
            // El clip va por NOMBRE: el índice depende del orden de mAnimations
            // en el FBX, y reexportar el modelo lo baraja en silencio.
            states.push_back({ {"name", s.name},
                               {"clip", s.clipName},
                               {"loop", s.loop},
                               {"pos", nlohmann::json::array({ s.editorPos.x, s.editorPos.y })} });
        }

        auto params = nlohmann::json::array();
        for (const auto& p : a.parameters())
            params.push_back({ {"name", p.name}, {"type", paramTypeToStr(p.type)} });

        auto transitions = nlohmann::json::array();
        for (const auto& t : a.transitions())
        {
            auto conds = nlohmann::json::array();
            for (const auto& c : t.conditions)
            {
                nlohmann::json cj = { {"type", condTypeToStr(c.type)} };
                if (c.type != AnimatorComponent::ConditionType::AnimationFinished)
                    cj["param"] = c.paramName;
                if (c.type == AnimatorComponent::ConditionType::Bool)
                    cj["expected"] = c.expected;
                // Solo las numéricas: en una Bool serían ruido en el .scene.
                if (c.type == AnimatorComponent::ConditionType::Int ||
                    c.type == AnimatorComponent::ConditionType::Float)
                {
                    cj["compare"]   = compareToStr(c.compare);
                    cj["threshold"] = c.threshold;
                }
                conds.push_back(cj);
            }
            // from/to son índices al array "states" de ESTE mismo JSON:
            // self-contained, sin depender de ningún asset externo.
            transitions.push_back({ {"from", t.fromState}, {"to", t.toState}, {"conditions", conds} });
        }

        return { {"entryState", a.entryState()},
                 {"parameters", params},
                 {"states", states},
                 {"transitions", transitions} };
    }

    // No deserializa estado runtime (estado actual, animTime, valores de
    // parámetros, triggers pendientes) porque no se serializa: el Stop de Play
    // reconstruye la escena desde JSON, así que el reset al estado de entrada
    // sale gratis, y guardar en mitad de Play no hornea estado transitorio.
    std::shared_ptr<AnimatorComponent> animatorFromJson(const nlohmann::json& j)
    {
        auto a = std::make_shared<AnimatorComponent>();

        // Parámetros primero: addParameter es quien crea las entradas de bools/
        // triggers que las condiciones consultarán.
        if (j.contains("parameters"))
            for (const auto& p : j["parameters"])
                a->addParameter(p.value("name", std::string()),
                                paramTypeFromStr(p.value("type", std::string("bool"))));

        if (j.contains("states"))
        {
            for (const auto& s : j["states"])
            {
                AnimatorComponent::State st;
                st.name     = s.value("name", std::string());
                st.clipName = s.value("clip", std::string());
                st.loop     = s.value("loop", true);
                if (s.contains("pos") && s["pos"].is_array() && s["pos"].size() == 2)
                    st.editorPos = glm::vec2(s["pos"][0].get<float>(), s["pos"][1].get<float>());
                // duration/ticksPerSecond/clipIndex los rellena bindClips contra
                // el SkinnedMesh: son del FBX, no del fichero de escena.
                a->addState(st);
            }
        }

        if (j.contains("transitions"))
        {
            for (const auto& t : j["transitions"])
            {
                AnimatorComponent::Transition tr;
                tr.fromState = t.value("from", -1);
                tr.toState   = t.value("to", -1);
                if (t.contains("conditions"))
                {
                    for (const auto& c : t["conditions"])
                    {
                        AnimatorComponent::Condition cond;
                        cond.type      = condTypeFromStr(c.value("type", std::string("bool")));
                        cond.paramName = c.value("param", std::string());
                        cond.expected  = c.value("expected", true);
                        // Ausentes en escenas anteriores a los parámetros
                        // numéricos: caen en los defaults del struct.
                        cond.compare   = compareFromStr(c.value("compare", std::string("greater")));
                        cond.threshold = c.value("threshold", 0.0f);
                        tr.conditions.push_back(cond);
                    }
                }
                a->addTransition(tr);
            }
        }

        // Después de addState: setEntryState valida contra m_states.size().
        a->setEntryState(j.value("entryState", 0));
        return a;
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

            // Fuentes de animación: el SkinnedMesh se reconstruye desde los FBX
            // en cada carga, así que sin esto los clips importados de ficheros
            // extra (y los renames) se perderían al guardar.
            if (const DonTopo::SkinnedMesh* sm = node.getSkinnedMesh())
            {
                nlohmann::json sources = nlohmann::json::array();
                for (const auto& src : sm->animationSources)
                    sources.push_back({ {"path", src.path},
                                        {"builtin", src.builtin},
                                        {"clips", src.clipNames} });
                meshJson["animationSources"] = std::move(sources);
            }
            j["mesh"] = std::move(meshJson);
        }
        if (node.hasBoxCollider())
        {
            const auto& c = node.getBoxCollider();
            j["boxCollider"] = { {"halfExtents", vec3ToJson(c->getHalfExtents())},
                                  {"center", vec3ToJson(c->getCenter())},
                                  {"isTrigger", c->isTrigger()} };
        }
        if (node.hasSphereCollider())
        {
            const auto& c = node.getSphereCollider();
            j["sphereCollider"] = { {"radius", c->getRadius()},
                                     {"center", vec3ToJson(c->getCenter())},
                                     {"isTrigger", c->isTrigger()} };
        }
        if (node.hasCapsuleCollider())
        {
            const auto& c = node.getCapsuleCollider();
            j["capsuleCollider"] = { {"radius", c->getRadius()},
                                      {"halfHeight", c->getHalfHeight()},
                                      {"center", vec3ToJson(c->getCenter())},
                                      {"isTrigger", c->isTrigger()} };
        }
        if (node.hasPlaneCollider())
        {
            const auto& c = node.getPlaneCollider();
            j["planeCollider"] = { {"center", vec3ToJson(c->getCenter())},
                                    {"isTrigger", c->isTrigger()} };
        }
        if (node.hasRigidbody())
        {
            const auto& rb = node.getRigidbody();
            j["rigidbody"] = { {"mass", rb->getMass()},
                               {"useGravity", rb->getUseGravity()},
                               {"isKinematic", rb->getIsKinematic()},
                               {"drag", rb->getDrag()},
                               {"angularDrag", rb->getAngularDrag()},
                               {"constraints", rb->getConstraints()} };
        }
        if (node.hasCameraComponent())
        {
            const auto& c = node.getCameraComponent();
            // "mode" como string y no como int del enum: legible en un .scene
            // editado a mano y estable si el enum crece por el medio.
            j["camera"] = { {"mode", c->getMode() == CameraComponent::ProjectionMode::Orthographic
                                         ? "orthographic" : "perspective"},
                            {"fov", c->getFov()},
                            {"orthographicSize", c->getOrthographicSize()},
                            {"near", c->getNear()},
                            {"far", c->getFar()} };
        }
        if (node.hasAnimator())
            j["animator"] = animatorToJson(*node.getAnimator());
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
                       DonTopo::PhysicsManager& physics, DonTopo::AudioManager& audio,
                       std::vector<std::string>* warnings,
                       std::unordered_map<std::string, bool>* hasBonesCache)
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
            // El flag "skinned" se sigue GUARDANDO (dato informativo, y no
            // rompe ficheros viejos) pero ya no se lee: manda el fichero, en
            // carga igual que en import. Si no fuera así, las escenas guardadas
            // antes de la auto-detección — todas con el flag a false, porque el
            // editor nunca creaba skinned — jamás podrían tener Animator sin
            // reimportar la malla a mano.
            const bool skinnedFlag = j["mesh"].value("skinned", false);
            bool skinned = false;
            if (!sourcePath.empty())
            {
                // Cache por-carga (ver hasBonesCache más abajo): sin ella cada
                // nodo que comparte sourcePath con otro repetiría el ReadFile
                // completo de Assimp que hace hasBones.
                if (hasBonesCache)
                {
                    auto it = hasBonesCache->find(sourcePath);
                    if (it != hasBonesCache->end())
                        skinned = it->second;
                    else
                        skinned = (*hasBonesCache)[sourcePath] = DonTopo::ModelLoader::hasBones(sourcePath);
                }
                else
                {
                    skinned = DonTopo::ModelLoader::hasBones(sourcePath);
                }
            }

            // hasBones() devuelve false tanto si el fichero no tiene huesos
            // como si no se puede leer (movido/borrado) — hay que distinguir
            // antes de avisar, porque decir "ya no declara huesos" de un
            // fichero que directamente no existe es peor que no avisar: apunta
            // al sitio equivocado y esconde que la malla no cargó en absoluto.
            if (skinnedFlag && !skinned && !sourcePath.empty() && warnings)
            {
                const std::string file = std::filesystem::path(sourcePath).filename().string();
                if (!std::filesystem::exists(sourcePath))
                {
                    warnings->push_back(file + ": la escena lo tenía guardado como animado, pero el"
                                                " fichero no se encuentra (¿se movió o se borró?);"
                                                " la malla no se puede cargar");
                }
                else
                {
                    warnings->push_back(file + ": la escena lo tenía guardado como animado, pero el fichero"
                                                " ya no declara huesos; se descartan sus fuentes de animación");
                }
            }
            try
            {
                if (skinned)
                {
                    auto mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));

                    // Fuentes de animación. La builtin ya la creó loadSkinned:
                    // de ella solo se recuperan los NOMBRES (un rename), y se
                    // aplican POSICIONALMENTE de una sola vez (no encadenando
                    // renameClip: eso colisiona consigo mismo ante un swap de
                    // dos nombres y no aplica nada) hasta el menor de los dos
                    // tamaños — un FBX reexportado con más o menos clips no
                    // debe romper la carga.
                    if (j["mesh"].contains("animationSources"))
                    {
                        for (const auto& sj : j["mesh"]["animationSources"])
                        {
                            const std::string path = sj.value("path", std::string());
                            const bool builtin     = sj.value("builtin", false);
                            std::vector<std::string> names;
                            if (sj.contains("clips"))
                                names = sj["clips"].get<std::vector<std::string>>();

                            if (builtin)
                            {
                                if (mesh->animationSources.empty()) continue;
                                auto& b = mesh->animationSources[0];
                                std::vector<std::string> renameWarnings;
                                DonTopo::applyClipNamesPositionally(*mesh, b, names, renameWarnings);
                                // Al warnings del parámetro, igual que el resto
                                // de esta función: printf no llega al Log
                                // Console en un build sin consola.
                                if (warnings)
                                    for (const auto& w : renameWarnings)
                                        warnings->push_back(w);
                                continue;
                            }

                            std::vector<std::string> sourceWarnings;
                            if (!DonTopo::addAnimationSource(*mesh, path, sourceWarnings, &names))
                            {
                                // Fichero movido, borrado o de otro rig: se
                                // avisa y se sigue. Los estados que usaran sus
                                // clips quedan huérfanos, y bindClips ya lo
                                // reporta — perder la escena entera por esto
                                // sería mucho peor. Al warnings del parámetro
                                // (Scene::lastWarnings(), lo que lee el Log
                                // Console), no a stdout: en un build sin
                                // consola un printf es invisible.
                                if (warnings)
                                    for (const auto& w : sourceWarnings)
                                        warnings->push_back(w);
                            }
                        }
                    }

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
            catch (const std::exception& e)
            {
                // Asset roto (movido/borrado) o formato no soportado: node
                // queda sin mesh, el resto de la escena sigue cargando. Antes
                // la excepción se tragaba aquí sin más — si el warning de
                // arriba ni siquiera dispara (skinnedFlag == false, o el
                // fichero nunca tuvo huesos) el usuario se queda sin ninguna
                // pista de por qué el mesh está vacío. Se reporta por
                // warnings, no por stdout: en un build sin consola un printf
                // es invisible.
                if (warnings)
                {
                    const std::string ref = sourcePath.empty() ? meshName : sourcePath;
                    warnings->push_back(ref + ": no se pudo cargar la malla (" + e.what() + ")");
                }
            }
        }

        // Los colliders se cargan siempre como static (dynamic=false); si el
        // nodo trae un Rigidbody (o useGravity legacy), el bloque de abajo lo
        // promociona a dynamic vía physics.attachRigidbody.
        if (j.contains("boxCollider"))
        {
            const auto& c = j["boxCollider"];
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.at("halfExtents")), jsonToVec3(c.at("center")),
                node->worldTransform, /*dynamic=*/false));
            node->getBoxCollider()->setOwner(node);
            physics.setTrigger(node->getBoxCollider(), c.value("isTrigger", false));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            node->setSphereCollider(physics.createSphereColliderComponent(
                c.at("radius").get<float>(), jsonToVec3(c.at("center")),
                node->worldTransform, /*dynamic=*/false));
            node->getSphereCollider()->setOwner(node);
            physics.setTrigger(node->getSphereCollider(), c.value("isTrigger", false));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                c.at("radius").get<float>(), c.at("halfHeight").get<float>(),
                jsonToVec3(c.at("center")), node->worldTransform, /*dynamic=*/false));
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

        // Rigidbody: bloque nuevo. Back-compat: escenas viejas guardaban
        // useGravity DENTRO del collider; si no hay bloque rigidbody pero un
        // collider trae useGravity legacy == true, sintetizamos un Rigidbody
        // heredando ese valor (cuerpo dinámico como antes). useGravity legacy
        // == false (kinematic sin gravedad) equivale a un collider static, que
        // es justo el estado por defecto → no se crea Rigidbody.
        auto legacyGravity = [&](const char* key) -> int {
            if (j.contains(key) && j[key].contains("useGravity"))
                return j[key]["useGravity"].get<bool>() ? 1 : 0;
            return -1; // sin campo legacy
        };
        if (j.contains("rigidbody"))
        {
            const auto& r = j["rigidbody"];
            auto rb = std::make_shared<Rigidbody>();
            rb->setMass(r.value("mass", 1.0f));
            rb->setUseGravity(r.value("useGravity", true));
            rb->setIsKinematic(r.value("isKinematic", false));
            rb->setDrag(r.value("drag", 0.0f));
            rb->setAngularDrag(r.value("angularDrag", 0.05f));
            rb->setConstraints(r.value("constraints", 0u));
            node->setRigidbody(rb);
            if (auto col = node->anyCollider()) physics.attachRigidbody(col, rb);
        }
        else
        {
            int g = legacyGravity("boxCollider");
            if (g < 0) g = legacyGravity("sphereCollider");
            if (g < 0) g = legacyGravity("capsuleCollider");
            if (g == 1)
            {
                auto rb = std::make_shared<Rigidbody>();
                rb->setUseGravity(true);
                node->setRigidbody(rb);
                if (auto col = node->anyCollider()) physics.attachRigidbody(col, rb);
            }
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1). Valor de "mode" desconocido ->
        // perspective.
        if (j.contains("camera"))
        {
            const auto& c = j["camera"];
            auto cam = std::make_shared<CameraComponent>();
            cam->setMode(c.value("mode", std::string("perspective")) == "orthographic"
                             ? CameraComponent::ProjectionMode::Orthographic
                             : CameraComponent::ProjectionMode::Perspective);
            // far ANTES que near: setNear clampa contra el far ACTUAL, así que
            // cargarlos al revés recortaría un near grande contra el far por
            // defecto (2000) y lo dejaría mal.
            cam->setFar(c.value("far", 2000.0f));
            cam->setNear(c.value("near", 1.0f));
            cam->setFov(c.value("fov", 45.0f));
            cam->setOrthographicSize(c.value("orthographicSize", 100.0f));
            node->setCameraComponent(cam);
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1).
        if (j.contains("animator"))
        {
            auto anim = animatorFromJson(j["animator"]);
            // El bloque "mesh" se parsea ANTES que este, así que el SkinnedMesh
            // ya está montado y bindClips puede resolver los nombres de clip
            // aquí mismo. Sin malla skinned (grafo huérfano) los clipIndex se
            // quedan a -1 y currentClipIndex cae a 0.
            if (auto* sm = node->getSkinnedMesh())
                anim->bindClips(*sm, warnings);
            node->setAnimator(std::move(anim));
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
            nodeFromJson(childJson, child, node->worldTransform, physics, audio, warnings, hasBonesCache);
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
        // Antes de nodeFromJson: si se limpiara después (como estaba), los
        // avisos que bindClips empuja a m_warnings durante la carga se
        // perderían de inmediato.
        m_warnings.clear();
        try
        {
            nodeFromJson(j, clone, target->worldTransform, physics, audio, &m_warnings, nullptr);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(clone);
            return nullptr;
        }

        clone->traverse([&](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
            // El clon nunca se lleva el CameraComponent: al clonar, el original
            // sigue vivo con su cámara, así que findCamera() ya es no-nulo y el
            // clon rompería el invariante. Determinista, no condicional. Su
            // único caller es Instantiate de Lua (ScriptBindings.cpp), que corre
            // en Play — ningún gate de la UI puede evitarlo, por eso la regla
            // vive aquí.
            if (n->hasCameraComponent())
            {
                n->setCameraComponent(nullptr);
                m_warnings.push_back("Clone de '" + n->name +
                                      "': se descarta el CameraComponent (ya hay una cámara en la escena)");
            }
        });
        return clone;
    }

    GameObject* Scene::findById(uint64_t id)
    {
        GameObject* found = nullptr;
        m_root.traverse([&](GameObject* n) { if (n->id == id) found = n; });
        return found;
    }

    GameObject* Scene::findCamera()
    {
        GameObject* found = nullptr;
        // traverse es pre-orden (fn(this) antes que los hijos) y no permite
        // early-exit: el guard de !found deja ganar a la primera igualmente.
        m_root.traverse([&](GameObject* n) {
            if (!found && n->hasCameraComponent()) found = n;
        });
        return found;
    }

    const GameObject* Scene::findCamera() const
    {
        // traverse es non-const (template en GameObject); el const_cast se
        // queda contenido aquí y la versión const no muta nada.
        return const_cast<Scene*>(this)->findCamera();
    }

    void Scene::pruneExtraCameras()
    {
        GameObject* first = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!n->hasCameraComponent()) return;
            if (!first) { first = n; return; }
            m_warnings.push_back("Escena con más de una cámara: se descarta la de '" + n->name +
                                  "' (se conserva la de '" + first->name + "')");
            n->setCameraComponent(nullptr);
        });
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
            nodeFromJson(j, node, target->worldTransform, physics, audio, &m_warnings, nullptr);
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
                // Solo collider (static): empujar pose SÓLO si cambió. Mover un
                // PxRigidStatic cada frame ensucia el pruner de scene-query de
                // PhysX (y emite warnings), así que se compara la pose actual
                // del actor (T*R, sin escala) con la del GameObject normalizada
                // (quitando escala) y sólo se teleporta si difieren.
                glm::mat4 want = go->worldTransform;
                for (int i = 0; i < 3; ++i)
                {
                    float len = glm::length(glm::vec3(want[i]));
                    if (len > 1e-6f) want[i] = glm::vec4(glm::vec3(want[i]) / len, 0.0f);
                }
                want[3].w = 1.0f;
                glm::mat4 have = col->getWorldTransform();
                bool changed = false;
                for (int i = 0; i < 4 && !changed; ++i)
                    for (int j = 0; j < 4; ++j)
                    {
                        float d = have[i][j] - want[i][j];
                        if (d < 0.0f) d = -d;
                        if (d > 1e-4f) { changed = true; break; }
                    }
                if (changed) col->teleport(go->worldTransform);
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
        m_warnings.clear();
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
        // Cache de hasBones() con vida atada a ESTA llamada a fromJson (local,
        // no miembro ni static): un FBX puede cambiar en disco entre dos
        // cargas de escena dentro de la misma sesión de editor, y un cache que
        // sobreviviera a esta función serviría un resultado stale — cargaría
        // el tipo de malla equivocado sin que nada lo delate. Dentro de una
        // sola carga el fichero es estable, así que compartirla entre los
        // nodos que repiten sourcePath (varios enemigos con el mismo FBX) es
        // seguro y evita repetir el ReadFile completo de Assimp por cada uno.
        std::unordered_map<std::string, bool> hasBonesCache;
        try
        {
            nodeFromJson(rootJson, &newRoot, glm::mat4(1.0f), physics, audio, &m_warnings, &hasBonesCache);
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

        // Tras reconstruir: el fichero puede traer dos cámaras (editado a mano).
        pruneExtraCameras();
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
