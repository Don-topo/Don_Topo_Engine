#include <functional>
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/AnimatorSerialization.h"
#include "DonTopo/Core/PropertyTracks.h"
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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/UiWidgets.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/TextComponent.h"

namespace
{
    using DonTopo::GameObject;
    using DonTopo::MaterialOverride;
    using DonTopo::applyMaterialOverrides;
    using DonTopo::collectMaterialOverrideWarnings;
    using DonTopo::materialsOfMesh;
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
    using DonTopo::UiCanvasRenderMode;
    using DonTopo::UiBillboard;
    using DonTopo::ButtonComponent;
    using DonTopo::UiButtonTransition;
    using DonTopo::UiTextAlign;
    using DonTopo::UiTextVAlign;
    using DonTopo::TextComponent;
    using DonTopo::UiTextOverflow;
    using DonTopo::ProgressBarComponent;
    using DonTopo::UiProgressFillDirection;
    using DonTopo::LayoutComponent;
    using DonTopo::UiLayoutMode;
    using DonTopo::UiCrossAlign;
    using DonTopo::PanelComponent;
    using DonTopo::ImageComponent;
    using DonTopo::UiImageMode;
    using DonTopo::UiFillDirection;
    using DonTopo::UiFillOrigin;
    using DonTopo::SliderComponent;
    using DonTopo::UiSliderDirection;
    using DonTopo::CheckboxComponent;
    using DonTopo::ToggleComponent;
    using DonTopo::ScrollbarComponent;
    using DonTopo::UiScrollbarDirection;
    using DonTopo::InputFieldComponent;
    using DonTopo::UiInputContentType;
    using DonTopo::DropdownComponent;
    using DonTopo::ScrollViewComponent;

    // Forward declarations: animatorFromJson (below) needs these readers
    // tolerant of corrupt JSON (defined next to jsonToMat4/
    // jsonToVec3, later in the file) for the threshold of the
    // numeric conditions and the editorPos of the states.
    //
    // required (default false, long-standing back-compat): the ABSENT key/index
    // also warns when required == true. It is for the fields that
    // nodeToJson ALWAYS writes (they are never really optional) — see the
    // big comment below, finding 1 of the review of this fix.
    float readFloat(const nlohmann::json& j, const char* key, float def,
                     std::vector<std::string>* warnings, const std::string& contexto,
                     bool required = false);
    float readArrayFloat(const nlohmann::json& arr, size_t idx, float def,
                          std::vector<std::string>* warnings, const std::string& contexto,
                          bool required = false);
    // Same criteria as readFloat, for the fields that nodeToJson ALWAYS writes
    // and that until now were read with .at() (an exception there takes down
    // the load of the WHOLE scene over a single field).
    bool readBool(const nlohmann::json& j, const char* key, bool def,
                   std::vector<std::string>* warnings, const std::string& contexto,
                   bool required = false);
    std::string readString(const nlohmann::json& j, const char* key, const std::string& def,
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
        return LightType::Point;    // unknown value -> point
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
        return UiScaleMode::ConstantPixelSize;  // unknown value -> the default
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
        return UiScreenMatch::MatchWidthOrHeight;   // unknown value -> the default
    }

    const char* uiCanvasRenderModeToStr(UiCanvasRenderMode m)
    {
        return m == UiCanvasRenderMode::World ? "world" : "screenSpace";
    }

    UiCanvasRenderMode uiCanvasRenderModeFromStr(const std::string& s)
    {
        return s == "world" ? UiCanvasRenderMode::World : UiCanvasRenderMode::ScreenSpace;
    }

    const char* uiBillboardToStr(UiBillboard b)
    {
        switch (b)
        {
            case UiBillboard::YawOnly: return "yawOnly";
            case UiBillboard::Full:    return "full";
            default:                   return "none";
        }
    }

    UiBillboard uiBillboardFromStr(const std::string& s)
    {
        if (s == "yawOnly") return UiBillboard::YawOnly;
        if (s == "full")    return UiBillboard::Full;
        return UiBillboard::None;   // unknown value -> the default
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
        return UiButtonTransition::ColorTint;   // unknown value -> the default
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
        return UiTextAlign::Left;   // unknown value -> the default
    }

    const char* uiTextVAlignToStr(UiTextVAlign a)
    {
        switch (a)
        {
            case UiTextVAlign::Middle: return "middle";
            case UiTextVAlign::Bottom: return "bottom";
            default:                   return "top";
        }
    }

    // The default is NOT the same for the two that use it: a loose Text is
    // "top" and a button label is "middle", so it is passed as a parameter
    // instead of being nailed down here. An old file does not bring the key and has to
    // load exactly as it looked.
    UiTextVAlign uiTextVAlignFromStr(const std::string& s, UiTextVAlign porDefecto)
    {
        if (s == "top")    return UiTextVAlign::Top;
        if (s == "middle") return UiTextVAlign::Middle;
        if (s == "bottom") return UiTextVAlign::Bottom;
        return porDefecto;
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
        return UiTextOverflow::Overflow;   // unknown value -> the default
    }

    const char* uiProgressFillDirectionToStr(UiProgressFillDirection d)
    {
        switch (d)
        {
            case UiProgressFillDirection::RightToLeft: return "rightToLeft";
            case UiProgressFillDirection::BottomToTop: return "bottomToTop";
            case UiProgressFillDirection::TopToBottom: return "topToBottom";
            default:                                   return "leftToRight";
        }
    }

    UiProgressFillDirection uiProgressFillDirectionFromStr(const std::string& s)
    {
        if (s == "rightToLeft") return UiProgressFillDirection::RightToLeft;
        if (s == "bottomToTop") return UiProgressFillDirection::BottomToTop;
        if (s == "topToBottom") return UiProgressFillDirection::TopToBottom;
        return UiProgressFillDirection::LeftToRight;   // unknown value -> the default
    }

    const char* uiInputContentTypeToStr(UiInputContentType t)
    {
        switch (t)
        {
            case UiInputContentType::IntegerNumber: return "integerNumber";
            case UiInputContentType::DecimalNumber: return "decimalNumber";
            case UiInputContentType::Alphanumeric:  return "alphanumeric";
            case UiInputContentType::Password:      return "password";
            default:                                return "standard";
        }
    }

    UiInputContentType uiInputContentTypeFromStr(const std::string& s)
    {
        if (s == "integerNumber") return UiInputContentType::IntegerNumber;
        if (s == "decimalNumber") return UiInputContentType::DecimalNumber;
        if (s == "alphanumeric")  return UiInputContentType::Alphanumeric;
        if (s == "password")      return UiInputContentType::Password;
        return UiInputContentType::Standard;   // unknown value -> the default
    }

    const char* uiScrollbarDirectionToStr(UiScrollbarDirection d)
    {
        switch (d)
        {
            case UiScrollbarDirection::LeftToRight: return "leftToRight";
            case UiScrollbarDirection::RightToLeft: return "rightToLeft";
            case UiScrollbarDirection::BottomToTop: return "bottomToTop";
            default:                                return "topToBottom";
        }
    }

    UiScrollbarDirection uiScrollbarDirectionFromStr(const std::string& s)
    {
        if (s == "leftToRight") return UiScrollbarDirection::LeftToRight;
        if (s == "rightToLeft") return UiScrollbarDirection::RightToLeft;
        if (s == "bottomToTop") return UiScrollbarDirection::BottomToTop;
        return UiScrollbarDirection::TopToBottom;   // unknown value -> the default
    }

    const char* uiSliderDirectionToStr(UiSliderDirection d)
    {
        switch (d)
        {
            case UiSliderDirection::RightToLeft: return "rightToLeft";
            case UiSliderDirection::BottomToTop: return "bottomToTop";
            case UiSliderDirection::TopToBottom: return "topToBottom";
            default:                             return "leftToRight";
        }
    }

    UiSliderDirection uiSliderDirectionFromStr(const std::string& s)
    {
        if (s == "rightToLeft") return UiSliderDirection::RightToLeft;
        if (s == "bottomToTop") return UiSliderDirection::BottomToTop;
        if (s == "topToBottom") return UiSliderDirection::TopToBottom;
        return UiSliderDirection::LeftToRight;   // unknown value -> the default
    }

    const char* uiImageModeToStr(UiImageMode m)
    {
        switch (m)
        {
            case UiImageMode::Tiled:  return "tiled";
            case UiImageMode::Sliced: return "sliced";
            case UiImageMode::Filled: return "filled";
            default:                  return "normal";
        }
    }

    UiImageMode uiImageModeFromStr(const std::string& s)
    {
        if (s == "tiled")  return UiImageMode::Tiled;
        if (s == "sliced") return UiImageMode::Sliced;
        if (s == "filled") return UiImageMode::Filled;
        return UiImageMode::Normal;   // unknown value -> the default
    }

    const char* uiFillDirectionToStr(UiFillDirection d)
    {
        return d == UiFillDirection::Vertical ? "vertical" : "horizontal";
    }

    UiFillDirection uiFillDirectionFromStr(const std::string& s)
    {
        return s == "vertical" ? UiFillDirection::Vertical : UiFillDirection::Horizontal;
    }

    const char* uiFillOriginToStr(UiFillOrigin o)
    {
        return o == UiFillOrigin::End ? "end" : "start";
    }

    UiFillOrigin uiFillOriginFromStr(const std::string& s)
    {
        return s == "end" ? UiFillOrigin::End : UiFillOrigin::Start;
    }

    const char* uiLayoutModeToStr(UiLayoutMode m)
    {
        switch (m)
        {
            case UiLayoutMode::Horizontal: return "horizontal";
            case UiLayoutMode::Grid:       return "grid";
            case UiLayoutMode::None:       return "none";
            default:                       return "vertical";
        }
    }

    UiLayoutMode uiLayoutModeFromStr(const std::string& s)
    {
        if (s == "horizontal") return UiLayoutMode::Horizontal;
        if (s == "grid")       return UiLayoutMode::Grid;
        if (s == "none")       return UiLayoutMode::None;
        return UiLayoutMode::Vertical;   // unknown value -> the default
    }

    const char* uiCrossAlignToStr(UiCrossAlign a)
    {
        switch (a)
        {
            case UiCrossAlign::Center: return "center";
            case UiCrossAlign::End:    return "end";
            default:                   return "start";
        }
    }

    UiCrossAlign uiCrossAlignFromStr(const std::string& s)
    {
        if (s == "center") return UiCrossAlign::Center;
        if (s == "end")    return UiCrossAlign::End;
        return UiCrossAlign::Start;   // unknown value -> the default
    }

    // The Button vectors go as an object with named components, just like
    // referenceResolution and safeArea of the canvas (and not like the vec3ToJson
    // array): a hand-edited .scene reads better, and readFloat already tolerates
    // a null per field without taking down the whole load.
    nlohmann::json vec2ToJsonXY(const glm::vec2& v)
    {
        return { {"x", v.x}, {"y", v.y} };
    }

    nlohmann::json vec4ToJsonXYZW(const glm::vec4& v)
    {
        return { {"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w} };
    }

    // The enums go as strings and not as ints: readable in a hand-edited
    // .scene and stable if the enum grows in the middle. Same criterion as the "mode"
    // of the camera.
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

}   // namespace (anonymous)

// Outside the anonymous namespace because the editor undo uses it (see
// AnimatorSerialization.h). It stays in this file because the format belongs to the
// scene, and its helpers (paramTypeToStr, condTypeToStr, compareToStr) stay
// inside the anonymous one: from here they are seen all the same.
//
// Each field dumped here is what the editor undo compares
// (animatorGraphKey, see AnimatorSerialization.h): a new field that does not go
// through this function is invisible to the undo even if it is edited from the panel.
// A new field also needs its mutation in graphMutations()
// (engine/tests/animator_tests.cpp) or nothing notices it is not undoable.
namespace DonTopo
{
    nlohmann::json animatorToJson(const AnimatorComponent& a)
    {
        // States and transitions of ONE layer: layer 0 goes under the usual keys
        // and the others, with the same format, under "layers".
        auto grafo = [&a](int capa) {
            auto states = nlohmann::json::array();
            for (const auto& s : a.states(capa))
            {
                // The clip goes by NAME: the index depends on the order of mAnimations
                // in the FBX, and re-exporting the model shuffles it silently.
                nlohmann::json sj = { {"name", s.name},
                                      {"clip", s.clipName},
                                      {"loop", s.loop},
                                      {"pos", nlohmann::json::array({ s.editorPos.x, s.editorPos.y })} };
                // Property clip: only if the state uses it, so a graph
                // of the usual kind is saved exactly as before.
                if (!s.propertyClipName.empty()) sj["propertyClip"] = s.propertyClipName;
                // Blend by parameter: only if the state uses it. Emitting it always
                // would fill the .scene of any normal graph with empty fields.
                // clipIndex/duration of each entry are NOT saved: they belong to the FBX,
                // they are filled by bindClips just like the state's clipIndex.
                if (!s.blendEntries.empty())
                {
                    sj["blendParam"]    = s.blendParam;
                    sj["clipThreshold"] = s.clipThreshold;
                    // 2D blend: only if it uses it, so a 1D one is saved as always.
                    const bool dosD = !s.blendParamY.empty();
                    if (dosD)
                    {
                        sj["blendParamY"]    = s.blendParamY;
                        sj["clipThresholdY"] = s.clipThresholdY;
                    }
                    nlohmann::json entradas = nlohmann::json::array();
                    for (const auto& e : s.blendEntries)
                    {
                        nlohmann::json ej = { {"clip", e.clipName}, {"threshold", e.threshold} };
                        if (dosD) ej["thresholdY"] = e.thresholdY;
                        entradas.push_back(std::move(ej));
                    }
                    sj["blendEntries"] = std::move(entradas);
                }
                // Root mode: only if it is not Off, which is what all the
                // scenes prior to this option bring.
                if (s.rootMotion == AnimatorComponent::RootMotion::Lock)  sj["rootMotion"] = "lock";
                if (s.rootMotion == AnimatorComponent::RootMotion::Apply) sj["rootMotion"] = "apply";
                // Events: only if there is any, like the blend.
                if (!s.events.empty())
                {
                    nlohmann::json eventos = nlohmann::json::array();
                    for (const auto& ev : s.events)
                        eventos.push_back({ {"name", ev.name}, {"time", ev.time} });
                    sj["events"] = std::move(eventos);
                }
                // Speed: only if it is not the usual one (x1, no multiplier).
                if (s.speed != 1.0f)
                    sj["speed"] = s.speed;
                if (!s.speedParam.empty())
                    sj["speedParam"] = s.speedParam;
                // Sub-machines: only what is not the default, so that a graph
                // without boxes is saved byte for byte as before.
                if (s.parent >= 0)   sj["parent"]     = s.parent;
                if (s.isSubMachine)  sj["subMachine"] = true;
                if (s.subEntry >= 0) sj["subEntry"]   = s.subEntry;
                states.push_back(sj);
            }


            auto transitions = nlohmann::json::array();
            for (const auto& t : a.transitions(capa))
            {
                auto conds = nlohmann::json::array();
                for (const auto& c : t.conditions)
                {
                    nlohmann::json cj = { {"type", condTypeToStr(c.type)} };
                    if (c.type != AnimatorComponent::ConditionType::AnimationFinished)
                        cj["param"] = c.paramName;
                    if (c.type == AnimatorComponent::ConditionType::Bool)
                        cj["expected"] = c.expected;
                    // Only the numeric ones: in a Bool they would be noise in the .scene.
                    if (c.type == AnimatorComponent::ConditionType::Int ||
                        c.type == AnimatorComponent::ConditionType::Float)
                    {
                        cj["compare"]   = compareToStr(c.compare);
                        cj["threshold"] = c.threshold;
                    }
                    conds.push_back(cj);
                }
                // from/to are indices into the "states" array of THIS same JSON:
                // self-contained, without depending on any external asset.
                // "duration" is the cross-fade in seconds; 0 (hard cut) is what
                // every scene saved before the field existed assumes.
                nlohmann::json tj = { {"from", t.fromState}, {"to", t.toState},
                                      {"duration", t.duration}, {"conditions", conds} };
                // Exit time and Any State: only if they are used, like the
                // blend fields. Any State is saved as "from": -2 (kAnyState), which is still
                // an integer: the "from" reader does not change.
                if (t.hasExitTime)
                {
                    tj["hasExitTime"] = true;
                    tj["exitTime"]    = t.exitTime;
                }
                if (t.fromState == AnimatorComponent::kAnyState && t.canTransitionToSelf)
                    tj["canTransitionToSelf"] = true;
                transitions.push_back(tj);
            }

            return std::make_pair(std::move(states), std::move(transitions));
        };

        auto params = nlohmann::json::array();
        for (const auto& p : a.parameters())
            params.push_back({ {"name", p.name}, {"type", paramTypeToStr(p.type)} });

        auto base = grafo(0);
        nlohmann::json out = { {"entryState", a.entryState()},
                               {"parameters", params},
                               {"states", std::move(base.first)},
                               {"transitions", std::move(base.second)},
                               {"anyStatePos", nlohmann::json::array({ a.anyStateEditorPos().x,
                                                                        a.anyStateEditorPos().y })} };
        // Layers: only if there is more than one, so a one-layer scene is saved
        // exactly as before they existed.
        if (a.layerCount() > 1)
        {
            auto capas = nlohmann::json::array();
            for (int li = 1; li < a.layerCount(); li++)
            {
                const auto& L = a.layer(li);
                auto g = grafo(li);
                nlohmann::json lj = { {"name", L.name},
                                      {"weight", L.weight},
                                      {"mode", L.mode == AnimatorComponent::LayerMode::Additive ? "additive" : "override"},
                                      {"entryState", L.entryState},
                                      {"states", std::move(g.first)},
                                      {"transitions", std::move(g.second)},
                                      {"anyStatePos", nlohmann::json::array({ L.anyStatePos.x, L.anyStatePos.y })} };
                if (!L.maskBones.empty()) lj["mask"] = L.maskBones;
                capas.push_back(std::move(lj));
            }
            out["layers"] = std::move(capas);
        }
        // IK: only if there are constraints, so an Animator without them is saved
        // exactly as before they existed.
        if (!a.ikConstraints().empty())
        {
            auto ik = nlohmann::json::array();
            for (const auto& c : a.ikConstraints())
                ik.push_back({ {"name", c.name},
                               {"type", c.type == AnimatorComponent::IkType::TwoBone ? "twoBone" : "lookAt"},
                               {"bone", c.boneName},
                               {"target", c.targetId},
                               {"pole", c.poleId},
                               {"weight", c.weight},
                               {"aimAxis", nlohmann::json::array({ c.aimAxis.x, c.aimAxis.y, c.aimAxis.z })},
                               {"maxAngle", c.maxAngle} });
            out["ik"] = std::move(ik);
        }
        // Property clips: same, only if there are any.
        if (!a.propertyClips().empty())
        {
            auto clips = nlohmann::json::array();
            for (const auto& c : a.propertyClips())
            {
                auto pistas = nlohmann::json::array();
                for (const auto& tr : c.tracks)
                {
                    auto keys = nlohmann::json::array();
                    for (const auto& k : tr.keys)
                        keys.push_back({ {"t", k.time}, {"v", k.value} });
                    // A curve is distinguished by "target"; a property
                    // track is saved as always, so that a .scene from
                    // before and one from now are identical if there are no curves.
                    if (tr.target == TrackTarget::Parameter)
                        pistas.push_back({ {"target", "parameter"},
                                           {"parameter", tr.parameterName},
                                           {"keys", std::move(keys)} });
                    else
                        pistas.push_back({ {"property", propertyName(tr.property)}, {"keys", std::move(keys)} });
                }
                clips.push_back({ {"name", c.name}, {"duration", c.duration}, {"tracks", std::move(pistas)} });
            }
            out["propertyClips"] = std::move(clips);
        }
        return out;
    }

    nlohmann::json animatorGraphKey(const AnimatorComponent& a)
    {
        nlohmann::json key = animatorToJson(a);
        for (auto& s : key["states"]) s.erase("pos");
        // Moving the Any State node is not an edit either.
        key.erase("anyStatePos");
        if (key.contains("layers"))
            for (auto& lj : key["layers"])
            {
                for (auto& s : lj["states"]) s.erase("pos");
                lj.erase("anyStatePos");
            }
        return key;
    }
}   // namespace DonTopo

namespace
{
    // It does not deserialize runtime state (current state, animTime, parameter
    // values, pending triggers) because it is not serialized: the Stop of Play
    // rebuilds the scene from JSON, so the reset to the entry state
    // comes for free, and saving in the middle of Play does not bake transient state.
    std::shared_ptr<AnimatorComponent> animatorFromJson(const nlohmann::json& j,
                                                          std::vector<std::string>* warnings)
    {
        auto a = std::make_shared<AnimatorComponent>();

        // Parameters first: addParameter is what creates the bool/
        // trigger entries that the conditions will query.
        if (j.contains("parameters"))
            for (const auto& p : j["parameters"])
                a->addParameter(p.value("name", std::string()),
                                paramTypeFromStr(p.value("type", std::string("bool"))));

        // The graph of ONE layer (states, Any State, transitions, entry):
        // layer 0 comes from the usual keys and the others from "layers".
        auto leerGrafo = [&](const nlohmann::json& g, int capa)
        {
            if (g.contains("states"))
            {
                for (const auto& s : g["states"])
                {
                    AnimatorComponent::State st;
                    st.name     = s.value("name", std::string());
                    st.clipName = s.value("clip", std::string());
                    st.loop     = s.value("loop", true);
                    // Sub-machines. The coherence (what each index points to) is
                    // validated below, once all the states have been read.
                    st.parent       = s.value("parent", -1);
                    st.isSubMachine = s.value("subMachine", false);
                    st.subEntry     = s.value("subEntry", -1);
                    // Absent in scenes prior to blend by parameter: without
                    // entries the state is single-clip, as always.
                    const std::string ctxBlend = "animator.state." + st.name;
                    st.propertyClipName = s.value("propertyClip", std::string());
                st.blendParam  = s.value("blendParam", std::string());
                    st.blendParamY = s.value("blendParamY", std::string());
                    if (s.contains("blendEntries") && s["blendEntries"].is_array())
                    {
                        st.clipThreshold  = readFloat(s, "clipThreshold", 0.0f, warnings, ctxBlend);
                        st.clipThresholdY = readFloat(s, "clipThresholdY", 0.0f, warnings, ctxBlend);
                        for (const auto& ej : s["blendEntries"])
                        {
                            if (!ej.is_object()) continue;
                            AnimatorComponent::BlendEntry e;
                            e.clipName  = ej.value("clip", std::string());
                            e.threshold  = readFloat(ej, "threshold", 0.0f, warnings, ctxBlend + ".blendEntries");
                            e.thresholdY = readFloat(ej, "thresholdY", 0.0f, warnings, ctxBlend + ".blendEntries");
                            st.blendEntries.push_back(std::move(e));
                        }
                    }
                    else if (!s.value("blendClip", std::string()).empty())
                    {
                        // Format prior to N clips: the (clip, blendClip) pair with
                        // [blendMin, blendMax]. As thresholds they give the SAME pose,
                        // including negative span (A/B come out swapped with the
                        // complementary weight) and span 0 (the tie discards the
                        // entry, like the weight 0 from before).
                        st.clipThreshold = readFloat(s, "blendMin", 0.0f, warnings, ctxBlend);
                        AnimatorComponent::BlendEntry e;
                        e.clipName  = s.value("blendClip", std::string());
                        e.threshold = readFloat(s, "blendMax", 1.0f, warnings, ctxBlend);
                        st.blendEntries.push_back(std::move(e));
                    }
                    // "rootMotion" from the real root motion; before, a bool
                    // lockRootMotion that equals Lock. Both absent: Off.
                    const std::string rm = s.value("rootMotion", std::string());
                    if (rm == "lock")       st.rootMotion = AnimatorComponent::RootMotion::Lock;
                    else if (rm == "apply") st.rootMotion = AnimatorComponent::RootMotion::Apply;
                    else if (!rm.empty())
                    {
                        if (warnings)
                            warnings->push_back("animator.state." + st.name + ": rootMotion '" + rm +
                                                "' unknown, using normal");
                    }
                    else if (s.value("lockRootMotion", false))
                        st.rootMotion = AnimatorComponent::RootMotion::Lock;
                    // Absent in scenes prior to events: none.
                    if (s.contains("events") && s["events"].is_array())
                    {
                        const std::string ctxEv = "animator.state." + st.name + ".events";
                        for (const auto& ej : s["events"])
                        {
                            if (!ej.is_object()) continue;
                            AnimatorComponent::AnimationEvent ev;
                            ev.name = ej.value("name", std::string());
                            ev.time = readFloat(ej, "time", 0.0f, warnings, ctxEv);
                            if (ev.time < 0.0f || ev.time > 1.0f)
                            {
                                if (warnings) warnings->push_back(ctxEv + ": time outside [0,1], clamped");
                                ev.time = std::clamp(ev.time, 0.0f, 1.0f);
                            }
                            st.events.push_back(std::move(ev));
                        }
                    }
                    // Absent in scenes prior to per-state speed: x1.
                    st.speed      = readFloat(s, "speed", 1.0f, warnings, "animator.state." + st.name + ".speed");
                    st.speedParam = s.value("speedParam", std::string());
                    if (st.speed < 0.0f)
                    {
                        if (warnings)
                            warnings->push_back("animator.state." + st.name + ".speed negative (" +
                                                 std::to_string(st.speed) + "), clamped to 0");
                        st.speed = 0.0f;
                    }
                    if (s.contains("pos") && s["pos"].is_array() && s["pos"].size() == 2)
                        st.editorPos = glm::vec2(readArrayFloat(s["pos"], 0, 0.0f, warnings, "animator.state." + st.name + ".pos"),
                                                  readArrayFloat(s["pos"], 1, 0.0f, warnings, "animator.state." + st.name + ".pos"));
                    // duration/ticksPerSecond/clipIndex are filled by bindClips against
                    // the SkinnedMesh: they belong to the FBX, not to the scene file.
                    a->addState(st, capa);
                }

                // Everything that comes in from the file goes through the same pass the
                // editor uses: impossible indices, parents that are not boxes, entries that
                // are not children and containment cycles. Before this was written here
                // by hand and only served loading.
                //
                // The transitions are read AFTER this, so their indices are
                // validated in their own loop (see below) and there are none here to
                // look at yet.
                a->sanitizeGraph(capa, warnings);
            }

            // Absent in scenes prior to Any State: the component's default
            // position stays.
            if (g.contains("anyStatePos") && g["anyStatePos"].is_array() && g["anyStatePos"].size() == 2)
                a->setAnyStateEditorPos(glm::vec2(readArrayFloat(g["anyStatePos"], 0, -220.0f, warnings, "animator.anyStatePos"),
                                                  readArrayFloat(g["anyStatePos"], 1, 40.0f, warnings, "animator.anyStatePos")), capa);

            if (g.contains("transitions"))
            {
                for (const auto& t : g["transitions"])
                {
                    AnimatorComponent::Transition tr;
                    tr.fromState = t.value("from", -1);
                    tr.toState   = t.value("to", -1);
                    // Absent in scenes prior to the cross-fade: 0 = hard cut,
                    // exactly what they did.
                    tr.duration  = readFloat(t, "duration", 0.0f, warnings,
                                              "animator.transition[" + std::to_string(tr.fromState) +
                                              "->" + std::to_string(tr.toState) + "]");
                    // Absent in scenes prior to exit time and Any State:
                    // they fall to the struct defaults.
                    tr.hasExitTime = t.value("hasExitTime", false);
                    tr.exitTime    = readFloat(t, "exitTime", 1.0f, warnings,
                                                "animator.transition[" + std::to_string(tr.fromState) +
                                                "->" + std::to_string(tr.toState) + "].exitTime");
                    if (tr.exitTime < 0.0f)
                    {
                        if (warnings)
                            warnings->push_back("animator.transition[" + std::to_string(tr.fromState) +
                                                 "->" + std::to_string(tr.toState) +
                                                 "].exitTime negative (" + std::to_string(tr.exitTime) +
                                                 "), clamped to 0");
                        tr.exitTime = 0.0f;
                    }
                    tr.canTransitionToSelf = t.value("canTransitionToSelf", false);
                    if (t.contains("conditions"))
                    {
                        for (const auto& c : t["conditions"])
                        {
                            AnimatorComponent::Condition cond;
                            cond.type      = condTypeFromStr(c.value("type", std::string("bool")));
                            cond.paramName = c.value("param", std::string());
                            cond.expected  = c.value("expected", true);
                            // Absent in scenes prior to numeric
                            // parameters: they fall to the struct defaults.
                            cond.compare   = compareFromStr(c.value("compare", std::string("greater")));
                            cond.threshold = readFloat(c, "threshold", 0.0f, warnings,
                                                        "animator.transition[" + std::to_string(tr.fromState) +
                                                        "->" + std::to_string(tr.toState) + "].condition");
                            tr.conditions.push_back(cond);
                        }
                    }
                    // Indices against the states that have JUST been loaded. A saved
                    // graph may bring transitions that no longer point to anything:
                    // the FBX was re-exported with fewer clips and someone deleted states, or
                    // the .scene was hand-edited. Without this they came in as is, and the
                    // two symptoms were silent — the AnimatorPanel skips them when
                    // drawing (they are not seen) and update discards them when evaluating (they are not
                    // used), but they were serialized again on every save: an
                    // invisible and permanent passenger.
                    //
                    // They are DISCARDED, not clamped: an invented index cannot be
                    // guessed, and leaving the transition pointing to an arbitrary state
                    // would be worse than not having it. Same criterion as
                    // pruneExtraCameras and as the reassignment of duplicate ids —
                    // the file came broken, it is repaired and that is said.
                    const int nEstados = (int)a->states(capa).size();
                    // Any State (kAnyState) is a valid origin; -1 or another negative
                    // still is not. The destination is always required to be in range.
                    const bool origenValido = tr.fromState == AnimatorComponent::kAnyState ||
                                              (tr.fromState >= 0 && tr.fromState < nEstados);
                    if (!origenValido || tr.toState < 0 || tr.toState >= nEstados)
                    {
                        if (warnings)
                            warnings->push_back("animator.transition[" + std::to_string(tr.fromState) +
                                                 "->" + std::to_string(tr.toState) +
                                                 "]: state index out of range (" +
                                                 std::to_string(nEstados) +
                                                 " state(s) in the graph), the transition is discarded");
                        continue;
                    }
                    a->addTransition(tr, capa);
                }
            }

            // After addState: setEntryState validates against m_states.size() and
            // RETURNS DOING NOTHING if the index is not valid, so the character
            // would start at state 0 without anybody saying why. The
            // behavior is left the same —0 is the only safe thing— but it stops being
            // silent.
            //
            // It only warns if the graph HAS states: with an empty list any
            // index is out of range, and a newly created animator with no states is
            // not a corrupt file.
            const int entrada = g.value("entryState", 0);
            if (!a->states(capa).empty() && (entrada < 0 || entrada >= (int)a->states(capa).size()) && warnings)
                warnings->push_back("animator.entryState: " + std::to_string(entrada) +
                                     " out of range (" + std::to_string(a->states(capa).size()) +
                                     " state(s) in the graph), starting in state 0");
            a->setEntryState(entrada, capa);
        };
        // Property clips: the index of each state and the `resolved` of
        // each track are rebuilt by bindProperties, which needs the GameObject.
        if (j.contains("propertyClips") && j["propertyClips"].is_array())
        {
            const auto& lista = j["propertyClips"];
            if ((int)lista.size() > AnimatorComponent::kMaxPropertyClips && warnings)
                warnings->push_back("Animator: the file has " + std::to_string(lista.size()) +
                                     " property clips; loading the first " +
                                     std::to_string(AnimatorComponent::kMaxPropertyClips));
            for (const auto& cj : lista)
            {
                if (!cj.is_object()) continue;
                DonTopo::PropertyClip clip;
                clip.name = cj.value("name", std::string());
                const std::string ctxClip = "animator.propertyClip." + clip.name;
                clip.duration = readFloat(cj, "duration", 1.0f, warnings, ctxClip);
                if (clip.duration <= 0.0f)
                {
                    if (warnings)
                        warnings->push_back(ctxClip + ": non-positive duration (" +
                                             std::to_string(clip.duration) + "), clamped");
                    clip.duration = 0.001f;
                }
                if (cj.contains("tracks") && cj["tracks"].is_array())
                    for (const auto& tj : cj["tracks"])
                    {
                        if (!tj.is_object()) continue;
                        DonTopo::PropertyTrack tr;
                        if (tj.value("target", std::string("property")) == "parameter")
                        {
                            // Curve: the destination is an Animator parameter. Its
                            // `resolved` is rebuilt by bindProperties, like that of the
                            // property tracks.
                            tr.target        = DonTopo::TrackTarget::Parameter;
                            tr.parameterName = tj.value("parameter", std::string());
                            if (tr.parameterName.empty())
                            {
                                if (warnings)
                                    warnings->push_back(ctxClip + ": a curve without a parameter, discarded");
                                continue;
                            }
                        }
                        else
                        {
                            const std::string prop = tj.value("property", std::string());
                            tr.property = DonTopo::propertyFromName(prop);
                            if (tr.property == DonTopo::PropertyId::Count)
                            {
                                if (warnings)
                                    warnings->push_back(ctxClip + ": property '" + prop +
                                                         "' unknown, the track is discarded");
                                continue;
                            }
                        }
                        if (tj.contains("keys") && tj["keys"].is_array())
                            for (const auto& kj : tj["keys"])
                            {
                                if (!kj.is_object()) continue;
                                DonTopo::PropertyKey k;
                                k.time  = readFloat(kj, "t", 0.0f, warnings, ctxClip + ".keys");
                                k.value = readFloat(kj, "v", 0.0f, warnings, ctxClip + ".keys");
                                tr.keys.push_back(k);
                            }
                        clip.tracks.push_back(std::move(tr));
                    }
                if (a->addPropertyClip(std::move(clip)) < 0) break;
            }
        }

        leerGrafo(j, 0);

        // IK: the weight, the target and the angle ARE editing, so the undo
        // key (animatorGraphKey) keeps them whole.
        if (j.contains("ik") && j["ik"].is_array())
        {
            const auto& lista = j["ik"];
            if ((int)lista.size() > AnimatorComponent::kMaxIkConstraints && warnings)
                warnings->push_back("Animator: the file has " + std::to_string(lista.size()) +
                                     " IK constraints; loading the first " +
                                     std::to_string(AnimatorComponent::kMaxIkConstraints));
            for (const auto& cj : lista)
            {
                if (!cj.is_object()) continue;
                AnimatorComponent::IkConstraint c;
                c.name     = cj.value("name", std::string());
                c.boneName = cj.value("bone", std::string());
                const std::string tipo = cj.value("type", std::string("lookAt"));
                if (tipo == "twoBone") c.type = AnimatorComponent::IkType::TwoBone;
                else if (tipo != "lookAt" && warnings)
                    warnings->push_back("animator.ik." + c.name + ": type '" + tipo +
                                         "' unknown, using lookAt");
                c.targetId = cj.value("target", (uint64_t)0);
                c.poleId   = cj.value("pole", (uint64_t)0);
                const std::string ctxIk = "animator.ik." + c.name;
                c.weight   = readFloat(cj, "weight", 1.0f, warnings, ctxIk);
                c.maxAngle = readFloat(cj, "maxAngle", 80.0f, warnings, ctxIk);
                if (cj.contains("aimAxis") && cj["aimAxis"].is_array() && cj["aimAxis"].size() == 3)
                    c.aimAxis = glm::vec3(readArrayFloat(cj["aimAxis"], 0, 0.0f, warnings, ctxIk + ".aimAxis"),
                                          readArrayFloat(cj["aimAxis"], 1, 0.0f, warnings, ctxIk + ".aimAxis"),
                                          readArrayFloat(cj["aimAxis"], 2, 1.0f, warnings, ctxIk + ".aimAxis"));
                if (a->addIkConstraint(std::move(c)) < 0) break;
            }
        }

        if (j.contains("layers") && j["layers"].is_array())
        {
            const auto& capas = j["layers"];
            if ((int)capas.size() > AnimatorComponent::kMaxLayers - 1 && warnings)
                warnings->push_back("Animator: the file has " + std::to_string(capas.size() + 1) +
                                     " layers; loading the first " + std::to_string(AnimatorComponent::kMaxLayers));
            for (const auto& lj : capas)
            {
                if (!lj.is_object()) continue;
                const int li = a->addLayer(lj.value("name", std::string("Layer")));
                if (li < 0) break;
                a->setLayerWeight(li, readFloat(lj, "weight", 1.0f, warnings, "animator.layer.weight"));
                const std::string modo = lj.value("mode", std::string("override"));
                if (modo == "additive")
                    a->setLayerMode(li, AnimatorComponent::LayerMode::Additive);
                else if (modo != "override" && warnings)
                    warnings->push_back("animator.layer." + a->layer(li).name + ": mode '" + modo +
                                         "' unknown, using override");
                if (lj.contains("mask") && lj["mask"].is_array())
                    for (const auto& b : lj["mask"])
                        if (b.is_string()) a->layerMutable(li).maskBones.push_back(b.get<std::string>());
                leerGrafo(lj, li);
            }
        }
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

    // Path that goes to the file: relative with "/" if it falls under the root, and absolute
    // as is if not. Outside the root a relative one would be a string of ".."
    // that does not survive moving the project elsewhere.
    std::string toStoredPath(const std::string& path, const std::string& assetRoot)
    {
        if (path.empty() || assetRoot.empty()) return path;
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(path, assetRoot, ec);
        if (ec || rel.empty() || *rel.begin() == "..") return path;
        return rel.generic_string();
    }

    // The inverse. An already absolute path is returned as is.
    std::string fromStoredPath(const std::string& stored, const std::string& assetRoot)
    {
        if (stored.empty() || assetRoot.empty()) return stored;
        std::filesystem::path p(stored);
        if (p.is_absolute()) return stored;
        return (std::filesystem::path(assetRoot) / p).string();
    }

    // carryOverrideBaseline: see the big comment next to "baseAlbedo" further
    // below. Default false (disk behavior); the two in-memory callers
    // (cloneGameObject, subtreeToJson) set it to true on purpose.
    nlohmann::json nodeToJson(const GameObject& node, const std::string& assetRoot,
                               bool carryOverrideBaseline = false)
    {
        nlohmann::json j;
        j["id"] = node.id;
        j["name"] = node.name;
        j["localTransform"] = mat4ToJson(node.localTransform);
        // Per-object SSR. It is ALWAYS saved (not inside an if) so that turning it off
        // on an object that had it on is recorded; in old files it does not
        // exist and nodeFromJson falls to the default (off), which is how they looked.
        j["ssrEnabled"]   = node.ssrEnabled;
        j["ssrIntensity"] = node.ssrIntensity;

        if (node.hasMesh())
        {
            const auto& mesh = node.getMesh();
            // "visible" is ALWAYS saved: in old files it does not exist and
            // loading falls to the default true, which is how they looked.
            nlohmann::json meshJson = { {"sourcePath", mesh->sourcePath}, {"name", mesh->name}, {"skinned", node.isSkinned()},
                                        {"visible", node.meshVisible} };
            // Piece 0 does not write the field: scenes from before (without
            // "piece") stay identical byte for byte.
            if (mesh->piece != 0) meshJson["piece"] = mesh->piece;
            if (mesh->sourcePath.empty())
            {
                // Procedural (Cube/Sphere/Plane/Capsule): there is no source file
                // to reload. Regenerating via the fixed parameters of
                // ScenePanel::createBasicShape would assume the mesh was created with
                // those defaults — false for procedural meshes with custom
                // parameters (e.g. the floor, Plane::create(1000.0f,
                // floorY) in main.cpp, very different from the Plane 50/0 of the Basic
                // Shapes menu). The real geometry is serialized to
                // rebuild the exact mesh without depending on which parameters
                // generated it.
                nlohmann::json verts = nlohmann::json::array();
                for (const auto& v : mesh->vertices)
                    verts.push_back(vertexToJson(v));
                meshJson["vertices"] = std::move(verts);
                meshJson["indices"]  = mesh->indices;
            }

            // Animation sources: the SkinnedMesh is rebuilt from the FBX files
            // on every load, so without this the clips imported from extra
            // files (and the renames) would be lost on saving.
            if (const DonTopo::SkinnedMesh* sm = node.getSkinnedMesh())
            {
                nlohmann::json sources = nlohmann::json::array();
                for (const auto& src : sm->animationSources)
                    sources.push_back({ {"path", src.path},
                                        {"builtin", src.builtin},
                                        {"clips", src.clipNames} });
                meshJson["animationSources"] = std::move(sources);
            }

            // Texture paths set by hand from Properties. Only the
            // materials with something to say, and within each one only the
            // non-empty keys: an object without overrides does not write the key, and
            // old scenes remain valid without touching them.
            nlohmann::json mats = nlohmann::json::array();
            for (const MaterialOverride& ov : node.materialOverrides)
            {
                // metallic/roughness use the sentinel -1.0f (see
                // MaterialOverride in GameObject.h) as the equivalent of the empty
                // string of the three textures: below 0 there is no active
                // override, and it does not enter the "nothing to
                // say" condition.
                //
                // This discard runs ALSO with carryOverrideBaseline, that is
                // in cloning and in the Undo/Redo snapshots, so an
                // entry with nothing active but with base*Taken raised (a slot
                // that had an override and was cleaned with Clear) does NOT travel: the
                // baseline block further below is only written for the entries
                // that survive this line. It is harmless, and for two
                // INDEPENDENT reasons -- hence it is not touched--: in the
                // clone, the mesh is already seeded with the material restored by
                // that same Clear, so the baseline that is recaptured is worth
                // the same as the one that would have been copied; and in the Undo of a
                // Create/Delete, insertFromJson reloads the mesh from disk, which
                // leaves the material in its FBX state for the same reason.
                // It is left written here because checking it costs both
                // whole derivations every time someone reads this block.
                if (ov.albedo.empty() && ov.normal.empty() && ov.orm.empty()
                    && ov.metallic < 0.0f && ov.roughness < 0.0f && ov.matAsset.empty()) continue;
                nlohmann::json entry = { {"index", ov.index} };
                if (!ov.albedo.empty()) entry["albedo"] = toStoredPath(ov.albedo, assetRoot);
                if (!ov.normal.empty()) entry["normal"] = toStoredPath(ov.normal, assetRoot);
                if (!ov.orm.empty())    entry["orm"]    = toStoredPath(ov.orm,    assetRoot);
                if (!ov.matAsset.empty()) entry["matAsset"] = toStoredPath(ov.matAsset, assetRoot);
                // Absent = not touched, and the effective value comes from the model on
                // reload (same criterion as the three textures above).
                if (ov.metallic  >= 0.0f) entry["metallic"]  = ov.metallic;
                if (ov.roughness >= 0.0f) entry["roughness"] = ov.roughness;
                // The baseline (base*/base*Taken) ONLY travels when this JSON is
                // an in-MEMORY jump (cloning a GameObject, or the Undo/Redo
                // snapshot of Create/Delete) and never when it is a real
                // save to disk — the criterion is not "empty assetRoot": the
                // Task 6 tests (and any caller without an open project)
                // call Scene::toJson()/fromJson(), the disk API, with
                // m_assetRoot also empty, so that condition alone
                // would confuse the two cases. carryOverrideBaseline is the
                // explicit parameter that does tell them apart: cloneGameObject and
                // subtreeToJson set it to true, Scene::toJson() never touches it
                // (default false).
                //
                // Why it is needed in memory: when cloning, the clone's mesh
                // is seeded from a PreloadedMeshCache with the LIVE mesh of
                // the original —which already has the override baked into the material—,
                // so without the real baseline traveling here, the clone
                // would capture the override texture as the "original", and a
                // Clear on the clone would not return the FBX one (see
                // test_clone_clear_restores_fbx_texture_not_override).
                //
                // Why it is NOT needed on disk: there the material is
                // rederived from the real FBX on every load, so the baseline is
                // recaptured by itself and at the correct value — saving an old one
                // would risk leaving it out of sync if the artist re-exported
                // the model between two saves.
                if (carryOverrideBaseline)
                {
                    entry["baseAlbedo"]      = toStoredPath(ov.baseAlbedo, assetRoot);
                    entry["baseAlbedoTaken"] = ov.baseAlbedoTaken;
                    entry["baseNormal"]      = toStoredPath(ov.baseNormal, assetRoot);
                    entry["baseNormalTaken"] = ov.baseNormalTaken;
                    entry["baseOrm"]         = toStoredPath(ov.baseOrm, assetRoot);
                    entry["baseOrmTaken"]    = ov.baseOrmTaken;
                    // Same in-memory path (cloning, Undo/Redo snapshot),
                    // same reason as the three above.
                    entry["baseMetallic"]       = ov.baseMetallic;
                    entry["baseMetallicTaken"]  = ov.baseMetallicTaken;
                    entry["baseRoughness"]      = ov.baseRoughness;
                    entry["baseRoughnessTaken"] = ov.baseRoughnessTaken;
                }
                mats.push_back(std::move(entry));
            }
            if (!mats.empty())
                meshJson["materials"] = std::move(mats);

            j["mesh"] = std::move(meshJson);
        }
        if (node.hasBoxCollider())
        {
            const auto& c = node.getBoxCollider();
            j["boxCollider"] = { {"halfExtents", vec3ToJson(c->getHalfExtents())},
                                  {"center", vec3ToJson(c->getCenter())},
                                  {"isTrigger", c->isTrigger()},
                                  {"staticFriction", c->getStaticFriction()},
                                  {"dynamicFriction", c->getDynamicFriction()},
                                  {"bounciness", c->getBounciness()} };
        }
        if (node.hasSphereCollider())
        {
            const auto& c = node.getSphereCollider();
            j["sphereCollider"] = { {"radius", c->getRadius()},
                                     {"center", vec3ToJson(c->getCenter())},
                                     {"isTrigger", c->isTrigger()},
                                     {"staticFriction", c->getStaticFriction()},
                                     {"dynamicFriction", c->getDynamicFriction()},
                                     {"bounciness", c->getBounciness()} };
        }
        if (node.hasCapsuleCollider())
        {
            const auto& c = node.getCapsuleCollider();
            j["capsuleCollider"] = { {"radius", c->getRadius()},
                                      {"halfHeight", c->getHalfHeight()},
                                      {"center", vec3ToJson(c->getCenter())},
                                      {"isTrigger", c->isTrigger()},
                                      {"staticFriction", c->getStaticFriction()},
                                      {"dynamicFriction", c->getDynamicFriction()},
                                      {"bounciness", c->getBounciness()} };
        }
        if (node.hasPlaneCollider())
        {
            const auto& c = node.getPlaneCollider();
            j["planeCollider"] = { {"center", vec3ToJson(c->getCenter())},
                                    {"isTrigger", c->isTrigger()},
                                    {"staticFriction", c->getStaticFriction()},
                                    {"dynamicFriction", c->getDynamicFriction()},
                                    {"bounciness", c->getBounciness()} };
        }
        if (node.hasRigidbody())
        {
            const auto& rb = node.getRigidbody();
            j["rigidbody"] = { {"mass", rb->getMass()},
                               {"useGravity", rb->getUseGravity()},
                               {"isKinematic", rb->getIsKinematic()},
                               {"drag", rb->getDrag()},
                               {"angularDrag", rb->getAngularDrag()},
                               {"constraints", rb->getConstraints()},
                               {"ccd", rb->getCcd()},
                               {"interpolate", rb->getInterpolate()} };
        }
        if (node.hasCameraComponent())
        {
            const auto& c = node.getCameraComponent();
            // "mode" as a string and not as the enum int: readable in a hand-edited
            // .scene and stable if the enum grows in the middle.
            j["camera"] = { {"mode", c->getMode() == CameraComponent::ProjectionMode::Orthographic
                                         ? "orthographic" : "perspective"},
                            {"fov", c->getFov()},
                            {"orthographicSize", c->getOrthographicSize()},
                            {"near", c->getNear()},
                            {"far", c->getFar()} };
        }
        if (node.hasReflectionProbe())
        {
            // Only the settings: the baked cubemap is NOT serialized (it is a
            // GPU resource of ~1.1 MB per probe). When loading the scene, the
            // Renderer rebakes the probes that have no capture, so
            // DonTopoRuntime ends up seeing exactly the same as the editor.
            const auto& p = node.getReflectionProbe();
            j["reflectionProbe"] = { {"radius", p->getRadius()},
                                     {"intensity", p->getIntensity()} };
        }
        if (node.hasLight())
        {
            // Neither position nor direction: both come from the worldTransform, which is already
            // serialized as the node's localTransform. "type" as a string and not
            // as the enum int, same criterion as the "mode" of the camera.
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
                            {"aspectRatio", c->aspectRatio},
                            {"renderMode", uiCanvasRenderModeToStr(c->renderMode)},
                            {"worldScale", c->worldScale},
                            {"billboard", uiBillboardToStr(c->billboard)},
                            {"depthTest", c->depthTest} };
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
                            {"textAlign", uiTextAlignToStr(b->textAlign)},
                            {"textVAlign", uiTextVAlignToStr(b->textVAlign)} };
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
                          {"vAlign", uiTextVAlignToStr(t->vAlign)},
                          {"overflow", uiTextOverflowToStr(t->overflow)},
                          {"wordWrap", t->wordWrap},
                          {"boldStrength", t->boldStrength},
                          {"italicSkew", t->italicSkew} };
        }
        if (node.hasProgressBar())
        {
            const auto& p = node.getProgressBar();
            j["progressBar"] = { {"anchorMin", vec2ToJsonXY(p->anchorMin)},
                                 {"anchorMax", vec2ToJsonXY(p->anchorMax)},
                                 {"pivot", vec2ToJsonXY(p->pivot)},
                                 {"position", vec2ToJsonXY(p->position)},
                                 {"size", vec2ToJsonXY(p->size)},
                                 {"color", vec4ToJsonXYZW(p->color)},
                                 {"visible", p->visible},
                                 {"value", p->value},
                                 {"minValue", p->minValue},
                                 {"maxValue", p->maxValue},
                                 {"fillColor", vec4ToJsonXYZW(p->fillColor)},
                                 {"fillDirection", uiProgressFillDirectionToStr(p->fillDirection)},
                                 {"atlasPath", p->atlasPath},
                                 {"backgroundPath", p->backgroundPath},
                                 {"fillPath", p->fillPath} };
        }
        if (node.hasPanel())
        {
            const auto& p = node.getPanel();
            j["panel"] = { {"anchorMin", vec2ToJsonXY(p->anchorMin)},
                           {"anchorMax", vec2ToJsonXY(p->anchorMax)},
                           {"pivot", vec2ToJsonXY(p->pivot)},
                           {"position", vec2ToJsonXY(p->position)},
                           {"size", vec2ToJsonXY(p->size)},
                           {"color", vec4ToJsonXYZW(p->color)},
                           {"visible", p->visible},
                           {"raycastTarget", p->raycastTarget},
                           {"atlasPath", p->atlasPath},
                           {"sprite", p->sprite} };
        }
        if (node.hasImage())
        {
            const auto& im = node.getImage();
            j["image"] = { {"anchorMin", vec2ToJsonXY(im->anchorMin)},
                           {"anchorMax", vec2ToJsonXY(im->anchorMax)},
                           {"pivot", vec2ToJsonXY(im->pivot)},
                           {"position", vec2ToJsonXY(im->position)},
                           {"size", vec2ToJsonXY(im->size)},
                           {"color", vec4ToJsonXYZW(im->color)},
                           {"visible", im->visible},
                           {"raycastTarget", im->raycastTarget},
                           {"atlasPath", im->atlasPath},
                           {"sprite", im->sprite},
                           {"mode", uiImageModeToStr(im->mode)},
                           {"borderLeft", im->borderLeft},
                           {"borderRight", im->borderRight},
                           {"borderTop", im->borderTop},
                           {"borderBottom", im->borderBottom},
                           {"fillCenter", im->fillCenter},
                           {"maxTiles", im->maxTiles},
                           {"fillDirection", uiFillDirectionToStr(im->fillDirection)},
                           {"fillOrigin", uiFillOriginToStr(im->fillOrigin)},
                           {"fillAmount", im->fillAmount} };
        }
        if (node.hasSlider())
        {
            const auto& s = node.getSlider();
            j["slider"] = { {"anchorMin", vec2ToJsonXY(s->anchorMin)},
                            {"anchorMax", vec2ToJsonXY(s->anchorMax)},
                            {"pivot", vec2ToJsonXY(s->pivot)},
                            {"position", vec2ToJsonXY(s->position)},
                            {"size", vec2ToJsonXY(s->size)},
                            {"color", vec4ToJsonXYZW(s->color)},
                            {"visible", s->visible},
                            {"interactable", s->interactable},
                            {"value", s->value},
                            {"minValue", s->minValue},
                            {"maxValue", s->maxValue},
                            {"wholeNumbers", s->wholeNumbers},
                            {"direction", uiSliderDirectionToStr(s->direction)},
                            {"fillColor", vec4ToJsonXYZW(s->fillColor)},
                            {"handleColor", vec4ToJsonXYZW(s->handleColor)},
                            {"handleSize", s->handleSize},
                            {"atlasPath", s->atlasPath},
                            {"backgroundSprite", s->backgroundSprite},
                            {"fillSprite", s->fillSprite},
                            {"handleSprite", s->handleSprite} };
        }
        if (node.hasCheckbox())
        {
            const auto& c = node.getCheckbox();
            j["checkbox"] = { {"anchorMin", vec2ToJsonXY(c->anchorMin)},
                              {"anchorMax", vec2ToJsonXY(c->anchorMax)},
                              {"pivot", vec2ToJsonXY(c->pivot)},
                              {"position", vec2ToJsonXY(c->position)},
                              {"size", vec2ToJsonXY(c->size)},
                              {"color", vec4ToJsonXYZW(c->color)},
                              {"visible", c->visible},
                              {"interactable", c->interactable},
                              {"isOn", c->isOn},
                              {"checkColor", vec4ToJsonXYZW(c->checkColor)},
                              {"checkPadding", c->checkPadding},
                              {"atlasPath", c->atlasPath},
                              {"backgroundSprite", c->backgroundSprite},
                              {"checkmarkSprite", c->checkmarkSprite} };
        }
        if (node.hasToggle())
        {
            const auto& t = node.getToggle();
            j["toggle"] = { {"anchorMin", vec2ToJsonXY(t->anchorMin)},
                            {"anchorMax", vec2ToJsonXY(t->anchorMax)},
                            {"pivot", vec2ToJsonXY(t->pivot)},
                            {"position", vec2ToJsonXY(t->position)},
                            {"size", vec2ToJsonXY(t->size)},
                            {"visible", t->visible},
                            {"interactable", t->interactable},
                            {"isOn", t->isOn},
                            {"offColor", vec4ToJsonXYZW(t->offColor)},
                            {"onColor", vec4ToJsonXYZW(t->onColor)},
                            {"knobColor", vec4ToJsonXYZW(t->knobColor)},
                            {"knobSize", t->knobSize},
                            {"knobPadding", t->knobPadding},
                            {"atlasPath", t->atlasPath},
                            {"backgroundSprite", t->backgroundSprite},
                            {"knobSprite", t->knobSprite} };
        }
        if (node.hasScrollbar())
        {
            const auto& s = node.getScrollbar();
            j["scrollbar"] = { {"anchorMin", vec2ToJsonXY(s->anchorMin)},
                               {"anchorMax", vec2ToJsonXY(s->anchorMax)},
                               {"pivot", vec2ToJsonXY(s->pivot)},
                               {"position", vec2ToJsonXY(s->position)},
                               {"size", vec2ToJsonXY(s->size)},
                               {"color", vec4ToJsonXYZW(s->color)},
                               {"visible", s->visible},
                               {"interactable", s->interactable},
                               {"value", s->value},
                               {"handleFraction", s->handleFraction},
                               {"direction", uiScrollbarDirectionToStr(s->direction)},
                               {"numberOfSteps", s->numberOfSteps},
                               {"handleColor", vec4ToJsonXYZW(s->handleColor)},
                               {"scrollStep", s->scrollStep},
                               {"atlasPath", s->atlasPath},
                               {"backgroundSprite", s->backgroundSprite},
                               {"handleSprite", s->handleSprite} };
        }
        if (node.hasInputField())
        {
            const auto& f = node.getInputField();
            // caretPos is NOT saved: it is where the cursor was in that session, not
            // scene data.
            j["inputField"] = { {"anchorMin", vec2ToJsonXY(f->anchorMin)},
                                {"anchorMax", vec2ToJsonXY(f->anchorMax)},
                                {"pivot", vec2ToJsonXY(f->pivot)},
                                {"position", vec2ToJsonXY(f->position)},
                                {"size", vec2ToJsonXY(f->size)},
                                {"color", vec4ToJsonXYZW(f->color)},
                                {"visible", f->visible},
                                {"interactable", f->interactable},
                                {"readOnly", f->readOnly},
                                {"text", f->text},
                                {"placeholder", f->placeholder},
                                {"fontPath", f->fontPath},
                                {"fontSize", f->fontSize},
                                {"textColor", vec4ToJsonXYZW(f->textColor)},
                                {"placeholderColor", vec4ToJsonXYZW(f->placeholderColor)},
                                {"align", uiTextAlignToStr(f->align)},
                                {"padding", f->padding},
                                {"characterLimit", f->characterLimit},
                                {"contentType", uiInputContentTypeToStr(f->contentType)},
                                {"passwordChar", f->passwordChar},
                                {"caretColor", vec4ToJsonXYZW(f->caretColor)},
                                {"caretWidth", f->caretWidth},
                                {"caretBlinkRate", f->caretBlinkRate},
                                {"atlasPath", f->atlasPath},
                                {"backgroundSprite", f->backgroundSprite} };
        }
        if (node.hasDropdown())
        {
            const auto& d = node.getDropdown();
            // isOpen is NOT saved: a scene that opened with the list
            // dropped down would have a panel covering the menu right after loading.
            j["dropdown"] = { {"anchorMin", vec2ToJsonXY(d->anchorMin)},
                              {"anchorMax", vec2ToJsonXY(d->anchorMax)},
                              {"pivot", vec2ToJsonXY(d->pivot)},
                              {"position", vec2ToJsonXY(d->position)},
                              {"size", vec2ToJsonXY(d->size)},
                              {"color", vec4ToJsonXYZW(d->color)},
                              {"visible", d->visible},
                              {"interactable", d->interactable},
                              {"options", d->options},
                              {"value", d->value},
                              {"itemHeight", d->itemHeight},
                              {"maxVisibleItems", d->maxVisibleItems},
                              {"listColor", vec4ToJsonXYZW(d->listColor)},
                              {"itemColor", vec4ToJsonXYZW(d->itemColor)},
                              {"itemSelectedColor", vec4ToJsonXYZW(d->itemSelectedColor)},
                              {"arrowColor", vec4ToJsonXYZW(d->arrowColor)},
                              {"fontPath", d->fontPath},
                              {"fontSize", d->fontSize},
                              {"textColor", vec4ToJsonXYZW(d->textColor)},
                              {"padding", d->padding},
                              {"atlasPath", d->atlasPath},
                              {"backgroundSprite", d->backgroundSprite},
                              {"arrowSprite", d->arrowSprite},
                              {"itemSprite", d->itemSprite} };
        }
        if (node.hasScrollView())
        {
            const auto& v = node.getScrollView();
            j["scrollView"] = { {"anchorMin", vec2ToJsonXY(v->anchorMin)},
                                {"anchorMax", vec2ToJsonXY(v->anchorMax)},
                                {"pivot", vec2ToJsonXY(v->pivot)},
                                {"position", vec2ToJsonXY(v->position)},
                                {"size", vec2ToJsonXY(v->size)},
                                {"color", vec4ToJsonXYZW(v->color)},
                                {"visible", v->visible},
                                {"horizontal", v->horizontal},
                                {"vertical", v->vertical},
                                {"contentSize", vec2ToJsonXY(v->contentSize)},
                                {"normalizedPosition", vec2ToJsonXY(v->normalizedPosition)},
                                {"scrollSensitivity", v->scrollSensitivity},
                                {"atlasPath", v->atlasPath},
                                {"backgroundSprite", v->backgroundSprite} };
        }
        if (node.hasLayout())
        {
            const auto& l = node.getLayout();
            j["layout"] = { {"anchorMin", vec2ToJsonXY(l->anchorMin)},
                            {"anchorMax", vec2ToJsonXY(l->anchorMax)},
                            {"pivot", vec2ToJsonXY(l->pivot)},
                            {"position", vec2ToJsonXY(l->position)},
                            {"size", vec2ToJsonXY(l->size)},
                            {"visible", l->visible},
                            {"mode", uiLayoutModeToStr(l->mode)},
                            {"paddingLeft", l->paddingLeft},
                            {"paddingRight", l->paddingRight},
                            {"paddingTop", l->paddingTop},
                            {"paddingBottom", l->paddingBottom},
                            {"spacing", vec2ToJsonXY(l->spacing)},
                            {"cellSize", vec2ToJsonXY(l->cellSize)},
                            {"columns", l->columns},
                            {"crossAlign", uiCrossAlignToStr(l->crossAlign)},
                            {"fitWidth", l->fitWidth},
                            {"fitHeight", l->fitHeight},
                            {"ignoreLayout", l->ignoreLayout},
                            {"clipChildren", l->clipChildren} };
        }
        if (node.hasAnimator())
            j["animator"] = animatorToJson(*node.getAnimator());
        if (node.hasAudioClip())
        {
            const auto& clip = node.getAudioClip();
            j["audioClip"] = { {"path", clip->getPath()},
                                // By name, not by enum index: reordering
                                // AudioBus cannot change the saved bus.
                                {"bus", audioBusToStr(clip->getBus())},
                                {"loadMode", audioLoadModeToStr(clip->getLoadMode())},
                                {"rolloff", audioRolloffToStr(clip->getRolloff())},
                                {"spread", clip->getSpread()},
                                {"stereoPan", clip->getStereoPan()},
                                {"dopplerLevel", clip->getDopplerLevel()},
                                {"mute", clip->getMute()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()},
                                {"playOnAwake", clip->getPlayOnAwake()},
                                {"volume", clip->getVolume()},
                                {"pitch", clip->getPitch()},
                                {"minDistance", clip->getMinDistance()},
                                {"maxDistance", clip->getMaxDistance()} };
        }
        if (node.hasReverbZone())
        {
            const auto& z = node.getReverbZone();
            // Neither position nor radius-in-world: the position comes from the
            // worldTransform, as in the Audio Listener.
            j["reverbZone"] = { {"preset", z->getPreset()},
                                 {"minDistance", z->getMinDistance()},
                                 {"maxDistance", z->getMaxDistance()},
                                 {"enabled", z->getEnabled()} };
        }
        if (node.hasAudioListener())
        {
            // Neither position nor orientation: they come from the worldTransform, which is already
            // serialized as the node's localTransform.
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
                            // Preserves integers as integers in the JSON
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
            j["children"].push_back(nodeToJson(*child, assetRoot, carryOverrideBaseline));

        return j;
    }

    // --- Tolerant reading of numbers from a potentially corrupt .scene ---
    //
    // std::clamp(NaN, lo, hi) returns NaN (every comparison with NaN is
    // false, so the clamp does not stop it) and nlohmann serializes a NaN
    // as JSON "null". A value like that coming from a broken Lua script (a
    // 0/0, for example — see the equivalent guard in ScriptBindings.cpp)
    // passes the clamp of setVolume/setPitch/etc., sneaks into the .scene as
    // null and, when reread, both ".at(key).get<float>()" and
    // ".value(key, default)" throw json::exception (type_error.302, "type
    // must be number, but is null"). Before this fix that exception escaped
    // from nodeFromJson with nobody telling it apart from a really corrupt
    // scene, and Scene::fromJson caught it returning false: A single
    // corrupt field took down the load of the WHOLE scene. Infinities, on the
    // other hand, the range clamp does stop fine (clamp(+inf,0,1) == 1.0) —
    // the truly dangerous one is NaN, not infinity; it is checked with
    // std::isfinite (covers both) for robustness, but it is the NaN case that
    // motivates this whole block.
    //
    // warnings accepts nullptr for signature robustness, but in practice it
    // never is: the 9 call-sites of jsonToVec3 (and, in cascade, everything that
    // hangs from nodeFromJson) pass &m_warnings — the three callers of
    // nodeFromJson (fromJson, insertFromJson, cloneGameObject) always do.
    //
    // required distinguishes two families of fields:
    //  - required == false (default): legitimate back-compat. They are fields that
    //    were added over the life of the format (volume, pitch, fov,
    //    near, far, mass, drag, threshold...) and an old scene never
    //    wrote them. Absent -> silent default, no warning.
    //  - required == true: fields that nodeToJson ALWAYS writes, unconditio-
    //    nally (halfExtents/center of the colliders, pos/color/uv/normal/
    //    tangent of each vertex...). There absence is NEVER back-compat:
    //    it is the same corruption (badly resolved merge, truncated write,
    //    hand edit) as a null or non-finite value, so it also warns
    //    naming the field and the object instead of silently fabricating a
    //    plausible value (a 25-unit box at the origin that the user
    //    sees, does not question, and ends up overwriting the real data on Save).

    // Reads j[key] as float. Absent: silent if !required, warns if
    // required. Null value, non-numeric type, or non-finite number (NaN/Inf):
    // ALWAYS warns (if there is a channel) naming the field, and falls to def.
    float readFloat(const nlohmann::json& j, const char* key, float def,
                     std::vector<std::string>* warnings, const std::string& contexto,
                     bool required)
    {
        if (!j.contains(key))
        {
            if (required && warnings)
                warnings->push_back(contexto + "." + key +
                                     ": missing in the scene, using the default value");
            return def;
        }
        const nlohmann::json& v = j[key];
        if (v.is_null() || !v.is_number())
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": corrupt value in the scene, using the default value");
            return def;
        }
        float f = v.get<float>();
        if (!std::isfinite(f))
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": non-finite value (NaN/Inf) in the scene, using the default value");
            return def;
        }
        return f;
    }

    bool readBool(const nlohmann::json& j, const char* key, bool def,
                   std::vector<std::string>* warnings, const std::string& contexto,
                   bool required)
    {
        if (!j.contains(key))
        {
            if (required && warnings)
                warnings->push_back(contexto + "." + key +
                                     ": missing in the scene, using the default value");
            return def;
        }
        const nlohmann::json& v = j[key];
        if (!v.is_boolean())
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": corrupt value in the scene, using the default value");
            return def;
        }
        return v.get<bool>();
    }

    // Empty string as def also serves as a "no usable value" signal: it is
    // what the audioClip block looks at to decide whether to create the component.
    std::string readString(const nlohmann::json& j, const char* key, const std::string& def,
                            std::vector<std::string>* warnings, const std::string& contexto,
                            bool required)
    {
        if (!j.contains(key))
        {
            if (required && warnings)
                warnings->push_back(contexto + "." + key +
                                     ": missing in the scene, using the default value");
            return def;
        }
        const nlohmann::json& v = j[key];
        if (!v.is_string())
        {
            if (warnings)
                warnings->push_back(contexto + "." + key +
                                     ": corrupt value in the scene, using the default value");
            return def;
        }
        return v.get<std::string>();
    }

    // The two "named component" vectors of the Button. Each component
    // goes through readFloat, so a null (serialized NaN) or a weird type falls
    // to the default and warns instead of taking down the whole scene load.
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

    // Variant of readFloat for an ELEMENT of a JSON array by index (instead
    // of an object key) — used by jsonToVec3/jsonToMat4/uv.
    //
    // NOTE: "arr is not an array" and "arr is an array but shorter than
    // expected" are two DIFFERENT anomalies and are treated differently (finding 2
    // of the review): a non-array value (typically null — the exact form
    // a serialized NaN takes, see the big comment above) is real
    // corruption and ALWAYS warns, whether or not the field is required. A short array
    // (index out of range) is the signature of "absent field" when the
    // caller extracted it with ".value(key, array())": there the rule
    // of required does apply, as in readFloat.
    float readArrayFloat(const nlohmann::json& arr, size_t idx, float def,
                          std::vector<std::string>* warnings, const std::string& contexto,
                          bool required)
    {
        if (!arr.is_array())
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: expected a number and it is not one, using the default value");
            return def;
        }
        if (idx >= arr.size())
        {
            if (required && warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: missing in the scene, using the default value");
            return def;
        }
        const nlohmann::json& v = arr[idx];
        if (v.is_null() || !v.is_number())
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: corrupt value in the scene, using the default value");
            return def;
        }
        float f = v.get<float>();
        if (!std::isfinite(f))
        {
            if (warnings)
                warnings->push_back(contexto + "[" + std::to_string(idx) +
                                     "]: non-finite value (NaN/Inf) in the scene, using the default value");
            return def;
        }
        return f;
    }

    // Unlike jsonToVec3 (which fills in component by component), here
    // ANY corrupt float among the 16 discards the whole matrix and falls to the
    // identity: a "half" transformation (15 original values + 1
    // set to its identity value) can look plausible and actually
    // have the scale or rotation silently broken — a recognizable
    // identity and a clear warning are preferable to a Frankenstein of
    // mixed fields. See the comment block above for why NaN.
    glm::mat4 jsonToMat4(const nlohmann::json& j, std::vector<std::string>* warnings,
                          const std::string& contexto)
    {
        bool ok = j.is_array() && j.size() >= 16;
        for (int i = 0; ok && i < 16; ++i)
            ok = j[i].is_number() && std::isfinite(j[i].get<float>());
        if (!ok)
        {
            if (warnings)
                warnings->push_back(contexto + ": corrupt localTransform in the scene "
                                     "(non-numeric, missing or non-finite value); using the identity matrix");
            return glm::mat4(1.0f);
        }
        glm::mat4 m(1.0f);
        float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            p[i] = j[i].get<float>();
        return m;
    }

    // required is forwarded as is to the 3 readArrayFloat: a required vec3
    // absent or corrupt warns 3 times (once per component), but each line
    // already names the object and the field (context), so it is still
    // diagnosable — it does not deserve the complexity of deduplicating into a single
    // warning at vec3 level.
    glm::vec3 jsonToVec3(const nlohmann::json& j, std::vector<std::string>* warnings,
                         const std::string& contexto, const glm::vec3& def = glm::vec3(0.0f),
                         bool required = false)
    {
        return glm::vec3(readArrayFloat(j, 0, def.x, warnings, contexto, required),
                          readArrayFloat(j, 1, def.y, warnings, contexto, required),
                          readArrayFloat(j, 2, def.z, warnings, contexto, required));
    }

    // Vertex: nodeToJson ALWAYS writes it with its 5 fields complete (a "half"
    // vertex is never optional) — all required (finding 1 of the
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

    // Creates the procedural Mesh corresponding to meshName (case-insensitive),
    // with the same fixed parameters as ScenePanel::createBasicShape. nullptr
    // if meshName matches none of the 4 basic shapes.
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

    // Caches with the life of ONE load (fromJson, cloneGameObject,
    // insertFromJson): within it the file is stable and is shared
    // among the nodes that repeat sourcePath. See the comment of
    // hasBonesCache in Scene::fromJson for why they do not live longer.
    //  - hasBones: the bone probe by sourcePath.
    //  - staticModels: the StaticModel of the SYNCHRONOUS static branch (Stop of
    //    Play reloads without loader or preload). Without it each node did its
    //    own full Assimp ReadFile: 60 pieces of a .glb = 60
    //    reads of the same file. One loadStatic per file and each node
    //    takes meshes[piece]; the mesh of each piece is shared among the
    //    nodes that ask for it (whoever edits it copies it, editMesh).
    //  - staticErrors: the failure of that loadStatic, so that the other nodes
    //    of the same file give the same warning without rereading it.
    struct NodeLoadCache
    {
        std::unordered_map<std::string, bool> hasBones;
        struct StaticEntry
        {
            DonTopo::StaticModel                                     model;
            std::unordered_map<int, std::shared_ptr<const DonTopo::Mesh>> byPiece;
        };
        std::unordered_map<std::string, StaticEntry> staticModels;
        std::unordered_map<std::string, std::string> staticErrors;
    };

    // The piece `piece` of `sourcePath` through the cache: same result and
    // same errors as ModelLoader::load(sourcePath, piece) -- out-of-range
    // piece throws with the path and the index --, with ONE read per file.
    std::shared_ptr<const DonTopo::Mesh> staticPieceFromCache(NodeLoadCache& cache, const std::string& sourcePath,
                                                             int piece)
    {
        if (auto err = cache.staticErrors.find(sourcePath); err != cache.staticErrors.end())
            throw std::runtime_error(err->second);
        auto it = cache.staticModels.find(sourcePath);
        if (it == cache.staticModels.end())
        {
            try
            {
                NodeLoadCache::StaticEntry entry;
                entry.model = DonTopo::ModelLoader::loadStatic(sourcePath);
                it = cache.staticModels.emplace(sourcePath, std::move(entry)).first;
            }
            catch (const std::exception& e)
            {
                cache.staticErrors[sourcePath] = e.what();
                throw;
            }
        }
        NodeLoadCache::StaticEntry& entry = it->second;
        if (piece < 0 || static_cast<size_t>(piece) >= entry.model.meshes.size())
            throw std::runtime_error("'" + sourcePath + "' has no piece " + std::to_string(piece) +
                                     " (it has " + std::to_string(entry.model.meshes.size()) + ")");
        auto& mesh = entry.byPiece[piece];
        if (!mesh) mesh = std::make_shared<const DonTopo::Mesh>(entry.model.meshes[piece]);
        return mesh;
    }

    // Rebuilds node (already inserted in the tree) from j, and recursively
    // its children. parentWorld is the already resolved worldTransform of the parent —
    // needed to pass a correct worldTransform to the collider
    // factories (which set the initial pose of the PhysX actor from it).
    // carryOverrideBaseline: read counterpart of the same nodeToJson flag —
    // see its big comment, next to "baseAlbedo". Default false (disk);
    // cloneGameObject and insertFromJson set it to true.
    void nodeFromJson(const nlohmann::json& j, GameObject* node, const glm::mat4& parentWorld,
                       DonTopo::PhysicsManager& physics, DonTopo::AudioManager& audio,
                       std::vector<std::string>* warnings,
                       NodeLoadCache* loadCache,
                       const std::string& assetRoot,
                       DonTopo::AsyncAssetLoader* loader = nullptr,
                       const DonTopo::PreloadedMeshCache* preloaded = nullptr,
                       bool carryOverrideBaseline = false)
    {
        // "id" does not exist in .scene files saved before this field —
        // the id that the GameObject constructor already assigned (atomic
        // counter) is left, backward-compatible. When it does exist (own Undo/Redo
        // snapshots or re-saved scenes), the same id is reused: this way an
        // Undo of Delete rebuilds the GameObject with the original id and the following
        // commands in the stack keep resolving it correctly.
        if (j.contains("id"))
        {
            // An id that is not an unsigned integer is NOT accepted: before it was read with
            // .at().get<uint64_t>() and a null (or a string) threw, the
            // exception went up to the catch of fromJson and the load of
            // the WHOLE scene was lost over one field. The id the
            // constructor already set is kept, which is also the only safe value: inventing a 0
            // for all the broken nodes would make them clash with each other.
            if (j["id"].is_number_unsigned())
            {
                node->id = j["id"].get<uint64_t>();
                // The global id counter does not see this assignment: without
                // advancing it, a file from another session (ids higher than those
                // handed out here) leaves the counter behind ids that are already
                // in the tree and the next new GameObject repeats one.
                // See reserveIdAtLeast.
                GameObject::reserveIdAtLeast(node->id);
            }
            else if (warnings)
            {
                warnings->push_back("node '" + node->name + "'.id: corrupt value in the scene, "
                                     "the object gets a new id");
            }
        }
        node->localTransform = jsonToMat4(j.value("localTransform", nlohmann::json::array()),
                                           warnings, "localTransform of '" + node->name + "'");
        node->worldTransform = parentWorld * node->localTransform;

        node->ssrEnabled = j.value("ssrEnabled", false);
        // The reversed comparison also covers a NaN, which would pass any
        // clamp written as min/max and would end up multiplying the reflection
        // color by NaN.
        const float ssrI   = j.value("ssrIntensity", 0.5f);
        node->ssrIntensity = (ssrI >= 0.0f && ssrI <= 1.0f) ? ssrI : 0.5f;

        if (j.contains("mesh"))
        {
            std::string sourcePath = j["mesh"].value("sourcePath", "");
            std::string meshName   = j["mesh"].value("name", "");
            node->meshVisible      = j["mesh"].value("visible", true);
            // Which piece of the file (Mesh::piece). Absent or invalid = 0, which is
            // what scenes from before loaded.
            int piece = 0;
            if (const auto it = j["mesh"].find("piece"); it != j["mesh"].end() && it->is_number_integer())
                piece = std::max(0, it->get<int>());
            // The "skinned" flag is still SAVED (informative data, and it does not
            // break old files) but it is no longer read: the file rules, on
            // load just as on import. If it were not so, scenes saved
            // before auto-detection — all with the flag at false, because the
            // editor never created skinned — could never have an Animator without
            // reimporting the mesh by hand.
            const bool skinnedFlag = j["mesh"].value("skinned", false);
            bool skinned = false;
            if (!sourcePath.empty())
            {
                // Per-load cache (see hasBonesCache below): without it each
                // node that shares sourcePath with another would repeat the full
                // Assimp ReadFile that hasBones does.
                if (loadCache)
                {
                    auto it = loadCache->hasBones.find(sourcePath);
                    if (it != loadCache->hasBones.end())
                        skinned = it->second;
                    else
                        skinned = loadCache->hasBones[sourcePath] = DonTopo::ModelLoader::hasBones(sourcePath);
                }
                else
                {
                    skinned = DonTopo::ModelLoader::hasBones(sourcePath);
                }
            }

            // hasBones() returns false both if the file has no bones
            // and if it cannot be read (moved/deleted) — the two must be told apart
            // before warning, because saying "no longer declares bones" of a
            // file that simply does not exist is worse than not warning: it points
            // to the wrong place and hides that the mesh did not load at all.
            if (skinnedFlag && !skinned && !sourcePath.empty() && warnings)
            {
                // FULL path, not filename(): in the missing file case
                // this warning and the one in the catch below fire for the same
                // node, and with different identifiers the Log Console seemed
                // to be talking about two different assets.
                if (!std::filesystem::exists(sourcePath))
                {
                    warnings->push_back(sourcePath + ": the scene saved it as animated, but the"
                                                      " file cannot be found (was it moved or deleted?);"
                                                      " the mesh cannot be loaded");
                }
                else
                {
                    warnings->push_back(sourcePath + ": the scene saved it as animated, but the file"
                                                      " no longer declares bones; its animation sources are discarded");
                }
            }
            try
            {
                if (skinned)
                {
                    // The skinned load stays SYNCHRONOUS on purpose, even if there
                    // is a loader: it rebuilds the Animator clip config
                    // (applyClipNamesPositionally/addAnimationSource, below)
                    // from the saved JSON and the already loaded SkinnedMesh.
                    // The asynchronous pump (Task 9, applyLoadedMesh) only does
                    // addSkinnedMesh + setMesh — it does not reapply that config — so
                    // an async scene load would silently lose the saved
                    // clips. The live drop (PropertiesPanel) is safe async
                    // because a freshly dropped FBX brings no saved clips to
                    // rebuild.
                    //
                    // Preload cache: if the runtime already loaded this FBX in
                    // parallel (loadAuto → SkinnedMesh for a rig), a
                    // DEEP COPY is used instead of the loadSkinned from disk. The clip
                    // config below is reapplied all the same over the copy, so
                    // the result is equivalent to the synchronous path without repeating
                    // the ReadFile. Miss (or unexpected non-skinned entry) → disk.
                    //
                    // If the preloaded one ALREADY has the same source
                    // configuration the JSON asks for (the case of the clone and of the undo of
                    // Delete, which seed the cache with the live mesh), it is
                    // SHARED without touching it: neither a 12 MB copy nor re-applying
                    // sources, which duplicated its clips. If it does not match (a freshly
                    // preloaded FBX with another configuration), it is copied and
                    // configured as always.
                    std::shared_ptr<const DonTopo::Mesh> compartida;
                    std::shared_ptr<DonTopo::SkinnedMesh> mesh;
                    if (preloaded)
                    {
                        auto it = preloaded->find(sourcePath);
                        if (it != preloaded->end())
                            if (const auto* sk = dynamic_cast<const DonTopo::SkinnedMesh*>(it->second.get()))
                            {
                                // Without the key (scene saved before it
                                // existed) the scene asks only for the
                                // animations of the FBX itself, which is exactly
                                // what a preloaded one with a single builtin
                                // source of that file brings. Without this equivalence
                                // each node took its own copy (~80 MB with
                                // modelAnimation.fbx). If it brings more sources, it is
                                // copied and configured as before.
                                const bool conClave = j["mesh"].contains("animationSources");
                                const bool soloBuiltin = sk->animationSources.size() == 1 &&
                                                         sk->animationSources[0].builtin &&
                                                         sk->animationSources[0].path == sourcePath;
                                const nlohmann::json fuentesPedidas = conClave
                                    ? j["mesh"]["animationSources"] : nlohmann::json::array();
                                if (conClave ? DonTopo::meshMatchesAnimationConfig(*sk, fuentesPedidas)
                                             : soloBuiltin)
                                    compartida = it->second;
                                else
                                    mesh = std::make_shared<DonTopo::SkinnedMesh>(*sk);
                            }
                    }
                    if (!mesh && !compartida)
                        mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));

                    // Animation sources. The builtin one was already created by loadSkinned:
                    // from it only the NAMES are recovered (a rename), and they are
                    // applied POSITIONALLY in one go (not chaining
                    // renameClip: that collides with itself on a swap of
                    // two names and applies nothing) up to the smaller of the two
                    // sizes — an FBX re-exported with more or fewer clips must
                    // not break the load. A shared mesh already brings them.
                    if (!compartida && j["mesh"].contains("animationSources"))
                    {
                        std::vector<DonTopo::AnimationSourceConfig> fuentes;
                        for (const auto& sj : j["mesh"]["animationSources"])
                        {
                            DonTopo::AnimationSourceConfig cfg;
                            cfg.path    = sj.value("path", std::string());
                            cfg.builtin = sj.value("builtin", false);
                            // The names are applied POSITIONALLY, so
                            // a half list is not "almost right": it shifts all
                            // the following names one place and renames the wrong
                            // clips. Either it comes in whole or none comes in
                            // — same criterion as jsonToMat4 with the
                            // matrix. Before, a single element that was not a
                            // string threw and the whole scene was lost.
                            if (sj.contains("clips"))
                            {
                                const nlohmann::json& cj = sj["clips"];
                                bool clipsOk = cj.is_array();
                                for (size_t ci = 0; clipsOk && ci < cj.size(); ++ci)
                                    clipsOk = cj[ci].is_string();
                                if (clipsOk)
                                    cfg.clipNames = cj.get<std::vector<std::string>>();
                                else if (warnings)
                                    warnings->push_back("mesh of '" + node->name +
                                                         "'.animationSources.clips: corrupt list in the "
                                                         "scene, the names saved for that source are ignored");
                            }

                            fuentes.push_back(std::move(cfg));
                        }

                        // Moved, deleted or other-rig file: it warns and
                        // continues (see applyAnimationSourceConfig). To the warnings of the
                        // parameter (Scene::lastWarnings(), what the Log
                        // Console reads), not to stdout: in a build without a console a
                        // printf is invisible. The parse warnings come out first and
                        // then those of applying; no test fixes the
                        // order.
                        std::vector<std::string> aplicaAvisos;
                        DonTopo::applyAnimationSourceConfig(*mesh, fuentes, aplicaAvisos);
                        if (warnings)
                            for (const std::string& w : aplicaAvisos)
                                warnings->push_back(w);
                    }

                    if (compartida) node->setMesh(compartida);
                    else            node->setMesh(std::move(mesh));
                }
                else if (!sourcePath.empty())
                {
                    // Preload cache first: if the runtime already read this
                    // file in parallel, a DEEP COPY is used instead of
                    // disk (or of enqueuing a request). A rig cached as
                    // SkinnedMesh is copied as such for robustness, although in the
                    // static branch the normal thing is a plain Mesh.
                    // It is SHARED: a static mesh has no configuration
                    // to apply, and whoever edits it (material) copies it at that
                    // moment via editMesh.
                    std::shared_ptr<const DonTopo::Mesh> cached;
                    if (preloaded)
                    {
                        auto it = preloaded->find(DonTopo::meshCacheKey(sourcePath, piece));
                        if (it != preloaded->end() && it->second)
                            cached = it->second;
                    }

                    if (cached)
                    {
                        node->setMesh(std::move(cached));
                    }
                    else if (loader)
                    {
                        // Asynchronous: the GameObject is left without a mesh and the request is
                        // noted. The pump will resolve it by id — never by
                        // pointer, which would be dangling if the user deletes it
                        // while it loads.
                        node->pendingMeshJob = loader->requestMesh(sourcePath, node->id, piece);
                    }
                    else if (loadCache)
                    {
                        // Synchronous (Stop of Play): one read per file, not
                        // per node. See NodeLoadCache.
                        node->setMesh(staticPieceFromCache(*loadCache, sourcePath, piece));
                    }
                    else
                    {
                        auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath, piece));
                        node->setMesh(std::move(mesh));
                    }
                }
                else if (j["mesh"].contains("vertices") && j["mesh"].contains("indices"))
                {
                    // Procedural with serialized geometry (files
                    // saved with this fix or later): it rebuilds the
                    // exact mesh, without depending on which parameters
                    // originally generated it.
                    // The indices are validated BEFORE parsing the vertices: if
                    // the list is broken there is no mesh to assemble and parsing them
                    // would be wasted work (and one warning per vertex as a bonus).
                    // Before, a single non-numeric element threw from
                    // get<vector<uint32_t>>; the catch below saved it, so
                    // the scene was not lost — but the warning spoke of
                    // "could not load the mesh" without naming the field, which is
                    // what sends you to look in the wrong place.
                    const nlohmann::json& idx = j["mesh"]["indices"];
                    bool indicesOk = idx.is_array();
                    for (size_t ii = 0; indicesOk && ii < idx.size(); ++ii)
                        indicesOk = idx[ii].is_number_unsigned();
                    if (!indicesOk)
                    {
                        // Half a geometry is worse than none: same criterion
                        // as jsonToMat4 with the matrix.
                        if (warnings)
                            warnings->push_back("mesh of '" + node->name + "'.indices: corrupt list "
                                                 "in the scene, the object loads without a mesh");
                    }
                    else
                    {
                        auto mesh = std::make_shared<DonTopo::Mesh>();
                        mesh->name = meshName;
                        for (const auto& vj : j["mesh"]["vertices"])
                            mesh->vertices.push_back(jsonToVertex(vj, warnings, "mesh of '" + node->name + "'"));
                        mesh->indices = idx.get<std::vector<uint32_t>>();
                        node->setMesh(std::move(mesh));
                    }
                }
                else if (auto mesh = proceduralMeshByName(meshName))
                {
                    // Fallback for files saved BEFORE this fix
                    // (without vertices/indices) — best-effort with the
                    // fixed parameters of Basic Shapes, same behavior
                    // (potentially incorrect for custom sizes) that
                    // they had before.
                    node->setMesh(std::move(mesh));
                }
            }
            catch (const std::exception& e)
            {
                // Broken asset (moved/deleted) or unsupported format: node
                // is left without a mesh, the rest of the scene keeps loading. Before,
                // the exception was swallowed here with nothing more — if the warning
                // above does not even fire (skinnedFlag == false, or the
                // file never had bones) the user is left without any
                // hint of why the mesh is empty. It is reported through
                // warnings, not stdout: in a build without a console a printf
                // is invisible.
                if (warnings)
                {
                    const std::string ref = sourcePath.empty() ? meshName : sourcePath;
                    warnings->push_back(ref + ": could not load the mesh (" + e.what() + ")");
                }
            }

            // Texture overrides. A block that is not an array, or an entry
            // without a numeric "index", is discarded with a warning: half a configuration
            // is worse than none, same criterion as jsonToMat4 with the matrix.
            if (j["mesh"].contains("materials"))
            {
                const nlohmann::json& mats = j["mesh"]["materials"];
                if (!mats.is_array())
                {
                    if (warnings)
                        warnings->push_back("mesh of '" + node->name + "'.materials: not a list, "
                                            "the hand-assigned textures are discarded");
                }
                else
                {
                    for (const auto& entry : mats)
                    {
                        if (!entry.is_object() || !entry.contains("index")
                            || !entry["index"].is_number_integer())
                        {
                            if (warnings)
                                warnings->push_back("mesh of '" + node->name + "'.materials: entry without a "
                                                    "valid index, discarded");
                            continue;
                        }
                        MaterialOverride ov;
                        ov.index  = entry["index"].get<int>();
                        ov.albedo   = fromStoredPath(entry.value("albedo", ""), assetRoot);
                        ov.normal   = fromStoredPath(entry.value("normal", ""), assetRoot);
                        ov.orm      = fromStoredPath(entry.value("orm",    ""), assetRoot);
                        ov.matAsset = fromStoredPath(entry.value("matAsset", ""), assetRoot);
                        // Absent = -1.0f (the "no override" sentinel; see
                        // MaterialOverride in GameObject.h), same criterion as
                        // "absent = empty string" of the three paths above.
                        // Through readFloat and not through a raw entry.value<float>():
                        // this whole block is NOT inside any try, and a
                        // "metallic": null or "metallic": "0.5" with .value<float>()
                        // throws type_error.302 — the exception goes up to
                        // fromJson(), which returns false, and THE WHOLE SCENE IS LOST
                        // without saying why. readFloat is the same guard
                        // the rest of the file already uses for this problem:
                        // it warns naming the field and falls to the sentinel.
                        // clamp(-1..1) also cuts a hand-mistyped "metallic": 5.0
                        // before it reaches the Material and the dedup
                        // key — the sentinel (negative) is not
                        // affected, clamp(-1, -1, 1) leaves it the same.
                        const std::string materialesCtx = "mesh of '" + node->name + "'.materials";
                        ov.metallic  = std::clamp(readFloat(entry, "metallic",  -1.0f, warnings, materialesCtx), -1.0f, 1.0f);
                        ov.roughness = std::clamp(readFloat(entry, "roughness", -1.0f, warnings, materialesCtx), -1.0f, 1.0f);
                        // The baseline is ONLY read on the MEMORY path (see
                        // the big comment of nodeToJson, next to the same
                        // flag): on disk these fields, even if they were in the
                        // JSON, are ignored on purpose — the correct baseline of
                        // a disk load is the one that applyMaterialOverrides
                        // captures over the material just derived from the FBX, not a saved one
                        // that could be from ANOTHER export of the model.
                        if (carryOverrideBaseline)
                        {
                            ov.baseAlbedo      = fromStoredPath(entry.value("baseAlbedo", ""), assetRoot);
                            ov.baseAlbedoTaken = entry.value("baseAlbedoTaken", false);
                            ov.baseNormal      = fromStoredPath(entry.value("baseNormal", ""), assetRoot);
                            ov.baseNormalTaken = entry.value("baseNormalTaken", false);
                            ov.baseOrm         = fromStoredPath(entry.value("baseOrm", ""), assetRoot);
                            ov.baseOrmTaken    = entry.value("baseOrmTaken", false);
                            // Same reason as metallic/roughness above:
                            // readFloat/readBool instead of a raw entry.value<T>(),
                            // so that a corrupt field warns and falls to the
                            // default instead of taking down the whole fromJson().
                            ov.baseMetallic       = readFloat(entry, "baseMetallic", 0.0f, warnings, materialesCtx);
                            ov.baseMetallicTaken  = readBool(entry, "baseMetallicTaken", false, warnings, materialesCtx);
                            ov.baseRoughness      = readFloat(entry, "baseRoughness", 0.0f, warnings, materialesCtx);
                            ov.baseRoughnessTaken = readBool(entry, "baseRoughnessTaken", false, warnings, materialesCtx);
                        }
                        node->materialOverrides.push_back(std::move(ov));
                    }
                }
            }

            // Overrides applied NOW that the mesh (if there was one: any
            // of the FIVE setMesh of the branches above — skinned; cached
            // and disk, the two of the static branch; serialized and fallback
            // by name, the two of the procedural) is already set. On the
            // ASYNCHRONOUS path (loader->requestMesh, further above) there is no mesh
            // yet and this call does nothing — the application is done by
            // AsyncAssetLoader::applyLoadedMesh when the worker delivers.
            //
            // Also gated by !materialOverrides.empty(): without overrides,
            // the loop below has nothing to traverse, but
            // materialsOfMesh (here, AND again inside
            // applyMaterialOverrides) would build anyway a
            // std::vector<Material*> on the heap for each node with a mesh. This
            // path is hit by cloneGameObject on every Lua Scene.Instantiate
            // in Play, with a measured budget of 24.5 ms/clone (see its
            // comment) — two extra allocs per node, most with no
            // override, are not free there.
            if (node->hasMesh() && !node->materialOverrides.empty())
            {
                // The out-of-range index warning that the comment
                // of GameObject::applyMaterialOverrides promises ("it is given by whoever has a
                // channel to give it"): here `warnings` exists, so it is given.
                // The text comes from collectMaterialOverrideWarnings and not from its own
                // loop, because the asynchronous pump —the normal path of
                // a large scene— has to give EXACTLY the same warning
                // through its channel, and with two twin loops that lasts as long as it takes
                // someone to touch only one.
                if (warnings)
                    collectMaterialOverrideWarnings(*node, *warnings);
                applyMaterialOverrides(*node);
            }
        }

        // The colliders are always loaded as static (dynamic=false); if the
        // node brings a Rigidbody (or legacy useGravity), the block below promotes it
        // to dynamic via physics.attachRigidbody.
        if (j.contains("boxCollider"))
        {
            const auto& c = j["boxCollider"];
            const std::string ctx = "boxCollider of '" + node->name + "'";
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.value("halfExtents", nlohmann::json::array()), warnings, ctx + ".halfExtents", glm::vec3(25.0f), true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getBoxCollider()->setOwner(node);
            physics.setTrigger(node->getBoxCollider(), c.value("isTrigger", false));
            // Material per collider. required=false: a scene saved before
            // this field loads without warnings and with the usual defaults
            // (0.5 / 0.5 / 0.1), that is, with the same behavior it had.
            node->getBoxCollider()->setFriction(
                readFloat(c, "staticFriction",  0.5f, warnings, ctx),
                readFloat(c, "dynamicFriction", 0.5f, warnings, ctx));
            node->getBoxCollider()->setBounciness(readFloat(c, "bounciness", 0.1f, warnings, ctx));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            const std::string ctx = "sphereCollider of '" + node->name + "'";
            node->setSphereCollider(physics.createSphereColliderComponent(
                readFloat(c, "radius", 25.0f, warnings, ctx, true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getSphereCollider()->setOwner(node);
            physics.setTrigger(node->getSphereCollider(), c.value("isTrigger", false));
            node->getSphereCollider()->setFriction(
                readFloat(c, "staticFriction",  0.5f, warnings, ctx),
                readFloat(c, "dynamicFriction", 0.5f, warnings, ctx));
            node->getSphereCollider()->setBounciness(readFloat(c, "bounciness", 0.1f, warnings, ctx));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            const std::string ctx = "capsuleCollider of '" + node->name + "'";
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                readFloat(c, "radius", 15.0f, warnings, ctx, true),
                readFloat(c, "halfHeight", 25.0f, warnings, ctx, true),
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform, /*dynamic=*/false));
            node->getCapsuleCollider()->setOwner(node);
            physics.setTrigger(node->getCapsuleCollider(), c.value("isTrigger", false));
            node->getCapsuleCollider()->setFriction(
                readFloat(c, "staticFriction",  0.5f, warnings, ctx),
                readFloat(c, "dynamicFriction", 0.5f, warnings, ctx));
            node->getCapsuleCollider()->setBounciness(readFloat(c, "bounciness", 0.1f, warnings, ctx));
        }
        if (j.contains("planeCollider"))
        {
            const auto& c = j["planeCollider"];
            const std::string ctx = "planeCollider of '" + node->name + "'";
            node->setPlaneCollider(physics.createPlaneColliderComponent(
                jsonToVec3(c.value("center", nlohmann::json::array()), warnings, ctx + ".center", glm::vec3(0.0f), true),
                node->worldTransform));
            node->getPlaneCollider()->setOwner(node);
            physics.setTrigger(node->getPlaneCollider(), c.value("isTrigger", false));
            node->getPlaneCollider()->setFriction(
                readFloat(c, "staticFriction",  0.5f, warnings, ctx),
                readFloat(c, "dynamicFriction", 0.5f, warnings, ctx));
            node->getPlaneCollider()->setBounciness(readFloat(c, "bounciness", 0.1f, warnings, ctx));
        }

        // Rigidbody: new block. Back-compat: old scenes saved
        // useGravity INSIDE the collider; if there is no rigidbody block but a
        // collider brings legacy useGravity == true, we synthesize a Rigidbody
        // inheriting that value (dynamic body as before). Legacy useGravity
        // == false (kinematic without gravity) equals a static collider, which
        // is exactly the default state → no Rigidbody is created.
        auto legacyGravity = [&](const char* key) -> int {
            if (!j.contains(key) || !j[key].contains("useGravity")) return -1; // no legacy field
            const nlohmann::json& g = j[key]["useGravity"];
            if (!g.is_boolean())
            {
                // This is the compatibility path for scenes PRIOR
                // to the Rigidbody: by definition what arrives here is an
                // old file, that is, the worst possible place to be strict.
                // Before, a get<bool>() on a corrupt value threw and
                // the whole load was lost. It warns and is treated as "no
                // legacy field", which leaves the collider static — the default state.
                if (warnings)
                    warnings->push_back(std::string(key) + " of '" + node->name +
                                         "'.useGravity: corrupt value in the scene, ignored");
                return -1;
            }
            return g.get<bool>() ? 1 : 0;
        };
        if (j.contains("rigidbody"))
        {
            const auto& r = j["rigidbody"];
            const std::string ctx = "rigidbody of '" + node->name + "'";
            auto rb = std::make_shared<Rigidbody>();
            rb->setMass(readFloat(r, "mass", 1.0f, warnings, ctx));
            rb->setUseGravity(r.value("useGravity", true));
            rb->setIsKinematic(r.value("isKinematic", false));
            rb->setDrag(readFloat(r, "drag", 0.0f, warnings, ctx));
            rb->setAngularDrag(readFloat(r, "angularDrag", 0.05f, warnings, ctx));
            rb->setConstraints(r.value("constraints", 0u));
            // Additive fields: a scene saved before they existed falls to the
            // default false, which is the usual behavior. They are set
            // BEFORE attachRigidbody because bindActor is what pushes them to the
            // actor and to the collider.
            rb->setCcd(r.value("ccd", false));
            rb->setInterpolate(r.value("interpolate", false));
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
        // Additive block: scenes saved before this field do not bring it
        // and load the same (version stays at 1). Unknown "mode" value ->
        // perspective.
        if (j.contains("camera"))
        {
            const auto& c = j["camera"];
            const std::string ctx = "camera of '" + node->name + "'";
            auto cam = std::make_shared<CameraComponent>();
            cam->setMode(c.value("mode", std::string("perspective")) == "orthographic"
                             ? CameraComponent::ProjectionMode::Orthographic
                             : CameraComponent::ProjectionMode::Perspective);
            // far BEFORE near: setNear clamps against the CURRENT far, so
            // loading them the other way round would clip a large near against the default
            // far (2000) and leave it wrong.
            cam->setFar(readFloat(c, "far", 2000.0f, warnings, ctx));
            cam->setNear(readFloat(c, "near", 1.0f, warnings, ctx));
            cam->setFov(readFloat(c, "fov", 45.0f, warnings, ctx));
            cam->setOrthographicSize(readFloat(c, "orthographicSize", 100.0f, warnings, ctx));
            node->setCameraComponent(cam);
        }
        // Additive block: scenes saved before this field do not bring it
        // and load the same (version stays at 1). Without this block there is no probe, and
        // without a probe the object is lit with the usual global IBL.
        if (j.contains("reflectionProbe"))
        {
            const auto& p = j["reflectionProbe"];
            const std::string ctx = "reflectionProbe of '" + node->name + "'";
            auto probe = std::make_shared<ReflectionProbeComponent>();
            probe->setRadius(readFloat(p, "radius", 300.0f, warnings, ctx));
            probe->setIntensity(readFloat(p, "intensity", 1.0f, warnings, ctx));
            node->setReflectionProbe(probe);
        }
        // Additive block: scenes saved before this field do not bring it
        // and load the same (version stays at 1). Unknown "type" value ->
        // point.
        if (j.contains("light"))
        {
            const auto& l = j["light"];
            const std::string ctx = "light of '" + node->name + "'";
            auto light = std::make_shared<LightComponent>();
            light->setType(lightTypeFromStr(l.value("type", std::string("point"))));
            light->setColor(jsonToVec3(l.value("color", nlohmann::json::array()),
                                       warnings, ctx + ".color", glm::vec3(1.0f)));
            light->setIntensity(readFloat(l, "intensity", 1.0f, warnings, ctx));
            light->setRange(readFloat(l, "range", 300.0f, warnings, ctx));
            // The two setters keep inner <= outer between them, so a
            // .scene with an inverted cone ends up with a valid cone whatever
            // happens (the second setter drags the first).
            light->setOuterAngle(readFloat(l, "outerAngle", 30.0f, warnings, ctx));
            light->setInnerAngle(readFloat(l, "innerAngle", 20.0f, warnings, ctx));
            light->setAreaWidth(readFloat(l, "areaWidth", 100.0f, warnings, ctx));
            light->setAreaHeight(readFloat(l, "areaHeight", 100.0f, warnings, ctx));
            node->setLight(light);
        }
        // Additive block: a scene saved before the Canvas component does not
        // bring the key and loads the same, without component and without warnings.
        if (j.contains("canvas"))
        {
            const auto& c = j["canvas"];
            const std::string ctx = "canvas of '" + node->name + "'";
            // A corrupt bool or string (null, or of the wrong type) falls to the
            // default instead of throwing: .value() does throw with a null, and a
            // broken field cannot take down the load of the whole scene.
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
            canvas->renderMode = uiCanvasRenderModeFromStr(readString(c, "renderMode", std::string(), warnings, ctx));
            canvas->worldScale = readFloat(c, "worldScale", 0.001f, warnings, ctx);
            canvas->billboard  = uiBillboardFromStr(readString(c, "billboard", std::string(), warnings, ctx));
            canvas->depthTest  = readBool(c, "depthTest", true, warnings, ctx);
            node->setCanvas(std::move(canvas));
        }
        // Additive block, same rule as the canvas: a scene saved before
        // the Button component does not bring the key and loads without it or a warning.
        if (j.contains("button"))
        {
            const auto& b = j["button"];
            const std::string ctx = "button of '" + node->name + "'";
            // A corrupt bool or string (null, or of the wrong type) falls to the
            // default instead of throwing: .value() does throw with a null, and a
            // broken field cannot take down the load of the whole scene.
            auto btn = std::make_shared<ButtonComponent>();
            btn->anchorMin = readVec2XY(b, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            btn->anchorMax = readVec2XY(b, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            btn->pivot     = readVec2XY(b, "pivot", glm::vec2(0.0f), warnings, ctx);
            btn->position  = readVec2XY(b, "position", glm::vec2(0.0f), warnings, ctx);
            btn->size      = readVec2XY(b, "size", glm::vec2(160.0f, 40.0f), warnings, ctx);
            btn->color     = readVec4XYZW(b, "color", glm::vec4(1.0f), warnings, ctx);
            btn->visible   = readBool(b, "visible", true, warnings, ctx);
            btn->atlasPath = readString(b, "atlasPath", std::string(), warnings, ctx);
            btn->sprite    = readString(b, "sprite", std::string(), warnings, ctx);

            btn->interactable = readBool(b, "interactable", true, warnings, ctx);
            btn->selected     = readBool(b, "selected", false, warnings, ctx);
            btn->transition   = uiButtonTransitionFromStr(readString(b, "transition", std::string(), warnings, ctx));

            btn->normalColor   = readVec4XYZW(b, "normalColor", glm::vec4(1.0f), warnings, ctx);
            btn->hoverColor    = readVec4XYZW(b, "hoverColor", glm::vec4(1.0f), warnings, ctx);
            btn->pressedColor  = readVec4XYZW(b, "pressedColor", glm::vec4(1.0f), warnings, ctx);
            btn->disabledColor = readVec4XYZW(b, "disabledColor", glm::vec4(1.0f), warnings, ctx);
            btn->selectedColor = readVec4XYZW(b, "selectedColor", glm::vec4(1.0f), warnings, ctx);

            btn->normalSprite   = readString(b, "normalSprite", std::string(), warnings, ctx);
            btn->hoverSprite    = readString(b, "hoverSprite", std::string(), warnings, ctx);
            btn->pressedSprite  = readString(b, "pressedSprite", std::string(), warnings, ctx);
            btn->disabledSprite = readString(b, "disabledSprite", std::string(), warnings, ctx);
            btn->selectedSprite = readString(b, "selectedSprite", std::string(), warnings, ctx);

            btn->fadeDuration = readFloat(b, "fadeDuration", 0.1f, warnings, ctx);

            btn->text      = readString(b, "text", std::string(), warnings, ctx);
            btn->fontPath  = readString(b, "fontPath", std::string(), warnings, ctx);
            btn->fontSize  = readFloat(b, "fontSize", 16.0f, warnings, ctx);
            btn->textColor = readVec4XYZW(b, "textColor", glm::vec4(1.0f), warnings, ctx);
            // Without the key the default is Center (the component's), not Left: that is why
            // it is not enough to pass "" to uiTextAlignFromStr.
            btn->textAlign = (b.contains("textAlign") && b["textAlign"].is_string())
                                 ? uiTextAlignFromStr(b["textAlign"].get<std::string>())
                                 : UiTextAlign::Center;
            // Same: without the key, the component default is Middle. A scene
            // saved before this existed ends up with the label
            // vertically centered, which is exactly the fix.
            btn->textVAlign =
                (b.contains("textVAlign") && b["textVAlign"].is_string())
                    ? uiTextVAlignFromStr(b["textVAlign"].get<std::string>(), UiTextVAlign::Middle)
                    : UiTextVAlign::Middle;
            node->setButton(std::move(btn));
        }
        // Additive block, same rule as the Button: a scene saved before
        // the Text component does not bring the key and loads without it or a warning.
        if (j.contains("text"))
        {
            const auto& t = j["text"];
            const std::string ctx = "text of '" + node->name + "'";
            // Same criterion as the Button: a corrupt bool or string falls to the
            // default instead of throwing, since a broken field cannot take down the
            // load of the whole scene.
            auto txt = std::make_shared<TextComponent>();
            txt->anchorMin = readVec2XY(t, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            txt->anchorMax = readVec2XY(t, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            txt->pivot     = readVec2XY(t, "pivot", glm::vec2(0.0f), warnings, ctx);
            txt->position  = readVec2XY(t, "position", glm::vec2(0.0f), warnings, ctx);
            txt->size      = readVec2XY(t, "size", glm::vec2(160.0f, 40.0f), warnings, ctx);
            txt->color     = readVec4XYZW(t, "color", glm::vec4(1.0f), warnings, ctx);
            txt->visible   = readBool(t, "visible", true, warnings, ctx);

            txt->text     = readString(t, "text", std::string(), warnings, ctx);
            txt->fontPath = readString(t, "fontPath", std::string(), warnings, ctx);
            txt->fontSize = readFloat(t, "fontSize", 16.0f, warnings, ctx);

            txt->outlineWidth = readFloat(t, "outlineWidth", 0.0f, warnings, ctx);
            txt->outlineColor = readVec4XYZW(t, "outlineColor",
                                             glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), warnings, ctx);
            txt->shadowOffset = readVec2XY(t, "shadowOffset", glm::vec2(0.0f), warnings, ctx);
            txt->shadowColor  = readVec4XYZW(t, "shadowColor",
                                             glm::vec4(0.0f, 0.0f, 0.0f, 0.5f), warnings, ctx);

            txt->align    = uiTextAlignFromStr(readString(t, "align", std::string(), warnings, ctx));
            txt->vAlign   = uiTextVAlignFromStr(readString(t, "vAlign", std::string(), warnings, ctx), UiTextVAlign::Top);
            txt->overflow = uiTextOverflowFromStr(readString(t, "overflow", std::string(), warnings, ctx));
            txt->wordWrap = readBool(t, "wordWrap", false, warnings, ctx);

            txt->boldStrength = readFloat(t, "boldStrength", 0.08f, warnings, ctx);
            txt->italicSkew   = readFloat(t, "italicSkew", 0.25f, warnings, ctx);
            node->setText(std::move(txt));
        }
        // Additive block, same rule as the Button and the Text: a scene
        // saved before the ProgressBar component does not bring the key and loads
        // without it or a warning.
        if (j.contains("progressBar"))
        {
            const auto& p = j["progressBar"];
            const std::string ctx = "progressBar of '" + node->name + "'";
            auto bar = std::make_shared<ProgressBarComponent>();
            bar->anchorMin = readVec2XY(p, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            bar->anchorMax = readVec2XY(p, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            bar->pivot     = readVec2XY(p, "pivot", glm::vec2(0.0f), warnings, ctx);
            bar->position  = readVec2XY(p, "position", glm::vec2(0.0f), warnings, ctx);
            bar->size      = readVec2XY(p, "size", glm::vec2(160.0f, 20.0f), warnings, ctx);
            bar->color     = readVec4XYZW(p, "color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f),
                                          warnings, ctx);
            bar->visible   = readBool(p, "visible", true, warnings, ctx);

            bar->value    = readFloat(p, "value", 0.5f, warnings, ctx);
            bar->minValue = readFloat(p, "minValue", 0.0f, warnings, ctx);
            bar->maxValue = readFloat(p, "maxValue", 1.0f, warnings, ctx);

            bar->fillColor = readVec4XYZW(p, "fillColor", glm::vec4(0.25f, 0.7f, 1.0f, 1.0f),
                                          warnings, ctx);
            bar->fillDirection = uiProgressFillDirectionFromStr(readString(p, "fillDirection", std::string(), warnings, ctx));

            bar->atlasPath      = readString(p, "atlasPath", std::string(), warnings, ctx);
            bar->backgroundPath = readString(p, "backgroundPath", std::string(), warnings, ctx);
            bar->fillPath       = readString(p, "fillPath", std::string(), warnings, ctx);
            node->setProgressBar(std::move(bar));
        }
        // Additive block, same rule as the other UI components: a
        // scene saved before the Panel component does not bring the key and loads
        // without it or a warning.
        if (j.contains("panel"))
        {
            const auto& p = j["panel"];
            const std::string ctx = "panel of '" + node->name + "'";
            auto panel = std::make_shared<PanelComponent>();
            panel->anchorMin = readVec2XY(p, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            panel->anchorMax = readVec2XY(p, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            panel->pivot     = readVec2XY(p, "pivot", glm::vec2(0.0f), warnings, ctx);
            panel->position  = readVec2XY(p, "position", glm::vec2(0.0f), warnings, ctx);
            panel->size      = readVec2XY(p, "size", glm::vec2(200.0f, 120.0f), warnings, ctx);
            panel->color     = readVec4XYZW(p, "color", glm::vec4(1.0f), warnings, ctx);
            panel->visible   = readBool(p, "visible", true, warnings, ctx);

            panel->raycastTarget = readBool(p, "raycastTarget", true, warnings, ctx);

            panel->atlasPath = readString(p, "atlasPath", std::string(), warnings, ctx);
            panel->sprite    = readString(p, "sprite", std::string(), warnings, ctx);
            node->setPanel(std::move(panel));
        }
        if (j.contains("image"))
        {
            const auto& im = j["image"];
            const std::string ctx = "image of '" + node->name + "'";
            auto img = std::make_shared<ImageComponent>();
            img->anchorMin = readVec2XY(im, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            img->anchorMax = readVec2XY(im, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            img->pivot     = readVec2XY(im, "pivot", glm::vec2(0.0f), warnings, ctx);
            img->position  = readVec2XY(im, "position", glm::vec2(0.0f), warnings, ctx);
            img->size      = readVec2XY(im, "size", glm::vec2(100.0f), warnings, ctx);
            img->color     = readVec4XYZW(im, "color", glm::vec4(1.0f), warnings, ctx);
            img->visible   = readBool(im, "visible", true, warnings, ctx);

            img->raycastTarget = readBool(im, "raycastTarget", true, warnings, ctx);

            img->atlasPath = readString(im, "atlasPath", std::string(), warnings, ctx);
            img->sprite    = readString(im, "sprite", std::string(), warnings, ctx);

            img->mode = uiImageModeFromStr(readString(im, "mode", std::string(), warnings, ctx));

            img->borderLeft   = readFloat(im, "borderLeft", 0.0f, warnings, ctx);
            img->borderRight  = readFloat(im, "borderRight", 0.0f, warnings, ctx);
            img->borderTop    = readFloat(im, "borderTop", 0.0f, warnings, ctx);
            img->borderBottom = readFloat(im, "borderBottom", 0.0f, warnings, ctx);
            img->fillCenter   = readBool(im, "fillCenter", true, warnings, ctx);

            // No negatives or an absurd cap: maxTiles bounds the quads the
            // batcher can emit, and a hand-edited JSON with -1 would wrap
            // around the uint32 and with it a blown-up vertex buffer.
            const float tiles = readFloat(im, "maxTiles", 1024.0f, warnings, ctx);
            img->maxTiles = tiles > 0.0f ? (uint32_t)tiles : 0u;

            img->fillDirection = uiFillDirectionFromStr(readString(im, "fillDirection", std::string(), warnings, ctx));
            img->fillOrigin    = uiFillOriginFromStr(readString(im, "fillOrigin", std::string(), warnings, ctx));
            img->fillAmount    = readFloat(im, "fillAmount", 1.0f, warnings, ctx);
            node->setImage(std::move(img));
        }
        // Additive block, same rule as the other UI components: a
        // scene saved before the Slider component does not bring the key and
        // loads without it or a warning.
        if (j.contains("slider"))
        {
            const auto& sl = j["slider"];
            const std::string ctx = "slider of '" + node->name + "'";
            auto slider = std::make_shared<SliderComponent>();
            slider->anchorMin = readVec2XY(sl, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            slider->anchorMax = readVec2XY(sl, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            slider->pivot     = readVec2XY(sl, "pivot", glm::vec2(0.0f), warnings, ctx);
            slider->position  = readVec2XY(sl, "position", glm::vec2(0.0f), warnings, ctx);
            slider->size      = readVec2XY(sl, "size", glm::vec2(200.0f, 20.0f), warnings, ctx);
            slider->color     = readVec4XYZW(sl, "color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f),
                                             warnings, ctx);
            slider->visible      = readBool(sl, "visible", true, warnings, ctx);
            slider->interactable = readBool(sl, "interactable", true, warnings, ctx);

            slider->value    = readFloat(sl, "value", 0.0f, warnings, ctx);
            slider->minValue = readFloat(sl, "minValue", 0.0f, warnings, ctx);
            slider->maxValue = readFloat(sl, "maxValue", 1.0f, warnings, ctx);
            slider->wholeNumbers = readBool(sl, "wholeNumbers", false, warnings, ctx);

            slider->direction = uiSliderDirectionFromStr(readString(sl, "direction", std::string(), warnings, ctx));

            slider->fillColor   = readVec4XYZW(sl, "fillColor", glm::vec4(0.25f, 0.7f, 1.0f, 1.0f),
                                               warnings, ctx);
            slider->handleColor = readVec4XYZW(sl, "handleColor", glm::vec4(1.0f), warnings, ctx);
            slider->handleSize  = readFloat(sl, "handleSize", 20.0f, warnings, ctx);

            slider->atlasPath        = readString(sl, "atlasPath", std::string(), warnings, ctx);
            slider->backgroundSprite = readString(sl, "backgroundSprite", std::string(), warnings, ctx);
            slider->fillSprite       = readString(sl, "fillSprite", std::string(), warnings, ctx);
            slider->handleSprite     = readString(sl, "handleSprite", std::string(), warnings, ctx);
            node->setSlider(std::move(slider));
        }
        // Additive block, same rule as the other UI components.
        if (j.contains("checkbox"))
        {
            const auto& cb = j["checkbox"];
            const std::string ctx = "checkbox of '" + node->name + "'";
            auto chk = std::make_shared<CheckboxComponent>();
            chk->anchorMin = readVec2XY(cb, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            chk->anchorMax = readVec2XY(cb, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            chk->pivot     = readVec2XY(cb, "pivot", glm::vec2(0.0f), warnings, ctx);
            chk->position  = readVec2XY(cb, "position", glm::vec2(0.0f), warnings, ctx);
            chk->size      = readVec2XY(cb, "size", glm::vec2(24.0f), warnings, ctx);
            chk->color     = readVec4XYZW(cb, "color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f),
                                          warnings, ctx);
            chk->visible      = readBool(cb, "visible", true, warnings, ctx);
            chk->interactable = readBool(cb, "interactable", true, warnings, ctx);
            chk->isOn         = readBool(cb, "isOn", false, warnings, ctx);

            chk->checkColor   = readVec4XYZW(cb, "checkColor", glm::vec4(1.0f), warnings, ctx);
            chk->checkPadding = readFloat(cb, "checkPadding", 4.0f, warnings, ctx);

            chk->atlasPath        = readString(cb, "atlasPath", std::string(), warnings, ctx);
            chk->backgroundSprite = readString(cb, "backgroundSprite", std::string(), warnings, ctx);
            chk->checkmarkSprite  = readString(cb, "checkmarkSprite", std::string(), warnings, ctx);
            node->setCheckbox(std::move(chk));
        }
        if (j.contains("toggle"))
        {
            const auto& tg = j["toggle"];
            const std::string ctx = "toggle of '" + node->name + "'";
            auto tog = std::make_shared<ToggleComponent>();
            tog->anchorMin = readVec2XY(tg, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            tog->anchorMax = readVec2XY(tg, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            tog->pivot     = readVec2XY(tg, "pivot", glm::vec2(0.0f), warnings, ctx);
            tog->position  = readVec2XY(tg, "position", glm::vec2(0.0f), warnings, ctx);
            tog->size      = readVec2XY(tg, "size", glm::vec2(56.0f, 28.0f), warnings, ctx);
            tog->visible      = readBool(tg, "visible", true, warnings, ctx);
            tog->interactable = readBool(tg, "interactable", true, warnings, ctx);
            tog->isOn         = readBool(tg, "isOn", false, warnings, ctx);

            tog->offColor  = readVec4XYZW(tg, "offColor", glm::vec4(0.3f, 0.3f, 0.3f, 1.0f),
                                          warnings, ctx);
            tog->onColor   = readVec4XYZW(tg, "onColor", glm::vec4(0.25f, 0.7f, 1.0f, 1.0f),
                                          warnings, ctx);
            tog->knobColor = readVec4XYZW(tg, "knobColor", glm::vec4(1.0f), warnings, ctx);

            tog->knobSize    = readFloat(tg, "knobSize", 20.0f, warnings, ctx);
            tog->knobPadding = readFloat(tg, "knobPadding", 4.0f, warnings, ctx);

            tog->atlasPath        = readString(tg, "atlasPath", std::string(), warnings, ctx);
            tog->backgroundSprite = readString(tg, "backgroundSprite", std::string(), warnings, ctx);
            tog->knobSprite       = readString(tg, "knobSprite", std::string(), warnings, ctx);
            node->setToggle(std::move(tog));
        }
        if (j.contains("scrollbar"))
        {
            const auto& sb = j["scrollbar"];
            const std::string ctx = "scrollbar of '" + node->name + "'";
            auto scr = std::make_shared<ScrollbarComponent>();
            scr->anchorMin = readVec2XY(sb, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            scr->anchorMax = readVec2XY(sb, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            scr->pivot     = readVec2XY(sb, "pivot", glm::vec2(0.0f), warnings, ctx);
            scr->position  = readVec2XY(sb, "position", glm::vec2(0.0f), warnings, ctx);
            scr->size      = readVec2XY(sb, "size", glm::vec2(20.0f, 200.0f), warnings, ctx);
            scr->color     = readVec4XYZW(sb, "color", glm::vec4(0.15f, 0.15f, 0.15f, 1.0f),
                                          warnings, ctx);
            scr->visible      = readBool(sb, "visible", true, warnings, ctx);
            scr->interactable = readBool(sb, "interactable", true, warnings, ctx);

            scr->value          = readFloat(sb, "value", 0.0f, warnings, ctx);
            scr->handleFraction = readFloat(sb, "handleFraction", 0.25f, warnings, ctx);
            scr->direction      = uiScrollbarDirectionFromStr(readString(sb, "direction", std::string(), warnings, ctx));

            // No negatives: numberOfSteps is a counter, and a hand-edited JSON with
            // -1 would wrap around the uint32.
            const float pasos = readFloat(sb, "numberOfSteps", 0.0f, warnings, ctx);
            scr->numberOfSteps = pasos > 0.0f ? (uint32_t)pasos : 0u;

            scr->handleColor = readVec4XYZW(sb, "handleColor", glm::vec4(0.6f, 0.6f, 0.6f, 1.0f),
                                            warnings, ctx);
            scr->scrollStep  = readFloat(sb, "scrollStep", 0.1f, warnings, ctx);

            scr->atlasPath        = readString(sb, "atlasPath", std::string(), warnings, ctx);
            scr->backgroundSprite = readString(sb, "backgroundSprite", std::string(), warnings, ctx);
            scr->handleSprite     = readString(sb, "handleSprite", std::string(), warnings, ctx);
            node->setScrollbar(std::move(scr));
        }
        // Additive block, same rule as the other UI components.
        if (j.contains("inputField"))
        {
            const auto& fj = j["inputField"];
            const std::string ctx = "inputField of '" + node->name + "'";
            auto f = std::make_shared<InputFieldComponent>();
            f->anchorMin = readVec2XY(fj, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            f->anchorMax = readVec2XY(fj, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            f->pivot     = readVec2XY(fj, "pivot", glm::vec2(0.0f), warnings, ctx);
            f->position  = readVec2XY(fj, "position", glm::vec2(0.0f), warnings, ctx);
            f->size      = readVec2XY(fj, "size", glm::vec2(200.0f, 32.0f), warnings, ctx);
            f->color     = readVec4XYZW(fj, "color", glm::vec4(0.15f, 0.15f, 0.15f, 1.0f),
                                        warnings, ctx);
            f->visible      = readBool(fj, "visible", true, warnings, ctx);
            f->interactable = readBool(fj, "interactable", true, warnings, ctx);
            f->readOnly     = readBool(fj, "readOnly", false, warnings, ctx);

            f->text        = readString(fj, "text", std::string(), warnings, ctx);
            f->placeholder = readString(fj, "placeholder", std::string(), warnings, ctx);
            f->fontPath    = readString(fj, "fontPath", std::string(), warnings, ctx);
            f->fontSize    = readFloat(fj, "fontSize", 16.0f, warnings, ctx);
            f->textColor   = readVec4XYZW(fj, "textColor", glm::vec4(1.0f), warnings, ctx);
            f->placeholderColor = readVec4XYZW(fj, "placeholderColor",
                                               glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), warnings, ctx);
            f->align   = uiTextAlignFromStr(readString(fj, "align", std::string(), warnings, ctx));
            f->padding = readFloat(fj, "padding", 6.0f, warnings, ctx);

            // No negatives: it is a counter, and a hand-edited JSON with -1
            // would wrap around the uint32 and with it an absurd limit.
            const float lim = readFloat(fj, "characterLimit", 0.0f, warnings, ctx);
            f->characterLimit = lim > 0.0f ? (uint32_t)lim : 0u;

            f->contentType  = uiInputContentTypeFromStr(readString(fj, "contentType", std::string(), warnings, ctx));
            // Empty in the JSON is respected: displayText already falls to the asterisk.
            f->passwordChar = (fj.contains("passwordChar") && fj["passwordChar"].is_string())
                                  ? fj["passwordChar"].get<std::string>() : std::string("*");

            f->caretColor     = readVec4XYZW(fj, "caretColor", glm::vec4(1.0f), warnings, ctx);
            f->caretWidth     = readFloat(fj, "caretWidth", 1.0f, warnings, ctx);
            f->caretBlinkRate = readFloat(fj, "caretBlinkRate", 0.5f, warnings, ctx);

            f->atlasPath        = readString(fj, "atlasPath", std::string(), warnings, ctx);
            f->backgroundSprite = readString(fj, "backgroundSprite", std::string(), warnings, ctx);
            // The cursor starts at the end of the loaded text, which is where
            // anyone who clicks on an already filled field expects it.
            f->caretEnd();
            node->setInputField(std::move(f));
        }
        if (j.contains("dropdown"))
        {
            const auto& dj = j["dropdown"];
            const std::string ctx = "dropdown of '" + node->name + "'";
            auto d = std::make_shared<DropdownComponent>();
            d->anchorMin = readVec2XY(dj, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            d->anchorMax = readVec2XY(dj, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            d->pivot     = readVec2XY(dj, "pivot", glm::vec2(0.0f), warnings, ctx);
            d->position  = readVec2XY(dj, "position", glm::vec2(0.0f), warnings, ctx);
            d->size      = readVec2XY(dj, "size", glm::vec2(200.0f, 32.0f), warnings, ctx);
            d->color     = readVec4XYZW(dj, "color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f),
                                        warnings, ctx);
            d->visible      = readBool(dj, "visible", true, warnings, ctx);
            d->interactable = readBool(dj, "interactable", true, warnings, ctx);

            // Options that are not strings are DISCARDED one by one instead of
            // throwing away the whole list: losing a combo over a corrupt entry
            // would be worse than losing that entry.
            if (dj.contains("options") && dj["options"].is_array())
                for (const auto& o : dj["options"])
                    if (o.is_string()) d->options.push_back(o.get<std::string>());

            d->value = (dj.contains("value") && dj["value"].is_number_integer())
                           ? dj["value"].get<int>() : 0;

            d->itemHeight = readFloat(dj, "itemHeight", 24.0f, warnings, ctx);
            const float vis = readFloat(dj, "maxVisibleItems", 6.0f, warnings, ctx);
            d->maxVisibleItems = vis > 0.0f ? (uint32_t)vis : 0u;

            d->listColor         = readVec4XYZW(dj, "listColor", glm::vec4(0.12f, 0.12f, 0.12f, 1.0f), warnings, ctx);
            d->itemColor         = readVec4XYZW(dj, "itemColor", glm::vec4(0.18f, 0.18f, 0.18f, 1.0f), warnings, ctx);
            d->itemSelectedColor = readVec4XYZW(dj, "itemSelectedColor", glm::vec4(0.25f, 0.45f, 0.7f, 1.0f), warnings, ctx);
            d->arrowColor        = readVec4XYZW(dj, "arrowColor", glm::vec4(1.0f), warnings, ctx);

            d->fontPath  = readString(dj, "fontPath", std::string(), warnings, ctx);
            d->fontSize  = readFloat(dj, "fontSize", 16.0f, warnings, ctx);
            d->textColor = readVec4XYZW(dj, "textColor", glm::vec4(1.0f), warnings, ctx);
            d->padding   = readFloat(dj, "padding", 6.0f, warnings, ctx);

            d->atlasPath        = readString(dj, "atlasPath", std::string(), warnings, ctx);
            d->backgroundSprite = readString(dj, "backgroundSprite", std::string(), warnings, ctx);
            d->arrowSprite      = readString(dj, "arrowSprite", std::string(), warnings, ctx);
            d->itemSprite       = readString(dj, "itemSprite", std::string(), warnings, ctx);
            node->setDropdown(std::move(d));
        }
        if (j.contains("scrollView"))
        {
            const auto& vj = j["scrollView"];
            const std::string ctx = "scrollView of '" + node->name + "'";
            auto v = std::make_shared<ScrollViewComponent>();
            v->anchorMin = readVec2XY(vj, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            v->anchorMax = readVec2XY(vj, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            v->pivot     = readVec2XY(vj, "pivot", glm::vec2(0.0f), warnings, ctx);
            v->position  = readVec2XY(vj, "position", glm::vec2(0.0f), warnings, ctx);
            v->size      = readVec2XY(vj, "size", glm::vec2(200.0f), warnings, ctx);
            v->color     = readVec4XYZW(vj, "color", glm::vec4(0.1f, 0.1f, 0.1f, 1.0f),
                                        warnings, ctx);
            v->visible    = readBool(vj, "visible", true, warnings, ctx);
            v->horizontal = readBool(vj, "horizontal", false, warnings, ctx);
            v->vertical   = readBool(vj, "vertical", true, warnings, ctx);

            v->contentSize        = readVec2XY(vj, "contentSize", glm::vec2(200.0f, 400.0f), warnings, ctx);
            v->normalizedPosition = readVec2XY(vj, "normalizedPosition", glm::vec2(0.0f), warnings, ctx);
            v->scrollSensitivity  = readFloat(vj, "scrollSensitivity", 40.0f, warnings, ctx);

            v->atlasPath        = readString(vj, "atlasPath", std::string(), warnings, ctx);
            v->backgroundSprite = readString(vj, "backgroundSprite", std::string(), warnings, ctx);
            node->setScrollView(std::move(v));
        }
        if (j.contains("layout"))
        {
            const auto& l = j["layout"];
            const std::string ctx = "layout of '" + node->name + "'";
            auto layout = std::make_shared<LayoutComponent>();
            layout->anchorMin = readVec2XY(l, "anchorMin", glm::vec2(0.0f), warnings, ctx);
            layout->anchorMax = readVec2XY(l, "anchorMax", glm::vec2(0.0f), warnings, ctx);
            layout->pivot     = readVec2XY(l, "pivot", glm::vec2(0.0f), warnings, ctx);
            layout->position  = readVec2XY(l, "position", glm::vec2(0.0f), warnings, ctx);
            layout->size      = readVec2XY(l, "size", glm::vec2(200.0f, 200.0f), warnings, ctx);
            layout->visible   = readBool(l, "visible", true, warnings, ctx);

            layout->mode = uiLayoutModeFromStr(readString(l, "mode", std::string(), warnings, ctx));

            layout->paddingLeft   = readFloat(l, "paddingLeft", 0.0f, warnings, ctx);
            layout->paddingRight  = readFloat(l, "paddingRight", 0.0f, warnings, ctx);
            layout->paddingTop    = readFloat(l, "paddingTop", 0.0f, warnings, ctx);
            layout->paddingBottom = readFloat(l, "paddingBottom", 0.0f, warnings, ctx);

            layout->spacing  = readVec2XY(l, "spacing", glm::vec2(0.0f), warnings, ctx);
            layout->cellSize = readVec2XY(l, "cellSize", glm::vec2(100.0f), warnings, ctx);
            // No negatives: columns is the number of grid columns, and a
            // hand-edited JSON with -1 would wrap around the uint32.
            const float cols = readFloat(l, "columns", 0.0f, warnings, ctx);
            layout->columns = cols > 0.0f ? (uint32_t)cols : 0u;

            layout->crossAlign = uiCrossAlignFromStr(readString(l, "crossAlign", std::string(), warnings, ctx));

            layout->fitWidth  = readBool(l, "fitWidth", false, warnings, ctx);
            layout->fitHeight = readBool(l, "fitHeight", false, warnings, ctx);

            layout->ignoreLayout = readBool(l, "ignoreLayout", false, warnings, ctx);
            layout->clipChildren = readBool(l, "clipChildren", false, warnings, ctx);
            node->setLayout(std::move(layout));
        }
        // Additive block: scenes saved before this field do not bring it
        // and load the same (version stays at 1).
        if (j.contains("animator"))
        {
            auto anim = animatorFromJson(j["animator"], warnings);
            // The "mesh" block is parsed BEFORE this one, so the SkinnedMesh
            // is already assembled and bindClips can resolve the clip names
            // right here. Without a skinned mesh (orphan graph) the clipIndex
            // stay at -1 and currentClipIndex falls to 0.
            if (auto* sm = node->getSkinnedMesh())
                anim->bindClips(*sm, warnings);
            // The property tracks are resolved against the OBJECT (which
            // components it has), not against the mesh: a graph without a skeleton
            // also has to end up resolved.
            anim->bindProperties(node, warnings);
            node->setAnimator(std::move(anim));
        }
        if (j.contains("audioClip"))
        {
            const auto& c = j["audioClip"];
            const std::string ctx = "audioClip of '" + node->name + "'";
            // path/is3D/loop are ALWAYS written by nodeToJson, so required
            // = true: if they are missing it is not back-compat, it is corruption and it has to
            // be named. Before they were read with .at(): an audioClip missing
            // any of the three threw json::exception, the
            // exception went up to the catch of fromJson and the load of the
            // WHOLE scene was lost over one field — exactly the opposite of the
            // criterion the rest of this file follows.
            std::string path = readString(c, "path", "", warnings, ctx, /*required=*/true);
            // Same whitelist as the UI path. A hand-edited .scene (or
            // one written by a tool) could bring any extension, and
            // since FMOD loads lazily that ended up in a muted clip whose only
            // symptom was silence. Here the problem is named and the clip is
            // discarded, letting the rest of the scene load.
            if (!path.empty())
            {
                std::string ext = std::filesystem::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                if (!DonTopo::isSupportedAudioExtension(ext))
                {
                    if (warnings)
                        warnings->push_back(ctx + ".path: unsupported audio format ('" +
                                             ext + "'), the clip is discarded");
                    path.clear();
                }
            }
            // Without a path there is nothing to load: the node is left without audio and the
            // rest of the scene goes on. is3D/loop do have a reasonable default
            // (2D, no loop), which is also what the UI creates clips with.
            auto clip = path.empty()
                            ? nullptr
                            : audio.createAudioClipComponent(
                                  path,
                                  readBool(c, "is3D", false, warnings, ctx, /*required=*/true),
                                  readBool(c, "loop", false, warnings, ctx, /*required=*/true));
            if (clip)
            {
                // Without required: this field came later, and an earlier scene
                // that does not bring it is legitimate back-compat, not corruption.
                clip->setPlayOnAwake(readBool(c, "playOnAwake", false, warnings, ctx));
                // The bus also came later: absent = "sfx", which is where
                // everything went out before the buses existed, so
                // an old scene sounds the same. A name that does NOT exist does
                // warn: it is corruption or a project from a newer version,
                // and falling to sfx silently would leave a clip playing through the wrong bus
                // with no hint at all.
                const std::string busName = readString(c, "bus", "sfx", warnings, ctx);
                DonTopo::AudioBus bus = DonTopo::AudioBus::Sfx;
                if (!DonTopo::audioBusFromStr(busName, bus) && warnings)
                    warnings->push_back(ctx + ".bus: unknown value '" + busName +
                                         "', using 'sfx'");
                clip->setBus(bus);
                // Same criterion as the bus: absent = "sample" (how everything was loaded
                // before), unknown name = warning. NOTE: setLoadMode
                // RELOADS the sound, so setting it after creating the clip
                // costs one extra load; it only happens in scenes whose clip streams,
                // and it avoids the factory having to know all the
                // component fields.
                const std::string loadModeName = readString(c, "loadMode", "sample", warnings, ctx);
                DonTopo::AudioLoadMode loadMode = DonTopo::AudioLoadMode::Sample;
                if (!DonTopo::audioLoadModeFromStr(loadModeName, loadMode) && warnings)
                    warnings->push_back(ctx + ".loadMode: unknown value '" + loadModeName +
                                         "', using 'sample'");
                clip->setLoadMode(loadMode);
                // Attenuation curve: same criterion as bus and loadMode.
                const std::string rolloffName = readString(c, "rolloff", "inverse", warnings, ctx);
                DonTopo::AudioRolloff rolloff = DonTopo::AudioRolloff::Inverse;
                if (!DonTopo::audioRolloffFromStr(rolloffName, rolloff) && warnings)
                    warnings->push_back(ctx + ".rolloff: unknown value '" + rolloffName +
                                         "', using 'inverse'");
                clip->setRolloff(rolloff);
                // The three voice ones. Neutral defaults: a scene prior to
                // this feature sounds exactly the same as before.
                clip->setSpread(readFloat(c, "spread", 0.0f, warnings, ctx));
                clip->setStereoPan(readFloat(c, "stereoPan", 0.0f, warnings, ctx));
                clip->setDopplerLevel(readFloat(c, "dopplerLevel", 0.0f, warnings, ctx));
                clip->setMute(readBool(c, "mute", false, warnings, ctx));
                // Same criterion. readFloat also tolerates a "null" (serialized
                // NaN, see the comment block next to jsonToMat4):
                // before, that null made the whole fromJson fail.
                clip->setVolume(readFloat(c, "volume", 1.0f, warnings, ctx));
                clip->setPitch(readFloat(c, "pitch", 1.0f, warnings, ctx));
                // Same compat criterion: component defaults for
                // scenes prior to these two fields. Max before min: the
                // two setters keep min <= max between them, so the
                // second drags the first and the pair always ends up valid.
                clip->setMaxDistance(readFloat(c, "maxDistance", 100.0f, warnings, ctx));
                clip->setMinDistance(readFloat(c, "minDistance", 1.0f, warnings, ctx));
                node->setAudioClip(std::move(clip));
            }
            // clip nullptr (broken asset/unsupported format): node is left without
            // audio, the rest of the scene keeps loading.
        }
        // Additive block: scenes saved before this field do not bring it
        // and load the same (version stays at 1). The one-per-scene invariant
        // is NOT enforced here (nodeFromJson does not see the whole tree): it is done by
        // pruneExtraAudioListeners at the end of fromJson.
        if (j.contains("reverbZone"))
        {
            const auto& z = j["reverbZone"];
            const std::string zctx = "reverbZone of '" + node->name + "'";
            auto zone = std::make_shared<DonTopo::ReverbZoneComponent>();
            // The preset is validated against the real list: an unknown one warns and
            // falls to "room" instead of installing an arbitrary ambience.
            const std::string preset = readString(z, "preset", "room", warnings, zctx);
            const auto& known = DonTopo::AudioManager::reverbPresetNames();
            if (std::find(known.begin(), known.end(), preset) == known.end())
            {
                if (warnings)
                    warnings->push_back(zctx + ".preset: unknown '" + preset +
                                         "', using 'room'");
                zone->setPreset("room");
            }
            else
            {
                zone->setPreset(preset);
            }
            // Max before min, as in the AudioClip: the two setters keep
            // the invariant between them.
            zone->setMaxDistance(readFloat(z, "maxDistance", 200.0f, warnings, zctx));
            zone->setMinDistance(readFloat(z, "minDistance", 50.0f, warnings, zctx));
            zone->setEnabled(readBool(z, "enabled", true, warnings, zctx));
            node->setReverbZone(std::move(zone));
        }
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
                // Without a name there is no .lua file to load, so that
                // component is discarded — but the GameObject and the rest of the
                // scene go on. Before it was an .at() and the whole load was lost.
                const std::string scriptName =
                    readString(sj, "name", std::string(), warnings,
                                "scripts of '" + node->name + "'", /*required=*/true);
                if (scriptName.empty())
                    continue;   // readString already warned
                auto comp = std::make_unique<DonTopo::ScriptComponent>(scriptName, node);
                if (sj.contains("overrides"))
                {
                    for (const auto& [key, val] : sj["overrides"].items())
                    {
                        if (val.is_boolean())     comp->overrides[key] = val.get<bool>();
                        else if (val.is_string()) comp->overrides[key] = val.get<std::string>();
                        else if (val.is_number()) comp->overrides[key] = val.get<double>();
                        // Other types: ignored (they are not serializable props)
                    }
                }
                // Note: if the script no longer exists in Scripts/, the component
                // is kept all the same ("missing script", spec) — the UI
                // flags it; the overrides are not lost on re-saving.
                node->addScript(std::move(comp));
            }
        }

        // "children" is ALWAYS written by nodeToJson (see the end of that
        // function), so its absence is never back-compat: it is corruption, and
        // until now an .at() turned it into "the whole scene is lost".
        // Now the node is loaded without children and the problem is named.
        auto childrenIt = j.find("children");
        if (childrenIt == j.end() || !childrenIt->is_array())
        {
            if (warnings)
                warnings->push_back("node '" + node->name + "'.children: missing or not a list "
                                     "in the scene, the object loads without children");
            return;
        }
        for (const auto& childJson : *childrenIt)
        {
            if (!childJson.is_object())
            {
                if (warnings)
                    warnings->push_back("node '" + node->name + "'.children: there is an entry that is not "
                                         "an object, discarded");
                continue;
            }
            // The name is also always written by nodeToJson. A node without it is
            // kept all the same (it may have half a tree hanging from it) but with an
            // empty name and its warning, instead of taking down the load.
            GameObject* child = node->addChild(
                readString(childJson, "name", std::string(), warnings,
                            "node '" + node->name + "'.children", /*required=*/true));
            // The child node is loaded INSIDE a try. Reason: the ~52 `.value(...)`
            // left in the component blocks throw `json::type_error`
            // if the key EXISTS with the wrong type (checked: string,
            // bool, int and float throw; the `.value` whose default is a `json`
            // DO NOT —any type converts to json—, which is the half of row
            // H2 that turned out not to be true). Without this guard, a single corrupt
            // field went up to the catch of fromJson and the WHOLE scene was lost,
            // without saying which node it came from.
            //
            // It costs the subtree of THAT node, not the scene: the child stays
            // created and with its name —it was already added above— and the traversal goes on
            // with its siblings. And it is named, which was what was missing: the nlohmann
            // message says the expected and the found type, and this warning
            // gives the node.
            //
            // It is caught here and not inside each component block because
            // here it covers the 32 there are AND those that get added: wrapping each one
            // would be the usual hand-written list.
            try
            {
                nodeFromJson(childJson, child, node->worldTransform, physics, audio, warnings, loadCache, assetRoot, loader, preloaded, carryOverrideBaseline);
            }
            catch (const nlohmann::json::exception& e)
            {
                if (warnings)
                    warnings->push_back("node '" + child->name + "': a field has a type it should not "
                                         "have (" + std::string(e.what()) + "). The node is left without "
                                         "its components and its children; the rest of the scene "
                                         "loads anyway");
            }
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

        // BEFORE the erase, not after: the listener traverses the subtree to
        // release its GPU slots, and for that it has to still exist.
        // It goes after the guard above on purpose — what is not deleted is not
        // announced, or the Renderer would release the slots of a live scene.
        if (m_onNodeRemoved) m_onNodeRemoved(node);

        auto& siblings = node->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [node](const std::unique_ptr<GameObject>& c) { return c.get() == node; }),
            siblings.end());
    }

    bool Scene::reparent(GameObject* node, GameObject* newParent, size_t index)
    {
        // The root hangs from nobody: without a parent there is no list to take it out of.
        if (!node || !node->parent) return false;

        GameObject* target = newParent ? newParent : &m_root;
        if (target == node) return false;

        // Cycle: dropping a node inside its own subtree would detach that
        // subtree from the tree, and with it the unique_ptr that keeps it alive.
        bool cycle = false;
        node->traverse([&](GameObject* go) { if (go == target) cycle = true; });
        if (cycle) return false;

        auto& oldSiblings = node->parent->children;
        auto it = std::find_if(oldSiblings.begin(), oldSiblings.end(),
            [node](const std::unique_ptr<GameObject>& c) { return c.get() == node; });
        // The node says it has a parent but is not in its list: incoherent tree,
        // better not to touch it.
        if (it == oldSiblings.end()) return false;

        // The unique_ptr is taken out BEFORE computing the gap: index is interpreted
        // over the destination list already without the node, so the same index works
        // wherever the node is (and the editor Undo can store the final
        // index as is).
        std::unique_ptr<GameObject> moved = std::move(*it);
        oldSiblings.erase(it);

        moved->parent = target;
        auto& newSiblings = target->children;
        const size_t clamped = std::min(index, newSiblings.size());
        newSiblings.insert(newSiblings.begin() + static_cast<ptrdiff_t>(clamped),
                           std::move(moved));
        return true;
    }

    GameObject* Scene::cloneGameObject(GameObject* src, GameObject* parent,
                                       PhysicsManager& physics, AudioManager& audio)
    {
        if (!src || src == &m_root) return nullptr;

        GameObject* target = parent ? parent : (src->parent ? src->parent : &m_root);
        // EMPTY root on purpose, not m_assetRoot: this JSON does not touch disk, it goes
        // straight from nodeToJson to nodeFromJson a few lines below. With the
        // real root the absolute->relative->absolute trip would be wasted work
        // (and a call to filesystem::relative, which DOES touch disk, per
        // override) — and this path is the one used by Lua's Scene.Instantiate IN
        // PLAY, already optimized on purpose not to go to disk per spawn (see the
        // PreloadedMeshCache comment below, 24.5 ms/clone measured).
        //
        // carryOverrideBaseline = true: the clone's mesh is seeded below
        // from `mallas`, the PreloadedMeshCache with the LIVE mesh of
        // src — which, if it has overrides, already carries them baked into the material.
        // Without the real baseline traveling in this JSON, applyMaterialOverrides
        // would capture that already overwritten texture as the "original", and a Clear on
        // the clone would not return the FBX one. See the big comment of
        // nodeToJson next to "baseAlbedo".
        nlohmann::json j = nodeToJson(*src, std::string(), /*carryOverrideBaseline=*/true);

        // Remove the "id" from the serialized tree, so that addChild/GameObject
        // keep their freshly generated ones.
        //
        // nodeFromJson reuses the id that comes in the JSON, and rightly so: it is what
        // lets the Undo of a Delete rebuild the GameObject with its original id
        // and the commands left in the stack keep resolving it.
        // But when cloning the ORIGINAL IS STILL ALIVE, so reusing it left two
        // nodes with the same id; findById returns the last of the traversal —the
        // clone—, and any undo command resolved by id ended up
        // writing into the wrong object.
        std::function<void(nlohmann::json&)> stripIds = [&](nlohmann::json& node) {
            node.erase("id");
            if (auto it = node.find("children"); it != node.end() && it->is_array())
                for (nlohmann::json& child : *it)
                    stripIds(child);
        };
        stripIds(j);

        GameObject* clone = target->addChild(src->name + " (Clone)");
        // Before nodeFromJson: if it were cleared after (as it was), the
        // warnings that bindClips pushes to m_warnings during the load would
        // be lost immediately.
        m_warnings.clear();
        // Cache seeded with the AUTHORITATIVE answer: the source object is already
        // in memory, so isSkinned() is free and cannot lie.
        // Without this each clone probed the FBX again with Assimp (and then parsed
        // it entirely again), two synchronous file reads per
        // spawn inside the Play loop — its only caller is Lua's Scene.Instantiate.
        // Worse than the cost: reading the disk lets a clone taken
        // while the artist re-exports the FBX come back with a different mesh type
        // from the object it was cloned from. If the clone is a subtree,
        // the cache also dedups among all its nodes.
        //
        // The MESHES go by the same reasoning and the same path: the source
        // object already has them in memory, so nodeFromJson deep-copies them
        // instead of reparsing the file. Without this, the bone probe
        // came from the cache but the whole parse still went to disk —
        // measured in Release, 24.5 ms per clone of a rigged character, that is
        // a frame and a half at 60 fps per each Instantiate.
        //
        // The SUBTREE is traversed, not just the root: a character usually brings its
        // meshes hanging, and seeding only src left the children going to
        // disk all the same. It holds for both caches, which are filled in the same
        // pass.
        // By the REAL sourcePath of the mesh (m->sourcePath), not by the
        // meshes key: since collectMeshes indexes by meshCacheKey (piece
        // included), the key of a piece != 0 carries "#piece=N" and no longer
        // matches the plain sourcePath that hasBonesCache looks up further
        // below — without this, each piece != 0 of a cloned static model
        // probed the file again with Assimp instead of using the cache.
        NodeLoadCache cache;
        const PreloadedMeshCache mallas = collectMeshes(src);
        for (const auto& [ruta, m] : mallas)
            cache.hasBones[m->sourcePath] = dynamic_cast<const SkinnedMesh*>(m.get()) != nullptr;
        try
        {
            // Empty root, counterpart of the one above: j was serialized with an empty root
            // (verbatim paths), so it is read the same way — without fromStoredPath
            // touching the filesystem or rewriting separators in memory for
            // each clone.
            nodeFromJson(j, clone, target->worldTransform, physics, audio, &m_warnings, &cache, std::string(),
                         /*loader=*/nullptr, &mallas, /*carryOverrideBaseline=*/true);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(clone);
            return nullptr;
        }

        clone->traverse([&](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
            // The clone never takes the CameraComponent: when cloning, the original
            // is still alive with its camera, so findCamera() is already non-null and the
            // clone would break the invariant. Deterministic, not conditional. Its
            // only caller is Lua's Instantiate (ScriptBindings.cpp), which runs
            // in Play — no UI gate can prevent it, which is why the rule
            // lives here.
            if (n->hasCameraComponent())
            {
                n->setCameraComponent(nullptr);
                m_warnings.push_back("Clone of '" + n->name +
                                      "': the CameraComponent is discarded (there is already a camera in the scene)");
            }
        });
        collapseWarnings();
        return clone;
    }

    GameObject* Scene::findById(uint64_t id)
    {
        // First match in pre-order (the first wins, just like findCamera/
        // findAudioListener/findCanvas below), NOT the last: before it
        // kept the last visited node, so a duplicate id (which
        // should not exist — see the guard of insertFromJson — but if the
        // invariant is broken through another path nobody else checks it) silently chose
        // the object that had been in the tree the shortest time. Deterministic
        // does not fix the broken invariant, but it stops depending on the
        // insertion order to decide which one wins.
        return m_root.findFirst([id](const GameObject* n) { return n->id == id; });
    }

    GameObject* Scene::findCamera()
    {
        // findFirst is pre-order and STOPS: the first wins, and it also stops
        // descending through the rest of the tree. The `traverse` with a !found guard that
        // was here visited the 5000 nodes of a large scene for nothing, and
        // this runs per frame (resolveFrameCamera).
        return m_root.findFirst([](const GameObject* n) { return n->hasCameraComponent(); });
    }

    const GameObject* Scene::findCamera() const
    {
        // traverse is non-const (template in GameObject); the const_cast stays
        // contained here and the const version mutates nothing.
        return const_cast<Scene*>(this)->findCamera();
    }

    GameObject* Scene::findAudioListener()
    {
        return m_root.findFirst([](const GameObject* n) { return n->hasAudioListener(); });
    }

    const GameObject* Scene::findAudioListener() const
    {
        return const_cast<Scene*>(this)->findAudioListener();
    }

    GameObject* Scene::findCanvas()
    {
        return m_root.findFirst([](const GameObject* n) { return n->hasCanvas(); });
    }

    const GameObject* Scene::findCanvas() const
    {
        return const_cast<Scene*>(this)->findCanvas();
    }

    void Scene::collectCanvases(std::vector<UiCanvasBinding>& out) const
    {
        out.clear();

        // Own recursion and not traverse(): it is necessary to carry downwards
        // which binding each widget falls in and which is its ancestor with UI, and
        // traverse only gives the node.
        struct Walker
        {
            std::vector<UiCanvasBinding>& out;

            // canvasIdx: which binding the widgets of this subtree fall in (-1 =
            // none yet). uiAncestor: the ancestor with UI INSIDE that
            // same canvas, which is what the children are anchored against.
            void visit(const GameObject* node, int canvasIdx, uint64_t uiAncestor)
            {
                if (node->hasCanvas())
                {
                    // A NESTED canvas opens its own binding and CUTS the anchoring
                    // chain: whatever hangs from it is anchored to its root, not to the
                    // widget that might be above it in the outer canvas.
                    UiCanvasBinding b;
                    b.ownerId        = node->id;
                    b.canvas         = node->getCanvas().get();
                    b.worldTransform = node->worldTransform;
                    out.push_back(std::move(b));
                    canvasIdx  = (int)out.size() - 1;
                    uiAncestor = 0;
                }

                // The layout container counts as UI even if it draws nothing: it contributes a
                // rect, and without that its children would go up to the ancestor above and
                // nobody would place them.
                const bool tieneUi = node->hasButton() || node->hasText() ||
                                     node->hasProgressBar() || node->hasLayout() ||
                                     node->hasPanel() || node->hasImage() ||
                                     node->hasSlider() || node->hasCheckbox() ||
                                     node->hasToggle() || node->hasScrollbar() ||
                                     node->hasInputField() || node->hasDropdown() ||
                                     node->hasScrollView();

                // Without a canvas above, a widget goes nowhere. The
                // editor already prevents it (uiComponentsAvailable), so this only
                // happens in hand-made scenes.
                if (tieneUi && canvasIdx >= 0)
                {
                    // Re-indexed and not a stored reference: the push_back above
                    // (or another further below, in a sibling canvas visited
                    // later) may reallocate the vector, and a reference to
                    // out[i] that survived the recursion into the children
                    // would be left pointing at freed memory.
                    UiWidgetLists& w = out[(size_t)canvasIdx].widgets;
                    if (node->hasPanel())       w.panels.emplace_back(node->id, node->getPanel().get());
                    if (node->hasImage())       w.images.emplace_back(node->id, node->getImage().get());
                    if (node->hasScrollView())  w.scrollViews.emplace_back(node->id, node->getScrollView().get());
                    if (node->hasSlider())      w.sliders.emplace_back(node->id, node->getSlider().get());
                    if (node->hasInputField())  w.inputFields.emplace_back(node->id, node->getInputField().get());
                    if (node->hasDropdown())    w.dropdowns.emplace_back(node->id, node->getDropdown().get());
                    if (node->hasScrollbar())   w.scrollbars.emplace_back(node->id, node->getScrollbar().get());
                    if (node->hasToggle())      w.toggles.emplace_back(node->id, node->getToggle().get());
                    if (node->hasCheckbox())    w.checkboxes.emplace_back(node->id, node->getCheckbox().get());
                    if (node->hasButton())      w.buttons.emplace_back(node->id, node->getButton().get());
                    if (node->hasProgressBar()) w.bars.emplace_back(node->id, node->getProgressBar().get());
                    if (node->hasText())        w.texts.emplace_back(node->id, node->getText().get());
                    if (node->hasLayout())      w.layouts.emplace_back(node->id, node->getLayout().get());
                    w.parents.emplace_back(node->id, uiAncestor);
                }

                // The children hang from THIS one if it contributes a rect; if not, they keep
                // hanging from whoever contributed it further up.
                const uint64_t paraLosHijos = (tieneUi && canvasIdx >= 0) ? node->id : uiAncestor;
                for (const auto& child : node->children)
                    visit(child.get(), canvasIdx, paraLosHijos);
            }
        };

        Walker walker{out};
        // The scene root is not a widget: its children start without a canvas
        // or ancestor.
        for (const auto& child : m_root.children) walker.visit(child.get(), -1, 0ull);
    }

    void Scene::collapseWarnings()
    {
        std::vector<std::string> unicos;
        std::vector<size_t>      veces;
        // Maps message -> position in unicos. With the whole message as key:
        // two different warnings from the same object have to remain two.
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

    void Scene::pruneDuplicateIds()
    {
        // Same kind of repair as pruneExtraCameras: the FILE may come
        // broken and loading cannot propagate it. nodeFromJson reuses the id each
        // node brings —it has to, it is what lets an Undo of Delete
        // rebuild the object with its original id— but nobody checked they were not
        // repeated, so a scene saved with two nodes of the same id
        // loaded broken again and again.
        //
        // And the damage is not cosmetic: the WHOLE editor resolves by id (the gizmo
        // through applyLocalTransform, the undo commands, the panel), and findById
        // returns the first in preorder. With two nodes sharing an id,
        // dragging the second wrote its whole matrix —position, rotation and
        // SCALE— into the first. With an FBX character, the other object jumped in size
        // with nothing explaining it.
        //
        // The FIRST in preorder wins, for two reasons pointing the same
        // way: it is what findById already returns, and it is the criterion of
        // insertFromJson (the one already there keeps its own).
        std::unordered_set<uint64_t> vistos;
        m_root.traverse([&](GameObject* n) {
            if (vistos.insert(n->id).second) return;
            const uint64_t idViejo = n->id;
            n->id = GameObject::allocateId();
            vistos.insert(n->id);
            m_warnings.push_back("node '" + n->name + "': id " + std::to_string(idViejo) +
                                  " was already in use in the loaded scene; reassigned to " +
                                  std::to_string(n->id));
        });
    }

    void Scene::pruneExtraCameras()
    {
        GameObject* first = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!n->hasCameraComponent()) return;
            if (!first) { first = n; return; }
            m_warnings.push_back("Scene with more than one camera: discarding the one on '" + n->name +
                                  "' (keeping the one on '" + first->name + "')");
            n->setCameraComponent(nullptr);
        });
    }

    void Scene::pruneExtraAudioListeners()
    {
        GameObject* first = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!n->hasAudioListener()) return;
            if (!first) { first = n; return; }
            m_warnings.push_back("Scene with more than one Audio Listener: discarding the one on '" + n->name +
                                  "' (keeping the one on '" + first->name + "')");
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

            // Position and direction come from the transform, like the camera: column
            // 3 is the world position and local -Z is where it looks.
            // A scale 0 on the Z axis (the editor lets you set it from Properties)
            // would leave the direction as NaN, so there it falls to -Y instead of
            // propagating the NaN up to the shader — same criterion as the runtime
            // audio listener.
            const glm::vec3 pos     = glm::vec3(n->worldTransform[3]);
            const glm::vec3 zAxis   = glm::vec3(n->worldTransform[2]);
            const glm::vec3 forward = (glm::length(zAxis) >= 1e-6f)
                                          ? glm::normalize(-zAxis)
                                          : glm::vec3(0.0f, -1.0f, 0.0f);

            Light l{};
            l.position  = glm::vec4(pos, 1.0f);
            l.color     = glm::vec4(lc.getColor(), lc.getIntensity());
            l.direction = glm::vec4(forward, (float)(int)lc.getType());
            // The angles already travel as cosine: the shader compares against the
            // cosine of the angle with the axis, it does not call cos() again per
            // fragment.
            l.params = glm::vec4(lc.getRange(),
                                 std::cos(glm::radians(lc.getInnerAngle())),
                                 std::cos(glm::radians(lc.getOuterAngle())),
                                 lc.getAreaWidth());

            // The Forward+ binning radius has to be THE SAME range
            // the fragment shader uses, or a light would switch off abruptly when
            // crossing the edge of a tile. The area is approximated as a point of
            // radius width/2, as there; the directional is not culled by
            // radius (it enters all cells), so its one does not matter.
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
        // Empty root on purpose: this JSON is the in-memory snapshot used by
        // CreateGameObjectCommand/DeleteGameObjectCommand for Undo/Redo, it never
        // touches disk. Paired with the same empty root of insertFromJson, further
        // below — same reasoning as cloneGameObject: no round trip through
        // filesystem::relative on every undo cycle.
        //
        // carryOverrideBaseline = true: same in-memory path as
        // cloneGameObject (see its comment), so the baseline travels the same way
        // here — it is the exact snapshot of the object at the moment of the Delete, not
        // a re-derivation from the FBX.
        return nodeToJson(*node, std::string(), /*carryOverrideBaseline=*/true);
    }

    bool meshMatchesAnimationConfig(const SkinnedMesh& mesh, const nlohmann::json& animationSources)
    {
        if (!animationSources.is_array()) return false;
        if (animationSources.size() != mesh.animationSources.size()) return false;
        for (size_t i = 0; i < animationSources.size(); i++)
        {
            const nlohmann::json& sj = animationSources[i];
            const AnimationSource& src = mesh.animationSources[i];
            if (!sj.is_object()) return false;
            if (sj.value("path", std::string()) != src.path) return false;
            if (sj.value("builtin", false) != src.builtin) return false;
            if (!sj.contains("clips") || !sj["clips"].is_array()) return src.clipNames.empty();
            const nlohmann::json& cj = sj["clips"];
            if (cj.size() != src.clipNames.size()) return false;
            for (size_t c = 0; c < cj.size(); c++)
                if (!cj[c].is_string() || cj[c].get<std::string>() != src.clipNames[c]) return false;
        }
        return true;
    }

    std::string meshCacheKey(const std::string& sourcePath, int piece)
    {
        return piece == 0 ? sourcePath : sourcePath + "#piece=" + std::to_string(piece);
    }

    PreloadedMeshCache Scene::collectMeshes(GameObject* root)
    {
        PreloadedMeshCache out;
        if (!root) return out;
        root->traverse([&](GameObject* n) {
            if (!n->hasMesh()) return;
            const std::string& ruta = n->getMesh()->sourcePath;
            if (ruta.empty()) return;   // procedural: there is no file to avoid
            out[meshCacheKey(ruta, n->getMesh()->piece)] = n->getMesh();
        });
        return out;
    }

    GameObject* Scene::insertFromJson(const nlohmann::json& j, GameObject* parent, size_t index,
                                       PhysicsManager& physics, AudioManager& audio,
                                       const PreloadedMeshCache* preloaded)
    {
        GameObject* target = parent ? parent : &m_root;

        // Ids already alive in the rest of the scene, taken BEFORE inserting
        // anything: nodeFromJson reuses on purpose the id that j brings (see its
        // big comment — it is what lets an Undo of Delete
        // rebuild the object with its original id) but does not check that that
        // id is not ALREADY alive in another node. A snapshot captured by an old
        // command of the Undo/Redo stack may fall behind a
        // scene reload that handed out that same id to another, different object;
        // without this guard the tree ends up with two nodes with the same id and
        // findById silently resolves the wrong one (reproduced bug: a
        // texture assigned to 'Plane' ended up applied to a skinned character
        // that shared its id because an old reinsertion brought it back
        // with that id already taken).
        std::unordered_set<uint64_t> idsVivos;
        m_root.traverse([&](GameObject* n) { idsVivos.insert(n->id); });

        // The two components of which the scene admits ONE, looked at BEFORE
        // inserting for the same reason as idsVivos: afterwards the one that was alive
        // can no longer be told apart from the one that just arrived in the snapshot.
        bool yaHayCamara  = findCamera() != nullptr;
        bool yaHayOyente  = findAudioListener() != nullptr;

        GameObject* node = target->addChild(j.value("name", std::string()));
        // Same as fromJson and cloneGameObject: the warnings are from THIS operation.
        // Without this clear, each undo of a Delete piled its own on top of those of
        // the previous load and m_warnings grew during the whole session, against
        // what lastWarnings() promises in the header.
        m_warnings.clear();
        // Without a live object to ask (this rebuilds an already deleted
        // subtree: the undo of a Delete). If the caller brings the live meshes
        // (preloaded), the hasBones cache is seeded with them and the
        // disk is not touched; if not, it starts empty and only contributes the dedup among the
        // nodes of THAT subtree.
        NodeLoadCache cache;
        // By the REAL sourcePath of the mesh (m->sourcePath), not by the key of
        // PreloadedMeshCache: since Task 2 that key is meshCacheKey(sourcePath,
        // piece), which for a piece != 0 carries "#piece=N" and no longer matches the
        // plain sourcePath that hasBonesCache looks up in nodeFromJson. Without
        // this, each child of a piece != 0 (insertModelPieces, its redo via
        // CreateGameObjectCommand::execute, and the undo of its Delete) failed the
        // query to this cache and probed the file again with
        // ModelLoader::hasBones -a synchronous Assimp ReadFile on the main
        // thread-, exactly what preloaded exists to avoid. Same
        // pattern as Scene::cloneGameObject (see its comment, a few lines
        // above in this file). OR instead of assigning directly: if two
        // entries share sourcePath (several pieces of the same file), a later
        // non-skinned one must not mask a skinned one already seen.
        if (preloaded)
            for (const auto& [ruta, m] : *preloaded)
                if (m) cache.hasBones[m->sourcePath] = cache.hasBones[m->sourcePath] || (dynamic_cast<const SkinnedMesh*>(m.get()) != nullptr);
        try
        {
            // Empty root, counterpart of subtreeToJson: j came from there with an empty
            // root (verbatim paths), so it is read the same way. carryOverrideBaseline
            // matching the same true of subtreeToJson.
            nodeFromJson(j, node, target->worldTransform, physics, audio, &m_warnings, &cache, std::string(),
                         /*loader=*/nullptr, preloaded, /*carryOverrideBaseline=*/true);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(node);
            return nullptr;
        }

        // nodeFromJson already reused (or left, if j did not bring "id") the ids of node
        // and its whole subtree; now that it is complete, any that
        // clashes with idsVivos (the rest of the scene, captured BEFORE
        // inserting) gets a new one from the global counter. Never the other way round: the
        // node that WAS already alive keeps its own, because it may have
        // fresher references pointing at it (the current selection, a
        // command that was just executed) than the snapshot being
        // reinserted — see the full reasoning above, next to
        // idsVivos. It also covers a clash INSIDE the reinserted
        // subtree itself (two snapshot nodes with the same id): idsVivos
        // is extended with each id already accepted in this same traversal.
        //
        // Known side effect, out of scope for this fix: if the
        // reassigned one is `node` (the root of this subtree), the very command
        // that called insertFromJson keeps storing the OLD id in its
        // snapshot. Its next execute()/undo() will resolve that id by
        // findById and will not find this object — a silent no-op, it does not
        // corrupt another object (which is exactly what this guard avoids), but
        // the command becomes useless. Fixing it fully requires invalidating the
        // Undo/Redo history on scene reload; it is a separate audit
        // of the id system, not this specific patch.
        node->traverse([&](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
            if (idsVivos.count(n->id))
            {
                const uint64_t idViejo = n->id;
                n->id = GameObject::allocateId();
                m_warnings.push_back("node '" + n->name + "': id " + std::to_string(idViejo) +
                                      " from the snapshot was already in use in the scene; reassigned to " +
                                      std::to_string(n->id) + " to avoid clashing with the live object");
            }
            idsVivos.insert(n->id);

            // "At most one camera (and one AudioListener) per scene" was enforced by
            // fromJson (pruneExtraCameras) and cloneGameObject, but NOT this
            // path, which is the Undo of a Delete. Normal usage scenario:
            // you delete the camera, set up another, undo the deletion — and the scene
            // was left with two. findCamera returns the first in preorder, so
            // Play could end up looking through the one the user thought they had
            // replaced, without a single line saying so.
            //
            // The one that was ALREADY alive wins, same criterion as the id guard
            // above and as pruneExtraCameras. The GameObject comes back whole
            // —which is what the user asked for when undoing—, it is only left without the
            // component, and with a warning: losing something when undoing cannot be silent.
            //
            // The flags are UPDATED on accepting one, so a snapshot that
            // brings two cameras within itself also ends up with a single one
            // (same case as two equal ids inside the subtree itself).
            if (n->hasCameraComponent())
            {
                if (yaHayCamara)
                {
                    n->setCameraComponent(nullptr);
                    m_warnings.push_back("node '" + n->name +
                                          "': its camera is discarded (there is already a camera in the scene)");
                }
                else
                {
                    yaHayCamara = true;
                }
            }
            if (n->hasAudioListener())
            {
                if (yaHayOyente)
                {
                    n->setAudioListener(nullptr);
                    m_warnings.push_back("node '" + n->name +
                                          "': its Audio Listener is discarded (there is already one in the scene)");
                }
                else
                {
                    yaHayOyente = true;
                }
            }
        });

        // addChild() inserted at the end; reposition to index if it is not already there.
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

    void Scene::update(float dt)
    {
        m_root.traverse([](GameObject* go) {
            auto col = go->anyCollider();
            if (!col) return;

            const bool hasRb     = go->hasRigidbody();
            const bool kinematic = hasRb && go->getRigidbody()->getIsKinematic();
            const bool simulated = hasRb && !kinematic; // real dynamic body

            if (simulated)
            {
                // PhysX rules: read actor pose -> GameObject.
                go->worldTransform = col->getWorldTransform();
                glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
            }
            else if (kinematic)
            {
                // Kinematic: push pose GameObject -> actor (setKinematicTarget).
                col->syncTransform(go->worldTransform);
            }
            else
            {
                // Collider only (static): push the pose ONLY if it changed. Moving a
                // PxRigidStatic every frame dirties PhysX's scene-query pruner
                // (and emits warnings), so the actor's current pose (T*R, without scale)
                // is compared with the GameObject's normalized one
                // (removing scale) and it is only teleported if they differ.
                glm::mat4 want = go->worldTransform;
                glm::vec3 wantScale(1.0f);
                for (int i = 0; i < 3; ++i)
                {
                    float len = glm::length(glm::vec3(want[i]));
                    wantScale[i] = len;
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
                // The scale is compared SEPARATELY: `have` is the actor pose, which
                // never carries it (PxTransform does not support it), so a scale-only
                // change would not move a single bit of the loop above and the
                // geometry would keep the size of the previous frame.
                // It is checked against the one the collider already has baked.
                // In absolute value: the geometry uses abs(scale) —a mirror does not
                // thin the shape— and glm::decompose may distribute the signs
                // differently in a mirror matrix, which would leave a permanent
                // mismatch and a teleport per frame.
                const glm::vec3 haveScale = col->getWorldScale();
                for (int i = 0; i < 3 && !changed; ++i)
                    if (std::fabs(std::fabs(wantScale[i]) - std::fabs(haveScale[i])) > 1e-4f)
                        changed = true;
                if (changed) col->teleport(go->worldTransform);
            }
        });

        // Physics-transform sync runs before propagating local transforms:
        // the colliders already write worldTransform/localTransform directly,
        // so updateWorldTransforms() only needs to recompute the nodes
        // without a collider (children of a parent whose worldTransform may have changed).
        m_root.updateWorldTransforms();

        // After the transforms are up to date: if done before, each
        // sound would go one frame behind its object.
        updateAudioSpatial(dt);
    }

    void Scene::updateAudioSpatial(float dt)
    {
        m_root.traverse([dt](GameObject* go) {
            if (go->hasAudioClip())
                go->getAudioClip()->updateSpatial(glm::vec3(go->worldTransform[3]), dt);
        });
    }

    void Scene::syncReverbZones(AudioManager& audio)
    {
        // The ids seen in this sweep; what the manager has and is not here
        // belongs to a deleted GameObject and has to be released. Without this part, deleting
        // an object with a zone would leave its reverb playing forever.
        std::vector<uint64_t> alive;
        m_root.traverse([&](GameObject* go) {
            if (!go->hasReverbZone()) return;
            const auto& z = go->getReverbZone();
            audio.syncReverbZone(go->id, glm::vec3(go->worldTransform[3]),
                                  z->getMinDistance(), z->getMaxDistance(),
                                  z->getPreset(), z->getEnabled());
            alive.push_back(go->id);
        });
        audio.retainReverbZones(alive);
    }

    void Scene::shutdown()
    {
        // Releases EVERYTHING the scene holds, destroying the tree. The three
        // callers (fromJson, and the exit of the sandbox and the runtime) either
        // replace the scene right away or are closing the process, so
        // none uses it again; what the two hosts do need —and they
        // say so in a comment when calling— is that the destructors of the
        // components run BEFORE destroying PhysicsManager and AudioManager.
        // A ~Collider against an already released PxScene is the failure this
        // exists to avoid.
        //
        // Before this was a hand-written list: the 4 colliders, the
        // AudioClip and the scripts. It stayed at 6 of the 28 components, so
        // Rigidbody, Animator, ReverbZone and AudioListener survived — and
        // number 29 would have needed remembering a seventh line. It is the
        // same pattern that already failed FOUR times in invalidateCaches of the Properties
        // panel, and it is closed the same way: instead of enumerating what has to be
        // cleaned, what contains it is thrown away. The GameObject destructor cannot
        // fall short.
        //
        // The root keeps id and name because it is the identity of the scene, not
        // one of its objects: `fromJson` overwrites it a line later, but the
        // two hosts leave it alive until Scene is destroyed.
        const uint64_t    idRaiz     = m_root.id;
        const std::string nombreRaiz = m_root.name;
        m_root        = GameObject(nombreRaiz);
        m_root.id     = idRaiz;
        m_root.parent = nullptr;
    }

    nlohmann::json Scene::toJson() const
    {
        nlohmann::json root;
        root["version"] = 1;
        // carryOverrideBaseline is NOT passed (default false): this is the real
        // disk API, even if m_assetRoot is empty (project not open,
        // or the Task 6 tests that do not call setAssetRoot) — an empty
        // assetRoot by itself does NOT distinguish disk from memory, see the big
        // comment of nodeToJson.
        root["root"] = nodeToJson(m_root, m_assetRoot);
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

        // Builds the new tree in a temporary GameObject, disconnected from
        // m_root: if nodeFromJson throws in the middle of a malformed inner node,
        // the temporary is destroyed by itself on leaving scope (releasing the
        // colliders/audio already created in it — physics/audio are still alive) and
        // m_root stays intact. It guarantees that a failed load never leaves the
        // scene half rebuilt, not just in the version/root check
        // above but also in the face of nested malformation further down the tree
        // (spec: "failed load does not modify the scene").
        GameObject newRoot(rootJson.value("name", "root"));
        // hasBones() cache with life tied to THIS call to fromJson (local,
        // not a member nor static): an FBX can change on disk between two
        // scene loads within the same editor session, and a cache that
        // survived this function would serve a stale result — it would load
        // the wrong mesh type with nothing giving it away. Within a
        // single load the file is stable, so sharing it among the
        // nodes that repeat sourcePath (several enemies with the same FBX) is
        // safe and avoids repeating the full Assimp ReadFile for each.
        // It is now NodeLoadCache: it also carries the StaticModel of the
        // synchronous static branch, with the same life and for the same reason.
        NodeLoadCache hasBonesCache;
        try
        {
            // carryOverrideBaseline is NOT passed (default false), on purpose:
            // this is the real disk load, so the baseline of each
            // override is rederived from the material that just came out of the FBX, not
            // from a saved one. See the big comment of nodeToJson.
            nodeFromJson(rootJson, &newRoot, glm::mat4(1.0f), physics, audio, &m_warnings, &hasBonesCache, m_assetRoot, loader, preloaded);
        }
        catch (const nlohmann::json::exception&)
        {
            return false;
        }

        shutdown();
        m_root = std::move(newRoot);
        // addChild() (called inside nodeFromJson via newRoot.addChild/
        // node->addChild) points the parent of each direct child at the original
        // newRoot object — which was a local variable of this function. After
        // the move-assignment, m_root lives at its own stable address (it is
        // a member of Scene), so the parent of the direct children has to be re-pointed
        // to &m_root. Grandchildren and deeper descendants do NOT
        // need this fix: their parent points to their immediate parent, which
        // lives on the heap via unique_ptr and does not change address with this
        // move-assignment.
        m_root.parent = nullptr;
        for (auto& child : m_root.children)
            child->parent = &m_root;

        m_root.updateWorldTransforms();

        // After rebuilding, the repairs of whatever the FILE may bring wrong.
        // The ids first: the other two prunes warn naming nodes, and with
        // repeated ids the editor would already be resolving to the wrong object.
        pruneDuplicateIds();
        // The file may bring two cameras (hand-edited).
        pruneExtraCameras();
        // Same as the cameras: the file may bring two listeners.
        pruneExtraAudioListeners();
        collapseWarnings(); // after the prunes: they also push warnings
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
