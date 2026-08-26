#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Editor/EditorContext.h"
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

// Guarda de los diálogos de asset de este panel (mesh, audio, fuente, atlas).
// A diferencia del Content Browser —que ya no puede salir de la raíz del
// proyecto— estos diálogos navegan por todo el disco, así que son la única vía
// por la que un asset de OTRO proyecto podía entrar en la escena y acabar en el
// paquete de export.
//
// Lo que se rechaza es exactamente eso: un asset que caiga dentro del workspace
// `projects/` pero fuera del proyecto abierto. Lo de fuera del workspace (los
// assets compartidos del repo: mallas, fuentes, audio) se sigue permitiendo,
// mismo criterio que la carpeta Scripts/ o el skybox del motor. Sin proyecto
// abierto (tests headless) pasa todo, como antes de que el concepto existiera.
bool assetAllowed(const DonTopo::EditorContext& ctx, const std::filesystem::path& path)
{
    if (!ctx.project || !ctx.project->valid()) return true;
    if (ctx.project->contains(path))           return true;

    // El workspace lo crea el selector al arrancar, así que este contains()
    // responde sobre una carpeta que existe; si aun así fallara, contains()
    // devuelve false y el asset se trata como compartido, no como ajeno.
    const DonTopo::ProjectContext workspace(DonTopo::ProjectContext::workspaceDir());
    if (!workspace.contains(path)) return true;

    ctx.logModule("Project", "Asset de otro proyecto, rechazado: " + path.string());
    return false;
}

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

// Resolutores de collider por tipo. Punteros a función (sin capturas) para que
// drawColliderLayerCombo los pueda meter en el lambda del Undo, que nunca debe
// capturar un puntero crudo al collider: un Undo de Delete reconstruye el
// GameObject y el viejo queda colgando.
DonTopo::Collider* resolveBoxCollider(DonTopo::GameObject* go)
{ return go->hasBoxCollider() ? go->getBoxCollider().get() : nullptr; }
DonTopo::Collider* resolveSphereCollider(DonTopo::GameObject* go)
{ return go->hasSphereCollider() ? go->getSphereCollider().get() : nullptr; }
DonTopo::Collider* resolveCapsuleCollider(DonTopo::GameObject* go)
{ return go->hasCapsuleCollider() ? go->getCapsuleCollider().get() : nullptr; }
DonTopo::Collider* resolvePlaneCollider(DonTopo::GameObject* go)
{ return go->hasPlaneCollider() ? go->getPlaneCollider().get() : nullptr; }

// Desplegable de la capa de colisión, común a los 4 colliders. NO cachea el
// valor en un miembro como los DragFloat de al lado: un combo confirma en el
// mismo frame y no hay arrastre que proteger, así que se lee el collider vivo.
// Se ofrecen las 32 capas que soporta el core, tengan nombre o no; los nombres
// se editan en los ajustes del proyecto.
void drawColliderLayerCombo(DonTopo::EditorContext& ctx, const char* label,
                            const char* seccion, DonTopo::Collider* collider,
                            DonTopo::Collider* (*resolve)(DonTopo::GameObject*))
{
    if (!collider || !ctx.selected) return;

    // Solo las capas CREADAS en los ajustes del proyecto, no las 32 del techo.
    // Si el collider quedó en una capa que ya no existe (no debería: removeLayer
    // reasigna), se amplía la lista hasta ella para no mostrar un combo vacío.
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
    const std::string desc = std::string("Layer de '") + ctx.selected->name + "' (" + seccion + ")";
    ctx.pushLog(desc + " cambiado a " + etiquetas[capa]);
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

} // namespace

namespace DonTopo {

PropertiesPanel::PropertiesPanel()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
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
    // TODOS de una sentencia, y por eso están en una struct. Un Undo/Redo muta
    // los componentes EN SITIO (el puntero no cambia), así que un cache que no
    // se resetee deja su sección mostrando el valor deshecho, y el próximo drag
    // de OTRO campo reaplica ese valor stale y resucita el cambio que el
    // usuario acababa de deshacer.
    //
    // Esta función se escribía enumerando miembro a miembro y se quedó corta
    // cuatro veces: el Undo no funcionaba en Sphere, Capsule ni Plane Collider
    // ni en Rigidbody, y solo se arreglaba el que alguien reportaba. Añadir el
    // puntero a EditCaches ya basta.
    m_caches = EditCaches{};
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

void PropertiesPanel::drawAssetDropBox(EditorContext& ctx, const char* idSuffix,
                                       const char* hint,
                                       const std::function<void()>& onBrowse,
                                       const std::function<void(const std::string&)>& onDrop)
{
    if (ImGui::Button((std::string("Browse...##") + idSuffix).c_str()))
        onBrowse();

    // Sin SameLine y con 40 px de alto: es el layout del Mesh, y las cajas de
    // UI venían cada una con el suyo (ruta y botón en la misma línea, hijo de
    // 34 px). Un único sitio del que salen las 16.
    ImGui::BeginChild((std::string("##DropZone") + idSuffix).c_str(), ImVec2(0, 40), true);
    ImGui::TextDisabled("%s", hint);
    // Mismo veto de edición que el Mesh: con el modal de Load Scene activo no
    // se aceptan drops nuevos.
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
            // Solo re-sincroniza el cache de edición al cambiar de selección: si se
            // recompusiera desde localTransform en cada frame, un valor intermedio
            // inválido (p.ej. escala 0 mientras se teclea "0.5") se re-descompondría
            // y rompería posición/rotación de forma permanente.
            if (m_caches.props != ctx.selected)
            {
                glm::vec3 skew;
                glm::vec4 perspective;
                glm::quat orientation;
                glm::decompose(ctx.selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
                m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
                m_caches.props = ctx.selected;
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

        using BoolRef = bool& (*)(CanvasComponent&);
        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*c);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*c) = val;
                const std::string lbl = std::string(label) + " del canvas de '" + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
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

        ImGui::TextDisabled("Modo");
        static const char* kRenderModes[] = { "Screen Space", "World" };
        comboEnum("Render Mode", (int)c->renderMode, kRenderModes, IM_ARRAYSIZE(kRenderModes),
                  +[](CanvasComponent& x, int v) { x.renderMode = (UiCanvasRenderMode)v; });

        if (c->renderMode == UiCanvasRenderMode::World)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "En World se ignoran: Scale Mode, Screen Match, Match,\n"
                               "los tres DPI, Safe Area y Aspect Ratio.");

            dragFloat("World Scale", +[](CanvasComponent& x) -> float& { return x.worldScale; },
                      0.0001f, 0.0f, 10.0f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unidades de mundo por PIXEL de canvas.\n"
                                  "Un canvas de 1920x1080 a 0.001 mide 1.92 x 1.08 unidades.");

            static const char* kBillboards[] = { "None", "Yaw Only", "Full" };
            comboEnum("Billboard", (int)c->billboard, kBillboards, IM_ARRAYSIZE(kBillboards),
                      +[](CanvasComponent& x, int v) { x.billboard = (UiBillboard)v; });

            checkBox("Depth Test", +[](CanvasComponent& x) -> bool& { return x.depthTest; });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A false se dibuja siempre encima, atravesando paredes");
        }

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

const std::vector<std::string>& PropertiesPanel::spriteNamesFor(EditorContext& ctx,
                                                                const std::string& atlasPath)
{
    if (m_spriteNamesValid && m_spriteNamesPath == atlasPath) return m_spriteNames;

    m_spriteNamesPath  = atlasPath;
    m_spriteNamesValid = true;
    m_spriteNames.clear();

    if (atlasPath.empty() || !ctx.renderer) return m_spriteNames;

    // loadUiAtlas cachea por ruta, así que esto NO carga una segunda copia del
    // atlas que ya está dibujándose: devuelve ese mismo. Y si la ruta no vale,
    // el resultado (lista vacía) se queda cacheado hasta que cambie la ruta, en
    // vez de reintentar el fichero en cada frame.
    if (const UiTextureAtlas* atlas = ctx.renderer->loadUiAtlas(atlasPath))
        m_spriteNames = atlas->spriteNames();

    return m_spriteNames;
}

void PropertiesPanel::setButtonAssetPath(EditorContext& ctx, uint64_t ownerId, bool isFont,
                                          const std::string& path)
{
    if (path.empty()) return;

    // El filtro del file dialog ya restringe, pero un drop llega con lo que sea:
    // el veto vive AQUÍ, en el punto por el que pasan todos los orígenes, y no
    // repetido en cada caja.
    if (!(isFont ? isUiFontPath(path) : isUiAtlasPath(path)))
    {
        m_buttonPathError = std::string("No es ") +
                            (isFont ? "una fuente (.ttf .otf .ttc): "
                                    : "una imagen (.png .jpg .jpeg .bmp .tga): ") +
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

    const std::string lbl = std::string(isFont ? "Fuente" : "Atlas") +
                            " del botón de '" + go->name + "'";
    ctx.pushLog(lbl + " cambiada a " + path);
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
    // Sin condicionar a ctx.selected/hasButton(): si no se drenan aquí, cambiar
    // de selección con el diálogo abierto deja el flag atascado en true para
    // siempre (mismo motivo que drawMeshDialog).
    if (m_fontDlgOpen && m_fontFileDialog->Display("ButtonFontDlg"))
    {
        if (m_fontFileDialog->IsOk() &&
            assetAllowed(ctx, m_fontFileDialog->GetFilePathName()))
            setButtonAssetPath(ctx, m_fontDlgOwner, /*isFont=*/true,
                                m_fontFileDialog->GetFilePathName());
        m_fontFileDialog->Close();
        m_fontDlgOpen = false;
    }

    if (m_uiAtlasDlgOpen && m_uiAtlasFileDialog->Display("ButtonAtlasDlg"))
    {
        if (m_uiAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_uiAtlasFileDialog->GetFilePathName()))
            setButtonAssetPath(ctx, m_uiAtlasDlgOwner, /*isFont=*/false,
                                m_uiAtlasFileDialog->GetFilePathName());
        m_uiAtlasFileDialog->Close();
        m_uiAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawButtonSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Widget de la UI 2D. Se dibuja en el árbol del Canvas de la escena, "
                           "y el estado (Normal/Hover/Pressed/Disabled/Selected) lo resuelve el "
                           "propio canvas con el ratón y el foco.");

        // Mismos accessors sin captura (function pointer) que el Canvas: así los
        // campos de dentro de una struct usan el MISMO helper que el resto.
        using FloatRef = float&       (*)(ButtonComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ButtonComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ButtonComponent&);
        using StrRef   = std::string& (*)(ButtonComponent&);
        using BoolRef  = bool&        (*)(ButtonComponent&);
        using EnumSet  = void         (*)(ButtonComponent&, int);

        // Combos y checkbox se commitean en el acto: un click = un cambio.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*b, idx);
                const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasButton()) acc(*go->getButton()) = val;
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: "before" leído ANTES de
        // dibujar, sesión abierta en IsItemActivated y commit en
        // IsItemDeactivatedAfterEdit, así un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Los colores llevan alfa (los cinco estados y el texto lo usan para
        // desvanecer), así que ColorEdit4 y no 3.
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
                    const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*b);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin él se cae al campo de
        // texto de siempre, que sigue valiendo para un atlas troceado a mano y
        // para una escena que ya traía un nombre escrito.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, b->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*b);

            // El vacío es "(imagen entera)": un atlas sin sprite se dibuja
            // completo, que es lo que hace UiTextureAtlas::uvRect sin nombre.
            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no está en el atlas NO se pierde ni se corrige
            // solo: se enseña al final marcado, y el componente sigue diciendo
            // lo que decía hasta que el usuario elija otra cosa.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
                items.push_back(huerfano.c_str());
                current = (int)items.size() - 1;
            }

            int idx = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::Combo(label, &idx, items.data(), (int)items.size()) && idx != current)
            {
                // El huérfano no es un destino: elegirlo deja el valor como está.
                const std::string after = (idx == 0)                    ? std::string()
                                        : (idx <= (int)nombres.size())  ? nombres[(size_t)idx - 1]
                                                                        : before;
                if (after != before)
                {
                    acc(*b) = after;
                    const std::string lbl = std::string(label) + " del botón de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("Color base del quad. Con Transition = Color Tint o Animation lo\n"
                              "sobrescribe el color del estado; solo manda con Sprite Swap.");
        checkBox("Visible", +[](ButtonComponent& c) -> bool& { return c.visible; });

        // Ruta escribible a mano + la caja de asset común (drawAssetDropBox:
        // botón y zona de drop, mismo layout que el Mesh). El veto por
        // extensión no está aquí sino en setButtonAssetPath, que es por donde
        // pasan los dos orígenes.
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
        // Sin atlas no hay nada que trocear, y el botón deshabilitado dice por
        // qué mejor que su ausencia.
        ImGui::BeginDisabled(b->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Editar sprites...")) ctx.openSpriteEditor(b->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && b->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Sprite", +[](ButtonComponent& c) -> std::string& { return c.sprite; });

        ImGui::TextDisabled("Estados");
        checkBox("Interactable", +[](ButtonComponent& c) -> bool& { return c.interactable; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A false se pinta Disabled y no emite Click");
        checkBox("Selected", +[](ButtonComponent& c) -> bool& { return c.selected; });

        static const char* kTransitions[] = { "Color Tint", "Sprite Swap", "Animation" };
        comboEnum("Transition", (int)b->transition, kTransitions, IM_ARRAYSIZE(kTransitions),
                  +[](ButtonComponent& c, int v) { c.transition = (UiButtonTransition)v; });

        ImGui::TextDisabled("Colores por estado (Color Tint / Animation)");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Segundos del fundido de Animation. 0 = salto seco");

        ImGui::TextDisabled("Texto");
        inputText("Text", +[](ButtonComponent& c) -> std::string& { return c.text; });
        assetBox("Font", /*isFont=*/true,
                 +[](ButtonComponent& c) -> std::string& { return c.fontPath; },
                 "ButtonFontDlg", "Choose font", ".ttf,.otf,.ttc",
                 "Drop .ttf/.otf here");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vacía = la fuente por defecto del proyecto");

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
            ImGui::SetTooltip("Dónde cae la etiqueta a lo alto del botón");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ButtonComponentCommand>(
            *ctx.scene, "Quitar Button de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getButton());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Button quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setTextFontPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Mismo veto que el Button y por el mismo sitio: aquí pasan el drop y el
    // file dialog, así que el filtro solo hay que ponerlo una vez.
    if (!isUiFontPath(path))
    {
        m_textPathError = "No es una fuente (.ttf .otf .ttc): " +
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

    const std::string lbl = "Fuente del texto de '" + go->name + "'";
    ctx.pushLog(lbl + " cambiada a " + path);
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
    // Sin condicionar a ctx.selected/hasText(): si no se drena aquí, cambiar de
    // selección con el diálogo abierto deja el flag atascado en true.
    if (m_textFontDlgOpen && m_textFontFileDialog->Display("TextFontDlg"))
    {
        if (m_textFontFileDialog->IsOk() &&
            assetAllowed(ctx, m_textFontFileDialog->GetFilePathName()))
            setTextFontPath(ctx, m_textFontDlgOwner, m_textFontFileDialog->GetFilePathName());
        m_textFontFileDialog->Close();
        m_textFontDlgOpen = false;
    }
}

void PropertiesPanel::drawTextSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Etiqueta de la UI 2D. Se dibuja en el árbol del Canvas de la escena. "
                           "El texto acepta tags de estilo: <color=#RRGGBB>, <size=N>, <b> y <i>.");

        // Los mismos accessors sin captura (function pointer) y el mismo baile
        // de undo que la sección del Button. Los labels llevan "##txt" porque un
        // GameObject puede tener Button y Text a la vez: dos widgets con el
        // MISMO id de ImGui en la misma ventana comparten estado.
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
                const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
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
                    const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del texto de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        ImGui::TextDisabled("Texto");
        inputText("Text##txt", +[](TextComponent& c) -> std::string& { return c.text; });

        // Ruta escribible a mano + la caja de asset común (drawAssetDropBox). El
        // veto por extensión está en setTextFontPath, por donde pasan los dos.
        inputText("Font##txt", +[](TextComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vacía = la fuente por defecto del proyecto");
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
            ImGui::SetTooltip("Color del relleno del glyph");

        static const char* kAligns[] = { "Left", "Center", "Right", "Justify" };
        comboEnum("Align##txt", (int)t->align, kAligns, IM_ARRAYSIZE(kAligns),
                  +[](TextComponent& c, int v) { c.align = (UiTextAlign)v; });

        static const char* kVAligns[] = { "Top", "Middle", "Bottom" };
        comboEnum("V Align##txt", (int)t->vAlign, kVAligns, IM_ARRAYSIZE(kVAligns),
                  +[](TextComponent& c, int v) { c.vAlign = (UiTextVAlign)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Dónde cae el bloque de líneas a lo alto del rect");

        static const char* kOverflows[] = { "Overflow", "Clip", "Ellipsis" };
        comboEnum("Overflow##txt", (int)t->overflow, kOverflows, IM_ARRAYSIZE(kOverflows),
                  +[](TextComponent& c, int v) { c.overflow = (UiTextOverflow)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Qué pasa con lo que no cabe en el rect. Clip parte el lote por scissor");

        checkBox("Word Wrap##txt", +[](TextComponent& c) -> bool& { return c.wordWrap; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Corta por palabras contra el ancho del rect. Los '\\n' cortan siempre");

        ImGui::TextDisabled("Contorno");
        dragFloat("Outline Width##txt", +[](TextComponent& c) -> float& { return c.outlineWidth; },
                  0.05f, 0.0f, 32.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Píxeles de pantalla. 0 = sin contorno");
        colorEdit("Outline Color##txt",
                  +[](TextComponent& c) -> glm::vec4& { return c.outlineColor; });

        ImGui::TextDisabled("Sombra");
        dragVec2("Shadow Offset##txt",
                 +[](TextComponent& c) -> glm::vec2& { return c.shadowOffset; },
                 0.25f, -64.0f, 64.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Píxeles. A {0,0} (o con alfa 0) no se emite ni un quad de sombra");
        colorEdit("Shadow Color##txt",
                  +[](TextComponent& c) -> glm::vec4& { return c.shadowColor; });

        ImGui::TextDisabled("Estilo de los tags <b> e <i>");
        dragFloat("Bold Strength##txt", +[](TextComponent& c) -> float& { return c.boldStrength; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Italic Skew##txt", +[](TextComponent& c) -> float& { return c.italicSkew; },
                  0.01f, -1.0f, 1.0f, "%.3f");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<TextComponentCommand>(
            *ctx.scene, "Quitar Text de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getText());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Text quitado de '" + ctx.selected->name + "'");
    }
}

namespace
{
    // Las tres rutas de imagen de la barra, en el mismo orden que el `field` que
    // se pasea por el file dialog y por el drop. Un accessor sin captura para
    // que el applier del undo (que sobrevive a la selección) no dependa de nada.
    std::string& barImagePathRef(ProgressBarComponent& c, int field)
    {
        if (field == 1) return c.backgroundPath;
        if (field == 2) return c.fillPath;
        return c.atlasPath;
    }

    const char* barImageFieldLabel(int field)
    {
        if (field == 1) return "Imagen de fondo de la barra de '";
        if (field == 2) return "Imagen de relleno de la barra de '";
        return "Atlas de la barra de '";
    }
}

void PropertiesPanel::setProgressBarImagePath(EditorContext& ctx, uint64_t ownerId, int field,
                                               const std::string& path)
{
    // Mismo veto que el Button y el Text, y por el mismo sitio: aquí pasan el
    // drop y el file dialog de las TRES cajas, así que el filtro solo hay que
    // ponerlo una vez.
    if (!isUiAtlasPath(path))
    {
        m_barPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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
    ctx.pushLog(lbl + " cambiada a " + path);
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
    // Sin condicionar a ctx.selected/hasProgressBar(): si no se drena aquí,
    // cambiar de selección con el diálogo abierto deja el flag atascado.
    if (m_barAtlasDlgOpen && m_barAtlasFileDialog->Display("BarImageDlg"))
    {
        if (m_barAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_barAtlasFileDialog->GetFilePathName()))
            setProgressBarImagePath(ctx, m_barAtlasDlgOwner, m_barAtlasDlgField,
                                    m_barAtlasFileDialog->GetFilePathName());
        m_barAtlasFileDialog->Close();
        m_barAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawProgressBarSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Barra de progreso de la UI 2D. Se dibuja en el árbol del Canvas "
                           "como fondo + relleno; el relleno sale de value contra [min, max].");

        // Los mismos accessors sin captura (function pointer) y el mismo baile
        // de undo que las secciones del Button y del Text. Los labels llevan
        // "##bar" porque un GameObject puede tener los tres componentes a la
        // vez: dos widgets con el MISMO id de ImGui comparten estado.
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
                const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
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
                    const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " de la barra de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        ImGui::TextDisabled("Valor");
        // Sin tope por min/max a propósito: el componente no clampa nada (lo
        // normaliza el sync), y el rango puede venir de un script.
        dragFloat("Value##bar", +[](ProgressBarComponent& c) -> float& { return c.value; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        dragFloat("Min##bar", +[](ProgressBarComponent& c) -> float& { return c.minValue; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        dragFloat("Max##bar", +[](ProgressBarComponent& c) -> float& { return c.maxValue; },
                  0.01f, -1e9f, 1e9f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Con Max <= Min la barra sale vacía: no hay forma de repartir "
                              "un intervalo vacío");

        static const char* kDirs[] = { "Left To Right", "Right To Left",
                                       "Bottom To Top", "Top To Bottom" };
        comboEnum("Fill Direction##bar", (int)p->fillDirection, kDirs, IM_ARRAYSIZE(kDirs),
                  +[](ProgressBarComponent& c, int v) {
                      c.fillDirection = (UiProgressFillDirection)v;
                  });

        ImGui::TextDisabled("Colores");
        colorEdit("Background Color##bar",
                  +[](ProgressBarComponent& c) -> glm::vec4& { return c.color; });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color del FONDO (el rect entero)");
        colorEdit("Fill Color##bar",
                  +[](ProgressBarComponent& c) -> glm::vec4& { return c.fillColor; });

        ImGui::TextDisabled("Imágenes");
        // Una ruta escribible a mano por campo + la caja de asset común
        // (drawAssetDropBox). El veto por extensión está en
        // setProgressBarImagePath, por donde pasan los tres campos y los dos
        // caminos.
        auto assetBox = [&](const char* label, int field, StrRef acc,
                            const char* idSuffix, const char* tip)
        {
            inputText(label, acc);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            // El idSuffix es único por campo: dos cajas con el mismo id
            // compartirían el estado de pulsado del botón.
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
                 "Imagen compartida: la usan las partes que no traigan la suya");
        assetBox("Background Image##bar", 1,
                 +[](ProgressBarComponent& c) -> std::string& { return c.backgroundPath; },
                 "barBg",
                 "Vacía = el Atlas; y sin ninguno, un quad de Background Color");
        assetBox("Fill Image##bar", 2,
                 +[](ProgressBarComponent& c) -> std::string& { return c.fillPath; },
                 "barFill",
                 "Vacía = el Atlas; y sin ninguno, un quad de Fill Color");

        if (!m_barPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_barPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ProgressBarComponentCommand>(
            *ctx.scene, "Quitar Progress Bar de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getProgressBar());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Progress Bar quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::drawLayoutSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        // El rect solo es suyo cuando no hay otro componente de UI en el objeto:
        // con uno, el rect lo manda aquel y estos campos no se leen. Se DICE en
        // vez de esconderlos: el editor no capa lo que el motor soporta.
        const bool ownsRect = !ctx.selected->hasButton() && !ctx.selected->hasText() &&
                              !ctx.selected->hasProgressBar();

        ImGui::TextWrapped("Auto-layout de la UI 2D: coloca a los HIJOS de este objeto. Sin "
                           "otro componente de UI aquí, el contenedor es un rect propio que "
                           "agrupa y recorta sin dibujarse.");

        // Mismos accessors sin captura (function pointer) y mismo baile de undo
        // que las secciones del Button, el Text y la ProgressBar. Los labels
        // llevan "##layout" porque un GameObject puede tener los cuatro
        // componentes a la vez: dos widgets con el MISMO id de ImGui comparten
        // estado.
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
                const std::string lbl = std::string(label) + " del layout de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del layout de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
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
                    const std::string lbl = std::string(label) + " del layout de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del layout de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Las columnas son un ENTERO, no un float con formato: un DragInt evita
        // que 3,4 columnas lleguen a la rejilla por el camino del redondeo.
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
                    const std::string lbl = std::string(label) + " del layout de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::SetTooltip("None = solo agrupa y recorta; los hijos se anclan por su cuenta");

        ImGui::TextDisabled("Rect del contenedor");
        if (!ownsRect)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "El rect lo manda el otro componente de UI de este objeto");
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

        ImGui::TextDisabled("Colocación");
        dragVec2("Spacing##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.spacing; },
                 1.0f, -16384.0f, 16384.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("X entre columnas, Y entre filas");

        static const char* kAlin[] = { "Start", "Center", "End" };
        comboEnum("Cross Align##layout", (int)l->crossAlign, kAlin, IM_ARRAYSIZE(kAlin),
                  +[](LayoutComponent& c, int v) { c.crossAlign = (UiCrossAlign)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Alineación en el eje TRANSVERSAL. El Grid no la usa: la celda "
                              "ya fija los dos ejes");

        // Los dos campos de la rejilla se enseñan siempre, deshabilitados fuera
        // de Grid: esconderlos cambiaría la FORMA del panel al tocar el modo, y
        // eso es justo lo que hace que un campo parezca perdido.
        const bool esGrid = l->mode == UiLayoutMode::Grid;
        if (!esGrid) ImGui::BeginDisabled();
        dragVec2("Cell Size##layout", +[](LayoutComponent& c) -> glm::vec2& { return c.cellSize; },
                 1.0f, 0.0f, 16384.0f, "%.0f");
        dragColumns("Columns##layout");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = las que quepan en el ancho del contenedor");
        if (!esGrid) ImGui::EndDisabled();

        ImGui::TextDisabled("Content size fitter");
        checkBox("Fit Width##layout", +[](LayoutComponent& c) -> bool& { return c.fitWidth; });
        checkBox("Fit Height##layout", +[](LayoutComponent& c) -> bool& { return c.fitHeight; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ese eje del Size pasa a ser la extensión de los hijos colocados "
                              "más el padding");

        ImGui::TextDisabled("Este objeto dentro del layout de su padre");
        checkBox("Ignore Layout##layout",
                 +[](LayoutComponent& c) -> bool& { return c.ignoreLayout; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Se ancla por su cuenta y NO ocupa hueco en el layout del padre");
        checkBox("Clip Children##layout",
                 +[](LayoutComponent& c) -> bool& { return c.clipChildren; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Recorta a los descendientes contra este rect (se INTERSECA con "
                              "el recorte que venga del padre)");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<LayoutComponentCommand>(
            *ctx.scene, "Quitar Layout de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getLayout());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Layout quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setPanelAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                         const std::string& path)
{
    // Mismo veto que el Button, el Text y la barra, y por el mismo sitio: aquí
    // pasan el drop y el file dialog, así que el filtro va una sola vez.
    if (!isUiAtlasPath(path))
    {
        m_panelPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = "Atlas del panel de '" + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected/hasPanel(): si no se drena aquí, cambiar de
    // selección con el diálogo abierto deja el flag atascado en true para
    // siempre (mismo motivo que drawMeshDialog).
    if (m_panelAtlasDlgOpen && m_panelAtlasFileDialog->Display("PanelAtlasDlg"))
    {
        if (m_panelAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_panelAtlasFileDialog->GetFilePathName()))
            setPanelAtlasPath(ctx, m_panelAtlasDlgOwner,
                              m_panelAtlasFileDialog->GetFilePathName());
        m_panelAtlasFileDialog->Close();
        m_panelAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawPanelSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Rectángulo de fondo de la UI 2D. Sin atlas es un quad de color "
                           "plano; con atlas, el sprite estirado al rect.");

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
                const std::string lbl = std::string(label) + " del panel de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, v,
                        [scene, id, acc](const bool& val) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasPanel()) acc(*go->getPanel()) = val;
                        }));
            }
        };

        // "before" leído ANTES de dibujar, sesión abierta en IsItemActivated y
        // commit en IsItemDeactivatedAfterEdit: un arrastre entero es UN paso de
        // undo, no uno por frame.
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
                    const std::string lbl = std::string(label) + " del panel de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del panel de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del panel de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin él se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, p->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*p);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no está en el atlas NO se pierde ni se corrige
            // solo: se enseña al final marcado.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " del panel de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("A false el panel deja pasar el ratón a lo que tenga detrás.\n"
                              "Un fondo a pantalla completa con esto activo se come TODOS los\n"
                              "clics y no hay nada que lo delate a la vista.");

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

        // Sin atlas no hay nada que trocear, y el botón deshabilitado dice por
        // qué mejor que su ausencia.
        ImGui::BeginDisabled(p->atlasPath.empty() || !ctx.openSpriteEditor);
        if (ImGui::Button("Editar sprites...##panel")) ctx.openSpriteEditor(p->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && p->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Sprite##panel", +[](PanelComponent& c) -> std::string& { return c.sprite; });

        if (!m_panelPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_panelPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<PanelComponentCommand>(
            *ctx.scene, "Quitar Panel de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getPanel());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Panel quitado de '" + ctx.selected->name + "'");
    }
}

void PropertiesPanel::setImageAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                         const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_imagePathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = "Atlas de la imagen de '" + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
        if (m_imageAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_imageAtlasFileDialog->GetFilePathName()))
            setImageAtlasPath(ctx, m_imageAtlasDlgOwner,
                              m_imageAtlasFileDialog->GetFilePathName());
        m_imageAtlasFileDialog->Close();
        m_imageAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawImageSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay sección, igual que los colliders.
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
        ImGui::TextWrapped("Sprite de la UI 2D. Los cuatro modos se resuelven en CPU dentro del "
                           "batcher (N quads del mismo atlas y el mismo scissor).");

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
                const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                ctx.pushLog(lbl + (v ? " activado" : " desactivado"));
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
                    const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("Tinte del sprite (se multiplica)");
        checkBox("Visible##image", +[](ImageComponent& c) -> bool& { return c.visible; });
        checkBox("Raycast Target##image",
                 +[](ImageComponent& c) -> bool& { return c.raycastTarget; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A false la imagen deja pasar el ratón a lo que tenga detrás");

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
        if (ImGui::Button("Editar sprites...##image")) ctx.openSpriteEditor(im->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && im->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Sprite##image", +[](ImageComponent& c) -> std::string& { return c.sprite; });

        if (!m_imagePathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_imagePathError.c_str());

        // Los tres bloques de abajo se enseñan SIEMPRE, no solo el del modo
        // activo: los campos son del componente y siguen ahí al cambiar de modo,
        // y esconderlos haría creer que se han perdido.
        ImGui::TextDisabled("Modo");
        static const char* kModes[] = { "Normal", "Tiled", "Sliced", "Filled" };
        comboEnum("Mode##image", (int)im->mode, kModes, IM_ARRAYSIZE(kModes),
                  +[](ImageComponent& c, int v) { c.mode = (UiImageMode)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Normal: el sprite estirado al rect.\n"
                              "Tiled: repetido a su tamaño nativo.\n"
                              "Sliced: 9-slice, las esquinas no se estiran.\n"
                              "Filled: solo una fracción del rect.");

        ImGui::TextDisabled("Sliced (bordes en píxeles DEL SPRITE)");
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
            ImGui::SetTooltip("Sin centro salen 8 quads: es lo que quiere un marco que deja\n"
                              "ver lo de detrás");

        ImGui::TextDisabled("Tiled");
        {
            // maxTiles es un uint32 y no hay dragUint: se edita como entero con
            // el mismo baile de undo que los demás.
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
                    const std::string lbl = "Max Tiles de la imagen de '" + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::SetTooltip("Tope duro de quads. Pasado el tope, el elemento se dibuja como\n"
                              "Normal en vez de reventar el buffer de vértices.");

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
            ImGui::SetTooltip("Start es izquierda en Horizontal y arriba en Vertical");
        dragFloat("Fill Amount##image",
                  +[](ImageComponent& c) -> float& { return c.fillAmount; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A 0 no se emite ni un quad");

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ImageComponentCommand>(
            *ctx.scene, "Quitar Image de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getImage());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Image quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setSliderAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Mismo veto que el resto de componentes de UI, y por el mismo sitio: aqui
    // pasan el drop y el file dialog, asi que el filtro va una sola vez.
    if (!isUiAtlasPath(path))
    {
        m_sliderPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "del slider de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_sliderAtlasDlgOpen && m_sliderAtlasFileDialog->Display("SliderAtlasDlg"))
    {
        if (m_sliderAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_sliderAtlasFileDialog->GetFilePathName()))
            setSliderAtlasPath(ctx, m_sliderAtlasDlgOwner, m_sliderAtlasFileDialog->GetFilePathName());
        m_sliderAtlasFileDialog->Close();
        m_sliderAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawSliderSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Widget de valor arrastrable. La pista entera es zona de clic; el asa se descuenta del recorrido para no salirse por las puntas.");

        using FloatRef = float&       (*)(SliderComponent&);
        using Vec2Ref  = glm::vec2&   (*)(SliderComponent&);
        using Vec4Ref  = glm::vec4&   (*)(SliderComponent&);
        using StrRef   = std::string& (*)(SliderComponent&);
        using BoolRef  = bool&        (*)(SliderComponent&);
        using EnumSet  = void         (*)(SliderComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        // Combos y checkbox se commitean en el acto: un click = un cambio.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sl, idx);
                const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasSlider()) acc(*go->getSlider()) = v;
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: "before" leido ANTES de
        // dibujar, sesion abierta en IsItemActivated y commit en
        // IsItemDeactivatedAfterEdit, asi un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sl);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sl->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sl);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no esta en el atlas NO se pierde ni se corrige
            // solo: se ensena al final marcado.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " del slider de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("A false se dibuja igual pero no se deja mover");

        ImGui::TextDisabled("Valor");
        dragFloat("Value##slider", +[](SliderComponent& c) -> float& { return c.value; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        dragFloat("Min##slider", +[](SliderComponent& c) -> float& { return c.minValue; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        dragFloat("Max##slider", +[](SliderComponent& c) -> float& { return c.maxValue; },
                  0.01f, -1e6f, 1e6f, "%.3f");
        checkBox("Whole Numbers##slider", +[](SliderComponent& c) -> bool& { return c.wholeNumbers; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Redondea el valor que se ESCRIBE, no solo el que se dibuja");

        static const char* kSliderDirs[] = { "Left To Right", "Right To Left",
                                             "Bottom To Top", "Top To Bottom" };
        comboEnum("Direction##slider", (int)sl->direction, kSliderDirs, IM_ARRAYSIZE(kSliderDirs),
                  +[](SliderComponent& c, int v) { c.direction = (UiSliderDirection)v; });

        ImGui::TextDisabled("Colores y asa");
        colorEdit("Track Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.color; });
        colorEdit("Fill Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.fillColor; });
        colorEdit("Handle Color##slider", +[](SliderComponent& c) -> glm::vec4& { return c.handleColor; });
        dragFloat("Handle Size##slider", +[](SliderComponent& c) -> float& { return c.handleSize; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Largo del asa EN EL EJE del recorrido, en px. A 0 el recorrido\n"
                              "es el rect entero.");

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
        if (ImGui::Button("Editar sprites...##slider")) ctx.openSpriteEditor(sl->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sl->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
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
            *ctx.scene, "Quitar Slider de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getSlider());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Slider quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setCheckboxAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Mismo veto que el resto de componentes de UI, y por el mismo sitio: aqui
    // pasan el drop y el file dialog, asi que el filtro va una sola vez.
    if (!isUiAtlasPath(path))
    {
        m_checkboxPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "de la casilla de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_checkboxAtlasDlgOpen && m_checkboxAtlasFileDialog->Display("CheckboxAtlasDlg"))
    {
        if (m_checkboxAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_checkboxAtlasFileDialog->GetFilePathName()))
            setCheckboxAtlasPath(ctx, m_checkboxAtlasDlgOwner, m_checkboxAtlasFileDialog->GetFilePathName());
        m_checkboxAtlasFileDialog->Close();
        m_checkboxAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawCheckboxSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Casilla de verificacion. Un click invierte el valor. La etiqueta de texto va aparte: el componente Text cabe en este mismo GameObject.");

        using FloatRef = float&       (*)(CheckboxComponent&);
        using Vec2Ref  = glm::vec2&   (*)(CheckboxComponent&);
        using Vec4Ref  = glm::vec4&   (*)(CheckboxComponent&);
        using StrRef   = std::string& (*)(CheckboxComponent&);
        using BoolRef  = bool&        (*)(CheckboxComponent&);
        using EnumSet  = void         (*)(CheckboxComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        // Combos y checkbox se commitean en el acto: un click = un cambio.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*cb, idx);
                const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCheckbox()) acc(*go->getCheckbox()) = v;
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: "before" leido ANTES de
        // dibujar, sesion abierta en IsItemActivated y commit en
        // IsItemDeactivatedAfterEdit, asi un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*cb);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, cb->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*cb);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no esta en el atlas NO se pierde ni se corrige
            // solo: se ensena al final marcado.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " de la casilla de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("A false se dibuja igual pero el click no la mueve");

        ImGui::TextDisabled("Valor y marca");
        checkBox("Is On##checkbox", +[](CheckboxComponent& c) -> bool& { return c.isOn; });
        colorEdit("Box Color##checkbox", +[](CheckboxComponent& c) -> glm::vec4& { return c.color; });
        colorEdit("Check Color##checkbox", +[](CheckboxComponent& c) -> glm::vec4& { return c.checkColor; });
        dragFloat("Check Padding##checkbox", +[](CheckboxComponent& c) -> float& { return c.checkPadding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Px que la marca se mete hacia dentro por los cuatro lados.\n"
                              "Uno que no cabe deja la marca a cero, nunca del reves.");

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
        if (ImGui::Button("Editar sprites...##checkbox")) ctx.openSpriteEditor(cb->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && cb->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Background##checkbox", +[](CheckboxComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Checkmark##checkbox", +[](CheckboxComponent& c) -> std::string& { return c.checkmarkSprite; });

        if (!m_checkboxPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_checkboxPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<CheckboxComponentCommand>(
            *ctx.scene, "Quitar Checkbox de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getCheckbox());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Checkbox quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setToggleAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Mismo veto que el resto de componentes de UI, y por el mismo sitio: aqui
    // pasan el drop y el file dialog, asi que el filtro va una sola vez.
    if (!isUiAtlasPath(path))
    {
        m_togglePathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "del interruptor de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_toggleAtlasDlgOpen && m_toggleAtlasFileDialog->Display("ToggleAtlasDlg"))
    {
        if (m_toggleAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_toggleAtlasFileDialog->GetFilePathName()))
            setToggleAtlasPath(ctx, m_toggleAtlasDlgOwner, m_toggleAtlasFileDialog->GetFilePathName());
        m_toggleAtlasFileDialog->Close();
        m_toggleAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawToggleSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Interruptor deslizante. Guarda el mismo dato que el Checkbox (un bool) pero con otros campos: dos colores de pista y el tamano del mando.");

        using FloatRef = float&       (*)(ToggleComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ToggleComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ToggleComponent&);
        using StrRef   = std::string& (*)(ToggleComponent&);
        using BoolRef  = bool&        (*)(ToggleComponent&);
        using EnumSet  = void         (*)(ToggleComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        // Combos y checkbox se commitean en el acto: un click = un cambio.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*tg, idx);
                const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasToggle()) acc(*go->getToggle()) = v;
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: "before" leido ANTES de
        // dibujar, sesion abierta en IsItemActivated y commit en
        // IsItemDeactivatedAfterEdit, asi un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*tg);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, tg->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*tg);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no esta en el atlas NO se pierde ni se corrige
            // solo: se ensena al final marcado.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " del interruptor de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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

        ImGui::TextDisabled("Valor, colores y mando");
        checkBox("Is On##toggle", +[](ToggleComponent& c) -> bool& { return c.isOn; });
        colorEdit("Off Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.offColor; });
        colorEdit("On Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.onColor; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("La pista NO tiene un color suelto: se pinta con este o con\n"
                              "Off Color segun el estado.");
        colorEdit("Knob Color##toggle", +[](ToggleComponent& c) -> glm::vec4& { return c.knobColor; });
        dragFloat("Knob Size##toggle", +[](ToggleComponent& c) -> float& { return c.knobSize; },
                  0.5f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Se acota a lo que quede entre paddings: uno mas grande que la\n"
                              "pista asomaria por el borde.");
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
        if (ImGui::Button("Editar sprites...##toggle")) ctx.openSpriteEditor(tg->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && tg->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Background##toggle", +[](ToggleComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Knob##toggle", +[](ToggleComponent& c) -> std::string& { return c.knobSprite; });

        if (!m_togglePathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_togglePathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ToggleComponentCommand>(
            *ctx.scene, "Quitar Toggle de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getToggle());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Toggle quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setScrollbarAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    // Mismo veto que el resto de componentes de UI, y por el mismo sitio: aqui
    // pasan el drop y el file dialog, asi que el filtro va una sola vez.
    if (!isUiAtlasPath(path))
    {
        m_scrollbarPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "de la barra de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_scrollbarAtlasDlgOpen && m_scrollbarAtlasFileDialog->Display("ScrollbarAtlasDlg"))
    {
        if (m_scrollbarAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_scrollbarAtlasFileDialog->GetFilePathName()))
            setScrollbarAtlasPath(ctx, m_scrollbarAtlasDlgOwner, m_scrollbarAtlasFileDialog->GetFilePathName());
        m_scrollbarAtlasFileDialog->Close();
        m_scrollbarAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawScrollbarSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Canal con asa de tamano variable. El valor va siempre en 0..1: quien lo interpreta es lo que se desplaza, no la barra.");

        using FloatRef = float&       (*)(ScrollbarComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ScrollbarComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ScrollbarComponent&);
        using StrRef   = std::string& (*)(ScrollbarComponent&);
        using BoolRef  = bool&        (*)(ScrollbarComponent&);
        using EnumSet  = void         (*)(ScrollbarComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        // Combos y checkbox se commitean en el acto: un click = un cambio.
        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sb, idx);
                const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollbar()) acc(*go->getScrollbar()) = v;
                        }));
            }
        };

        // Los escalares comparten el baile de siempre: "before" leido ANTES de
        // dibujar, sesion abierta en IsItemActivated y commit en
        // IsItemDeactivatedAfterEdit, asi un arrastre entero es UN paso de undo.
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
                    const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sb);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sb->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sb);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            // Un nombre que ya no esta en el atlas NO se pierde ni se corrige
            // solo: se ensena al final marcado.
            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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

        ImGui::TextDisabled("Valor");
        dragFloat("Value##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.value; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Handle Fraction##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.handleFraction; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fraccion del canal que ocupa el asa: 1 = el contenido cabe\n"
                              "entero y no hay nada que desplazar.");

        static const char* kScrollDirs[] = { "Left To Right", "Right To Left",
                                             "Top To Bottom", "Bottom To Top" };
        comboEnum("Direction##scrollbar", (int)sb->direction, kScrollDirs, IM_ARRAYSIZE(kScrollDirs),
                  +[](ScrollbarComponent& c, int v) { c.direction = (UiScrollbarDirection)v; });

        {
            // numberOfSteps es un uint32 y no hay dragUint: se edita como entero
            // con el mismo baile de undo que los demas.
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
                    const std::string lbl = "Number Of Steps de la barra de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::SetTooltip("Paradas discretas. 0 y 1 = continuo: enganchar a una sola\n"
                              "parada dejaria la barra muerta en un sitio.");

        dragFloat("Scroll Step##scrollbar", +[](ScrollbarComponent& c) -> float& { return c.scrollStep; },
                  0.01f, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cuanto mueve la rueda por muesca, en fraccion del recorrido");

        ImGui::TextDisabled("Colores");
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
        if (ImGui::Button("Editar sprites...##scrollbar")) ctx.openSpriteEditor(sb->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sb->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Background##scrollbar", +[](ScrollbarComponent& c) -> std::string& { return c.backgroundSprite; });
        spriteField("Handle##scrollbar", +[](ScrollbarComponent& c) -> std::string& { return c.handleSprite; });

        if (!m_scrollbarPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_scrollbarPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ScrollbarComponentCommand>(
            *ctx.scene, "Quitar Scrollbar de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getScrollbar());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Scrollbar quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setInputFieldAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_inputFieldPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "del campo de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Mismo veto y por el mismo sitio que el atlas: aqui pasan el drop y el file
    // dialog, asi que el filtro va una sola vez.
    if (!isUiFontPath(path))
    {
        m_inputFieldPathError = "No es una fuente (.ttf .otf .ttc): " +
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

    const std::string lbl = std::string("Fuente ") + "del campo de " + go->name + "'";
    ctx.pushLog(lbl + " cambiada a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_inputFieldAtlasDlgOpen && m_inputFieldAtlasFileDialog->Display("InputFieldAtlasDlg"))
    {
        if (m_inputFieldAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_inputFieldAtlasFileDialog->GetFilePathName()))
            setInputFieldAtlasPath(ctx, m_inputFieldAtlasDlgOwner, m_inputFieldAtlasFileDialog->GetFilePathName());
        m_inputFieldAtlasFileDialog->Close();
        m_inputFieldAtlasDlgOpen = false;
    }

    if (m_inputFieldFontDlgOpen && m_inputFieldFontFileDialog->Display("InputFieldFontDlg"))
    {
        if (m_inputFieldFontFileDialog->IsOk() &&
            assetAllowed(ctx, m_inputFieldFontFileDialog->GetFilePathName()))
            setInputFieldFontPath(ctx, m_inputFieldFontDlgOwner, m_inputFieldFontFileDialog->GetFilePathName());
        m_inputFieldFontFileDialog->Close();
        m_inputFieldFontDlgOpen = false;
    }
}

void PropertiesPanel::drawInputFieldSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Campo de texto. Es el unico widget en el que escribe el JUGADOR: el canvas entrega los caracteres al elemento con foco y el handler los mete aqui.");

        using FloatRef = float&       (*)(InputFieldComponent&);
        using Vec2Ref  = glm::vec2&   (*)(InputFieldComponent&);
        using Vec4Ref  = glm::vec4&   (*)(InputFieldComponent&);
        using StrRef   = std::string& (*)(InputFieldComponent&);
        using BoolRef  = bool&        (*)(InputFieldComponent&);
        using EnumSet  = void         (*)(InputFieldComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*fld, idx);
                const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasInputField()) acc(*go->getInputField()) = v;
                        }));
            }
        };

        // "before" leido ANTES de dibujar, sesion abierta en IsItemActivated y
        // commit en IsItemDeactivatedAfterEdit: un arrastre entero es UN paso de
        // undo, no uno por frame.
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
                    const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*fld);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, fld->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*fld);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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
            ImGui::SetTooltip("A false ni siquiera toma el foco");
        checkBox("Read Only##inputfield", +[](InputFieldComponent& c) -> bool& { return c.readOnly; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toma el foco y deja mover el cursor, pero no cambiar el texto");

        ImGui::TextDisabled("Texto");
        inputText("Text##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.text; });
        inputText("Placeholder##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.placeholder; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lo que se ensena con el campo VACIO, con su propio color");
        dragFloat("Font Size##inputfield", +[](InputFieldComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Text Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.textColor; });
        colorEdit("Placeholder Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.placeholderColor; });

        static const char* kInputAligns[] = { "Left", "Center", "Right", "Justify" };
        comboEnum("Align##inputfield", (int)fld->align, kInputAligns, IM_ARRAYSIZE(kInputAligns),
                  +[](InputFieldComponent& c, int v) { c.align = (UiTextAlign)v; });
        dragFloat("Padding##inputfield", +[](InputFieldComponent& c) -> float& { return c.padding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");

        ImGui::TextDisabled("Filtro");
        {
            // characterLimit es un uint32 y no hay dragUint: se edita como entero
            // con el mismo baile de undo que los demas.
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
                    const std::string lbl = "Character Limit del campo de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::SetTooltip("Cuenta CARACTERES, no bytes. 0 = sin limite.");

        static const char* kContentTypes[] = { "Standard", "Integer Number", "Decimal Number",
                                                "Alphanumeric", "Password" };
        comboEnum("Content Type##inputfield", (int)fld->contentType, kContentTypes,
                  IM_ARRAYSIZE(kContentTypes),
                  +[](InputFieldComponent& c, int v) { c.contentType = (UiInputContentType)v; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Filtra lo que se puede TECLEAR. Password guarda el texto tal\n"
                              "cual y solo cambia lo que se ensena.");
        inputText("Password Char##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.passwordChar; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vacio cae al asterisco: un campo que no ensena NADA parece roto");

        ImGui::TextDisabled("Cursor");
        colorEdit("Caret Color##inputfield", +[](InputFieldComponent& c) -> glm::vec4& { return c.caretColor; });
        dragFloat("Caret Width##inputfield", +[](InputFieldComponent& c) -> float& { return c.caretWidth; },
                  0.1f, 0.0f, 64.0f, "%.2f");
        dragFloat("Caret Blink Rate##inputfield", +[](InputFieldComponent& c) -> float& { return c.caretBlinkRate; },
                  0.01f, 0.0f, 10.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Segundos por medio ciclo. 0 = fijo, sin parpadeo.");

        ImGui::TextDisabled("Fuente");
        inputText("Font##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vacia = la fuente por defecto del proyecto");
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
        if (ImGui::Button("Editar sprites...##inputfield")) ctx.openSpriteEditor(fld->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && fld->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Background##inputfield", +[](InputFieldComponent& c) -> std::string& { return c.backgroundSprite; });

        if (!m_inputFieldPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_inputFieldPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<InputFieldComponentCommand>(
            *ctx.scene, "Quitar Input Field de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getInputField());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Input Field quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setDropdownAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_dropdownPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "del desplegable de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Mismo veto y por el mismo sitio que el atlas: aqui pasan el drop y el file
    // dialog, asi que el filtro va una sola vez.
    if (!isUiFontPath(path))
    {
        m_dropdownPathError = "No es una fuente (.ttf .otf .ttc): " +
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

    const std::string lbl = std::string("Fuente ") + "del desplegable de " + go->name + "'";
    ctx.pushLog(lbl + " cambiada a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_dropdownAtlasDlgOpen && m_dropdownAtlasFileDialog->Display("DropdownAtlasDlg"))
    {
        if (m_dropdownAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_dropdownAtlasFileDialog->GetFilePathName()))
            setDropdownAtlasPath(ctx, m_dropdownAtlasDlgOwner, m_dropdownAtlasFileDialog->GetFilePathName());
        m_dropdownAtlasFileDialog->Close();
        m_dropdownAtlasDlgOpen = false;
    }

    if (m_dropdownFontDlgOpen && m_dropdownFontFileDialog->Display("DropdownFontDlg"))
    {
        if (m_dropdownFontFileDialog->IsOk() &&
            assetAllowed(ctx, m_dropdownFontFileDialog->GetFilePathName()))
            setDropdownFontPath(ctx, m_dropdownFontDlgOwner, m_dropdownFontFileDialog->GetFilePathName());
        m_dropdownFontFileDialog->Close();
        m_dropdownFontDlgOpen = false;
    }
}

void PropertiesPanel::drawDropdownSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Desplegable. Anadir o quitar una opcion cambia la FORMA del arbol de UI, asi que el canvas se reconstruye; abrir y cerrar no.");

        using FloatRef = float&       (*)(DropdownComponent&);
        using Vec2Ref  = glm::vec2&   (*)(DropdownComponent&);
        using Vec4Ref  = glm::vec4&   (*)(DropdownComponent&);
        using StrRef   = std::string& (*)(DropdownComponent&);
        using BoolRef  = bool&        (*)(DropdownComponent&);
        using EnumSet  = void         (*)(DropdownComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*dd, idx);
                const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasDropdown()) acc(*go->getDropdown()) = v;
                        }));
            }
        };

        // "before" leido ANTES de dibujar, sesion abierta en IsItemActivated y
        // commit en IsItemDeactivatedAfterEdit: un arrastre entero es UN paso de
        // undo, no uno por frame.
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
                    const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*dd);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, dd->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*dd);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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

        ImGui::TextDisabled("Opciones");
        {
            // La lista entera es UN paso de undo: anadir, quitar o renombrar
            // empuja el vector completo. Por campo serian tres comandos para lo
            // que el usuario vive como un cambio.
            const std::vector<std::string> before = dd->options;
            bool cambiada = false;

            for (size_t k = 0; k < dd->options.size(); k++)
            {
                ImGui::PushID((int)k);
                char buf[256] = {};
                strncpy_s(buf, dd->options[k].c_str(), sizeof(buf) - 1);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
                if (ImGui::InputText("##opt", buf, sizeof(buf)))
                {
                    dd->options[k] = std::string(buf);
                    // El renombrado NO se commitea por tecla: se empuja al salir
                    // del campo, igual que cualquier otro InputText.
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) cambiada = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    dd->options.erase(dd->options.begin() + (long)k);
                    // Quitar la opcion elegida deja el valor apuntando a otra:
                    // se acota aqui para que el combo no ensene vacio.
                    if (dd->value >= (int)dd->options.size())
                        dd->value = (int)dd->options.size() - 1;
                    if (dd->value < 0) dd->value = 0;
                    cambiada = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Anadir opcion##dropdown"))
            {
                dd->options.push_back("Opcion " + std::to_string(dd->options.size() + 1));
                cambiada = true;
            }

            if (cambiada && scene && ctx.undo && dd->options != before)
            {
                const std::string lbl = std::string("Opciones ") + "del desplegable de " + owner + "'";
                ctx.pushLog(lbl + " cambiadas");
                ctx.undo->push(std::make_unique<PropertyCommand<std::vector<std::string>>>(
                    lbl, before, dd->options,
                    [scene, id](const std::vector<std::string>& v) {
                        if (GameObject* go = scene->findById(id))
                            if (go->hasDropdown())
                            {
                                go->getDropdown()->options = v;
                                // El indice tambien se acota al deshacer: una
                                // lista mas corta no puede dejarlo fuera.
                                int& val = go->getDropdown()->value;
                                if (val >= (int)v.size()) val = (int)v.size() - 1;
                                if (val < 0) val = 0;
                            }
                    }));
            }
        }

        {
            // El valor es el INDICE 0-based, igual que en C++ y en Lua.
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
                    const std::string lbl = "Value del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::TextDisabled("Elegida: %s", dd->selectedLabel().c_str());
        else if (!dd->options.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "El indice no apunta a ninguna opcion");

        ImGui::TextDisabled("Lista");
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
                    const std::string lbl = "Max Visible Items del desplegable de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
            ImGui::SetTooltip("0 = todas. Acota el alto de la lista para que un combo de\n"
                              "cincuenta idiomas no ocupe tres pantallas.");

        colorEdit("List Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.listColor; });
        colorEdit("Item Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.itemColor; });
        colorEdit("Item Selected##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.itemSelectedColor; });
        colorEdit("Arrow Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.arrowColor; });

        ImGui::TextDisabled("Texto");
        dragFloat("Font Size##dropdown", +[](DropdownComponent& c) -> float& { return c.fontSize; },
                  0.5f, 1.0f, 512.0f, "%.1f");
        colorEdit("Text Color##dropdown", +[](DropdownComponent& c) -> glm::vec4& { return c.textColor; });
        dragFloat("Padding##dropdown", +[](DropdownComponent& c) -> float& { return c.padding; },
                  0.5f, 0.0f, 4096.0f, "%.1f");

        ImGui::TextDisabled("Fuente");
        inputText("Font##dropdown", +[](DropdownComponent& c) -> std::string& { return c.fontPath; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vacia = la fuente por defecto del proyecto");
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
        if (ImGui::Button("Editar sprites...##dropdown")) ctx.openSpriteEditor(dd->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && dd->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
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
            *ctx.scene, "Quitar Dropdown de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getDropdown());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Dropdown quitado de '" + ctx.selected->name + "'");
    }
}


void PropertiesPanel::setScrollViewAtlasPath(EditorContext& ctx, uint64_t ownerId,
                                       const std::string& path)
{
    if (!isUiAtlasPath(path))
    {
        m_scrollViewPathError = "No es una imagen (.png .jpg .jpeg .bmp .tga): " +
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

    const std::string lbl = std::string("Atlas ") + "de la vista de " + go->name + "'";
    ctx.pushLog(lbl + " cambiado a " + path);
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
    // Sin condicionar a ctx.selected: si no se drena aqui, cambiar de seleccion
    // con el dialogo abierto deja el flag atascado en true para siempre.
    if (m_scrollViewAtlasDlgOpen && m_scrollViewAtlasFileDialog->Display("ScrollViewAtlasDlg"))
    {
        if (m_scrollViewAtlasFileDialog->IsOk() &&
            assetAllowed(ctx, m_scrollViewAtlasFileDialog->GetFilePathName()))
            setScrollViewAtlasPath(ctx, m_scrollViewAtlasDlgOwner, m_scrollViewAtlasFileDialog->GetFilePathName());
        m_scrollViewAtlasFileDialog->Close();
        m_scrollViewAtlasDlgOpen = false;
    }
}

void PropertiesPanel::drawScrollViewSection(EditorContext& ctx)
{
    // Add-gate: sin el componente no hay seccion, igual que los colliders.
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
        ImGui::TextWrapped("Vista desplazable. Los HIJOS de este GameObject cuelgan de su contenido, no del viewport: por eso desplazarse los arrastra.");

        using FloatRef = float&       (*)(ScrollViewComponent&);
        using Vec2Ref  = glm::vec2&   (*)(ScrollViewComponent&);
        using Vec4Ref  = glm::vec4&   (*)(ScrollViewComponent&);
        using StrRef   = std::string& (*)(ScrollViewComponent&);
        using BoolRef  = bool&        (*)(ScrollViewComponent&);
        using EnumSet  = void         (*)(ScrollViewComponent&, int);
        (void)sizeof(EnumSet);   // no todos los widgets tienen enum

        auto comboEnum = [&](const char* label, int before, const char* const* items,
                             int count, EnumSet apply)
        {
            int idx = before;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::Combo(label, &idx, items, count) && idx != before)
            {
                apply(*sv, idx);
                const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                ctx.pushLog(lbl + " cambiado a " + items[idx]);
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
                const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasScrollView()) acc(*go->getScrollView()) = v;
                        }));
            }
        };

        // "before" leido ANTES de dibujar, sesion abierta en IsItemActivated y
        // commit en IsItemDeactivatedAfterEdit: un arrastre entero es UN paso de
        // undo, no uno por frame.
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
                    const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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
                    const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un InputText entero (escribir y salir del campo) es UN paso de undo,
        // no uno por tecla: mismo criterio que el arrastre de un DragFloat.
        auto inputText = [&](const char* label, StrRef acc)
        {
            const std::string before = acc(*sv);
            char buf[512] = {};
            strncpy_s(buf, before.c_str(), sizeof(buf) - 1);
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
                    const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado");
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

        // Un sprite es un NOMBRE dentro del atlas, no texto libre. Con sidecar
        // (<atlas>.sprites.json) se elige de la lista; sin el se cae al campo de
        // texto, que sigue valiendo para un atlas troceado a mano.
        auto spriteField = [&](const char* label, StrRef acc)
        {
            const std::vector<std::string>& nombres = spriteNamesFor(ctx, sv->atlasPath);
            if (nombres.empty()) { inputText(label, acc); return; }

            const std::string before = acc(*sv);

            std::vector<const char*> items;
            items.reserve(nombres.size() + 2);
            items.push_back("(imagen entera)");
            for (const std::string& n : nombres) items.push_back(n.c_str());

            int current = 0;
            for (size_t i = 0; i < nombres.size(); ++i)
                if (nombres[i] == before) { current = (int)i + 1; break; }

            std::string huerfano;
            if (current == 0 && !before.empty())
            {
                huerfano = before + "  (no esta en el atlas)";
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
                    const std::string lbl = std::string(label) + " de la vista de " + owner + "'";
                    ctx.pushLog(lbl + " cambiado a '" + after + "'");
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

        ImGui::TextDisabled("Ejes y contenido");
        checkBox("Horizontal##scrollview", +[](ScrollViewComponent& c) -> bool& { return c.horizontal; });
        checkBox("Vertical##scrollview", +[](ScrollViewComponent& c) -> bool& { return c.vertical; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Un eje apagado no se mueve aunque el contenido sea mas grande");
        dragVec2("Content Size##scrollview", +[](ScrollViewComponent& c) -> glm::vec2& { return c.contentSize; },
                 1.0f, 0.0f, 65536.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tamano del area desplazable. Es un CAMPO y no algo medido de los\n"
                              "hijos: medir el subarbol cada frame acoplaria el scroll al layout.");
        dragVec2("Normalized Pos##scrollview",
                 +[](ScrollViewComponent& c) -> glm::vec2& { return c.normalizedPosition; },
                 0.01f, 0.0f, 1.0f, "%.3f");
        dragFloat("Scroll Sensitivity##scrollview",
                  +[](ScrollViewComponent& c) -> float& { return c.scrollSensitivity; },
                  1.0f, 0.0f, 4096.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pixeles que mueve la rueda por muesca");

        {
            const glm::vec2 r = sv->scrollRange();
            ImGui::TextDisabled("Recorrido: %.0f x %.0f px", r.x, r.y);
            if (r.x <= 0.0f && r.y <= 0.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                   "El contenido cabe entero: no hay nada que desplazar");
        }
        ImGui::TextDisabled("Los hijos de este GameObject cuelgan del contenido, no del viewport.");

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
        if (ImGui::Button("Editar sprites...##scrollview")) ctx.openSpriteEditor(sv->atlasPath);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && sv->atlasPath.empty())
            ImGui::SetTooltip("Primero elige un atlas");
        spriteField("Background##scrollview", +[](ScrollViewComponent& c) -> std::string& { return c.backgroundSprite; });

        if (!m_scrollViewPathError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_scrollViewPathError.c_str());

        ImGui::TreePop();
    }

    if (removeClicked && ctx.scene && ctx.undo)
    {
        auto cmd = std::make_unique<ScrollViewComponentCommand>(
            *ctx.scene, "Quitar Scroll View de '" + ctx.selected->name + "'", ctx.selected->id,
            /*add=*/false, *ctx.selected->getScrollView());
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        ctx.pushLog("Componente Scroll View quitado de '" + ctx.selected->name + "'");
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

        // Material de física del collider. Mismo begin/commit que Center/Size:
        // el snapshot se toma en IsItemActivated y el comando se empuja en
        // IsItemDeactivatedAfterEdit, así un arrastre entero = un solo undo.
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
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Box Collider) " + (m_editIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                BoxColliderState before{ m_editColliderCenter, m_editColliderSize, oldTrigger,
                                         m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };
                BoxColliderState after{ m_editColliderCenter, m_editColliderSize, m_editIsTrigger,
                                        m_editColliderStaticFriction, m_editColliderDynamicFriction, m_editColliderBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<BoxColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Box Collider)", before, after, applyBoxState));
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
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderCenter));
    if (sizeCommitted)
        ctx.pushLog("Size de '" + ctx.selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderSize));
    if (materialCommitted)
        ctx.pushLog("Material de '" + ctx.selected->name + "' (Box Collider) cambiado");

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
            "Box Collider de '" + ctx.selected->name + "'", before, after, applyBoxState));
    }

    if (removeClicked)
    {
        ctx.selected->setBoxCollider(nullptr);
        m_caches.box = nullptr;
        ctx.pushLog("Componente Box Collider quitado de '" + ctx.selected->name + "'");
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

        // Material de física del collider; mismo begin/commit que Center/Radius.
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
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Sphere Collider) " + (m_editSphereIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                SphereColliderState before{ m_editSphereCenter, m_editSphereRadius, oldTrigger,
                                            m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };
                SphereColliderState after{ m_editSphereCenter, m_editSphereRadius, m_editSphereIsTrigger,
                                           m_editSphereStaticFriction, m_editSphereDynamicFriction, m_editSphereBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<SphereColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Sphere Collider)", before, after, applySphereState));
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
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Sphere Collider) cambiado a " + formatVec3(m_editSphereCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius de '" + ctx.selected->name + "' (Sphere Collider) cambiado a " + formatFloat(m_editSphereRadius));
    if (materialCommitted)
        ctx.pushLog("Material de '" + ctx.selected->name + "' (Sphere Collider) cambiado");

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
            "Sphere Collider de '" + ctx.selected->name + "'", before, after, applySphereState));
    }

    if (removeClicked)
    {
        ctx.selected->setSphereCollider(nullptr);
        m_caches.sphere = nullptr;
        ctx.pushLog("Componente Sphere Collider quitado de '" + ctx.selected->name + "'");
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

        // Material de física del collider; mismo begin/commit que Center/Radius.
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
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Capsule Collider) " + (m_editCapsuleIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                CapsuleColliderState before{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, oldTrigger,
                                             m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };
                CapsuleColliderState after{ m_editCapsuleCenter, m_editCapsuleRadius, m_editCapsuleHeight, m_editCapsuleIsTrigger,
                                            m_editCapsuleStaticFriction, m_editCapsuleDynamicFriction, m_editCapsuleBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<CapsuleColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Capsule Collider)", before, after, applyCapsuleState));
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
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatVec3(m_editCapsuleCenter));
    if (radiusCommitted)
        ctx.pushLog("Radius de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleRadius));
    if (heightCommitted)
        ctx.pushLog("Height de '" + ctx.selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleHeight));
    if (materialCommitted)
        ctx.pushLog("Material de '" + ctx.selected->name + "' (Capsule Collider) cambiado");

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
            "Capsule Collider de '" + ctx.selected->name + "'", before, after, applyCapsuleState));
    }

    if (removeClicked)
    {
        ctx.selected->setCapsuleCollider(nullptr);
        m_caches.capsule = nullptr;
        ctx.pushLog("Componente Capsule Collider quitado de '" + ctx.selected->name + "'");
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

        // Material de física del collider; mismo begin/commit que Center.
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
            ctx.pushLog(std::string("Is Trigger de '") + ctx.selected->name +
                     "' (Plane Collider) " + (m_editPlaneIsTrigger ? "activado" : "desactivado"));
            if (ctx.scene)
            {
                PlaneColliderState before{ m_editPlaneCenter, oldTrigger,
                                           m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };
                PlaneColliderState after{ m_editPlaneCenter, m_editPlaneIsTrigger,
                                          m_editPlaneStaticFriction, m_editPlaneDynamicFriction, m_editPlaneBounciness };
                ctx.undo->push(std::make_unique<PropertyCommand<PlaneColliderState>>(
                    "Is Trigger de '" + ctx.selected->name + "' (Plane Collider)", before, after, applyPlaneState));
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
        ctx.pushLog("Center de '" + ctx.selected->name + "' (Plane Collider) cambiado a " + formatVec3(m_editPlaneCenter));
    if (materialCommitted)
        ctx.pushLog("Material de '" + ctx.selected->name + "' (Plane Collider) cambiado");

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
            "Plane Collider de '" + ctx.selected->name + "'", before, after, applyPlaneState));
    }

    if (removeClicked)
    {
        ctx.selected->setPlaneCollider(nullptr);
        m_caches.plane = nullptr;
        ctx.pushLog("Componente Plane Collider quitado de '" + ctx.selected->name + "'");
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
        m_caches.rigidbody = rb;
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
        m_caches.rigidbody = nullptr;
        ctx.pushLog("Componente Rigidbody quitado de '" + ctx.selected->name + "'");
    }

    ImGui::TreePop();
}

void PropertiesPanel::drawCameraSection(EditorContext& ctx)
{
    // Oculta hasta que se pulse Add: la sección solo existe si el componente
    // existe, y el componente solo existe tras Add (mismo early-return que
    // drawRigidbodySection).
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
        m_caches.camera = nullptr;
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
        if (m_meshFileDialog->IsOk() &&
            assetAllowed(ctx, m_meshFileDialog->GetFilePathName()))
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
        if (m_audioFileDialog->IsOk() &&
            assetAllowed(ctx, m_audioFileDialog->GetFilePathName()))
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
            m_caches.box = nullptr;
            ctx.pushLog("Componente Box Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setSphereCollider(ctx.physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getSphereCollider()->setOwner(ctx.selected);
            m_caches.sphere = nullptr;
            ctx.pushLog("Componente Sphere Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setCapsuleCollider(ctx.physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), ctx.selected->worldTransform, /*dynamic=*/false));
            ctx.selected->getCapsuleCollider()->setOwner(ctx.selected);
            m_caches.capsule = nullptr;
            ctx.pushLog("Componente Capsule Collider añadido a '" + ctx.selected->name + "'");
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && ctx.physics)
        {
            ctx.selected->setPlaneCollider(ctx.physics->createPlaneColliderComponent(
                glm::vec3(0.0f), ctx.selected->worldTransform));
            ctx.selected->getPlaneCollider()->setOwner(ctx.selected);
            m_caches.plane = nullptr;
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
                m_caches.rigidbody = nullptr;
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
            ImGui::BeginDisabled(ctx.selected->hasButton());
            if (ImGui::Selectable("Button") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ButtonComponentCommand>(
                    *ctx.scene, "Añadir Button a '" + ctx.selected->name + "'", ctx.selected->id,
                    /*add=*/true, ButtonComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Button añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasText());
            if (ImGui::Selectable("Text") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<TextComponentCommand>(
                    *ctx.scene, "Añadir Text a '" + ctx.selected->name + "'", ctx.selected->id,
                    /*add=*/true, TextComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Text añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            // Panel primero: es el fondo, y en el sync se monta debajo de todo.
            ImGui::BeginDisabled(ctx.selected->hasPanel());
            if (ImGui::Selectable("Panel") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<PanelComponentCommand>(
                    *ctx.scene, "Añadir Panel a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, PanelComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Panel añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasImage());
            if (ImGui::Selectable("Image") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ImageComponentCommand>(
                    *ctx.scene, "Añadir Image a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ImageComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Image añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasSlider());
            if (ImGui::Selectable("Slider") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<SliderComponentCommand>(
                    *ctx.scene, "Anadir Slider a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, SliderComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Slider anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasCheckbox());
            if (ImGui::Selectable("Checkbox") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<CheckboxComponentCommand>(
                    *ctx.scene, "Anadir Checkbox a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, CheckboxComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Checkbox anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasToggle());
            if (ImGui::Selectable("Toggle") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ToggleComponentCommand>(
                    *ctx.scene, "Anadir Toggle a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ToggleComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Toggle anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasScrollbar());
            if (ImGui::Selectable("Scroll Bar") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ScrollbarComponentCommand>(
                    *ctx.scene, "Anadir Scroll Bar a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ScrollbarComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Scroll Bar anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasInputField());
            if (ImGui::Selectable("Input Field") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<InputFieldComponentCommand>(
                    *ctx.scene, "Anadir Input Field a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, InputFieldComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Input Field anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasDropdown());
            if (ImGui::Selectable("Dropdown") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<DropdownComponentCommand>(
                    *ctx.scene, "Anadir Dropdown a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, DropdownComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Dropdown anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasScrollView());
            if (ImGui::Selectable("Scroll View") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ScrollViewComponentCommand>(
                    *ctx.scene, "Anadir Scroll View a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ScrollViewComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Scroll View anadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(ctx.selected->hasProgressBar());
            if (ImGui::Selectable("Progress Bar") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<ProgressBarComponentCommand>(
                    *ctx.scene, "Añadir Progress Bar a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, ProgressBarComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Progress Bar añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();

            // Layout: el único de los cuatro que no dibuja nada. Sin otro
            // componente de UI en el objeto monta su propio contenedor, así que
            // también vale en un GameObject pelado que solo agrupe.
            ImGui::BeginDisabled(ctx.selected->hasLayout());
            if (ImGui::Selectable("Layout") && ctx.scene && ctx.undo)
            {
                auto cmd = std::make_unique<LayoutComponentCommand>(
                    *ctx.scene, "Añadir Layout a '" + ctx.selected->name + "'",
                    ctx.selected->id, /*add=*/true, LayoutComponent{});
                cmd->execute();
                ctx.undo->push(std::move(cmd));
                ctx.pushLog("Componente Layout añadido a '" + ctx.selected->name + "'");
            }
            ImGui::EndDisabled();
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
            m_caches.camera = nullptr;
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
