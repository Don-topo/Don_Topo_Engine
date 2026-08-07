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
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace DonTopo {

namespace {

// Bbox LOCAL del mesh de go, el mismo cálculo por vértices que ya usan
// selectionAxisScale/focusSelected. false si go no tiene malla o la malla está
// vacía: esos objetos no entran en el picking.
//
// Un SkinnedMesh deja Mesh::vertices VACÍO —su geometría vive en
// skinnedVertices—, así que hay que mirar ahí o los personajes animados no se
// podrían picar. Es la pose de bind: la animación la aplica el compute de
// skinning en GPU y aquí no hay pose evaluada, igual que el culling de skinned
// del Renderer, que también usa un bound de bind pose.
bool localBounds(GameObject* go, glm::vec3& bMin, glm::vec3& bMax)
{
    if (!go->hasMesh())
        return false;

    if (const SkinnedMesh* skinned = go->getSkinnedMesh())
    {
        const auto& sv = skinned->skinnedVertices;
        if (sv.empty())
            return false;

        bMin = glm::vec3(sv[0].position);
        bMax = glm::vec3(sv[0].position);
        for (const auto& v : sv)
        {
            bMin = glm::min(bMin, glm::vec3(v.position));
            bMax = glm::max(bMax, glm::vec3(v.position));
        }
        return true;
    }

    const auto& vertices = go->getMesh()->vertices;
    if (vertices.empty())
        return false;

    bMin = vertices[0].pos;
    bMax = vertices[0].pos;
    for (const auto& v : vertices)
    {
        bMin = glm::min(bMin, v.pos);
        bMax = glm::max(bMax, v.pos);
    }
    return true;
}

// Esfera envolvente en mundo a partir del bbox local, escalada por la escala
// máxima del worldTransform (el radio no puede depender del eje, así que manda
// la mayor). Es SOLO el descarte rápido del picking: para una malla plana y
// enorme —el suelo— la esfera es gigante y se traga a la cámara, así que quien
// decide el impacto es rayAabbLocal, no esta.
void worldBoundingSphere(GameObject* go, const glm::vec3& bMin, const glm::vec3& bMax,
                         glm::vec3& center, float& radius)
{
    const glm::vec3 localCenter = (bMin + bMax) * 0.5f;
    const float     localRadius = glm::length(bMax - localCenter);

    const glm::vec3 worldScale(
        glm::length(glm::vec3(go->worldTransform[0])),
        glm::length(glm::vec3(go->worldTransform[1])),
        glm::length(glm::vec3(go->worldTransform[2])));
    const float maxWorldScale = glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z));

    center = glm::vec3(go->worldTransform * glm::vec4(localCenter, 1.0f));
    radius = localRadius * maxWorldScale;
}

// Corte rayo/AABB en el espacio LOCAL del objeto (el rayo se lleva allí con la
// inversa del worldTransform), que en mundo es la caja orientada del objeto.
// Devuelve el punto de entrada en MUNDO: con escalas distintas por eje el t
// local no es distancia, así que la comparación entre objetos se hace fuera,
// con la distancia real a la cámara. La cara de entrada se ignora si la cámara
// está dentro (t < 0 en el eje de entrada): entonces el impacto es el origen.
bool rayAabbLocal(const glm::mat4& world, const glm::vec3& bMin, const glm::vec3& bMax,
                  const glm::vec3& origin, const glm::vec3& dir, glm::vec3& hitWorld)
{
    const glm::mat4 inv = glm::inverse(world);
    const glm::vec3 o   = glm::vec3(inv * glm::vec4(origin, 1.0f));
    const glm::vec3 d   = glm::vec3(inv * glm::vec4(dir, 0.0f));

    float tEnter = -std::numeric_limits<float>::infinity();
    float tExit  =  std::numeric_limits<float>::infinity();

    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(d[i]) < 1e-8f)
        {
            // Rayo paralelo a este par de planos: o está dentro de la franja o
            // no corta nunca.
            if (o[i] < bMin[i] || o[i] > bMax[i])
                return false;
            continue;
        }
        float t1 = (bMin[i] - o[i]) / d[i];
        float t2 = (bMax[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tEnter = glm::max(tEnter, t1);
        tExit  = glm::min(tExit,  t2);
        if (tEnter > tExit)
            return false;
    }

    if (tExit < 0.0f)
        return false; // la caja entera queda detrás de la cámara

    const float t = tEnter >= 0.0f ? tEnter : 0.0f; // cámara dentro de la caja
    hitWorld = glm::vec3(world * glm::vec4(o + d * t, 1.0f));
    return true;
}

// Corte rayo/esfera. dir NORMALIZADA, así que t sale en unidades de mundo y se
// puede comparar entre objetos. Con la cámara dentro de la esfera devuelve
// t = 0 (impacto en el propio origen): el objeto que envuelve a la cámara es
// el más cercano posible, no uno detrás de ella.
bool raySphere(const glm::vec3& origin, const glm::vec3& dir,
               const glm::vec3& center, float radius, float& t)
{
    const glm::vec3 oc = origin - center;
    const float b = glm::dot(oc, dir);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f)
        return false;

    const float s  = std::sqrt(disc);
    const float t0 = -b - s;
    const float t1 = -b + s;
    if (t0 >= 0.0f) { t = t0;   return true; }
    if (t1 >= 0.0f) { t = 0.0f; return true; }
    return false; // la esfera entera queda detrás de la cámara
}

} // namespace

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

    // Atenuación del AudioClip 3D: esfera interior (min, volumen pleno) y
    // exterior (max, silencio). Solo del objeto seleccionado y solo si el clip
    // es 3D — en 2D FMOD no atenúa por distancia y las esferas mentirían.
    if (ctx.selected->hasAudioClip() && ctx.selected->getAudioClip()->getIs3D())
    {
        // Magenta: ni el amarillo de los colliders, ni el cian de la cámara, ni
        // el naranja de las luces.
        const glm::vec3 kAudioColor(1.0f, 0.2f, 0.8f);

        // Base con los ejes NORMALIZADOS: las distancias de FMOD son unidades de
        // mundo, así que la escala del GameObject no puede estirar las esferas
        // (al revés que los colliders, que sí escalan con el objeto). Base
        // degenerada (una escala a 0 desde Properties) -> identidad, pa no
        // meter NaN en el vertex buffer del gizmo.
        glm::mat4 basis(1.0f);
        const glm::vec3 axes[3] = { glm::vec3(ctx.selected->worldTransform[0]),
                                    glm::vec3(ctx.selected->worldTransform[1]),
                                    glm::vec3(ctx.selected->worldTransform[2]) };
        if (glm::length(axes[0]) >= 1e-6f && glm::length(axes[1]) >= 1e-6f &&
            glm::length(axes[2]) >= 1e-6f)
        {
            basis[0] = glm::vec4(glm::normalize(axes[0]), 0.0f);
            basis[1] = glm::vec4(glm::normalize(axes[1]), 0.0f);
            basis[2] = glm::vec4(glm::normalize(axes[2]), 0.0f);
        }
        basis[3] = ctx.selected->worldTransform[3];

        const AudioClipComponent& clip = *ctx.selected->getAudioClip();
        Gizmos::drawWireSphere(basis, glm::vec3(0.0f), clip.getMinDistance(), kAudioColor);
        Gizmos::drawWireSphere(basis, glm::vec3(0.0f), clip.getMaxDistance(), kAudioColor);
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

void ViewportPanel::drawCanvasGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCanvas() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // Lo que dejó el último buildDrawData del canvas vivo: origen en píxeles
    // del render y tamaño del área útil = referencia * escala. Aquí no se
    // vuelve a resolver nada.
    const UiCanvas& canvas = ctx.renderer->uiCanvas();
    const glm::vec2 origin = canvas.uiOrigin();
    const glm::vec2 size   = canvas.referenceSize() * canvas.uiScale();
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;

    // El render va 1:1 con el panel (el propio panel dicta su tamaño), pero si
    // alguna vez no coincidiera, el factor lo corrige en vez de mentir.
    const glm::vec2 renderSize{ (float)ctx.renderer->renderWidth(),
                                (float)ctx.renderer->renderHeight() };
    const glm::vec2 k = (renderSize.x > 0.0f && renderSize.y > 0.0f)
                        ? imageSize / renderSize : glm::vec2(1.0f);

    const ImVec2 p0{ imagePos.x + origin.x * k.x, imagePos.y + origin.y * k.y };
    const ImVec2 p1{ p0.x + size.x * k.x, p0.y + size.y * k.y };
    ImGui::GetWindowDrawList()->AddRect(p0, p1, IM_COL32(80, 200, 255, 220), 0.0f, 0, 2.0f);
}

GameObject* ViewportPanel::pickObject(EditorContext& ctx, const glm::mat4& cameraView,
                                      const glm::vec2& mousePx, const glm::vec2& imageSize) const
{
    if (!ctx.scene || imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return nullptr;

    // Aspect del render target, el mismo que usa el Renderer para armar la
    // proyección del frame (y que ya usa drawCameraGizmo); si el panel es lo
    // que dicta ese tamaño, coincide con imageSize.
    const float aspect = ctx.renderer ? ctx.renderer->viewportAspect()
                                      : imageSize.x / imageSize.y;

    // Cámara del frame, igual que Renderer::currentFrameCamera: en Play manda
    // el CameraComponent de la escena (su projectionMatrix ya trae el Y-flip de
    // Vulkan y z=[0,1]); en edición, la de vuelo del editor, cuya proyección es
    // 45° fijos + Y-flip. El near/far del editor sale de un estado privado del
    // Renderer, pero la DIRECCIÓN del rayo que pasa por un píxel no depende de
    // los planos, solo de fov/aspect/Y-flip: por eso aquí valen unos genéricos.
    glm::mat4 view = cameraView;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan Y flip, igual que el Renderer
    if (ctx.isPlaying)
    {
        if (GameObject* cam = ctx.scene->findCamera())
        {
            view = CameraComponent::viewFromWorld(cam->worldTransform);
            proj = cam->getCameraComponent()->projectionMatrix(aspect);
        }
    }

    // NDC del ratón dentro de la IMAGEN. Con el Y-flip dentro de la proyección,
    // y = -1 es el borde SUPERIOR de la imagen, que es justo el sentido en el
    // que crece el píxel del ratón: nada que invertir aquí.
    const float ndcX = (mousePx.x / imageSize.x) * 2.0f - 1.0f;
    const float ndcY = (mousePx.y / imageSize.y) * 2.0f - 1.0f;

    const glm::mat4 invViewProj = glm::inverse(proj * view);
    // z=1 es el plano lejano en las dos convenciones de profundidad (ZO de
    // CameraComponent y [-1,1] de la proyección del editor), así que este punto
    // vale para ambas sin reconstruir nada a mano.
    const glm::vec4 farH = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::fabs(farH.w) < 1e-9f)
        return nullptr;

    // El origen es la posición real de la cámara (inversa de la view), no el
    // punto del plano cercano: así t es distancia a la cámara y comparar t
    // entre objetos ordena de verdad por cercanía.
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 origin  = glm::vec3(invView[3]);
    const glm::vec3 target  = glm::vec3(farH) / farH.w;
    const glm::vec3 delta   = target - origin;
    if (glm::length(delta) < 1e-6f)
        return nullptr;
    const glm::vec3 dir = glm::normalize(delta);

    GameObject* best     = nullptr;
    float       bestDist = 0.0f;
    // Pre-orden: GameObject::traverse visita el nodo y luego sus hijos. Entre
    // dos impactos gana el más cercano a la cámara; el empate lo rompe el
    // primero visitado.
    ctx.scene->traverse([&](GameObject* go) {
        glm::vec3 bMin, bMax;
        if (!localBounds(go, bMin, bMax))
            return;

        // Descarte rápido por esfera; el impacto de verdad lo da la caja.
        glm::vec3 center;
        float     radius = 0.0f;
        float     tSphere = 0.0f;
        worldBoundingSphere(go, bMin, bMax, center, radius);
        if (!raySphere(origin, dir, center, radius, tSphere))
            return;

        glm::vec3 hit;
        if (!rayAabbLocal(go->worldTransform, bMin, bMax, origin, dir, hit))
            return;

        const float dist = glm::length(hit - origin);
        if (!best || dist < bestDist)
        {
            best     = go;
            bestDist = dist;
        }
    });

    return best;
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
    // Área útil del Canvas seleccionado, justo sobre la imagen: es 2D, así que
    // va con el draw list de ImGui y no con Gizmos (que dibuja en el mundo).
    drawCanvasGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    // Hover de la IMAGEN, no de la ventana: con esto un popup o cualquier otra
    // ventana por encima ya no cuenta como clic en la escena.
    const bool imageHovered = ImGui::IsItemHovered();

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
    bool axisBallClicked = false;

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

        if (clicked)
        {
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy <= ballRadius * ballRadius)
            {
                // Marca el clic como consumido por el gizmo de ejes: reorientar
                // la cámara no debe además cambiar la selección.
                axisBallClicked = true;
                if (ctx.onAxisSelected)
                    ctx.onAxisSelected(axes[i].world);
            }
        }
    }
    drawList->AddCircleFilled(center, 3.0f, IM_COL32(200, 200, 200, 255));

    // Selección por clic en la escena. Puertas, en este orden: el clic cae
    // sobre la imagen (no sobre otra ventana ni sobre el gizmo de ejes), ningún
    // widget de ImGui está activo (arrastre de slider, drag&drop...), el gizmo
    // de manipulación no está encima ni en uso, y no hay modal de carga. Sin
    // impacto, la selección se va a nullptr, igual que el clic en zona vacía
    // del panel Scene.
    const bool gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (clicked && imageHovered && !axisBallClicked && !gizmoBusy &&
        !ImGui::IsAnyItemActive() && !ctx.editingLocked)
    {
        const glm::vec2 mousePx(mouse.x - vpPos.x, mouse.y - vpPos.y);
        ctx.selected = pickObject(ctx, cameraView, mousePx, glm::vec2(vpSize.x, vpSize.y));
    }

    ImGui::End();
}

} // namespace DonTopo
