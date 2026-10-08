#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/PrefabOverrideTracker.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/Sphere.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Renderer/Capsule.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include <imgui.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace {

// Valid name: non-empty after trim, only alphanumerics/space/_/-/. (no
// control characters or symbols that could break asset paths or the UI).
bool isValidGameObjectName(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    if (begin == std::string::npos)
        return false;

    for (size_t i = begin; i <= end; ++i)
    {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c) && c != ' ' && c != '_' && c != '-' && c != '.')
            return false;
    }
    return true;
}

std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// Moves dragged to the position of target within the children list of
// target->parent (or to the end of target->children if target is the root: this way
// it can never end up as a sibling of the root or outside its subtree).
void moveGameObject(DonTopo::GameObject* dragged, DonTopo::GameObject* target)
{
    using DonTopo::GameObject;

    if (!dragged || !target || dragged == target || !dragged->parent)
        return; // the root (no parent) cannot be dragged

    bool cycle = false;
    dragged->traverse([&](GameObject* go) { if (go == target) cycle = true; });
    if (cycle)
        return; // do not drop a node inside its own subtree

    GameObject* destParent;
    ptrdiff_t destIndex;
    if (!target->parent)
    {
        destParent = target;
        destIndex  = static_cast<ptrdiff_t>(destParent->children.size());
    }
    else
    {
        destParent = target->parent;
        auto it = std::find_if(destParent->children.begin(), destParent->children.end(),
            [target](const std::unique_ptr<GameObject>& c) { return c.get() == target; });
        destIndex = it - destParent->children.begin();
    }

    GameObject* srcParent = dragged->parent;
    auto srcIt = std::find_if(srcParent->children.begin(), srcParent->children.end(),
        [dragged](const std::unique_ptr<GameObject>& c) { return c.get() == dragged; });
    ptrdiff_t srcIndex = srcIt - srcParent->children.begin();

    std::unique_ptr<GameObject> moved = std::move(*srcIt);
    srcParent->children.erase(srcIt);

    if (srcParent == destParent && srcIndex < destIndex)
        --destIndex; // the gap left by the erase shifts the following indices

    moved->parent = destParent;
    destParent->children.insert(destParent->children.begin() + destIndex, std::move(moved));
}

// The parent moveGameObject would give `dragged` when dropped on target.
DonTopo::GameObject* moveDestinationParent(DonTopo::GameObject* target)
{
    return target && target->parent ? target->parent : target;
}

// Inside an open BeginDragDropTarget: accepts a Content Browser asset only
// when it is a .prefab (the peek keeps other assets from being highlighted
// or eaten here). The dropped absolute path in `path`.
bool acceptPrefabAssetDrop(std::string& path)
{
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (!peek || !peek->IsDataType("DT_ASSET_PATH") || !peek->Data) return false;
    std::string ext = std::filesystem::path(static_cast<const char*>(peek->Data)).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".prefab") return false;
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH");
    if (!payload) return false;
    path = static_cast<const char*>(payload->Data);
    return true;
}

} // namespace

namespace DonTopo {

void ScenePanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    m_selectionDeletedThisFrame = false;
    if (!m_open) return;
    ImGui::Begin("Scene", &m_open);
    // The root is not drawn as a node: the list directly shows its
    // children, root is still the real parent underneath (same behavior
    // of create/delete/rename/reorder as they already had).
    if (sceneRoot)
        for (const auto& child : sceneRoot->children)
            drawNode(ctx, child.get());

    // Empty space after the list: dropping here re-attaches the dragged node
    // as a direct child of the root (equivalent to dropping on the root row
    // from before, now that this row no longer exists).
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked())
        ctx.selected = nullptr; // click on an empty area deselects
    // Reparent veto while the loading modal is active (hierarchy editing).
    // The tree rendering and the selection continue: only moving is vetoed.
    if (!ctx.editingLocked && sceneRoot && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = sceneRoot;
        }
        // A .prefab from the Content Browser on the empty space: a new
        // instance at the scene root (spec §5.2).
        std::string prefabPath;
        if (acceptPrefabAssetDrop(prefabPath))
            dropPrefab(ctx, prefabPath, nullptr);
        ImGui::EndDragDropTarget();
    }

    bool canDelete = ctx.selected && ctx.selected->parent != nullptr;
    bool canRename = ctx.selected && ctx.selected->parent != nullptr;

    if (ImGui::IsWindowFocused() && canDelete && ImGui::IsKeyPressed(ImGuiKey_Delete))
        requestDelete(ctx, ctx.selected);
    if (ImGui::IsWindowFocused() && canRename && ImGui::IsKeyPressed(ImGuiKey_F2))
        beginRename(ctx.selected);

    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
        {
            GameObject* created = sceneRoot->addChild("GameObject");
            ctx.pushLog("GameObject '" + created->name + "' created");

            if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
            {
                uint64_t parentId = sceneRoot->id;
                size_t index = sceneRoot->children.size() - 1;
                nlohmann::json snapshot = ctx.scene->subtreeToJson(created);
                ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
                    *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
                    "Create '" + created->name + "'", parentId, index, std::move(snapshot)));
            }
        }
        // Visible only if there is no camera in the scene yet; the invariant is
        // decided by Scene::findCamera, not by a flag of this panel.
        if (ctx.scene && !ctx.scene->findCamera())
        {
            if (ImGui::MenuItem("Create Camera") && sceneRoot)
                createCamera(ctx, sceneRoot);
        }
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(ctx, sceneRoot, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(ctx, sceneRoot, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(ctx, sceneRoot, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(ctx, sceneRoot, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename", nullptr, false, canRename))
            beginRename(ctx.selected);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canDelete))
            requestDelete(ctx, ctx.selected);
        ImGui::EndPopup();
    }

    // Run the deletion after walking the whole tree: doing it earlier
    // would invalidate the children range-fors in progress on the call stack.
    if (m_pendingDelete)
    {
        GameObject* target = m_pendingDelete;
        m_pendingDelete = nullptr;

        // The selection may be the target itself or a descendant of it;
        // it has to be checked before deleting the subtree (afterwards it no longer exists).
        bool selectionInSubtree = false;
        target->traverse([&](GameObject* go) {
            if (go == ctx.selected) selectionInSubtree = true;
        });

        ctx.pushLog("GameObject '" + target->name + "' deleted");

        // Snapshot for Undo, taken BEFORE touching anything.
        bool canUndoDelete = ctx.scene && ctx.physics && ctx.audio && ctx.renderer && target->parent;
        uint64_t parentId = 0;
        size_t index = 0;
        nlohmann::json snapshot;
        std::string deletedName = target->name;
        if (canUndoDelete)
        {
            parentId = target->parent->id;
            auto& siblings = target->parent->children;
            auto it = std::find_if(siblings.begin(), siblings.end(),
                [target](const std::unique_ptr<GameObject>& c) { return c.get() == target; });
            index = static_cast<size_t>(it - siblings.begin());
            snapshot = ctx.scene->subtreeToJson(target);
        }

        // The GPU is now released by Scene::removeGameObject through its listener, further
        // down. It used to be released HERE, that is, before running Lua's OnDestroy:
        // the object was still alive for the script but already without its resources.
        // Now it is released afterwards, which is the correct order.

        // Without this, deleting from the editor in Play skips OnDestroy and leaves
        // dead pointers in the alive-set until the next update
        // (use-after-free window via hot reload).
        if (ctx.isPlaying && ctx.scriptManager)
        {
            // Snapshot before calling Lua: OnDestroy can add
            // components and invalidate the iteration.
            std::vector<ScriptComponent*> subtreeScripts;
            target->traverse([&](GameObject* n) {
                for (auto& s : n->getScripts())
                    subtreeScripts.push_back(s.get());
            });
            for (ScriptComponent* s : subtreeScripts)
                ctx.scriptManager->callOnDestroy(*s);
        }

        assert(ctx.scene && "EditorContext::scene must be set (see Renderer::setScene) before deleting GameObjects");
        ctx.scene->removeGameObject(target);
        if (ctx.scriptManager)
            ctx.scriptManager->rebuildAliveSet();
        if (selectionInSubtree)
        {
            ctx.selected = nullptr;
            m_selectionDeletedThisFrame = true;
        }

        if (canUndoDelete)
        {
            ctx.undo->push(std::make_unique<DeleteGameObjectCommand>(
                *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
                "Delete '" + deletedName + "'", parentId, index, std::move(snapshot)));
        }
    }

    if (m_pendingMoveSource && m_pendingMoveTarget)
    {
        GameObject* dragged = m_pendingMoveSource;
        GameObject* target  = m_pendingMoveTarget;
        m_pendingMoveSource = nullptr;
        m_pendingMoveTarget = nullptr;

        bool canUndoMove = ctx.scene && dragged->parent;
        uint64_t id = 0, oldParentId = 0;
        size_t oldIndex = 0;
        std::string draggedName;
        if (canUndoMove)
        {
            id = dragged->id;
            oldParentId = dragged->parent->id;
            draggedName = dragged->name;
            auto& oldSiblings = dragged->parent->children;
            auto it = std::find_if(oldSiblings.begin(), oldSiblings.end(),
                [dragged](const std::unique_ptr<GameObject>& c) { return c.get() == dragged; });
            oldIndex = static_cast<size_t>(it - oldSiblings.begin());
        }

        // Prefab structure (Edit Mode; Play restores the scene on Stop): a
        // linked node keeps its place, and no instance goes inside another.
        std::string refusal;
        if (!ctx.isPlaying && dragged->parent)
            if (GameObject* destParent = moveDestinationParent(target))
                refusal = prefabMoveRefusal(*dragged, *destParent);
        if (!refusal.empty())
        {
            ctx.pushLog(refusal);
            canUndoMove = false;
        }
        else
            moveGameObject(dragged, target);

        if (canUndoMove && dragged->parent)
        {
            uint64_t newParentId = dragged->parent->id;
            auto& newSiblings = dragged->parent->children;
            auto it = std::find_if(newSiblings.begin(), newSiblings.end(),
                [dragged](const std::unique_ptr<GameObject>& c) { return c.get() == dragged; });
            size_t newIndex = static_cast<size_t>(it - newSiblings.begin());

            // moveGameObject() can be a no-op (drop on itself, on one of its own
            // descendants, or dragged without a parent); if nothing changed,
            // do not pollute the stack with a phantom command.
            if (!(newParentId == oldParentId && newIndex == oldIndex))
            {
                ctx.undo->push(std::make_unique<ReparentCommand>(
                    *ctx.scene, "Move '" + draggedName + "'", id,
                    oldParentId, oldIndex, newParentId, newIndex));
            }
        }
    }

    if (m_openRenamePopup)
    {
        ImGui::OpenPopup("Rename GameObject");
        m_openRenamePopup = false;
    }
    if (ImGui::BeginPopupModal("Rename GameObject", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        bool enterPressed = ImGui::InputText("##renameInput", m_renameBuffer, sizeof(m_renameBuffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        bool accept = ImGui::Button("Accept") || enterPressed;
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel");

        if (accept)
        {
            std::string newName = trim(m_renameBuffer);
            if (m_renameTarget && isValidGameObjectName(newName))
            {
                std::string oldName  = m_renameTarget->name;
                m_renameTarget->name = newName;
                ctx.pushLog("GameObject renamed: '" + oldName + "' -> '" + newName + "'");

                if (ctx.scene && newName != oldName)
                {
                    Scene* scene = ctx.scene;
                    uint64_t id = m_renameTarget->id;
                    ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                        "Rename '" + oldName + "' a '" + newName + "'", oldName, newName,
                        [scene, id](const std::string& n) {
                            GameObject* go = scene->findById(id);
                            if (go) go->name = n;
                        }));
                }
            }
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
        else if (cancel)
        {
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void ScenePanel::beginRename(GameObject* node)
{
    if (!node || !node->parent)
        return; // the root cannot be renamed

    m_renameTarget = node;
    std::string current = node->name.empty() ? "GameObject" : node->name;
    std::strncpy(m_renameBuffer, current.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_openRenamePopup = true;
}

void ScenePanel::createBasicShape(EditorContext& ctx, GameObject* parent, const std::string& name,
                                   std::shared_ptr<Mesh> mesh)
{
    if (!parent || !ctx.renderer || !mesh)
        return;

    GameObject* go = parent->addChild(name);
    go->staticRenderIndex = ctx.renderer->addStaticMesh(*mesh);
    go->setMesh(std::move(mesh));
    ctx.pushLog("GameObject '" + go->name + "' created");

    if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
    {
        uint64_t parentId = parent->id;
        size_t index = parent->children.size() - 1;
        nlohmann::json snapshot = ctx.scene->subtreeToJson(go);
        ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
            *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
            "Create '" + go->name + "'", parentId, index, std::move(snapshot)));
    }
}

void ScenePanel::createCamera(EditorContext& ctx, GameObject* parent)
{
    if (!parent || !ctx.scene) return;

    GameObject* go = parent->addChild("Camera");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    ctx.pushLog("GameObject '" + go->name + "' with Camera created");

    // Same pattern as createBasicShape: the snapshot is taken AFTER assembling
    // the component, so Undo/Redo rebuilds it whole.
    if (ctx.physics && ctx.audio && ctx.renderer && ctx.undo)
    {
        uint64_t parentId = parent->id;
        size_t index = parent->children.size() - 1;
        nlohmann::json snapshot = ctx.scene->subtreeToJson(go);
        ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
            *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
            "Create '" + go->name + "'", parentId, index, std::move(snapshot)));
    }
}

void ScenePanel::drawNode(EditorContext& ctx, GameObject* node)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (node->children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
    if (node == ctx.selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label = node->name.empty() ? "GameObject" : node->name;
    // Prefab nodes (spec §5.3): blue, red when the template is missing or
    // failed; added children keep the normal color. The id is the pointer,
    // so the color never changes the widget's identity.
    const PrefabNodeState prefabState = prefabNodeState(*node, ctx.prefabs);
    const bool colored = prefabState != PrefabNodeState::None;
    if (colored)
        ImGui::PushStyleColor(ImGuiCol_Text, prefabState == PrefabNodeState::Missing
                                                 ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f)
                                                 : ImVec4(0.48f, 0.72f, 1.0f, 1.0f));
    bool open = ImGui::TreeNodeEx((const void*)node, flags, "%s", label.c_str());
    if (colored)
        ImGui::PopStyleColor();
    if (colored && prefabState == PrefabNodeState::Missing && node->prefabLink &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Prefab '%s' is missing or failed to load; the instance keeps its stored state",
                          node->prefabLink->path.c_str());
    if (ImGui::IsItemClicked())
        ctx.selected = node;

    // Drag: the root (parent == nullptr) cannot be dragged. Also vetoed
    // while the loading modal is active (hierarchy editing).
    if (!ctx.editingLocked && node->parent && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("DT_GAMEOBJECT", &node, sizeof(GameObject*));
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
    }
    // Drop: dropping on any node (including the root) repositions the
    // dragged one; moveGameObject already blocks cycles and "leaving" the root.
    if (!ctx.editingLocked && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = node;
        }
        // A .prefab dropped on a node: a new instance as its child.
        std::string prefabPath;
        if (acceptPrefabAssetDrop(prefabPath))
            dropPrefab(ctx, prefabPath, node);
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem())
    {
        // Only on an instance root (spec §5.3), first so they are easy to find.
        if (node->prefabLink && node->parent)
        {
            drawPrefabMenuItems(ctx, node);
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Create GameObject"))
        {
            GameObject* created = node->addChild("GameObject");
            ctx.pushLog("GameObject '" + created->name + "' created");

            if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
            {
                uint64_t parentId = node->id;
                size_t index = node->children.size() - 1;
                nlohmann::json snapshot = ctx.scene->subtreeToJson(created);
                ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
                    *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
                    "Create '" + created->name + "'", parentId, index, std::move(snapshot)));
            }
        }
        // Same gate as the window menu: the camera can hang from
        // any node, but there can only be one.
        if (ctx.scene && !ctx.scene->findCamera())
        {
            if (ImGui::MenuItem("Create Camera"))
                createCamera(ctx, node);
        }
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(ctx, node, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(ctx, node, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(ctx, node, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(ctx, node, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        bool canModify = node->parent != nullptr;
        if (ImGui::MenuItem("Rename", nullptr, false, canModify))
            beginRename(node);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canModify))
            requestDelete(ctx, node);
        ImGui::EndPopup();
    }

    if (open)
    {
        for (const auto& child : node->children)
            drawNode(ctx, child.get());
        ImGui::TreePop();
    }
}

void ScenePanel::requestDelete(EditorContext& ctx, GameObject* node)
{
    if (!node) return;
    // Edit Mode only: what is deleted in Play comes back on Stop, and the
    // override model (what this protects) is not tracked there.
    if (!ctx.isPlaying && !canDeleteNode(*node))
    {
        ctx.pushLog("Unpack the prefab to delete this object ('" + node->name + "')");
        return;
    }
    m_pendingDelete = node;
}

void ScenePanel::dropPrefab(EditorContext& ctx, const std::string& assetPath, GameObject* parent)
{
    if (ctx.editingLocked)
    {
        ctx.pushLog("Scene load in progress: the prefab was not instantiated");
        return;
    }
    if (!ctx.prefabs || !ctx.instantiatePrefab)
    {
        ctx.pushLog("Instantiate: prefabs need an open project");
        return;
    }
    const std::optional<std::string> key = prefabKeyForAsset(assetPath, *ctx.prefabs);
    if (!key)
    {
        ctx.pushLog("Instantiate: '" + assetPath + "' is not a prefab inside the project");
        return;
    }
    // Said here, before any wait for the template; InstantiatePrefabCommand
    // refuses it too, for any other caller.
    if (parent && prefabInstanceRoot(parent))
    {
        ctx.pushLog("Instantiate: '" + parent->name + "' is part of a prefab instance "
                    "(nested prefabs are not supported)");
        return;
    }
    // Resolved by id at the end of the frame (the scene root is 0).
    ctx.instantiatePrefab(*key, parent && parent->parent ? parent->id : 0);
}

void ScenePanel::drawPrefabMenuItems(EditorContext& ctx, GameObject* node)
{
    if (ImGui::MenuItem("Unpack Prefab"))
    {
        if (!ctx.runPrefabCommand)
            ctx.pushLog("Unpack Prefab: prefabs need an open project");
        else
        {
            const uint64_t id = node->id;
            ctx.runPrefabCommand([id](const PrefabCommandContext& pc) -> std::unique_ptr<PrefabCommand> {
                return std::make_unique<UnpackPrefabCommand>(pc, id);
            });
        }
    }
    if (ImGui::MenuItem("Select Prefab Asset") && ctx.revealAsset)
    {
        // Link paths are project-relative.
        const std::filesystem::path root = ctx.prefabs ? std::filesystem::path(ctx.prefabs->assetRoot())
                                         : ctx.project  ? ctx.project->root()
                                                        : std::filesystem::path();
        ctx.revealAsset(root / std::filesystem::path(node->prefabLink->path));
    }
}

} // namespace DonTopo
