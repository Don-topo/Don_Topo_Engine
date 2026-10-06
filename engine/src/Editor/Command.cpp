#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Editor/PrefabOverrideTracker.h"
#include "DonTopo/Files/FileManager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/type_ptr.hpp>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace DonTopo {

GameObject* duplicateAsSibling(Scene& scene, GameObject* src,
                                PhysicsManager& physics, AudioManager& audio)
{
    // Without a parent it is the scene root: it cannot have siblings and
    // cloneGameObject rejects it anyway.
    if (!src || !src->parent) return nullptr;
    // SIBLING: the duplicate's parent is the ORIGINAL's parent, not the original. Passing
    // `src` here would hang it from itself and every Ctrl+D would nest one more level.
    return scene.cloneGameObject(src, src->parent, physics, audio);
}

std::vector<GameObject*> insertModelPieces(Scene& scene, GameObject* parent, const std::string& sourcePath,
                                           const std::vector<ModelPiece>& pieces,
                                           const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                           PhysicsManager& physics, AudioManager& audio,
                                           std::vector<std::string>* warnings)
{
    std::vector<GameObject*> out;
    if (!parent) return out;
    for (const ModelPiece& p : pieces)
    {
        if (p.piece < 0 || static_cast<size_t>(p.piece) >= meshes.size() || !meshes[p.piece]) continue;
        glm::mat4 local = p.transform;
        const float* v = glm::value_ptr(local);
        if (!std::all_of(v, v + 16, [](float f) { return std::isfinite(f); }))
        {
            local = glm::mat4(1.0f);
            if (warnings) warnings->push_back("Piece '" + p.name + "' had an invalid transform: the identity is used");
        }
        nlohmann::json localJson = nlohmann::json::array();
        for (int i = 0; i < 16; ++i) localJson.push_back(glm::value_ptr(local)[i]);
        const nlohmann::json j = {
            { "name", p.name.empty() ? std::string("Piece ") + std::to_string(p.piece) : p.name },
            { "localTransform", localJson },
            { "mesh", { { "sourcePath", sourcePath }, { "name", meshes[p.piece]->name }, { "skinned", false },
                        { "visible", true }, { "piece", p.piece } } },
            { "children", nlohmann::json::array() } };
        PreloadedMeshCache cache;
        cache[meshCacheKey(sourcePath, p.piece)] = meshes[p.piece];
        if (GameObject* node = scene.insertFromJson(j, parent, parent->children.size(), physics, audio, &cache))
            out.push_back(node);
    }
    return out;
}

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
    if (!node || !newParent) return;

    // The move itself lives in Scene::reparent (Core) since Lua also
    // needs it: two copies of the same cut-and-paste over unique_ptr is how
    // cycles get fixed in one and not in the other. The indices this
    // command stores are already indices over the list without the node, which is
    // exactly what reparent expects.
    m_scene.reparent(node, newParent, index);
}

DeleteGameObjectCommand::DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio,
                                                  EditorRenderer& renderer, std::string label,
                                                  uint64_t parentId, size_t index, nlohmann::json snapshot)
    : m_scene(scene), m_physics(physics), m_audio(audio), m_renderer(renderer),
      m_label(std::move(label)), m_parentId(parentId), m_index(index), m_snapshot(std::move(snapshot)) {}

void DeleteGameObjectCommand::execute()
{
    uint64_t id = m_snapshot.value("id", uint64_t{0});
    GameObject* node = m_scene.findById(id);
    if (!node) return;
    m_meshes = m_scene.collectMeshes(node);
    // The GPU is released by Scene::removeGameObject via its listener (P8): this was
    // the third place that had to remember, and the only one without its own hook.
    m_scene.removeGameObject(node);
}

void DeleteGameObjectCommand::undo()
{
    GameObject* parent = m_scene.findById(m_parentId);
    GameObject* node = m_scene.insertFromJson(m_snapshot, parent, m_index, m_physics, m_audio, &m_meshes);
    if (node)
    {
        m_renderer.registerGameObject(node);
        // registerGameObject enqueues the upload in the deferred batch; without a flush
        // the recreated object would be invisible for ~2 frames (pop-in). This is a
        // synchronous, user-initiated transition, so it blocks.
        m_renderer.flushUploadsAndWait();
    }
}

CreateGameObjectCommand::CreateGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio,
                                                  EditorRenderer& renderer, std::string label,
                                                  uint64_t parentId, size_t index, nlohmann::json snapshot,
                                                  PreloadedMeshCache preloaded)
    : m_scene(scene), m_physics(physics), m_audio(audio), m_renderer(renderer),
      m_label(std::move(label)), m_parentId(parentId), m_index(index), m_snapshot(std::move(snapshot)),
      m_preloaded(std::move(preloaded)) {}

void CreateGameObjectCommand::execute()
{
    GameObject* parent = m_scene.findById(m_parentId);
    GameObject* node = m_scene.insertFromJson(m_snapshot, parent, m_index, m_physics, m_audio,
                                               m_preloaded.empty() ? nullptr : &m_preloaded);
    if (node)
    {
        m_renderer.registerGameObject(node);
        // Same reason as in DeleteGameObjectCommand::undo: synchronous upload
        // so that the created object is visible right away, without a ~2 frame pop-in.
        m_renderer.flushUploadsAndWait();
    }
}

void CreateGameObjectCommand::undo()
{
    uint64_t id = m_snapshot.value("id", uint64_t{0});
    GameObject* node = m_scene.findById(id);
    if (!node) return;
    m_renderer.removeGameObject(node);
    m_scene.removeGameObject(node);
}

CameraComponentCommand::CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, CameraState state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(state) {}

void CameraComponentCommand::execute() { apply(m_add); }
void CameraComponentCommand::undo()    { apply(!m_add); }

void CameraComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setCameraComponent(nullptr);
        return;
    }

    auto cam = std::make_shared<CameraComponent>();
    cam->setMode(m_state.mode);
    // far before near: setNear clamps against the current far (see
    // CameraComponent::setNear).
    cam->setFar(m_state.farPlane);
    cam->setNear(m_state.nearPlane);
    cam->setFov(m_state.fov);
    cam->setOrthographicSize(m_state.orthographicSize);
    go->setCameraComponent(cam);
}

CanvasComponentCommand::CanvasComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, CanvasComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(state) {}

void CanvasComponentCommand::execute() { apply(m_add); }
void CanvasComponentCommand::undo()    { apply(!m_add); }

void CanvasComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setCanvas(nullptr);
        return;
    }
    go->setCanvas(std::make_shared<CanvasComponent>(m_state));
}

AudioClipComponentCommand::AudioClipComponentCommand(Scene& scene, AudioManager& audio,
                                                      std::string label, uint64_t id, bool add,
                                                      std::string path, AudioClipState state)
    : m_scene(scene), m_audio(audio), m_label(std::move(label)), m_id(id), m_add(add),
      m_path(std::move(path)), m_state(state) {}

void AudioClipComponentCommand::execute() { apply(m_add); }
void AudioClipComponentCommand::undo()    { apply(!m_add); }

void AudioClipComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setAudioClip(nullptr);
        return;
    }
    // is3D and loop are baked into the sound's FMOD_MODE, so they are passed to
    // the factory instead of being assigned afterwards: doing it with the setters would force
    // an immediate reload of the freshly created sound.
    auto clip = m_audio.createAudioClipComponent(m_path, m_state.is3D, m_state.loop);
    // The asset may have disappeared from disk between the Remove and the Ctrl+Z. The
    // GameObject is left without a clip instead of with a broken one, which is what
    // Scene::fromJson does in that case.
    if (!clip) return;
    clip->setPlayOnAwake(m_state.playOnAwake);
    clip->setVolume(m_state.volume);
    clip->setPitch(m_state.pitch);
    // Max before min because of the min <= max invariant of the setters, same as
    // in scene loading.
    clip->setMaxDistance(m_state.maxDistance);
    clip->setMinDistance(m_state.minDistance);
    go->setAudioClip(std::move(clip));
}

AudioListenerComponentCommand::AudioListenerComponentCommand(Scene& scene, std::string label,
                                                              uint64_t id, bool add, bool enabled)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_enabled(enabled) {}

void AudioListenerComponentCommand::execute() { apply(m_add); }
void AudioListenerComponentCommand::undo()    { apply(!m_add); }

void AudioListenerComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setAudioListener(nullptr);
        return;
    }
    auto listener = std::make_shared<AudioListenerComponent>();
    listener->setEnabled(m_enabled);
    go->setAudioListener(std::move(listener));
}

ButtonComponentCommand::ButtonComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, ButtonComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ButtonComponentCommand::execute() { apply(m_add); }
void ButtonComponentCommand::undo()    { apply(!m_add); }

void ButtonComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setButton(nullptr);
        return;
    }
    go->setButton(std::make_shared<ButtonComponent>(m_state));
}

TextComponentCommand::TextComponentCommand(Scene& scene, std::string label, uint64_t id,
                                            bool add, TextComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void TextComponentCommand::execute() { apply(m_add); }
void TextComponentCommand::undo()    { apply(!m_add); }

void TextComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setText(nullptr);
        return;
    }
    go->setText(std::make_shared<TextComponent>(m_state));
}

ProgressBarComponentCommand::ProgressBarComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                          bool add, ProgressBarComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ProgressBarComponentCommand::execute() { apply(m_add); }
void ProgressBarComponentCommand::undo()    { apply(!m_add); }

void ProgressBarComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setProgressBar(nullptr);
        return;
    }
    go->setProgressBar(std::make_shared<ProgressBarComponent>(m_state));
}

LayoutComponentCommand::LayoutComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, LayoutComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void LayoutComponentCommand::execute() { apply(m_add); }
void LayoutComponentCommand::undo()    { apply(!m_add); }

void LayoutComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setLayout(nullptr);
        return;
    }
    go->setLayout(std::make_shared<LayoutComponent>(m_state));
}

PanelComponentCommand::PanelComponentCommand(Scene& scene, std::string label, uint64_t id,
                                              bool add, PanelComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void PanelComponentCommand::execute() { apply(m_add); }
void PanelComponentCommand::undo()    { apply(!m_add); }

void PanelComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setPanel(nullptr);
        return;
    }
    go->setPanel(std::make_shared<PanelComponent>(m_state));
}

ImageComponentCommand::ImageComponentCommand(Scene& scene, std::string label, uint64_t id,
                                              bool add, ImageComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ImageComponentCommand::execute() { apply(m_add); }
void ImageComponentCommand::undo()    { apply(!m_add); }

void ImageComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setImage(nullptr);
        return;
    }
    go->setImage(std::make_shared<ImageComponent>(m_state));
}

SliderComponentCommand::SliderComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, SliderComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void SliderComponentCommand::execute() { apply(m_add); }
void SliderComponentCommand::undo()    { apply(!m_add); }

void SliderComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setSlider(nullptr);
        return;
    }
    go->setSlider(std::make_shared<SliderComponent>(m_state));
}

CheckboxComponentCommand::CheckboxComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, CheckboxComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void CheckboxComponentCommand::execute() { apply(m_add); }
void CheckboxComponentCommand::undo()    { apply(!m_add); }

void CheckboxComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setCheckbox(nullptr);
        return;
    }
    go->setCheckbox(std::make_shared<CheckboxComponent>(m_state));
}

ToggleComponentCommand::ToggleComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, ToggleComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ToggleComponentCommand::execute() { apply(m_add); }
void ToggleComponentCommand::undo()    { apply(!m_add); }

void ToggleComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setToggle(nullptr);
        return;
    }
    go->setToggle(std::make_shared<ToggleComponent>(m_state));
}

ScrollbarComponentCommand::ScrollbarComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, ScrollbarComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ScrollbarComponentCommand::execute() { apply(m_add); }
void ScrollbarComponentCommand::undo()    { apply(!m_add); }

void ScrollbarComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setScrollbar(nullptr);
        return;
    }
    go->setScrollbar(std::make_shared<ScrollbarComponent>(m_state));
}

InputFieldComponentCommand::InputFieldComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, InputFieldComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void InputFieldComponentCommand::execute() { apply(m_add); }
void InputFieldComponentCommand::undo()    { apply(!m_add); }

void InputFieldComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setInputField(nullptr);
        return;
    }
    go->setInputField(std::make_shared<InputFieldComponent>(m_state));
}

DropdownComponentCommand::DropdownComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, DropdownComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void DropdownComponentCommand::execute() { apply(m_add); }
void DropdownComponentCommand::undo()    { apply(!m_add); }

void DropdownComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setDropdown(nullptr);
        return;
    }
    go->setDropdown(std::make_shared<DropdownComponent>(m_state));
}

ScrollViewComponentCommand::ScrollViewComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, ScrollViewComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void ScrollViewComponentCommand::execute() { apply(m_add); }
void ScrollViewComponentCommand::undo()    { apply(!m_add); }

void ScrollViewComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setScrollView(nullptr);
        return;
    }
    go->setScrollView(std::make_shared<ScrollViewComponent>(m_state));
}

AnimatorComponentCommand::AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                    bool add, AnimatorComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void AnimatorComponentCommand::execute() { apply(m_add); }
void AnimatorComponentCommand::undo()    { apply(!m_add); }

void AnimatorComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setAnimator(nullptr);
        return;
    }
    go->setAnimator(std::make_shared<AnimatorComponent>(m_state));
}

AnimatorGraphCommand::AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id,
                                           AnimatorComponent::Graph before,
                                           AnimatorComponent::Graph after)
    : m_scene(scene), m_label(std::move(label)), m_id(id),
      m_before(std::move(before)), m_after(std::move(after)) {}

void AnimatorGraphCommand::execute() { apply(m_after); }
void AnimatorGraphCommand::undo()    { apply(m_before); }

void AnimatorGraphCommand::apply(const AnimatorComponent::Graph& g)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->getAnimator()) return;
    go->getAnimator()->applyGraph(g);
    // The snapshot carries the clipIndex values from when it was taken, and an animation
    // source added or removed in the meantime changes the clip list: they are
    // resolved again by name. rebindClips and not bindClips: this runs in
    // Edit Mode (Ctrl+Z is disabled in Play, see
    // EditorUI::handleUndoRedoShortcut), where the preview clock
    // keeps running and the parameter values seen in the panel are
    // the live ones; bindClips's reset() would zero them and the preview would jump
    // abruptly to the entry state, visible to the user. Secondarily
    // it also protects a future undo in the middle of Play.
    if (const SkinnedMesh* mesh = go->getSkinnedMesh())
        go->getAnimator()->rebindClips(*mesh, nullptr);
    // The snapshot carries the whole property clips: their indices and each track's
    // `resolved` are redone just like the clipIndex values.
    go->getAnimator()->bindProperties(go, nullptr);
}

AnimationSourceCommand::AnimationSourceCommand(Scene& scene, EditorRenderer* renderer,
                                                std::string label, uint64_t id, bool add,
                                                std::string path,
                                                std::vector<std::string> clipNames,
                                                size_t pathOccurrence)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_add(add), m_path(std::move(path)), m_clipNames(std::move(clipNames)),
      m_pathOccurrence(pathOccurrence) {}

void AnimationSourceCommand::execute() { m_add ? applyAdd() : applyRemove(); }
void AnimationSourceCommand::undo()    { m_add ? applyRemove() : applyAdd(); }

void AnimationSourceCommand::applyAdd()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    // Writes clips/sources: editSkinnedMesh copies the mesh if it is shared.
    SkinnedMesh* mesh = go->editSkinnedMesh();
    if (!mesh) return;

    std::vector<std::string> warnings;
    // Empty m_clipNames = first time (the user has just chosen the
    // file): the names are decided by addAnimationSource and stored here
    // so that a later redo reproduces exactly the same ones.
    const std::vector<std::string>* forced = m_clipNames.empty() ? nullptr : &m_clipNames;
    if (!addAnimationSource(*mesh, m_path, warnings, forced)) return;

    m_clipNames = mesh->animationSources.back().clipNames;
    // addAnimationSource always adds at the end: the source we have just
    // (re)inserted is, by definition, the last one with that path (occurrence 0
    // counted from the end). This is recomputed in every applyAdd (both the
    // real Add and the undo of a Remove that reinserts at the end) so that a later
    // applyRemove keeps pointing to IT and not to the ordinal that was
    // captured at the original click, which after the reinsert no longer describes its
    // position (see the long comment in applyRemove).
    m_pathOccurrence = 0;

    // bindClips resolves by name in Scene::nodeFromJson, but that only
    // runs on a scene load: here the mesh mutates live (possibly
    // in the middle of Play Mode) and nobody else re-resolves clipIndex. Without this, the
    // states keep the OLD index, which after the add is still
    // valid as an index but may point to a different clip (Finding 1 of
    // the review: "Remove" shifts the rest of the list). rebindClips (without
    // the reset() of bindClips) preserves m_currentState and the user's
    // parameters, which bindClips would destroy.
    if (go->hasAnimator())
    {
        std::vector<std::string> bindWarnings;
        go->getAnimator()->rebindClips(*mesh, &bindWarnings);
        go->getAnimator()->bindProperties(go, &bindWarnings);
        // No log channel from Command.cpp (ICommand does not know
        // EditorContext/pushLog, unlike AnimatorPanel): they are
        // discarded, same precedent that this very method already sets a few
        // lines above with the addAnimationSource warnings.
    }

    if (m_renderer && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
}

void AnimationSourceCommand::applyRemove()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    // Writes clips/sources: editSkinnedMesh copies the mesh if it is shared.
    SkinnedMesh* mesh = go->editSkinnedMesh();
    if (!mesh) return;

    // Location by IDENTITY, not by position: m_pathOccurrence is
    // "the Nth source with this path, counting from the end of the vector",
    // and that is stable for ONE isolated command, but it breaks with
    // INTERLEAVED commands. applyAdd (further up) ALWAYS reinserts at the end; so
    // when the undo of a pending Remove reinserts a source, the "end"
    // that the pathOccurrence of ANOTHER command in the stack assumed has
    // shifted underneath. Real example (final review, blocking):
    // sources=[B,S1], the same FBX is reimported (cmdA, sources=[B,S1,S2]), row
    // S1 is removed (cmdB, pathOccurrence=1 because S2 stays ahead,
    // sources=[B,S2]); Ctrl+Z of cmdB reinserts S1 at the end (sources=
    // [B,S2,S1]); Ctrl+Z of cmdA, with its pathOccurrence=0, finds the
    // FIRST non-builtin source scanning from the end, which is now the
    // just-recovered S1, not the S2 that cmdA actually inserted.
    //
    // uniqueClipName (SkinnedMeshAnimations.cpp) guarantees that clip names
    // are unique within the mesh (removeAnimationSource already relies on
    // that invariant, see its comment), so the exact set of
    // clipNames of a source identifies it unambiguously, whatever its current
    // position in the vector. m_clipNames always travels with the command:
    // the panel passes it (src.clipNames) or applyAdd recomputes it after a
    // successful (re)import, so in the normal path it is never empty
    // when we get here.
    bool removed = false;
    if (!m_clipNames.empty())
    {
        for (size_t i = mesh->animationSources.size(); i-- > 0; )
        {
            const auto& src = mesh->animationSources[i];
            if (src.builtin || src.clipNames != m_clipNames) continue;
            removeAnimationSource(*mesh, i);
            removed = true;
            break;
        }
    }

    // Positional fallback: it can only fire if m_clipNames arrives empty
    // (it should not in the normal path) or if no live source matches by
    // names (e.g. redo after a scene reload, with the mesh rebuilt
    // from JSON). The original scan is kept, counted from the end
    // for the same reason as always: applyAdd reinserts at the end, so
    // pathOccurrence=0 still points to the last one reinserted.
    if (!removed)
    {
        size_t skipped = 0;
        for (size_t i = mesh->animationSources.size(); i-- > 0; )
        {
            const auto& src = mesh->animationSources[i];
            if (src.builtin || src.path != m_path) continue;
            if (skipped != m_pathOccurrence) { skipped++; continue; }
            m_clipNames = src.clipNames;
            removeAnimationSource(*mesh, i);
            removed = true;
            break;
        }
    }

    // Nothing to remove (redo after a scene reload, or mesh replaced between
    // execute() and undo()): rebuildSkinnedMesh is a vkDeviceWaitIdle + a
    // full destroy/recreate of buffers/textures/descriptor sets, and
    // paying for it for a mesh that did not change is pure waste (applyAdd already
    // did this same early-return with its "if (!addAnimationSource(...))
    // return;").
    if (!removed) return;

    // Same reason as in applyAdd: the mesh mutated live and clipIndex
    // points to indices that no longer describe the same clips (the gap
    // left by the removed clip shifts the ones behind it). Not reset(): the
    // Animator's runtime state is preserved.
    if (go->hasAnimator())
    {
        std::vector<std::string> bindWarnings;
        go->getAnimator()->rebindClips(*mesh, &bindWarnings);
        go->getAnimator()->bindProperties(go, &bindWarnings);
        // They are discarded for the same reason as in applyAdd: there is no log
        // channel available from an ICommand.
    }

    if (m_renderer && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
}

ClipRenameCommand::ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                                      std::string oldName, std::string newName)
    : m_scene(scene), m_label(std::move(label)), m_id(id),
      m_oldName(std::move(oldName)), m_newName(std::move(newName)) {}

void ClipRenameCommand::execute() { apply(m_oldName, m_newName); }
void ClipRenameCommand::undo()    { apply(m_newName, m_oldName); }

void ClipRenameCommand::apply(const std::string& from, const std::string& to)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    // Writes clips/sources: editSkinnedMesh copies the mesh if it is shared.
    SkinnedMesh* mesh = go->editSkinnedMesh();
    if (!mesh) return;
    if (!renameClip(*mesh, from, to)) return;
    if (go->hasAnimator())
        go->getAnimator()->renameClipReferences(from, to);
}

void setMaterialTextureOverride(GameObject& go, int materialIndex,
                                 MaterialTextureSlot slot, const std::string& path)
{
    MaterialOverride* ov = nullptr;
    for (auto& candidato : go.materialOverrides)
        if (candidato.index == materialIndex) { ov = &candidato; break; }

    if (!ov)
    {
        MaterialOverride nuevo;
        nuevo.index = materialIndex;
        go.materialOverrides.push_back(nuevo);
        ov = &go.materialOverrides.back();
    }

    switch (slot)
    {
        case MaterialTextureSlot::Albedo: ov->albedo = path; break;
        case MaterialTextureSlot::Normal: ov->normal = path; break;
        case MaterialTextureSlot::Orm:    ov->orm    = path; break;
    }

    applyMaterialOverrides(go);
}

MaterialTextureCommand::MaterialTextureCommand(Scene& scene, EditorRenderer* renderer,
                                                std::string label, uint64_t id,
                                                int materialIndex, MaterialTextureSlot slot,
                                                std::string before, std::string after)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_materialIndex(materialIndex), m_slot(slot),
      m_before(std::move(before)), m_after(std::move(after)) {}

void MaterialTextureCommand::execute() { apply(m_after); }
void MaterialTextureCommand::undo()    { apply(m_before); }

void MaterialTextureCommand::apply(const std::string& path)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->hasMesh()) return;

    // The material_index was valid when this command was built, but the
    // replay (undo/redo) can arrive much later: removing the Mesh component
    // does not push a command (it does not reorder the stack), so a Ctrl+Y on THIS
    // command can land with the GameObject already carrying ANOTHER mesh, with fewer
    // materials than the one from back then. setMaterialTextureOverride is left WITHOUT
    // this cut on purpose (it is the primitive shared with the scene reader, and its
    // tolerance for an out-of-range index is what lets
    // a .scene with more materials than the currently loaded mesh has
    // not lose the override), but this caller does know the live mesh and
    // can avoid writing an index that no longer exists in it.
    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (m_materialIndex < 0 || m_materialIndex >= (int)mats.size()) return;

    setMaterialTextureOverride(*go, m_materialIndex, m_slot, path);

    if (!m_renderer) return;

    // Skinned and static go through different paths because the GPU resources
    // are different: the character is rebuilt whole (it is the only thing there is), the
    // static one just changes material.
    if (const SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
    else if (go->staticRenderIndex >= 0)
        m_renderer->rebuildStaticMesh(go->staticRenderIndex, *go->getMesh());
}

void setMaterialAssetOverride(GameObject& go, int materialIndex, const std::string& matAssetPath)
{
    MaterialOverride* ov = nullptr;
    for (auto& candidato : go.materialOverrides)
        if (candidato.index == materialIndex) { ov = &candidato; break; }
    if (!ov)
    {
        MaterialOverride nuevo;
        nuevo.index = materialIndex;
        go.materialOverrides.push_back(nuevo);
        ov = &go.materialOverrides.back();
    }
    ov->matAsset = matAssetPath;
    applyMaterialOverrides(go);
}

MaterialAssetCommand::MaterialAssetCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                                           uint64_t id, int materialIndex,
                                           std::string before, std::string after)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_materialIndex(materialIndex), m_before(std::move(before)), m_after(std::move(after)) {}

void MaterialAssetCommand::execute() { apply(m_after); }
void MaterialAssetCommand::undo()    { apply(m_before); }

void MaterialAssetCommand::apply(const std::string& matAssetPath)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->hasMesh()) return;

    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (m_materialIndex < 0 || m_materialIndex >= (int)mats.size()) return;

    setMaterialAssetOverride(*go, m_materialIndex, matAssetPath);

    if (!m_renderer) return;
    if (const SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
    else if (go->staticRenderIndex >= 0)
        m_renderer->rebuildStaticMesh(go->staticRenderIndex, *go->getMesh());
}

void setMaterialFactorOverride(GameObject& go, int materialIndex,
                                MaterialFactorSlot slot, float value)
{
    MaterialOverride* ov = nullptr;
    for (auto& candidato : go.materialOverrides)
        if (candidato.index == materialIndex) { ov = &candidato; break; }

    if (!ov)
    {
        MaterialOverride nuevo;
        nuevo.index = materialIndex;
        go.materialOverrides.push_back(nuevo);
        ov = &go.materialOverrides.back();
    }

    switch (slot)
    {
        case MaterialFactorSlot::Metallic:  ov->metallic  = value; break;
        case MaterialFactorSlot::Roughness: ov->roughness = value; break;
    }

    applyMaterialOverrides(go);
}

MaterialFactorCommand::MaterialFactorCommand(Scene& scene, EditorRenderer* renderer,
                                              std::string label, uint64_t id,
                                              int materialIndex, MaterialFactorSlot slot,
                                              float before, float after)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_materialIndex(materialIndex), m_slot(slot), m_before(before), m_after(after) {}

void MaterialFactorCommand::execute() { apply(m_after); }
void MaterialFactorCommand::undo()    { apply(m_before); }

void MaterialFactorCommand::apply(float value)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->hasMesh()) return;

    // Same cut as MaterialTextureCommand::apply, same reason: the index
    // was valid when this command was built, but the replay (undo/redo)
    // can arrive with the GameObject carrying another mesh, with fewer
    // materials than the one from back then.
    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (m_materialIndex < 0 || m_materialIndex >= (int)mats.size()) return;

    setMaterialFactorOverride(*go, m_materialIndex, m_slot, value);

    if (!m_renderer) return;

    // The static one is NO LONGER rebuilt: the factors left the dedup key
    // (makeSharedMeshKey), so they are two floats per object and it is enough to
    // write them. Before, a whole rebuildStaticMesh was needed (with
    // waitForGpu and three textures coming back) just because changing a number
    // changed the object's key.
    //
    // The SKINNED one still does its rebuild: its factors live per SUBMESH (Vulkan
    // in SkinnedMatGfx, D3D12 in SkinnedSubMesh) and there is no per-submesh setter,
    // which would be yet another public method. It is the expensive path, but a character
    // has a handful of submeshes and this only runs on slider RELEASE or on an
    // undo, never per drag frame.
    if (const SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
    else if (go->staticRenderIndex >= 0)
        m_renderer->setObjectMaterialFactors(static_cast<size_t>(go->staticRenderIndex),
                                             mats[(size_t)m_materialIndex]->metallic,
                                             mats[(size_t)m_materialIndex]->roughness);
}

MeshComponentCommand::MeshComponentCommand(Scene& scene, EditorRenderer* renderer,
                                           std::string label, GameObject& go, bool add)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(go.id),
      m_add(add), m_meshVista(go.getMesh()), m_overrides(go.materialOverrides) {}

void MeshComponentCommand::execute() { if (m_add) put();    else remove(); }
void MeshComponentCommand::undo()    { if (m_add) remove(); else put();    }

void MeshComponentCommand::remove()
{
    GameObject* go = m_scene.findById(m_id);
    // Only OURS: if another arrived through a path without undo, it is not this
    // command's.
    if (!go || !go->hasMesh() || go->getMesh() != m_meshVista.lock()) return;

    // From here on the command is the owner: the object releases it now.
    m_mesh = go->getMesh();

    // The backend releases the GPU and does setMesh(nullptr). Without a renderer (headless
    // tests) only the CPU part remains.
    if (m_renderer) m_renderer->removeMeshComponent(go);
    else            go->setMesh(nullptr);

    // Guard by hasMesh(), the SAME signal that decides whether the panel draws the
    // Mesh section and whether it accepts loading a replacement. If a backend went back to
    // leaving the mesh in place, emptying anyway would leave the Material showing
    // the override's texture with nothing writing it to the .scene: the
    // assignment would be lost on reload with nothing warning about it.
    if (!go->hasMesh())
        go->materialOverrides.clear();
}

void MeshComponentCommand::put()
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !m_mesh) return;
    if (go->hasMesh() || go->pendingMeshJob != 0) return;

    go->setMesh(m_mesh);
    // It belongs to the object again: the command releases it so as not to count as owner
    // (see m_meshVista) and keeps only the view.
    m_meshVista = m_mesh;
    m_mesh.reset();
    // AFTER setMesh, which lowers the base*Taken: with the snapshot's baselines
    // raised, applyMaterialOverrides does not recapture as original what
    // the stored mesh already carries baked in.
    go->materialOverrides = m_overrides;
    applyMaterialOverrides(*go);

    if (!m_renderer) return;
    if (const SkinnedMesh* sk = go->getSkinnedMesh())
        go->skinnedRenderIndex = m_renderer->addSkinnedMesh(*sk, nullptr);
    else
        go->staticRenderIndex  = m_renderer->addStaticMesh(*go->getMesh(), nullptr);
    // Same reason as DeleteGameObjectCommand::undo: without waiting, the recovered
    // object would appear ~2 frames late.
    m_renderer->flushUploadsAndWait();
}

// ── Prefab commands ─────────────────────────────────────────────────────────

namespace {

namespace fs = std::filesystem;

// Exact bytes, binary: what the content guards compare and what Apply's undo
// writes back. FileManager::readText takes a narrow string; a fs::path does
// not depend on the code page.
std::optional<std::string> readFileBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return ss.str();
}

bool writeFileBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << bytes;
    out.close();
    return !out.fail();
}

uint64_t localIdOf(const nlohmann::json& n, const char* key)
{
    const auto it = n.find(key);
    if (it == n.end()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) { const int64_t v = it->get<int64_t>(); return v > 0 ? static_cast<uint64_t>(v) : 0; }
    return 0;
}

nlohmann::json* findTemplateNode(nlohmann::json& n, uint64_t localId)
{
    if (localIdOf(n, "id") == localId) return &n;
    if (auto kids = n.find("children"); kids != n.end() && kids->is_array())
        for (auto& c : *kids)
            if (nlohmann::json* found = findTemplateNode(c, localId)) return found;
    return nullptr;
}

uint64_t maxTemplateId(const nlohmann::json& n)
{
    uint64_t m = localIdOf(n, "id");
    if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
        for (const auto& c : *kids) m = (std::max)(m, maxTemplateId(c));
    return m;
}

bool sameTarget(const PrefabOverride& a, const PrefabOverride& b)
{
    return a.node == b.node && a.path == b.path;
}

// which empty = every override.
bool selected(const std::vector<PrefabOverride>& which, const PrefabOverride& o)
{
    return which.empty() ||
           std::any_of(which.begin(), which.end(), [&](const PrefabOverride& w) { return sameTarget(w, o); });
}

const PrefabTemplate* readyTemplate(const PrefabLibrary& lib, const std::string& path)
{
    const PrefabTemplate* t = lib.find(path);
    if (!t || t->state != PrefabTemplate::State::Ready || !t->memoryRoot.is_object()) return nullptr;
    return t;
}

// The roots of an instance's added subtrees, in pre-order: children of a
// linked node that are not linked themselves. A nested instance root (its own
// link) counts as one: its subtree belongs to another prefab.
void collectAddedRoots(GameObject* instanceRoot, std::vector<GameObject*>& out)
{
    for (auto& c : instanceRoot->children)
    {
        GameObject* k = c.get();
        if (k->prefabNode != 0 && !k->prefabLink) collectAddedRoots(k, out);
        else out.push_back(k);
    }
}

size_t indexInParent(const GameObject* node)
{
    if (!node || !node->parent) return SIZE_MAX;
    const auto& kids = node->parent->children;
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].get() == node) return i;
    return SIZE_MAX;
}

std::string stemOf(const std::string& path)
{
    return fs::path(path).stem().string();
}

// Undo merge: each entry of `back` goes into `list` unless the list already
// has one on the same node + path (recorded after the command: it wins).
void addMissing(std::vector<PrefabOverride>& list, const std::vector<PrefabOverride>& back)
{
    for (const PrefabOverride& b : back)
        if (std::none_of(list.begin(), list.end(), [&](const PrefabOverride& o) { return sameTarget(o, b); }))
            list.push_back(b);
}

// Entries of `before` with no entry on the same node + path in `after`.
std::vector<PrefabOverride> missingFrom(const std::vector<PrefabOverride>& before,
                                        const std::vector<PrefabOverride>& after)
{
    std::vector<PrefabOverride> out;
    for (const PrefabOverride& b : before)
        if (std::none_of(after.begin(), after.end(), [&](const PrefabOverride& a) { return sameTarget(a, b); }))
            out.push_back(b);
    return out;
}

} // namespace

std::vector<uint64_t> PrefabCommand::instancesOf(const std::string& key) const
{
    std::vector<uint64_t> ids;
    m_ctx.scene.traverse([&](GameObject* n) {
        if (n->prefabLink && n->parent && m_ctx.library.normalizeKey(n->prefabLink->path) == key)
            ids.push_back(n->id);
    });
    return ids;
}

bool PrefabCommand::ensureSynced(uint64_t rootId)
{
    GameObject* root = m_ctx.scene.findById(rootId);
    if (!root || !root->prefabLink) return false;
    const PrefabTemplate* t = readyTemplate(m_ctx.library, root->prefabLink->path);
    if (!t) return false;
    if (m_ctx.tracker.isSynced(root, m_ctx.library)) return true;
    // Never checked against this generation (a clone, a template that just
    // became Ready): what the host's next revision pass would do, now.
    propagateInstance(m_ctx.scene, root, *t, m_ctx.tracker, m_ctx.physics, m_ctx.audio, m_ctx.registerGpu);
    root = m_ctx.scene.findById(rootId);
    return root && m_ctx.tracker.isSynced(root, m_ctx.library);
}

void PrefabCommand::recordPendingEdits(uint64_t rootId)
{
    GameObject* root = m_ctx.scene.findById(rootId);
    if (!root || !root->prefabLink) return;
    if (m_ctx.tracker.isSynced(root, m_ctx.library))
    {
        // Exactly what the tracker's revision pass does.
        const PrefabTemplate* t = readyTemplate(m_ctx.library, root->prefabLink->path);
        m_ctx.scene.recomputePrefabOverrides(root, t->memoryRoot);
        m_ctx.tracker.captureBaseline(m_ctx.scene, root, &t->memoryRoot);
    }
    else if (m_ctx.tracker.hasBaseline(rootId))
    {
        m_ctx.tracker.foldUnrecordedEdits(m_ctx.scene, root);
    }
}

void PrefabCommand::resyncInstance(uint64_t rootId, const nlohmann::json* standsOn)
{
    // What the tracker knew about this instance described it BEFORE the
    // command: its baseline would turn the command's own change into a folded
    // "edit", its synced generation would let a recompute diff it against a
    // template it no longer stands on.
    m_ctx.tracker.forget(rootId);
    GameObject* root = m_ctx.scene.findById(rootId);
    if (!root || !root->prefabLink) return;
    const PrefabTemplate* t = readyTemplate(m_ctx.library, root->prefabLink->path);
    m_ctx.tracker.captureBaseline(m_ctx.scene, root, standsOn ? standsOn : (t ? &t->memoryRoot : nullptr));
    // Ready: brought to it now (a match leaves it untouched and synced, a
    // mismatch rebuilds it). Loading: the host's onReady propagates it, and
    // the baseline just taken keeps an edit made meanwhile.
    if (t)
        propagateInstance(m_ctx.scene, root, *t, m_ctx.tracker, m_ctx.physics, m_ctx.audio, m_ctx.registerGpu);
}

// ---- Create ----

CreatePrefabCommand::CreatePrefabCommand(PrefabCommandContext ctx, uint64_t nodeId, std::filesystem::path absPrefabPath)
    : PrefabCommand(std::move(ctx), "Create Prefab '" + absPrefabPath.stem().string() + "'"),
      m_nodeId(nodeId), m_path(std::move(absPrefabPath))
{
    m_key = m_ctx.library.normalizeKey(m_path.string());
}

void CreatePrefabCommand::execute()
{
    m_done = false;
    m_message.clear();
    GameObject* node = m_ctx.scene.findById(m_nodeId);
    if (!node || !node->parent)
    {
        m_message = "Create Prefab: the object no longer exists or is the scene root";
        return;
    }
    bool linked = false;
    for (GameObject* p = node; p; p = p->parent)
        if (p->prefabNode != 0 || p->prefabLink) linked = true;
    node->traverse([&](GameObject* n) { if (n->prefabNode != 0 || n->prefabLink) linked = true; });
    if (linked)
    {
        m_message = "'" + node->name + "' is already part of a prefab instance (nested prefabs are not supported)";
        return;
    }
    if (m_key.empty())
    {
        m_message = "Create Prefab: invalid path '" + m_path.string() + "'";
        return;
    }
    // Links are project-relative (Global Constraints): normalizeKey leaves a
    // path outside the project root absolute.
    if (fs::path(m_key).is_absolute() || fs::path(m_key).has_root_name())
    {
        m_message = "Create Prefab: '" + m_path.string() + "' is outside the project; "
                    "prefabs must be saved inside it";
        return;
    }
    // Never over someone else's file. The one exception is a redo finding the
    // exact bytes this command wrote (nothing deleted them in between).
    std::error_code ec;
    const bool existed = fs::exists(m_path, ec);
    if (existed)
    {
        const std::optional<std::string> current = readFileBytes(m_path);
        if (!m_hasWritten || !current || *current != m_written)
        {
            m_message = "'" + m_path.string() + "' already exists: not overwritten";
            return;
        }
    }

    const nlohmann::json file = m_ctx.scene.makePrefab(node, m_key, /*link=*/true);
    if (!FileManager::writeJson(m_path.string(), file))
    {
        m_ctx.scene.unpackPrefab(node);
        // A write that failed half way may have left a truncated file. It is
        // removed only if THIS execute created it (there was none before).
        if (!existed && fs::exists(m_path, ec)) fs::remove(m_path, ec);
        m_message = "Create Prefab: could not write '" + m_path.string() + "'";
        return;
    }
    // What is on disk now (text mode may have turned '\n' into "\r\n"): the
    // bytes the undo's content guard compares against.
    const std::optional<std::string> written = readFileBytes(m_path);
    m_hasWritten = written.has_value();
    m_written    = written.value_or(std::string());

    m_ctx.library.updateFromMemory(m_key, file, m_ctx.audio);
    nlohmann::json standsOn = file["root"];
    resolveStoredPaths(standsOn, m_ctx.library.assetRoot());
    resyncInstance(m_nodeId, &standsOn);
    m_done = true;
    m_message = "Created prefab '" + m_path.string() + "'";
}

void CreatePrefabCommand::undo()
{
    if (!m_done) return;
    m_done = false;
    GameObject* node = m_ctx.scene.findById(m_nodeId);
    if (node && node->prefabLink && m_ctx.library.normalizeKey(node->prefabLink->path) == m_key)
        m_ctx.scene.unpackPrefab(node);
    m_ctx.tracker.forget(m_nodeId);

    // The file goes ONLY if it still holds exactly the bytes execute wrote:
    // decided by what is inside it, never by its name. Anything else (an Apply
    // whose undo is not in the stack any more, an external editor, a copy
    // dropped over it) keeps it.
    std::error_code ec;
    const std::string shown = "'" + m_path.string() + "'";
    if (!fs::exists(m_path, ec))
    {
        m_ctx.library.evict(m_key);
        m_message = "Prefab file " + shown + " was already gone";
        return;
    }
    const std::optional<std::string> current = readFileBytes(m_path);
    if (!m_hasWritten || !current)
    {
        m_message = "Prefab file " + shown + " could not be read: kept";
        return;
    }
    if (*current != m_written)
    {
        m_message = "Prefab file " + shown + " was modified after it was created: kept";
        return;
    }
    if (!fs::remove(m_path, ec) || ec)
    {
        m_message = "Prefab file " + shown + " could not be removed: kept";
        return;
    }
    // No file, no template: Instantiate in Play must not keep working on a
    // prefab that does not exist, nor the renderer keep its meshes pinned (the
    // host unpins through the library's onEvicted). A redo registers it again.
    m_ctx.library.evict(m_key);
    m_message = "Prefab file " + shown + " deleted";
}

// ---- Instantiate ----

InstantiatePrefabCommand::InstantiatePrefabCommand(PrefabCommandContext ctx, std::string key, uint64_t parentId)
    : PrefabCommand(std::move(ctx), "Instantiate '" + stemOf(key) + "'"),
      m_key(std::move(key)), m_parentId(parentId) {}

void InstantiatePrefabCommand::execute()
{
    m_done = false;
    m_message.clear();
    GameObject* parent = m_parentId != 0 ? m_ctx.scene.findById(m_parentId) : nullptr;
    if (m_parentId != 0 && !parent)
    {
        m_message = "Instantiate: the parent object no longer exists";
        return;
    }
    GameObject* node = nullptr;
    if (m_snapshot.is_null())
    {
        const PrefabTemplate* t = readyTemplate(m_ctx.library, m_key);
        if (!t)
        {
            m_message = "Instantiate: prefab '" + m_key + "' is not loaded";
            return;
        }
        // Fresh ids for every node (spec §7).
        node = m_ctx.scene.instantiatePrefab(*t, parent, nullptr, nullptr, m_ctx.physics, m_ctx.audio);
        for (const std::string& w : m_ctx.scene.lastWarnings())
            m_message += (m_message.empty() ? "" : "\n") + w;
        if (!node) return;
        m_rootId   = node->id;
        m_snapshot = m_ctx.scene.subtreeToJson(node);
        m_meshes   = Scene::collectMeshes(node);
        m_index    = indexInParent(node);
    }
    else
    {
        // Redo: the snapshot of the first execute, ids included, so later
        // commands in the stack still resolve their targets.
        node = m_ctx.scene.insertFromJson(m_snapshot, parent, m_index, m_ctx.physics, m_ctx.audio,
                                          m_meshes.empty() ? nullptr : &m_meshes);
        if (!node)
        {
            m_message = "Instantiate: could not rebuild the instance";
            return;
        }
    }
    if (m_ctx.registerGpu) m_ctx.registerGpu(node);
    resyncInstance(m_rootId);
    m_done = true;
}

void InstantiatePrefabCommand::undo()
{
    if (!m_done) return;
    m_done = false;
    m_ctx.tracker.forget(m_rootId);
    if (GameObject* node = m_ctx.scene.findById(m_rootId))
        m_ctx.scene.removeGameObject(node);
}

// ---- Apply ----

ApplyPrefabOverridesCommand::ApplyPrefabOverridesCommand(PrefabCommandContext ctx, uint64_t instanceRootId,
                                                         std::vector<PrefabOverride> which, bool includeAddedChildren,
                                                         std::vector<uint64_t> onlyAddedChildIds)
    : PrefabCommand(std::move(ctx), "Apply to Prefab"), m_rootId(instanceRootId), m_which(std::move(which)),
      m_includeAdded(includeAddedChildren), m_onlyAdded(std::move(onlyAddedChildIds))
{
    if (GameObject* root = m_ctx.scene.findById(m_rootId); root && root->prefabLink)
        m_label = "Apply to Prefab '" + stemOf(root->prefabLink->path) + "'";
}

void ApplyPrefabOverridesCommand::rebuildInstances(const std::vector<uint64_t>& instances,
                                                   const nlohmann::json& tmpl, bool recordNewIds,
                                                   bool recordOrphans)
{
    // Rebuilt from the command's own template, now: it may still be loading in
    // the library. Seeded with the template's meshes and every instance's live
    // ones (an applied child's mesh is in the applying instance), so no file is
    // read for what is already in RAM.
    PreloadedMeshCache cache;
    MaterialAssetCache mats;
    if (const PrefabTemplate* t = m_ctx.library.find(m_key))
    {
        cache = t->meshes;
        mats  = t->mats;
    }
    for (uint64_t id : instances)
        if (GameObject* r = m_ctx.scene.findById(id))
            for (const auto& [key, mesh] : Scene::collectMeshes(r)) cache.emplace(key, mesh);

    for (uint64_t id : instances)
    {
        GameObject* r = m_ctx.scene.findById(id);
        if (!r || !r->prefabLink) continue;
        if (!prefabInstanceMatchesTemplate(m_ctx.scene, r, tmpl))
        {
            const std::vector<PrefabOverride> before = r->prefabLink->overrides;
            std::unordered_set<uint64_t> had;
            r->traverse([&](GameObject* n) { had.insert(n->id); });
            const auto hints = m_createdIds.find(id);
            bool rebuilt = false;
            GameObject* live = m_ctx.scene.rebuildPrefabInstance(
                r, tmpl, m_ctx.physics, m_ctx.audio, &cache, &mats, &rebuilt,
                hints != m_createdIds.end() ? &hints->second : nullptr);
            if (live && m_ctx.registerGpu) m_ctx.registerGpu(live);
            if (live && rebuilt && recordNewIds)
                live->traverse([&](GameObject* n) {
                    if (n->prefabNode != 0 && !had.count(n->id)) m_createdIds[id][n->prefabNode] = n->id;
                });
            if (live && rebuilt && recordOrphans && live->prefabLink)
            {
                // What the new template orphaned (a component applied as removed):
                // the undo gives these back, and only these.
                std::vector<PrefabOverride> lost = missingFrom(before, live->prefabLink->overrides);
                if (!lost.empty()) m_orphaned[id] = std::move(lost);
            }
        }
        // Stands on tmpl from here on, also while the library still loads it.
        m_ctx.tracker.forget(id);
        if (GameObject* live = m_ctx.scene.findById(id))
            m_ctx.tracker.captureBaseline(m_ctx.scene, live, &tmpl);
    }
}

void ApplyPrefabOverridesCommand::execute()
{
    m_done = false;
    m_message.clear();
    m_applied.clear();
    m_linkedAdded.clear();
    GameObject* root = m_ctx.scene.findById(m_rootId);
    if (!root || !root->prefabLink || !root->parent)
    {
        m_message = "Apply: the object is not a prefab instance";
        return;
    }
    m_key = m_ctx.library.normalizeKey(root->prefabLink->path);
    const PrefabTemplate* loaded = readyTemplate(m_ctx.library, m_key);
    if (!loaded)
    {
        m_message = "Apply: the prefab '" + m_key + "' is not loaded";
        return;
    }
    m_path = fs::path(m_ctx.library.assetRoot()) / fs::path(m_key);
    const std::optional<std::string> previous = readFileBytes(m_path);
    if (!previous)
    {
        m_message = "Apply: could not read '" + m_path.string() + "'";
        return;
    }
    nlohmann::json file = nlohmann::json::parse(*previous, nullptr, /*allow_exceptions=*/false);
    if (!file.is_object() || !file.contains("root") || !file["root"].is_object())
    {
        m_message = "Apply: '" + m_path.string() + "' is not a valid prefab file";
        return;
    }
    // The new file is built from the template in memory: if the file is no
    // longer what was loaded (edited outside the editor), that would silently
    // erase the outside change.
    {
        nlohmann::json onDisk = file["root"];
        resolveStoredPaths(onDisk, m_ctx.library.assetRoot());
        if (!prefabValuesEqual(onDisk, loaded->memoryRoot))
        {
            m_message = "Apply: '" + m_key + "' changed on disk; reload the scene first";
            return;
        }
    }
    if (!ensureSynced(m_rootId))
    {
        m_message = "Apply: '" + root->name + "' is not up to date with its prefab yet (still loading); try again";
        return;
    }
    // Edits of any instance of this prefab the tracker has not recorded yet:
    // the rebuilds below derive each instance from its stored overrides.
    const std::vector<uint64_t> instances = instancesOf(m_key);
    for (uint64_t id : instances) recordPendingEdits(id);

    root = m_ctx.scene.findById(m_rootId);
    const PrefabTemplate* t = readyTemplate(m_ctx.library, m_key);
    if (!root || !t)
    {
        m_message = "Apply: the prefab is not loaded";
        return;
    }

    // The new template: the chosen overrides applied to the current one...
    nlohmann::json tmpl = t->memoryRoot;
    std::vector<PrefabOverride> chosen;
    for (const PrefabOverride& o : root->prefabLink->overrides)
        if (selected(m_which, o)) chosen.push_back(o);
    std::vector<std::string> warnings;
    applyPrefabOverrides(tmpl, chosen, &warnings);

    // ...plus the added children, with new prefab-local ids max+1.., each
    // placed right after the linked sibling that precedes it in the instance,
    // so the instance's linked children keep the template's order.
    std::vector<std::pair<uint64_t, uint64_t>> toLink;   // scene id -> new local id
    if (m_includeAdded)
    {
        std::vector<GameObject*> added;
        collectAddedRoots(root, added);
        uint64_t next = maxTemplateId(tmpl);
        std::unordered_map<uint64_t, uint64_t> newLocal;   // added root scene id -> its local id
        for (GameObject* a : added)
        {
            if (!m_onlyAdded.empty() &&
                std::find(m_onlyAdded.begin(), m_onlyAdded.end(), a->id) == m_onlyAdded.end())
                continue;
            bool nested = false;
            a->traverse([&](GameObject* n) { if (n->prefabLink || n->prefabNode != 0) nested = true; });
            if (nested)
            {
                warnings.push_back("'" + a->name + "' is a prefab instance itself: not applied "
                                   "(nested prefabs are not supported)");
                continue;
            }
            GameObject* parent = a->parent;
            nlohmann::json* parentJson = findTemplateNode(tmpl, parent->prefabNode);
            if (!parentJson)
            {
                warnings.push_back("'" + a->name + "': its parent is not in the prefab; not applied");
                continue;
            }
            uint64_t after = 0;
            for (const auto& c : parent->children)
            {
                if (c.get() == a) break;
                if (c->prefabLink) continue;
                if (c->prefabNode != 0) after = c->prefabNode;
                else if (const auto it = newLocal.find(c->id); it != newLocal.end()) after = it->second;
            }
            nlohmann::json sub = m_ctx.scene.subtreeToMemoryJson(a);
            std::vector<GameObject*> order;
            a->traverse([&](GameObject* n) { order.push_back(n); });
            const uint64_t first = next + 1;
            std::function<void(nlohmann::json&)> renumber = [&](nlohmann::json& j) {
                j["id"] = ++next;
                j.erase("prefab");
                j.erase("prefabNode");
                if (auto kids = j.find("children"); kids != j.end() && kids->is_array())
                    for (auto& k : *kids) renumber(k);
            };
            renumber(sub);
            // nodeToJson writes every child, in order: the same pre-order as traverse.
            for (size_t i = 0; i < order.size(); ++i) toLink.emplace_back(order[i]->id, first + i);
            newLocal[a->id] = first;

            nlohmann::json& kids = (*parentJson)["children"];
            if (!kids.is_array()) kids = nlohmann::json::array();
            size_t pos = 0;
            if (after != 0)
                for (size_t i = 0; i < kids.size(); ++i)
                    if (localIdOf(kids[i], "id") == after) { pos = i + 1; break; }
            kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(pos), std::move(sub));
        }
    }
    if (chosen.empty() && toLink.empty())
    {
        m_message = "Apply: nothing to apply";
        for (const std::string& w : warnings) m_message += "\n" + w;
        return;
    }

    nlohmann::json disk = tmpl;
    storeRelativePaths(disk, m_ctx.library.assetRoot());
    file["root"] = std::move(disk);
    if (!FileManager::writeJson(m_path.string(), file))
    {
        // A failed write may have truncated it: the previous bytes go back.
        writeFileBytes(m_path, *previous);
        m_message = "Apply: could not write '" + m_path.string() + "'";
        return;
    }
    m_previous = *previous;
    m_written  = readFileBytes(m_path).value_or(std::string());

    // The instance no longer overrides what it applied, and its applied
    // children are linked. BEFORE the library update: when the template is
    // Ready at once, its onReady propagates every instance from these links.
    root = m_ctx.scene.findById(m_rootId);
    std::erase_if(root->prefabLink->overrides, [&](const PrefabOverride& o) {
        return std::any_of(chosen.begin(), chosen.end(), [&](const PrefabOverride& c) { return sameTarget(c, o); });
    });
    m_applied = chosen;
    for (const auto& [sceneId, local] : toLink)
        if (GameObject* n = m_ctx.scene.findById(sceneId))
        {
            n->prefabNode = local;
            m_linkedAdded.push_back(sceneId);
        }
    // Every instance rebuilt from the new template now, with the ids of the
    // first execute on a redo; the library's onReady then finds them matching.
    m_orphaned.clear();
    rebuildInstances(instances, tmpl, /*recordNewIds=*/!m_idsRecorded, /*recordOrphans=*/true);
    m_idsRecorded = true;
    m_ctx.library.updateFromMemory(m_key, file, m_ctx.audio);
    // Ready at once: synced now. Still loading: each one's baseline stands on
    // the new template, and the host's onReady syncs them.
    if (readyTemplate(m_ctx.library, m_key))
        for (uint64_t id : instances) resyncInstance(id);

    m_done = true;
    m_message = "Applied " + std::to_string(chosen.size()) + " override(s)" +
                (toLink.empty() ? std::string() : " and " + std::to_string(toLink.size()) + " added object(s)") +
                " to '" + m_key + "'";
    for (const std::string& w : warnings) m_message += "\n" + w;
}

void ApplyPrefabOverridesCommand::undo()
{
    if (!m_done) return;
    m_done = false;
    // Decided FIRST, before anything is touched: the previous bytes go back
    // only over the bytes Apply wrote. A file changed since (outside the
    // editor) is kept, and then the scene is left exactly as Apply left it too
    // -- rolling the links back over a template that still has the applied
    // content would duplicate the applied children and pin applied values as
    // overrides.
    const std::optional<std::string> current = readFileBytes(m_path);
    const nlohmann::json prev = nlohmann::json::parse(m_previous, nullptr, /*allow_exceptions=*/false);
    if (!current || *current != m_written || !prev.is_object() || !prev.contains("root") ||
        !prev["root"].is_object())
    {
        m_message = "'" + m_path.string() + "' changed on disk after the Apply: kept as it is "
                    "(nothing was undone)";
        return;
    }
    if (!writeFileBytes(m_path, m_previous))
    {
        m_message = "'" + m_path.string() + "' could not be written: nothing was undone";
        return;
    }
    nlohmann::json standsOn = prev["root"];
    resolveStoredPaths(standsOn, m_ctx.library.assetRoot());

    // Edits recorded since the Apply stay; only what Apply changed goes back.
    const std::vector<uint64_t> instances = instancesOf(m_key);
    for (uint64_t id : instances) recordPendingEdits(id);
    if (GameObject* r = m_ctx.scene.findById(m_rootId); r && r->prefabLink)
        addMissing(r->prefabLink->overrides, m_applied);
    for (const auto& [id, lost] : m_orphaned)
        if (GameObject* r = m_ctx.scene.findById(id); r && r->prefabLink)
            addMissing(r->prefabLink->overrides, lost);    for (uint64_t id : m_linkedAdded)
        if (GameObject* n = m_ctx.scene.findById(id)) n->prefabNode = 0;

    // Links first, then the library: a template Ready at once propagates every
    // instance from them in its onReady (and finds them matching).
    rebuildInstances(instances, standsOn, /*recordNewIds=*/false, /*recordOrphans=*/false);
    m_ctx.library.updateFromMemory(m_key, prev, m_ctx.audio);
    if (readyTemplate(m_ctx.library, m_key))
        for (uint64_t id : instances) resyncInstance(id);
    m_message = "Restored '" + m_path.string() + "'";
}

// ---- Revert ----

RevertPrefabOverridesCommand::RevertPrefabOverridesCommand(PrefabCommandContext ctx, uint64_t instanceRootId,
                                                           std::vector<PrefabOverride> which, bool includeAddedChildren)
    : PrefabCommand(std::move(ctx), "Revert Prefab Overrides"), m_rootId(instanceRootId),
      m_which(std::move(which)), m_includeAdded(includeAddedChildren)
{
    if (GameObject* root = m_ctx.scene.findById(m_rootId); root && root->prefabLink)
        m_label = "Revert Prefab Overrides '" + stemOf(root->prefabLink->path) + "'";
}

void RevertPrefabOverridesCommand::execute()
{
    m_done = false;
    m_message.clear();
    GameObject* root = m_ctx.scene.findById(m_rootId);
    if (!root || !root->prefabLink || !root->parent)
    {
        m_message = "Revert: the object is not a prefab instance";
        return;
    }
    if (!ensureSynced(m_rootId))
    {
        m_message = "Revert: '" + root->name + "' is not up to date with its prefab yet (still loading); try again";
        return;
    }
    recordPendingEdits(m_rootId);
    root = m_ctx.scene.findById(m_rootId);

    std::vector<PrefabOverride> chosen;
    for (const PrefabOverride& o : root->prefabLink->overrides)
        if (selected(m_which, o)) chosen.push_back(o);
    std::vector<uint64_t> addedIds;
    if (m_includeAdded)
    {
        std::vector<GameObject*> added;
        collectAddedRoots(root, added);
        for (GameObject* a : added) addedIds.push_back(a->id);
    }
    if (chosen.empty() && addedIds.empty())
    {
        m_message = "Revert: nothing to revert";
        return;
    }

    // Only what is reverted is kept for the undo: the overrides (with their
    // values) and each deleted child whole (ids, meshes), where it hung.
    m_reverted = chosen;
    m_deleted.clear();
    for (uint64_t id : addedIds)
        if (GameObject* n = m_ctx.scene.findById(id))
            m_deleted.push_back({ n->parent->id, indexInParent(n), m_ctx.scene.subtreeToJson(n),
                                  Scene::collectMeshes(n) });

    std::erase_if(root->prefabLink->overrides, [&](const PrefabOverride& o) {
        return std::any_of(chosen.begin(), chosen.end(), [&](const PrefabOverride& c) { return sameTarget(c, o); });
    });
    for (uint64_t id : addedIds)
        if (GameObject* n = m_ctx.scene.findById(id)) m_ctx.scene.removeGameObject(n);
    // Rebuilt from the template with what is left (synced, fresh baseline).
    resyncInstance(m_rootId);
    m_done = true;
}

void RevertPrefabOverridesCommand::undo()
{
    if (!m_done) return;
    m_done = false;
    m_message.clear();
    GameObject* root = m_ctx.scene.findById(m_rootId);
    if (!root || !root->prefabLink)
    {
        m_message = "Revert: the instance no longer exists; nothing was undone";
        return;
    }
    // What was recorded since the Revert stays: the reverted overrides come
    // back only where the instance has no newer entry on the same path.
    recordPendingEdits(m_rootId);
    root = m_ctx.scene.findById(m_rootId);
    addMissing(root->prefabLink->overrides, m_reverted);
    // In the order they were deleted, so each index is the one it had.
    for (DeletedChild& d : m_deleted)
    {
        GameObject* parent = m_ctx.scene.findById(d.parentId);
        if (!parent)
        {
            m_message += (m_message.empty() ? "" : "\n") + std::string("Revert: the parent of a deleted object "
                                                                       "no longer exists; it was not restored");
            continue;
        }
        GameObject* node = m_ctx.scene.insertFromJson(d.snapshot, parent, d.index, m_ctx.physics, m_ctx.audio,
                                                      d.meshes.empty() ? nullptr : &d.meshes);
        if (node && m_ctx.registerGpu) m_ctx.registerGpu(node);
    }
    // Re-derived from the CURRENT template with the restored overrides (added
    // children are carried as they are, ids kept).
    resyncInstance(m_rootId);
}

// ---- Unpack ----

UnpackPrefabCommand::UnpackPrefabCommand(PrefabCommandContext ctx, uint64_t instanceRootId)
    : PrefabCommand(std::move(ctx), "Unpack Prefab"), m_rootId(instanceRootId)
{
    if (GameObject* root = m_ctx.scene.findById(m_rootId))
        m_label = "Unpack Prefab '" + root->name + "'";
}

void UnpackPrefabCommand::execute()
{
    m_done = false;
    m_message.clear();
    GameObject* root = m_ctx.scene.findById(m_rootId);
    if (!root || !root->prefabLink)
    {
        m_message = "Unpack: the object is not a prefab instance";
        return;
    }
    // An edit the tracker has not recorded yet would be lost by the undo's
    // resync (it re-derives the instance from its stored overrides).
    recordPendingEdits(m_rootId);
    root = m_ctx.scene.findById(m_rootId);
    // The objects as they are now, linked: the undo tells the edits made to
    // them while unpacked apart from this.
    m_atUnpack = m_ctx.scene.subtreeToMemoryJson(root);
    m_nodes.clear();
    root->traverse([&](GameObject* n) {
        if (n->prefabNode == 0 && !n->prefabLink) return;
        m_nodes.push_back({ n->id, n->prefabNode,
                            n->prefabLink ? std::make_shared<PrefabLink>(*n->prefabLink) : nullptr });
    });
    m_ctx.scene.unpackPrefab(root);
    for (const NodeLink& n : m_nodes)
        if (n.link) m_ctx.tracker.forget(n.id);
    m_done = true;
}

void UnpackPrefabCommand::undo()
{
    if (!m_done) return;
    m_done = false;
    for (const NodeLink& n : m_nodes)
        if (GameObject* node = m_ctx.scene.findById(n.id))
        {
            node->prefabNode = n.prefabNode;
            node->prefabLink = n.link ? std::make_shared<PrefabLink>(*n.link) : nullptr;
        }

    // Edits made to the objects while they were plain objects: each linked
    // node is diffed against itself at Unpack time, and the differences are
    // merged into the restored overrides (a newer value wins). Without this
    // the resync below would re-derive the instance and silently drop them.
    if (GameObject* root = m_ctx.scene.findById(m_rootId); root && root->prefabLink && m_atUnpack.is_object())
    {
        // Linked nodes by prefab-local id; a nested instance's subtree is its own.
        auto index = [](const nlohmann::json& tree) {
            std::unordered_map<uint64_t, std::pair<const nlohmann::json*, bool>> out;   // local -> (node, isRoot)
            std::function<void(const nlohmann::json&, bool)> walk = [&](const nlohmann::json& n, bool isRoot) {
                const uint64_t local = localIdOf(n, "prefabNode");
                if (local == 0 || (!isRoot && n.contains("prefab"))) return;
                out.emplace(local, std::make_pair(&n, isRoot));
                if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                    for (const auto& c : *kids) if (c.is_object()) walk(c, false);
            };
            walk(tree, true);
            return out;
        };
        const nlohmann::json live = m_ctx.scene.subtreeToMemoryJson(root);
        const auto before = index(m_atUnpack);
        std::vector<PrefabOverride>& list = root->prefabLink->overrides;
        for (const auto& [local, node] : index(live))
        {
            const auto b = before.find(local);
            if (b == before.end()) continue;
            for (PrefabOverride& e : diffPrefabNode(local, *b->second.first, *node.first, node.second))
            {
                auto same = std::find_if(list.begin(), list.end(), [&](const PrefabOverride& o) { return sameTarget(o, e); });
                if (same != list.end()) same->value = std::move(e.value);
                else                    list.push_back(std::move(e));
            }
        }
    }
    for (const NodeLink& n : m_nodes)
        if (n.link) resyncInstance(n.id);
}

} // namespace DonTopo
