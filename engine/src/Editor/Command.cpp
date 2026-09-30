#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include <algorithm>
#include <cmath>
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

} // namespace DonTopo
