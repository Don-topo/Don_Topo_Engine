#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Core/CopyToBuffer.h"
#include "DonTopo/Core/TransformDecompose.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <type_traits>
#include <variant>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"

namespace {

// Guard for this panel's asset dialogs (mesh, audio, font, atlas).
// Unlike Content Browser—which can no longer exit the project root—these
// dialogs browse the entire disk, so they are the only way an asset from
// ANOTHER project could enter the scene and end up in the export package.
//
// Two cases of "not in the open project" with different destinations:
// - Inside the workspace `projects/` but from ANOTHER project: rejected
//   outright (same message as always). Crossing assets between two separate
//   projects is still not the flow we want to support.
// - Genuinely outside the workspace (Desktop, Downloads, a USB...): it is
//   IMPORTED—copied to assets/Imported/<Type>/ if the extension is supported,
//   the same as external drop onto Content Browser. Before this function
//   existed, this case was accepted without copying (referenced the external
//   path as is); that "accept without copying" was precisely the hole this
//   feature closes—the initial fix inadvertently inverted the two branches
//   and left this case, the most common of the two, behaving exactly as
//   before (found in final review).
// No open project (headless tests) pass everything, as before the concept
// existed.
// The 18 asset dialogs from this panel pass through here when drained, and
// NO ONE applies a path without asking first. That is why the edit veto
// lives in this function and not in each "Browse..." button: gating the
// button only blocks the case of opening the dialog with the modal already
// placed, and leaves out the one that actually happens—the dialog opened
// BEFORE, which stays alive because none of these is modal (zero
// ImGuiFileDialogFlags_Modal in src/) and the toolbar stays clickable.
// Placed here, dialog number 19 inherits it by merely following the pattern
// of the other 18.
//
// The name says "accept or import", not "belongs to the project", because
// it answers two questions—whose asset is it and whether the panel is in
// condition to apply it right now—and also HAS EFFECTS: copies the file to
// the project and writes to the Log. Returns the path the caller should use
// (the copy, not the chosen one), or nullopt if rejected.
std::optional<std::filesystem::path> acceptOrImportAsset(const DonTopo::EditorContext& ctx,
                                                     const std::filesystem::path& path)
{
    // Veto while the Load Scene modal is active: the scene on which the dialog
    // was opened is being replaced, so applying the choice would write to an
    // object that is no longer the one the user had in front of them. With a
    // line in the Log, which is the only thing that distinguishes this from
    // "Browse did nothing".
    if (ctx.editingLocked)
    {
        ctx.logModule("Project", "Scene load in progress: the chosen asset is discarded");
        return std::nullopt;
    }

    if (!ctx.project || !ctx.project->valid()) return path;
    if (ctx.project->contains(path))           return path;

    // The workspace is created by the selector at startup, so this contains()
    // answers on a folder that exists; if it still failed, contains() returns
    // false and the path is treated as genuinely external (importable), not as
    // from another project.
    const DonTopo::ProjectContext workspace(DonTopo::ProjectContext::workspaceDir());
    if (workspace.contains(path))
    {
        ctx.logModule("Project", "Asset from another project, rejected: " + path.string());
        return std::nullopt;
    }

    // Genuinely outside the workspace: attempt to import. If the extension is
    // not supported, reject instead of accepting without copying—referencing
    // an absolute path outside the project is exactly what this feature wants
    // to stop doing.
    const std::string ext = path.extension().string();
    if (!DonTopo::isImportableExtension(ext))
    {
        ctx.logModule("Project", "Unsupported extension for import: " + path.string());
        return std::nullopt;
    }

    const std::filesystem::path destDir = DonTopo::importedAssetDestDir(ctx.project->root(), ext);
    const DonTopo::AssetImportOutcome outcome = DonTopo::importExternalAsset(path, destDir);
    if (outcome.result != DonTopo::AssetImportResult::Copied)
    {
        ctx.logModule("Project", "Import rejected (" + path.filename().string() + "): " +
                      DonTopo::describeImportResult(outcome));
        return std::nullopt;
    }
    ctx.logModule("Project", "Asset imported: " + outcome.destPath.filename().string());
    return outcome.destPath;
}

// 2 decimals—sufficient to read the value at a glance in the Log without
// kilometer-long lines; the Properties panel already shows 3 decimals for
// fine editing, the Log is just a readable summary.
std::string formatVec3(const glm::vec3& v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

std::string formatFloat(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", f);
    return buf;
}

// "attackRange" -> "Attack Range" (script property labels)
std::string prettyPropLabel(const std::string& raw)
{
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char c = raw[i];
        if (i == 0) { out += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); continue; }
        if (std::isupper(static_cast<unsigned char>(c))) out += ' ';
        out += c;
    }
    return out;
}

// Compare floats with tolerance—avoids pushing an Undo command when the
// drag ends at the same value it started with (rounding noise).
bool nearlyEqualF(float a, float b) { return std::fabs(a - b) < 0.0001f; }

// Warning below the "Is Trigger" checkbox when the GameObject has no
// Rigidbody.
//
// PhysX does not generate pairs between two static actors—they cannot
// move relative to each other, so it does not even call the filter shader—,
// and a collider without Rigidbody is PxRigidStatic. That is: a trigger
// without Rigidbody does NOT detect objects without one either. It is the
// same rule as Unity, but nothing here said so: you marked the checkbox,
// nothing happened, and there was no clue why. See trigger tests in
// physics_tests.cpp, which fix all three combinations.
void drawTriggerRigidbodyHint(const DonTopo::GameObject* go, bool isTrigger)
{
    if (!isTrigger || !go || go->hasRigidbody()) return;
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "No Rigidbody: only detects objects that have one");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PhysX does not report overlaps between two static objects.\n"
                          "Add a Rigidbody to this object or to the one that should enter.");
}

// Collider resolvers by type. Function pointers (no captures) so that
// drawColliderLayerCombo can put them in the Undo lambda, which must never
// capture a raw collider pointer: a Delete Undo rebuilds the GameObject
// and the old one hangs.
DonTopo::Collider* resolveBoxCollider(DonTopo::GameObject* go)
{ return go->hasBoxCollider() ? go->getBoxCollider().get() : nullptr; }
DonTopo::Collider* resolveSphereCollider(DonTopo::GameObject* go)
{ return go->hasSphereCollider() ? go->getSphereCollider().get() : nullptr; }
DonTopo::Collider* resolveCapsuleCollider(DonTopo::GameObject* go)
{ return go->hasCapsuleCollider() ? go->getCapsuleCollider().get() : nullptr; }
DonTopo::Collider* resolvePlaneCollider(DonTopo::GameObject* go)
{ return go->hasPlaneCollider() ? go->getPlaneCollider().get() : nullptr; }

// Collision layer dropdown, common to all 4 colliders. Does NOT cache the
// value in a member like the DragFloats beside it: a combo confirms in the
// same frame and there is no drag to protect, so the live collider is read.
// All 32 layers supported by the core are offered, whether named or not;
// the names are edited in project settings.
void drawColliderLayerCombo(DonTopo::EditorContext& ctx, const char* label,
                            const char* seccion, DonTopo::Collider* collider,
                            DonTopo::Collider* (*resolve)(DonTopo::GameObject*))
{
    if (!collider || !ctx.selected) return;

    // Only layers CREATED in project settings, not the 32 in the ceiling.
    // If the collider ended up in a layer that no longer exists (should not:
    // removeLayer reassigns), the list is expanded up to it to avoid showing
    // an empty combo.
    const int creadas = ctx.physics ? ctx.physics->layerCount() : 1;
    const int antes   = collider->getLayer();
    const int total   = std::max(creadas, antes + 1);

    std::string etiquetas[DonTopo::PhysicsManager::kLayerCount];
    const char* items[DonTopo::PhysicsManager::kLayerCount];
    for (int i = 0; i < total; ++i)
    {
        const std::string nombre = ctx.physics ? ctx.physics->getLayerName(i) : std::string();
        etiquetas[i] = std::to_string(i) + (nombre.empty() ? std::string() : ": " + nombre);
        items[i]     = etiquetas[i].c_str();
    }

    int capa = antes;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (!ImGui::Combo(label, &capa, items, total)) return;
    if (capa == antes) return;

    collider->setLayer(capa);
    const std::string desc = std::string("Layer of '") + ctx.selected->name + "' (" + seccion + ")";
    ctx.pushLog(desc + " changed to " + etiquetas[capa]);
    if (!ctx.scene || !ctx.undo) return;

    DonTopo::Scene* scene = ctx.scene;
    const uint64_t  id    = ctx.selected->id;
    ctx.undo->push(std::make_unique<DonTopo::PropertyCommand<int>>(
        desc, antes, capa,
        [scene, id, resolve](const int& c) {
            DonTopo::GameObject* go = scene->findById(id);
            if (!go) return;
            if (DonTopo::Collider* col = resolve(go)) col->setLayer(c);
        }));
}

// The path that is NOW in the material for that slot (override applied or
// what the FBX brought): that is what is shown.
std::string currentTexturePath(const DonTopo::Material& mat, DonTopo::MaterialTextureSlot slot)
{
    switch (slot)
    {
        case DonTopo::MaterialTextureSlot::Albedo: return mat.texturePath;
        case DonTopo::MaterialTextureSlot::Normal: return mat.normalMapPath;
        case DonTopo::MaterialTextureSlot::Orm:    return mat.metallicRoughnessPath;
    }
    return {};
}

// The path of the OVERRIDE, which is not the same: empty means "this is
// from the FBX", and it is what decides whether Clear has something to do.
std::string currentOverride(const DonTopo::GameObject& go, int materialIndex,
                            DonTopo::MaterialTextureSlot slot)
{
    for (const DonTopo::MaterialOverride& ov : go.materialOverrides)
        if (ov.index == materialIndex)
            switch (slot)
            {
                case DonTopo::MaterialTextureSlot::Albedo: return ov.albedo;
                case DonTopo::MaterialTextureSlot::Normal: return ov.normal;
                case DonTopo::MaterialTextureSlot::Orm:    return ov.orm;
            }
    return {};
}

bool hasOverride(const DonTopo::GameObject& go, int materialIndex, DonTopo::MaterialTextureSlot slot)
{
    return !currentOverride(go, materialIndex, slot).empty();
}

// The .mat linked to that slot, or empty if there is none.
std::string currentMaterialAsset(const DonTopo::GameObject& go, int materialIndex)
{
    for (const DonTopo::MaterialOverride& ov : go.materialOverrides)
        if (ov.index == materialIndex) return ov.matAsset;
    return {};
}

// The OVERRIDE factor, RAW (the sentinel -1.0f if there is none for that
// slot), symmetric to currentOverride() with textures—and for the same
// reason: mat.metallic/mat.roughness are the value ALREADY APPLIED (FBX or
// override), and using THAT as the "before" of a command would break undo.
// A PropertyCommand<float> with before=FBX-value and slot="there was no
// override" are different data even though the number matches: undo must
// return "without override" (the sentinel), not rewrite the FBX value AS
// the active override—which is exactly what nodeToJson serializes, and
// would nail the factor in the .scene even though the user never touched it.
float currentFactorOverride(const DonTopo::GameObject& go, int materialIndex,
                            DonTopo::MaterialFactorSlot slot)
{
    for (const DonTopo::MaterialOverride& ov : go.materialOverrides)
        if (ov.index == materialIndex)
            switch (slot)
            {
                case DonTopo::MaterialFactorSlot::Metallic:  return ov.metallic;
                case DonTopo::MaterialFactorSlot::Roughness: return ov.roughness;
            }
    return -1.0f;
}

} // namespace

namespace DonTopo {

PropertiesPanel::PropertiesPanel()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_textureFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_matAssetFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_fontFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_uiAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_textFontFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_barAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_inputFieldAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_inputFieldFontFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_dropdownAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_dropdownFontFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_scrollViewAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_sliderAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_checkboxAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_toggleAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_scrollbarAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_panelAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_imageAtlasFileDialog(std::make_unique<IGFD::FileDialog>())
{
}

PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::invalidateCaches()
{
    // ALL of one statement, and that is why they are in a struct. An Undo/Redo
    // mutates components IN PLACE (the pointer does not change), so a cache
    // that is not reset leaves its section showing the undone value, and the
    // next drag of ANOTHER field reapplies that stale value and resurrects the
    // change the user just undid.
    //
    // This function was written enumerating member by member and came up short
    // four times: Undo did not work on Sphere, Capsule, or Plane Collider or
    // on Rigidbody, and only was fixed when someone reported it. Adding the
    // pointer to EditCaches is enough.
    m_caches = EditCaches{};
}

void PropertiesPanel::loadMeshForSelected(EditorContext& ctx, uint64_t ownerId,
                                          const std::string& path)
{
    if (!ctx.assetLoader || !ctx.scene) return;

    // Resolved by id at the time of applying, never by ctx.selected: the Browse
    // dialog is not modal and drains several frames later, with Hierarchy
    // clickable in between. Reading the selection here loaded the FBX on the
    // wrong object. The drop passes through here the same (with the id of the
    // object it is dropped on), where it does not matter because it is immediate.
    GameObject* owner = ctx.scene->findById(ownerId);
    if (!owner)
    {
        // In the log and not silently: the user has navigated an entire dialog and
        // their choice goes nowhere (same criterion as assignMaterialTexture).
        ctx.pushLog("The object that requested the mesh no longer exists; load discarded");
        return;
    }

    // The hasMesh() guard is not enough anymore: while loading is in flight
    // hasMesh() is false, so a second drop would queue a duplicate load and the
    // second result would overwrite the first. pendingMeshJob != 0 cuts it off.
    if (owner->hasMesh() || owner->pendingMeshJob != 0)
        return;

    const std::string ext = std::filesystem::path(path).extension().string();
    if (!ModelLoader::isSupportedModelExtension(ext))
    {
        m_meshLoadError = "Unsupported format: " + ext;
        return;
    }

    // Does not load: queues it. Registration in the Renderer (addSkinnedMesh/
    // addStaticMesh) and setMesh are done by EditorUI::onAssetsLoaded (via
    // applyLoadedMesh) when the worker finishes and the frame pump picks it up.
    owner->pendingMeshJob = ctx.assetLoader->requestMesh(path, owner->id);
    // So that, when it lands, EditorUI knows this load is an edit and stacks
    // its undo (see consumeUserMeshJob).
    m_userMeshJobs[owner->id] = owner->pendingMeshJob;
    m_meshLoadError.clear();
    ctx.pushLog("Loading '" + path + "'...");
}

bool PropertiesPanel::consumeUserMeshJob(uint64_t targetId, uint64_t job)
{
    const auto it = m_userMeshJobs.find(targetId);
    if (it == m_userMeshJobs.end() || it->second != job) return false;
    m_userMeshJobs.erase(it);
    return true;
}

// Snapshot and restoration of AudioClipComponent, in one place: used by
// sliders (command when drag ends), checkboxes (immediate command) and
// Add/Remove of the component. Here at the top and not next to
// drawAudioClipSection because loadAudioClipForSelected, which is right
// below, already needs them.
static AudioClipState audioClipStateOf(const AudioClipComponent& clip)
{
    return AudioClipState{ clip.getVolume(), clip.getPitch(),
                            clip.getMinDistance(), clip.getMaxDistance(),
                            clip.getLoop(), clip.getIs3D(), clip.getPlayOnAwake(),
                            clip.getBus(), clip.getLoadMode(), clip.getRolloff(),
                            clip.getSpread(), clip.getStereoPan(), clip.getDopplerLevel(),
                            clip.getMute() };
}

// Resolves the GameObject by id on each apply, never captures the pointer:
// so it survives an undo of Delete that may have rebuilt the object in the
// meantime.
static void applyAudioClipState(Scene& scene, uint64_t ownerId, const AudioClipState& s)
{
    GameObject* go = scene.findById(ownerId);
    if (!go || !go->hasAudioClip()) return;
    auto& clip = go->getAudioClip();
    // loop and is3D first: reload the sound (they go in FMOD_MODE) and that
    // reload reapplies the component's distances, so writing them before would
    // be wasted work. The two setters are no-op if the value does not change.
    clip->setLoop(s.loop);
    clip->setIs3D(s.is3D);
    clip->setPlayOnAwake(s.playOnAwake);
    clip->setBus(s.bus);
    // Reloads the sound if it changes, like loop/is3D.
    clip->setLoadMode(s.loadMode);
    clip->setRolloff(s.rolloff);
    clip->setSpread(s.spread);
    clip->setStereoPan(s.stereoPan);
    clip->setDopplerLevel(s.dopplerLevel);
    clip->setMute(s.mute);
    clip->setVolume(s.volume);
    clip->setPitch(s.pitch);
    // Max before min: the two setters keep min <= max between them.
    clip->setMaxDistance(s.maxDistance);
    clip->setMinDistance(s.minDistance);
}

void PropertiesPanel::loadAudioClipForSelected(EditorContext& ctx, const std::string& path)
{
    if (!ctx.selected || !ctx.audio || ctx.selected->hasAudioClip())
        return;

    // The list lives in AudioBus.h, shared with scene loading and Lua
    // AddComponent: before it was here alone and other routes accepted any
    // extension.
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (!isSupportedAudioExtension(ext))
    {
        m_audioLoadError = "Unsupported format: " + ext;
        return;
    }

    auto clip = ctx.audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip)
    {
        m_audioLoadError = "Could not load the audio";
        return;
    }
    // The addition goes through undo like the rest of the panel's components.
    // The state is that of the newly created clip (all defaults): what makes
    // the command useful here is not preserving values, it is that Ctrl+Z
    // after an accidental drop removes the component instead of doing nothing.
    if (ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<AudioClipComponentCommand>(
            *ctx.scene, *ctx.audio,
            "Add Audio Clip to '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/true, path, audioClipStateOf(*clip));
        // The component is already created: assigned here and the command only
        // recreates it if a redo is needed. Creating it twice would load the sound
        // twice.
        ctx.selected->setAudioClip(std::move(clip));
        ctx.undo->push(std::move(cmd));
        m_audioLoadError.clear();
        ctx.pushLog("Audio Clip component added to '" + ctx.selected->name + "'");
        return;
    }
    ctx.selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
    ctx.pushLog("Audio Clip component added to '" + ctx.selected->name + "'");
}

void PropertiesPanel::drawAssetDropBox(EditorContext& ctx, const char* idSuffix,
                                       const char* hint,
                                       const std::function<void()>& onBrowse,
                                       const std::function<void(const std::string&)>& onDrop)
{
    // Disabled during scene load, and here inside for all 16 boxes at once.
    // It is half of what COMMUNICATES: what really prevents is
    // acceptOrImportAsset when draining the dialog (a dialog already open
    // survives the modal, so a gray button is not enough). Same distribution
    // as scene IO in Play: disabled widget warns, the function that does the
    // work prevents.
    ImGui::BeginDisabled(ctx.editingLocked);
    if (ImGui::Button((std::string("Browse...##") + idSuffix).c_str()))
        onBrowse();
    ImGui::EndDisabled();

    // No SameLine and 40 px tall: it is the Mesh layout, and the UI boxes
    // each came with theirs (path and button on the same line, child of 34 px).
    // One single place they all come from.
    ImGui::BeginChild((std::string("##DropZone") + idSuffix).c_str(), ImVec2(0, 40), true);
    ImGui::TextDisabled("%s", hint);
    // Same edit veto as Mesh: with the Load Scene modal active, new drops are
    // not accepted.
    if (!ctx.editingLocked && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            onDrop(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
}

void PropertiesPanel::draw(EditorContext& ctx)
{
    if (m_open)
    {
        ImGui::Begin("Properties", &m_open);
        if (!ctx.selected)
        {
            m_caches.props = nullptr;
        }
        else
        {
            // Simulated body (non-kinematic Rigidbody): PhysX moves it localTransform
            // EVERY frame, so it cannot enter the "external change" branch below—it
            // would carry the world values shown by its own branch.
            const bool simulado = ctx.selected->hasAnyCollider() && ctx.selected->hasRigidbody()
                                  && !ctx.selected->getRigidbody()->getIsKinematic();
            const bool seleccionNueva = m_caches.props != ctx.selected;
            // Someone OUTSIDE this panel moved the object without changing the
            // selection: the translation gizmo from the viewport, a script, a Ctrl+Z.
            // Without this the fields stayed at the value from when it was selected
            // and, worse, the next touch to a DragFloat recomposed the matrix from
            // that stale cache and erased the movement.
            const bool movidoDeFuera = !simulado && !m_transformDragActive
                                       && ctx.selected->localTransform != m_transformCached;

            // What is NOT done is recompose every frame: an invalid intermediate value
            // (e.g. scale 0 while typing "0.5") would be re-decomposed and break
            // position/rotation permanently. Hence the `!m_transformDragActive` above,
            // and the saving of m_transformCached next to each matrix write below,
            // which prevents what this same panel writes from being read as external
            // change in the next frame.
            if (seleccionNueva || movidoDeFuera)
            {
                glm::quat orientation;
                // Without looking at what decompose returns, a scale 0 left the quaternion
                // here UNINITIALIZED, and eulerAngles converted it into the 90 degrees
                // that appeared on X and Z.
                decomposeTransform(ctx.selected->localTransform, &m_editPosition,
                                   &orientation, &m_editScale);
                m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
                m_transformCached = ctx.selected->localTransform;
                // Load errors are from the SELECTION, not the transform: moving it with
                // the gizmo cannot make the "could not load the FBX" message disappear.
                if (seleccionNueva)
                {
                    m_caches.props = ctx.selected;
                    m_meshLoadError.clear();
                    m_audioLoadError.clear();
                    m_textureLoadError.clear();
                }
            }
            // PhysX moves worldTransform (and localTransform, see traverse in the main
            // loop) every frame, but this never touches this edit cache—without this
            // refresh, Position/Rotation shown stay frozen at the value from when it was
            // selected, even though the object keeps falling/rotating by physics. Only
            // position+rotation (scale is purely from the editor, physx does not know
            // it); skipped while dragging a slider to not fight the user's drag.
            else if (simulado && !m_transformDragActive)
            {
                glm::quat orientation;
                // Same reason as above: with a singular matrix decompose writes nothing,
                // and without checking it what was shown was garbage.
                decomposeTransform(ctx.selected->worldTransform, &m_editPosition, &orientation);
                m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
            }

            ImGui::Text("%s", ctx.selected->name.empty() ? "GameObject" : ctx.selected->name.c_str());
            ImGui::Separator();

            bool changed = false;
            bool posRotActive = false;
            bool scaleActive = false;
            bool activated = false;
            bool posCommitted = false;
            bool rotCommitted = false;
            bool scaleCommitted = false;

            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
            {
                ImGui::Text("Position");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                posCommitted |= ImGui::IsItemDeactivatedAfterEdit();

                ImGui::Text("Rotation");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("X##2", &m_editRotationDeg.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Y##2", &m_editRotationDeg.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Z##2", &m_editRotationDeg.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
                posRotActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();

                ImGui::Text("Scale   ");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("X##3", &m_editScale.x, 0.005f, 0.001f, +FLT_MAX, "% .3f");
                scaleActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Y##3", &m_editScale.y, 0.005f, 0.001f, +FLT_MAX, "% .3f");
                scaleActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                changed |= ImGui::DragFloat("Z##3", &m_editScale.z, 0.005f, 0.001f, +FLT_MAX, "% .3f");
                scaleActive |= ImGui::IsItemActive();
                activated |= ImGui::IsItemActivated();
                scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();

                ImGui::TreePop();
            }

            m_transformDragActive = posRotActive || scaleActive;

            if (activated)
                m_transformBeforeEdit = ctx.selected->localTransform;

            if (posCommitted)
                ctx.pushLog("Position of '" + ctx.selected->name + "' changed to " + formatVec3(m_editPosition));
            if (rotCommitted)
                ctx.pushLog("Rotation of '" + ctx.selected->name + "' changed to " + formatVec3(m_editRotationDeg));
            if (scaleCommitted)
                ctx.pushLog("Scale of '" + ctx.selected->name + "' changed to " + formatVec3(m_editScale));

            if (changed)
            {
                glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
                glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
                glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
                ctx.selected->localTransform = t * r * s;
                // What this panel just wrote is not an external change: without this line,
                // the next frame would see the matrix different from the cached one and
                // re-decompose, turning a "370" the user is typing in Rotation.X into a "10".
                m_transformCached = ctx.selected->localTransform;

                if (ctx.selected->hasAnyCollider())
                {
                    ctx.selected->updateWorldTransforms(ctx.selected->parent ? ctx.selected->parent->worldTransform
                                                                           : glm::mat4(1.0f));
                    // teleport() (not syncTransform): setGlobalPose works for any actor type
                    // (static, kinematic or dynamic), while syncTransform uses
                    // setKinematicTarget, only valid on kinematic. anyCollider() gives the
                    // only collider (the 4 types are mutually exclusive).
                    if (auto col = ctx.selected->anyCollider())
                        col->teleport(ctx.selected->worldTransform);
                }
            }

            if ((posCommitted || rotCommitted || scaleCommitted) && ctx.scene)
            {
                Scene* scene = ctx.scene;
                uint64_t id = ctx.selected->id;
                glm::mat4 before = m_transformBeforeEdit;
                glm::mat4 after = ctx.selected->localTransform;
                ctx.undo->push(std::make_unique<PropertyCommand<glm::mat4>>(
                    "Transform of '" + ctx.selected->name + "'", before, after,
                    [scene, id](const glm::mat4& t) {
                        GameObject* go = scene->findById(id);
                        if (!go) return;
                        go->localTransform = t;
                        if (go->hasAnyCollider())
                        {
                            go->updateWorldTransforms(go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
                            if (go->hasBoxCollider())
                                go->getBoxCollider()->teleport(go->worldTransform);
                            else if (go->hasSphereCollider())
                                go->getSphereCollider()->teleport(go->worldTransform);
                            else if (go->hasCapsuleCollider())
                                go->getCapsuleCollider()->teleport(go->worldTransform);
                            else if (go->hasPlaneCollider())
                                go->getPlaneCollider()->teleport(go->worldTransform);
                        }
                    }));
            }

            drawBoxColliderSection(ctx);
            drawSphereColliderSection(ctx);
            drawCapsuleColliderSection(ctx);
            drawPlaneColliderSection(ctx);
            drawRigidbodySection(ctx);
            drawCameraSection(ctx);
            drawAnimatorSection(ctx);
            drawMeshSection(ctx);
            drawSsrSection(ctx);
            drawReflectionProbeSection(ctx);
            drawLightSection(ctx);
            drawAudioClipSection(ctx);
            drawAudioListenerSection(ctx);
            drawReverbZoneSection(ctx);
            drawCanvasSection(ctx);
            drawButtonSection(ctx);
            drawTextSection(ctx);
            drawProgressBarSection(ctx);
            drawPanelSection(ctx);
            drawImageSection(ctx);
            drawSliderSection(ctx);
            drawCheckboxSection(ctx);
            drawToggleSection(ctx);
            drawScrollbarSection(ctx);
            drawInputFieldSection(ctx);
            drawDropdownSection(ctx);
            drawScrollViewSection(ctx);
            drawLayoutSection(ctx);
            drawScriptsSection(ctx);
            drawAddComponentButton(ctx);
        }

        ImGui::End();
    }

    drawMeshDialog(ctx);
    drawAudioClipDialog(ctx);
    drawButtonPathDialogs(ctx);
    drawTextPathDialog(ctx);
    drawProgressBarPathDialog(ctx);
    drawPanelPathDialog(ctx);
    drawImagePathDialog(ctx);
    drawSliderPathDialog(ctx);
    drawCheckboxPathDialog(ctx);
    drawTogglePathDialog(ctx);
    drawScrollbarPathDialog(ctx);
    drawInputFieldPathDialog(ctx);
    drawDropdownPathDialog(ctx);
    drawScrollViewPathDialog(ctx);
}

void PropertiesPanel::drawSsrSection(EditorContext& ctx)
{
    // Without a mesh there is no surface to reflect.
    if (!ctx.selected->hasMesh()) return;

    if (!ImGui::TreeNodeEx("Screen Space Reflections", ImGuiTreeNodeFlags_OpenOnArrow))
        return;

    Scene*         scene = ctx.scene;
    const uint64_t id    = ctx.selected->id;

    bool enabled = ctx.selected->ssrEnabled;
    if (ImGui::Checkbox("Enable SSR", &enabled))
    {
        const bool before = ctx.selected->ssrEnabled;
        ctx.selected->ssrEnabled = enabled;
        ctx.pushLog("SSR of '" + ctx.selected->name + "' " +
                    (enabled ? "enabled" : "disabled"));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                "SSR of '" + ctx.selected->name + "'", before, enabled,
                [scene, id](const bool& v) {
                    if (GameObject* go = scene->findById(id)) go->ssrEnabled = v;
                }));
        }
    }

    ImGui::BeginDisabled(!ctx.selected->ssrEnabled);
    // The "before" is read BEFORE drawing the slider: SliderFloat jumps to the
    // value under the cursor in the same frame of the click, so reading it again
    // after would already be the new one and undo would return the click value,
    // not the original.
    const float beforeIntensity = ctx.selected->ssrIntensity;
    float       intensity       = ctx.selected->ssrIntensity;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    // Reflectivity at normal incidence: 1 = mirror from any angle, low values
    // reflect mostly edge-on (polished floor, water).
    if (ImGui::SliderFloat("Reflectivity", &intensity, 0.0f, 1.0f, "%.2f"))
        ctx.selected->ssrIntensity = intensity;

    if (ImGui::IsItemActivated())
    {
        m_ssrDragActive          = true;
        m_ssrDragBeforeIntensity = beforeIntensity;
        m_ssrDragOwnerId         = id;
    }
    // The owner id avoids applying a foreign "before" if the drag was interrupted
    // without commit (e.g. a Ctrl+Z mid-drag rebuilds the GameObject).
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ssrDragActive && m_ssrDragOwnerId == id)
    {
        m_ssrDragActive = false;
        ctx.pushLog("Reflectivity of '" + ctx.selected->name + "' changed to " +
                    std::to_string(ctx.selected->ssrIntensity));
        if (scene && ctx.undo)
        {
            const float after = ctx.selected->ssrIntensity;
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Reflectivity of '" + ctx.selected->name + "'", m_ssrDragBeforeIntensity, after,
                [scene, id](const float& v) {
                    if (GameObject* go = scene->findById(id)) go->ssrIntensity = v;
                }));
        }
    }
    ImGui::EndDisabled();

    ImGui::TreePop();
}

void PropertiesPanel::drawReflectionProbeSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasReflectionProbe()) return;

    if (!ImGui::TreeNodeEx("Reflection Probe", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*                    scene = ctx.scene;
    const uint64_t            id    = ctx.selected->id;
    ReflectionProbeComponent* probe = ctx.selected->getReflectionProbe().get();

    ImGui::TextWrapped("The probe captures the surroundings from this object's position "
                       "and replaces the global IBL for whatever falls inside its radius.");

    // The "before" values are read BEFORE drawing the sliders: SliderFloat jumps to
    // the value under the cursor in the same frame of the click, so reading them
    // again after would already be the new and undo would return the click value.
    const float beforeRadius    = probe->getRadius();
    const float beforeIntensity = probe->getIntensity();

    float radius = beforeRadius;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Radius", &radius, 1.0f, 5000.0f, "%.0f"))
        probe->setRadius(radius);
    if (ImGui::IsItemActivated())
    {
        m_probeDragActive   = true;
        m_probeDragOwnerId  = id;
        m_probeDragBefore   = beforeRadius;
        m_probeDragIsRadius = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_probeDragActive &&
        m_probeDragIsRadius && m_probeDragOwnerId == id)
    {
        m_probeDragActive = false;
        const float after = probe->getRadius();
        ctx.pushLog("Radius of probe '" + ctx.selected->name + "' changed to " +
                    std::to_string(after));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Radius of probe '" + ctx.selected->name + "'", m_probeDragBefore, after,
                [scene, id](const float& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasReflectionProbe()) go->getReflectionProbe()->setRadius(v);
                }));
        }
    }

    float intensity = beforeIntensity;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 4.0f, "%.2f"))
        probe->setIntensity(intensity);
    if (ImGui::IsItemActivated())
    {
        m_probeDragActive   = true;
        m_probeDragOwnerId  = id;
        m_probeDragBefore   = beforeIntensity;
        m_probeDragIsRadius = false;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_probeDragActive &&
        !m_probeDragIsRadius && m_probeDragOwnerId == id)
    {
        m_probeDragActive = false;
        const float after = probe->getIntensity();
        ctx.pushLog("Intensity of probe '" + ctx.selected->name + "' changed to " +
                    std::to_string(after));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Intensity of probe '" + ctx.selected->name + "'", m_probeDragBefore, after,
                [scene, id](const float& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasReflectionProbe()) go->getReflectionProbe()->setIntensity(v);
                }));
        }
    }

    // Baking is an event: the button only QUEUES it. The Renderer executes it at
    // the start of the next frame, which is where it can wait for the GPU.
    if (ctx.renderer)
    {
        if (ImGui::Button("Bake"))
        {
            ctx.renderer->requestProbeBake(id);
            ctx.pushLog("Bake of probe '" + ctx.selected->name + "' queued");
        }
        ImGui::SameLine();
        const float ms = ctx.renderer->probeBakeMs(id);
        if (ms < 0.0f) ImGui::TextUnformatted("not baked");
        else           ImGui::Text("%.2f ms of GPU", ms);
        // From the active backend, not from Vulkan by name: each one stores
        // different resources per probe (H51).
        ImGui::Text("Memory: %.2f MB",
                    (double)ctx.renderer->probeMemoryBytes() / (1024.0 * 1024.0));
    }

    if (ImGui::Button("Remove Reflection Probe"))
    {
        ctx.selected->setReflectionProbe(nullptr);
        m_probeDragActive = false;
        ctx.pushLog("Reflection Probe component removed from '" + ctx.selected->name + "'");
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawReverbZoneSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasReverbZone()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Reverb Zone",
                                          ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##reverbZone");

    if (sectionOpen)
    {
        auto& zone = ctx.selected->getReverbZone();
        ImGui::TextWrapped("Spherical sound ambience: everything inside sounds with this "
                           "reverb. The position comes from the Transform. Several fit in a "
                           "scene, and FMOD blends the ones that overlap.");

        bool enabled = zone->getEnabled();
        if (ImGui::Checkbox("Enabled##reverb", &enabled))
            zone->setEnabled(enabled);

        // Combo by NAME: what is saved in the scene is the string, so reordering
        // this list does not change the environment of any project.
        const auto& presets = AudioManager::reverbPresetNames();
        int current = 0;
        for (int i = 0; i < (int)presets.size(); ++i)
            if (presets[i] == zone->getPreset()) { current = i; break; }
        if (ImGui::BeginCombo("Preset", presets[current].c_str()))
        {
            for (int i = 0; i < (int)presets.size(); ++i)
            {
                const bool selected = (i == current);
                if (ImGui::Selectable(presets[i].c_str(), selected))
                    zone->setPreset(presets[i]);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        float minD = zone->getMinDistance();
        if (ImGui::SliderFloat("Min distance##reverb", &minD, 0.1f, 1000.0f, "%.1f"))
            zone->setMinDistance(minD);
        float maxD = zone->getMaxDistance();
        if (ImGui::SliderFloat("Max distance##reverb", &maxD, 1.0f, 2000.0f, "%.1f"))
            zone->setMaxDistance(maxD);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Inside Min the reverb is at full strength; between Min and Max it fades out. "
                              "Beyond Max there is no effect.");

        ImGui::TreePop();
    }

    if (removeClicked)
    {
        // The FMOD resource is released by the sync of the next frame, which sees
        // this id no longer has a zone (Scene::syncReverbZones -> retainReverbZones).
        ctx.selected->setReverbZone(nullptr);
        ctx.pushLog("Reverb Zone component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawAudioListenerSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasAudioListener()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Audio Listener",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##audioListener");

    if (sectionOpen)
    {
        ImGui::TextWrapped("The scene's 3D audio is heard from here. The position and "
                           "orientation come from this object's Transform (it faces its "
                           "local -Z), not from fields of its own. At most one per scene.");

        bool enabled = ctx.selected->getAudioListener()->getEnabled();
        if (ImGui::Checkbox("Enabled", &enabled))
            ctx.selected->getAudioListener()->setEnabled(enabled);

        ImGui::TreePop();
    }

    if (removeClicked)
    {
        // With the current enabled state in the snapshot: removing a DISABLED
        // listener and undoing has to return it disabled, not enabled.
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<AudioListenerComponentCommand>(
                *ctx.scene, "Remove Audio Listener from '" + ctx.selected->name + "'",
                ctx.selected->id, /*add=*/false,
                ctx.selected->getAudioListener()->getEnabled());
            cmd->execute();
            ctx.undo->push(std::move(cmd));
        }
        else
        {
            ctx.selected->setAudioListener(nullptr);
        }
        ctx.pushLog("Audio Listener component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawCanvasSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasCanvas()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Canvas",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##canvas");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        CanvasComponent* c = ctx.selected->getCanvas().get();
        ImGui::TextWrapped("Root of the 2D UI. The usable area comes from the render, minus the "
                           "safe area insets and cropped to the aspect ratio; that gives a single, "
                           "uniform scale for the whole tree.");

        // Fields are reached through a no-capture accessor (function pointer) and
        // not through member pointer: so the four safe area insets, which live one
        // level deeper, use the SAME helper as the rest.
        using FloatRef = float& (*)(CanvasComponent&);
        using Vec2Ref  = glm::vec2& (*)(CanvasComponent&);
        using EnumSet  = void (*)(CanvasComponent&, int);

        // Combos are committed on the spot (one click = one change), like the
        // Type of the light.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*c, idx);
                const std::string lbl = std::string(label) + " of canvas '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCanvas()) apply(*go->getCanvas(), v);
                        }));
            }
        };

        // Scalars share the usual dance: the "before" is read BEFORE drawing, the
        // session opens in IsItemActivated and commits in IsItemDeactivatedAfterEdit,
        // so an entire drag is ONE undo step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*c);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*c) = v;
            if (ImGui::IsItemActivated())
            {
                m_canvasDragBefore  = before;
                m_canvasDragOwnerId = id;
                m_canvasDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_canvasDragOwnerId == id &&
                m_canvasDragField == label)
            {
                const float after = acc(*c);
                m_canvasDragField = nullptr;
                if (after != m_canvasDragBefore)
                {
                    const std::string lbl = std::string(label) + " of canvas '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_canvasDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCanvas()) acc(*go->getCanvas()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*c);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*c) = v;
            if (ImGui::IsItemActivated())
            {
                m_canvasDragBefore2 = before;
                m_canvasDragOwnerId = id;
                m_canvasDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_canvasDragOwnerId == id &&
                m_canvasDragField == label)
            {
                const glm::vec2 after = acc(*c);
                m_canvasDragField = nullptr;
                if (after != m_canvasDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of canvas '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_canvasDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCanvas()) acc(*go->getCanvas()) = val;
                            }));
                }
            }
        };

        using BoolRef = bool& (*)(CanvasComponent&);
        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*c);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*c) = val;
                const std::string lbl = std::string(label) + " of canvas '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCanvas()) acc(*go->getCanvas()) = v;
                        }));
            }
        };

        static const char* kModes[] = { "Constant Pixel Size", "Scale With Screen Size",
                                        "Constant Physical Size" };
        comboEnum("Scale Mode", (int)c->scaleMode, kModes, IM_ARRAYSIZE(kModes),
                  +[](CanvasComponent& cc, int v) { cc.scaleMode = (UiScaleMode)v; });

        dragFloat("Scale Factor",
                  +[](CanvasComponent& cc) -> float& { return cc.scaleFactor; },
                  0.01f, 0.0f, 100.0f, "%.3f");
        dragVec2("Reference Resolution",
                 +[](CanvasComponent& cc) -> glm::vec2& { return cc.referenceResolution; },
                 1.0f, 0.0f, 16384.0f, "%.0f");

        static const char* kMatches[] = { "Match Width Or Height", "Expand", "Shrink" };
        comboEnum("Screen Match", (int)c->screenMatch, kMatches, IM_ARRAYSIZE(kMatches),
                  +[](CanvasComponent& cc, int v) { cc.screenMatch = (UiScreenMatch)v; });

        dragFloat("Match Width/Height",
                  +[](CanvasComponent& cc) -> float& { return cc.matchWidthOrHeight; },
                  0.01f, 0.0f, 1.0f, "%.2f");
        dragFloat("Screen DPI",
                  +[](CanvasComponent& cc) -> float& { return cc.screenDpi; },
                  1.0f, 0.0f, 2000.0f, "%.1f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = unknown: the Fallback DPI is used");
        dragFloat("Fallback DPI",
                  +[](CanvasComponent& cc) -> float& { return cc.fallbackDpi; },
                  1.0f, 1.0f, 2000.0f, "%.1f");
        dragFloat("Reference DPI",
                  +[](CanvasComponent& cc) -> float& { return cc.referenceDpi; },
                  1.0f, 1.0f, 2000.0f, "%.1f");

        ImGui::Text("Safe Area (real px)");
        dragFloat("Left##safe",
                  +[](CanvasComponent& cc) -> float& { return cc.safeArea.left; },
                  1.0f, 0.0f, 8192.0f, "%.0f");
        dragFloat("Top##safe",
                  +[](CanvasComponent& cc) -> float& { return cc.safeArea.top; },
                  1.0f, 0.0f, 8192.0f, "%.0f");
        dragFloat("Right##safe",
                  +[](CanvasComponent& cc) -> float& { return cc.safeArea.right; },
                  1.0f, 0.0f, 8192.0f, "%.0f");
        dragFloat("Bottom##safe",
                  +[](CanvasComponent& cc) -> float& { return cc.safeArea.bottom; },
                  1.0f, 0.0f, 8192.0f, "%.0f");

        dragFloat("Aspect Ratio",
                  +[](CanvasComponent& cc) -> float& { return cc.aspectRatio; },
                  0.01f, 0.0f, 10.0f, "%.4f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = off. 16/9 = 1.7778");

        ImGui::TextDisabled("Modo");
        static const char* kRenderModes[] = { "Screen Space", "World" };
        comboEnum("Render Mode", (int)c->renderMode, kRenderModes, IM_ARRAYSIZE(kRenderModes),
                  +[](CanvasComponent& x, int v) { x.renderMode = (UiCanvasRenderMode)v; });

        if (c->renderMode == UiCanvasRenderMode::World)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "Ignored in World: Scale Mode, Screen Match, Match,\n"
                               "the three DPI fields, Safe Area and Aspect Ratio.");

            dragFloat("World Scale", +[](CanvasComponent& x) -> float& { return x.worldScale; },
                      0.0001f, 0.0f, 10.0f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("World units per canvas PIXEL.\n"
                                  "A 1920x1080 canvas at 0.001 measures 1.92 x 1.08 units.");

            static const char* kBillboards[] = { "None", "Yaw Only", "Full" };
            comboEnum("Billboard", (int)c->billboard, kBillboards, IM_ARRAYSIZE(kBillboards),
                      +[](CanvasComponent& x, int v) { x.billboard = (UiBillboard)v; });

            checkBox("Depth Test", +[](CanvasComponent& x) -> bool& { return x.depthTest; });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When false it is always drawn on top, through walls");
        }

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<CanvasComponentCommand>(
            *ctx.scene, "Remove Canvas from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getCanvas());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Canvas component removed from '" + ctx.selected->name + "'");
    }
}

const std::vector<std::string>& PropertiesPanel::spriteNamesFor(EditorContext& ctx,
                                                                const std::string& atlasPath)
{
    if (m_spriteNamesValid && m_spriteNamesPath == atlasPath) return m_spriteNames;

    m_spriteNamesPath  = atlasPath;
    m_spriteNamesValid = true;
    m_spriteNames.clear();

    if (atlasPath.empty() || !ctx.renderer) return m_spriteNames;

    // loadUiAtlas caches by path, so this does NOT load a second copy of the
    // atlas that is already being drawn: it returns the same one. And if the
    // path is invalid, the result (empty list) stays cached until the path
    // changes, instead of retrying the file every frame.
    if (const UiTextureAtlas* atlas = ctx.renderer->loadUiAtlas(atlasPath))
        m_spriteNames = atlas->spriteNames();

    return m_spriteNames;
}

void PropertiesPanel::setButtonAssetPath(EditorContext& ctx, uint64_t ownerId, bool isFont,
                                          const std::string& path)
{
    if (path.empty()) return;

    // The file dialog filter already restricts, but a drop arrives with
    // anything: the veto lives HERE, at the point where all sources pass, and not
    // repeated in each box.
    if (!(isFont ? isUiFontPath(path) : isUiAtlasPath(path)))
    {
        m_buttonPathError = std::string("No es ") +
                            (isFont ? "a font (.ttf .otf .ttc): "
                                    : "an image (.png .jpg .jpeg .bmp .tga): ") +
                            std::filesystem::path(path).filename().string();
        ctx.pushLog(m_buttonPathError);
        return;
    }
    m_buttonPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasButton()) return;

    ButtonComponent& b = *go->getButton();
    const std::string before = isFont ? b.fontPath : b.atlasPath;
    if (before == path) return;

    (isFont ? b.fontPath : b.atlasPath) = path;

    const std::string lbl = std::string(isFont ? "Font" : "Atlas") +
                            " of button '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId, isFont](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasButton())
                        (isFont ? g->getButton()->fontPath : g->getButton()->atlasPath) = v;
            }));
}

void PropertiesPanel::drawButtonPathDialogs(EditorContext& ctx)
{
    // Without conditioning on ctx.selected/hasButton(): if not drained here,
    // changing selection with the dialog open leaves the flag stuck at true
    // forever (same reason as drawMeshDialog).
    if (m_fontDlgOpen && m_fontFileDialog->Display("ButtonFontDlg"))
    {
        if (m_fontFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_fontFileDialog->GetFilePathName()))
                setButtonAssetPath(ctx, m_fontDlgOwner, /*isFont=*/true, resolved->string());
        }
        m_fontFileDialog->Close();
        m_fontDlgOpen = false;
    }

    if (m_uiAtlasDlgOpen && m_uiAtlasFileDialog->Display("ButtonAtlasDlg"))
    {
        if (m_uiAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_uiAtlasFileDialog->GetFilePathName()))
                setButtonAssetPath(ctx, m_uiAtlasDlgOwner, /*isFont=*/false, resolved->string());
        }
        m_uiAtlasFileDialog->Close();
        m_uiAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawButtonSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasButton()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Button",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##button");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ButtonComponent* b = ctx.selected->getButton().get();
        ImGui::TextWrapped("2D UI widget. It is drawn in the scene's Canvas tree, "
                           "and its state (Normal/Hover/Pressed/Disabled/Selected) is resolved by the "
                           "canvas itself from the mouse and focus.");

        // Same no-capture accessors (function pointer) as Canvas: so fields inside
        // a struct use the SAME helper as the rest.
        using FloatRef = float&       (*)(ButtonComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ButtonComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ButtonComponent&);
        using StrRef   = std::string& (*)(ButtonComponent&);
        using BoolRef  = bool&        (*)(ButtonComponent&);
        using EnumSet  = void         (*)(ButtonComponent&, int);

        // Combos and checkbox commit on the spot: one click = one change.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*b, idx);
                const std::string lbl = std::string(label) + " of button '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasButton()) apply(*go->getButton(), v);
                        }));
            }
        };

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*b);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*b) = v;
                const std::string lbl = std::string(label) + " of button '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasButton()) acc(*go->getButton()) = val;
                        }));
            }
        };

        // Scalars share the usual dance: "before" read BEFORE drawing, session
        // open in IsItemActivated and commit in IsItemDeactivatedAfterEdit, so an
        // entire drag is ONE undo step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*b);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*b) = v;
            if (ImGui::IsItemActivated())
            {
                m_buttonDragBefore  = before;
                m_buttonDragOwnerId = id;
                m_buttonDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_buttonDragOwnerId == id &&
                m_buttonDragField == label)
            {
                const float after = acc(*b);
                m_buttonDragField = nullptr;
                if (after != m_buttonDragBefore)
                {
                    const std::string lbl = std::string(label) + " of button '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_buttonDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasButton()) acc(*go->getButton()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*b);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*b) = v;
            if (ImGui::IsItemActivated())
            {
                m_buttonDragBefore2 = before;
                m_buttonDragOwnerId = id;
                m_buttonDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_buttonDragOwnerId == id &&
                m_buttonDragField == label)
            {
                const glm::vec2 after = acc(*b);
                m_buttonDragField = nullptr;
                if (after != m_buttonDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of button '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_buttonDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasButton()) acc(*go->getButton()) = val;
                            }));
                }
            }
        };

        // Colors carry alpha (the five states and the text use it to fade), so
        // ColorEdit4 and not 3.
        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*b);
            glm::vec4       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &v.x))
                acc(*b) = v;
            if (ImGui::IsItemActivated())
            {
                m_buttonDragBefore4 = before;
                m_buttonDragOwnerId = id;
                m_buttonDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_buttonDragOwnerId == id &&
                m_buttonDragField == label)
            {
                const glm::vec4 after = acc(*b);
                m_buttonDragField = nullptr;
                if (after != m_buttonDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of button '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_buttonDragBefore4, after,
                            [scene, id, acc](const glm::vec4& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasButton()) acc(*go->getButton()) = val;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one
        // per key: same criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*b);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*b) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_buttonDragBeforeStr = before;
                m_buttonDragOwnerId   = id;
                m_buttonDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_buttonDragOwnerId == id &&
                m_buttonDragField == label)
            {
                const std::string after = acc(*b);
                const std::string prev  = m_buttonDragBeforeStr;
                m_buttonDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of button '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasButton()) acc(*go->getButton()) = val;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar
        // (<atlas>.sprites.json) choose from the list; without it fall back to the
        // text field of always, which still works for a hand-cut atlas and for a
        // scene that already brought a written name.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, b->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*b);

            // Empty is "(whole image)": an atlas without sprite is drawn complete,
            // which is what UiTextureAtlas::uvRect does without a name.
            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected:
            // it is shown at the end marked, and the component keeps saying what it
            // said until the user chooses something else.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                // The orphan is not a destination: choosing it leaves the value as is.
                const std::string after = (idx == 0)                    ? std::string()
                                        : (idx <= (int)nombres.size())  ? nombres[(size_t)idx - 1]
                                                                        : before;
                if (after != before)
                {
                    acc(*b) = after;
                    const std::string lbl = std::string(label) + " of button '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasButton()) acc(*go->getButton()) = val;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min", +[](ButtonComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max", +[](ButtonComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot", +[](ButtonComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position", +[](ButtonComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size", +[](ButtonComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Color", +[](ButtonComponent& c) -> glm::vec4& { return c.color; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Base color of the quad. With Transition = Color Tint or Animation the\n"
                              "state color overrides it; it only applies with Sprite Swap.");
        checkBox("Visible", +[](ButtonComponent& c) -> bool& { return c.visible; });

        // Writable path by hand + the common asset box (drawAssetDropBox: button
        // and drop zone, same layout as Mesh). The extension veto is not here but
        // in setButtonAssetPath, which is where both sources pass through.
        auto assetBox = [&](const char* label, bool isFont, StrRef acc,
                            const char* dlgKey, const char* dlgTitle, const char* filters,
                            const char* hint)
        {
            inputText(label, acc);
            drawAssetDropBox(ctx, label, hint,
                [&]
                {
                    IGFD::FileDialogConfig cfg;
                    cfg.path  = "assets";
                    cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                                ImGuiFileDialogFlags_HideColumnDate |
                                ImGuiFileDialogFlags_DisableThumbnailMode |
                                ImGuiFileDialogFlags_DisablePlaceMode;
                    if (isFont)
                    {
                        m_fontDlgOwner = id;
                        m_fontDlgOpen  = true;
                        m_fontFileDialog->OpenDialog(dlgKey, dlgTitle, filters, cfg);
                    }
                    else
                    {
                        m_uiAtlasDlgOwner = id;
                        m_uiAtlasDlgOpen  = true;
                        m_uiAtlasFileDialog->OpenDialog(dlgKey, dlgTitle, filters, cfg);
                    }
                },
                [&](const std::string& dropped) { setButtonAssetPath(ctx, id, isFont, dropped); });
        };

        ImGui::TextDisabled("Sprite");
        assetBox("Atlas", /*isFont=*/false,
                 +[](ButtonComponent& c) -> std::string& { return c.atlasPath; },
                 "ButtonAtlasDlg", "Choose atlas", ".png,.jpg,.jpeg,.bmp,.tga",
                 "Drop .png/.jpg/.bmp/.tga here");
        // Without an atlas there is nothing to cut, and the disabled button says why
        // better than its absence.
        ImGui::BeginDisabled(b->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...")) ctx.openSpriteEditor(b->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && b->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Sprite", +[](ButtonComponent& c) -> std::string& { return c.sprite; });

        ImGui::TextDisabled("States");
        checkBox("Interactable", +[](ButtonComponent& c) -> bool& { return c.interactable; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false it is painted Disabled and emits no Click");
        checkBox("Selected", +[](ButtonComponent& c) -> bool& { return c.selected; });

        static const char* kTransitions[] = { "Color Tint", "Sprite Swap", "Animation" };
        comboEnum("Transition", (int)b->transition, kTransitions, IM_ARRAYSIZE(kTransitions),
                  +[](ButtonComponent& c, int v) { c.transition = (UiButtonTransition)v; });

        ImGui::TextDisabled("Colors per state (Color Tint / Animation)");
        colorEdit("Normal##col",   +[](ButtonComponent& c) -> glm::vec4& { return c.normalColor; });
        colorEdit("Hover##col",    +[](ButtonComponent& c) -> glm::vec4& { return c.hoverColor; });
        colorEdit("Pressed##col",  +[](ButtonComponent& c) -> glm::vec4& { return c.pressedColor; });
        colorEdit("Disabled##col", +[](ButtonComponent& c) -> glm::vec4& { return c.disabledColor; });
        colorEdit("Selected##col", +[](ButtonComponent& c) -> glm::vec4& { return c.selectedColor; });

        spriteField("Normal##spr",   +[](ButtonComponent& c) -> std::string& { return c.normalSprite; });
        spriteField("Hover##spr",    +[](ButtonComponent& c) -> std::string& { return c.hoverSprite; });
        spriteField("Pressed##spr",  +[](ButtonComponent& c) -> std::string& { return c.pressedSprite; });
        spriteField("Disabled##spr", +[](ButtonComponent& c) -> std::string& { return c.disabledSprite; });
        spriteField("Selected##spr", +[](ButtonComponent& c) -> std::string& { return c.selectedSprite; });

        dragFloat("Fade Duration", +[](ButtonComponent& c) -> float& { return c.fadeDuration; },
                  0.01f, 0.0f, 10.0f, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seconds of the Animation fade. 0 = instant switch");

        ImGui::TextDisabled("Text");
        inputText("Text", +[](ButtonComponent& c) -> std::string& { return c.text; });
        assetBox("Font", /*isFont=*/true,
                 +[](ButtonComponent& c) -> std::string& { return c.fontPath; },
                 "ButtonFontDlg", "Choose font", ".ttf,.otf,.ttc",
                 "Drop .ttf/.otf here");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empty = the project's default font");

        if (!m_buttonPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_buttonPathError.c_str());
        dragFloat("Font Size", +[](ButtonComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Text Color", +[](ButtonComponent& c) -> glm::vec4& { return c.textColor; });

        static const char* kAligns[] = { "Left", "Center", "Right", "Justify" };
        comboEnum("Align", (int)b->textAlign, kAligns, IM_ARRAYSIZE(kAligns),
                  +[](ButtonComponent& c, int v) { c.textAlign = (UiTextAlign)v; });

        static const char* kVAligns[] = { "Top", "Middle", "Bottom" };
        comboEnum("V Align", (int)b->textVAlign, kVAligns, IM_ARRAYSIZE(kVAligns),
                  +[](ButtonComponent& c, int v) { c.textVAlign = (UiTextVAlign)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where the label sits vertically within the button");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ButtonComponentCommand>(
            *ctx.scene, "Remove Button from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getButton());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Button component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setTextFontPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Same veto as Button and for the same reason: the drop and the file dialog
    // pass here, so the filter only goes in once.
    if (!isUiFontPath(path))
    {
        m_textPathError = "Not a font (.ttf .otf .ttc): " +
                          std::filesystem::path(path).filename().string();
        ctx.pushLog(m_textPathError);
        return;
    }
    m_textPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasText()) return;

    TextComponent& t = *go->getText();
    const std::string before = t.fontPath;
    if (before == path) return;

    t.fontPath = path;

    const std::string lbl = "Font of text '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasText()) g->getText()->fontPath = v;
            }));
}

void PropertiesPanel::drawTextPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected/hasText(): if not drained here,
    // changing selection with the dialog open leaves the flag stuck at true.
    if (m_textFontDlgOpen && m_textFontFileDialog->Display("TextFontDlg"))
    {
        if (m_textFontFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_textFontFileDialog->GetFilePathName()))
                setTextFontPath(ctx, m_textFontDlgOwner, resolved->string());
        }
        m_textFontFileDialog->Close();
        m_textFontDlgOpen = false;
    }
}

void PropertiesPanel::drawTextSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasText()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Text",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##text");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        TextComponent* t = ctx.selected->getText().get();
        ImGui::TextWrapped("2D UI label. It is drawn in the scene's Canvas tree. "
                           "The text accepts style tags: <color=#RRGGBB>, <size=N>, <b> and <i>.");

        // Same no-capture accessors (function pointer) and same undo dance as
        // Button section. Labels carry "##txt" because a GameObject can have Button
        // and Text at the same time: two widgets with the SAME ImGui id in the same
        // window share state.
        using FloatRef = float&       (*)(TextComponent&);
        using Vec2Ref  = glm::vec2&   (*)(TextComponent&);
        using Vec4Ref  = glm::vec4&   (*)(TextComponent&);
        using StrRef   = std::string& (*)(TextComponent&);
        using BoolRef  = bool&        (*)(TextComponent&);
        using EnumSet  = void         (*)(TextComponent&, int);

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*t, idx);
                const std::string lbl = std::string(label) + " of text '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasText()) apply(*go->getText(), v);
                        }));
            }
        };

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*t);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*t) = v;
                const std::string lbl = std::string(label) + " of text '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasText()) acc(*go->getText()) = val;
                        }));
            }
        };

        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*t);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*t) = v;
            if (ImGui::IsItemActivated())
            {
                m_textDragBefore  = before;
                m_textDragOwnerId = id;
                m_textDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_textDragOwnerId == id &&
                m_textDragField == label)
            {
                const float after = acc(*t);
                m_textDragField = nullptr;
                if (after != m_textDragBefore)
                {
                    const std::string lbl = std::string(label) + " of text '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_textDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasText()) acc(*go->getText()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*t);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*t) = v;
            if (ImGui::IsItemActivated())
            {
                m_textDragBefore2 = before;
                m_textDragOwnerId = id;
                m_textDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_textDragOwnerId == id &&
                m_textDragField == label)
            {
                const glm::vec2 after = acc(*t);
                m_textDragField = nullptr;
                if (after != m_textDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of text '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_textDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasText()) acc(*go->getText()) = val;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*t);
            glm::vec4       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &v.x))
                acc(*t) = v;
            if (ImGui::IsItemActivated())
            {
                m_textDragBefore4 = before;
                m_textDragOwnerId = id;
                m_textDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_textDragOwnerId == id &&
                m_textDragField == label)
            {
                const glm::vec4 after = acc(*t);
                m_textDragField = nullptr;
                if (after != m_textDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of text '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_textDragBefore4, after,
                            [scene, id, acc](const glm::vec4& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasText()) acc(*go->getText()) = val;
                            }));
                }
            }
        };

        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*t);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*t) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_textDragBeforeStr = before;
                m_textDragOwnerId   = id;
                m_textDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_textDragOwnerId == id &&
                m_textDragField == label)
            {
                const std::string after = acc(*t);
                const std::string prev  = m_textDragBeforeStr;
                m_textDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of text '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasText()) acc(*go->getText()) = val;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##txt", +[](TextComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##txt", +[](TextComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##txt", +[](TextComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##txt", +[](TextComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##txt", +[](TextComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##txt", +[](TextComponent& c) -> bool& { return c.visible; });

        ImGui::TextDisabled("Text");
        inputText("Text##txt", +[](TextComponent& c) -> std::string& { return c.text; });

        // Writable path by hand + the common asset box (drawAssetDropBox). The
        // extension veto is in setTextFontPath, where both sources pass through.
        inputText("Font##txt", +[](TextComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empty = the project's default font");
        drawAssetDropBox(ctx, "txtfont", "Drop .ttf/.otf here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_textFontDlgOwner = id;
                m_textFontDlgOpen  = true;
                m_textFontFileDialog->OpenDialog("TextFontDlg", "Choose font", ".ttf,.otf,.ttc", cfg);
            },
            [&](const std::string& dropped) { setTextFontPath(ctx, id, dropped); });

        if (!m_textPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_textPathError.c_str());

        dragFloat("Font Size##txt", +[](TextComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Color##txt", +[](TextComponent& c) -> glm::vec4& { return c.color; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Glyph fill color");

        static const char* kAligns[] = { "Left", "Center", "Right", "Justify" };
        comboEnum("Align##txt", (int)t->align, kAligns, IM_ARRAYSIZE(kAligns),
                  +[](TextComponent& c, int v) { c.align = (UiTextAlign)v; });

        static const char* kVAligns[] = { "Top", "Middle", "Bottom" };
        comboEnum("V Align##txt", (int)t->vAlign, kVAligns, IM_ARRAYSIZE(kVAligns),
                  +[](TextComponent& c, int v) { c.vAlign = (UiTextVAlign)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where the block of lines sits vertically within the rect");

        static const char* kOverflows[] = { "Overflow", "Clip", "Ellipsis" };
        comboEnum("Overflow##txt", (int)t->overflow, kOverflows, IM_ARRAYSIZE(kOverflows),
                  +[](TextComponent& c, int v) { c.overflow = (UiTextOverflow)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What happens to what does not fit in the rect. Clip splits the batch by scissor");

        checkBox("Word Wrap##txt", +[](TextComponent& c) -> bool& { return c.wordWrap; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Wraps by words against the rect's width. '\\n' always breaks");

        ImGui::TextDisabled("Outline");
        dragFloat("Outline Width##txt", +[](TextComponent& c) -> float& { return c.outlineWidth; },
                  0.05f, 0.0f, 32.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Screen pixels. 0 = no outline");
        colorEdit("Outline Color##txt",
                  +[](TextComponent& c) -> glm::vec4& { return c.outlineColor; });

        ImGui::TextDisabled("Shadow");
        dragVec2("Shadow Offset##txt",
                 +[](TextComponent& c) -> glm::vec2& { return c.shadowOffset; },
                 0.25f, -64.0f, 64.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pixels. At {0,0} (or with alpha 0) not a single shadow quad is emitted");
        colorEdit("Shadow Color##txt",
                  +[](TextComponent& c) -> glm::vec4& { return c.shadowColor; });

        ImGui::TextDisabled("Style of the <b> and <i> tags");
        dragFloat("Bold Strength##txt", +[](TextComponent& c) -> float& { return c.boldStrength; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Italic Skew##txt", +[](TextComponent& c) -> float& { return c.italicSkew; },
                  0.01f, -1.0f, 1.0f, "%.3f");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<TextComponentCommand>(
            *ctx.scene, "Remove Text from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getText());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Text component removed from '" + ctx.selected->name + "'");
    }
}

namespace
{
    // The three image paths of the bar, in the same order as the `field` that
    // goes through the file dialog and the drop. A no-capture accessor so the
    // undo applier (which survives selection change) depends on nothing.
    std::string& barImagePathRef(ProgressBarComponent& c, int field)
    {
        if (field == 1) return c.backgroundPath;
        if (field == 2) return c.fillPath;
        return c.atlasPath;
    }

    const char* barImageFieldLabel(int field)
    {
        if (field == 1) return "Background image of bar '";
        if (field == 2) return "Fill image of bar '";
        return "Atlas of bar '";
    }
}

void PropertiesPanel::setProgressBarImagePath(EditorContext& ctx, uint64_t ownerId, int field,
                                               const std::string& path)
{
    // Same veto as Button and Text, and for the same reason: the drop and
    // file dialog of the THREE boxes pass here, so the filter only goes once.
    if (!isUiAtlasPath(path))
    {
        m_barPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_barPathError);
        return;
    }
    m_barPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasProgressBar()) return;

    ProgressBarComponent& p = *go->getProgressBar();
    const std::string before = barImagePathRef(p, field);
    if (before == path) return;

    barImagePathRef(p, field) = path;

    const std::string lbl = std::string(barImageFieldLabel(field)) + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId, field](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasProgressBar())
                        barImagePathRef(*g->getProgressBar(), field) = v;
            }));
}

void PropertiesPanel::drawProgressBarPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected/hasProgressBar(): if not drained
    // here, changing selection with the dialog open leaves the flag stuck.
    if (m_barAtlasDlgOpen && m_barAtlasFileDialog->Display("BarImageDlg"))
    {
        if (m_barAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_barAtlasFileDialog->GetFilePathName()))
                setProgressBarImagePath(ctx, m_barAtlasDlgOwner, m_barAtlasDlgField, resolved->string());
        }
        m_barAtlasFileDialog->Close();
        m_barAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawProgressBarSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasProgressBar()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Progress Bar",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##bar");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ProgressBarComponent* p = ctx.selected->getProgressBar().get();
        ImGui::TextWrapped("2D UI progress bar. It is drawn in the Canvas tree "
                           "as background + fill; the fill comes from value against [min, max].");

        // Same no-capture accessors (function pointer) and same undo dance as
        // Button and Text sections. Labels carry "##bar" because a GameObject can
        // have all three components at the same time: two widgets with the SAME
        // ImGui id share state.
        using FloatRef = float&       (*)(ProgressBarComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ProgressBarComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ProgressBarComponent&);
        using StrRef   = std::string& (*)(ProgressBarComponent&);
        using BoolRef  = bool&        (*)(ProgressBarComponent&);
        using EnumSet  = void         (*)(ProgressBarComponent&, int);

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*p, idx);
                const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasProgressBar()) apply(*go->getProgressBar(), v);
                        }));
            }
        };

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*p);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*p) = v;
                const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasProgressBar()) acc(*go->getProgressBar()) = val;
                        }));
            }
        };

        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*p);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*p) = v;
            if (ImGui::IsItemActivated())
            {
                m_barDragBefore  = before;
                m_barDragOwnerId = id;
                m_barDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_barDragOwnerId == id &&
                m_barDragField == label)
            {
                const float after = acc(*p);
                m_barDragField = nullptr;
                if (after != m_barDragBefore)
                {
                    const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_barDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasProgressBar()) acc(*go->getProgressBar()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*p);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*p) = v;
            if (ImGui::IsItemActivated())
            {
                m_barDragBefore2 = before;
                m_barDragOwnerId = id;
                m_barDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_barDragOwnerId == id &&
                m_barDragField == label)
            {
                const glm::vec2 after = acc(*p);
                m_barDragField = nullptr;
                if (after != m_barDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_barDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasProgressBar()) acc(*go->getProgressBar()) = val;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*p);
            glm::vec4       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &v.x))
                acc(*p) = v;
            if (ImGui::IsItemActivated())
            {
                m_barDragBefore4 = before;
                m_barDragOwnerId = id;
                m_barDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_barDragOwnerId == id &&
                m_barDragField == label)
            {
                const glm::vec4 after = acc(*p);
                m_barDragField = nullptr;
                if (after != m_barDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_barDragBefore4, after,
                            [scene, id, acc](const glm::vec4& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasProgressBar()) acc(*go->getProgressBar()) = val;
                            }));
                }
            }
        };

        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*p);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*p) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_barDragBeforeStr = before;
                m_barDragOwnerId   = id;
                m_barDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_barDragOwnerId == id &&
                m_barDragField == label)
            {
                const std::string after = acc(*p);
                const std::string prev  = m_barDragBeforeStr;
                m_barDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of bar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasProgressBar()) acc(*go->getProgressBar()) = val;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##bar",
                 +[](ProgressBarComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##bar",
                 +[](ProgressBarComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##bar", +[](ProgressBarComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##bar",
                 +[](ProgressBarComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##bar", +[](ProgressBarComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##bar", +[](ProgressBarComponent& c) -> bool& { return c.visible; });

        ImGui::TextDisabled("Value");
        // Without a ceiling by min/max on purpose: the component does not clamp
        // anything (the sync normalizes it), and the range can come from a script.
        dragFloat("Value##bar", +[](ProgressBarComponent& c) -> float& { return c.value; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        dragFloat("Min##bar", +[](ProgressBarComponent& c) -> float& { return c.minValue; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        dragFloat("Max##bar", +[](ProgressBarComponent& c) -> float& { return c.maxValue; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("With Max <= Min the bar comes out empty: there is no way to split "
                              "an empty interval");

        static const char* kDirs[] = { "Left To Right", "Right To Left",
                                       "Bottom To Top", "Top To Bottom" };
        comboEnum("Fill Direction##bar", (int)p->fillDirection, kDirs, IM_ARRAYSIZE(kDirs),
                  +[](ProgressBarComponent& c, int v) {
                      c.fillDirection = (UiProgressFillDirection)v;
                  });

        ImGui::TextDisabled("Colors");
        colorEdit("Background Color##bar",
                  +[](ProgressBarComponent& c) -> glm::vec4& { return c.color; });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("BACKGROUND color (the whole rect)");
        colorEdit("Fill Color##bar",
                  +[](ProgressBarComponent& c) -> glm::vec4& { return c.fillColor; });

        ImGui::TextDisabled("Images");
        // One writable path per field + the common asset box (drawAssetDropBox).
        // The extension veto is in setProgressBarImagePath, where the three fields
        // and two sources pass through.
        auto assetBox = [&](const char* label, int field, StrRef acc,
                            const char* idSuffix, const char* tip)
        {
            inputText(label, acc);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            // The idSuffix is unique per field: two boxes with the same id would
            // share the pressed state of the button.
            drawAssetDropBox(ctx, idSuffix, "Drop .png/.jpg here",
                [&]
                {
                    IGFD::FileDialogConfig cfg;
                    cfg.path  = "assets";
                    cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                                ImGuiFileDialogFlags_HideColumnDate |
                                ImGuiFileDialogFlags_DisableThumbnailMode |
                                ImGuiFileDialogFlags_DisablePlaceMode;
                    m_barAtlasDlgOwner = id;
                    m_barAtlasDlgField = field;
                    m_barAtlasDlgOpen  = true;
                    m_barAtlasFileDialog->OpenDialog("BarImageDlg", "Choose image",
                                                     ".png,.jpg,.jpeg,.bmp,.tga", cfg);
                },
                [&](const std::string& dropped)
                {
                    setProgressBarImagePath(ctx, id, field, dropped);
                });
        };

        assetBox("Atlas##bar", 0,
                 +[](ProgressBarComponent& c) -> std::string& { return c.atlasPath; },
                 "barAtlas",
                 "Shared image: used by the parts that do not bring their own");
        assetBox("Background Image##bar", 1,
                 +[](ProgressBarComponent& c) -> std::string& { return c.backgroundPath; },
                 "barBg",
                 "Empty = the Atlas; and without one, a Background Color quad");
        assetBox("Fill Image##bar", 2,
                 +[](ProgressBarComponent& c) -> std::string& { return c.fillPath; },
                 "barFill",
                 "Empty = the Atlas; and without one, a Fill Color quad");

        if (!m_barPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_barPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ProgressBarComponentCommand>(
            *ctx.scene, "Remove Progress Bar from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getProgressBar());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Progress Bar component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawLayoutSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasLayout()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Layout",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##layout");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        LayoutComponent* l = ctx.selected->getLayout().get();
        // The rect is only its own when there is no other UI component on the
        // object: with one, that one commands the rect and these fields are not read.
        // It is STATED instead of hidden: the editor does not gate what the engine supports.
        const bool ownsRect = !ctx.selected->hasButton() && !ctx.selected->hasText() &&
                              !ctx.selected->hasProgressBar();

        ImGui::TextWrapped("2D UI auto-layout: places this object's CHILDREN. Without "
                           "another UI component here, the container is a rect of its own that "
                           "groups and clips without being drawn.");

        // Same no-capture accessors (function pointer) and same undo dance as Button,
        // Text and ProgressBar sections. Labels carry "##layout" because a
        // GameObject can have all four components at the same time: two widgets
        // with the SAME ImGui id share state.
        using FloatRef = float&     (*)(LayoutComponent&);
        using Vec2Ref  = glm::vec2& (*)(LayoutComponent&);
        using BoolRef  = bool&      (*)(LayoutComponent&);
        using EnumSet  = void       (*)(LayoutComponent&, int);

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*l, idx);
                const std::string lbl = std::string(label) + " of layout '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasLayout()) apply(*go->getLayout(), v);
                        }));
            }
        };

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*l);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*l) = v;
                const std::string lbl = std::string(label) + " of layout '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasLayout()) acc(*go->getLayout()) = val;
                        }));
            }
        };

        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*l);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*l) = v;
            if (ImGui::IsItemActivated())
            {
                m_layoutDragBefore  = before;
                m_layoutDragOwnerId = id;
                m_layoutDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_layoutDragOwnerId == id &&
                m_layoutDragField == label)
            {
                const float after = acc(*l);
                m_layoutDragField = nullptr;
                if (after != m_layoutDragBefore)
                {
                    const std::string lbl = std::string(label) + " of layout '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_layoutDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasLayout()) acc(*go->getLayout()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*l);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*l) = v;
            if (ImGui::IsItemActivated())
            {
                m_layoutDragBefore2 = before;
                m_layoutDragOwnerId = id;
                m_layoutDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_layoutDragOwnerId == id &&
                m_layoutDragField == label)
            {
                const glm::vec2 after = acc(*l);
                m_layoutDragField = nullptr;
                if (after != m_layoutDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of layout '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_layoutDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasLayout()) acc(*go->getLayout()) = val;
                            }));
                }
            }
        };

        // Columns are an INTEGER, not a float with formatting: a DragInt prevents
        // 3.4 columns from reaching the grid by the rounding path.
        auto dragColumns = [&](const char* label)
        {
            const int before = (int)l->columns;
            int       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt(label, &v, 0.1f, 0, 512))
                l->columns = (uint32_t)(v < 0 ? 0 : v);
            if (ImGui::IsItemActivated())
            {
                m_layoutDragBefore  = (float)before;
                m_layoutDragOwnerId = id;
                m_layoutDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_layoutDragOwnerId == id &&
                m_layoutDragField == label)
            {
                const int after = (int)l->columns;
                m_layoutDragField = nullptr;
                if (after != (int)m_layoutDragBefore)
                {
                    const std::string lbl = std::string(label) + " of layout '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_layoutDragBefore, after,
                            [scene, id](const int& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasLayout())
                                        go->getLayout()->columns = (uint32_t)(val < 0 ? 0 : val);
                            }));
                }
            }
        };

        static const char* kModos[] = { "None", "Horizontal", "Vertical", "Grid" };
        comboEnum("Mode##layout", (int)l->mode, kModos, IM_ARRAYSIZE(kModos),
                  +[](LayoutComponent& c, int v) { c.mode = (UiLayoutMode)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("None = only groups and clips; the children anchor on their own");

        ImGui::TextDisabled("Container rect");
        if (!ownsRect)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "The rect is driven by this object's other UI component");
        dragVec2("Anchor Min##layout",
                 +[](LayoutComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##layout",
                 +[](LayoutComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##layout",
                 +[](LayoutComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##layout", +[](LayoutComponent& c) -> bool& { return c.visible; });

        ImGui::TextDisabled("Padding");
        dragFloat("Left##layout", +[](LayoutComponent& c) -> float& { return c.paddingLeft; },
                  1.0f, 0.0f, 16384.0f, "%.0f");
        dragFloat("Right##layout", +[](LayoutComponent& c) -> float& { return c.paddingRight; },
                  1.0f, 0.0f, 16384.0f, "%.0f");
        dragFloat("Top##layout", +[](LayoutComponent& c) -> float& { return c.paddingTop; },
                  1.0f, 0.0f, 16384.0f, "%.0f");
        dragFloat("Bottom##layout", +[](LayoutComponent& c) -> float& { return c.paddingBottom; },
                  1.0f, 0.0f, 16384.0f, "%.0f");

        ImGui::TextDisabled("Placement");
        dragVec2("Spacing##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.spacing; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("X between columns, Y between rows");

        static const char* kAlin[] = { "Start", "Center", "End" };
        comboEnum("Cross Align##layout", (int)l->crossAlign, kAlin, IM_ARRAYSIZE(kAlin),
                  +[](LayoutComponent& c, int v) { c.crossAlign = (UiCrossAlign)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Alignment on the CROSS axis. Grid does not use it: the cell "
                              "already fixes both axes");

        // The two grid fields are always shown, disabled outside of Grid: hiding
        // them would change the SHAPE of the panel when toggling the mode, and
        // that is exactly what makes a field seem lost.
        const bool esGrid = l->mode == UiLayoutMode::Grid;
        if (!esGrid) ImGui::BeginDisabled();
        dragVec2("Cell Size##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.cellSize; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        dragColumns("Columns##layout");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = as many as fit in the container's width");
        if (!esGrid) ImGui::EndDisabled();

        ImGui::TextDisabled("Content size fitter");
        checkBox("Fit Width##layout", +[](LayoutComponent& c) -> bool& { return c.fitWidth; });
        checkBox("Fit Height##layout", +[](LayoutComponent& c) -> bool& { return c.fitHeight; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("That Size axis becomes the extent of the placed children "
                              "plus the padding");

        ImGui::TextDisabled("This object inside its parent's layout");
        checkBox("Ignore Layout##layout",
                 +[](LayoutComponent& c) -> bool& { return c.ignoreLayout; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Anchors on its own and does NOT take a slot in the parent's layout");
        checkBox("Clip Children##layout",
                 +[](LayoutComponent& c) -> bool& { return c.clipChildren; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clips the descendants against this rect (INTERSECTED with "
                              "the clipping coming from the parent)");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<LayoutComponentCommand>(
            *ctx.scene, "Remove Layout from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getLayout());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Layout component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setPanelAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                         const std::string& path)
{
    // Same veto as Button, Text and the bar, and for the same reason: the drop
    // and file dialog pass here, so the filter goes once.
    if (!isUiAtlasPath(path))
    {
        m_panelPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                           std::filesystem::path(path).filename().string();
        ctx.pushLog(m_panelPathError);
        return;
    }
    m_panelPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasPanel()) return;

    PanelComponent& p = *go->getPanel();
    const std::string before = p.atlasPath;
    if (before == path) return;

    p.atlasPath = path;

    const std::string lbl = "Atlas of panel '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasPanel()) g->getPanel()->atlasPath = v;
            }));
}

void PropertiesPanel::drawPanelPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected/hasPanel(): if not drained here,
    // changing selection with the dialog open leaves the flag stuck at true
    // forever (same reason as drawMeshDialog).
    if (m_panelAtlasDlgOpen && m_panelAtlasFileDialog->Display("PanelAtlasDlg"))
    {
        if (m_panelAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_panelAtlasFileDialog->GetFilePathName()))
                setPanelAtlasPath(ctx, m_panelAtlasDlgOwner, resolved->string());
        }
        m_panelAtlasFileDialog->Close();
        m_panelAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawPanelSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasPanel()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Panel",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##panel");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        PanelComponent* p = ctx.selected->getPanel().get();
        ImGui::TextWrapped("2D UI background rectangle. Without an atlas it is a flat color "
                           "quad; with an atlas, the sprite stretched to the rect.");

        using Vec2Ref = glm::vec2&   (*)(PanelComponent&);
        using Vec4Ref = glm::vec4&   (*)(PanelComponent&);
        using StrRef  = std::string& (*)(PanelComponent&);
        using BoolRef = bool&        (*)(PanelComponent&);

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*p);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*p) = v;
                const std::string lbl = std::string(label) + " of panel '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasPanel()) acc(*go->getPanel()) = val;
                        }));
            }
        };

        // "before" read BEFORE drawing, session open in IsItemActivated and commit
        // in IsItemDeactivatedAfterEdit: an entire drag is ONE undo step, not one
        // per frame.
        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*p);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*p) = v;
            if (ImGui::IsItemActivated())
            {
                m_panelDragBefore2 = before;
                m_panelDragOwnerId = id;
                m_panelDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_panelDragOwnerId == id &&
                m_panelDragField == label)
            {
                const glm::vec2 after = acc(*p);
                m_panelDragField = nullptr;
                if (after != m_panelDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of panel '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_panelDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasPanel()) acc(*go->getPanel()) = val;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*p);
            glm::vec4       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &v.x))
                acc(*p) = v;
            if (ImGui::IsItemActivated())
            {
                m_panelDragBefore4 = before;
                m_panelDragOwnerId = id;
                m_panelDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_panelDragOwnerId == id &&
                m_panelDragField == label)
            {
                const glm::vec4 after = acc(*p);
                m_panelDragField = nullptr;
                if (after != m_panelDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of panel '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_panelDragBefore4, after,
                            [scene, id, acc](const glm::vec4& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasPanel()) acc(*go->getPanel()) = val;
                            }));
                }
            }
        };

        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*p);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*p) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_panelDragBeforeStr = before;
                m_panelDragOwnerId   = id;
                m_panelDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_panelDragOwnerId == id &&
                m_panelDragField == label)
            {
                const std::string after = acc(*p);
                const std::string prev  = m_panelDragBeforeStr;
                m_panelDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of panel '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasPanel()) acc(*go->getPanel()) = val;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar
        // (<atlas>.sprites.json) choose from the list; without it fall back to the
        // text field, which still works for a hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, p->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*p);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected:
            // it is shown at the end marked.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*p) = after;
                    const std::string lbl = std::string(label) + " of panel '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasPanel()) acc(*go->getPanel()) = val;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##panel",
                 +[](PanelComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##panel",
                 +[](PanelComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##panel", +[](PanelComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##panel",
                 +[](PanelComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##panel", +[](PanelComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Color##panel", +[](PanelComponent& c) -> glm::vec4& { return c.color; });
        checkBox("Visible##panel", +[](PanelComponent& c) -> bool& { return c.visible; });
        checkBox("Raycast Target##panel",
                 +[](PanelComponent& c) -> bool& { return c.raycastTarget; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false the panel lets the mouse through to whatever is behind it.\n"
                              "A full-screen background with this on swallows ALL the\n"
                              "clicks, and nothing on screen gives it away.");

        ImGui::TextDisabled("Sprite");
        inputText("Atlas##panel", +[](PanelComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "panelAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_panelAtlasDlgOwner = id;
                m_panelAtlasDlgOpen  = true;
                m_panelAtlasFileDialog->OpenDialog("PanelAtlasDlg", "Choose atlas",
                                                   ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setPanelAtlasPath(ctx, id, dropped); });

        // Without an atlas there is nothing to cut, and the disabled button says why
        // better than its absence.
        ImGui::BeginDisabled(p->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##panel")) ctx.openSpriteEditor(p->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && p->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Sprite##panel", +[](PanelComponent& c) -> std::string& { return c.sprite; });

        if (!m_panelPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_panelPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<PanelComponentCommand>(
            *ctx.scene, "Remove Panel from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getPanel());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Panel component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setImageAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                         const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_imagePathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                           std::filesystem::path(path).filename().string();
        ctx.pushLog(m_imagePathError);
        return;
    }
    m_imagePathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasImage()) return;

    ImageComponent& im = *go->getImage();
    const std::string before = im.atlasPath;
    if (before == path) return;

    im.atlasPath = path;

    const std::string lbl = "Atlas of image '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasImage()) g->getImage()->atlasPath = v;
            }));
}

void PropertiesPanel::drawImagePathDialog(EditorContext& ctx)
{
    if (m_imageAtlasDlgOpen && m_imageAtlasFileDialog->Display("ImageAtlasDlg"))
    {
        if (m_imageAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_imageAtlasFileDialog->GetFilePathName()))
                setImageAtlasPath(ctx, m_imageAtlasDlgOwner, resolved->string());
        }
        m_imageAtlasFileDialog->Close();
        m_imageAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawImageSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasImage()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Image",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##image");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ImageComponent* im = ctx.selected->getImage().get();
        ImGui::TextWrapped("2D UI sprite. The four modes are resolved on the CPU inside the "
                           "batcher (N quads from the same atlas and the same scissor).");

        using FloatRef = float&       (*)(ImageComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ImageComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ImageComponent&);
        using StrRef   = std::string& (*)(ImageComponent&);
        using BoolRef  = bool&        (*)(ImageComponent&);
        using EnumSet  = void         (*)(ImageComponent&, int);

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*im, idx);
                const std::string lbl = std::string(label) + " of image '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasImage()) apply(*go->getImage(), v);
                        }));
            }
        };

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*im);
            bool       v      = before;
            if (ImGui::Checkbox(label, &v) && v != before)
            {
                acc(*im) = v;
                const std::string lbl = std::string(label) + " of image '" + owner + "'";
                ctx.pushLog(lbl + (v ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasImage()) acc(*go->getImage()) = val;
                        }));
            }
        };

        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*im);
            float       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &v, speed, lo, hi, fmt))
                acc(*im) = v;
            if (ImGui::IsItemActivated())
            {
                m_imageDragBefore  = before;
                m_imageDragOwnerId = id;
                m_imageDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_imageDragOwnerId == id &&
                m_imageDragField == label)
            {
                const float after = acc(*im);
                m_imageDragField = nullptr;
                if (after != m_imageDragBefore)
                {
                    const std::string lbl = std::string(label) + " of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_imageDragBefore, after,
                            [scene, id, acc](const float& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage()) acc(*go->getImage()) = val;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*im);
            glm::vec2       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt))
                acc(*im) = v;
            if (ImGui::IsItemActivated())
            {
                m_imageDragBefore2 = before;
                m_imageDragOwnerId = id;
                m_imageDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_imageDragOwnerId == id &&
                m_imageDragField == label)
            {
                const glm::vec2 after = acc(*im);
                m_imageDragField = nullptr;
                if (after != m_imageDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_imageDragBefore2, after,
                            [scene, id, acc](const glm::vec2& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage()) acc(*go->getImage()) = val;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*im);
            glm::vec4       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &v.x))
                acc(*im) = v;
            if (ImGui::IsItemActivated())
            {
                m_imageDragBefore4 = before;
                m_imageDragOwnerId = id;
                m_imageDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_imageDragOwnerId == id &&
                m_imageDragField == label)
            {
                const glm::vec4 after = acc(*im);
                m_imageDragField = nullptr;
                if (after != m_imageDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_imageDragBefore4, after,
                            [scene, id, acc](const glm::vec4& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage()) acc(*go->getImage()) = val;
                            }));
                }
            }
        };

        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*im);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*im) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_imageDragBeforeStr = before;
                m_imageDragOwnerId   = id;
                m_imageDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_imageDragOwnerId == id &&
                m_imageDragField == label)
            {
                const std::string after = acc(*im);
                const std::string prev  = m_imageDragBeforeStr;
                m_imageDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage()) acc(*go->getImage()) = val;
                            }));
                }
            }
        };

        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, im->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*im);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*im) = after;
                    const std::string lbl = std::string(label) + " of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage()) acc(*go->getImage()) = val;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##image",
                 +[](ImageComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##image",
                 +[](ImageComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##image", +[](ImageComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##image",
                 +[](ImageComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##image", +[](ImageComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Color##image", +[](ImageComponent& c) -> glm::vec4& { return c.color; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sprite tint (multiplied)");
        checkBox("Visible##image", +[](ImageComponent& c) -> bool& { return c.visible; });
        checkBox("Raycast Target##image",
                 +[](ImageComponent& c) -> bool& { return c.raycastTarget; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false the image lets the mouse through to whatever is behind it");

        ImGui::TextDisabled("Sprite");
        inputText("Atlas##image", +[](ImageComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "imageAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_imageAtlasDlgOwner = id;
                m_imageAtlasDlgOpen  = true;
                m_imageAtlasFileDialog->OpenDialog("ImageAtlasDlg", "Choose atlas",
                                                   ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setImageAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(im->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##image")) ctx.openSpriteEditor(im->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && im->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Sprite##image", +[](ImageComponent& c) -> std::string& { return c.sprite; });

        if (!m_imagePathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_imagePathError.c_str());

        // The three blocks below are ALWAYS shown, not only the one of the active
        // mode: the fields are of the component and stay there when changing mode,
        // and hiding them would make it seem they are lost.
        ImGui::TextDisabled("Modo");
        static const char* kModes[] = { "Normal", "Tiled", "Sliced", "Filled" };
        comboEnum("Mode##image", (int)im->mode, kModes, IM_ARRAYSIZE(kModes),
                  +[](ImageComponent& c, int v) { c.mode = (UiImageMode)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Normal: the sprite stretched to the rect.\n"
                              "Tiled: repeated at its native size.\n"
                              "Sliced: 9-slice, the corners do not stretch.\n"
                              "Filled: only a fraction of the rect.");

        ImGui::TextDisabled("Sliced (borders in SPRITE pixels)");
        dragFloat("Border Left##image",
                  +[](ImageComponent& c) -> float& { return c.borderLeft; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        dragFloat("Border Right##image",
                  +[](ImageComponent& c) -> float& { return c.borderRight; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        dragFloat("Border Top##image",
                  +[](ImageComponent& c) -> float& { return c.borderTop; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        dragFloat("Border Bottom##image",
                  +[](ImageComponent& c) -> float& { return c.borderBottom; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        checkBox("Fill Center##image", +[](ImageComponent& c) -> bool& { return c.fillCenter; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Without the center you get 8 quads: what a frame that shows\n"
                              "what is behind it wants");

        ImGui::TextDisabled("Tiled");
        {
            // maxTiles is a uint32 and there is no dragUint: edited as an integer with
            // the same undo dance as the rest.
            const int before = (int)im->maxTiles;
            int       v      = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt("Max Tiles##image", &v, 8.0f, 0, 65536))
                im->maxTiles = (uint32_t)(v < 0 ? 0 : v);
            if (ImGui::IsItemActivated())
            {
                m_imageDragBefore  = (float)before;
                m_imageDragOwnerId = id;
                m_imageDragField   = "Max Tiles##image";
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_imageDragOwnerId == id &&
                m_imageDragField == std::string("Max Tiles##image"))
            {
                const int after = (int)im->maxTiles;
                m_imageDragField = nullptr;
                if (after != (int)m_imageDragBefore)
                {
                    const std::string lbl = "Max Tiles of image '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_imageDragBefore, after,
                            [scene, id](const int& val) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasImage())
                                        go->getImage()->maxTiles = (uint32_t)(val < 0 ? 0 : val);
                            }));
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hard quad cap. Past the cap, the element is drawn as\n"
                              "Normal instead of blowing up the vertex buffer.");

        ImGui::TextDisabled("Filled");
        static const char* kFillDirs[] = { "Horizontal", "Vertical" };
        comboEnum("Fill Direction##image", (int)im->fillDirection, kFillDirs,
                  IM_ARRAYSIZE(kFillDirs),
                  +[](ImageComponent& c, int v) { c.fillDirection = (UiFillDirection)v; });
        static const char* kFillOrigins[] = { "Start", "End" };
        comboEnum("Fill Origin##image", (int)im->fillOrigin, kFillOrigins,
                  IM_ARRAYSIZE(kFillOrigins),
                  +[](ImageComponent& c, int v) { c.fillOrigin = (UiFillOrigin)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Start is left in Horizontal and top in Vertical");
        dragFloat("Fill Amount##image",
                  +[](ImageComponent& c) -> float& { return c.fillAmount; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("At 0 not a single quad is emitted");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ImageComponentCommand>(
            *ctx.scene, "Remove Image from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getImage());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Image component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setSliderAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Same veto as the rest of UI components, and for the same reason: the
    // drop and file dialog pass here, so the filter goes once.
    if (!isUiAtlasPath(path))
    {
        m_sliderPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_sliderPathError);
        return;
    }
    m_sliderPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasSlider()) return;

    SliderComponent& c = *go->getSlider();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of slider '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasSlider()) g->getSlider()->atlasPath = v;
            }));
}

void PropertiesPanel::drawSliderPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing
    // selection with the dialog open leaves the flag stuck at true forever.
    if (m_sliderAtlasDlgOpen && m_sliderAtlasFileDialog->Display("SliderAtlasDlg"))
    {
        if (m_sliderAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_sliderAtlasFileDialog->GetFilePathName()))
                setSliderAtlasPath(ctx, m_sliderAtlasDlgOwner, resolved->string());
        }
        m_sliderAtlasFileDialog->Close();
        m_sliderAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawSliderSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasSlider()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Slider",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##slider");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        SliderComponent* sl = ctx.selected->getSlider().get();
        ImGui::TextWrapped("Draggable value widget. The whole track is a click zone; the handle is subtracted from the travel so it never sticks out of the ends.");

        using FloatRef = float&       (*)(SliderComponent&);
        using Vec2Ref  = glm::vec2&   (*)(SliderComponent&);
        using Vec4Ref  = glm::vec4&   (*)(SliderComponent&);
        using StrRef   = std::string& (*)(SliderComponent&);
        using BoolRef  = bool&        (*)(SliderComponent&);
        using EnumSet  = void         (*)(SliderComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        // Combos and checkbox commit on the spot: one click = one change.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sl, idx);
                const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasSlider()) apply(*go->getSlider(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*sl);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*sl) = val;
                const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasSlider()) acc(*go->getSlider()) = v;
                        }));
            }
        };

        // Scalars share the usual dance: "before" read BEFORE drawing, session
        // open in IsItemActivated and commit in IsItemDeactivatedAfterEdit, so an
        // entire drag is ONE undo step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*sl);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*sl) = val;
            if (ImGui::IsItemActivated())
            {
                m_sliderDragBefore  = before;
                m_sliderDragOwnerId = id;
                m_sliderDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_sliderDragOwnerId == id &&
                m_sliderDragField == label)
            {
                const float after = acc(*sl);
                m_sliderDragField = nullptr;
                if (after != m_sliderDragBefore)
                {
                    const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_sliderDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasSlider()) acc(*go->getSlider()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*sl);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*sl) = val;
            if (ImGui::IsItemActivated())
            {
                m_sliderDragBefore2 = before;
                m_sliderDragOwnerId = id;
                m_sliderDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_sliderDragOwnerId == id &&
                m_sliderDragField == label)
            {
                const glm::vec2 after = acc(*sl);
                m_sliderDragField = nullptr;
                if (after != m_sliderDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_sliderDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasSlider()) acc(*go->getSlider()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*sl);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*sl) = val;
            if (ImGui::IsItemActivated())
            {
                m_sliderDragBefore4 = before;
                m_sliderDragOwnerId = id;
                m_sliderDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_sliderDragOwnerId == id &&
                m_sliderDragField == label)
            {
                const glm::vec4 after = acc(*sl);
                m_sliderDragField = nullptr;
                if (after != m_sliderDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_sliderDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasSlider()) acc(*go->getSlider()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one
        // per key: same criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sl);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*sl) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_sliderDragBeforeStr = before;
                m_sliderDragOwnerId   = id;
                m_sliderDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_sliderDragOwnerId == id &&
                m_sliderDragField == label)
            {
                const std::string after = acc(*sl);
                const std::string prev  = m_sliderDragBeforeStr;
                m_sliderDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasSlider()) acc(*go->getSlider()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar
        // (<atlas>.sprites.json) choose from the list; without it fall back to the
        // text field, which still works for a hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sl->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sl);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected:
            // it is shown at the end marked.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*sl) = after;
                    const std::string lbl = std::string(label) + " of slider '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasSlider()) acc(*go->getSlider()) = v;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##slider", +[](SliderComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##slider", +[](SliderComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##slider", +[](SliderComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##slider", +[](SliderComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##slider", +[](SliderComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##slider", +[](SliderComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##slider", +[](SliderComponent& c) -> bool& { return c.interactable; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false it is drawn the same but cannot be moved");

        ImGui::TextDisabled("Value");
        dragFloat("Value##slider", +[](SliderComponent& c) -> float& { return c.value; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        dragFloat("Min##slider", +[](SliderComponent& c) -> float& { return c.minValue; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        dragFloat("Max##slider", +[](SliderComponent& c) -> float& { return c.maxValue; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        checkBox("Whole Numbers##slider", +[](SliderComponent& c) -> bool& { return c.wholeNumbers; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rounds the value that is WRITTEN, not just the one drawn");

        static const char* kSliderDirs[] = { "Left To Right", "Right To Left",
                                             "Bottom To Top", "Top To Bottom" };
        comboEnum("Direction##slider", (int)sl->direction, kSliderDirs, IM_ARRAYSIZE(kSliderDirs),
                  +[](SliderComponent& c, int v) { c.direction = (UiSliderDirection)v; });

        ImGui::TextDisabled("Colors and handle");
        colorEdit("Track Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.color; });
        colorEdit("Fill Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.fillColor; });
        colorEdit("Handle Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.handleColor; });
        dragFloat("Handle Size##slider", +[](SliderComponent& c) -> float& { return c.handleSize; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Handle length ALONG the travel axis, in px. At 0 the travel\n"
                              "is the whole rect.");

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##slider", +[](SliderComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "sliderAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_sliderAtlasDlgOwner = id;
                m_sliderAtlasDlgOpen  = true;
                m_sliderAtlasFileDialog->OpenDialog("SliderAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setSliderAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(sl->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##slider")) ctx.openSpriteEditor(sl->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sl->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##slider", +[](SliderComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Fill##slider", +[](SliderComponent& c) -> std::string& { return c.fillSprite; });
        spriteField("Handle##slider", +[](SliderComponent& c) -> std::string& { return c.handleSprite; });

        if (!m_sliderPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_sliderPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<SliderComponentCommand>(
            *ctx.scene, "Remove Slider from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getSlider());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Slider component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setCheckboxAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Same veto as the rest of UI components, and for the same reason: the
    // drop and file dialog pass here, so the filter goes once.
    if (!isUiAtlasPath(path))
    {
        m_checkboxPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_checkboxPathError);
        return;
    }
    m_checkboxPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasCheckbox()) return;

    CheckboxComponent& c = *go->getCheckbox();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of checkbox '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasCheckbox()) g->getCheckbox()->atlasPath = v;
            }));
}

void PropertiesPanel::drawCheckboxPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing
    // selection with the dialog open leaves the flag stuck at true forever.
    if (m_checkboxAtlasDlgOpen && m_checkboxAtlasFileDialog->Display("CheckboxAtlasDlg"))
    {
        if (m_checkboxAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_checkboxAtlasFileDialog->GetFilePathName()))
                setCheckboxAtlasPath(ctx, m_checkboxAtlasDlgOwner, resolved->string());
        }
        m_checkboxAtlasFileDialog->Close();
        m_checkboxAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawCheckboxSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasCheckbox()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Checkbox",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##checkbox");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        CheckboxComponent* cb = ctx.selected->getCheckbox().get();
        ImGui::TextWrapped("Checkbox. A click toggles the value. The text label is separate: a Text component fits on this same GameObject.");

        using FloatRef = float&       (*)(CheckboxComponent&);
        using Vec2Ref  = glm::vec2&   (*)(CheckboxComponent&);
        using Vec4Ref  = glm::vec4&   (*)(CheckboxComponent&);
        using StrRef   = std::string& (*)(CheckboxComponent&);
        using BoolRef  = bool&        (*)(CheckboxComponent&);
        using EnumSet  = void         (*)(CheckboxComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        // Combos and checkbox commit on the spot: one click = one change.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*cb, idx);
                const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCheckbox()) apply(*go->getCheckbox(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*cb);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*cb) = val;
                const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                        }));
            }
        };

        // Scalars share the usual dance: "before" read BEFORE drawing, session
        // open in IsItemActivated and commit in IsItemDeactivatedAfterEdit, so an
        // entire drag is ONE undo step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*cb);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*cb) = val;
            if (ImGui::IsItemActivated())
            {
                m_checkboxDragBefore  = before;
                m_checkboxDragOwnerId = id;
                m_checkboxDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_checkboxDragOwnerId == id &&
                m_checkboxDragField == label)
            {
                const float after = acc(*cb);
                m_checkboxDragField = nullptr;
                if (after != m_checkboxDragBefore)
                {
                    const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_checkboxDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*cb);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*cb) = val;
            if (ImGui::IsItemActivated())
            {
                m_checkboxDragBefore2 = before;
                m_checkboxDragOwnerId = id;
                m_checkboxDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_checkboxDragOwnerId == id &&
                m_checkboxDragField == label)
            {
                const glm::vec2 after = acc(*cb);
                m_checkboxDragField = nullptr;
                if (after != m_checkboxDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_checkboxDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*cb);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*cb) = val;
            if (ImGui::IsItemActivated())
            {
                m_checkboxDragBefore4 = before;
                m_checkboxDragOwnerId = id;
                m_checkboxDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_checkboxDragOwnerId == id &&
                m_checkboxDragField == label)
            {
                const glm::vec4 after = acc(*cb);
                m_checkboxDragField = nullptr;
                if (after != m_checkboxDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_checkboxDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one
        // per key: same criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*cb);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*cb) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_checkboxDragBeforeStr = before;
                m_checkboxDragOwnerId   = id;
                m_checkboxDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_checkboxDragOwnerId == id &&
                m_checkboxDragField == label)
            {
                const std::string after = acc(*cb);
                const std::string prev  = m_checkboxDragBeforeStr;
                m_checkboxDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar
        // (<atlas>.sprites.json) choose from the list; without it fall back to the
        // text field, which still works for a hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, cb->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*cb);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected:
            // it is shown at the end marked.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*cb) = after;
                    const std::string lbl = std::string(label) + " of checkbox '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##checkbox", +[](CheckboxComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##checkbox", +[](CheckboxComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##checkbox", +[](CheckboxComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##checkbox", +[](CheckboxComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##checkbox", +[](CheckboxComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##checkbox", +[](CheckboxComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##checkbox", +[](CheckboxComponent& c) -> bool& { return c.interactable; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false it is drawn the same but a click does not change it");

        ImGui::TextDisabled("Value and checkmark");
        checkBox("Is On##checkbox", +[](CheckboxComponent& c) -> bool& { return c.isOn; });
        colorEdit("Box Color##checkbox", +[](CheckboxComponent& c) -> glm::vec4& { return c.color; });
        colorEdit("Check Color##checkbox", +[](CheckboxComponent& c) -> glm::vec4& { return c.checkColor; });
        dragFloat("Check Padding##checkbox", +[](CheckboxComponent& c) -> float& { return c.checkPadding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Px the checkmark is inset on all four sides.\n"
                              "One that does not fit leaves the checkmark at zero, never inverted.");

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##checkbox", +[](CheckboxComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "checkboxAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_checkboxAtlasDlgOwner = id;
                m_checkboxAtlasDlgOpen  = true;
                m_checkboxAtlasFileDialog->OpenDialog("CheckboxAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setCheckboxAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(cb->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##checkbox")) ctx.openSpriteEditor(cb->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && cb->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##checkbox", +[](CheckboxComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Checkmark##checkbox", +[](CheckboxComponent& c) -> std::string& { return c.checkmarkSprite; });

        if (!m_checkboxPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_checkboxPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<CheckboxComponentCommand>(
            *ctx.scene, "Remove Checkbox from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getCheckbox());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Checkbox component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setToggleAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Same veto as the rest of UI components, and for the same reason: the
    // drop and file dialog pass here, so the filter goes once.
    if (!isUiAtlasPath(path))
    {
        m_togglePathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_togglePathError);
        return;
    }
    m_togglePathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasToggle()) return;

    ToggleComponent& c = *go->getToggle();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of toggle '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasToggle()) g->getToggle()->atlasPath = v;
            }));
}

void PropertiesPanel::drawTogglePathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing
    // selection with the dialog open leaves the flag stuck at true forever.
    if (m_toggleAtlasDlgOpen && m_toggleAtlasFileDialog->Display("ToggleAtlasDlg"))
    {
        if (m_toggleAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_toggleAtlasFileDialog->GetFilePathName()))
                setToggleAtlasPath(ctx, m_toggleAtlasDlgOwner, resolved->string());
        }
        m_toggleAtlasFileDialog->Close();
        m_toggleAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawToggleSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasToggle()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Toggle",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##toggle");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ToggleComponent* tg = ctx.selected->getToggle().get();
        ImGui::TextWrapped("Sliding switch. It stores the same data as the Checkbox (a bool) but with other fields: two track colors and the knob size.");

        using FloatRef = float&       (*)(ToggleComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ToggleComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ToggleComponent&);
        using StrRef   = std::string& (*)(ToggleComponent&);
        using BoolRef  = bool&        (*)(ToggleComponent&);
        using EnumSet  = void         (*)(ToggleComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        // Combos and checkbox commit on the spot: one click = one change.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*tg, idx);
                const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasToggle()) apply(*go->getToggle(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*tg);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*tg) = val;
                const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasToggle()) acc(*go->getToggle()) = v;
                        }));
            }
        };

        // Scalars share the usual dance: "before" read BEFORE drawing, session
        // open in IsItemActivated and commit in IsItemDeactivatedAfterEdit, so an
        // entire drag is ONE undo step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*tg);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*tg) = val;
            if (ImGui::IsItemActivated())
            {
                m_toggleDragBefore  = before;
                m_toggleDragOwnerId = id;
                m_toggleDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_toggleDragOwnerId == id &&
                m_toggleDragField == label)
            {
                const float after = acc(*tg);
                m_toggleDragField = nullptr;
                if (after != m_toggleDragBefore)
                {
                    const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_toggleDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasToggle()) acc(*go->getToggle()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*tg);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*tg) = val;
            if (ImGui::IsItemActivated())
            {
                m_toggleDragBefore2 = before;
                m_toggleDragOwnerId = id;
                m_toggleDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_toggleDragOwnerId == id &&
                m_toggleDragField == label)
            {
                const glm::vec2 after = acc(*tg);
                m_toggleDragField = nullptr;
                if (after != m_toggleDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_toggleDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasToggle()) acc(*go->getToggle()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*tg);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*tg) = val;
            if (ImGui::IsItemActivated())
            {
                m_toggleDragBefore4 = before;
                m_toggleDragOwnerId = id;
                m_toggleDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_toggleDragOwnerId == id &&
                m_toggleDragField == label)
            {
                const glm::vec4 after = acc(*tg);
                m_toggleDragField = nullptr;
                if (after != m_toggleDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_toggleDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasToggle()) acc(*go->getToggle()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one
        // per key: same criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*tg);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*tg) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_toggleDragBeforeStr = before;
                m_toggleDragOwnerId   = id;
                m_toggleDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_toggleDragOwnerId == id &&
                m_toggleDragField == label)
            {
                const std::string after = acc(*tg);
                const std::string prev  = m_toggleDragBeforeStr;
                m_toggleDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasToggle()) acc(*go->getToggle()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar
        // (<atlas>.sprites.json) choose from the list; without it fall back to the
        // text field, which still works for a hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, tg->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*tg);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected:
            // it is shown at the end marked.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*tg) = after;
                    const std::string lbl = std::string(label) + " of toggle '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasToggle()) acc(*go->getToggle()) = v;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##toggle", +[](ToggleComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##toggle", +[](ToggleComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##toggle", +[](ToggleComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##toggle", +[](ToggleComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##toggle", +[](ToggleComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##toggle", +[](ToggleComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##toggle", +[](ToggleComponent& c) -> bool& { return c.interactable; });

        ImGui::TextDisabled("Value, colors and knob");
        checkBox("Is On##toggle", +[](ToggleComponent& c) -> bool& { return c.isOn; });
        colorEdit("Off Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.offColor; });
        colorEdit("On Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.onColor; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The track has NO color of its own: it is painted with this one or with\n"
                              "Off Color depending on the state.");
        colorEdit("Knob Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.knobColor; });
        dragFloat("Knob Size##toggle", +[](ToggleComponent& c) -> float& { return c.knobSize; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clamped to what is left between paddings: one bigger than the\n"
                              "track would stick out of the edge.");
        dragFloat("Knob Padding##toggle", +[](ToggleComponent& c) -> float& { return c.knobPadding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##toggle", +[](ToggleComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "toggleAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_toggleAtlasDlgOwner = id;
                m_toggleAtlasDlgOpen  = true;
                m_toggleAtlasFileDialog->OpenDialog("ToggleAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setToggleAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(tg->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##toggle")) ctx.openSpriteEditor(tg->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && tg->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##toggle", +[](ToggleComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Knob##toggle", +[](ToggleComponent& c) -> std::string& { return c.knobSprite; });

        if (!m_togglePathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_togglePathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ToggleComponentCommand>(
            *ctx.scene, "Remove Toggle from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getToggle());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Toggle component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setScrollbarAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Same veto as the rest of UI components, and for the same reason: the drop and file dialog
    // pass here, so the filter goes once.
    if (!isUiAtlasPath(path))
    {
        m_scrollbarPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_scrollbarPathError);
        return;
    }
    m_scrollbarPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasScrollbar()) return;

    ScrollbarComponent& c = *go->getScrollbar();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of scrollbar '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasScrollbar()) g->getScrollbar()->atlasPath = v;
            }));
}

void PropertiesPanel::drawScrollbarPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing selection with the dialog
    // open leaves the flag stuck at true forever.
    if (m_scrollbarAtlasDlgOpen && m_scrollbarAtlasFileDialog->Display("ScrollbarAtlasDlg"))
    {
        if (m_scrollbarAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_scrollbarAtlasFileDialog->GetFilePathName()))
                setScrollbarAtlasPath(ctx, m_scrollbarAtlasDlgOwner, resolved->string());
        }
        m_scrollbarAtlasFileDialog->Close();
        m_scrollbarAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawScrollbarSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasScrollbar()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Scrollbar",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##scrollbar");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ScrollbarComponent* sb = ctx.selected->getScrollbar().get();
        ImGui::TextWrapped("Channel with a variable-size handle. The value is always in 0..1: whatever scrolls interprets it, not the bar.");

        using FloatRef = float&       (*)(ScrollbarComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ScrollbarComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ScrollbarComponent&);
        using StrRef   = std::string& (*)(ScrollbarComponent&);
        using BoolRef  = bool&        (*)(ScrollbarComponent&);
        using EnumSet  = void         (*)(ScrollbarComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        // Combos and checkbox commit on the spot: one click = one change.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sb, idx);
                const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollbar()) apply(*go->getScrollbar(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*sb);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*sb) = val;
                const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                        }));
            }
        };

        // Scalars share the usual dance: "before" read BEFORE drawing, session open in
        // IsItemActivated and commit in IsItemDeactivatedAfterEdit, so an entire drag is ONE undo
        // step.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*sb);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*sb) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollbarDragBefore  = before;
                m_scrollbarDragOwnerId = id;
                m_scrollbarDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollbarDragOwnerId == id &&
                m_scrollbarDragField == label)
            {
                const float after = acc(*sb);
                m_scrollbarDragField = nullptr;
                if (after != m_scrollbarDragBefore)
                {
                    const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_scrollbarDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*sb);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*sb) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollbarDragBefore2 = before;
                m_scrollbarDragOwnerId = id;
                m_scrollbarDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollbarDragOwnerId == id &&
                m_scrollbarDragField == label)
            {
                const glm::vec2 after = acc(*sb);
                m_scrollbarDragField = nullptr;
                if (after != m_scrollbarDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_scrollbarDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*sb);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*sb) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollbarDragBefore4 = before;
                m_scrollbarDragOwnerId = id;
                m_scrollbarDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollbarDragOwnerId == id &&
                m_scrollbarDragField == label)
            {
                const glm::vec4 after = acc(*sb);
                m_scrollbarDragField = nullptr;
                if (after != m_scrollbarDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_scrollbarDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one per key: same
        // criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sb);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*sb) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_scrollbarDragBeforeStr = before;
                m_scrollbarDragOwnerId   = id;
                m_scrollbarDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollbarDragOwnerId == id &&
                m_scrollbarDragField == label)
            {
                const std::string after = acc(*sb);
                const std::string prev  = m_scrollbarDragBeforeStr;
                m_scrollbarDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar (<atlas>.sprites.json)
        // choose from the list; without it fall back to the text field, which still works for a
        // hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sb->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sb);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // A name that is no longer in the atlas is NOT lost or auto-corrected: it is shown at
            // the end marked.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*sb) = after;
                    const std::string lbl = std::string(label) + " of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                            }));
                }
            }
        };

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##scrollbar", +[](ScrollbarComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##scrollbar", +[](ScrollbarComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##scrollbar", +[](ScrollbarComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##scrollbar", +[](ScrollbarComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##scrollbar", +[](ScrollbarComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        checkBox("Visible##scrollbar", +[](ScrollbarComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##scrollbar", +[](ScrollbarComponent& c) -> bool& { return c.interactable; });

        ImGui::TextDisabled("Value");
        dragFloat("Value##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.value; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Handle Fraction##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.handleFraction; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fraction of the channel taken by the handle: 1 = the content fits\n"
                              "entirely and there is nothing to scroll.");

        static const char* kScrollDirs[] = { "Left To Right", "Right To Left",
                                             "Top To Bottom", "Bottom To Top" };
        comboEnum("Direction##scrollbar", (int)sb->direction, kScrollDirs, IM_ARRAYSIZE(kScrollDirs),
                  +[](ScrollbarComponent& c, int v) { c.direction = (UiScrollbarDirection)v; });

        {
            // numberOfSteps is a uint32 and there is no dragUint: edited as an integer with the
            // same undo dance as the rest.
            const int before = (int)sb->numberOfSteps;
            int       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt("Number Of Steps##scrollbar", &val, 0.25f, 0, 1024))
                sb->numberOfSteps = (uint32_t)(val < 0 ? 0 : val);
            if (ImGui::IsItemActivated())
            {
                m_scrollbarDragBefore  = (float)before;
                m_scrollbarDragOwnerId = id;
                m_scrollbarDragField   = "Number Of Steps##scrollbar";
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollbarDragOwnerId == id &&
                m_scrollbarDragField == std::string("Number Of Steps##scrollbar"))
            {
                const int after = (int)sb->numberOfSteps;
                m_scrollbarDragField = nullptr;
                if (after != (int)m_scrollbarDragBefore)
                {
                    const std::string lbl = "Number Of Steps of scrollbar '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_scrollbarDragBefore, after,
                            [scene, id](const int& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollbar())
                                        go->getScrollbar()->numberOfSteps = (uint32_t)(v < 0 ? 0 : v);
                            }));
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discrete stops. 0 and 1 = continuous: snapping to a single\n"
                              "stop would leave the bar stuck in one place.");

        dragFloat("Scroll Step##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.scrollStep; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much the wheel moves per notch, as a fraction of the travel");

        ImGui::TextDisabled("Colors");
        colorEdit("Track Color##scrollbar", +[](ScrollbarComponent& c) -> glm::vec4& { return c.color; });
        colorEdit("Handle Color##scrollbar", +[](ScrollbarComponent& c) -> glm::vec4& { return c.handleColor; });

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##scrollbar", +[](ScrollbarComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "scrollbarAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_scrollbarAtlasDlgOwner = id;
                m_scrollbarAtlasDlgOpen  = true;
                m_scrollbarAtlasFileDialog->OpenDialog("ScrollbarAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setScrollbarAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(sb->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##scrollbar")) ctx.openSpriteEditor(sb->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sb->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##scrollbar", +[](ScrollbarComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Handle##scrollbar", +[](ScrollbarComponent& c) -> std::string& { return c.handleSprite; });

        if (!m_scrollbarPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_scrollbarPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ScrollbarComponentCommand>(
            *ctx.scene, "Remove Scrollbar from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getScrollbar());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Scrollbar component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setInputFieldAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_inputFieldPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_inputFieldPathError);
        return;
    }
    m_inputFieldPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasInputField()) return;

    InputFieldComponent& c = *go->getInputField();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of input field '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasInputField()) g->getInputField()->atlasPath = v;
            }));
}

void PropertiesPanel::setInputFieldFontPath(EditorContext& ctx, uint64_t ownerId,
                                      const std::string& path)
{
    // Same veto and for the same reason as atlas: the drop and file dialog pass here, so the filter
    // goes once.
    if (!isUiFontPath(path))
    {
        m_inputFieldPathError = "Not a font (.ttf .otf .ttc): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_inputFieldPathError);
        return;
    }
    m_inputFieldPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasInputField()) return;

    InputFieldComponent& c = *go->getInputField();
    const std::string before = c.fontPath;
    if (before == path) return;

    c.fontPath = path;

    const std::string lbl = std::string("Font ") + "of input field '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasInputField()) g->getInputField()->fontPath = v;
            }));
}

void PropertiesPanel::drawInputFieldPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing selection with the dialog
    // open leaves the flag stuck at true forever.
    if (m_inputFieldAtlasDlgOpen && m_inputFieldAtlasFileDialog->Display("InputFieldAtlasDlg"))
    {
        if (m_inputFieldAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_inputFieldAtlasFileDialog->GetFilePathName()))
                setInputFieldAtlasPath(ctx, m_inputFieldAtlasDlgOwner, resolved->string());
        }
        m_inputFieldAtlasFileDialog->Close();
        m_inputFieldAtlasDlgOpen = false;
    }

    if (m_inputFieldFontDlgOpen && m_inputFieldFontFileDialog->Display("InputFieldFontDlg"))
    {
        if (m_inputFieldFontFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_inputFieldFontFileDialog->GetFilePathName()))
                setInputFieldFontPath(ctx, m_inputFieldFontDlgOwner, resolved->string());
        }
        m_inputFieldFontFileDialog->Close();
        m_inputFieldFontDlgOpen = false;
    }
}

void PropertiesPanel::drawInputFieldSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasInputField()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Input Field",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##inputfield");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        InputFieldComponent* fld = ctx.selected->getInputField().get();
        ImGui::TextWrapped("Text field. It is the only widget the PLAYER types into: the canvas delivers the characters to the focused element and the handler inserts them here.");

        using FloatRef = float&       (*)(InputFieldComponent&);
        using Vec2Ref  = glm::vec2&   (*)(InputFieldComponent&);
        using Vec4Ref  = glm::vec4&   (*)(InputFieldComponent&);
        using StrRef   = std::string& (*)(InputFieldComponent&);
        using BoolRef  = bool&        (*)(InputFieldComponent&);
        using EnumSet  = void         (*)(InputFieldComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*fld, idx);
                const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasInputField()) apply(*go->getInputField(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*fld);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*fld) = val;
                const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasInputField()) acc(*go->getInputField()) = v;
                        }));
            }
        };

        // "before" read BEFORE drawing, session open in IsItemActivated and commit in
        // IsItemDeactivatedAfterEdit: an entire drag is ONE undo step, not one per frame.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*fld);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*fld) = val;
            if (ImGui::IsItemActivated())
            {
                m_inputFieldDragBefore  = before;
                m_inputFieldDragOwnerId = id;
                m_inputFieldDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_inputFieldDragOwnerId == id &&
                m_inputFieldDragField == label)
            {
                const float after = acc(*fld);
                m_inputFieldDragField = nullptr;
                if (after != m_inputFieldDragBefore)
                {
                    const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_inputFieldDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField()) acc(*go->getInputField()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*fld);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*fld) = val;
            if (ImGui::IsItemActivated())
            {
                m_inputFieldDragBefore2 = before;
                m_inputFieldDragOwnerId = id;
                m_inputFieldDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_inputFieldDragOwnerId == id &&
                m_inputFieldDragField == label)
            {
                const glm::vec2 after = acc(*fld);
                m_inputFieldDragField = nullptr;
                if (after != m_inputFieldDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_inputFieldDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField()) acc(*go->getInputField()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*fld);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*fld) = val;
            if (ImGui::IsItemActivated())
            {
                m_inputFieldDragBefore4 = before;
                m_inputFieldDragOwnerId = id;
                m_inputFieldDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_inputFieldDragOwnerId == id &&
                m_inputFieldDragField == label)
            {
                const glm::vec4 after = acc(*fld);
                m_inputFieldDragField = nullptr;
                if (after != m_inputFieldDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_inputFieldDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField()) acc(*go->getInputField()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one per key: same
        // criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*fld);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*fld) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_inputFieldDragBeforeStr = before;
                m_inputFieldDragOwnerId   = id;
                m_inputFieldDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_inputFieldDragOwnerId == id &&
                m_inputFieldDragField == label)
            {
                const std::string after = acc(*fld);
                const std::string prev  = m_inputFieldDragBeforeStr;
                m_inputFieldDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField()) acc(*go->getInputField()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar (<atlas>.sprites.json)
        // choose from the list; without it fall back to the text field, which still works for a
        // hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, fld->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*fld);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*fld) = after;
                    const std::string lbl = std::string(label) + " of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField()) acc(*go->getInputField()) = v;
                            }));
                }
            }
        };
        (void)spriteField;

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##inputfield", +[](InputFieldComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##inputfield", +[](InputFieldComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##inputfield", +[](InputFieldComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##inputfield", +[](InputFieldComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##inputfield", +[](InputFieldComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Box Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.color; });
        checkBox("Visible##inputfield", +[](InputFieldComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##inputfield", +[](InputFieldComponent& c) -> bool& { return c.interactable; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When false it does not even take focus");
        checkBox("Read Only##inputfield", +[](InputFieldComponent& c) -> bool& { return c.readOnly; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Takes focus and lets the cursor move, but not change the text");

        ImGui::TextDisabled("Text");
        inputText("Text##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.text; });
        inputText("Placeholder##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.placeholder; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What is shown when the field is EMPTY, with its own color");
        dragFloat("Font Size##inputfield", +[](InputFieldComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Text Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.textColor; });
        colorEdit("Placeholder Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.placeholderColor; });

        static const char* kInputAligns[] = { "Left", "Center", "Right", "Justify" };
        comboEnum("Align##inputfield", (int)fld->align, kInputAligns, IM_ARRAYSIZE(kInputAligns),
                  +[](InputFieldComponent& c, int v) { c.align = (UiTextAlign)v; });
        dragFloat("Padding##inputfield", +[](InputFieldComponent& c) -> float& { return c.padding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");

        ImGui::TextDisabled("Filter");
        {
            // characterLimit is a uint32 and there is no dragUint: edited as an integer with the
            // same undo dance as the rest.
            const int before = (int)fld->characterLimit;
            int       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt("Character Limit##inputfield", &val, 0.25f, 0, 65536))
                fld->characterLimit = (uint32_t)(val < 0 ? 0 : val);
            if (ImGui::IsItemActivated())
            {
                m_inputFieldDragBefore  = (float)before;
                m_inputFieldDragOwnerId = id;
                m_inputFieldDragField   = "Character Limit##inputfield";
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_inputFieldDragOwnerId == id &&
                m_inputFieldDragField == std::string("Character Limit##inputfield"))
            {
                const int after = (int)fld->characterLimit;
                m_inputFieldDragField = nullptr;
                if (after != (int)m_inputFieldDragBefore)
                {
                    const std::string lbl = "Character Limit of input field '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_inputFieldDragBefore, after,
                            [scene, id](const int& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasInputField())
                                        go->getInputField()->characterLimit = (uint32_t)(v < 0 ? 0 : v);
                            }));
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Counts CHARACTERS, not bytes. 0 = no limit.");

        static const char* kContentTypes[] = { "Standard", "Integer Number", "Decimal Number",
                                                "Alphanumeric", "Password" };
        comboEnum("Content Type##inputfield", (int)fld->contentType, kContentTypes,
                  IM_ARRAYSIZE(kContentTypes),
                  +[](InputFieldComponent& c, int v) { c.contentType = (UiInputContentType)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Filters what can be TYPED. Password stores the text as\n"
                              "is and only changes what is shown.");
        inputText("Password Char##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.passwordChar; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empty falls back to the asterisk: a field that shows NOTHING looks broken");

        ImGui::TextDisabled("Cursor");
        colorEdit("Caret Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.caretColor; });
        dragFloat("Caret Width##inputfield", +[](InputFieldComponent& c) -> float& { return c.caretWidth; },
                  0.1f, 0.0f, 64.0f, "%.2f");
        dragFloat("Caret Blink Rate##inputfield", +[](InputFieldComponent& c) -> float& { return c.caretBlinkRate; },
                  0.01f, 0.0f, 10.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Seconds per half cycle. 0 = steady, no blinking.");

        ImGui::TextDisabled("Font");
        inputText("Font##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empty = the project's default font");
        drawAssetDropBox(ctx, "inputfieldFont", "Drop .ttf/.otf here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_inputFieldFontDlgOwner = id;
                m_inputFieldFontDlgOpen  = true;
                m_inputFieldFontFileDialog->OpenDialog("InputFieldFontDlg", "Choose font", ".ttf,.otf,.ttc", cfg);
            },
            [&](const std::string& dropped) { setInputFieldFontPath(ctx, id, dropped); });

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "inputfieldAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_inputFieldAtlasDlgOwner = id;
                m_inputFieldAtlasDlgOpen  = true;
                m_inputFieldAtlasFileDialog->OpenDialog("InputFieldAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setInputFieldAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(fld->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##inputfield")) ctx.openSpriteEditor(fld->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && fld->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.backgroundSprite; });

        if (!m_inputFieldPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_inputFieldPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<InputFieldComponentCommand>(
            *ctx.scene, "Remove Input Field from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getInputField());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Input Field component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setDropdownAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_dropdownPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_dropdownPathError);
        return;
    }
    m_dropdownPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasDropdown()) return;

    DropdownComponent& c = *go->getDropdown();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of dropdown '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasDropdown()) g->getDropdown()->atlasPath = v;
            }));
}

void PropertiesPanel::setDropdownFontPath(EditorContext& ctx, uint64_t ownerId,
                                      const std::string& path)
{
    // Same veto and for the same reason as atlas: the drop and file dialog pass here, so the filter
    // goes once.
    if (!isUiFontPath(path))
    {
        m_dropdownPathError = "Not a font (.ttf .otf .ttc): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_dropdownPathError);
        return;
    }
    m_dropdownPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasDropdown()) return;

    DropdownComponent& c = *go->getDropdown();
    const std::string before = c.fontPath;
    if (before == path) return;

    c.fontPath = path;

    const std::string lbl = std::string("Font ") + "of dropdown '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasDropdown()) g->getDropdown()->fontPath = v;
            }));
}

void PropertiesPanel::drawDropdownPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing selection with the dialog
    // open leaves the flag stuck at true forever.
    if (m_dropdownAtlasDlgOpen && m_dropdownAtlasFileDialog->Display("DropdownAtlasDlg"))
    {
        if (m_dropdownAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_dropdownAtlasFileDialog->GetFilePathName()))
                setDropdownAtlasPath(ctx, m_dropdownAtlasDlgOwner, resolved->string());
        }
        m_dropdownAtlasFileDialog->Close();
        m_dropdownAtlasDlgOpen = false;
    }

    if (m_dropdownFontDlgOpen && m_dropdownFontFileDialog->Display("DropdownFontDlg"))
    {
        if (m_dropdownFontFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_dropdownFontFileDialog->GetFilePathName()))
                setDropdownFontPath(ctx, m_dropdownFontDlgOwner, resolved->string());
        }
        m_dropdownFontFileDialog->Close();
        m_dropdownFontDlgOpen = false;
    }
}

void PropertiesPanel::drawDropdownSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasDropdown()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Dropdown",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##dropdown");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        DropdownComponent* dd = ctx.selected->getDropdown().get();
        ImGui::TextWrapped("Dropdown. Adding or removing an option changes the SHAPE of the UI tree, so the canvas is rebuilt; opening and closing does not.");

        using FloatRef = float&       (*)(DropdownComponent&);
        using Vec2Ref  = glm::vec2&   (*)(DropdownComponent&);
        using Vec4Ref  = glm::vec4&   (*)(DropdownComponent&);
        using StrRef   = std::string& (*)(DropdownComponent&);
        using BoolRef  = bool&        (*)(DropdownComponent&);
        using EnumSet  = void         (*)(DropdownComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*dd, idx);
                const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasDropdown()) apply(*go->getDropdown(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*dd);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*dd) = val;
                const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                        }));
            }
        };

        // "before" read BEFORE drawing, session open in IsItemActivated and commit in
        // IsItemDeactivatedAfterEdit: an entire drag is ONE undo step, not one per frame.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*dd);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*dd) = val;
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBefore  = before;
                m_dropdownDragOwnerId = id;
                m_dropdownDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == label)
            {
                const float after = acc(*dd);
                m_dropdownDragField = nullptr;
                if (after != m_dropdownDragBefore)
                {
                    const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_dropdownDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*dd);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*dd) = val;
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBefore2 = before;
                m_dropdownDragOwnerId = id;
                m_dropdownDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == label)
            {
                const glm::vec2 after = acc(*dd);
                m_dropdownDragField = nullptr;
                if (after != m_dropdownDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_dropdownDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*dd);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*dd) = val;
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBefore4 = before;
                m_dropdownDragOwnerId = id;
                m_dropdownDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == label)
            {
                const glm::vec4 after = acc(*dd);
                m_dropdownDragField = nullptr;
                if (after != m_dropdownDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_dropdownDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one per key: same
        // criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*dd);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*dd) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBeforeStr = before;
                m_dropdownDragOwnerId   = id;
                m_dropdownDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == label)
            {
                const std::string after = acc(*dd);
                const std::string prev  = m_dropdownDragBeforeStr;
                m_dropdownDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar (<atlas>.sprites.json)
        // choose from the list; without it fall back to the text field, which still works for a
        // hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, dd->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*dd);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*dd) = after;
                    const std::string lbl = std::string(label) + " of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                            }));
                }
            }
        };
        (void)spriteField;

        ImGui::TextDisabled("Rect");
        dragVec2("Anchor Min##dropdown", +[](DropdownComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##dropdown", +[](DropdownComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##dropdown", +[](DropdownComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##dropdown", +[](DropdownComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##dropdown", +[](DropdownComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Box Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.color; });
        checkBox("Visible##dropdown", +[](DropdownComponent& c) -> bool& { return c.visible; });
        checkBox("Interactable##dropdown", +[](DropdownComponent& c) -> bool& { return c.interactable; });

        ImGui::TextDisabled("Options");
        {
            // The entire list is ONE undo step: add, remove or rename pushes the whole vector. Per
            // field would be three commands for what the user experiences as one change.
            const std::vector<std::string> before = dd->options;
            bool cambiada = false;

            for (size_t k = 0; k < dd->options.size(); k++)
            {
                ImGui::PushID((int)k);
                char buf[256] = {};
                copyToBuffer(buf, dd->options[k]);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
                if (ImGui::InputText("##opt", buf, sizeof(buf)))
                {
                    dd->options[k] = std::string(buf);
                    // The rename does NOT commit per key: it is pushed when exiting the field, like
                    // any other InputText.
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) cambiada = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    dd->options.erase(dd->options.begin() + (long)k);
                    // Removing the chosen option leaves the value pointing to another: clamped here
                    // so the combo does not show empty.
                    if (dd->value >= (int)dd->options.size())
                        dd->value = (int)dd->options.size() - 1;
                    if (dd->value < 0) dd->value = 0;
                    cambiada = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Add option##dropdown"))
            {
                dd->options.push_back("Option " + std::to_string(dd->options.size() + 1));
                cambiada = true;
            }

            if (cambiada && scene && ctx.undo && dd->options != before)
            {
                const std::string lbl = std::string("Options ") + "of dropdown '" + owner + "'";
                ctx.pushLog(lbl + " changed");
                ctx.undo->push(std::make_unique<PropertyCommand<std::vector<std::string>>>(
                    lbl, before, dd->options,
                    [scene, id](const std::vector<std::string>& v) {
                        if (GameObject* go = scene->findById(id))
                            if (go->hasDropdown())
                            {
                                go->getDropdown()->options = v;
                                // The index is also clamped when undoing: a shorter list cannot
                                // leave it out of range.
                                int& val = go->getDropdown()->value;
                                if (val >= (int)v.size()) val = (int)v.size() - 1;
                                if (val < 0) val = 0;
                            }
                    }));
            }
        }

        {
            // The value is the 0-based INDEX, like in C++ and Lua.
            const int before = dd->value;
            int       val    = before;
            const int maxIdx = dd->options.empty() ? 0 : (int)dd->options.size() - 1;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt("Value##dropdown", &val, 0.1f, 0, maxIdx))
                dd->value = val;
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBefore  = (float)before;
                m_dropdownDragOwnerId = id;
                m_dropdownDragField   = "Value##dropdown";
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == std::string("Value##dropdown"))
            {
                const int after = dd->value;
                m_dropdownDragField = nullptr;
                if (after != (int)m_dropdownDragBefore)
                {
                    const std::string lbl = "Value of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_dropdownDragBefore, after,
                            [scene, id](const int& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown()) go->getDropdown()->value = v;
                            }));
                }
            }
        }
        if (!dd->selectedLabel().empty())
            ImGui::TextDisabled("Selected: %s", dd->selectedLabel().c_str());
        else if (!dd->options.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "The index does not point to any option");

        ImGui::TextDisabled("List");
        dragFloat("Item Height##dropdown", +[](DropdownComponent& c) -> float& { return c.itemHeight; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        {
            const int before = (int)dd->maxVisibleItems;
            int       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragInt("Max Visible Items##dropdown", &val, 0.25f, 0, 256))
                dd->maxVisibleItems = (uint32_t)(val < 0 ? 0 : val);
            if (ImGui::IsItemActivated())
            {
                m_dropdownDragBefore  = (float)before;
                m_dropdownDragOwnerId = id;
                m_dropdownDragField   = "Max Visible Items##dropdown";
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_dropdownDragOwnerId == id &&
                m_dropdownDragField == std::string("Max Visible Items##dropdown"))
            {
                const int after = (int)dd->maxVisibleItems;
                m_dropdownDragField = nullptr;
                if (after != (int)m_dropdownDragBefore)
                {
                    const std::string lbl = "Max Visible Items of dropdown '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                            lbl, (int)m_dropdownDragBefore, after,
                            [scene, id](const int& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasDropdown())
                                        go->getDropdown()->maxVisibleItems = (uint32_t)(v < 0 ? 0 : v);
                            }));
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = all. Caps the list height so that a combo with\n"
                              "fifty languages does not take up three screens.");

        colorEdit("List Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.listColor; });
        colorEdit("Item Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.itemColor; });
        colorEdit("Item Selected##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.itemSelectedColor; });
        colorEdit("Arrow Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.arrowColor; });

        ImGui::TextDisabled("Text");
        dragFloat("Font Size##dropdown", +[](DropdownComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Text Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.textColor; });
        dragFloat("Padding##dropdown", +[](DropdownComponent& c) -> float& { return c.padding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");

        ImGui::TextDisabled("Font");
        inputText("Font##dropdown", +[](DropdownComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empty = the project's default font");
        drawAssetDropBox(ctx, "dropdownFont", "Drop .ttf/.otf here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_dropdownFontDlgOwner = id;
                m_dropdownFontDlgOpen  = true;
                m_dropdownFontFileDialog->OpenDialog("DropdownFontDlg", "Choose font", ".ttf,.otf,.ttc", cfg);
            },
            [&](const std::string& dropped) { setDropdownFontPath(ctx, id, dropped); });

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##dropdown", +[](DropdownComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "dropdownAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_dropdownAtlasDlgOwner = id;
                m_dropdownAtlasDlgOpen  = true;
                m_dropdownAtlasFileDialog->OpenDialog("DropdownAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setDropdownAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(dd->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##dropdown")) ctx.openSpriteEditor(dd->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && dd->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##dropdown", +[](DropdownComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Arrow##dropdown", +[](DropdownComponent& c) -> std::string& { return c.arrowSprite; });
        spriteField("Item##dropdown", +[](DropdownComponent& c) -> std::string& { return c.itemSprite; });

        if (!m_dropdownPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_dropdownPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<DropdownComponentCommand>(
            *ctx.scene, "Remove Dropdown from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getDropdown());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Dropdown component removed from '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setScrollViewAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_scrollViewPathError = "Not an image (.png .jpg .jpeg .bmp .tga): " +
                         std::filesystem::path(path).filename().string();
        ctx.pushLog(m_scrollViewPathError);
        return;
    }
    m_scrollViewPathError.clear();

    Scene* scene = ctx.scene;
    if (!scene) return;
    GameObject* go = scene->findById(ownerId);
    if (!go || !go->hasScrollView()) return;

    ScrollViewComponent& c = *go->getScrollView();
    const std::string before = c.atlasPath;
    if (before == path) return;

    c.atlasPath = path;

    const std::string lbl = std::string("Atlas ") + "of scroll view '" + go->name + "'";
    ctx.pushLog(lbl + " changed to " + path);
    if (ctx.undo)
        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
            lbl, before, path,
            [scene, ownerId](const std::string& v) {
                if (GameObject* g = scene->findById(ownerId))
                    if (g->hasScrollView()) g->getScrollView()->atlasPath = v;
            }));
}

void PropertiesPanel::drawScrollViewPathDialog(EditorContext& ctx)
{
    // Without conditioning on ctx.selected: if not drained here, changing selection with the dialog
    // open leaves the flag stuck at true forever.
    if (m_scrollViewAtlasDlgOpen && m_scrollViewAtlasFileDialog->Display("ScrollViewAtlasDlg"))
    {
        if (m_scrollViewAtlasFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_scrollViewAtlasFileDialog->GetFilePathName()))
                setScrollViewAtlasPath(ctx, m_scrollViewAtlasDlgOwner, resolved->string());
        }
        m_scrollViewAtlasFileDialog->Close();
        m_scrollViewAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawScrollViewSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasScrollView()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Scroll View",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##scrollview");

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    const std::string owner = ctx.selected->name;

    if (sectionOpen)
    {
        ScrollViewComponent* sv = ctx.selected->getScrollView().get();
        ImGui::TextWrapped("Scrollable view. This GameObject's CHILDREN hang from its content, not from the viewport: that is why scrolling drags them.");

        using FloatRef = float&       (*)(ScrollViewComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ScrollViewComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ScrollViewComponent&);
        using StrRef   = std::string& (*)(ScrollViewComponent&);
        using BoolRef  = bool&        (*)(ScrollViewComponent&);
        using EnumSet  = void         (*)(ScrollViewComponent&, int);
        (void)sizeof(EnumSet);   // not all widgets have enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sv, idx);
                const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                ctx.pushLog(lbl + " changed to " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollView()) apply(*go->getScrollView(), v);
                        }));
            }
        };
        (void)comboEnum;

        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*sv);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*sv) = val;
                const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                ctx.pushLog(lbl + (val ? " enabled" : " disabled"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                        }));
            }
        };

        // "before" read BEFORE drawing, session open in IsItemActivated and commit in
        // IsItemDeactivatedAfterEdit: an entire drag is ONE undo step, not one per frame.
        auto dragFloat = [&](const char* label, FloatRef acc, float speed,
                             float lo, float hi, const char* fmt)
        {
            const float before = acc(*sv);
            float       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt))
                acc(*sv) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollViewDragBefore  = before;
                m_scrollViewDragOwnerId = id;
                m_scrollViewDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollViewDragOwnerId == id &&
                m_scrollViewDragField == label)
            {
                const float after = acc(*sv);
                m_scrollViewDragField = nullptr;
                if (after != m_scrollViewDragBefore)
                {
                    const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                            lbl, m_scrollViewDragBefore, after,
                            [scene, id, acc](const float& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                            }));
                }
            }
        };

        auto dragVec2 = [&](const char* label, Vec2Ref acc, float speed,
                            float lo, float hi, const char* fmt)
        {
            const glm::vec2 before = acc(*sv);
            glm::vec2       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::DragFloat2(label, &val.x, speed, lo, hi, fmt))
                acc(*sv) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollViewDragBefore2 = before;
                m_scrollViewDragOwnerId = id;
                m_scrollViewDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollViewDragOwnerId == id &&
                m_scrollViewDragField == label)
            {
                const glm::vec2 after = acc(*sv);
                m_scrollViewDragField = nullptr;
                if (after != m_scrollViewDragBefore2)
                {
                    const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec2>>(
                            lbl, m_scrollViewDragBefore2, after,
                            [scene, id, acc](const glm::vec2& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                            }));
                }
            }
        };

        auto colorEdit = [&](const char* label, Vec4Ref acc)
        {
            const glm::vec4 before = acc(*sv);
            glm::vec4       val    = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::ColorEdit4(label, &val.x))
                acc(*sv) = val;
            if (ImGui::IsItemActivated())
            {
                m_scrollViewDragBefore4 = before;
                m_scrollViewDragOwnerId = id;
                m_scrollViewDragField   = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollViewDragOwnerId == id &&
                m_scrollViewDragField == label)
            {
                const glm::vec4 after = acc(*sv);
                m_scrollViewDragField = nullptr;
                if (after != m_scrollViewDragBefore4)
                {
                    const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<glm::vec4>>(
                            lbl, m_scrollViewDragBefore4, after,
                            [scene, id, acc](const glm::vec4& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                            }));
                }
            }
        };

        // An entire InputText (write and exit the field) is ONE undo step, not one per key: same
        // criterion as dragging a DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sv);
            char buf[512] = {};
            copyToBuffer(buf, before);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::InputText(label, buf, sizeof(buf)))
                acc(*sv) = std::string(buf);
            if (ImGui::IsItemActivated())
            {
                m_scrollViewDragBeforeStr = before;
                m_scrollViewDragOwnerId   = id;
                m_scrollViewDragField     = label;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_scrollViewDragOwnerId == id &&
                m_scrollViewDragField == label)
            {
                const std::string after = acc(*sv);
                const std::string prev  = m_scrollViewDragBeforeStr;
                m_scrollViewDragField = nullptr;
                if (after != prev)
                {
                    const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                    ctx.pushLog(lbl + " changed");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, prev, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                            }));
                }
            }
        };

        // A sprite is a NAME inside the atlas, not free text. With sidecar (<atlas>.sprites.json)
        // choose from the list; without it fall back to the text field, which still works for a
        // hand-cut atlas.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sv->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sv);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(whole image)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (not in the atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                const std::string after = (idx == 0)                   ? std::string()
                                        : (idx <= (int)nombres.size()) ? nombres[(size_t)idx - 1]
                                                                       : before;
                if (after != before)
                {
                    acc(*sv) = after;
                    const std::string lbl = std::string(label) + " of scroll view '" + owner + "'";
                    ctx.pushLog(lbl + " changed to '" + after + "'");
                    if (scene && ctx.undo)
                        ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                            lbl, before, after,
                            [scene, id, acc](const std::string& v) {
                                if (GameObject* go = scene->findById(id))
                                    if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                            }));
                }
            }
        };
        (void)spriteField;

        ImGui::TextDisabled("Rect (el VIEWPORT)");
        dragVec2("Anchor Min##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.anchorMin; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Anchor Max##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.anchorMax; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Pivot##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.pivot; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragVec2("Position##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.position; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        dragVec2("Size##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.size; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        colorEdit("Color##scrollview", +[](ScrollViewComponent& c) -> glm::vec4& { return c.color; });
        checkBox("Visible##scrollview", +[](ScrollViewComponent& c) -> bool& { return c.visible; });

        ImGui::TextDisabled("Axes and content");
        checkBox("Horizontal##scrollview", +[](ScrollViewComponent& c) -> bool& { return c.horizontal; });
        checkBox("Vertical##scrollview", +[](ScrollViewComponent& c) -> bool& { return c.vertical; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A disabled axis does not move even if the content is bigger");
        dragVec2("Content Size##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.contentSize; },
                 1.0f, 0.0f, 65536.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Size of the scrollable area. It is a FIELD and not something measured from the\n"
                              "children: measuring the subtree every frame would couple scrolling to layout.");
        dragVec2("Normalized Pos##scrollview",
                 +[](ScrollViewComponent& c) -> glm::vec2& { return c.normalizedPosition; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Scroll Sensitivity##scrollview",
                  +[](ScrollViewComponent& c) -> float& { return c.scrollSensitivity; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pixels the wheel moves per notch");

        {
            const glm::vec2 r = sv->scrollRange();
            ImGui::TextDisabled("Travel: %.0f x %.0f px", r.x, r.y);
            if (r.x <= 0.0f && r.y <= 0.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                   "The content fits entirely: there is nothing to scroll");
        }
        ImGui::TextDisabled("This GameObject's children hang from the content, not from the viewport.");

        ImGui::TextDisabled("Sprites");
        inputText("Atlas##scrollview", +[](ScrollViewComponent& c) -> std::string& { return c.atlasPath; });
        drawAssetDropBox(ctx, "scrollviewAtlas", "Drop .png/.jpg/.bmp/.tga here",
            [&]
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = "assets";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                m_scrollViewAtlasDlgOwner = id;
                m_scrollViewAtlasDlgOpen  = true;
                m_scrollViewAtlasFileDialog->OpenDialog("ScrollViewAtlasDlg", "Choose atlas",
                                                 ".png,.jpg,.jpeg,.bmp,.tga", cfg);
            },
            [&](const std::string& dropped) { setScrollViewAtlasPath(ctx, id, dropped); });

        ImGui::BeginDisabled(sv->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Edit sprites...##scrollview")) ctx.openSpriteEditor(sv->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sv->atlasPath.empty())
            ImGui::SetTooltip("Choose an atlas first");
        spriteField("Background##scrollview", +[](ScrollViewComponent& c) -> std::string& { return c.backgroundSprite; });

        if (!m_scrollViewPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_scrollViewPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ScrollViewComponentCommand>(
            *ctx.scene, "Remove Scroll View from '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getScrollView());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Scroll View component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawLightSection(EditorContext& ctx)
{
    // Add-gate: without the component there is no section, like colliders.
    if (!ctx.selected->hasLight()) return;

    if (!ImGui::TreeNodeEx("Light", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    LightComponent*   light = ctx.selected->getLight().get();
    const std::string owner = ctx.selected->name;

    ImGui::TextWrapped("The position and direction come from this object's Transform "
                       "(the light points along its local -Z), not from fields of its own.");

    const char* kTypes[] = { "Point", "Spot", "Directional", "Area" };
    int typeIdx = (int)light->getType();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::Combo("Type", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes)))
    {
        const int before = (int)light->getType();
        light->setType((LightType)typeIdx);
        ctx.pushLog("Type of light '" + owner + "' changed to " + kTypes[typeIdx]);
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                "Type of light '" + owner + "'", before, typeIdx,
                [scene, id](const int& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasLight()) go->getLight()->setType((LightType)v);
                }));
        }
    }

    // What the renderer can do with shadows, said here instead of the user deducing it from a
    // shadow that does not match. The rule does not depend on which light is selected, so it is
    // shown always.
    ImGui::TextDisabled("Shadows: the FIRST light in the scene, and up to 4 more spot lights.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The first light casts shadows whatever its type, using the best\n"
            "technique for it: cascades if directional, a six-face cubemap\n"
            "if point, one perspective face if spot.\n\n"
            "In addition, the first 4 SPOT lights after it cast shadows too, with\n"
            "one face each. Spots only: a secondary directional would need\n"
            "its own four cascades and a point light six faces, and they do not fit.\n\n"
            "The rest light the scene but cast no shadow. The order is the\n"
            "scene's, not by brightness or proximity: if it depended on the camera,\n"
            "a light would gain and lose its shadow as you move.");

    // No longer any warning: all three types have their shadow correct. A directional projects in
    // parallel with cascades, which is what matches a light at infinity; a spot with one face in
    // perspective; and a point with the six of a cubemap. Here lived an orange TextColored that
    // said "Its shadow is an approximation" and was right while it was.

    ImGui::Separator();

    // The "before" is read BEFORE drawing: the picker changes the value in the same frame of the
    // click and reading it again after would already be the new one.
    const glm::vec3 beforeColor = light->getColor();
    glm::vec3       color       = beforeColor;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::ColorEdit3("Color", &color.x))
        light->setColor(color);
    if (ImGui::IsItemActivated())
    {
        m_lightColorBefore = beforeColor;
        m_lightDragOwnerId = id;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragOwnerId == id)
    {
        const glm::vec3 after = light->getColor();
        ctx.pushLog("Color of light '" + owner + "' changed");
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<glm::vec3>>(
                "Color of light '" + owner + "'", m_lightColorBefore, after,
                [scene, id](const glm::vec3& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasLight()) go->getLight()->setColor(v);
                }));
        }
    }

    // All six scalars share the same undo dance (read the "before" before drawing, open session in
    // IsItemActivated, commit in IsItemDeactivatedAfterEdit), so it goes once here not six times.
    // The field in drag is identified by its label: a bool does not go for six.
    auto floatSlider = [&](const char* label, float lo, float hi, const char* fmt,
                           float (LightComponent::*getter)() const,
                           void (LightComponent::*setter)(float))
    {
        const float before = (light->*getter)();
        float       v      = before;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
            (light->*setter)(v);
        if (ImGui::IsItemActivated())
        {
            m_lightDragActive  = true;
            m_lightDragOwnerId = id;
            m_lightDragBefore  = before;
            m_lightDragField   = label;
        }
        // The owner id and the label prevent applying a foreign "before" if the drag was
        // interrupted without commit (e.g. a Ctrl+Z mid-drag).
        if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragActive &&
            m_lightDragOwnerId == id && m_lightDragField &&
            std::strcmp(m_lightDragField, label) == 0)
        {
            m_lightDragActive = false;
            const float after = (light->*getter)();
            ctx.pushLog(std::string(label) + " of light '" + owner + "' changed to " +
                        std::to_string(after));
            if (scene && ctx.undo)
            {
                ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                    std::string(label) + " of light '" + owner + "'", m_lightDragBefore, after,
                    [scene, id, setter](const float& x) {
                        if (GameObject* go = scene->findById(id))
                            if (go->hasLight()) ((*go->getLight()).*setter)(x);
                    }));
            }
        }
    };

    floatSlider("Intensity", 0.0f, 20.0f, "%.2f",
                &LightComponent::getIntensity, &LightComponent::setIntensity);

    // Each type shows ONLY what it uses: a cone has no area size and a directional has no range.
    // Fields that do not appear stay saved in the component (and in the .scene), so changing type
    // and back loses nothing—they are hidden, not reset.
    switch (light->getType())
    {
        case LightType::Point:
            floatSlider("Range", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getRange, &LightComponent::setRange);
            break;

        case LightType::Spot:
            floatSlider("Range", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getRange, &LightComponent::setRange);
            floatSlider("Inner Angle", 0.0f, 89.9f, "%.1f",
                        &LightComponent::getInnerAngle, &LightComponent::setInnerAngle);
            floatSlider("Outer Angle", 0.0f, 89.9f, "%.1f",
                        &LightComponent::getOuterAngle, &LightComponent::setOuterAngle);
            ImGui::TextDisabled("The inner angle never exceeds the outer one: they drag each other.");
            break;

        case LightType::Directional:
            ImGui::TextDisabled("No range: lights the whole scene along the local -Z direction.");
            break;

        case LightType::Area:
            floatSlider("Area Width", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getAreaWidth, &LightComponent::setAreaWidth);
            floatSlider("Area Height", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getAreaHeight, &LightComponent::setAreaHeight);
            ImGui::TextDisabled("The range comes from the width (Width/2), not from Range.");
            break;
    }

    if (ImGui::Button("Remove Light"))
    {
        ctx.selected->setLight(nullptr);
        m_lightDragActive = false;
        ctx.pushLog("Light component removed from '" + owner + "'");
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawBoxColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasBoxCollider())
    {
        m_caches.box = nullptr;
        return;
    }

    BoxCollider* bc = ctx.selected->getBoxCollider().get();

    if (m_caches.box != bc)
    {
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
        m_editIsTrigger      = bc->isTrigger();
        m_editColliderStaticFriction  = bc->getStaticFriction();
        m_editColliderDynamicFriction = bc->getDynamicFriction();
        m_editColliderBounciness      = bc->getBounciness();
        m_caches.box  = bc;
    }
    else if (ctx.selected->hasRigidbody() && !ctx.selected->getRigidbody()->getIsKinematic() && !m_colliderDragActive)
    {
        // Simulated body: Center/Size refresh (stable under simulation).
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
    }

    Scene* scene = ctx.scene;
    uint64_t id = ctx.selected->id;
    PhysicsManager* physics = ctx.physics;
    auto applyBoxState = [scene, id, physics](const BoxColliderState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasBoxCollider()) return;
        go->getBoxCollider()->setCenter(s.center);
        go->getBoxCollider()->setHalfExtents(s.size * 0.5f);
        if (physics) physics->setTrigger(go->getBoxCollider(), s.isTrigger);
        go->getBoxCollider()->setFriction(s.staticFriction, s.dynamicFriction);
        go->getBoxCollider()->setBounciness(s.bounciness);
    };

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool activated = false;
    bool centerCommitted = false;
    bool sizeCommitted = false;
    bool materialCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        // Physics material of the collider. Same begin/commit as Center/Size: the snapshot is taken
        // in IsItemActivated and the command is pushed in IsItemDeactivatedAfterEdit, so an entire
        // drag = one undo.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Static Friction##c3", &m_editColliderStaticFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Dynamic Friction##c3", &m_editColliderDynamicFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Bounciness##c3", &m_editColliderBounciness, 0.01f, 0.0f, 1.0f, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        drawColliderLayerCombo(ctx, "Layer##lb", "Box Collider", bc, resolveBoxCollider);

        bool oldTrigger = m_editIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getBoxCollider(), m_editIsTrigger);
            ctx.pushLog(std::string("Is Trigger of '") + ctx.selected->name +
                     "' (Box Collider) " + (m_editIsTrigger ? "enabled" : "disabled"));
            if (ctx.scene)
            {
                BoxColliderState before{ m_editColliderCenter, m_editColliderSize, oldTrigger,
                                         m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };
                BoxColliderState after{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger,
                                        m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<BoxColliderState>>(
                    "Is Trigger of '" + ctx.selected->name + "' (Box Collider)", before, after, applyBoxState));
            }
        }
        drawTriggerRigidbodyHint(ctx.selected, m_editIsTrigger);

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (activated)
        m_boxColliderBeforeEdit = BoxColliderState{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger,
                                                    m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };

    if (centerCommitted)
        ctx.pushLog("Center of '" + ctx.selected->name + "' (Box Collider) changed to " + formatVec3(m_editColliderCenter));
    if (sizeCommitted)
        ctx.pushLog("Size of '" + ctx.selected->name + "' (Box Collider) changed to " + formatVec3(m_editColliderSize));
    if (materialCommitted)
        ctx.pushLog("Material of '" + ctx.selected->name + "' (Box Collider) changed");

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setFriction(m_editColliderStaticFriction, m_editColliderDynamicFriction);
        bc->setBounciness(m_editColliderBounciness);
    }

    if ((centerCommitted || sizeCommitted || materialCommitted) && ctx.scene)
    {
        BoxColliderState before = m_boxColliderBeforeEdit;
        BoxColliderState after{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger,
                                m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };
        ctx.undo->push(std::make_unique<PropertyCommand<BoxColliderState>>(
            "Box Collider of '" + ctx.selected->name + "'", before, after, applyBoxState));
    }

    if (removeClicked)
    {
        ctx.selected->setBoxCollider(nullptr);
        m_caches.box = nullptr;
        ctx.pushLog("Box Collider component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawSphereColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasSphereCollider())
    {
        m_caches.sphere = nullptr;
        return;
    }

    SphereCollider* sc = ctx.selected->getSphereCollider().get();

    if (m_caches.sphere != sc)
    {
        m_editSphereCenter        = sc->getCenter();
        m_editSphereRadius        = sc->getRadius();
        m_editSphereIsTrigger     = sc->isTrigger();
        m_editSphereStaticFriction  = sc->getStaticFriction();
        m_editSphereDynamicFriction = sc->getDynamicFriction();
        m_editSphereBounciness      = sc->getBounciness();
        m_caches.sphere = sc;
    }
    else if (ctx.selected->hasRigidbody() && !ctx.selected->getRigidbody()->getIsKinematic() && !m_sphereColliderDragActive)
    {
        m_editSphereCenter = sc->getCenter();
        m_editSphereRadius = sc->getRadius();
    }

    Scene* scene = ctx.scene;
    uint64_t id = ctx.selected->id;
    PhysicsManager* physics = ctx.physics;
    auto applySphereState = [scene, id, physics](const SphereColliderState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasSphereCollider()) return;
        go->getSphereCollider()->setCenter(s.center);
        go->getSphereCollider()->setRadius(s.radius);
        if (physics) physics->setTrigger(go->getSphereCollider(), s.isTrigger);
        go->getSphereCollider()->setFriction(s.staticFriction, s.dynamicFriction);
        go->getSphereCollider()->setBounciness(s.bounciness);
    };

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool activated = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;
    bool materialCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        // Physics material of the collider; same begin/commit as Center/Radius.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Static Friction##s3", &m_editSphereStaticFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Dynamic Friction##s3", &m_editSphereDynamicFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Bounciness##s3", &m_editSphereBounciness, 0.01f, 0.0f, 1.0f, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        drawColliderLayerCombo(ctx, "Layer##ls", "Sphere Collider", sc, resolveSphereCollider);

        bool oldTrigger = m_editSphereIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editSphereIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getSphereCollider(), m_editSphereIsTrigger);
            ctx.pushLog(std::string("Is Trigger of '") + ctx.selected->name +
                     "' (Sphere Collider) " + (m_editSphereIsTrigger ? "enabled" : "disabled"));
            if (ctx.scene)
            {
                SphereColliderState before{ m_editSphereCenter, m_editSphereRadius, oldTrigger,
                                            m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };
                SphereColliderState after{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger,
                                           m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<SphereColliderState>>(
                    "Is Trigger of '" + ctx.selected->name + "' (Sphere Collider)", before, after, applySphereState));
            }
        }
        drawTriggerRigidbodyHint(ctx.selected, m_editSphereIsTrigger);

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (activated)
        m_sphereColliderBeforeEdit = SphereColliderState{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger,
                                                          m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };

    if (centerCommitted)
        ctx.pushLog("Center of '" + ctx.selected->name + "' (Sphere Collider) changed to " + formatVec3(m_editSphereCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius of '" + ctx.selected->name + "' (Sphere Collider) changed to " + formatFloat(m_editSphereRadius));
    if (materialCommitted)
        ctx.pushLog("Material of '" + ctx.selected->name + "' (Sphere Collider) changed");

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setFriction(m_editSphereStaticFriction, m_editSphereDynamicFriction);
        sc->setBounciness(m_editSphereBounciness);
    }

    if ((centerCommitted || radiusCommitted || materialCommitted) && ctx.scene)
    {
        SphereColliderState before = m_sphereColliderBeforeEdit;
        SphereColliderState after{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger,
                                   m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };
        ctx.undo->push(std::make_unique<PropertyCommand<SphereColliderState>>(
            "Sphere Collider of '" + ctx.selected->name + "'", before, after, applySphereState));
    }

    if (removeClicked)
    {
        ctx.selected->setSphereCollider(nullptr);
        m_caches.sphere = nullptr;
        ctx.pushLog("Sphere Collider component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawCapsuleColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasCapsuleCollider())
    {
        m_caches.capsule = nullptr;
        return;
    }

    CapsuleCollider* cc = ctx.selected->getCapsuleCollider().get();

    if (m_caches.capsule != cc)
    {
        m_editCapsuleCenter        = cc->getCenter();
        m_editCapsuleRadius        = cc->getRadius();
        m_editCapsuleHeight        = cc->getHalfHeight() * 2.0f;
        m_editCapsuleIsTrigger     = cc->isTrigger();
        m_editCapsuleStaticFriction  = cc->getStaticFriction();
        m_editCapsuleDynamicFriction = cc->getDynamicFriction();
        m_editCapsuleBounciness      = cc->getBounciness();
        m_caches.capsule = cc;
    }
    else if (ctx.selected->hasRigidbody() && !ctx.selected->getRigidbody()->getIsKinematic() && !m_capsuleColliderDragActive)
    {
        m_editCapsuleCenter = cc->getCenter();
        m_editCapsuleRadius = cc->getRadius();
        m_editCapsuleHeight = cc->getHalfHeight() * 2.0f;
    }

    Scene* scene = ctx.scene;
    uint64_t id = ctx.selected->id;
    PhysicsManager* physics = ctx.physics;
    auto applyCapsuleState = [scene, id, physics](const CapsuleColliderState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasCapsuleCollider()) return;
        go->getCapsuleCollider()->setCenter(s.center);
        go->getCapsuleCollider()->setRadius(s.radius);
        go->getCapsuleCollider()->setHalfHeight(s.height * 0.5f);
        if (physics) physics->setTrigger(go->getCapsuleCollider(), s.isTrigger);
        go->getCapsuleCollider()->setFriction(s.staticFriction, s.dynamicFriction);
        go->getCapsuleCollider()->setBounciness(s.bounciness);
    };

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool activated = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;
    bool heightCommitted = false;
    bool materialCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        heightCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        // Physics material of the collider; same begin/commit as Center/Radius.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Static Friction##k4", &m_editCapsuleStaticFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Dynamic Friction##k4", &m_editCapsuleDynamicFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Bounciness##k4", &m_editCapsuleBounciness, 0.01f, 0.0f, 1.0f, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        drawColliderLayerCombo(ctx, "Layer##lc", "Capsule Collider", cc, resolveCapsuleCollider);

        bool oldTrigger = m_editCapsuleIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editCapsuleIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getCapsuleCollider(), m_editCapsuleIsTrigger);
            ctx.pushLog(std::string("Is Trigger of '") + ctx.selected->name +
                     "' (Capsule Collider) " + (m_editCapsuleIsTrigger ? "enabled" : "disabled"));
            if (ctx.scene)
            {
                CapsuleColliderState before{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, oldTrigger,
                                             m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };
                CapsuleColliderState after{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger,
                                            m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<CapsuleColliderState>>(
                    "Is Trigger of '" + ctx.selected->name + "' (Capsule Collider)", before, after, applyCapsuleState));
            }
        }
        drawTriggerRigidbodyHint(ctx.selected, m_editCapsuleIsTrigger);

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (activated)
        m_capsuleColliderBeforeEdit = CapsuleColliderState{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger,
                                                            m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };

    if (centerCommitted)
        ctx.pushLog("Center of '" + ctx.selected->name + "' (Capsule Collider) changed to " + formatVec3(m_editCapsuleCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius of '" + ctx.selected->name + "' (Capsule Collider) changed to " + formatFloat(m_editCapsuleRadius));
    if (heightCommitted)
        ctx.pushLog("Height of '" + ctx.selected->name + "' (Capsule Collider) changed to " + formatFloat(m_editCapsuleHeight));
    if (materialCommitted)
        ctx.pushLog("Material of '" + ctx.selected->name + "' (Capsule Collider) changed");

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setFriction(m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction);
        cc->setBounciness(m_editCapsuleBounciness);
    }

    if ((centerCommitted || radiusCommitted || heightCommitted || materialCommitted) && ctx.scene)
    {
        CapsuleColliderState before = m_capsuleColliderBeforeEdit;
        CapsuleColliderState after{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger,
                                    m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };
        ctx.undo->push(std::make_unique<PropertyCommand<CapsuleColliderState>>(
            "Capsule Collider of '" + ctx.selected->name + "'", before, after, applyCapsuleState));
    }

    if (removeClicked)
    {
        ctx.selected->setCapsuleCollider(nullptr);
        m_caches.capsule = nullptr;
        ctx.pushLog("Capsule Collider component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawPlaneColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasPlaneCollider())
    {
        m_caches.plane = nullptr;
        return;
    }

    PlaneCollider* pc = ctx.selected->getPlaneCollider().get();

    if (m_caches.plane != pc)
    {
        m_editPlaneCenter        = pc->getCenter();
        m_editPlaneIsTrigger     = pc->isTrigger();
        m_editPlaneStaticFriction  = pc->getStaticFriction();
        m_editPlaneDynamicFriction = pc->getDynamicFriction();
        m_editPlaneBounciness      = pc->getBounciness();
        m_caches.plane = pc;
    }

    Scene* scene = ctx.scene;
    uint64_t id = ctx.selected->id;
    PhysicsManager* physics = ctx.physics;
    auto applyPlaneState = [scene, id, physics](const PlaneColliderState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasPlaneCollider()) return;
        go->getPlaneCollider()->setCenter(s.center);
        if (physics) physics->setTrigger(go->getPlaneCollider(), s.isTrigger);
        go->getPlaneCollider()->setFriction(s.staticFriction, s.dynamicFriction);
        go->getPlaneCollider()->setBounciness(s.bounciness);
    };

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool activated = false;
    bool centerCommitted = false;
    bool materialCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        // Physics material of the collider; same begin/commit as Center.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Static Friction##p2", &m_editPlaneStaticFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Dynamic Friction##p2", &m_editPlaneDynamicFriction, 0.01f, 0.0f, +FLT_MAX, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Bounciness##p2", &m_editPlaneBounciness, 0.01f, 0.0f, 1.0f, "% .3f", ImGuiSliderFlags_AlwaysClamp);
        dragActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        materialCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        drawColliderLayerCombo(ctx, "Layer##lp", "Plane Collider", pc, resolvePlaneCollider);

        bool oldTrigger = m_editPlaneIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editPlaneIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getPlaneCollider(), m_editPlaneIsTrigger);
            ctx.pushLog(std::string("Is Trigger of '") + ctx.selected->name +
                     "' (Plane Collider) " + (m_editPlaneIsTrigger ? "enabled" : "disabled"));
            if (ctx.scene)
            {
                PlaneColliderState before{ m_editPlaneCenter, oldTrigger,
                                           m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };
                PlaneColliderState after{ m_editPlaneCenter, m_editPlaneIsTrigger,
                                          m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<PlaneColliderState>>(
                    "Is Trigger of '" + ctx.selected->name + "' (Plane Collider)", before, after, applyPlaneState));
            }
        }
        drawTriggerRigidbodyHint(ctx.selected, m_editPlaneIsTrigger);

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (activated)
        m_planeColliderBeforeEdit = PlaneColliderState{ m_editPlaneCenter, m_editPlaneIsTrigger,
                                                        m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };

    if (centerCommitted)
        ctx.pushLog("Center of '" + ctx.selected->name + "' (Plane Collider) changed to " + formatVec3(m_editPlaneCenter));
    if (materialCommitted)
        ctx.pushLog("Material of '" + ctx.selected->name + "' (Plane Collider) changed");

    if (colliderChanged)
    {
        pc->setCenter(m_editPlaneCenter);
        pc->setFriction(m_editPlaneStaticFriction, m_editPlaneDynamicFriction);
        pc->setBounciness(m_editPlaneBounciness);
    }

    if ((centerCommitted || materialCommitted) && ctx.scene)
    {
        PlaneColliderState before = m_planeColliderBeforeEdit;
        PlaneColliderState after{ m_editPlaneCenter, m_editPlaneIsTrigger,
                                  m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };
        ctx.undo->push(std::make_unique<PropertyCommand<PlaneColliderState>>(
            "Plane Collider of '" + ctx.selected->name + "'", before, after, applyPlaneState));
    }

    if (removeClicked)
    {
        ctx.selected->setPlaneCollider(nullptr);
        m_caches.plane = nullptr;
        ctx.pushLog("Plane Collider component removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawRigidbodySection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasRigidbody()) { m_caches.rigidbody = nullptr; return; }
    Rigidbody* rb = ctx.selected->getRigidbody().get();
    if (m_caches.rigidbody != rb)
    {
        m_editRbMass        = rb->getMass();
        m_editRbUseGravity  = rb->getUseGravity();
        m_editRbKinematic   = rb->getIsKinematic();
        m_editRbDrag        = rb->getDrag();
        m_editRbAngularDrag = rb->getAngularDrag();
        m_editRbConstraints = rb->getConstraints();
        m_editRbCcd         = rb->getCcd();
        m_editRbInterpolate = rb->getInterpolate();
        m_caches.rigidbody = rb;
    }

    ImGui::Separator();
    if (!ImGui::TreeNodeEx("Rigidbody", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*   scene = ctx.scene;
    uint64_t id    = ctx.selected->id;

    // Applies a RigidbodyState to the GameObject resolved by id (survives undo-of-delete). Same
    // pattern as applyBoxState.
    auto applyRbState = [scene, id](const RigidbodyState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasRigidbody()) return;
        auto rb2 = go->getRigidbody();
        rb2->setMass(s.mass);
        rb2->setUseGravity(s.useGravity);
        rb2->setIsKinematic(s.isKinematic);
        rb2->setDrag(s.drag);
        rb2->setAngularDrag(s.angularDrag);
        rb2->setConstraints(s.constraints);
        rb2->setCcd(s.ccd);
        rb2->setInterpolate(s.interpolate);
    };
    auto currentState = [&]() {
        return RigidbodyState{ m_editRbMass, m_editRbUseGravity, m_editRbKinematic,
                               m_editRbDrag, m_editRbAngularDrag, m_editRbConstraints,
                               m_editRbCcd, m_editRbInterpolate };
    };

    // --- Drag floats: snapshot when ANY activates, command when ANY deactivates.
    // IsItemActivated/IsItemDeactivatedAfterEdit are checked per widget and accumulated
    // (not a single final query: that would only reflect the last DragFloat and leave
    // Mass/Drag without undo).
    //
    // Without a gate by m_rigidbodyDragActive: only one ImGui widget can have ActiveId at a
    // time, so the gate prevented no real re-snapshot and instead left the flag stuck when a
    // click did not reach editing (IsItemDeactivatedAfterEdit requires prior editing, and
    // DragFloat does not edit if the mouse does not move). With the flag stuck, the next edit—
    // even in ANOTHER GameObject—reused the stale snapshot, and Ctrl+Z wrote mass, gravity,
    // kinematic, drag, angular drag and constraints of the previous one to the new object.
    auto snapshotBefore = [&]() {
        m_rigidbodyDragActive  = true;
        m_rigidbodyDragOwnerId = id;
        m_rigidbodyBeforeEdit  = RigidbodyState{ rb->getMass(), rb->getUseGravity(), rb->getIsKinematic(),
                                                 rb->getDrag(), rb->getAngularDrag(), rb->getConstraints(),
                                                 rb->getCcd(), rb->getInterpolate() };
    };
    bool floatChanged = false;
    bool floatCommitted = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Mass", &m_editRbMass, 0.1f, 0.0001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Drag", &m_editRbDrag, 0.01f, 0.0f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Angular Drag", &m_editRbAngularDrag, 0.01f, 0.0f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    if (floatChanged) { rb->setMass(m_editRbMass); rb->setDrag(m_editRbDrag); rb->setAngularDrag(m_editRbAngularDrag); }
    // The owner guard covers the drag starting in one GameObject and committing while the
    // panel already draws another: without it the "before" of the first would be applied to
    // the second.
    if (m_rigidbodyDragActive && floatCommitted && m_rigidbodyDragOwnerId == id)
    {
        m_rigidbodyDragActive = false;
        if (ctx.scene)
            ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                "Rigidbody of '" + ctx.selected->name + "'", m_rigidbodyBeforeEdit, currentState(), applyRbState));
    }

    // --- Checkboxes: immediate command with before/after ---
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Use Gravity", &m_editRbUseGravity))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("Use Gravity of '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbUseGravity ? "enabled" : "disabled"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "Use Gravity of '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
    }
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Is Kinematic", &m_editRbKinematic))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("Is Kinematic of '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbKinematic ? "enabled" : "disabled"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "Is Kinematic of '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
    }
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Collision Detection (CCD)", &m_editRbCcd))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("CCD of '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbCcd ? "enabled" : "disabled"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "CCD of '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Continuous detection: keeps a fast body from passing through\n"
                              "thin geometry. Costs CPU; PhysX ignores it for kinematic bodies.");
    }
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Interpolate", &m_editRbInterpolate))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("Interpolate of '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbInterpolate ? "enabled" : "disabled"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "Interpolate of '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smooths the VISIBLE pose between fixed physics steps.\n"
                              "It does not change the simulation: raycasts and triggers see the real pose.");
    }

    // --- Constraints ---
    ImGui::TextUnformatted("Freeze Position");
    bool px = m_editRbConstraints & RB_FreezePositionX;
    bool py = m_editRbConstraints & RB_FreezePositionY;
    bool pz = m_editRbConstraints & RB_FreezePositionZ;
    bool rx = m_editRbConstraints & RB_FreezeRotationX;
    bool ry = m_editRbConstraints & RB_FreezeRotationY;
    bool rz = m_editRbConstraints & RB_FreezeRotationZ;
    bool cbChanged = false;
    RigidbodyState cbBefore = currentState();
    cbChanged |= ImGui::Checkbox("PX", &px); ImGui::SameLine();
    cbChanged |= ImGui::Checkbox("PY", &py); ImGui::SameLine();
    cbChanged |= ImGui::Checkbox("PZ", &pz);
    ImGui::TextUnformatted("Freeze Rotation");
    cbChanged |= ImGui::Checkbox("RX", &rx); ImGui::SameLine();
    cbChanged |= ImGui::Checkbox("RY", &ry); ImGui::SameLine();
    cbChanged |= ImGui::Checkbox("RZ", &rz);
    if (cbChanged)
    {
        uint32_t mask = 0;
        if (px) mask |= RB_FreezePositionX; if (py) mask |= RB_FreezePositionY; if (pz) mask |= RB_FreezePositionZ;
        if (rx) mask |= RB_FreezeRotationX; if (ry) mask |= RB_FreezeRotationY; if (rz) mask |= RB_FreezeRotationZ;
        m_editRbConstraints = mask;
        applyRbState(currentState());
        if (ctx.scene)
            ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                "Constraints of '" + ctx.selected->name + "' (Rigidbody)", cbBefore, currentState(), applyRbState));
    }

    if (ImGui::Button("Remove Rigidbody"))
    {
        if (auto col = ctx.selected->anyCollider(); col && ctx.physics)
            ctx.physics->detachRigidbody(col);
        ctx.selected->setRigidbody(nullptr);
        m_caches.rigidbody = nullptr;
        ctx.pushLog("Rigidbody component removed from '" + ctx.selected->name + "'");
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawCameraSection(EditorContext& ctx)
{
    // Hidden until Add is pressed: the section exists only if the component exists, and the
    // component only exists after Add (same early-return as drawRigidbodySection).
    if (!ctx.selected || !ctx.selected->hasCameraComponent()) { m_caches.camera = nullptr; return; }
    CameraComponent* cam = ctx.selected->getCameraComponent().get();
    if (m_caches.camera != cam)
    {
        m_editCamMode      = cam->getMode();
        m_editCamFov       = cam->getFov();
        m_editCamOrthoSize = cam->getOrthographicSize();
        m_editCamNear      = cam->getNear();
        m_editCamFar       = cam->getFar();
        m_caches.camera  = cam;
    }

    ImGui::Separator();
    if (!ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*   scene = ctx.scene;
    uint64_t id    = ctx.selected->id;

    // Applies a CameraState to the GameObject resolved by id (survives undo-of-delete).
    // Same pattern as applyRbState.
    auto applyCamState = [scene, id](const CameraState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasCameraComponent()) return;
        auto c = go->getCameraComponent();
        c->setMode(s.mode);
        // far before near: setNear clamps against the current far.
        c->setFar(s.farPlane);
        c->setNear(s.nearPlane);
        c->setFov(s.fov);
        c->setOrthographicSize(s.orthographicSize);
    };
    auto currentState = [&]() {
        return CameraState{ m_editCamMode, m_editCamFov, m_editCamOrthoSize, m_editCamNear, m_editCamFar };
    };

    // --- Combo of mode: immediate command with before/after ---
    {
        CameraState before = currentState();
        const char* modes[] = { "Perspective", "Orthographic" };
        int modeIdx = (m_editCamMode == CameraComponent::ProjectionMode::Orthographic) ? 1 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo("Projection", &modeIdx, modes, 2))
        {
            m_editCamMode = (modeIdx == 1) ? CameraComponent::ProjectionMode::Orthographic
                                            : CameraComponent::ProjectionMode::Perspective;
            applyCamState(currentState());
            ctx.pushLog(std::string("Projection of '") + ctx.selected->name + "' (Camera): " + modes[modeIdx]);
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<CameraState>>(
                    "Projection of '" + ctx.selected->name + "' (Camera)", before, currentState(), applyCamState));
        }
    }

    // --- Drag floats: snapshot when ANY activates, command when ANY deactivates (same
    // cumulative pattern as Rigidbody).
    //
    // Without a gate by m_cameraDragActive and with owner: same reason as in
    // drawRigidbodySection—the gate left the flag stuck when a click did not reach editing,
    // and the next drag on another GameObject committed the stale snapshot.
    auto snapshotBefore = [&]() {
        m_cameraDragActive  = true;
        m_cameraDragOwnerId = id;
        m_cameraBeforeEdit  = CameraState{ cam->getMode(), cam->getFov(), cam->getOrthographicSize(),
                                           cam->getNear(), cam->getFar() };
    };
    bool floatChanged = false;
    bool floatCommitted = false;

    // Only the field of the active mode is shown: showing the other would suggest it does
    // something, and it does not.
    if (m_editCamMode == CameraComponent::ProjectionMode::Orthographic)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        floatChanged |= ImGui::DragFloat("Size", &m_editCamOrthoSize, 1.0f, 0.001f, FLT_MAX, "%.3f");
        if (ImGui::IsItemActivated()) snapshotBefore();
        floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    }
    else
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        floatChanged |= ImGui::DragFloat("Field of View", &m_editCamFov, 0.5f, 1.0f, 179.0f, "%.1f");
        if (ImGui::IsItemActivated()) snapshotBefore();
        floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Near", &m_editCamNear, 0.1f, 0.001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Far", &m_editCamFar, 1.0f, 0.001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    if (floatChanged)
    {
        applyCamState(currentState());
        // The component clamps can have corrected the value (e.g. near above far): the cache
        // is re-synced so the widget shows what actually stayed saved, not what was dragged.
        m_editCamFov       = cam->getFov();
        m_editCamOrthoSize = cam->getOrthographicSize();
        m_editCamNear      = cam->getNear();
        m_editCamFar       = cam->getFar();
    }
    if (m_cameraDragActive && floatCommitted && m_cameraDragOwnerId == id)
    {
        m_cameraDragActive = false;
        if (ctx.scene)
            ctx.undo->push(std::make_unique<PropertyCommand<CameraState>>(
                "Camera of '" + ctx.selected->name + "'", m_cameraBeforeEdit, currentState(), applyCamState));
    }

    if (ImGui::Button("Remove Camera"))
    {
        // Goes through the undo stack like Add (see CameraComponentCommand): if Remove were not
        // undoable, removing the camera would be an irreversible loss.
        CameraState st = currentState();
        m_caches.camera = nullptr;
        ctx.pushLog("Camera component removed from '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Remove Camera from '" + ctx.selected->name + "'", id, /*add=*/false, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
        }
        else
        {
            ctx.selected->setCameraComponent(nullptr);
        }
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}

// The graph is NOT edited here: that is the Animator panel's (the canvas needs its own zoom/pan).
// This section only summarizes and gives the entry point.
void PropertiesPanel::drawAnimatorSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasAnimator()) return;
    auto anim = ctx.selected->getAnimator();

    if (!ImGui::TreeNodeEx("Animator", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("States: %d", (int)anim->states().size());
    ImGui::Text("Transitions: %d", (int)anim->transitions().size());
    if (anim->layerCount() > 1) ImGui::Text("Layers: %d (states and transitions: the base layer)", anim->layerCount());
    const int entry = anim->entryState();
    if (entry >= 0 && entry < (int)anim->states().size())
        ImGui::Text("Entry: %s", anim->states()[entry].name.c_str());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No entry state");

    if (!anim->parameters().empty())
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Parameters");
        for (const auto& p : anim->parameters())
            ImGui::BulletText("%s (%s)", p.name.c_str(), paramTypeLabel(p.type));
    }

    if (ImGui::Button("Open Animator") && ctx.openAnimator)
        ctx.openAnimator();

    ImGui::SameLine();
    if (ImGui::Button("Remove Animator"))
    {
        // Goes through the undo stack like Add (see AnimatorComponentCommand): the graph is
        // preserved in the command so Undo returns it whole.
        const uint64_t id = ctx.selected->id;
        AnimatorComponent st = *anim;
        ctx.pushLog("Animator component removed from '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<AnimatorComponentCommand>(
                *ctx.scene, "Remove Animator from '" + ctx.selected->name + "'", id, /*add=*/false, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
        }
        else
        {
            ctx.selected->setAnimator(nullptr);
        }
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawMeshSection(EditorContext& ctx)
{
    // Hidden by default: only drawn if it already has mesh, or if "Add > Mesh" was pressed for this
    // specific GameObject (m_meshAddRequestedFor).
    if (!ctx.selected->hasMesh() && m_meshAddRequestedFor != ctx.selected->id)
        return;

    ImGui::Separator();

    if (ctx.selected->hasMesh())
    {
        bool sectionOpen = ImGui::TreeNodeEx("Mesh", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            ImGui::Text("%s", ctx.selected->getMesh()->name.c_str());

            // Only the drawn: hidden does not reach the GPU (not scene, not shadows, not AO), but
            // physics, collisions and viewport selection still work the same. Works for static and
            // skinned, which share this section.
            Scene*         meshScene = ctx.scene;
            const uint64_t meshId    = ctx.selected->id;
            bool           visible   = ctx.selected->meshVisible;
            if (ImGui::Checkbox("Visible", &visible))
            {
                const bool before = ctx.selected->meshVisible;
                ctx.selected->meshVisible = visible;
                ctx.pushLog("Mesh of '" + ctx.selected->name + "' " +
                            (visible ? "visible" : "hidden"));
                if (meshScene && ctx.undo)
                {
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        "Visible of '" + ctx.selected->name + "'", before, visible,
                        [meshScene, meshId](const bool& v) {
                            if (GameObject* go = meshScene->findById(meshId)) go->meshVisible = v;
                        }));
                }
            }

            drawTexturesSection(ctx);

            ImGui::TreePop();
        }

        if (removeClicked && ctx.renderer && ctx.scene)
        {
            // Through the undo stack: the command removes the mesh, empties materialOverrides and
            // returns it all in a Ctrl+Z. The why of emptying, and the guard by hasMesh(), lives in
            // MeshComponentCommand (Command.h), which is what does it.
            auto cmd = std::make_unique<MeshComponentCommand>(
                *ctx.scene, ctx.renderer, "Remove Mesh from '" + ctx.selected->name + "'",
                *ctx.selected, /*add=*/false);
            cmd->execute();
            if (ctx.undo) ctx.undo->push(std::move(cmd));
            // Hides the section again after removing the mesh—you have to press "Add > Mesh" again
            // to open it.
            m_meshAddRequestedFor = 0;
            ctx.pushLog("Mesh component removed from '" + ctx.selected->name + "'");
        }

        return;
    }

    ImGui::Text("Mesh");
    // Same distribution as drawAssetDropBox: the gray warns, acceptOrImportAsset prevents. This box
    // does not come from that helper (it has its own drop zone), so BeginDisabled has to be set by
    // hand.
    ImGui::BeginDisabled(ctx.editingLocked);
    if (ImGui::Button("Browse..."))
    {
        m_meshDlgOpen  = true;
        // The owner is set HERE, when opening: by the time the dialog drains the selection may be
        // another (see m_meshDlgOwner).
        m_meshDlgOwner = ctx.selected->id;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        // Key without "##" prefix: Display() builds the window name as title+"##"+key; with
        // key="##AddMeshDlg" the result carried 4 hashes in a row ("Choose Model####AddMeshDlg"),
        // and ImGui treats "###" as a special ID separator (everything after determines the ID,
        // ignoring the rest)—it was calculated differently in window->ID than in the ID saved in
        // settings when persisting the layout, and the mismatch fired "Assertion failed:
        // settings->ID == window->ID" when resizing (when save is forced). The official IGFD
        // example uses plain keys (without "##"), like here.
        m_meshFileDialog->OpenDialog("AddMeshDlg", "Choose Model", ModelLoader::supportedModelFilter(), cfg);
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##MeshDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop a model here");
    // Edit veto while the load modal is active: new drops are not accepted until Load Scene
    // finishes (or is canceled).
    if (!ctx.editingLocked && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadMeshForSelected(ctx, ctx.selected->id,
                                std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_meshLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_meshLoadError.c_str());
}

void PropertiesPanel::drawTexturesSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasMesh()) return;

    const std::vector<const Material*> mats = materialsOfMesh(*ctx.selected);
    if (mats.empty()) return;

    // Expanded by default, like the Mesh TreeNode itself. Collapsed was practically invisible: the
    // material is not a separate component—there is no "Material" in the Add menu—so whoever looks
    // for it checks there, not inside Mesh, and a closed header under the Visible checkbox does not
    // say it is there. The cost is that the Mesh section is always taller, quite a bit more with a
    // skinned with several materials.
    //
    // "Material" and not "Textures": the section is no longer just textures since it adds the
    // Metallic/Roughness sliders below.
    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) return;

    // Captured ONCE, not read from ctx.selected in each callback: the drop and Clear are
    // synchronous (resolved this same frame, on the object being drawn right now), but Browse is
    // not—its result comes several frames later, when selection may have changed. All callbacks
    // below pass this id explicitly instead of checking ctx.selected again, so the six paths (drop
    // and browse of the three slots) use the same source of truth.
    const uint64_t ownerId = ctx.selected->id;

    struct SlotDesc { const char* nombre; MaterialTextureSlot slot; };
    static const SlotDesc kSlots[3] = {
        { "Albedo",             MaterialTextureSlot::Albedo },
        { "Normal Map",         MaterialTextureSlot::Normal },
        { "Metallic/Roughness", MaterialTextureSlot::Orm    },
    };

    for (int m = 0; m < (int)mats.size(); ++m)
    {
        // PushID by material: CollapsingHeader does NOT open its own ID scope (unlike BeginMenu),
        // so without this the three slots of material 0 and those of 1 collide and the drop of one
        // eats the other's.
        ImGui::PushID(m);
        if (mats.size() > 1)
            ImGui::Text("Material %d", m);

        for (const SlotDesc& d : kSlots)
        {
            // PushID by slot, inside the material one: the three "Browse..."/"Clear" buttons of one
            // material share names across slots and would collide the same way.
            ImGui::PushID(d.nombre);

            const std::string actual = currentTexturePath(*mats[(size_t)m], d.slot);
            ImGui::Text("%s: %s", d.nombre,
                        actual.empty() ? "None"
                                       : std::filesystem::path(actual).filename().string().c_str());

            const int materialIndex = m;
            const MaterialTextureSlot slot = d.slot;
            drawAssetDropBox(
                ctx, d.nombre, "Drop image here",
                [this, &ctx, ownerId, materialIndex, slot]() {
                    // No gate of its own: this callback can no longer run with the scene loading,
                    // because drawAssetDropBox disables the button for its 16 boxes—the gate that
                    // was here duplicated, while the other 15 were left without it.
                    m_textureDlgOpen     = true;
                    m_textureDlgOwner    = ownerId;
                    m_textureDlgMaterial = materialIndex;
                    m_textureDlgSlot     = slot;
                    IGFD::FileDialogConfig cfg;
                    cfg.path  = "assets";
                    cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                                ImGuiFileDialogFlags_HideColumnDate |
                                ImGuiFileDialogFlags_DisableThumbnailMode |
                                ImGuiFileDialogFlags_DisablePlaceMode;
                    // Key without "##": Display() builds the window name as title+"##"+key, and a
                    // "###" there breaks the ID saved by the layout (same reason documented in
                    // drawMeshSection for "AddMeshDlg").
                    m_textureFileDialog->OpenDialog("PickTextureDlg", "Choose image",
                                                     ".png,.jpg,.jpeg,.bmp,.tga", cfg);
                },
                [this, &ctx, ownerId, materialIndex, slot](const std::string& path) {
                    assignMaterialTexture(ctx, ownerId, materialIndex, slot, path);
                });

            ImGui::BeginDisabled(ctx.editingLocked || !hasOverride(*ctx.selected, materialIndex, slot));
            if (ImGui::Button("Clear"))
                assignMaterialTexture(ctx, ownerId, materialIndex, slot, "");
            ImGui::EndDisabled();

            ImGui::PopID();
        }

        {
            const std::string matAssetActual = currentMaterialAsset(*ctx.selected, m);
            ImGui::Text("Material asset: %s", matAssetActual.empty() ? "None"
                        : std::filesystem::path(matAssetActual).filename().string().c_str());
            drawAssetDropBox(
                ctx, "MatAsset", "Drop .mat here",
                [this, ownerId, m]() {
                    m_matAssetDlgOwner    = ownerId;
                    m_matAssetDlgMaterial = m;
                    m_matAssetDlgOpen     = true;
                    IGFD::FileDialogConfig cfg;
                    cfg.path = "assets";
                    m_matAssetFileDialog->OpenDialog("PickMatAssetDlg", "Choose material", ".mat", cfg);
                },
                [this, &ctx, ownerId, m](const std::string& path) {
                    assignMaterialAsset(ctx, ownerId, m, path);
                });
            ImGui::BeginDisabled(ctx.editingLocked || matAssetActual.empty());
            if (ImGui::Button("Clear##MatAsset")) assignMaterialAsset(ctx, ownerId, m, "");
            ImGui::EndDisabled();
        }

        // Metallic/Roughness of the material: two sliders, not another texture, so outside the
        // kSlots loop above but inside the same PushID(m)—without it, "Metallic"/"Roughness" of
        // material 0 and 1 would collide like the three texture buttons.
        {
            const Material& mat = *mats[(size_t)m];
            // SAME condition the two backends look at to decide if the map commands
            // (D3D12Renderer::addStaticMesh/rebuildStaticMesh, Renderer::createSharedGpuMesh), not
            // !path.empty(): a model with the ORM EMBEDDED (a .glb, metallicRoughnessPath empty but
            // embeddedMetallicRoughness full) has a map too, and with the check-only-path the
            // sliders came out enabled without warning even though both backends force 1.0 and
            // ignore the value—a widget that did nothing.
            const bool hasOrmMap = chooseTextureSource(mat.metallicRoughnessPath,
                                                        mat.embeddedMetallicRoughness) !=
                                   TextureSource::None;
            const int  materialIndex = m;

            // The two sliders go through m_materialFactorSlider not a bare SliderFloat: the value
            // is NOT written to the Material while dragging (only previewed on the GPU), and in
            // that case ImGui does not give the value on the frame it is released (see
            // DeferredSlider.h). With the bare SliderFloat the commit read a local that that frame
            // was worth the pre-drag: the command did not create, the slider went back to 0 and the
            // object stayed with the last previewed. The helper also reads the "before" before
            // drawing, which SliderFloat jumps to the value under the cursor in the same frame of
            // the click.
            ImGui::BeginDisabled(ctx.editingLocked || hasOrmMap);
            const DeferredSliderFloat::Result rm =
                m_materialFactorSlider.draw("Metallic", mat.metallic, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            if (hasOrmMap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A Metallic/Roughness map is assigned: the map wins, "
                                  "this slider has no effect while it stays assigned.");
            if (rm.activated)
            {
                m_materialFactorDragActive        = true;
                m_materialFactorDragOwnerId       = ownerId;
                m_materialFactorDragMaterialIndex = materialIndex;
                m_materialFactorDragSlot          = MaterialFactorSlot::Metallic;
                m_materialFactorDragBefore        = rm.begin;
            }
            // Owner guard, same reason as Audio Clip: the slider's ActiveId survives a selection
            // change mid-drag, and without comparing owner+index+slot the commit would apply to the
            // wrong object/material. The preview goes through the same guard: without it, a drag
            // started on one object would paint over the one selected mid-drag.
            const bool dragDeMetallic = m_materialFactorDragActive &&
                m_materialFactorDragOwnerId == ownerId &&
                m_materialFactorDragMaterialIndex == materialIndex &&
                m_materialFactorDragSlot == MaterialFactorSlot::Metallic;
            // The viewport follows the slider while dragging. Roughness goes as is: the setter
            // writes both, so pass it the one NOT being moved.
            if (rm.active && dragDeMetallic)
                previewMaterialFactors(ctx, ownerId, rm.value, mat.roughness);
            // Release without commit (a dry click, or a drag of another owner) leaves the GPU with
            // the last pushed: return what the Material says, which is what the drag never touched.
            if (rm.cancelled || (rm.committed && !dragDeMetallic))
                previewMaterialFactors(ctx, ownerId, mat.metallic, mat.roughness);
            if (rm.committed && dragDeMetallic)
            {
                m_materialFactorDragActive = false;
                if (!nearlyEqualF(m_materialFactorDragBefore, rm.value) && ctx.scene)
                {
                    // The command's "before" is the RAW override (the sentinel if there never was
                    // one), not m_materialFactorDragBefore (the EFFECTIVE value shown before the
                    // drag, used above only to know if something changed). With the effective as
                    // "before", undoing the first edit would write the FBX value AS the ACTIVE
                    // OVERRIDE—nodeToJson would serialize it, nailing the factor in the .scene even
                    // though the user never touched it.
                    const float beforeOverride =
                        currentFactorOverride(*ctx.selected, materialIndex, MaterialFactorSlot::Metallic);
                    auto cmd = std::make_unique<MaterialFactorCommand>(
                        *ctx.scene, ctx.renderer, "Metallic of '" + ctx.selected->name + "'",
                        ownerId, materialIndex, MaterialFactorSlot::Metallic,
                        beforeOverride, rm.value);
                    cmd->execute();
                    if (ctx.undo) ctx.undo->push(std::move(cmd));
                    ctx.pushLog("Metallic of '" + ctx.selected->name + "' changed");
                }
            }

            ImGui::BeginDisabled(ctx.editingLocked || hasOrmMap);
            const DeferredSliderFloat::Result rr =
                m_materialFactorSlider.draw("Roughness", mat.roughness, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            if (hasOrmMap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A Metallic/Roughness map is assigned: the map wins, "
                                  "this slider has no effect while it stays assigned.");
            if (rr.activated)
            {
                m_materialFactorDragActive        = true;
                m_materialFactorDragOwnerId       = ownerId;
                m_materialFactorDragMaterialIndex = materialIndex;
                m_materialFactorDragSlot          = MaterialFactorSlot::Roughness;
                m_materialFactorDragBefore        = rr.begin;
            }
            // Same trio as in Metallic, with roles reversed.
            const bool dragDeRoughness = m_materialFactorDragActive &&
                m_materialFactorDragOwnerId == ownerId &&
                m_materialFactorDragMaterialIndex == materialIndex &&
                m_materialFactorDragSlot == MaterialFactorSlot::Roughness;
            if (rr.active && dragDeRoughness)
                previewMaterialFactors(ctx, ownerId, mat.metallic, rr.value);
            if (rr.cancelled || (rr.committed && !dragDeRoughness))
                previewMaterialFactors(ctx, ownerId, mat.metallic, mat.roughness);
            if (rr.committed && dragDeRoughness)
            {
                m_materialFactorDragActive = false;
                if (!nearlyEqualF(m_materialFactorDragBefore, rr.value) && ctx.scene)
                {
                    // Same reason as the Metallic block: the RAW override, not the effective one.
                    const float beforeOverride =
                        currentFactorOverride(*ctx.selected, materialIndex, MaterialFactorSlot::Roughness);
                    auto cmd = std::make_unique<MaterialFactorCommand>(
                        *ctx.scene, ctx.renderer, "Roughness of '" + ctx.selected->name + "'",
                        ownerId, materialIndex, MaterialFactorSlot::Roughness,
                        beforeOverride, rr.value);
                    cmd->execute();
                    if (ctx.undo) ctx.undo->push(std::move(cmd));
                    ctx.pushLog("Roughness of '" + ctx.selected->name + "' changed");
                }
            }
        }

        ImGui::PopID();
    }

    if (!m_textureLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_textureLoadError.c_str());
}

// Push the factors to the GPU WHILE dragging, without touching the CPU Material and without going
// through the undo stack. Both deliberate:
//
// - Without touching the Material because the baseline (base*/base*Taken) is captured the FIRST
//    time applyMaterialOverrides hits the slot, and that happens when releasing. If the drag wrote
//    mat.metallic, that capture would take as "original" the mid-drag value instead of what the FBX
//    brings, and a later Clear would return some slider position.
// - Without a command because a drag is dozens of frames: one per frame would flood the history and
//    Ctrl+Z would have to be pressed fifty times to undo a gesture. The command pushes it on
//    commit, with the final value.
//
// Static only: in skinned the factors live per SUBMESH and there is no setter for that, so it still
// applies on release (see MaterialFactorCommand).
void PropertiesPanel::previewMaterialFactors(EditorContext& ctx, uint64_t ownerId,
                                              float metallic, float roughness)
{
    if (!ctx.renderer || !ctx.scene) return;
    GameObject* go = ctx.scene->findById(ownerId);
    if (!go || !go->hasMesh() || go->staticRenderIndex < 0) return;
    if (go->getSkinnedMesh()) return;
    ctx.renderer->setObjectMaterialFactors(static_cast<size_t>(go->staticRenderIndex),
                                          metallic, roughness);
}

void PropertiesPanel::assignMaterialTexture(EditorContext& ctx, uint64_t ownerId, int materialIndex,
                                             MaterialTextureSlot slot, const std::string& path)
{
    // ctx.scene may be nullptr (m_scene by default in EditorUI): without this guard, findById below
    // would dereference null.
    if (!ctx.scene) return;

    // Resolved by id, NEVER read from ctx.selected: the Browse dialog result comes several frames
    // after opening and none of this panel's dialogs is modal (zero ImGuiFileDialogFlags_Modal in
    // the whole file), so Hierarchy stays clickable while open. Without resolving by the id
    // captured when opening, the result would apply to the object selected AT THAT MOMENT, not the
    // one that opened the dialog.
    GameObject* go = ctx.scene->findById(ownerId);
    if (!go)
    {
        // The object was deleted (or the snapshot is from an undo/redo in between) while the dialog
        // was still open: without this warning the choice would be silently discarded and Browse
        // would seem to have done nothing.
        ctx.logModule("Mesh", "Could not apply the texture: the object no longer exists");
        return;
    }
    if (!go->hasMesh() || ctx.editingLocked) return;

    // Second line of defense against the same scenario above: the dialog opened on a material of
    // ONE object (e.g. index 2 of a skinned with 3 materials) and by the time it closes, ownerId
    // resolves to ANOTHER object with fewer materials. setMaterialTextureOverride does not validate
    // the index (tolerates it on purpose for overrides of scenes with more materials than the mesh
    // loaded right now—see the tests for out-of-range index), so without this cut here, in the
    // panel, an override would be written that applyMaterialOverrides silently ignores today but
    // that nodeToJson serializes anyway, and that would fire the "out-of-range index" warning on
    // the next scene load.
    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (materialIndex < 0 || materialIndex >= (int)mats.size())
    {
        ctx.logModule("Mesh", "Could not apply the texture: the material no longer exists on '"
                              + go->name + "'");
        return;
    }

    // Clear enters with empty path and without checking extension: there is no extension to
    // validate, and what is validated is what is ASSIGNED, not what is removed.
    if (!path.empty())
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        static const std::set<std::string> kImagenes =
            { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
        if (!kImagenes.count(ext))
        {
            m_textureLoadError = "Unsupported format: " + ext;
            return;
        }
    }

    const std::string antes = currentOverride(*go, materialIndex, slot);
    // Same texture that was already there: nothing to do. Without this cut, reassigning the same
    // would stack an inert command (a Ctrl+Z that apparently does not respond) and fire a complete
    // reupload to GPU for nothing. Same criterion as setButtonAssetPath.
    if (antes == path) return;
    m_textureLoadError.clear();

    auto cmd = std::make_unique<MaterialTextureCommand>(
        *ctx.scene, ctx.renderer,
        (path.empty() ? "Remove texture from '" : "Texture of '") + go->name + "'",
        go->id, materialIndex, slot, antes, path);
    cmd->execute();
    if (ctx.undo) ctx.undo->push(std::move(cmd));

    ctx.pushLog((path.empty() ? "Texture removed from '" : "Texture assigned to '")
                + go->name + "'");
}

void PropertiesPanel::assignMaterialAsset(EditorContext& ctx, uint64_t ownerId, int materialIndex,
                                          const std::string& path)
{
    if (!ctx.scene) return;
    GameObject* go = ctx.scene->findById(ownerId);
    if (!go) { ctx.logModule("Mesh", "Could not link the material: the object no longer exists"); return; }
    if (!go->hasMesh() || ctx.editingLocked) return;

    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (materialIndex < 0 || materialIndex >= (int)mats.size())
    {
        ctx.logModule("Mesh", "Could not link the material: the material no longer exists on '" + go->name + "'");
        return;
    }
    if (!path.empty())
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".mat")
        {
            m_textureLoadError = "Unsupported format: " + ext;
            return;
        }
    }
    const std::string antes = currentMaterialAsset(*go, materialIndex);
    if (antes == path) return;
    m_textureLoadError.clear();

    auto cmd = std::make_unique<MaterialAssetCommand>(
        *ctx.scene, ctx.renderer,
        (path.empty() ? "Unlink material from '" : "Material of '") + go->name + "'",
        go->id, materialIndex, antes, path);
    cmd->execute();
    if (ctx.undo) ctx.undo->push(std::move(cmd));
    ctx.pushLog((path.empty() ? "Material unlinked from '" : "Material linked to '") + go->name + "'");
}

void PropertiesPanel::drawMeshDialog(EditorContext& ctx)
{
    // Executes every frame independently of ctx.selected/hasMesh(): if not drained here, changing
    // selection (or deselecting) while the dialog is open leaves m_meshDlgOpen stuck at true
    // forever. m_meshFileDialog is its own instance (not shared with m_audioFileDialog), so
    // resizing this popup does not touch the internal state of the Audio dialog or vice versa.
    if (m_meshDlgOpen && m_meshFileDialog->Display("AddMeshDlg"))
    {
        if (m_meshFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_meshFileDialog->GetFilePathName()))
                loadMeshForSelected(ctx, m_meshDlgOwner, resolved->string());
        }
        m_meshFileDialog->Close();
        m_meshDlgOpen = false;
    }

    // Same draining, same reason, for the Textures dialog: own instance (m_textureFileDialog), so
    // resizing this popup does not touch the internal state of the Mesh one or the Audio one.
    if (m_textureDlgOpen && m_textureFileDialog->Display("PickTextureDlg"))
    {
        if (m_textureFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_textureFileDialog->GetFilePathName()))
                assignMaterialTexture(ctx, m_textureDlgOwner, m_textureDlgMaterial, m_textureDlgSlot,
                                      resolved->string());
        }
        m_textureFileDialog->Close();
        m_textureDlgOpen = false;
    }

    // Same draining for the "Material asset" dialog: own instance (m_matAssetFileDialog), same
    // reason as the ones above.
    if (m_matAssetDlgOpen && m_matAssetFileDialog->Display("PickMatAssetDlg"))
    {
        if (m_matAssetFileDialog->IsOk())
            assignMaterialAsset(ctx, m_matAssetDlgOwner, m_matAssetDlgMaterial,
                                m_matAssetFileDialog->GetFilePathName());
        m_matAssetFileDialog->Close();
        m_matAssetDlgOpen = false;
    }
}

void PropertiesPanel::drawAudioClipSection(EditorContext& ctx)
{
    // Hidden by default: only drawn if it already has AudioClip, or if "Add > Audio Clip" was
    // pressed for this specific GameObject (m_audioClipAddRequestedFor).
    if (!ctx.selected->hasAudioClip() && m_audioClipAddRequestedFor != ctx.selected->id)
        return;

    ImGui::Separator();

    if (ctx.selected->hasAudioClip())
    {
        auto& clip = ctx.selected->getAudioClip();
        bool sectionOpen = ImGui::TreeNodeEx("Audio Clip", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            std::string fname = std::filesystem::path(clip->getPath()).filename().string();
            ImGui::Text("%s", fname.c_str());

            // Load failure is not known when adding the clip: FMOD reads in its thread
            // (FMOD_NONBLOCKING) and the error appears frames later, so it is checked every time
            // the section is drawn instead of cached. Without this, a broken asset looked the same
            // as a good one here and you only noticed because nothing played.
            if (clip->hasLoadError())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Could not load");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("'%s': missing file, unsupported format or corrupt data",
                                      clip->getPath().c_str());
            }

            ImGui::BeginDisabled(ctx.audio == nullptr);
            if (ImGui::Button("Play"))
            {
                glm::vec3 worldPos(ctx.selected->worldTransform[3]);
                clip->play(worldPos);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                clip->stop();
            ImGui::EndDisabled();

            // The three checkboxes share the same undo path as the sliders (a
            // PropertyCommand<AudioClipState> with the whole state), but commit instantly instead
            // of on release: a checkbox has no drag. Matters most on Loop and Is 3D, which RELOAD
            // the sound (is3D/loop go in FMOD_MODE) and cut anything playing: without undo, that
            // cut was irreversible.
            const AudioClipState toggleBefore = audioClipStateOf(*clip);
            bool toggled = false;

            bool loop = clip->getLoop();
            if (ImGui::Checkbox("Loop", &loop))
            {
                clip->setLoop(loop);
                toggled = true;
            }

            bool is3D = clip->getIs3D();
            if (ImGui::Checkbox("Is 3D?", &is3D))
            {
                clip->setIs3D(is3D);
                toggled = true;
            }

            bool mute = clip->getMute();
            if (ImGui::Checkbox("Mute", &mute))
            {
                clip->setMute(mute);
                toggled = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mutes without losing the volume: unchecking it restores the "
                                  "previous one. Saved in the scene.");

            bool playOnAwake = clip->getPlayOnAwake();
            if (ImGui::Checkbox("Play On Awake", &playOnAwake))
            {
                clip->setPlayOnAwake(playOnAwake);
                toggled = true;
            }

            // Output bus. Shares the undo path of the checkboxes (the same AudioClipState), because
            // it also commits outright. The order of the array follows the enum AudioBus; what is
            // saved in the scene is the NAME, so reordering it here does not break any project.
            const char* kBusNames[] = { "Master", "Music", "SFX" };
            int busIdx = static_cast<int>(clip->getBus());
            if (ImGui::Combo("Bus", &busIdx, kBusNames, IM_ARRAYSIZE(kBusNames)))
            {
                clip->setBus(static_cast<AudioBus>(busIdx));
                toggled = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Group the sound plays through. Master scales the other "
                                  "two.\nOnly affects the NEXT playback: the group is "
                                  "chosen when the voice starts.");

            // Load mode. Unlike the bus, this DOES reload the sound (goes in FMOD_MODE) and cuts
            // what was playing.
            const char* kLoadModeNames[] = { "Sample (in RAM)", "Stream (from disk)" };
            int loadModeIdx = static_cast<int>(clip->getLoadMode());
            if (ImGui::Combo("Load Mode", &loadModeIdx, kLoadModeNames, IM_ARRAYSIZE(kLoadModeNames)))
            {
                clip->setLoadMode(static_cast<AudioLoadMode>(loadModeIdx));
                toggled = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sample: fully decompressed into RAM. Starts instantly and "
                                  "allows several voices at once.\nStream: read from disk on "
                                  "the fly, uses very little memory, but can only play ONE "
                                  "instance at a time.\nUse Stream for music and long ambiences, "
                                  "Sample for effects.\nChanging it reloads the sound and cuts whatever "
                                  "is playing.");

            // Attenuation curve. Like Load Mode, goes in FMOD_MODE and reloads. Only drawn in 3D:
            // in 2D there is no distance attenuation to shape, like the min/max distances below.
            if (is3D)
            {
                const char* kRolloffNames[] = { "Inverse (realistic)", "Linear",
                                                 "Linear Square" };
                int rolloffIdx = static_cast<int>(clip->getRolloff());
                if (ImGui::Combo("Rolloff", &rolloffIdx, kRolloffNames, IM_ARRAYSIZE(kRolloffNames)))
                {
                    clip->setRolloff(static_cast<AudioRolloff>(rolloffIdx));
                    toggled = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Shape of the falloff between Min and Max distance.\nInverse: falls "
                                      "fast up close and stretches out far away (the default, the "
                                      "most realistic).\nLinear: falls at a constant rate and goes fully "
                                      "silent at Max distance.\nLinear Square: in between, also "
                                      "silent at Max.\nChanging it reloads the sound.");
            }

            if (toggled && ctx.scene && ctx.undo)
            {
                const uint64_t ownerId = ctx.selected->id;
                Scene* sc = ctx.scene;
                ctx.undo->push(std::make_unique<PropertyCommand<AudioClipState>>(
                    "Audio Clip of '" + ctx.selected->name + "'",
                    toggleBefore, audioClipStateOf(*clip),
                    [sc, ownerId](const AudioClipState& s) { applyAudioClipState(*sc, ownerId, s); }));
            }

            // --- Volume / Pitch: snapshot when any activates, one command on release. Values are
            // written live while dragging (so you hear the change), and the command only serves so
            // Ctrl+Z returns the entire drag at once.
            //
            // SliderFloat (unlike DragFloat) jumps to the value under the cursor in the SAME frame
            // IsItemActivated() becomes true, so the "before" cannot be re-read from the component
            // after drawing the widget: by then it is already the new value. That is why the reads
            // are hoisted here, before the sliders, and the snapshot uses these variables instead
            // of re-reading clip->getVolume()/getPitch().
            const float volumeBefore = clip->getVolume();
            const float pitchBefore  = clip->getPitch();
            const float minDistBefore = clip->getMinDistance();
            const float maxDistBefore = clip->getMaxDistance();
            const float spreadBefore  = clip->getSpread();
            const float panBefore     = clip->getStereoPan();
            const float dopplerBefore = clip->getDopplerLevel();
            float volume  = volumeBefore;
            float pitch   = pitchBefore;
            float minDist = minDistBefore;
            float maxDist = maxDistBefore;

            const uint64_t clipOwnerId = ctx.selected->id;
            Scene* scene = ctx.scene;

            bool activated = false;
            bool committed = false;

            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f"))
                clip->setVolume(volume);
            activated |= ImGui::IsItemActivated();
            committed |= ImGui::IsItemDeactivatedAfterEdit();

            if (ImGui::SliderFloat("Pitch", &pitch, 0.5f, 2.0f, "%.2f"))
                clip->setPitch(pitch);
            activated |= ImGui::IsItemActivated();
            committed |= ImGui::IsItemDeactivatedAfterEdit();

            // Attenuation distances: only make sense in 3D (in 2D FMOD does not attenuate by
            // distance), so they are not drawn with is3D unchecked. The value stays saved in the
            // component: when is3D is marked again the edits reappear.
            if (is3D)
            {
                if (ImGui::SliderFloat("Min distance", &minDist, 0.1f, 50.0f, "%.2f"))
                    clip->setMinDistance(minDist);
                activated |= ImGui::IsItemActivated();
                committed |= ImGui::IsItemDeactivatedAfterEdit();

                if (ImGui::SliderFloat("Max distance", &maxDist, 1.0f, 1000.0f, "%.1f"))
                    clip->setMaxDistance(maxDist);
                activated |= ImGui::IsItemActivated();
                // The clamp of max >= min is done by the component's own setter (not the UI), so on
                // release it is already applied.
                committed |= ImGui::IsItemDeactivatedAfterEdit();

                // Spread and doppler are voice properties: they do not reload anything, but they
                // are read when starting playback, so moving them with something already playing
                // does not take effect until the next Play. They go through the same undo path as
                // the other sliders.
                float spread = spreadBefore;
                if (ImGui::SliderFloat("Spread", &spread, 0.0f, 360.0f, "%.0f deg"))
                    clip->setSpread(spread);
                activated |= ImGui::IsItemActivated();
                committed |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Widens the 3D source. 0 keeps it a point; high "
                                      "values make it enveloping up close.");

                float doppler = dopplerBefore;
                if (ImGui::SliderFloat("Doppler", &doppler, 0.0f, 5.0f, "%.2f"))
                    clip->setDopplerLevel(doppler);
                activated |= ImGui::IsItemActivated();
                committed |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How much the relative velocity between the "
                                      "source and the listener shifts the pitch.\n0 turns it off (default). Only "
                                      "works in Play: velocities are not computed in Edit Mode.");
            }
            else
            {
                // Manual pan: ONLY in 2D. In 3D the source position decides it, and offering it
                // here would make it seem you could force it.
                float pan = panBefore;
                if (ImGui::SliderFloat("Stereo pan", &pan, -1.0f, 1.0f, "%.2f"))
                    clip->setStereoPan(pan);
                activated |= ImGui::IsItemActivated();
                committed |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("-1 fully left, 0 centered, 1 fully right. "
                                      "2D clips only.");
            }

            // Without a gate by m_audioDragActive: only one ImGui widget can have ActiveId at a
            // time, so the gate adds nothing but a bug: IsItemDeactivatedAfterEdit() requires real
            // editing, and a click that activates the slider without moving it never reaches
            // "committed", leaving the flag stuck with a stale "before" that the next edit—even on
            // another GameObject—would reuse.
            if (activated)
            {
                m_audioDragActive  = true;
                m_audioDragOwnerId = clipOwnerId;
                // Current state of the clip, but with the CONTINUOUS ones replaced by the hoisted
                // reads from before drawing: a SliderFloat already jumped to the value under the
                // cursor in the frame it activates, so reading them from the component would give
                // the new value.
                m_audioDragBefore              = audioClipStateOf(*clip);
                m_audioDragBefore.volume       = volumeBefore;
                m_audioDragBefore.pitch        = pitchBefore;
                m_audioDragBefore.minDistance  = minDistBefore;
                m_audioDragBefore.maxDistance  = maxDistBefore;
                m_audioDragBefore.spread       = spreadBefore;
                m_audioDragBefore.stereoPan    = panBefore;
                m_audioDragBefore.dopplerLevel = dopplerBefore;
            }

            // Owner guard: the ActiveId of an ImGui slider is preserved while the mouse stays
            // pressed, even if selection changes mid-drag (Hierarchy, keyboard shortcut or a
            // script) and the panel passes to drawing the AudioClip of ANOTHER GameObject. Since
            // the widget id ("Volume"/"Pitch") is the same in both, ImGui would still consider it
            // the same drag and the final commit would apply to that other object; this id prevents
            // applying a "before" that belongs to the original GameObject.
            if (committed && m_audioDragActive && m_audioDragOwnerId == clipOwnerId)
            {
                m_audioDragActive = false;
                const AudioClipState before = m_audioDragBefore;
                const AudioClipState after  = audioClipStateOf(*clip);

                if (!nearlyEqualF(before.volume, after.volume) ||
                    !nearlyEqualF(before.pitch,  after.pitch)  ||
                    !nearlyEqualF(before.minDistance, after.minDistance) ||
                    !nearlyEqualF(before.maxDistance, after.maxDistance) ||
                    !nearlyEqualF(before.spread, after.spread) ||
                    !nearlyEqualF(before.stereoPan, after.stereoPan) ||
                    !nearlyEqualF(before.dopplerLevel, after.dopplerLevel))
                {
                    if (ctx.scene)
                        ctx.undo->push(std::make_unique<PropertyCommand<AudioClipState>>(
                            "Audio Clip of '" + ctx.selected->name + "'", before, after,
                            [scene, clipOwnerId](const AudioClipState& s) {
                                applyAudioClipState(*scene, clipOwnerId, s);
                            }));
                }
            }

            ImGui::TreePop();
        }

        if (removeClicked)
        {
            // Through the undo stack, not bare: the snapshot is taken BEFORE releasing the
            // component, so Ctrl+Z returns the clip with its volume, pitch and distances, not a
            // freshly created one with defaults.
            if (ctx.scene && ctx.undo && ctx.audio)
            {
                auto cmd = std::make_unique<AudioClipComponentCommand>(
                    *ctx.scene, *ctx.audio,
                    "Remove Audio Clip from '" + ctx.selected->name + "'", ctx.selected->id,
                    /*add=*/false, clip->getPath(), audioClipStateOf(*clip));
                cmd->execute();
                ctx.undo->push(std::move(cmd));
            }
            else
            {
                ctx.selected->setAudioClip(nullptr);
            }
            // Hides the section again after removing the clip—you have to press "Add > Audio Clip"
            // again to open it.
            m_audioClipAddRequestedFor = 0;
            ctx.pushLog("Audio Clip component removed from '" + ctx.selected->name + "'");
        }

        return;
    }

    ImGui::Text("Audio Clip");
    // Two reasons for the gray, and neither hides the other: without AudioManager there is nothing
    // to load, and with the scene loading the choice would be discarded when draining
    // (acceptOrImportAsset).
    ImGui::BeginDisabled(ctx.audio == nullptr || ctx.editingLocked);
    if (ImGui::Button("Browse..."))
    {
        m_audioDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        // Plain key without "##" (same reason documented in drawMeshSection for AddMeshDlg: with
        // "##" prefix the concatenated title generated 4 hashes in a row and broke the ID persisted
        // by ImGui).
        m_audioFileDialog->OpenDialog("AddAudioDlg", "Choose Audio", ".wav,.mp3,.ogg,.flac", cfg);
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    // Same veto as the mesh drop: no new drops while the scene loads.
    if (!ctx.editingLocked && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadAudioClipForSelected(ctx, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_audioLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_audioLoadError.c_str());
}

void PropertiesPanel::drawAudioClipDialog(EditorContext& ctx)
{
    // Executes every frame independently of ctx.selected/hasAudioClip(): if not drained here,
    // changing selection while the dialog is open leaves m_audioDlgOpen stuck at true (same reason
    // as drawMeshDialog).
    if (m_audioDlgOpen && m_audioFileDialog->Display("AddAudioDlg"))
    {
        if (m_audioFileDialog->IsOk())
        {
            if (auto resolved = acceptOrImportAsset(ctx, m_audioFileDialog->GetFilePathName()))
                loadAudioClipForSelected(ctx, resolved->string());
        }
        m_audioFileDialog->Close();
        m_audioDlgOpen = false;
    }
}

void PropertiesPanel::drawScriptsSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.scriptManager || !ctx.selected->hasScripts()) return;

    ScriptComponent* toRemove = nullptr;

    for (auto& compPtr : ctx.selected->getScripts())
    {
        ScriptComponent* comp = compPtr.get();
        ImGui::PushID(comp);

        // TreeNodeEx (narrow label) not CollapsingHeader (full-width frame): the header would
        // overlap the "x" button and eat its click. Same pattern as collider sections.
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((comp->scriptName + " (Script)").c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 65.0f);
        if (ImGui::SmallButton("Edit"))
            ctx.openScript(ctx.scriptManager->scriptsDirPath() / (comp->scriptName + ".lua"));
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (ImGui::SmallButton("x"))
            toRemove = comp;

        if (open)
        {
            if (!ctx.scriptManager->hasClass(comp->scriptName))
            {
                const std::string* err = ctx.scriptManager->getCompileError(comp->scriptName);
                if (err)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Compilation error:\n%s", err->c_str());
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Script not found: %s.lua", comp->scriptName.c_str());
                // Overrides untouched (spec: data is not lost)
            }
            else
            {
                const ScriptClass& cls = ctx.scriptManager->getRegistry().at(comp->scriptName);
                const bool live = ctx.isPlaying && comp->instance.valid();

                for (const ScriptProp& prop : cls.props)
                {
                    // Shown value: live instance > override > default
                    ScriptValue value = prop.defaultValue;
                    if (auto it = comp->overrides.find(prop.name); it != comp->overrides.end())
                        value = it->second;
                    if (live)
                    {
                        sol::object lv = comp->instance[prop.name];
                        if (lv.get_type() == sol::type::number)       value = lv.as<double>();
                        else if (lv.get_type() == sol::type::boolean) value = lv.as<bool>();
                        else if (lv.get_type() == sol::type::string)  value = lv.as<std::string>();
                    }

                    const std::string label = prettyPropLabel(prop.name);
                    bool edited = false;

                    if (std::holds_alternative<double>(value))
                    {
                        double d = std::get<double>(value);
                        if (prop.isInteger)
                        {
                            int i = static_cast<int>(d);
                            if (ImGui::DragInt(label.c_str(), &i)) { value = double(i); edited = true; }
                        }
                        else
                        {
                            float f = static_cast<float>(d);
                            if (ImGui::DragFloat(label.c_str(), &f, 0.1f)) { value = double(f); edited = true; }
                        }
                    }
                    else if (std::holds_alternative<bool>(value))
                    {
                        bool b = std::get<bool>(value);
                        if (ImGui::Checkbox(label.c_str(), &b)) { value = b; edited = true; }
                    }
                    else
                    {
                        char buf[256] = {};
                        const std::string& s = std::get<std::string>(value);
                        copyToBuffer(buf, s);
                        if (ImGui::InputText(label.c_str(), buf, sizeof(buf)))
                        { value = std::string(buf); edited = true; }
                    }

                    if (edited)
                    {
                        comp->overrides[prop.name] = value;
                        if (live)
                        {
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, value);
                        }
                        ctx.pushLog("Script '" + comp->scriptName + "." + prop.name +
                                "' changed on '" + ctx.selected->name + "'");
                    }
                }

                if (ImGui::Button("Reset"))
                {
                    comp->overrides.clear();
                    if (live)
                    {
                        // Reapplies defaults from the .lua to the live instance
                        for (const ScriptProp& prop : cls.props)
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, prop.defaultValue);
                    }
                    ctx.pushLog("Script '" + comp->scriptName + "' reset to defaults on '" +
                            ctx.selected->name + "'");
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (toRemove)
    {
        if (ctx.isPlaying) ctx.scriptManager->callOnDestroy(*toRemove);
        const std::string name = toRemove->scriptName;
        ctx.selected->removeScript(toRemove);
        ctx.pushLog("Script component '" + name + "' removed from '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawAddComponentButton(EditorContext& ctx)
{
    ImGui::Separator();
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        bool alreadyHasAny = ctx.selected->hasAnyCollider();
        ImGui::BeginDisabled(alreadyHasAny);

        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setBoxCollider(ctx.physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                ctx.selected->worldTransform, /*dynamic=*/false));
            // Opaque owner = GameObject, so TriggerEvent.other can resolve it.
            ctx.selected->getBoxCollider()->setOwner(ctx.selected);
            m_caches.box = nullptr;
            ctx.pushLog("Box Collider component added to '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setSphereCollider(ctx.physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getSphereCollider()->setOwner(ctx.selected);
            m_caches.sphere = nullptr;
            ctx.pushLog("Sphere Collider component added to '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setCapsuleCollider(ctx.physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getCapsuleCollider()->setOwner(ctx.selected);
            m_caches.capsule = nullptr;
            ctx.pushLog("Capsule Collider component added to '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setPlaneCollider(ctx.physics->createPlaneColliderComponent(
                glm::vec3(0.0f), ctx.selected->worldTransform));
            ctx.selected->getPlaneCollider()->setOwner(ctx.selected);
            m_caches.plane = nullptr;
            ctx.pushLog("Plane Collider component added to '" + ctx.selected->name + "'");
        }

        ImGui::EndDisabled();

        // Rigidbody: needs a collider that brings the shape; hidden if it already has one or if
        // there is no collider to hook it to.
        if (!ctx.selected->hasRigidbody() && ctx.selected->hasAnyCollider())
        {
            if (ImGui::Selectable("Rigidbody") && ctx.physics)
            {
                auto rb = std::make_shared<Rigidbody>();
                ctx.selected->setRigidbody(rb);
                if (auto col = ctx.selected->anyCollider())
                    ctx.physics->attachRigidbody(col, rb);
                m_caches.rigidbody = nullptr;
                ctx.pushLog("Rigidbody component added to '" + ctx.selected->name + "'");
            }
        }

        bool alreadyHasMesh = ctx.selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = ctx.selected->id;
        ImGui::EndDisabled();

        bool alreadyHasAudio = ctx.selected->hasAudioClip();
        ImGui::BeginDisabled(alreadyHasAudio);
        if (ImGui::Selectable("Audio Clip") && !alreadyHasAudio)
            m_audioClipAddRequestedFor = ctx.selected->id;
        ImGui::EndDisabled();

        // Audio Listener: at most one per scene, same criterion as the camera—the gate asks
        // Scene::findAudioListener, not a flag of its own, and the existing one is not touched
        // (neither deleted nor stolen).
        GameObject* existingListener = ctx.scene ? ctx.scene->findAudioListener() : nullptr;
        ImGui::BeginDisabled(existingListener != nullptr);
        if (ImGui::Selectable("Audio Listener") && !existingListener)
        {
            if (ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<AudioListenerComponentCommand>(
                    *ctx.scene, "Add Audio Listener to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, /*enabled=*/true);
                cmd->execute();
                ctx.undo->push(std::move(cmd));
            }
            else
            {
                ctx.selected->setAudioListener(std::make_shared<AudioListenerComponent>());
            }
            ctx.pushLog("Audio Listener component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (existingListener && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("There is already an Audio Listener in the scene ('%s'): remove it from there "
                              "before adding another", existingListener->name.c_str());

        // Reverb Zone: no scene invariant (several fit, and FMOD mixes the ones that overlap), so
        // the only gate is not putting two on the same object.
        const bool alreadyHasReverb = ctx.selected->hasReverbZone();
        ImGui::BeginDisabled(alreadyHasReverb);
        if (ImGui::Selectable("Reverb Zone") && !alreadyHasReverb)
        {
            ctx.selected->setReverbZone(std::make_shared<ReverbZoneComponent>());
            ctx.pushLog("Reverb Zone component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Canvas: root of 2D UI. No scene invariant (several fit), so the only gate is not adding
        // two to the same object. Goes through the undo stack like the camera: the state of the 10
        // fields is preserved in an Add-undo-redo.
        const bool alreadyHasCanvas = ctx.selected->hasCanvas();
        ImGui::BeginDisabled(alreadyHasCanvas);
        if (ImGui::Selectable("Canvas") && !alreadyHasCanvas && ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<CanvasComponentCommand>(
                *ctx.scene, "Add Canvas to '" + ctx.selected->name + "'", ctx.selected->id,
                /*add=*/true, CanvasComponent{});
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            ctx.pushLog("Canvas component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // UI components: only exist hanging from a Canvas, so a GameObject without Canvas does not
        // see them. The list is empty until the widgets are implemented; the gate is already the
        // definitive one.
        if (uiComponentsAvailable(ctx.selected))
        {
            ImGui::Separator();
            ImGui::TextDisabled("UI");
            ImGui::BeginDisabled(ctx.selected->hasButton());
            if (ImGui::Selectable("Button") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ButtonComponentCommand>(
                    *ctx.scene, "Add Button to '" + ctx.selected->name + "'", ctx.selected->id,
                    /*add=*/true, ButtonComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Button component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasText());
            if (ImGui::Selectable("Text") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<TextComponentCommand>(
                    *ctx.scene, "Add Text to '" + ctx.selected->name + "'", ctx.selected->id,
                    /*add=*/true, TextComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Text component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            // Panel first: it is the background, and in the sync it is placed below everything.
            ImGui::BeginDisabled(ctx.selected->hasPanel());
            if (ImGui::Selectable("Panel") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<PanelComponentCommand>(
                    *ctx.scene, "Add Panel to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, PanelComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Panel component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasImage());
            if (ImGui::Selectable("Image") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ImageComponentCommand>(
                    *ctx.scene, "Add Image to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ImageComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Image component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasSlider());
            if (ImGui::Selectable("Slider") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<SliderComponentCommand>(
                    *ctx.scene, "Add Slider to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, SliderComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Slider component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasCheckbox());
            if (ImGui::Selectable("Checkbox") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<CheckboxComponentCommand>(
                    *ctx.scene, "Add Checkbox to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, CheckboxComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Checkbox component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasToggle());
            if (ImGui::Selectable("Toggle") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ToggleComponentCommand>(
                    *ctx.scene, "Add Toggle to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ToggleComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Toggle component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasScrollbar());
            if (ImGui::Selectable("Scroll Bar") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ScrollbarComponentCommand>(
                    *ctx.scene, "Add Scroll Bar to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ScrollbarComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Scroll Bar component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasInputField());
            if (ImGui::Selectable("Input Field") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<InputFieldComponentCommand>(
                    *ctx.scene, "Add Input Field to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, InputFieldComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Input Field component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasDropdown());
            if (ImGui::Selectable("Dropdown") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<DropdownComponentCommand>(
                    *ctx.scene, "Add Dropdown to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, DropdownComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Dropdown component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasScrollView());
            if (ImGui::Selectable("Scroll View") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ScrollViewComponentCommand>(
                    *ctx.scene, "Add Scroll View to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ScrollViewComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Scroll View component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasProgressBar());
            if (ImGui::Selectable("Progress Bar") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ProgressBarComponentCommand>(
                    *ctx.scene, "Add Progress Bar to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ProgressBarComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Progress Bar component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            // Layout: the only one of the four that does not draw anything. Without another UI
            // component on the object it mounts its own container, so it is also valid on a bare
            // GameObject that just groups.
            ImGui::BeginDisabled(ctx.selected->hasLayout());
            if (ImGui::Selectable("Layout") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<LayoutComponentCommand>(
                    *ctx.scene, "Add Layout to '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, LayoutComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Layout component added to '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();
        }

        // Camera: at most one per scene, and the gate asks the only source of truth
        // (Scene::findCamera), not a flag of its own. Disabled and not hidden because that is what
        // the other items in this popup do—and the tooltip says WHO has it already, because a gray
        // item with no explanation is a dead end.
        GameObject* existingCamera = ctx.scene ? ctx.scene->findCamera() : nullptr;
        ImGui::BeginDisabled(existingCamera != nullptr);
        if (ImGui::Selectable("Camera") && !existingCamera && ctx.scene && ctx.undo)
        {
            CameraComponent defaults;
            CameraState st{ defaults.getMode(), defaults.getFov(), defaults.getOrthographicSize(),
                            defaults.getNear(), defaults.getFar() };
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Add Camera to '" + ctx.selected->name + "'", ctx.selected->id, /*add=*/true, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            m_caches.camera = nullptr;
            ctx.pushLog("Camera component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (existingCamera && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("There is already a camera in the scene ('%s')", existingCamera->name.c_str());

        // Reflection Probe: no uniqueness invariant (as many as fit in memory), so the only gate is
        // not adding two to one object. When added, the Renderer detects it in the next frame,
        // creates its cubemaps and bakes it once: no need to press Bake to see it.
        const bool alreadyHasProbe = ctx.selected->hasReflectionProbe();
        ImGui::BeginDisabled(alreadyHasProbe);
        if (ImGui::Selectable("Reflection Probe") && !alreadyHasProbe)
        {
            ctx.selected->setReflectionProbe(std::make_shared<ReflectionProbeComponent>());
            ctx.pushLog("Reflection Probe component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Light: no scene uniqueness invariant (several of the same type fit), so the only gate is
        // not adding two to the same object. The first MAX_LIGHTS in scene order are the ones that
        // reach the shader.
        const bool alreadyHasLight = ctx.selected->hasLight();
        ImGui::BeginDisabled(alreadyHasLight);
        if (ImGui::Selectable("Light") && !alreadyHasLight)
        {
            ctx.selected->setLight(std::make_shared<LightComponent>());
            ctx.pushLog("Light component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Animator: from property clips (C14) it works for ANY object—a door, a light, a camera.
        // With skinned mesh it adds clips from the model; without it, only property clips.
        const bool canAnimate     = true;
        const bool alreadyHasAnim = ctx.selected->hasAnimator();
        ImGui::BeginDisabled(!canAnimate || alreadyHasAnim);
        if (ImGui::Selectable("Animator") && canAnimate && !alreadyHasAnim)
        {
            // Outside the undo stack, like Script: the user builds the graph by direct mutation
            // (without commands), and a reflected Ctrl+Z after Add would pop the
            // AnimatorComponentCommand and empty the entire graph via setAnimator(nullptr). Remove
            // does go through the stack (see below in drawAnimatorSection) because there is no such
            // risk there.
            ctx.selected->setAnimator(std::make_shared<AnimatorComponent>());
            ctx.pushLog("Animator component added to '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (!canAnimate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The Animator needs a skinned mesh (the clips come from the FBX)");

        if (ctx.scriptManager)
        {
            if (ImGui::BeginMenu("Script"))
            {
                for (const auto& entry : ctx.scriptManager->getRegistry())
                {
                    const std::string& name = entry.first;
                    if (ImGui::MenuItem(name.c_str()))
                    {
                        auto comp = std::make_unique<ScriptComponent>(name, ctx.selected);
                        ctx.selected->addScript(std::move(comp));
                        // In Play the lifecycle instantiates and fires Awake/Start in the next
                        // update (started == false).
                        ctx.pushLog("Script component '" + name + "' added to '" + ctx.selected->name + "'");
                    }
                }
                if (!ctx.scriptManager->getRegistry().empty())
                    ImGui::Separator();
                if (ImGui::MenuItem("New Script..."))
                {
                    m_newScriptTargetId = ctx.selected->id;
                    m_newScriptNameBuffer[0] = '\0';
                    m_newScriptError.clear();
                    m_openNewScriptPopup = true;
                }
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }

    drawNewScriptPopup(ctx);
}

void PropertiesPanel::drawNewScriptPopup(EditorContext& ctx)
{
    if (m_openNewScriptPopup)
    {
        ImGui::OpenPopup("New Script");
        m_openNewScriptPopup = false;
    }

    if (!ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Script name (without .lua):");
    ImGui::InputText("##NewScriptName", m_newScriptNameBuffer, sizeof(m_newScriptNameBuffer));
    if (!m_newScriptError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_newScriptError.c_str());

    if (ImGui::Button("Create"))
    {
        const std::string name = m_newScriptNameBuffer;

        // Valid Lua identifier: letter or '_' + alphanumerics/'_'—the file name is also the name of
        // the class's global table.
        bool validName = !name.empty() &&
            (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
        for (size_t i = 1; validName && i < name.size(); ++i)
            validName = std::isalnum(static_cast<unsigned char>(name[i])) || name[i] == '_';

        const std::filesystem::path path = ctx.scriptManager->scriptsDirPath() / (name + ".lua");

        if (!validName)
            m_newScriptError = "Invalid name: must start with a letter or '_', then letters, digits or '_'";
        else if (ctx.scriptManager->hasClass(name) || std::filesystem::exists(path))
            m_newScriptError = "A script with that name already exists";
        else
        {
            std::ofstream file(path);
            if (!file)
                m_newScriptError = "Could not create the file at " + path.string();
            else
            {
                file << name << " = {\n"
                     << "    -- Serializable properties (shown in the editor)\n"
                     << "    speed = 1\n"
                     << "}\n\n"
                     << "function " << name << ":Start()\n"
                     << "end\n\n"
                     << "function " << name << ":Update(dt)\n"
                     << "end\n";
                file.close();

                if (ctx.scriptManager->loadScript(path))
                {
                    ctx.openScript(path);

                    // The GameObject may have been deleted while the popup was open—resolve by id,
                    // which is the only thing that tells "still alive" from "another object
                    // recycled its address" (see m_newScriptTargetId). The traverse comparing
                    // pointers that was here did not tell them apart, and the script was added to
                    // the newly arrived one.
                    GameObject* target = ctx.scene && m_newScriptTargetId
                                             ? ctx.scene->findById(m_newScriptTargetId)
                                             : nullptr;
                    if (target)
                    {
                        target->addScript(
                            std::make_unique<ScriptComponent>(name, target));
                        ctx.pushLog("Script '" + name + "' created and added to '" +
                                target->name + "'");
                    }
                    else
                        ctx.pushLog("Script '" + name + "' created (the GameObject no longer exists)");
                    ImGui::CloseCurrentPopup();
                }
                else
                    m_newScriptError = "The script did not compile (see Log)";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace DonTopo
