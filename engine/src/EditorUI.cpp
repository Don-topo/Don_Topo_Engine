#include "DonTopo/EditorUI.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/Camera.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/ModelLoader.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>

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

EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
{
}

EditorUI::~EditorUI() = default;

void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawAudioClipDialog();
    drawContentBrowser();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::drawScene(GameObject* sceneRoot)
{
    ImGui::Begin("Scene");
    // El root no se dibuja como nodo: la lista muestra directamente sus
    // hijos, root sigue siendo el padre real por debajo (mismo comportamiento
    // de create/delete/rename/reorder que ya tenían).
    if (sceneRoot)
        for (const auto& child : sceneRoot->children)
            drawSceneNode(child.get());

    // Espacio vacío tras la lista: soltar aquí reengancha el nodo arrastrado
    // como hijo directo del root (equivalente a soltar sobre la fila root
    // de antes, ahora que esa fila ya no existe).
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked())
        m_selected = nullptr; // clic en zona vacía deselecciona
    if (sceneRoot && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = sceneRoot;
        }
        ImGui::EndDragDropTarget();
    }

    bool canDelete = m_selected && m_selected->parent != nullptr;
    bool canRename = m_selected && m_selected->parent != nullptr;

    if (ImGui::IsWindowFocused() && canDelete && ImGui::IsKeyPressed(ImGuiKey_Delete))
        m_pendingDelete = m_selected;
    if (ImGui::IsWindowFocused() && canRename && ImGui::IsKeyPressed(ImGuiKey_F2))
        beginRename(m_selected);

    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
            sceneRoot->addChild("GameObject");
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(sceneRoot, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(sceneRoot, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(sceneRoot, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(sceneRoot, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename", nullptr, false, canRename))
            beginRename(m_selected);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canDelete))
            m_pendingDelete = m_selected;
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
            if (go == m_selected) selectionInSubtree = true;
        });

        if (m_onDelete)
            m_onDelete(target);
        if (target->parent)
        {
            auto& siblings = target->parent->children;
            siblings.erase(
                std::remove_if(siblings.begin(), siblings.end(),
                    [target](const std::unique_ptr<GameObject>& c) { return c.get() == target; }),
                siblings.end());
        }
        if (selectionInSubtree)
        {
            m_selected = nullptr;
            m_propsCachedFor = nullptr;
            m_colliderCachedFor = nullptr;
        }
    }

    if (m_pendingMoveSource && m_pendingMoveTarget)
    {
        moveGameObject(m_pendingMoveSource, m_pendingMoveTarget);
        m_pendingMoveSource = nullptr;
        m_pendingMoveTarget = nullptr;
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
                m_renameTarget->name = newName;
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

void EditorUI::beginRename(GameObject* node)
{
    if (!node || !node->parent)
        return; // root no se puede renombrar

    m_renameTarget = node;
    std::string current = node->name.empty() ? "GameObject" : node->name;
    std::strncpy(m_renameBuffer, current.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_openRenamePopup = true;
}

void EditorUI::createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh)
{
    if (!parent || !m_renderer || !mesh)
        return;

    GameObject* go = parent->addChild(name);
    go->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
    go->setMesh(std::move(mesh));
}

void EditorUI::loadMeshForSelected(const std::string& path)
{
    if (!m_selected || !m_renderer || m_selected->hasMesh())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }

    try
    {
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
    }
    catch (const std::exception& e)
    {
        m_meshLoadError = e.what();
    }
}

void EditorUI::loadAudioClipForSelected(const std::string& path)
{
    if (!m_selected || !m_audio || m_selected->hasAudioClip())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::set<std::string> kValidExt = {".wav", ".mp3", ".ogg", ".flac"};
    if (!kValidExt.count(ext))
    {
        m_audioLoadError = "Formato no soportado: " + ext;
        return;
    }

    auto clip = m_audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip)
    {
        m_audioLoadError = "No se pudo cargar el audio";
        return;
    }
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
}

void EditorUI::drawSceneNode(GameObject* node)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (node->children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
    if (node == m_selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label = node->name.empty() ? "GameObject" : node->name;
    bool open = ImGui::TreeNodeEx((const void*)node, flags, "%s", label.c_str());
    if (ImGui::IsItemClicked())
        m_selected = node;

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
            node->addChild("GameObject");
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(node, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(node, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(node, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(node, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
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
            drawSceneNode(child.get());
        ImGui::TreePop();
    }
}

float EditorUI::selectionAxisScale(GameObject* node) const
{
    constexpr float kFallback = 50.0f;
    // 2.0 en vez de 1.3: con 1.3 solo sobresalía un poco del mesh y costaba
    // verlo; así el tramo visible fuera del objeto es tan largo como su
    // propio medio-tamaño.
    constexpr float kFactor   = 2.0f;

    if (!node->hasMesh())
        return kFallback;

    const auto& vertices = node->getMesh()->vertices;
    if (vertices.empty())
        return kFallback;

    glm::vec3 bMin = vertices[0].pos;
    glm::vec3 bMax = vertices[0].pos;
    for (const auto& v : vertices)
    {
        bMin = glm::min(bMin, v.pos);
        bMax = glm::max(bMax, v.pos);
    }

    glm::vec3 extent  = bMax - bMin;
    float     maxHalf = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;
    return glm::max(maxHalf, 1.0f) * kFactor;
}

void EditorUI::focusSelected(Camera& camera)
{
    if (!m_selected)
        return;

    constexpr float kFallbackRadius = 50.0f;

    glm::vec3 center = glm::vec3(m_selected->worldTransform[3]);
    float     radius = kFallbackRadius;

    if (m_selected->hasMesh())
    {
        const auto& vertices = m_selected->getMesh()->vertices;
        if (!vertices.empty())
        {
            glm::vec3 bMin = vertices[0].pos;
            glm::vec3 bMax = vertices[0].pos;
            for (const auto& v : vertices)
            {
                bMin = glm::min(bMin, v.pos);
                bMax = glm::max(bMax, v.pos);
            }
            glm::vec3 extent   = bMax - bMin;
            float     maxHalf  = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;

            glm::vec3 worldScale(
                glm::length(glm::vec3(m_selected->worldTransform[0])),
                glm::length(glm::vec3(m_selected->worldTransform[1])),
                glm::length(glm::vec3(m_selected->worldTransform[2])));
            float maxWorldScale = glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z));

            radius = glm::max(maxHalf, 1.0f) * maxWorldScale;
        }
    }

    camera.focusOn(center, radius);
}

void EditorUI::drawSelectionGizmo()
{
    if (!m_selected)
        return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));

    const glm::vec3 kColliderColor(1.0f, 1.0f, 0.0f);
    if (m_selected->hasBoxCollider())
    {
        BoxCollider* bc = m_selected->getBoxCollider().get();
        Gizmos::drawWireBox(m_selected->worldTransform, bc->getCenter(),
                             bc->getHalfExtents(), kColliderColor);
    }
    else if (m_selected->hasSphereCollider())
    {
        SphereCollider* sc = m_selected->getSphereCollider().get();
        Gizmos::drawWireSphere(m_selected->worldTransform, sc->getCenter(),
                                sc->getRadius(), kColliderColor);
    }
    else if (m_selected->hasCapsuleCollider())
    {
        CapsuleCollider* cc = m_selected->getCapsuleCollider().get();
        Gizmos::drawWireCapsule(m_selected->worldTransform, cc->getCenter(),
                                 cc->getRadius(), cc->getHalfHeight(), kColliderColor);
    }
    else if (m_selected->hasPlaneCollider())
    {
        PlaneCollider* pc = m_selected->getPlaneCollider().get();
        Gizmos::drawWirePlane(m_selected->worldTransform, pc->getCenter(), kColliderColor);
    }
}

void EditorUI::drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    ImGui::Begin("Viewport");
    m_viewportHovered = ImGui::IsWindowHovered();
    ImVec2 vpPos  = ImGui::GetCursorScreenPos();
    ImVec2 vpSize = ImGui::GetContentRegionAvail();
    ImGui::Image((ImTextureID)(intptr_t)viewportTexture, vpSize);

    // Axis gizmo estilo Unity/Godot (esquina superior derecha): ejes mundo
    // proyectados por la rotación real de la cámara (parte 3x3 de la view
    // matrix), así que gira con ella. Clicar una bola reorienta la cámara
    // pa mirar a lo largo de ese eje (via m_onAxisSelected).
    const glm::mat3 camRot(cameraView);

    struct Axis { glm::vec3 world; glm::vec3 screenDir; ImU32 color; const char* label; };
    Axis axes[3] = {
        { glm::vec3(1, 0, 0), camRot * glm::vec3(1, 0, 0), IM_COL32(220,  60,  60, 255), "X" },
        { glm::vec3(0, 1, 0), camRot * glm::vec3(0, 1, 0), IM_COL32( 70, 200,  70, 255), "Y" },
        { glm::vec3(0, 0, 1), camRot * glm::vec3(0, 0, 1), IM_COL32( 70, 130, 230, 255), "Z" },
    };

    const float radius = 34.0f;
    const float margin  = 16.0f;
    const float ballRadius = 7.0f;
    ImVec2 center(vpPos.x + vpSize.x - radius - margin, vpPos.y + radius + margin);

    // Pinta primero el eje más lejano de cámara pa que el más cercano quede encima.
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3, [&](int a, int b) { return axes[a].screenDir.z < axes[b].screenDir.z; });

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool clicked = m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i : order)
    {
        const glm::vec3& d = axes[i].screenDir;
        ImVec2 tip(center.x + d.x * radius, center.y - d.y * radius);
        drawList->AddLine(center, tip, axes[i].color, 2.0f);
        drawList->AddCircleFilled(tip, ballRadius, axes[i].color);

        ImVec2 textSize = ImGui::CalcTextSize(axes[i].label);
        drawList->AddText(ImVec2(tip.x - textSize.x * 0.5f, tip.y - textSize.y * 0.5f),
                           IM_COL32(0, 0, 0, 255), axes[i].label);

        if (clicked && m_onAxisSelected)
        {
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy <= ballRadius * ballRadius)
                m_onAxisSelected(axes[i].world);
        }
    }
    drawList->AddCircleFilled(center, 3.0f, IM_COL32(200, 200, 200, 255));

    ImGui::End();
}

void EditorUI::drawProperties()
{
    ImGui::Begin("Properties");
    if (!m_selected)
    {
        m_propsCachedFor = nullptr;
        ImGui::End();
        return;
    }

    // Solo re-sincroniza el cache de edición al cambiar de selección: si se
    // recompusiera desde localTransform en cada frame, un valor intermedio
    // inválido (p.ej. escala 0 mientras se teclea "0.5") se re-descompondría
    // y rompería posición/rotación de forma permanente.
    if (m_propsCachedFor != m_selected)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
        m_propsCachedFor = m_selected;
        m_meshLoadError.clear();
        m_audioLoadError.clear();
    }
    // BoxCollider dinámico (useGravity=true): PhysX mueve worldTransform (y
    // localTransform, ver traverse en el loop principal) cada frame, pero eso
    // nunca toca este cache de edición — sin este refresco, Position/Rotation
    // mostrados quedan congelados en el valor de cuando se seleccionó, aunque
    // el objeto siga cayendo/rotando por física. Solo posición+rotación (la
    // escala es puramente del editor, physx no la conoce); se salta mientras
    // se está arrastrando un slider pa no pelear con el drag del usuario.
    else if (m_selected->hasBoxCollider() && m_selected->getBoxCollider()->isDynamic() && !m_transformDragActive)
    {
        glm::vec3 skew, unusedScale;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->worldTransform, unusedScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
    }

    ImGui::Text("%s", m_selected->name.empty() ? "GameObject" : m_selected->name.c_str());
    ImGui::Separator();

    bool changed = false;
    bool posRotActive = false;

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
    {
        ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();

        ImGui::Text("Rotation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##2", &m_editRotationDeg.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##2", &m_editRotationDeg.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##2", &m_editRotationDeg.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();

        ImGui::Text("Scale   ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##3", &m_editScale.x, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##3", &m_editScale.y, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##3", &m_editScale.z, 0.005f, 0.001f, +FLT_MAX, "% .3f");

        ImGui::TreePop();
    }

    m_transformDragActive = posRotActive;

    if (changed)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
        glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
        m_selected->localTransform = t * r * s;

        if (m_selected->hasAnyCollider())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            // teleport() (no syncTransform): funciona tanto si el actor es
            // dinámico (isDynamic()==true) como si es kinematic
            // (isDynamic()==false) — syncTransform usa setKinematicTarget,
            // que solo es válido en modo kinematic. hasAnyCollider() cubre
            // los 4 tipos porque son mutuamente excluyentes.
            if (m_selected->hasBoxCollider())
                m_selected->getBoxCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasSphereCollider())
                m_selected->getSphereCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasCapsuleCollider())
                m_selected->getCapsuleCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasPlaneCollider())
                m_selected->getPlaneCollider()->teleport(m_selected->worldTransform);
        }
    }

    drawBoxColliderSection();
    drawSphereColliderSection();
    drawCapsuleColliderSection();
    drawPlaneColliderSection();
    drawMeshSection();
    drawAudioClipSection();
    drawAddComponentButton();

    ImGui::End();
}

void EditorUI::drawBoxColliderSection()
{
    if (!m_selected->hasBoxCollider())
    {
        m_colliderCachedFor = nullptr;
        return;
    }

    BoxCollider* bc = m_selected->getBoxCollider().get();

    if (m_colliderCachedFor != bc)
    {
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
        m_editUseGravity     = bc->getUseGravity();
        m_colliderCachedFor  = bc;
    }
    else if (bc->isDynamic() && !m_colliderDragActive)
    {
        // Solo Center/Size se refrescan (son estables bajo simulación); el
        // toggle de gravedad lo controla el usuario y no cambia solo.
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setUseGravity(m_editUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
    }
}

void EditorUI::drawSphereColliderSection()
{
    if (!m_selected->hasSphereCollider())
    {
        m_sphereColliderCachedFor = nullptr;
        return;
    }

    SphereCollider* sc = m_selected->getSphereCollider().get();

    if (m_sphereColliderCachedFor != sc)
    {
        m_editSphereCenter        = sc->getCenter();
        m_editSphereRadius        = sc->getRadius();
        m_editSphereUseGravity    = sc->getUseGravity();
        m_sphereColliderCachedFor = sc;
    }
    else if (sc->isDynamic() && !m_sphereColliderDragActive)
    {
        m_editSphereCenter = sc->getCenter();
        m_editSphereRadius = sc->getRadius();
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editSphereUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setUseGravity(m_editSphereUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
    }
}

void EditorUI::drawCapsuleColliderSection()
{
    if (!m_selected->hasCapsuleCollider())
    {
        m_capsuleColliderCachedFor = nullptr;
        return;
    }

    CapsuleCollider* cc = m_selected->getCapsuleCollider().get();

    if (m_capsuleColliderCachedFor != cc)
    {
        m_editCapsuleCenter        = cc->getCenter();
        m_editCapsuleRadius        = cc->getRadius();
        m_editCapsuleHeight        = cc->getHalfHeight() * 2.0f;
        m_editCapsuleUseGravity    = cc->getUseGravity();
        m_capsuleColliderCachedFor = cc;
    }
    else if (cc->isDynamic() && !m_capsuleColliderDragActive)
    {
        m_editCapsuleCenter = cc->getCenter();
        m_editCapsuleRadius = cc->getRadius();
        m_editCapsuleHeight = cc->getHalfHeight() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editCapsuleUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setUseGravity(m_editCapsuleUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
    }
}

void EditorUI::drawPlaneColliderSection()
{
    if (!m_selected->hasPlaneCollider())
    {
        m_planeColliderCachedFor = nullptr;
        return;
    }

    PlaneCollider* pc = m_selected->getPlaneCollider().get();

    if (m_planeColliderCachedFor != pc)
    {
        m_editPlaneCenter        = pc->getCenter();
        m_planeColliderCachedFor = pc;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);

    if (removeClicked)
    {
        m_selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
    }
}

void EditorUI::drawMeshSection()
{
    // Oculto por defecto: solo se dibuja si ya tiene mesh, o si se pulsó
    // "Add > Mesh" para este GameObject concreto (m_meshAddRequestedFor).
    if (!m_selected->hasMesh() && m_meshAddRequestedFor != m_selected)
        return;

    ImGui::Separator();

    if (m_selected->hasMesh())
    {
        bool sectionOpen = ImGui::TreeNodeEx("Mesh", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            ImGui::Text("%s", m_selected->getMesh()->name.c_str());
            ImGui::TreePop();
        }

        if (removeClicked && m_renderer)
        {
            m_renderer->removeMeshComponent(m_selected);
            // Vuelve a ocultar la sección tras quitar el mesh — hay que
            // pulsar "Add > Mesh" de nuevo para reabrirla.
            m_meshAddRequestedFor = nullptr;
        }

        return;
    }

    ImGui::Text("Mesh");
    if (ImGui::Button("Browse..."))
    {
        m_meshDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path = "assets";
        // Key sin prefijo "##": Display() construye el nombre interno de la
        // ventana como título+"##"+key; con key="##AddMeshDlg" el resultado
        // llevaba 4 almohadillas seguidas ("Choose FBX####AddMeshDlg"), y
        // ImGui trata "###" como separador especial de ID (todo lo posterior
        // determina el ID, ignorando el resto) — se calculaba distinto en
        // window->ID que en el ID guardado en settings al persistir el
        // layout, y el mismatch disparaba
        // "Assertion failed: settings->ID == window->ID" al redimensionar
        // (momento en que se fuerza el guardado). El ejemplo oficial de IGFD
        // usa keys planas (sin "##"), como aquí.
        m_meshFileDialog->OpenDialog("AddMeshDlg", "Choose FBX", ".fbx", cfg);
    }

    ImGui::BeginChild("##MeshDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop .fbx here");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadMeshForSelected(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_meshLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_meshLoadError.c_str());
}

void EditorUI::drawMeshDialog()
{
    // Se ejecuta cada frame independientemente de m_selected/hasMesh(): si no
    // se drena aquí, cambiar de selección (o deseleccionar) mientras el
    // diálogo está abierto deja m_meshDlgOpen atascado en true para siempre.
    // m_meshFileDialog es una instancia propia (no el singleton Instance()
    // de Content Browser), así que redimensionar este popup no toca el
    // estado interno de ContentDlg ni viceversa.
    if (m_meshDlgOpen && m_meshFileDialog->Display("AddMeshDlg"))
    {
        if (m_meshFileDialog->IsOk())
            loadMeshForSelected(m_meshFileDialog->GetFilePathName());
        m_meshFileDialog->Close();
        m_meshDlgOpen = false;
    }
}

void EditorUI::drawAudioClipSection()
{
    // Oculto por defecto: solo se dibuja si ya tiene AudioClip, o si se
    // pulsó "Add > Audio Clip" para este GameObject concreto
    // (m_audioClipAddRequestedFor).
    if (!m_selected->hasAudioClip() && m_audioClipAddRequestedFor != m_selected)
        return;

    ImGui::Separator();

    if (m_selected->hasAudioClip())
    {
        auto& clip = m_selected->getAudioClip();
        bool sectionOpen = ImGui::TreeNodeEx("Audio Clip", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            std::string fname = std::filesystem::path(clip->getPath()).filename().string();
            ImGui::Text("%s", fname.c_str());

            ImGui::BeginDisabled(m_audio == nullptr);
            if (ImGui::Button("Play"))
            {
                glm::vec3 worldPos(m_selected->worldTransform[3]);
                clip->play(worldPos);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                clip->stop();
            ImGui::EndDisabled();

            bool loop = clip->getLoop();
            if (ImGui::Checkbox("Loop", &loop))
                clip->setLoop(loop);

            bool is3D = clip->getIs3D();
            if (ImGui::Checkbox("Is 3D?", &is3D))
                clip->setIs3D(is3D);

            ImGui::TreePop();
        }

        if (removeClicked)
        {
            m_selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
        }

        return;
    }

    ImGui::Text("Audio Clip");
    ImGui::BeginDisabled(m_audio == nullptr);
    if (ImGui::Button("Browse..."))
    {
        m_audioDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path = "assets";
        // Key plana sin "##" (mismo motivo documentado en drawMeshSection
        // para AddMeshDlg: con prefijo "##" el título concatenado generaba
        // 4 almohadillas seguidas y rompía el ID persistido de ImGui).
        m_audioFileDialog->OpenDialog("AddAudioDlg", "Choose Audio", ".wav,.mp3,.ogg,.flac", cfg);
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadAudioClipForSelected(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_audioLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_audioLoadError.c_str());
}

void EditorUI::drawAudioClipDialog()
{
    // Se ejecuta cada frame independientemente de m_selected/hasAudioClip():
    // si no se drena aquí, cambiar de selección mientras el diálogo está
    // abierto deja m_audioDlgOpen atascado en true (mismo motivo que
    // drawMeshDialog).
    if (m_audioDlgOpen && m_audioFileDialog->Display("AddAudioDlg"))
    {
        if (m_audioFileDialog->IsOk())
            loadAudioClipForSelected(m_audioFileDialog->GetFilePathName());
        m_audioFileDialog->Close();
        m_audioDlgOpen = false;
    }
}

void EditorUI::drawAddComponentButton()
{
    ImGui::Separator();
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        bool alreadyHasAny = m_selected->hasAnyCollider();
        ImGui::BeginDisabled(alreadyHasAny);

        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_sphereColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_capsuleColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
                glm::vec3(0.0f), m_selected->worldTransform));
            m_planeColliderCachedFor = nullptr;
        }

        ImGui::EndDisabled();

        bool alreadyHasMesh = m_selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        bool alreadyHasAudio = m_selected->hasAudioClip();
        ImGui::BeginDisabled(alreadyHasAudio);
        if (ImGui::Selectable("Audio Clip") && !alreadyHasAudio)
            m_audioClipAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }
}

void EditorUI::drawContentBrowser()
{
    ImGui::Begin("Content Browser");
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    // Left: ImGuiFileDialog embedded
    ImGui::BeginChild("##FileDlgPane", ImVec2(leftWidth, totalHeight), false);
    {
        if (!m_dlgOpen) {
            IGFD::FileDialogConfig cfg;
            cfg.path  = "assets";
            cfg.flags = ImGuiFileDialogFlags_NoDialog |
                        ImGuiFileDialogFlags_DontShowHiddenFiles;
            IGFD::FileDialog::Instance()->OpenDialog(
                "##ContentDlg", "Files", nullptr, cfg);
            m_dlgOpen = true;
        }
        ImVec2 dlgSize = ImGui::GetContentRegionAvail();
        if (IGFD::FileDialog::Instance()->Display(
                "##ContentDlg", ImGuiWindowFlags_None, dlgSize, dlgSize))
        {
            IGFD::FileDialog::Instance()->Close();
            m_dlgOpen = false;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        std::string browsedDir = IGFD::FileDialog::Instance()->GetCurrentPath();
        if (browsedDir.empty()) browsedDir = "assets";
        if (browsedDir != m_currentDir) {
            m_currentDir = browsedDir;
            m_scanned = false;
        }

        if (!m_scanned) {
            m_assets.clear();
            if (std::filesystem::exists(m_currentDir))
                for (auto& e : std::filesystem::directory_iterator(m_currentDir))
                    if (e.is_regular_file())
                        m_assets.push_back(e.path());
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));
        ImGui::Columns(cols, "##AssetGrid", false);

        static const std::set<std::string> kDraggableExt = {".fbx", ".wav", ".mp3", ".ogg", ".flac"};

        for (auto& path : m_assets) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac") {
                btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG";
            } else if (ext == ".spv") {
                btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV";
            } else {
                btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "...";
            }

            ImGui::PushID(path.string().c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);

            if (kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace DonTopo
