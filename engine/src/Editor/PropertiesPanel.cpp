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
#include "DonTopo/Renderer/SkinnedMesh.h"
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

// Compara floats con tolerancia — evita empujar un comando de Undo cuando el
// drag termina en el mismo valor con el que empezó (ruido de redondeo).
bool nearlyEqualF(float a, float b) { return std::fabs(a - b) < 0.0001f; }

// Aviso bajo el checkbox "Is Trigger" cuando el GameObject no tiene Rigidbody.
//
// PhysX no genera pares entre dos actores estáticos —no pueden moverse el uno
// respecto al otro, así que ni siquiera llama al filter shader—, y un collider
// sin Rigidbody es PxRigidStatic. O sea: un trigger sin Rigidbody NO detecta
// objetos que tampoco lo tengan. Es la misma regla que Unity, pero aquí no
// había nada que la dijera: se marcaba el checkbox, no pasaba nada, y no
// quedaba ni una pista de por qué. Ver los tests de triggers en
// physics_tests.cpp, que fijan las tres combinaciones.
void drawTriggerRigidbodyHint(const DonTopo::GameObject* go, bool isTrigger)
{
    if (!isTrigger || !go || go->hasRigidbody()) return;
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "Sin Rigidbody: solo detecta objetos que sí lo tengan");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PhysX no reporta solapes entre dos objetos estáticos.\n"
                          "Añade un Rigidbody a este objeto o al que deba entrar.");
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
    // Un Undo/Redo de cámara muta el componente EN SITIO (el puntero no
    // cambia), así que sin esto m_cameraCachedFor seguiría apuntando al mismo
    // CameraComponent y drawCameraSection nunca refrescaría m_editCam* tras el
    // undo: el panel se quedaría mostrando el valor deshecho, y el próximo
    // drag de OTRO campo reaplicaría ese valor stale, resucitando el cambio
    // que el usuario acababa de deshacer.
    m_cameraCachedFor = nullptr;
}

void PropertiesPanel::loadMeshForSelected(EditorContext& ctx, const std::string& path)
{
    // El guard de hasMesh() ya no basta: mientras la carga está en vuelo
    // hasMesh() es falso, así que un segundo drop encolaría una carga duplicada
    // y el segundo resultado pisaría al primero. pendingMeshJob != 0 lo corta.
    if (!ctx.selected || !ctx.assetLoader
        || ctx.selected->hasMesh() || ctx.selected->pendingMeshJob != 0)
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }

    // No carga: encola. El registro en el Renderer (addSkinnedMesh/addStaticMesh)
    // y el setMesh los hace EditorUI::onAssetsLoaded (vía applyLoadedMesh) cuando
    // el worker termine y el pump por frame lo recoja.
    ctx.selected->pendingMeshJob = ctx.assetLoader->requestMesh(path, ctx.selected->id);
    m_meshLoadError.clear();
    ctx.pushLog("Cargando '" + path + "'...");
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
            drawCameraSection(ctx);
            drawAnimatorSection(ctx);
            drawMeshSection(ctx);
            drawSsrSection(ctx);
            drawReflectionProbeSection(ctx);
            drawLightSection(ctx);
            drawAudioClipSection(ctx);
            drawAudioListenerSection(ctx);
            drawCanvasSection(ctx);
            drawScriptsSection(ctx);
            drawAddComponentButton(ctx);
        }

        ImGui::End();
    }

    drawMeshDialog(ctx);
    drawAudioClipDialog(ctx);
}

void PropertiesPanel::drawSsrSection(EditorContext& ctx)
{
    // Sin malla no hay superficie que refleje.
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
        ctx.pushLog("SSR de '" + ctx.selected->name + "' " +
                    (enabled ? "activado" : "desactivado"));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                "SSR de '" + ctx.selected->name + "'", before, enabled,
                [scene, id](const bool& v) {
                    if (GameObject* go = scene->findById(id)) go->ssrEnabled = v;
                }));
        }
    }

    ImGui::BeginDisabled(!ctx.selected->ssrEnabled);
    // El "before" se lee ANTES de dibujar el slider: SliderFloat salta al valor
    // bajo el cursor en el mismo frame del click, así que releerlo después daría
    // ya el nuevo y el undo devolvería el valor del click, no el original.
    const float beforeIntensity = ctx.selected->ssrIntensity;
    float       intensity       = ctx.selected->ssrIntensity;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    // Reflectividad a incidencia normal: 1 = espejo desde cualquier ángulo,
    // valores bajos reflejan sobre todo de canto (suelo pulido, agua).
    if (ImGui::SliderFloat("Reflectivity", &intensity, 0.0f, 1.0f, "%.2f"))
        ctx.selected->ssrIntensity = intensity;

    if (ImGui::IsItemActivated())
    {
        m_ssrDragActive          = true;
        m_ssrDragBeforeIntensity = beforeIntensity;
        m_ssrDragOwnerId         = id;
    }
    // El id del dueño evita aplicar un "before" ajeno si el drag se interrumpió
    // sin commit (p.ej. un Ctrl+Z a mitad de arrastre reconstruye el GameObject).
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ssrDragActive && m_ssrDragOwnerId == id)
    {
        m_ssrDragActive = false;
        ctx.pushLog("Reflectivity de '" + ctx.selected->name + "' cambiado a " +
                    std::to_string(ctx.selected->ssrIntensity));
        if (scene && ctx.undo)
        {
            const float after = ctx.selected->ssrIntensity;
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Reflectivity de '" + ctx.selected->name + "'", m_ssrDragBeforeIntensity, after,
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
    // Add-gate: sin el componente no hay sección, igual que los colliders.
    if (!ctx.selected->hasReflectionProbe()) return;

    if (!ImGui::TreeNodeEx("Reflection Probe", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*                    scene = ctx.scene;
    const uint64_t            id    = ctx.selected->id;
    ReflectionProbeComponent* probe = ctx.selected->getReflectionProbe().get();

    ImGui::TextWrapped("La sonda captura el entorno desde la posición de este objeto "
                       "y sustituye al IBL global en lo que caiga dentro del radio.");

    // Los "before" se leen ANTES de dibujar los sliders: SliderFloat salta al
    // valor bajo el cursor en el mismo frame del click, así que releerlos
    // después daría ya el nuevo y el undo devolvería el valor del click.
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
        ctx.pushLog("Radius de la sonda de '" + ctx.selected->name + "' cambiado a " +
                    std::to_string(after));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Radius de la sonda de '" + ctx.selected->name + "'", m_probeDragBefore, after,
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
        ctx.pushLog("Intensity de la sonda de '" + ctx.selected->name + "' cambiada a " +
                    std::to_string(after));
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                "Intensity de la sonda de '" + ctx.selected->name + "'", m_probeDragBefore, after,
                [scene, id](const float& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasReflectionProbe()) go->getReflectionProbe()->setIntensity(v);
                }));
        }
    }

    // El bake es un evento: el botón sólo ENCOLA. El Renderer lo ejecuta al
    // principio del frame siguiente, que es donde puede esperar a la GPU.
    if (ctx.renderer)
    {
        if (ImGui::Button("Bake"))
        {
            ctx.renderer->requestProbeBake(id);
            ctx.pushLog("Bake de la sonda de '" + ctx.selected->name + "' encolado");
        }
        ImGui::SameLine();
        const float ms = ctx.renderer->probeBakeMs(id);
        if (ms < 0.0f) ImGui::TextUnformatted("sin bakear");
        else           ImGui::Text("%.2f ms de GPU", ms);
        ImGui::Text("Memoria: %.2f MB", (double)Renderer::probeMemoryBytes() / (1024.0 * 1024.0));
    }

    if (ImGui::Button("Remove Reflection Probe"))
    {
        ctx.selected->setReflectionProbe(nullptr);
        m_probeDragActive = false;
        ctx.pushLog("Componente Reflection Probe quitado de '" + ctx.selected->name + "'");
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawAudioListenerSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
    if (!ctx.selected->hasAudioListener()) return;

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Audio Listener",
                                         ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    const bool removeClicked = ImGui::SmallButton("x##audioListener");

    if (sectionOpen)
    {
        ImGui::TextWrapped("Desde aquí se oye el audio 3D de la escena. La posición y la "
                           "orientación salen del Transform de este objeto (mira hacia su "
                           "-Z local), no de campos propios. Como mucho uno por escena.");

        bool enabled = ctx.selected->getAudioListener()->getEnabled();
        if (ImGui::Checkbox("Enabled", &enabled))
            ctx.selected->getAudioListener()->setEnabled(enabled);

        ImGui::TreePop();
    }

    if (removeClicked)
    {
        ctx.selected->setAudioListener(nullptr);
        ctx.pushLog("Componente Audio Listener quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawCanvasSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Raíz de la UI 2D. El área útil sale del render, menos los insets "
                           "del safe area y recortada al aspect ratio; de ahí sale una escala "
                           "única y uniforme para todo el árbol.");

        // Los campos se alcanzan por un accessor sin captura (function pointer)
        // y no por puntero a miembro: así los cuatro insets del safe area, que
        // viven un nivel más adentro, usan el MISMO helper que el resto.
        using FloatRef = float& (*)(CanvasComponent&);
        using Vec2Ref  = glm::vec2& (*)(CanvasComponent&);
        using EnumSet  = void (*)(CanvasComponent&, int);

        // Los combos se commitean en el acto (un click = un cambio), igual que
        // el Type de la luz.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*c, idx);
                const std::string lbl = std::string(label) + " del canvas de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                        lbl, before, idx,
                        [scene, id, apply](const int& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCanvas()) apply(*go->getCanvas(), v);
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: el "before" se lee ANTES
        // de dibujar, la sesión se abre en IsItemActivated y se commitea en
        // IsItemDeactivatedAfterEdit, así un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " del canvas de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del canvas de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiada");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = desconocido: se usa el Fallback DPI");
        dragFloat("Fallback DPI",
                  +[](CanvasComponent& cc) -> float& { return cc.fallbackDpi; },
                  1.0f, 1.0f, 2000.0f, "%.1f");
        dragFloat("Reference DPI",
                  +[](CanvasComponent& cc) -> float& { return cc.referenceDpi; },
                  1.0f, 1.0f, 2000.0f, "%.1f");

        ImGui::Text("Safe Area (px reales)");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = apagado. 16/9 = 1.7778");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<CanvasComponentCommand>(
            *ctx.scene, "Quitar Canvas de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getCanvas());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Canvas quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawLightSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
    if (!ctx.selected->hasLight()) return;

    if (!ImGui::TreeNodeEx("Light", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*            scene = ctx.scene;
    const uint64_t    id    = ctx.selected->id;
    LightComponent*   light = ctx.selected->getLight().get();
    const std::string owner = ctx.selected->name;

    ImGui::TextWrapped("La posición y la dirección salen del Transform de este objeto "
                       "(la luz apunta hacia su -Z local), no de campos propios.");

    const char* kTypes[] = { "Point", "Spot", "Directional", "Area" };
    int typeIdx = (int)light->getType();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::Combo("Type", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes)))
    {
        const int before = (int)light->getType();
        light->setType((LightType)typeIdx);
        ctx.pushLog("Tipo de la luz de '" + owner + "' cambiado a " + kTypes[typeIdx]);
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<int>>(
                "Tipo de la luz de '" + owner + "'", before, typeIdx,
                [scene, id](const int& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasLight()) go->getLight()->setType((LightType)v);
                }));
        }
    }

    // El "before" se lee ANTES de dibujar: el picker cambia el valor en el mismo
    // frame del click y releerlo después daría ya el nuevo.
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
        ctx.pushLog("Color de la luz de '" + owner + "' cambiado");
        if (scene && ctx.undo)
        {
            ctx.undo->push(std::make_unique<PropertyCommand<glm::vec3>>(
                "Color de la luz de '" + owner + "'", m_lightColorBefore, after,
                [scene, id](const glm::vec3& v) {
                    if (GameObject* go = scene->findById(id))
                        if (go->hasLight()) go->getLight()->setColor(v);
                }));
        }
    }

    // Los seis escalares comparten el mismo baile de undo (leer el "before"
    // antes de dibujar, abrir sesión en IsItemActivated, commitear en
    // IsItemDeactivatedAfterEdit), así que va una vez aquí y no seis veces.
    // El campo en drag se identifica por su etiqueta: un bool no llega pa seis.
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
        // El id del dueño y la etiqueta evitan aplicar un "before" ajeno si el
        // drag se interrumpió sin commit (p.ej. un Ctrl+Z a mitad de arrastre).
        if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragActive &&
            m_lightDragOwnerId == id && m_lightDragField &&
            std::strcmp(m_lightDragField, label) == 0)
        {
            m_lightDragActive = false;
            const float after = (light->*getter)();
            ctx.pushLog(std::string(label) + " de la luz de '" + owner + "' cambiado a " +
                        std::to_string(after));
            if (scene && ctx.undo)
            {
                ctx.undo->push(std::make_unique<PropertyCommand<float>>(
                    std::string(label) + " de la luz de '" + owner + "'", m_lightDragBefore, after,
                    [scene, id, setter](const float& x) {
                        if (GameObject* go = scene->findById(id))
                            if (go->hasLight()) ((*go->getLight()).*setter)(x);
                    }));
            }
        }
    };

    floatSlider("Intensity", 0.0f, 20.0f, "%.2f",
                &LightComponent::getIntensity, &LightComponent::setIntensity);

    // Cada tipo enseña SOLO lo que usa: un cono no tiene tamaño de área y una
    // directional no tiene alcance. Los campos que no salen siguen guardados en
    // el componente (y en el .scene), así que cambiar de tipo y volver no
    // pierde nada — se ocultan, no se resetean.
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
            ImGui::TextDisabled("El interior nunca pasa del exterior: se arrastran entre ellos.");
            break;

        case LightType::Directional:
            ImGui::TextDisabled("Sin alcance: ilumina toda la escena en la dirección del -Z local.");
            break;

        case LightType::Area:
            floatSlider("Area Width", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getAreaWidth, &LightComponent::setAreaWidth);
            floatSlider("Area Height", 1.0f, 5000.0f, "%.0f",
                        &LightComponent::getAreaHeight, &LightComponent::setAreaHeight);
            ImGui::TextDisabled("El alcance sale del ancho (Width/2), no del Range.");
            break;
    }

    if (ImGui::Button("Remove Light"))
    {
        ctx.selected->setLight(nullptr);
        m_lightDragActive = false;
        ctx.pushLog("Componente Light quitado de '" + owner + "'");
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
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
        drawTriggerRigidbodyHint(ctx.selected, m_editIsTrigger);

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
        drawTriggerRigidbodyHint(ctx.selected, m_editSphereIsTrigger);

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
        drawTriggerRigidbodyHint(ctx.selected, m_editCapsuleIsTrigger);

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
        drawTriggerRigidbodyHint(ctx.selected, m_editPlaneIsTrigger);

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

    // --- Drag floats: snapshot al activar CUALQUIERA, comando al soltar
    // CUALQUIERA. IsItemActivated/IsItemDeactivatedAfterEdit se consultan por
    // widget y se acumulan (no una sola query final: esa sólo reflejaría el
    // último DragFloat y dejaría Mass/Drag sin undo).
    //
    // Sin gate por m_rigidbodyDragActive: sólo un widget de ImGui puede tener
    // ActiveId a la vez, así que el gate no evitaba ningún re-snapshot real y a
    // cambio dejaba el flag pegado cuando un click no llegaba a editar
    // (IsItemDeactivatedAfterEdit exige edición previa, y DragFloat no edita si
    // el ratón no se mueve). Con el flag pegado, la siguiente edición —incluso
    // en OTRO GameObject— reutilizaba el snapshot viejo, y el Ctrl+Z escribía
    // en el objeto nuevo la masa, gravedad, kinematic, drag, angular drag y
    // constraints del anterior.
    auto snapshotBefore = [&]() {
        m_rigidbodyDragActive  = true;
        m_rigidbodyDragOwnerId = id;
        m_rigidbodyBeforeEdit  = RigidbodyState{ rb->getMass(), rb->getUseGravity(), rb->getIsKinematic(),
                                                 rb->getDrag(), rb->getAngularDrag(), rb->getConstraints() };
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
    // La guarda de propietario cubre el drag que empieza en un GameObject y
    // acaba commiteando mientras el panel ya dibuja otro: sin ella se aplicaría
    // el "before" del primero al segundo.
    if (m_rigidbodyDragActive && floatCommitted && m_rigidbodyDragOwnerId == id)
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

void PropertiesPanel::drawCameraSection(EditorContext& ctx)
{
    // Oculta hasta que se pulse Add: la sección solo existe si el componente
    // existe, y el componente solo existe tras Add (mismo early-return que
    // drawRigidbodySection).
    if (!ctx.selected || !ctx.selected->hasCameraComponent()) { m_cameraCachedFor = nullptr; return; }
    CameraComponent* cam = ctx.selected->getCameraComponent().get();
    if (m_cameraCachedFor != cam)
    {
        m_editCamMode      = cam->getMode();
        m_editCamFov       = cam->getFov();
        m_editCamOrthoSize = cam->getOrthographicSize();
        m_editCamNear      = cam->getNear();
        m_editCamFar       = cam->getFar();
        m_cameraCachedFor  = cam;
    }

    ImGui::Separator();
    if (!ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*   scene = ctx.scene;
    uint64_t id    = ctx.selected->id;

    // Aplica un CameraState al GameObject resuelto por id (sobrevive a
    // undo-de-delete). Mismo patrón que applyRbState.
    auto applyCamState = [scene, id](const CameraState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasCameraComponent()) return;
        auto c = go->getCameraComponent();
        c->setMode(s.mode);
        // far antes que near: setNear clampa contra el far actual.
        c->setFar(s.farPlane);
        c->setNear(s.nearPlane);
        c->setFov(s.fov);
        c->setOrthographicSize(s.orthographicSize);
    };
    auto currentState = [&]() {
        return CameraState{ m_editCamMode, m_editCamFov, m_editCamOrthoSize, m_editCamNear, m_editCamFar };
    };

    // --- Combo de modo: comando inmediato con before/after ---
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
            ctx.pushLog(std::string("Projection de '") + ctx.selected->name + "' (Camera): " + modes[modeIdx]);
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<CameraState>>(
                    "Projection de '" + ctx.selected->name + "' (Camera)", before, currentState(), applyCamState));
        }
    }

    // --- Drag floats: snapshot al activar CUALQUIERA, comando al soltar
    // CUALQUIERA (mismo patrón acumulativo que Rigidbody).
    //
    // Sin gate por m_cameraDragActive, y con propietario: mismo motivo que en
    // drawRigidbodySection — el gate dejaba el flag pegado cuando un click no
    // llegaba a editar, y el siguiente drag en otro GameObject commiteaba el
    // snapshot ajeno.
    auto snapshotBefore = [&]() {
        m_cameraDragActive  = true;
        m_cameraDragOwnerId = id;
        m_cameraBeforeEdit  = CameraState{ cam->getMode(), cam->getFov(), cam->getOrthographicSize(),
                                           cam->getNear(), cam->getFar() };
    };
    bool floatChanged = false;
    bool floatCommitted = false;

    // Solo se muestra el campo del modo activo: enseñar el otro sugeriría que
    // hace algo, y no hace nada.
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
        // Los clamps del componente pueden haber corregido el valor (p.ej. near
        // por encima de far): se re-sincroniza el cache pa que el widget enseñe
        // lo que de verdad quedó guardado, no lo que se arrastró.
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
                "Camera de '" + ctx.selected->name + "'", m_cameraBeforeEdit, currentState(), applyCamState));
    }

    if (ImGui::Button("Remove Camera"))
    {
        // Pasa por el stack igual que el Add (ver CameraComponentCommand): si el
        // Remove no fuera deshacible, quitar la cámara sería una pérdida
        // irreversible.
        CameraState st = currentState();
        m_cameraCachedFor = nullptr;
        ctx.pushLog("Componente Camera quitado de '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Quitar Camera de '" + ctx.selected->name + "'", id, /*add=*/false, st);
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

// El grafo NO se edita aquí: eso es del panel Animator (el canvas necesita su
// propio zoom/pan). Esta sección solo resume y da la puerta de entrada.
void PropertiesPanel::drawAnimatorSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasAnimator()) return;
    auto anim = ctx.selected->getAnimator();

    if (!ImGui::TreeNodeEx("Animator", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("Estados: %d", (int)anim->states().size());
    ImGui::Text("Transiciones: %d", (int)anim->transitions().size());
    const int entry = anim->entryState();
    if (entry >= 0 && entry < (int)anim->states().size())
        ImGui::Text("Entrada: %s", anim->states()[entry].name.c_str());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Sin estado de entrada");

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
        // Pasa por el stack igual que el Add (ver AnimatorComponentCommand): el
        // grafo se conserva en el comando pa que el Undo lo devuelva entero.
        const uint64_t id = ctx.selected->id;
        AnimatorComponent st = *anim;
        ctx.pushLog("Componente Animator quitado de '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<AnimatorComponentCommand>(
                *ctx.scene, "Quitar Animator de '" + ctx.selected->name + "'", id, /*add=*/false, st);
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

            // Solo el dibujado: oculto no llega a la GPU (ni escena, ni sombras,
            // ni AO), pero física, colisiones y selección en el viewport siguen
            // igual. Vale para estático y skinned, que comparten esta sección.
            Scene*         meshScene = ctx.scene;
            const uint64_t meshId    = ctx.selected->id;
            bool           visible   = ctx.selected->meshVisible;
            if (ImGui::Checkbox("Visible", &visible))
            {
                const bool before = ctx.selected->meshVisible;
                ctx.selected->meshVisible = visible;
                ctx.pushLog("Mesh de '" + ctx.selected->name + "' " +
                            (visible ? "visible" : "oculto"));
                if (meshScene && ctx.undo)
                {
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        "Visible de '" + ctx.selected->name + "'", before, visible,
                        [meshScene, meshId](const bool& v) {
                            if (GameObject* go = meshScene->findById(meshId)) go->meshVisible = v;
                        }));
                }
            }
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
    // Veto de edición mientras el modal de carga está activo: no se aceptan
    // drops nuevos hasta que Load Scene termine (o se cancele).
    if (!ctx.editingLocked && ImGui::BeginDragDropTarget())
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
    // m_meshFileDialog es una instancia propia (no compartida con
    // m_audioFileDialog), así que redimensionar este popup no toca el
    // estado interno del diálogo de Audio ni viceversa.
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

            // --- Volume / Pitch: snapshot al activar cualquiera de los dos,
            // un solo comando al soltar. Los valores se escriben en vivo
            // mientras se arrastra (así se oye el cambio), y el comando sólo
            // sirve para que Ctrl+Z devuelva el drag entero de una vez.
            //
            // SliderFloat (a diferencia de DragFloat) salta al valor bajo el
            // cursor en el MISMO frame en que IsItemActivated() se vuelve
            // true, así que el "before" no puede releerse del componente
            // después de dibujar el widget: para entonces ya vale el valor
            // nuevo. Por eso se hoistean las lecturas aquí, antes de los
            // sliders, y el snapshot usa estas variables en vez de releer
            // clip->getVolume()/getPitch().
            const float volumeBefore = clip->getVolume();
            const float pitchBefore  = clip->getPitch();
            const float minDistBefore = clip->getMinDistance();
            const float maxDistBefore = clip->getMaxDistance();
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

            // Distancias de atenuación: solo tienen sentido en 3D (en 2D FMOD
            // no atenúa por distancia), así que ni se dibujan con is3D
            // desmarcado. El valor sigue guardado en el componente: al volver a
            // marcar is3D reaparece lo que se hubiera editado.
            if (is3D)
            {
                if (ImGui::SliderFloat("Min distance", &minDist, 0.1f, 50.0f, "%.2f"))
                    clip->setMinDistance(minDist);
                activated |= ImGui::IsItemActivated();
                committed |= ImGui::IsItemDeactivatedAfterEdit();

                if (ImGui::SliderFloat("Max distance", &maxDist, 1.0f, 1000.0f, "%.1f"))
                    clip->setMaxDistance(maxDist);
                activated |= ImGui::IsItemActivated();
                // El clamp de max >= min lo hace el propio setter del
                // componente (no la UI), así que al soltar ya está aplicado.
                committed |= ImGui::IsItemDeactivatedAfterEdit();
            }

            // Sin gate por m_audioDragActive: solo un widget de ImGui puede
            // tener ActiveId a la vez, así que el gate no aporta nada salvo
            // un bug: IsItemDeactivatedAfterEdit() exige edición real, y un
            // click que activa el slider sin moverlo nunca llega a
            // "committed", dejando el flag pegado con un "before" rancio que
            // la siguiente edición —incluso en otro GameObject— reutilizaría.
            if (activated)
            {
                m_audioDragActive            = true;
                m_audioDragBeforeVolume      = volumeBefore;
                m_audioDragBeforePitch       = pitchBefore;
                m_audioDragBeforeMinDistance = minDistBefore;
                m_audioDragBeforeMaxDistance = maxDistBefore;
                m_audioDragOwnerId           = clipOwnerId;
            }

            // Guarda de propietario: el ActiveId de un slider de ImGui se
            // conserva mientras el ratón sigue pulsado, aunque la selección
            // cambie a mitad de arrastre (Hierarchy, atajo de teclado o un
            // script) y el panel pase a dibujar el AudioClip de OTRO
            // GameObject. Como el id del widget ("Volume"/"Pitch") es el
            // mismo en ambos, ImGui seguiría considerándolo el mismo drag y
            // el commit final llegaría para ese otro objeto; este id evita
            // aplicarle un "before" que pertenece al GameObject original.
            if (committed && m_audioDragActive && m_audioDragOwnerId == clipOwnerId)
            {
                m_audioDragActive = false;
                const AudioClipState before{ m_audioDragBeforeVolume, m_audioDragBeforePitch,
                                             m_audioDragBeforeMinDistance, m_audioDragBeforeMaxDistance };
                const AudioClipState after { clip->getVolume(), clip->getPitch(),
                                             clip->getMinDistance(), clip->getMaxDistance() };

                if (!nearlyEqualF(before.volume, after.volume) ||
                    !nearlyEqualF(before.pitch,  after.pitch)  ||
                    !nearlyEqualF(before.minDistance, after.minDistance) ||
                    !nearlyEqualF(before.maxDistance, after.maxDistance))
                {
                    // Resuelve el GameObject por id en cada aplicación, nunca
                    // captura el puntero: sobrevive a un undo de Delete que
                    // haya reconstruido el objeto entretanto.
                    auto apply = [scene, clipOwnerId](const AudioClipState& s) {
                        GameObject* go = scene->findById(clipOwnerId);
                        if (!go || !go->hasAudioClip()) return;
                        go->getAudioClip()->setVolume(s.volume);
                        go->getAudioClip()->setPitch(s.pitch);
                        // Max antes que min: los dos setters mantienen
                        // min <= max entre ellos, y en ese orden el par
                        // restaurado no se pisa a sí mismo.
                        go->getAudioClip()->setMaxDistance(s.maxDistance);
                        go->getAudioClip()->setMinDistance(s.minDistance);
                    };
                    if (ctx.scene)
                        ctx.undo->push(std::make_unique<PropertyCommand<AudioClipState>>(
                            "Audio Clip de '" + ctx.selected->name + "'", before, after, apply));
                }
            }

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
    // Mismo veto que el drop de mesh: sin drops nuevos mientras carga la escena.
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

        // Audio Listener: como mucho uno por escena, mismo criterio que la
        // cámara — el gate pregunta a Scene::findAudioListener, no a un flag
        // propio, y el existente no se toca (ni se borra ni se roba).
        GameObject* existingListener = ctx.scene ? ctx.scene->findAudioListener() : nullptr;
        ImGui::BeginDisabled(existingListener != nullptr);
        if (ImGui::Selectable("Audio Listener") && !existingListener)
        {
            ctx.selected->setAudioListener(std::make_shared<AudioListenerComponent>());
            ctx.pushLog("Componente Audio Listener añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (existingListener && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Ya hay un Audio Listener en la escena ('%s'): quítalo de ahí "
                              "antes de poner otro", existingListener->name.c_str());

        // Canvas: raíz de la UI 2D. Sin invariante de escena (caben varios),
        // así que el único gate es no añadir dos al mismo objeto. Pasa por el
        // stack de undo como la cámara: el estado de los 10 campos se conserva
        // en un Add-undo-redo.
        const bool alreadyHasCanvas = ctx.selected->hasCanvas();
        ImGui::BeginDisabled(alreadyHasCanvas);
        if (ImGui::Selectable("Canvas") && !alreadyHasCanvas && ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<CanvasComponentCommand>(
                *ctx.scene, "Añadir Canvas a '" + ctx.selected->name + "'", ctx.selected->id,
                /*add=*/true, CanvasComponent{});
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            ctx.pushLog("Componente Canvas añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Componentes de UI: solo existen colgando de un Canvas, así que un
        // GameObject sin Canvas ni los ve. La lista está vacía hasta que se
        // implementen los widgets; el gate ya es el definitivo.
        if (uiComponentsAvailable(ctx.selected))
        {
            ImGui::Separator();
            ImGui::TextDisabled("UI");
            // (aquí van los widgets de UI, uno por Selectable)
        }

        // Cámara: como mucho una por escena, y el gate pregunta a la única
        // fuente de verdad (Scene::findCamera), no a un flag propio. Deshabilitado
        // y no oculto porque es lo que hacen los demás items de este popup — y el
        // tooltip dice QUIÉN la tiene ya, que si no un item gris sin explicación
        // es un callejón sin salida.
        GameObject* existingCamera = ctx.scene ? ctx.scene->findCamera() : nullptr;
        ImGui::BeginDisabled(existingCamera != nullptr);
        if (ImGui::Selectable("Camera") && !existingCamera && ctx.scene && ctx.undo)
        {
            CameraComponent defaults;
            CameraState st{ defaults.getMode(), defaults.getFov(), defaults.getOrthographicSize(),
                            defaults.getNear(), defaults.getFar() };
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Añadir Camera a '" + ctx.selected->name + "'", ctx.selected->id, /*add=*/true, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            m_cameraCachedFor = nullptr;
            ctx.pushLog("Componente Camera añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (existingCamera && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Ya hay una cámara en la escena ('%s')", existingCamera->name.c_str());

        // Reflection Probe: sin invariante de unicidad (caben las que quepan en
        // memoria), así que el único gate es no añadir dos a un mismo objeto.
        // Al añadirla, el Renderer la detecta en el frame siguiente, le crea sus
        // cubemaps y la bakea una vez: no hace falta pulsar Bake para verla.
        const bool alreadyHasProbe = ctx.selected->hasReflectionProbe();
        ImGui::BeginDisabled(alreadyHasProbe);
        if (ImGui::Selectable("Reflection Probe") && !alreadyHasProbe)
        {
            ctx.selected->setReflectionProbe(std::make_shared<ReflectionProbeComponent>());
            ctx.pushLog("Componente Reflection Probe añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Light: sin invariante de unicidad por escena (caben varias del mismo
        // tipo), así que el único gate es no añadir dos al mismo objeto. Las
        // primeras MAX_LIGHTS en orden de escena son las que llegan al shader.
        const bool alreadyHasLight = ctx.selected->hasLight();
        ImGui::BeginDisabled(alreadyHasLight);
        if (ImGui::Selectable("Light") && !alreadyHasLight)
        {
            ctx.selected->setLight(std::make_shared<LightComponent>());
            ctx.pushLog("Componente Light añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();

        // Animator: solo tiene sentido sobre un mesh skinned (es quien trae los
        // clips). El gate pregunta al GameObject, no a un flag propio.
        const bool canAnimate     = ctx.selected->isSkinned();
        const bool alreadyHasAnim = ctx.selected->hasAnimator();
        ImGui::BeginDisabled(!canAnimate || alreadyHasAnim);
        if (ImGui::Selectable("Animator") && canAnimate && !alreadyHasAnim)
        {
            // Fuera del stack de undo, igual que Script: el usuario construye el
            // grafo por mutación directa (sin comandos), y un Ctrl+Z reflejo tras
            // Add popparía el AnimatorComponentCommand y vaciaría el grafo entero
            // vía setAnimator(nullptr). Remove sí pasa por el stack (ver más abajo
            // en drawAnimatorSection) porque ahí no hay ese riesgo.
            ctx.selected->setAnimator(std::make_shared<AnimatorComponent>());
            ctx.pushLog("Componente Animator añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (!canAnimate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("El Animator necesita un mesh skinned (los clips vienen del FBX)");

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
