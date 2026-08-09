#include <functional>
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
#include "DonTopo/Renderer/AsyncAssetLoader.h"
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
    using DonTopo::ReflectionProbeComponent;
    using DonTopo::LightComponent;
    using DonTopo::AudioListenerComponent;
    using DonTopo::LightType;
    using DonTopo::CanvasComponent;
    using DonTopo::UiScaleMode;
    using DonTopo::UiScreenMatch;
    using DonTopo::ButtonComponent;
    using DonTopo::UiButtonTransition;
    using DonTopo::UiTextAlign;
    using DonTopo::TextComponent;
    using DonTopo::UiTextOverflow;

    // Forward declarations: animatorFromJson (más abajo) necesita estos
    // lectores tolerantes a JSON corrupto (definidos junto a jsonToMat4/
    // jsonToVec3, después en el fichero) para el threshold de las
    // condiciones numéricas y el editorPos de los estados.
    //
    // required (default false, back-compat de toda la vida): la clave/índice
    // AUSENTE también avisa cuando required == true. Es para los campos que
    // nodeToJson escribe SIEMPRE (nunca son opcionales de verdad) — ver el
    // comentario grande de abajo, hallazgo 1 del review de este fix.
    float readFloat(const nlohmann::json& j, const char* key, float def,
                     std::vector<std::string>* warnings, const std::string& contexto,
                     bool required = false);
    float readArrayFloat(const nlohmann::json& arr, size_t idx, float def,
                          std::vector<std::string>* warnings, const std::string& contexto,
                          bool required = false);

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

    const char* lightTypeToStr(LightType t)
    {
        switch (t)
        {
            case LightType::Spot:        return "spot";
            case LightType::Directional: return "directional";
            case LightType::Area:        return "area";
            default:                     return "point";
        }
    }

    LightType lightTypeFromStr(const std::string& s)
    {
        if (s == "spot")        return LightType::Spot;
        if (s == "directional") return LightType::Directional;
        if (s == "area")        return LightType::Area;
        return LightType::Point;    // valor desconocido -> point
    }

    const char* uiScaleModeToStr(UiScaleMode m)
    {
        switch (m)
        {
            case UiScaleMode::ScaleWithScreenSize:  return "scaleWithScreenSize";
            case UiScaleMode::ConstantPhysicalSize: return "constantPhysicalSize";
            default:                                return "constantPixelSize";
        }
    }

    UiScaleMode uiScaleModeFromStr(const std::string& s)
    {
        if (s == "scaleWithScreenSize")  return UiScaleMode::ScaleWithScreenSize;
        if (s == "constantPhysicalSize") return UiScaleMode::ConstantPhysicalSize;
        return UiScaleMode::ConstantPixelSize;  // valor desconocido -> el default
    }

    const char* uiScreenMatchToStr(UiScreenMatch m)
    {
        switch (m)
        {
            case UiScreenMatch::Expand: return "expand";
            case UiScreenMatch::Shrink: return "shrink";
            default:                    return "matchWidthOrHeight";
        }
    }

    UiScreenMatch uiScreenMatchFromStr(const std::string& s)
    {
        if (s == "expand") return UiScreenMatch::Expand;
        if (s == "shrink") return UiScreenMatch::Shrink;
        return UiScreenMatch::MatchWidthOrHeight;   // valor desconocido -> el default
    }

    const char* uiButtonTransitionToStr(UiButtonTransition t)
    {
        switch (t)
        {
            case UiButtonTransition::SpriteSwap: return "spriteSwap";
            case UiButtonTransition::Animation:  return "animation";
            default:                             return "colorTint";
        }
    }

    UiButtonTransition uiButtonTransitionFromStr(const std::string& s)
    {
        if (s == "spriteSwap") return UiButtonTransition::SpriteSwap;
        if (s == "animation")  return UiButtonTransition::Animation;
        return UiButtonTransition::ColorTint;   // valor desconocido -> el default
    }

    const char* uiTextAlignToStr(UiTextAlign a)
    {
        switch (a)
        {
            case UiTextAlign::Center:  return "center";
            case UiTextAlign::Right:   return "right";
            case UiTextAlign::Justify: return "justify";
            default:                   return "left";
        }
    }

    UiTextAlign uiTextAlignFromStr(const std::string& s)
    {
        if (s == "center")  return UiTextAlign::Center;
        if (s == "right")   return UiTextAlign::Right;
        if (s == "justify") return UiTextAlign::Justify;
        return UiTextAlign::Left;   // valor desconocido -> el default
    }

    const char* uiTextOverflowToStr(UiTextOverflow o)
    {
        switch (o)
        {
            case UiTextOverflow::Clip:     return "clip";
            case UiTextOverflow::Ellipsis: return "ellipsis";
            default:                       return "overflow";
        }
    }

    UiTextOverflow uiTextOverflowFromStr(const std::string& s)
    {
        if (s == "clip")     return UiTextOverflow::Clip;
        if (s == "ellipsis") return UiTextOverflow::Ellipsis;
        return UiTextOverflow::Overflow;   // valor desconocido -> el default
    }

    // Los vectores del Button van como objeto con componentes nombradas, igual
    // que referenceResolution y safeArea del canvas (y no como el array de
    // vec3ToJson): un .scene editado a mano se lee mejor, y readFloat ya tolera
    // un null por campo sin tumbar la carga entera.
    nlohmann::json vec2ToJsonXY(const glm::vec2& v)
    {
        return { {"x", v.x}, {"y", v.y} };
    }

    nlohmann::json vec4ToJsonXYZW(const glm::vec4& v)
    {
        return { {"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w} };
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
    std::shared_ptr<AnimatorComponent> animatorFromJson(const nlohmann::json& j,
                                                          std::vector<std::string>* warnings)
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
                    st.editorPos = glm::vec2(readArrayFloat(s["pos"], 0, 0.0f, warnings, "animator.state." + st.name + ".pos"),
                                              readArrayFloat(s["pos"], 1, 0.0f, warnings, "animator.state." + st.name + ".pos"));
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
                        cond.threshold = readFloat(c, "threshold", 0.0f, warnings,
                                                    "animator.transition[" + std::to_string(tr.fromState) +
                                                    "->" + std::to_string(tr.toState) + "].condition");
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
        // SSR por objeto. Se guarda SIEMPRE (no dentro de un if) para que apagarlo
        // sobre un objeto que lo tenía puesto quede grabado; en ficheros viejos no
        // existe y nodeFromJson cae al default (apagado), que es como se veían.
        j["ssrEnabled"]   = node.ssrEnabled;
        j["ssrIntensity"] = node.ssrIntensity;

        if (node.hasMesh())
        {
            const auto& mesh = node.getMesh();
            // "visible" se guarda SIEMPRE: en ficheros viejos no existe y la
            // carga cae al default true, que es como se veían.
            nlohmann::json meshJson = { {"sourcePath", mesh->sourcePath}, {"name", mesh->name}, {"skinned", node.isSkinned()},
                                        {"visible", node.meshVisible} };
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
        if (node.hasReflectionProbe())
        {
            // Solo los ajustes: el cubemap bakeado NO se serializa (es un
            // recurso GPU de ~1,1 MB por sonda). Al cargar la escena, el
            // Renderer rehornea las sondas que no tienen captura, así que
            // DonTopoRuntime acaba viendo exactamente lo mismo que el editor.
            const auto& p = node.getReflectionProbe();
            j["reflectionProbe"] = { {"radius", p->getRadius()},
                                     {"intensity", p->getIntensity()} };
        }
        if (node.hasLight())
        {
            // Ni posición ni dirección: las dos salen del worldTransform, que ya
            // se serializa como localTransform del nodo. "type" como string y no
            // como int del enum, mismo criterio que el "mode" de la cámara.
            const auto& l = node.getLight();
            j["light"] = { {"type", lightTypeToStr(l->getType())},
                           {"color", vec3ToJson(l->getColor())},
                           {"intensity", l->getIntensity()},
                           {"range", l->getRange()},
                           {"innerAngle", l->getInnerAngle()},
                           {"outerAngle", l->getOuterAngle()},
                           {"areaWidth", l->getAreaWidth()},
                           {"areaHeight", l->getAreaHeight()} };
        }
        if (node.hasCanvas())
        {
            const auto& c = node.getCanvas();
            j["canvas"] = { {"scaleMode", uiScaleModeToStr(c->scaleMode)},
                            {"scaleFactor", c->scaleFactor},
                            {"referenceResolution", { {"x", c->referenceResolution.x},
                                                      {"y", c->referenceResolution.y} }},
                            {"screenMatch", uiScreenMatchToStr(c->screenMatch)},
                            {"matchWidthOrHeight", c->matchWidthOrHeight},
                            {"screenDpi", c->screenDpi},
                            {"fallbackDpi", c->fallbackDpi},
                            {"referenceDpi", c->referenceDpi},
                            {"safeArea", { {"left", c->safeArea.left},
                                           {"top", c->safeArea.top},
                                           {"right", c->safeArea.right},
                                           {"bottom", c->safeArea.bottom} }},
                            {"aspectRatio", c->aspectRatio} };
        }
        if (node.hasButton())
        {
            const auto& b = node.getButton();
            j["button"] = { {"anchorMin", vec2ToJsonXY(b->anchorMin)},
                            {"anchorMax", vec2ToJsonXY(b->anchorMax)},
                            {"pivot", vec2ToJsonXY(b->pivot)},
                            {"position", vec2ToJsonXY(b->position)},
                            {"size", vec2ToJsonXY(b->size)},
                            {"color", vec4ToJsonXYZW(b->color)},
                            {"visible", b->visible},
                            {"atlasPath", b->atlasPath},
                            {"sprite", b->sprite},
                            {"interactable", b->interactable},
                            {"selected", b->selected},
                            {"transition", uiButtonTransitionToStr(b->transition)},
                            {"normalColor", vec4ToJsonXYZW(b->normalColor)},
                            {"hoverColor", vec4ToJsonXYZW(b->hoverColor)},
                            {"pressedColor", vec4ToJsonXYZW(b->pressedColor)},
                            {"disabledColor", vec4ToJsonXYZW(b->disabledColor)},
                            {"selectedColor", vec4ToJsonXYZW(b->selectedColor)},
                            {"normalSprite", b->normalSprite},
                            {"hoverSprite", b->hoverSprite},
                            {"pressedSprite", b->pressedSprite},
                            {"disabledSprite", b->disabledSprite},
                            {"selectedSprite", b->selectedSprite},
                            {"fadeDuration", b->fadeDuration},
                            {"text", b->text},
                            {"fontPath", b->fontPath},
                            {"fontSize", b->fontSize},
                            {"textColor", vec4ToJsonXYZW(b->textColor)},
                            {"textAlign", uiTextAlignToStr(b->textAlign)} };
        }
        if (node.hasText())
        {
            const auto& t = node.getText();
            j["text"] = { {"anchorMin", vec2ToJsonXY(t->anchorMin)},
                          {"anchorMax", vec2ToJsonXY(t->anchorMax)},
                          {"pivot", vec2ToJsonXY(t->pivot)},
                          {"position", vec2ToJsonXY(t->position)},
                          {"size", vec2ToJsonXY(t->size)},
                          {"color", vec4ToJsonXYZW(t->color)},
                          {"visible", t->visible},
                          {"text", t->text},
                          {"fontPath", t->fontPath},
                          {"fontSize", t->fontSize},
                          {"outlineWidth", t->outlineWidth},
                          {"outlineColor", vec4ToJsonXYZW(t->outlineColor)},
                          {"shadowOffset", vec2ToJsonXY(t->shadowOffset)},
                          {"shadowColor", vec4ToJsonXYZW(t->shadowColor)},
                          {"align", uiTextAlignToStr(t->align)},
                          {"overflow", uiTextOverflowToStr(t->overflow)},
                          {"wordWrap", t->wordWrap},
                          {"boldStrength", t->boldStrength},
                          {"italicSkew", t->italicSkew} };
        }
        if (node.hasAnimator())
            j["animator"] = animatorToJson(*node.getAnimator());
        if (node.hasAudioClip())
        {
            const auto& clip = node.getAudioClip();
            j["audioClip"] = { {"path", clip->getPath()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()},
                                {"playOnAwake", clip->getPlayOnAwake()},
                                {"volume", clip->getVolume()},
                                {"pitch", clip->getPitch()},
                                {"minDistance", clip->getMinDistance()},
                                {"maxDistance", clip->getMaxDistance()} };
        }
        if (node.hasAudioListener())
        {
            // Ni posición ni orientación: salen del worldTransform, que ya se
            // serializa como localTransform del nodo.
            j["audioListener"] = { {"enabled", node.getAudioListener()->getEnabled()} };
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

    // --- Lectura tolerante de números desde un .scene potencialmente corrupto ---
    //
    // std::clamp(NaN, lo, hi) devuelve NaN (toda comparación con NaN es
    // falsa, así que el clamp no lo detiene) y nlohmann serializa un NaN
    // como JSON "null". Un valor así llegado desde un script Lua roto (un
    // 0/0, por ejemplo — ver el guard equivalente en ScriptBindings.cpp)
    // pasa el clamp de setVolume/setPitch/etc., se cuela en el .scene como
    // null y, al releerlo, tanto ".at(key).get<float>()" como
    // ".value(key, default)" lanzan json::exception (type_error.302, "type
    // must be number, but is null"). Antes de este fix esa excepción escapaba
    // de nodeFromJson sin que nadie la distinguiera de una escena realmente
    // corrupta, y Scene::fromJson la capturaba devolviendo false: UN solo
    // campo corrupto tumbaba la carga de la escena ENTERA. Los infinitos, en
    // cambio, el clamp de rango sí los para bien (clamp(+inf,0,1) == 1.0) —
    // el peligroso de verdad es el NaN, no el infinito; se comprueba con
    // std::isfinite (cubre ambos) por robustez, pero es el caso NaN el que
    // motiva este bloque entero.
    //
    // warnings acepta nullptr por robustez de la firma, pero en la práctica
    // nunca lo es: los 9 call-sites de jsonToVec3 (y, en cascada, todo lo que
    // cuelga de nodeFromJson) pasan &m_warnings — los tres callers de
    // nodeFromJson (fromJson, insertFromJson, cloneGameObject) lo hacen
    // siempre.
    //
    // required distingue dos familias de campos:
    //  - required == false (default): back-compat legítima. Son campos que se
    //    añadieron a lo largo de la vida del formato (volume, pitch, fov,
    //    near, far, mass, drag, threshold...) y una escena vieja nunca los
    //    escribió. Ausente -> default silencioso, sin aviso.
    //  - required == true: campos que nodeToJson escribe SIEMPRE, incondicio-
    //    nalmente (halfExtents/center de los colliders, pos/color/uv/normal/
    //    tangent de cada vértice...). Ahí la ausencia NUNCA es back-compat:
    //    es la misma corrupción (merge mal resuelto, escritura truncada,
    //    edición a mano) que un valor null o no finito, así que también avisa
    //    nombrando el campo y el objeto en vez de fabricar en silencio un
    //    valor plausible (una caja de 25 unidades en el origen que el usuario
    //    ve, no cuestiona, y acaba sobrescribiendo el dato real al Guardar).

    // Lee j[key] como float. Ausente: silencioso si !required, avisa si
    // required. Valor null, tipo no numérico, o número no finito (NaN/Inf):
    // SIEMPRE avisa (si hay canal) nombrando el campo, y cae a def.
    float readFloat(const nlohmann::json& j, const char* key, float def,
                     std::vector<std::string>* warnings, const std::string& contexto,
                     bool required)
    {
        if (!j.contains(key))
        {
            if (required && warnings)
                warnings->push_back(contexto + "." + key +
                                     ": falta en la escena, se usa el valor por defecto");
            return def;
        }
        const nlohmann::json& v = j[key];
        if (v.is_null() || !v.is_number())
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": valor corrupto en la escena, se usa el valor por defecto");
            return def;
        }
        float f = v.get<float>();
        if (!std::isfinite(f))
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": valor no finito (NaN/Inf) en la escena, se usa el valor por defecto");
            return def;
        }
        return f;
    }

    // Los dos vectores "con componentes nombradas" del Button. Cada componente
    // pasa por readFloat, así que un null (NaN serializado) o un tipo raro cae
    // al default y avisa en vez de tumbar la carga de la escena entera.
    glm::vec2 readVec2XY(const nlohmann::json& j, const char* key, const glm::vec2& def,
                          std::vector<std::string>* warnings, const std::string& contexto)
    {
        if (!j.contains(key) || !j[key].is_object()) return def;
        const nlohmann::json& v = j[key];
        const std::string ctx = contexto + "." + key;
        return glm::vec2(readFloat(v, "x", def.x, warnings, ctx),
                          readFloat(v, "y", def.y, warnings, ctx));
    }

    glm::vec4 readVec4XYZW(const nlohmann::json& j, const char* key, const glm::vec4& def,
                            std::vector<std::string>* warnings, const std::string& contexto)
    {
        if (!j.contains(key) || !j[key].is_object()) return def;
        const nlohmann::json& v = j[key];
        const std::string ctx = contexto + "." + key;
        return glm::vec4(readFloat(v, "x", def.x, warnings, ctx),
                          readFloat(v, "y", def.y, warnings, ctx),
                          readFloat(v, "z", def.z, warnings, ctx),
                          readFloat(v, "w", def.w, warnings, ctx));
    }

    // Variante de readFloat para un ELEMENTO de un array JSON por índice (en
    // vez de una clave de objeto) — la usan jsonToVec3/jsonToMat4/uv.
    //
    // OJO: "arr no es un array" y "arr es un array pero más corto de lo
    // esperado" son dos anomalías DISTINTAS y se tratan distinto (hallazgo 2
    // del review): un valor no-array (típicamente null — la forma exacta que
    // toma un NaN serializado, ver comentario grande de arriba) es corrupción
    // de verdad y avisa SIEMPRE, sea o no required el campo. Un array corto
    // (índice fuera de rango) es la firma de "campo ausente" cuando el
    // llamador lo extrajo con ".value(key, array())": ahí sí aplica la regla
    // de required, igual que en readFloat.
    float readArrayFloat(const nlohmann::json& arr, size_t idx, float def,
                          std::vector<std::string>* warnings, const std::string& contexto,
                          bool required)
    {
        if (!arr.is_array())
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: se esperaba un número y no lo es, se usa el valor por defecto");
            return def;
        }
        if (idx >= arr.size())
        {
            if (required && warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: falta en la escena, se usa el valor por defecto");
            return def;
        }
        const nlohmann::json& v = arr[idx];
        if (v.is_null() || !v.is_number())
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: valor corrupto en la escena, se usa el valor por defecto");
            return def;
        }
        float f = v.get<float>();
        if (!std::isfinite(f))
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: valor no finito (NaN/Inf) en la escena, se usa el valor por defecto");
            return def;
        }
        return f;
    }

    // A diferencia de jsonToVec3 (que rellena componente a componente), aquí
    // CUALQUIER float corrupto de los 16 descarta la matriz entera y cae a la
    // identidad: una transformación "a medias" (15 valores originales + 1
    // puesto a su valor de identidad) puede parecer plausible y en realidad
    // tener la escala o la rotación rotas de forma silenciosa — preferible
    // una identidad reconocible y un aviso claro a un Frankenstein de campos
    // mezclados. Ver el bloque de comentarios de arriba para el porqué NaN.
    glm::mat4 jsonToMat4(const nlohmann::json& j, std::vector<std::string>* warnings,
                          const std::string& contexto)
    {
        bool ok = j.is_array() && j.size() >= 16;
        for (int i = 0; ok && i < 16; ++i)
            ok = j[i].is_number() && std::isfinite(j[i].get<float>());
        if (!ok)
        {
            if (warnings)
                warnings->push_back(contexto + ": localTransform corrupto en la escena "
                                     "(valor no numérico, ausente o no finito); se usa la matriz identidad");
            return glm::mat4(1.0f);
        }
        glm::mat4 m(1.0f);
        float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            p[i] = j[i].get<float>();
        return m;
    }

    // required se reenvía tal cual a los 3 readArrayFloat: un vec3 required
    // ausente o corrupto avisa 3 veces (una por componente), pero cada línea
    // ya nombra el objeto y el campo (contexto), así que sigue siendo
    // diagnosticable — no merece la complejidad de deduplicar en un único
    // aviso a nivel de vec3.
    glm::vec3 jsonToVec3(const nlohmann::json& j, std::vector<std::string>* warnings,
                         const std::string& contexto, const glm::vec3& def = glm::vec3(0.0f),
                         bool required = false)
    {
        return glm::vec3(readArrayFloat(j, 0, def.x, warnings, contexto, required),
                          readArrayFloat(j, 1, def.y, warnings, contexto, required),
                          readArrayFloat(j, 2, def.z, warnings, contexto, required));
    }

    // Vertex: nodeToJson lo escribe SIEMPRE con sus 5 campos completos (nunca
    // es opcional un vértice "a medias") — todos required (hallazgo 1 del
    // review).
    DonTopo::Vertex jsonToVertex(const nlohmann::json& j, std::vector<std::string>* warnings,
                                  const std::string& contexto)
    {
        DonTopo::Vertex v{};
        v.pos     = jsonToVec3(j.value("pos",    nlohmann::json::array()), warnings, contexto + ".pos", glm::vec3(0.0f), true);
        v.color   = jsonToVec3(j.value("color",  nlohmann::json::array()), warnings, contexto + ".color", glm::vec3(1.0f), true);
        const nlohmann::json uv = j.value("uv", nlohmann::json::array());
        v.uv      = glm::vec2(readArrayFloat(uv, 0, 0.0f, warnings, contexto + ".uv", true),
                               readArrayFloat(uv, 1, 0.0f, warnings, contexto + ".uv", true));
        v.normal  = jsonToVec3(j.value("normal",  nlohmann::json::array()), warnings, contexto + ".normal", glm::vec3(0.0f, 1.0f, 0.0f), true);
        v.tangent = jsonToVec3(j.value("tangent", nlohmann::json::array()), warnings, contexto + ".tangent", glm::vec3(1.0f, 0.0f, 0.0f), true);
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
                       std::unordered_map<std::string, bool>* hasBonesCache,
                       DonTopo::AsyncAssetLoader* loader = nullptr,
                       const DonTopo::PreloadedMeshCache* preloaded = nullptr)
    {
        // "id" no existe en ficheros .scene guardados antes de este campo —
        // se deja el id que el constructor de GameObject ya asignó (contador
        // atómico), backward-compatible. Cuando sí existe (snapshots propios
        // de Undo/Redo o escenas re-guardadas), se reusa el mismo id: así un
        // Undo de Delete reconstruye el GameObject con el id original y los
        // comandos siguientes en el stack lo siguen resolviendo bien.
        if (j.contains("id"))
            node->id = j.at("id").get<uint64_t>();
        node->localTransform = jsonToMat4(j.value("localTransform", nlohmann::json::array()),
                                           warnings, "localTransform de '" + node->name + "'");
        node->worldTransform = parentWorld * node->localTransform;

        node->ssrEnabled = j.value("ssrEnabled", false);
        // La comparación al revés cubre también un NaN, que pasaría cualquier
        // clamp escrito como min/max y acabaría multiplicando el color del
        // reflejo por NaN.
        const float ssrI   = j.value("ssrIntensity", 0.5f);
        node->ssrIntensity = (ssrI >= 0.0f && ssrI <= 1.0f) ? ssrI : 0.5f;

        if (j.contains("mesh"))
        {
            std::string sourcePath = j["mesh"].value("sourcePath", "");
            std::string meshName   = j["mesh"].value("name", "");
            node->meshVisible      = j["mesh"].value("visible", true);
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
                // Path COMPLETO, no filename(): en el caso de fichero ausente
                // este aviso y el del catch de abajo disparan para el mismo
                // nodo, y con identificadores distintos el Log Console parecía
                // estar hablando de dos assets diferentes.
                if (!std::filesystem::exists(sourcePath))
                {
                    warnings->push_back(sourcePath + ": la escena lo tenía guardado como animado, pero el"
                                                      " fichero no se encuentra (¿se movió o se borró?);"
                                                      " la malla no se puede cargar");
                }
                else
                {
                    warnings->push_back(sourcePath + ": la escena lo tenía guardado como animado, pero el fichero"
                                                      " ya no declara huesos; se descartan sus fuentes de animación");
                }
            }
            try
            {
                if (skinned)
                {
                    // La carga skinned se queda SÍNCRONA a propósito, aunque
                    // haya loader: reconstruye la config de clips del Animator
                    // (applyClipNamesPositionally/addAnimationSource, más abajo)
                    // a partir del JSON guardado y del SkinnedMesh ya cargado.
                    // El pump asíncrono (Task 9, applyLoadedMesh) solo hace
                    // addSkinnedMesh + setMesh — no reaplica esa config — así
                    // que una carga async de escena perdería en silencio los
                    // clips guardados. El drop en vivo (PropertiesPanel) sí es
                    // seguro async porque un FBX recién soltado no trae clips
                    // guardados que reconstruir.
                    //
                    // Cache de precarga: si el runtime ya cargó este FBX en
                    // paralelo (loadAuto → SkinnedMesh para un rig), se usa una
                    // COPIA PROFUNDA en vez del loadSkinned de disco. La config
                    // de clips de abajo se reaplica igual sobre la copia, así que
                    // el resultado es equivalente al camino síncrono sin repetir
                    // el ReadFile. Miss (o entrada no-skinned inesperada) → disco.
                    std::shared_ptr<DonTopo::SkinnedMesh> mesh;
                    if (preloaded)
                    {
                        auto it = preloaded->find(sourcePath);
                        if (it != preloaded->end())
                            if (const auto* sk = dynamic_cast<const DonTopo::SkinnedMesh*>(it->second.get()))
                                mesh = std::make_shared<DonTopo::SkinnedMesh>(*sk);
                    }
                    if (!mesh)
                        mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));

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
                    // Cache de precarga primero: si el runtime ya leyó este
                    // fichero en paralelo, se usa una COPIA PROFUNDA en vez del
                    // disco (o de encolar una petición). Un rig cacheado como
                    // SkinnedMesh se copia como tal por robustez, aunque en la
                    // rama estática lo normal es un Mesh plano.
                    std::shared_ptr<DonTopo::Mesh> cached;
                    if (preloaded)
                    {
                        auto it = preloaded->find(sourcePath);
                        if (it != preloaded->end() && it->second)
                        {
                            if (const auto* sk = dynamic_cast<const DonTopo::SkinnedMesh*>(it->second.get()))
                                cached = std::make_shared<DonTopo::SkinnedMesh>(*sk);
                            else
                                cached = std::make_shared<DonTopo::Mesh>(*it->second);
                        }
                    }

                    if (cached)
                    {
                        node->setMesh(std::move(cached));
                    }
                    else if (loader)
                    {
                        // Asíncrono: el GameObject queda sin mesh y se apunta a
                        // la petición. El pump lo resolverá por id — nunca por
                        // puntero, que sería dangling si el usuario lo borra
                        // mientras carga.
                        node->pendingMeshJob = loader->requestMesh(sourcePath, node->id);
                    }
                    else
                    {
                        auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath));
                        node->setMesh(std::move(mesh));
                    }
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
                        mesh->vertices.push_back(jsonToVertex(vj, warnings, "mesh de '" + node->name + "'"));
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
            const std::string ctx = "boxCollider de '" + node->name + "'";
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.value("halfExtents", nlohmann::json::array()), warnings, ctx + ".halfExtents", glm::vec3(25.0f), true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getBoxCollider()->setOwner(node);
            physics.setTrigger(node->getBoxCollider(), c.value("isTrigger", false));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            const std::string ctx = "sphereCollider de '" + node->name + "'";
            node->setSphereCollider(physics.createSphereColliderComponent(
                readFloat(c, "radius", 25.0f, warnings, ctx, true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getSphereCollider()->setOwner(node);
            physics.setTrigger(node->getSphereCollider(), c.value("isTrigger", false));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            const std::string ctx = "capsuleCollider de '" + node->name + "'";
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                readFloat(c, "radius", 15.0f, warnings, ctx, true),
                readFloat(c, "halfHeight", 25.0f, warnings, ctx, true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getCapsuleCollider()->setOwner(node);
            physics.setTrigger(node->getCapsuleCollider(), c.value("isTrigger", false));
        }
        if (j.contains("planeCollider"))
        {
            const auto& c = j["planeCollider"];
            node->setPlaneCollider(physics.createPlaneColliderComponent(
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, "planeCollider de '" + node->name + "'.center", glm::vec3(0.0f), true),
                node->worldTransform));
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
            const std::string ctx = "rigidbody de '" + node->name + "'";
            auto rb = std::make_shared<Rigidbody>();
            rb->setMass(readFloat(r, "mass", 1.0f, warnings, ctx));
            rb->setUseGravity(r.value("useGravity", true));
            rb->setIsKinematic(r.value("isKinematic", false));
            rb->setDrag(readFloat(r, "drag", 0.0f, warnings, ctx));
            rb->setAngularDrag(readFloat(r, "angularDrag", 0.05f, warnings, ctx));
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
            const std::string ctx = "camera de '" + node->name + "'";
            auto cam = std::make_shared<CameraComponent>();
            cam->setMode(c.value("mode", std::string("perspective")) == "orthographic"
                             ? CameraComponent::ProjectionMode::Orthographic
                             : CameraComponent::ProjectionMode::Perspective);
            // far ANTES que near: setNear clampa contra el far ACTUAL, así que
            // cargarlos al revés recortaría un near grande contra el far por
            // defecto (2000) y lo dejaría mal.
            cam->setFar(readFloat(c, "far", 2000.0f, warnings, ctx));
            cam->setNear(readFloat(c, "near", 1.0f, warnings, ctx));
            cam->setFov(readFloat(c, "fov", 45.0f, warnings, ctx));
            cam->setOrthographicSize(readFloat(c, "orthographicSize", 100.0f, warnings, ctx));
            node->setCameraComponent(cam);
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1). Sin este bloque no hay sonda, y
        // sin sonda el objeto se ilumina con el IBL global de siempre.
        if (j.contains("reflectionProbe"))
        {
            const auto& p = j["reflectionProbe"];
            const std::string ctx = "reflectionProbe de '" + node->name + "'";
            auto probe = std::make_shared<ReflectionProbeComponent>();
            probe->setRadius(readFloat(p, "radius", 300.0f, warnings, ctx));
            probe->setIntensity(readFloat(p, "intensity", 1.0f, warnings, ctx));
            node->setReflectionProbe(probe);
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1). Valor de "type" desconocido ->
        // point.
        if (j.contains("light"))
        {
            const auto& l = j["light"];
            const std::string ctx = "light de '" + node->name + "'";
            auto light = std::make_shared<LightComponent>();
            light->setType(lightTypeFromStr(l.value("type", std::string("point"))));
            light->setColor(jsonToVec3(l.value("color", nlohmann::json::array()),
                                       warnings, ctx + ".color", glm::vec3(1.0f)));
            light->setIntensity(readFloat(l, "intensity", 1.0f, warnings, ctx));
            light->setRange(readFloat(l, "range", 300.0f, warnings, ctx));
            // Los dos setters mantienen inner <= outer entre ellos, así que un
            // .scene con el cono invertido acaba con un cono válido pase lo que
            // pase (el segundo setter arrastra al primero).
            light->setOuterAngle(readFloat(l, "outerAngle", 30.0f, warnings, ctx));
            light->setInnerAngle(readFloat(l, "innerAngle", 20.0f, warnings, ctx));
            light->setAreaWidth(readFloat(l, "areaWidth", 100.0f, warnings, ctx));
            light->setAreaHeight(readFloat(l, "areaHeight", 100.0f, warnings, ctx));
            node->setLight(light);
        }
        // Bloque aditivo: una escena guardada antes del componente Canvas no
        // trae la clave y carga igual, sin componente y sin avisos.
        if (j.contains("canvas"))
        {
            const auto& c = j["canvas"];
            const std::string ctx = "canvas de '" + node->name + "'";
            auto canvas = std::make_shared<CanvasComponent>();
            canvas->scaleMode = uiScaleModeFromStr(
                c.value("scaleMode", std::string("constantPixelSize")));
            canvas->scaleFactor = readFloat(c, "scaleFactor", 1.0f, warnings, ctx);
            const nlohmann::json ref = c.value("referenceResolution", nlohmann::json::object());
            canvas->referenceResolution.x = readFloat(ref, "x", 1920.0f, warnings,
                                                      ctx + ".referenceResolution");
            canvas->referenceResolution.y = readFloat(ref, "y", 1080.0f, warnings,
                                                      ctx + ".referenceResolution");
            canvas->screenMatch = uiScreenMatchFromStr(
                c.value("screenMatch", std::string("matchWidthOrHeight")));
            canvas->matchWidthOrHeight = readFloat(c, "matchWidthOrHeight", 0.5f, warnings, ctx);
            canvas->screenDpi    = readFloat(c, "screenDpi", 0.0f, warnings, ctx);
            canvas->fallbackDpi  = readFloat(c, "fallbackDpi", 96.0f, warnings, ctx);
            canvas->referenceDpi = readFloat(c, "referenceDpi", 96.0f, warnings, ctx);
            const nlohmann::json safe = c.value("safeArea", nlohmann::json::object());
            canvas->safeArea.left   = readFloat(safe, "left", 0.0f, warnings, ctx + ".safeArea");
            canvas->safeArea.top    = readFloat(safe, "top", 0.0f, warnings, ctx + ".safeArea");
            canvas->safeArea.right  = readFloat(safe, "right", 0.0f, warnings, ctx + ".safeArea");
            canvas->safeArea.bottom = readFloat(safe, "bottom", 0.0f, warnings, ctx + ".safeArea");
            canvas->aspectRatio = readFloat(c, "aspectRatio", 0.0f, warnings, ctx);
            node->setCanvas(std::move(canvas));
        }
        // Bloque aditivo, misma regla que el canvas: una escena guardada antes
        // del componente Button no trae la clave y se carga sin él ni un aviso.
        if (j.contains("button"))
        {
            const auto& b = j["button"];
            const std::string ctx = "button de '" + node->name + "'";
            // Un bool o un string corrupto (null, o del tipo que no toca) cae al
            // default en vez de lanzar: .value() sí lanza con un null, y un
            // campo roto no puede tumbar la carga de la escena entera.
            auto readBool = [&](const char* key, bool def) {
                return (b.contains(key) && b[key].is_boolean()) ? b[key].get<bool>() : def;
            };
            auto readStr = [&](const char* key) {
                return (b.contains(key) && b[key].is_string())
                           ? b[key].get<std::string>() : std::string();
            };
            auto btn = std::make_shared<ButtonComponent>();
            btn->anchorMin = readVec2XY(b, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            btn->anchorMax = readVec2XY(b, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            btn->pivot     = readVec2XY(b, "pivot", glm::vec2(0.0f), warnings, ctx);
            btn->position  = readVec2XY(b, "position", glm::vec2(0.0f), warnings, ctx);
            btn->size      = readVec2XY(b, "size", glm::vec2(160.0f, 40.0f), warnings, ctx);
            btn->color     = readVec4XYZW(b, "color", glm::vec4(1.0f), warnings, ctx);
            btn->visible   = readBool("visible", true);
            btn->atlasPath = readStr("atlasPath");
            btn->sprite    = readStr("sprite");

            btn->interactable = readBool("interactable", true);
            btn->selected     = readBool("selected", false);
            btn->transition   = uiButtonTransitionFromStr(readStr("transition"));

            btn->normalColor   = readVec4XYZW(b, "normalColor", glm::vec4(1.0f), warnings, ctx);
            btn->hoverColor    = readVec4XYZW(b, "hoverColor", glm::vec4(1.0f), warnings, ctx);
            btn->pressedColor  = readVec4XYZW(b, "pressedColor", glm::vec4(1.0f), warnings, ctx);
            btn->disabledColor = readVec4XYZW(b, "disabledColor", glm::vec4(1.0f), warnings, ctx);
            btn->selectedColor = readVec4XYZW(b, "selectedColor", glm::vec4(1.0f), warnings, ctx);

            btn->normalSprite   = readStr("normalSprite");
            btn->hoverSprite    = readStr("hoverSprite");
            btn->pressedSprite  = readStr("pressedSprite");
            btn->disabledSprite = readStr("disabledSprite");
            btn->selectedSprite = readStr("selectedSprite");

            btn->fadeDuration = readFloat(b, "fadeDuration", 0.1f, warnings, ctx);

            btn->text      = readStr("text");
            btn->fontPath  = readStr("fontPath");
            btn->fontSize  = readFloat(b, "fontSize", 16.0f, warnings, ctx);
            btn->textColor = readVec4XYZW(b, "textColor", glm::vec4(1.0f), warnings, ctx);
            // Sin clave el default es Center (el del componente), no Left: por
            // eso no basta con pasarle "" a uiTextAlignFromStr.
            btn->textAlign = (b.contains("textAlign") && b["textAlign"].is_string())
                                 ? uiTextAlignFromStr(b["textAlign"].get<std::string>())
                                 : UiTextAlign::Center;
            node->setButton(std::move(btn));
        }
        // Bloque aditivo, misma regla que el Button: una escena guardada antes
        // del componente Text no trae la clave y se carga sin él ni un aviso.
        if (j.contains("text"))
        {
            const auto& t = j["text"];
            const std::string ctx = "text de '" + node->name + "'";
            // Mismo criterio que el Button: un bool o un string corrupto cae al
            // default en vez de lanzar, que un campo roto no puede tumbar la
            // carga de la escena entera.
            auto readBool = [&](const char* key, bool def) {
                return (t.contains(key) && t[key].is_boolean()) ? t[key].get<bool>() : def;
            };
            auto readStr = [&](const char* key) {
                return (t.contains(key) && t[key].is_string())
                           ? t[key].get<std::string>() : std::string();
            };
            auto txt = std::make_shared<TextComponent>();
            txt->anchorMin = readVec2XY(t, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            txt->anchorMax = readVec2XY(t, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            txt->pivot     = readVec2XY(t, "pivot", glm::vec2(0.0f), warnings, ctx);
            txt->position  = readVec2XY(t, "position", glm::vec2(0.0f), warnings, ctx);
            txt->size      = readVec2XY(t, "size", glm::vec2(160.0f, 40.0f), warnings, ctx);
            txt->color     = readVec4XYZW(t, "color", glm::vec4(1.0f), warnings, ctx);
            txt->visible   = readBool("visible", true);

            txt->text     = readStr("text");
            txt->fontPath = readStr("fontPath");
            txt->fontSize = readFloat(t, "fontSize", 16.0f, warnings, ctx);

            txt->outlineWidth = readFloat(t, "outlineWidth", 0.0f, warnings, ctx);
            txt->outlineColor = readVec4XYZW(t, "outlineColor",
                                             glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), warnings, ctx);
            txt->shadowOffset = readVec2XY(t, "shadowOffset", glm::vec2(0.0f), warnings, ctx);
            txt->shadowColor  = readVec4XYZW(t, "shadowColor",
                                             glm::vec4(0.0f, 0.0f, 0.0f, 0.5f), warnings, ctx);

            txt->align    = uiTextAlignFromStr(readStr("align"));
            txt->overflow = uiTextOverflowFromStr(readStr("overflow"));
            txt->wordWrap = readBool("wordWrap", false);

            txt->boldStrength = readFloat(t, "boldStrength", 0.08f, warnings, ctx);
            txt->italicSkew   = readFloat(t, "italicSkew", 0.25f, warnings, ctx);
            node->setText(std::move(txt));
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1).
        if (j.contains("animator"))
        {
            auto anim = animatorFromJson(j["animator"], warnings);
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
                // .value() con default: compat con escenas guardadas antes de
                // que existieran estos campos. Con .at() reventaría toda
                // escena anterior a la feature. readFloat además tolera un
                // "null" (NaN serializado, ver el bloque de comentarios junto
                // a jsonToMat4): antes, ese null hacía fallar fromJson entero.
                const std::string ctx = "audioClip de '" + node->name + "'";
                clip->setVolume(readFloat(c, "volume", 1.0f, warnings, ctx));
                clip->setPitch(readFloat(c, "pitch", 1.0f, warnings, ctx));
                // Mismo criterio de compat: defaults del componente pa las
                // escenas anteriores a estos dos campos. Max antes que min: los
                // dos setters mantienen min <= max entre ellos, así que el
                // segundo arrastra al primero y el par acaba siempre válido.
                clip->setMaxDistance(readFloat(c, "maxDistance", 100.0f, warnings, ctx));
                clip->setMinDistance(readFloat(c, "minDistance", 1.0f, warnings, ctx));
                node->setAudioClip(std::move(clip));
            }
            // clip nullptr (asset roto/formato no soportado): node queda sin
            // audio, el resto de la escena sigue cargando.
        }
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1). El invariante de uno por escena
        // NO se impone aquí (nodeFromJson no ve el árbol entero): lo hace
        // pruneExtraAudioListeners al final de fromJson.
        if (j.contains("audioListener"))
        {
            auto listener = std::make_shared<AudioListenerComponent>();
            listener->setEnabled(j["audioListener"].value("enabled", true));
            node->setAudioListener(std::move(listener));
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
            nodeFromJson(childJson, child, node->worldTransform, physics, audio, warnings, hasBonesCache, loader, preloaded);
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

        // Fuera los "id" del árbol serializado, para que addChild/GameObject
        // dejen los suyos recién generados.
        //
        // nodeFromJson reusa el id que venga en el JSON, y hace bien: es lo que
        // permite que el Undo de un Delete reconstruya el GameObject con su id
        // original y los comandos que quedan en el stack lo sigan resolviendo.
        // Pero al clonar el ORIGINAL SIGUE VIVO, así que reusarlo dejaba dos
        // nodos con el mismo id; findById devuelve el último del recorrido —el
        // clon—, y cualquier comando de undo resuelto por id acababa
        // escribiendo en el objeto equivocado.
        std::function<void(nlohmann::json&)> stripIds = [&](nlohmann::json& node) {
            node.erase("id");
            if (auto it = node.find("children"); it != node.end() && it->is_array())
                for (nlohmann::json& child : *it)
                    stripIds(child);
        };
        stripIds(j);

        GameObject* clone = target->addChild(src->name + " (Clone)");
        // Antes de nodeFromJson: si se limpiara después (como estaba), los
        // avisos que bindClips empuja a m_warnings durante la carga se
        // perderían de inmediato.
        m_warnings.clear();
        // Cache sembrada con la respuesta AUTORITATIVA: el objeto origen ya
        // está en memoria, así que isSkinned() es gratis y no puede mentir.
        // Sin esto cada clon volvía a sondear el FBX con Assimp (y luego a
        // parsearlo entero otra vez), dos lecturas síncronas de fichero por
        // spawn dentro del bucle de Play — su único caller es Scene.Instantiate
        // de Lua. Peor que el coste: leer el disco permite que un clon tomado
        // mientras el artista reexporta el FBX vuelva con un tipo de malla
        // distinto al del objeto del que se clonó. Si el clon es un subárbol,
        // la cache además dedup entre todos sus nodos.
        std::unordered_map<std::string, bool> cache;
        if (src->hasMesh() && !src->getMesh()->sourcePath.empty())
            cache[src->getMesh()->sourcePath] = src->isSkinned();
        try
        {
            nodeFromJson(j, clone, target->worldTransform, physics, audio, &m_warnings, &cache);
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
        collapseWarnings();
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

    GameObject* Scene::findAudioListener()
    {
        GameObject* found = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!found && n->hasAudioListener()) found = n;
        });
        return found;
    }

    const GameObject* Scene::findAudioListener() const
    {
        return const_cast<Scene*>(this)->findAudioListener();
    }

    GameObject* Scene::findCanvas()
    {
        GameObject* found = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!found && n->hasCanvas()) found = n;
        });
        return found;
    }

    const GameObject* Scene::findCanvas() const
    {
        return const_cast<Scene*>(this)->findCanvas();
    }

    void Scene::collapseWarnings()
    {
        std::vector<std::string> unicos;
        std::vector<size_t>      veces;
        // Mapea mensaje -> posición en unicos. Con el mensaje entero como clave:
        // dos avisos distintos del mismo objeto tienen que seguir siendo dos.
        std::unordered_map<std::string, size_t> visto;

        for (const std::string& w : m_warnings)
        {
            auto [it, nuevo] = visto.emplace(w, unicos.size());
            if (nuevo) { unicos.push_back(w); veces.push_back(1); }
            else       { veces[it->second]++; }
        }

        for (size_t i = 0; i < unicos.size(); i++)
            if (veces[i] > 1)
                unicos[i] += " (x" + std::to_string(veces[i]) + ")";

        m_warnings = std::move(unicos);
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

    void Scene::pruneExtraAudioListeners()
    {
        GameObject* first = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!n->hasAudioListener()) return;
            if (!first) { first = n; return; }
            m_warnings.push_back("Escena con más de un Audio Listener: se descarta el de '" + n->name +
                                  "' (se conserva el de '" + first->name + "')");
            n->setAudioListener(nullptr);
        });
    }

    size_t Scene::collectLights(std::vector<Light>& outLights, std::vector<float>& outRadii) const
    {
        outLights.clear();
        outRadii.clear();
        size_t total = 0;

        const_cast<GameObject&>(m_root).traverse([&](GameObject* n) {
            if (!n->hasLight()) return;
            total++;
            if (outLights.size() >= (size_t)MAX_LIGHTS) return;

            const auto& lc = *n->getLight();

            // Posición y dirección salen del transform, igual que la cámara: la
            // columna 3 es la posición de mundo y -Z local es hacia dónde mira.
            // Una escala 0 en el eje Z (el editor deja ponerla desde Properties)
            // dejaría la dirección en NaN, así que ahí se cae a -Y en vez de
            // propagar el NaN hasta el shader — mismo criterio que el listener
            // de audio del runtime.
            const glm::vec3 pos     = glm::vec3(n->worldTransform[3]);
            const glm::vec3 zAxis   = glm::vec3(n->worldTransform[2]);
            const glm::vec3 forward = (glm::length(zAxis) >= 1e-6f)
                                          ? glm::normalize(-zAxis)
                                          : glm::vec3(0.0f, -1.0f, 0.0f);

            Light l{};
            l.position  = glm::vec4(pos, 1.0f);
            l.color     = glm::vec4(lc.getColor(), lc.getIntensity());
            l.direction = glm::vec4(forward, (float)(int)lc.getType());
            // Los ángulos viajan ya en coseno: el shader compara contra el
            // coseno del ángulo con el eje, no vuelve a llamar a cos() por
            // fragmento.
            l.params = glm::vec4(lc.getRange(),
                                 std::cos(glm::radians(lc.getInnerAngle())),
                                 std::cos(glm::radians(lc.getOuterAngle())),
                                 lc.getAreaWidth());

            // El radio del binning de Forward+ tiene que ser EL MISMO alcance
            // que usa el fragment shader, o una luz se apagaría de golpe al
            // cruzar el borde de un tile. El area se aproxima como un point de
            // radio ancho/2, igual que allí; la directional no se culea por
            // radio (entra en todas las celdas), así que el suyo da igual.
            const float radius = (lc.getType() == LightType::Area)
                                     ? lc.getAreaWidth() * 0.5f
                                     : lc.getRange();

            outLights.push_back(l);
            outRadii.push_back(radius);
        });

        return total;
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
        // Igual que fromJson y cloneGameObject: los avisos son de ESTA operación.
        // Sin este clear, cada undo de un Delete apilaba los suyos sobre los de
        // la carga anterior y m_warnings crecía durante toda la sesión, en contra
        // de lo que promete lastWarnings() en el header.
        m_warnings.clear();
        // Sin objeto vivo al que preguntar (esto reconstruye un subárbol ya
        // borrado: el undo de un Delete), así que la cache arranca vacía y
        // sólo aporta el dedup entre los nodos de ESE subárbol — que ya evita
        // repetir el ReadFile de Assimp por cada nodo que comparta sourcePath.
        std::unordered_map<std::string, bool> cache;
        try
        {
            nodeFromJson(j, node, target->worldTransform, physics, audio, &m_warnings, &cache);
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
        collapseWarnings();
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

    bool Scene::fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio,
                         AsyncAssetLoader* loader, const PreloadedMeshCache* preloaded)
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
            nodeFromJson(rootJson, &newRoot, glm::mat4(1.0f), physics, audio, &m_warnings, &hasBonesCache, loader, preloaded);
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
        // Igual que las cámaras: el fichero puede traer dos listeners.
        pruneExtraAudioListeners();
        collapseWarnings(); // después de los prune: también empujan avisos
        return true;
    }

    bool Scene::load(const std::string& path, PhysicsManager& physics, AudioManager& audio,
                     AsyncAssetLoader* loader, const PreloadedMeshCache* preloaded)
    {
        auto parsed = FileManager::readJson(path);
        if (!parsed)
            return false;
        return fromJson(*parsed, physics, audio, loader, preloaded);
    }
}
