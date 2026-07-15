#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Editor/EditorContext.h"
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
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <type_traits>
#include <variant>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace {

// 2 decimales — suficiente para leer el valor de un vistazo en el Log sin
// líneas kilométricas; el panel Properties ya muestra 3 decimales para
// edición fina, el Log es solo un resumen legible.
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

// "attackRange" -> "Attack Range" (labels de props de scripts)
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

} // namespace

namespace DonTopo {

PropertiesPanel::PropertiesPanel()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
{
}

PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::invalidateCaches()
{
    m_propsCachedFor = nullptr;
    m_colliderCachedFor = nullptr;
}

void PropertiesPanel::loadMeshForSelected(EditorContext& ctx, const std::string& path)
{
    if (!ctx.selected || !ctx.renderer || ctx.selected->hasMesh())
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
        ctx.selected->staticRenderIndex = ctx.renderer->addStaticMesh(*mesh);
        ctx.selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
        ctx.pushLog("Componente Mesh añadido a '" + ctx.selected->name + "'");
    }
    catch (const std::exception& e)
    {
        m_meshLoadError = e.what();
    }
}

void PropertiesPanel::loadAudioClipForSelected(EditorContext& ctx, const std::string& path)
{
    if (!ctx.selected || !ctx.audio || ctx.selected->hasAudioClip())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::set<std::string> kValidExt = {".wav", ".mp3", ".ogg", ".flac"};
    if (!kValidExt.count(ext))
    {
        m_audioLoadError = "Formato no soportado: " + ext;
        return;
    }

    auto clip = ctx.audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip)
    {
        m_audioLoadError = "No se pudo cargar el audio";
        return;
    }
    ctx.selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
    ctx.pushLog("Componente Audio Clip añadido a '" + ctx.selected->name + "'");
}

void PropertiesPanel::draw(EditorContext& ctx)
{
    if (m_open)
    {
        ImGui::Begin("Properties", &m_open);
        if (!ctx.selected)
        {
            m_propsCachedFor = nullptr;
        }
        else
        {
            // Solo re-sincroniza el cache de edición al cambiar de selección: si se
            // recompusiera desde localTransform en cada frame, un valor intermedio
            // inválido (p.ej. escala 0 mientras se teclea "0.5") se re-descompondría
            // y rompería posición/rotación de forma permanente.
            if (m_propsCachedFor != ctx.selected)
            {
                glm::vec3 skew;
                glm::vec4 perspective;
                glm::quat orientation;
                glm::decompose(ctx.selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
                m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
                m_propsCachedFor = ctx.selected;
                m_meshLoadError.clear();
                m_audioLoadError.clear();
            }
            // Cuerpo simulado (Rigidbody no-kinematic): PhysX mueve worldTransform (y
            // localTransform, ver traverse en el loop principal) cada frame, pero eso
            // nunca toca este cache de edición — sin este refresco, Position/Rotation
            // mostrados quedan congelados en el valor de cuando se seleccionó, aunque
            // el objeto siga cayendo/rotando por física. Solo posición+rotación (la
            // escala es puramente del editor, physx no la conoce); se salta mientras
            // se está arrastrando un slider pa no pelear con el drag del usuario.
            else if (ctx.selected->hasAnyCollider() && ctx.selected->hasRigidbody()
                     && !ctx.selected->getRigidbody()->getIsKinematic() && !m_transformDragActive)
            {
                glm::vec3 skew, unusedScale;
                glm::vec4 perspective;
                glm::quat orientation;
                glm::decompose(ctx.selected->worldTransform, unusedScale, orientation, m_editPosition, skew, perspective);
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
                ctx.pushLog("Position de '" + ctx.selected->name + "' cambiado a " + formatVec3(m_editPosition));
            if (rotCommitted)
                ctx.pushLog("Rotation de '" + ctx.selected->name + "' cambiado a " + formatVec3(m_editRotationDeg));
            if (scaleCommitted)
                ctx.pushLog("Scale de '" + ctx.selected->name + "' cambiado a " + formatVec3(m_editScale));

            if (changed)
            {
                glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
                glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
                glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
                ctx.selected->localTransform = t * r * s;

                if (ctx.selected->hasAnyCollider())
                {
                    ctx.selected->updateWorldTransforms(ctx.selected->parent ? ctx.selected->parent->worldTransform
                                                                           : glm::mat4(1.0f));
                    // teleport() (no syncTransform): setGlobalPose sirve para
                    // cualquier tipo de actor (static, kinematic o dinámico),
                    // mientras syncTransform usa setKinematicTarget, sólo válido
                    // en kinematic. anyCollider() da el único collider (los 4
                    // tipos son mutuamente excluyentes).
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
                    "Transform de '" + ctx.selected->name + "'", before, after,
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
            drawMeshSection(ctx);
            drawAudioClipSection(ctx);
            drawScriptsSection(ctx);
            drawAddComponentButton(ctx);
        }

        ImGui::End();
    }

    drawMeshDialog(ctx);
    drawAudioClipDialog(ctx);
}

void PropertiesPanel::drawBoxColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasBoxCollider())
    {
        m_colliderCachedFor = nullptr;
        return;
    }

    BoxCollider* bc = ctx.selected->getBoxCollider().get();

    if (m_colliderCachedFor != bc)
    {
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
        m_editIsTrigger      = bc->isTrigger();
        m_colliderCachedFor  = bc;
    }
    else if (ctx.selected->hasRigidbody() && !ctx.selected->getRigidbody()->getIsKinematic() && !m_colliderDragActive)
    {
        // Cuerpo simulado: Center/Size se refrescan (estables bajo simulación).
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

        bool oldTrigger = m_editIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getBoxCollider(), m_editIsTrigger);
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Box Collider) " + (m_editIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                BoxColliderState before{ m_editColliderCenter, m_editColliderSize, oldTrigger };
                BoxColliderState after{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger };
                ctx.undo->push(std::make_unique<PropertyCommand<BoxColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Box Collider)", before, after, applyBoxState));
            }
        }

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (activated)
        m_boxColliderBeforeEdit = BoxColliderState{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger };

    if (centerCommitted)
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderCenter));
    if (sizeCommitted)
        ctx.pushLog("Size de '" + ctx.selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderSize));

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
    }

    if ((centerCommitted || sizeCommitted) && ctx.scene)
    {
        BoxColliderState before = m_boxColliderBeforeEdit;
        BoxColliderState after{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger };
        ctx.undo->push(std::make_unique<PropertyCommand<BoxColliderState>>(
            "Box Collider de '" + ctx.selected->name + "'", before, after, applyBoxState));
    }

    if (removeClicked)
    {
        ctx.selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
        ctx.pushLog("Componente Box Collider quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawSphereColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasSphereCollider())
    {
        m_sphereColliderCachedFor = nullptr;
        return;
    }

    SphereCollider* sc = ctx.selected->getSphereCollider().get();

    if (m_sphereColliderCachedFor != sc)
    {
        m_editSphereCenter        = sc->getCenter();
        m_editSphereRadius        = sc->getRadius();
        m_editSphereIsTrigger     = sc->isTrigger();
        m_sphereColliderCachedFor = sc;
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

        bool oldTrigger = m_editSphereIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editSphereIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getSphereCollider(), m_editSphereIsTrigger);
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Sphere Collider) " + (m_editSphereIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                SphereColliderState before{ m_editSphereCenter, m_editSphereRadius, oldTrigger };
                SphereColliderState after{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger };
                ctx.undo->push(std::make_unique<PropertyCommand<SphereColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Sphere Collider)", before, after, applySphereState));
            }
        }

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (activated)
        m_sphereColliderBeforeEdit = SphereColliderState{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger };

    if (centerCommitted)
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Sphere Collider) cambiado a " + formatVec3(m_editSphereCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius de '" + ctx.selected->name + "' (Sphere Collider) cambiado a " + formatFloat(m_editSphereRadius));

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
    }

    if ((centerCommitted || radiusCommitted) && ctx.scene)
    {
        SphereColliderState before = m_sphereColliderBeforeEdit;
        SphereColliderState after{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger };
        ctx.undo->push(std::make_unique<PropertyCommand<SphereColliderState>>(
            "Sphere Collider de '" + ctx.selected->name + "'", before, after, applySphereState));
    }

    if (removeClicked)
    {
        ctx.selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
        ctx.pushLog("Componente Sphere Collider quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawCapsuleColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasCapsuleCollider())
    {
        m_capsuleColliderCachedFor = nullptr;
        return;
    }

    CapsuleCollider* cc = ctx.selected->getCapsuleCollider().get();

    if (m_capsuleColliderCachedFor != cc)
    {
        m_editCapsuleCenter        = cc->getCenter();
        m_editCapsuleRadius        = cc->getRadius();
        m_editCapsuleHeight        = cc->getHalfHeight() * 2.0f;
        m_editCapsuleIsTrigger     = cc->isTrigger();
        m_capsuleColliderCachedFor = cc;
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

        bool oldTrigger = m_editCapsuleIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editCapsuleIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getCapsuleCollider(), m_editCapsuleIsTrigger);
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Capsule Collider) " + (m_editCapsuleIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                CapsuleColliderState before{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, oldTrigger };
                CapsuleColliderState after{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger };
                ctx.undo->push(std::make_unique<PropertyCommand<CapsuleColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Capsule Collider)", before, after, applyCapsuleState));
            }
        }

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (activated)
        m_capsuleColliderBeforeEdit = CapsuleColliderState{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger };

    if (centerCommitted)
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatVec3(m_editCapsuleCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleRadius));
    if (heightCommitted)
        ctx.pushLog("Height de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleHeight));

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
    }

    if ((centerCommitted || radiusCommitted || heightCommitted) && ctx.scene)
    {
        CapsuleColliderState before = m_capsuleColliderBeforeEdit;
        CapsuleColliderState after{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger };
        ctx.undo->push(std::make_unique<PropertyCommand<CapsuleColliderState>>(
            "Capsule Collider de '" + ctx.selected->name + "'", before, after, applyCapsuleState));
    }

    if (removeClicked)
    {
        ctx.selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
        ctx.pushLog("Componente Capsule Collider quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawPlaneColliderSection(EditorContext& ctx)
{
    if (!ctx.selected->hasPlaneCollider())
    {
        m_planeColliderCachedFor = nullptr;
        return;
    }

    PlaneCollider* pc = ctx.selected->getPlaneCollider().get();

    if (m_planeColliderCachedFor != pc)
    {
        m_editPlaneCenter        = pc->getCenter();
        m_editPlaneIsTrigger     = pc->isTrigger();
        m_planeColliderCachedFor = pc;
    }

    Scene* scene = ctx.scene;
    uint64_t id = ctx.selected->id;
    PhysicsManager* physics = ctx.physics;
    auto applyPlaneState = [scene, id, physics](const PlaneColliderState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasPlaneCollider()) return;
        go->getPlaneCollider()->setCenter(s.center);
        if (physics) physics->setTrigger(go->getPlaneCollider(), s.isTrigger);
    };

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool activated = false;
    bool centerCommitted = false;

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

        bool oldTrigger = m_editPlaneIsTrigger;
        if (ImGui::Checkbox("Is Trigger", &m_editPlaneIsTrigger))
        {
            if (ctx.physics)
                ctx.physics->setTrigger(ctx.selected->getPlaneCollider(), m_editPlaneIsTrigger);
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Plane Collider) " + (m_editPlaneIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                PlaneColliderState before{ m_editPlaneCenter, oldTrigger };
                PlaneColliderState after{ m_editPlaneCenter, m_editPlaneIsTrigger };
                ctx.undo->push(std::make_unique<PropertyCommand<PlaneColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Plane Collider)", before, after, applyPlaneState));
            }
        }

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (activated)
        m_planeColliderBeforeEdit = PlaneColliderState{ m_editPlaneCenter, m_editPlaneIsTrigger };

    if (centerCommitted)
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Plane Collider) cambiado a " + formatVec3(m_editPlaneCenter));

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);

    if (centerCommitted && ctx.scene)
    {
        PlaneColliderState before = m_planeColliderBeforeEdit;
        PlaneColliderState after{ m_editPlaneCenter, m_editPlaneIsTrigger };
        ctx.undo->push(std::make_unique<PropertyCommand<PlaneColliderState>>(
            "Plane Collider de '" + ctx.selected->name + "'", before, after, applyPlaneState));
    }

    if (removeClicked)
    {
        ctx.selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
        ctx.pushLog("Componente Plane Collider quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawRigidbodySection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasRigidbody()) { m_rigidbodyCachedFor = nullptr; return; }
    Rigidbody* rb = ctx.selected->getRigidbody().get();
    if (m_rigidbodyCachedFor != rb)
    {
        m_editRbMass        = rb->getMass();
        m_editRbUseGravity  = rb->getUseGravity();
        m_editRbKinematic   = rb->getIsKinematic();
        m_editRbDrag        = rb->getDrag();
        m_editRbAngularDrag = rb->getAngularDrag();
        m_editRbConstraints = rb->getConstraints();
        m_rigidbodyCachedFor = rb;
    }

    ImGui::Separator();
    if (!ImGui::TreeNodeEx("Rigidbody", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*   scene = ctx.scene;
    uint64_t id    = ctx.selected->id;

    // Aplica un RigidbodyState al GameObject resuelto por id (sobrevive a
    // undo-de-delete). Mismo patrón que applyBoxState.
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
    };
    auto currentState = [&]() {
        return RigidbodyState{ m_editRbMass, m_editRbUseGravity, m_editRbKinematic,
                               m_editRbDrag, m_editRbAngularDrag, m_editRbConstraints };
    };

    // --- Drag floats: snapshot al activar, comando al soltar ---
    bool floatChanged = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Mass", &m_editRbMass, 0.1f, 0.0001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated() && !m_rigidbodyDragActive)
    {
        m_rigidbodyDragActive = true;
        m_rigidbodyBeforeEdit = RigidbodyState{ rb->getMass(), rb->getUseGravity(), rb->getIsKinematic(),
                                                rb->getDrag(), rb->getAngularDrag(), rb->getConstraints() };
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Drag", &m_editRbDrag, 0.01f, 0.0f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated() && !m_rigidbodyDragActive)
    {
        m_rigidbodyDragActive = true;
        m_rigidbodyBeforeEdit = RigidbodyState{ rb->getMass(), rb->getUseGravity(), rb->getIsKinematic(),
                                                rb->getDrag(), rb->getAngularDrag(), rb->getConstraints() };
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Angular Drag", &m_editRbAngularDrag, 0.01f, 0.0f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated() && !m_rigidbodyDragActive)
    {
        m_rigidbodyDragActive = true;
        m_rigidbodyBeforeEdit = RigidbodyState{ rb->getMass(), rb->getUseGravity(), rb->getIsKinematic(),
                                                rb->getDrag(), rb->getAngularDrag(), rb->getConstraints() };
    }
    if (floatChanged) { rb->setMass(m_editRbMass); rb->setDrag(m_editRbDrag); rb->setAngularDrag(m_editRbAngularDrag); }
    if (m_rigidbodyDragActive && ImGui::IsItemDeactivatedAfterEdit())
    {
        m_rigidbodyDragActive = false;
        if (ctx.scene)
            ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                "Rigidbody de '" + ctx.selected->name + "'", m_rigidbodyBeforeEdit, currentState(), applyRbState));
    }

    // --- Checkboxes: comando inmediato con before/after ---
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Use Gravity", &m_editRbUseGravity))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("Use Gravity de '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbUseGravity ? "activado" : "desactivado"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "Use Gravity de '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
    }
    {
        RigidbodyState before = currentState();
        if (ImGui::Checkbox("Is Kinematic", &m_editRbKinematic))
        {
            applyRbState(currentState());
            ctx.pushLog(std::string("Is Kinematic de '") + ctx.selected->name +
                     "' (Rigidbody) " + (m_editRbKinematic ? "activado" : "desactivado"));
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<RigidbodyState>>(
                    "Is Kinematic de '" + ctx.selected->name + "' (Rigidbody)", before, currentState(), applyRbState));
        }
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
                "Constraints de '" + ctx.selected->name + "' (Rigidbody)", cbBefore, currentState(), applyRbState));
    }

    if (ImGui::Button("Remove Rigidbody"))
    {
        if (auto col = ctx.selected->anyCollider(); col && ctx.physics)
            ctx.physics->detachRigidbody(col);
        ctx.selected->setRigidbody(nullptr);
        m_rigidbodyCachedFor = nullptr;
        ctx.pushLog("Componente Rigidbody quitado de '" + ctx.selected->name + "'");
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawMeshSection(EditorContext& ctx)
{
    // Oculto por defecto: solo se dibuja si ya tiene mesh, o si se pulsó
    // "Add > Mesh" para este GameObject concreto (m_meshAddRequestedFor).
    if (!ctx.selected->hasMesh() && m_meshAddRequestedFor != ctx.selected)
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
            ImGui::TreePop();
        }

        if (removeClicked && ctx.renderer)
        {
            ctx.renderer->removeMeshComponent(ctx.selected);
            // Vuelve a ocultar la sección tras quitar el mesh — hay que
            // pulsar "Add > Mesh" de nuevo para reabrirla.
            m_meshAddRequestedFor = nullptr;
            ctx.pushLog("Componente Mesh quitado de '" + ctx.selected->name + "'");
        }

        return;
    }

    ImGui::Text("Mesh");
    if (ImGui::Button("Browse..."))
    {
        m_meshDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
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
            loadMeshForSelected(ctx, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_meshLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_meshLoadError.c_str());
}

void PropertiesPanel::drawMeshDialog(EditorContext& ctx)
{
    // Se ejecuta cada frame independientemente de ctx.selected/hasMesh(): si no
    // se drena aquí, cambiar de selección (o deseleccionar) mientras el
    // diálogo está abierto deja m_meshDlgOpen atascado en true para siempre.
    // m_meshFileDialog es una instancia propia (no el singleton Instance()
    // de Content Browser), así que redimensionar este popup no toca el
    // estado interno de ContentDlg ni viceversa.
    if (m_meshDlgOpen && m_meshFileDialog->Display("AddMeshDlg"))
    {
        if (m_meshFileDialog->IsOk())
            loadMeshForSelected(ctx, m_meshFileDialog->GetFilePathName());
        m_meshFileDialog->Close();
        m_meshDlgOpen = false;
    }
}

void PropertiesPanel::drawAudioClipSection(EditorContext& ctx)
{
    // Oculto por defecto: solo se dibuja si ya tiene AudioClip, o si se
    // pulsó "Add > Audio Clip" para este GameObject concreto
    // (m_audioClipAddRequestedFor).
    if (!ctx.selected->hasAudioClip() && m_audioClipAddRequestedFor != ctx.selected)
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

            bool loop = clip->getLoop();
            if (ImGui::Checkbox("Loop", &loop))
                clip->setLoop(loop);

            bool is3D = clip->getIs3D();
            if (ImGui::Checkbox("Is 3D?", &is3D))
                clip->setIs3D(is3D);

            bool playOnAwake = clip->getPlayOnAwake();
            if (ImGui::Checkbox("Play On Awake", &playOnAwake))
                clip->setPlayOnAwake(playOnAwake);

            ImGui::TreePop();
        }

        if (removeClicked)
        {
            ctx.selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
            ctx.pushLog("Componente Audio Clip quitado de '" + ctx.selected->name + "'");
        }

        return;
    }

    ImGui::Text("Audio Clip");
    ImGui::BeginDisabled(ctx.audio == nullptr);
    if (ImGui::Button("Browse..."))
    {
        m_audioDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
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
            loadAudioClipForSelected(ctx, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_audioLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_audioLoadError.c_str());
}

void PropertiesPanel::drawAudioClipDialog(EditorContext& ctx)
{
    // Se ejecuta cada frame independientemente de ctx.selected/hasAudioClip():
    // si no se drena aquí, cambiar de selección mientras el diálogo está
    // abierto deja m_audioDlgOpen atascado en true (mismo motivo que
    // drawMeshDialog).
    if (m_audioDlgOpen && m_audioFileDialog->Display("AddAudioDlg"))
    {
        if (m_audioFileDialog->IsOk())
            loadAudioClipForSelected(ctx, m_audioFileDialog->GetFilePathName());
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

        // TreeNodeEx (label estrecho) y no CollapsingHeader (frame de ancho
        // completo): el header solaparía el botón "x" y se comería su click.
        // Mismo patrón que las secciones de collider.
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
                        "Error de compilación:\n%s", err->c_str());
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Script no encontrado: %s.lua", comp->scriptName.c_str());
                // Overrides intactos (spec: no se pierden datos)
            }
            else
            {
                const ScriptClass& cls = ctx.scriptManager->getRegistry().at(comp->scriptName);
                const bool live = ctx.isPlaying && comp->instance.valid();

                for (const ScriptProp& prop : cls.props)
                {
                    // Valor mostrado: instancia viva > override > default
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
                        strncpy_s(buf, s.c_str(), sizeof(buf) - 1);
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
                                "' cambiado en '" + ctx.selected->name + "'");
                    }
                }

                if (ImGui::Button("Reset"))
                {
                    comp->overrides.clear();
                    if (live)
                    {
                        // Reaplica defaults del .lua a la instancia viva
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
                    ctx.pushLog("Script '" + comp->scriptName + "' reseteado a defaults en '" +
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
        ctx.pushLog("Componente Script '" + name + "' quitado de '" + ctx.selected->name + "'");
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
            // Owner opaco = GameObject, para que TriggerEvent.other lo resuelva.
            ctx.selected->getBoxCollider()->setOwner(ctx.selected);
            m_colliderCachedFor = nullptr;
            ctx.pushLog("Componente Box Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setSphereCollider(ctx.physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getSphereCollider()->setOwner(ctx.selected);
            m_sphereColliderCachedFor = nullptr;
            ctx.pushLog("Componente Sphere Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setCapsuleCollider(ctx.physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getCapsuleCollider()->setOwner(ctx.selected);
            m_capsuleColliderCachedFor = nullptr;
            ctx.pushLog("Componente Capsule Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setPlaneCollider(ctx.physics->createPlaneColliderComponent(
                glm::vec3(0.0f), ctx.selected->worldTransform));
            ctx.selected->getPlaneCollider()->setOwner(ctx.selected);
            m_planeColliderCachedFor = nullptr;
            ctx.pushLog("Componente Plane Collider añadido a '" + ctx.selected->name + "'");
        }

        ImGui::EndDisabled();

        // Rigidbody: necesita un collider que aporte la forma; oculto si ya
        // tiene uno o si no hay collider al que engancharlo.
        if (!ctx.selected->hasRigidbody() && ctx.selected->hasAnyCollider())
        {
            if (ImGui::Selectable("Rigidbody") && ctx.physics)
            {
                auto rb = std::make_shared<Rigidbody>();
                ctx.selected->setRigidbody(rb);
                if (auto col = ctx.selected->anyCollider())
                    ctx.physics->attachRigidbody(col, rb);
                m_rigidbodyCachedFor = nullptr;
                ctx.pushLog("Componente Rigidbody añadido a '" + ctx.selected->name + "'");
            }
        }

        bool alreadyHasMesh = ctx.selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = ctx.selected;
        ImGui::EndDisabled();

        bool alreadyHasAudio = ctx.selected->hasAudioClip();
        ImGui::BeginDisabled(alreadyHasAudio);
        if (ImGui::Selectable("Audio Clip") && !alreadyHasAudio)
            m_audioClipAddRequestedFor = ctx.selected;
        ImGui::EndDisabled();

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
                        // En Play el lifecycle instancia y dispara Awake/Start
                        // en el siguiente update (started == false).
                        ctx.pushLog("Componente Script '" + name + "' añadido a '" + ctx.selected->name + "'");
                    }
                }
                if (!ctx.scriptManager->getRegistry().empty())
                    ImGui::Separator();
                if (ImGui::MenuItem("Nuevo Script..."))
                {
                    m_newScriptTarget = ctx.selected;
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
        ImGui::OpenPopup("Nuevo Script");
        m_openNewScriptPopup = false;
    }

    if (!ImGui::BeginPopupModal("Nuevo Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Nombre del script (sin .lua):");
    ImGui::InputText("##NewScriptName", m_newScriptNameBuffer, sizeof(m_newScriptNameBuffer));
    if (!m_newScriptError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_newScriptError.c_str());

    if (ImGui::Button("Crear"))
    {
        const std::string name = m_newScriptNameBuffer;

        // Identificador Lua válido: letra o '_' + alfanuméricos/'_' — el
        // nombre del archivo es también el de la tabla global de la clase.
        bool validName = !name.empty() &&
            (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
        for (size_t i = 1; validName && i < name.size(); ++i)
            validName = std::isalnum(static_cast<unsigned char>(name[i])) || name[i] == '_';

        const std::filesystem::path path = ctx.scriptManager->scriptsDirPath() / (name + ".lua");

        if (!validName)
            m_newScriptError = "Nombre inválido: letra o '_' inicial, luego alfanuméricos o '_'";
        else if (ctx.scriptManager->hasClass(name) || std::filesystem::exists(path))
            m_newScriptError = "Ya existe un script con ese nombre";
        else
        {
            std::ofstream file(path);
            if (!file)
                m_newScriptError = "No se pudo crear el archivo en " + path.string();
            else
            {
                file << name << " = {\n"
                     << "    -- Propiedades serializables (aparecen en el editor)\n"
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

                    // El GameObject pudo borrarse mientras el popup estaba
                    // abierto — comprobar que sigue vivo antes de añadir.
                    bool targetAlive = false;
                    if (ctx.scene && m_newScriptTarget)
                        ctx.scene->traverse([&](GameObject* go) {
                            if (go == m_newScriptTarget) targetAlive = true;
                        });
                    if (targetAlive)
                    {
                        m_newScriptTarget->addScript(
                            std::make_unique<ScriptComponent>(name, m_newScriptTarget));
                        ctx.pushLog("Script '" + name + "' creado y añadido a '" +
                                m_newScriptTarget->name + "'");
                    }
                    else
                        ctx.pushLog("Script '" + name + "' creado (el GameObject ya no existe)");
                    ImGui::CloseCurrentPopup();
                }
                else
                    m_newScriptError = "El script no compiló (ver Log)";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace DonTopo
