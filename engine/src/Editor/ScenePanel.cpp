#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/Renderer.h"
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

namespace {

// Nombre válido: no vacío tras trim, solo alfanuméricos/espacio/_/-/. (sin
// caracteres de control ni símbolos que puedan romper rutas de asset o UI).
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

// Mueve dragged pa la posición de target dentro de la lista de hijos de
// target->parent (o al final de target->children si target es el root: así
// nunca puede quedar como hermano del root ni fuera de su subárbol).
void moveGameObject(DonTopo::GameObject* dragged, DonTopo::GameObject* target)
{
    using DonTopo::GameObject;

    if (!dragged || !target || dragged == target || !dragged->parent)
        return; // root (sin parent) no se puede arrastrar

    bool cycle = false;
    dragged->traverse([&](GameObject* go) { if (go == target) cycle = true; });
    if (cycle)
        return; // no soltar un nodo dentro de su propio subárbol

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
        --destIndex; // el hueco dejado por el erase desplaza los índices siguientes

    moved->parent = destParent;
    destParent->children.insert(destParent->children.begin() + destIndex, std::move(moved));
}

} // namespace

namespace DonTopo {

void ScenePanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    m_selectionDeletedThisFrame = false;
    if (!m_open) return;
    ImGui::Begin("Scene", &m_open);
    // El root no se dibuja como nodo: la lista muestra directamente sus
    // hijos, root sigue siendo el padre real por debajo (mismo comportamiento
    // de create/delete/rename/reorder que ya tenían).
    if (sceneRoot)
        for (const auto& child : sceneRoot->children)
            drawNode(ctx, child.get());

    // Espacio vacío tras la lista: soltar aquí reengancha el nodo arrastrado
    // como hijo directo del root (equivalente a soltar sobre la fila root
    // de antes, ahora que esa fila ya no existe).
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked())
        ctx.selected = nullptr; // clic en zona vacía deselecciona
    if (sceneRoot && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = sceneRoot;
        }
        ImGui::EndDragDropTarget();
    }

    bool canDelete = ctx.selected && ctx.selected->parent != nullptr;
    bool canRename = ctx.selected && ctx.selected->parent != nullptr;

    if (ImGui::IsWindowFocused() && canDelete && ImGui::IsKeyPressed(ImGuiKey_Delete))
        m_pendingDelete = ctx.selected;
    if (ImGui::IsWindowFocused() && canRename && ImGui::IsKeyPressed(ImGuiKey_F2))
        beginRename(ctx.selected);

    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
        {
            GameObject* created = sceneRoot->addChild("GameObject");
            ctx.pushLog("GameObject '" + created->name + "' creado");

            if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
            {
                uint64_t parentId = sceneRoot->id;
                size_t index = sceneRoot->children.size() - 1;
                nlohmann::json snapshot = ctx.scene->subtreeToJson(created);
                ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
                    *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
                    "Crear '" + created->name + "'", parentId, index, std::move(snapshot)));
            }
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
            m_pendingDelete = ctx.selected;
        ImGui::EndPopup();
    }

    // Ejecutar el borrado tras recorrer todo el árbol: hacerlo antes
    // invalidaría los for-range de children en curso en la pila de llamadas.
    if (m_pendingDelete)
    {
        GameObject* target = m_pendingDelete;
        m_pendingDelete = nullptr;

        // La selección puede ser el propio target o un descendiente suyo;
        // hay que comprobarlo antes de borrar el subárbol (después ya no existe).
        bool selectionInSubtree = false;
        target->traverse([&](GameObject* go) {
            if (go == ctx.selected) selectionInSubtree = true;
        });

        ctx.pushLog("GameObject '" + target->name + "' eliminado");

        // Snapshot pa Undo, tomado ANTES de tocar nada (onDelete libera GPU
        // pero no cambia los datos de Scene que subtreeToJson serializa).
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

        if (ctx.onDelete)
            ctx.onDelete(target);

        // Sin esto, borrar desde el editor en Play salta OnDestroy y deja
        // punteros muertos en el alive-set hasta el siguiente update
        // (ventana de use-after-free vía hot reload).
        if (ctx.isPlaying && ctx.scriptManager)
        {
            // Snapshot antes de llamar a Lua — OnDestroy puede añadir
            // componentes e invalidar la iteración.
            std::vector<ScriptComponent*> subtreeScripts;
            target->traverse([&](GameObject* n) {
                for (auto& s : n->getScripts())
                    subtreeScripts.push_back(s.get());
            });
            for (ScriptComponent* s : subtreeScripts)
                ctx.scriptManager->callOnDestroy(*s);
        }

        assert(ctx.scene && "EditorContext::scene debe estar asignado (ver Renderer::setScene) antes de borrar GameObjects");
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
                "Borrar '" + deletedName + "'", parentId, index, std::move(snapshot)));
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

        moveGameObject(dragged, target);

        if (canUndoMove && dragged->parent)
        {
            uint64_t newParentId = dragged->parent->id;
            auto& newSiblings = dragged->parent->children;
            auto it = std::find_if(newSiblings.begin(), newSiblings.end(),
                [dragged](const std::unique_ptr<GameObject>& c) { return c.get() == dragged; });
            size_t newIndex = static_cast<size_t>(it - newSiblings.begin());

            // moveGameObject() puede ser un no-op (drop en sí mismo, en un
            // descendiente propio, o dragged sin parent) — si nada cambió,
            // no ensuciar el stack con un comando fantasma.
            if (!(newParentId == oldParentId && newIndex == oldIndex))
            {
                ctx.undo->push(std::make_unique<ReparentCommand>(
                    *ctx.scene, "Mover '" + draggedName + "'", id,
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
                ctx.pushLog("GameObject renombrado: '" + oldName + "' -> '" + newName + "'");

                if (ctx.scene && newName != oldName)
                {
                    Scene* scene = ctx.scene;
                    uint64_t id = m_renameTarget->id;
                    ctx.undo->push(std::make_unique<PropertyCommand<std::string>>(
                        "Renombrar '" + oldName + "' a '" + newName + "'", oldName, newName,
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
        return; // root no se puede renombrar

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
    ctx.pushLog("GameObject '" + go->name + "' creado");

    if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
    {
        uint64_t parentId = parent->id;
        size_t index = parent->children.size() - 1;
        nlohmann::json snapshot = ctx.scene->subtreeToJson(go);
        ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
            *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
            "Crear '" + go->name + "'", parentId, index, std::move(snapshot)));
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
    bool open = ImGui::TreeNodeEx((const void*)node, flags, "%s", label.c_str());
    if (ImGui::IsItemClicked())
        ctx.selected = node;

    // Drag: el root (parent == nullptr) no se puede arrastrar.
    if (node->parent && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("DT_GAMEOBJECT", &node, sizeof(GameObject*));
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
    }
    // Drop: soltar sobre cualquier nodo (incluido el root) reposiciona el
    // arrastrado; moveGameObject ya bloquea ciclos y "salir" del root.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = node;
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Create GameObject"))
        {
            GameObject* created = node->addChild("GameObject");
            ctx.pushLog("GameObject '" + created->name + "' creado");

            if (ctx.scene && ctx.physics && ctx.audio && ctx.renderer)
            {
                uint64_t parentId = node->id;
                size_t index = node->children.size() - 1;
                nlohmann::json snapshot = ctx.scene->subtreeToJson(created);
                ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
                    *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
                    "Crear '" + created->name + "'", parentId, index, std::move(snapshot)));
            }
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
            m_pendingDelete = node;
        ImGui::EndPopup();
    }

    if (open)
    {
        for (const auto& child : node->children)
            drawNode(ctx, child.get());
        ImGui::TreePop();
    }
}

} // namespace DonTopo
