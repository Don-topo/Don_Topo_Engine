#include "DonTopo/Editor/ViewportPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Camera.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Gizmos.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Renderer/Renderer.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace DonTopo {

float ViewportPanel::selectionAxisScale(GameObject* node) const
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

void ViewportPanel::focusSelected(EditorContext& ctx, Camera& camera)
{
    if (!ctx.selected)
        return;

    constexpr float kFallbackRadius = 50.0f;

    glm::vec3 center = glm::vec3(ctx.selected->worldTransform[3]);
    float     radius = kFallbackRadius;

    if (ctx.selected->hasMesh())
    {
        const auto& vertices = ctx.selected->getMesh()->vertices;
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
                glm::length(glm::vec3(ctx.selected->worldTransform[0])),
                glm::length(glm::vec3(ctx.selected->worldTransform[1])),
                glm::length(glm::vec3(ctx.selected->worldTransform[2])));
            float maxWorldScale = glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z));

            radius = glm::max(maxHalf, 1.0f) * maxWorldScale;
        }
    }

    camera.focusOn(center, radius);
}

void ViewportPanel::drawSelectionGizmo(EditorContext& ctx)
{
    if (!ctx.selected)
        return;
    Gizmos::drawAxes(ctx.selected->worldTransform, selectionAxisScale(ctx.selected));

    const glm::vec3 kColliderColor(1.0f, 1.0f, 0.0f);
    if (ctx.selected->hasBoxCollider())
    {
        BoxCollider* bc = ctx.selected->getBoxCollider().get();
        Gizmos::drawWireBox(ctx.selected->worldTransform, bc->getCenter(),
                             bc->getHalfExtents(), kColliderColor);
    }
    else if (ctx.selected->hasSphereCollider())
    {
        SphereCollider* sc = ctx.selected->getSphereCollider().get();
        Gizmos::drawWireSphere(ctx.selected->worldTransform, sc->getCenter(),
                                sc->getRadius(), kColliderColor);
    }
    else if (ctx.selected->hasCapsuleCollider())
    {
        CapsuleCollider* cc = ctx.selected->getCapsuleCollider().get();
        Gizmos::drawWireCapsule(ctx.selected->worldTransform, cc->getCenter(),
                                 cc->getRadius(), cc->getHalfHeight(), kColliderColor);
    }
    else if (ctx.selected->hasPlaneCollider())
    {
        PlaneCollider* pc = ctx.selected->getPlaneCollider().get();
        Gizmos::drawWirePlane(ctx.selected->worldTransform, pc->getCenter(), kColliderColor);
    }
}

void ViewportPanel::drawCameraGizmo(EditorContext& ctx)
{
    // Solo en edición: en Play ya se está mirando POR esa cámara, dibujar su
    // propio frustum no aporta nada (y taparía la vista desde dentro).
    if (ctx.isPlaying || !ctx.scene || !ctx.renderer)
        return;

    GameObject* cam = ctx.scene->findCamera();
    if (!cam) return;

    // El aspect sale del Renderer (el del render target), no del tamaño de esta
    // ventana ImGui: tiene que ser EXACTAMENTE el que usará la proyección al
    // dar a Play, o el wireframe dibujaría un encuadre que luego no se cumple.
    const glm::mat4 viewProj =
        cam->getCameraComponent()->projectionMatrix(ctx.renderer->viewportAspect()) *
        CameraComponent::viewFromWorld(cam->worldTransform);

    // Cian: distinto del amarillo de los colliders, pa no confundirlos.
    const glm::vec3 kCameraGizmoColor(0.0f, 1.0f, 1.0f);
    // true: esta viewProj sale de CameraComponent::projectionMatrix, que usa
    // *_ZO (near->z_ndc=0) pa Vulkan, no la convención NO por defecto de glm.
    Gizmos::drawFrustum(viewProj, kCameraGizmoColor, /*depthZeroToOne=*/true);
}

void ViewportPanel::drawLightGizmos(EditorContext& ctx)
{
    // El flag del menú View manda: cada Gizmos::drawX ya lo mira por dentro,
    // pero comprobarlo aquí se ahorra el recorrido entero de la escena.
    if (!ctx.scene || !Gizmos::isEnabled())
        return;

    // Naranja: ni el amarillo de los colliders ni el cian de la cámara.
    const glm::vec3 kLightColor(1.0f, 0.8f, 0.2f);

    ctx.scene->traverse([&](GameObject* go) {
        if (!go->hasLight()) return;
        const LightComponent& lc = *go->getLight();

        // Misma base que usa Scene::collectLights pa mandar la luz al shader:
        // posición en la columna 3 y -Z local como dirección. Los ejes van
        // NORMALIZADOS — el gizmo mide en unidades de mundo, así que la escala
        // del GameObject no debe estirarlo (al revés que los colliders, que sí
        // escalan con el objeto).
        const glm::vec3 pos   = glm::vec3(go->worldTransform[3]);
        glm::vec3 right = glm::vec3(go->worldTransform[0]);
        glm::vec3 up    = glm::vec3(go->worldTransform[1]);
        glm::vec3 fwd   = -glm::vec3(go->worldTransform[2]);
        // Base degenerada (una escala a 0 desde Properties): normalize daría
        // NaN y el vertex buffer del gizmo se llenaría de basura.
        if (glm::length(right) < 1e-6f || glm::length(up) < 1e-6f || glm::length(fwd) < 1e-6f)
        {
            right = glm::vec3(1.0f, 0.0f, 0.0f);
            up    = glm::vec3(0.0f, 0.0f, 1.0f);
            fwd   = glm::vec3(0.0f, -1.0f, 0.0f);
        }
        right = glm::normalize(right);
        up    = glm::normalize(up);
        fwd   = glm::normalize(fwd);

        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(right, 0.0f);
        basis[1] = glm::vec4(up,    0.0f);
        basis[2] = glm::vec4(-fwd,  0.0f);
        basis[3] = glm::vec4(pos,   1.0f);

        switch (lc.getType())
        {
            case LightType::Point:
                Gizmos::drawWireSphere(basis, glm::vec3(0.0f), lc.getRange(), kLightColor);
                break;

            case LightType::Spot:
            {
                Gizmos::drawWireSphere(basis, glm::vec3(0.0f), lc.getRange(), kLightColor);
                // Cuatro generatrices del cono exterior (arriba/abajo/izquierda/
                // derecha): con el ángulo del borde, que es donde el spot se
                // apaga del todo.
                const float a = glm::radians(lc.getOuterAngle());
                const float c = std::cos(a);
                const float s = std::sin(a);
                const glm::vec3 dirs[4] = {
                    fwd * c + right * s, fwd * c - right * s,
                    fwd * c + up    * s, fwd * c - up    * s,
                };
                for (const glm::vec3& d : dirs)
                    Gizmos::drawRay(pos, d, lc.getRange(), kLightColor);
                break;
            }

            case LightType::Directional:
                // No tiene alcance: la longitud es solo pa verla, no significa
                // hasta dónde llega (llega a todas partes).
                Gizmos::drawRay(pos, fwd, 500.0f, kLightColor);
                break;

            case LightType::Area:
            {
                // drawWirePlane dibuja una rejilla de 10x10 unidades en el plano
                // XZ de la matriz que se le pase, así que la base va montada pa
                // que ese plano sea el del rectángulo (su normal es fwd) y
                // escalada a ancho x alto.
                glm::mat4 rect(1.0f);
                rect[0] = glm::vec4(right * (lc.getAreaWidth()  / 10.0f), 0.0f);
                rect[1] = glm::vec4(fwd,                                  0.0f);
                rect[2] = glm::vec4(up    * (lc.getAreaHeight() / 10.0f), 0.0f);
                rect[3] = glm::vec4(pos,                                  1.0f);
                Gizmos::drawWirePlane(rect, glm::vec3(0.0f), kLightColor);
                Gizmos::drawRay(pos, fwd, lc.getAreaWidth() * 0.5f, kLightColor);
                break;
            }
        }
    });
}

void ViewportPanel::draw(EditorContext& ctx, VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    // Contorno del objeto seleccionado. Se fija SIEMPRE y sin condiciones, aquí
    // arriba: si se hiciera solo cuando hay selección, el Renderer se quedaría
    // con el índice del objeto anterior al deseleccionar y lo seguiría
    // resaltando. Un objeto sin malla no tiene índice de render (-1 en los dos
    // campos), así que tampoco dibuja nada.
    if (ctx.renderer)
    {
        ctx.renderer->setOutlineTarget(
            ctx.selected ? ctx.selected->staticRenderIndex  : -1,
            ctx.selected ? ctx.selected->skinnedRenderIndex : -1);
    }

    // Veto de edición mientras el modal de carga está activo: el gizmo de
    // manipulación (ImGuizmo) mueve/rota/escala el objeto seleccionado, así que
    // se salta. drawCameraGizmo es solo debug-draw (no edita), se deja siempre.
    if (!ctx.editingLocked)
        drawSelectionGizmo(ctx);
    drawCameraGizmo(ctx);
    // También en Play: la luz sigue siendo un objeto de escena que hay que
    // poder situar mientras corre el juego.
    drawLightGizmos(ctx);

    if (!m_open)
    {
        // Sin esto, cerrar Viewport dejaría m_hovered en su último
        // valor (posiblemente true) y el mouse-look de la cámara en
        // sandbox/src/main.cpp seguiría respondiendo con el panel oculto.
        m_hovered = false;
        return;
    }
    ImGui::Begin("Viewport", &m_open);
    m_hovered = ImGui::IsWindowHovered();
    ImVec2 vpPos  = ImGui::GetCursorScreenPos();
    ImVec2 vpSize = ImGui::GetContentRegionAvail();
    // Se publica para que el Renderer renderice a ESTE tamaño exacto: así la
    // imagen se dibuja 1:1 y no pasa por el reescalado de ImGui.
    m_contentWidth  = (uint32_t)(vpSize.x > 0.0f ? vpSize.x : 0.0f);
    m_contentHeight = (uint32_t)(vpSize.y > 0.0f ? vpSize.y : 0.0f);
    ImGui::Image((ImTextureID)(intptr_t)viewportTexture, vpSize);

    // Axis gizmo estilo Unity/Godot (esquina superior derecha): ejes mundo
    // proyectados por la rotación real de la cámara (parte 3x3 de la view
    // matrix), así que gira con ella. Clicar una bola reorienta la cámara
    // pa mirar a lo largo de ese eje (via ctx.onAxisSelected).
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
    bool clicked = m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

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

        if (clicked && ctx.onAxisSelected)
        {
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy <= ballRadius * ballRadius)
                ctx.onAxisSelected(axes[i].world);
        }
    }
    drawList->AddCircleFilled(center, 3.0f, IM_COL32(200, 200, 200, 255));

    ImGui::End();
}

} // namespace DonTopo
