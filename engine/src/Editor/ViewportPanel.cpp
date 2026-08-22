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
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/CanvasComponent.h"   // uiWorldCanvasMatrix, UiCanvasRenderMode
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

// El Canvas al que pertenece `go`: el ancestro MÁS CERCANO que tenga uno, y el
// propio `go` cuenta. Es EXACTAMENTE la regla de Scene::collectCanvases (un
// canvas anidado abre binding propio y CORTA la cadena de anclaje), así que el
// gizmo mira el mismo canvas al que el sync manda el widget. nullptr si no
// cuelga de ninguno: el editor lo impide (uiComponentsAvailable), pero una
// escena hecha a mano puede traerlo.
//
// No es static a propósito, por lo mismo que projectWorldCanvasCorners:
// dt_camera_tests la declara a mano para poder probar la regla sin GUI.
const CanvasComponent* owningCanvasOf(const GameObject* go)
{
    for (const GameObject* n = go; n != nullptr; n = n->parent)
        if (n->hasCanvas())
            return n->getCanvas().get();
    return nullptr;
}

// Proyecta las cuatro esquinas de un canvas de MUNDO a píxeles de la imagen del
// viewport. `mvp` es proj·view·model —la misma cadena que graba el backend— y
// las esquinas se dan en píxeles de CANVAS, que es el espacio del que parte
// uiWorldCanvasMatrix. Salen en el orden (0,0), (w,0), (w,h), (0,h); el (0,0)
// del canvas es su esquina SUPERIOR izquierda, porque la Y del canvas crece
// hacia abajo y uiWorldCanvasMatrix la niega.
//
// Devuelve false —y deja `outCorners` SIN TOCAR— si alguna esquina tiene w <= 0:
// está detrás del plano de la cámara, y dividir por ese w la espeja al otro
// lado. El cuadrilátero saldría cruzado o disparado al infinito y nada lo
// diría, porque aquí no hay clipping de GPU que valga: es ImGui pintando las
// líneas que le den. Por eso el rechazo es EXPLÍCITO. La comparación va como
// !(w > 0) para que un NaN —matriz degenerada— caiga también del lado del
// rechazo.
//
// Fuera del encuadre pero DELANTE sí se acepta: el criterio es el signo de w, no
// que el rect quepa en la imagen. Recortar aquí dejaría sin gizmo justo al
// canvas que asoma medio por el borde, que es cuando más se busca.
//
// No es static a propósito: es la única parte del gizmo que se puede probar sin
// ventana, y dt_camera_tests la declara a mano para enlazarla (el editor no
// tiene header público donde ponerla).
bool projectWorldCanvasCorners(const glm::mat4& mvp, const glm::vec2& canvasSize,
                               const glm::vec2& imagePos, const glm::vec2& imageSize,
                               glm::vec2 outCorners[4])
{
    const glm::vec2 esquinas[4] = {
        glm::vec2(0.0f,         0.0f),
        glm::vec2(canvasSize.x, 0.0f),
        glm::vec2(canvasSize.x, canvasSize.y),
        glm::vec2(0.0f,         canvasSize.y),
    };

    // A un intermedio y no directo a outCorners: si una esquina rechaza cuando
    // ya se han calculado otras, el caller no puede quedarse con una mezcla de
    // esquinas nuevas y viejas. Lo prueba
    // test_world_canvas_gizmo_rechaza_esquina_detras_de_la_camara, cuyo fixture
    // está montado a propósito para que la que rechace NO sea la primera.
    glm::vec2 px[4];
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec4 clip = mvp * glm::vec4(esquinas[i].x, esquinas[i].y, 0.0f, 1.0f);
        if (!(clip.w > 0.0f))
            return false;

        // NDC -> píxel de la imagen. La Y va SIN invertir porque la proyección
        // que se le pasa trae el Y-flip de Vulkan cocinado (ndc.y = -1 arriba),
        // el mismo criterio que pickObject. En D3D12 la proyección del backend
        // NO lleva ese flip y su NDC tiene +1 arriba: las dos inversiones se
        // cancelan y el píxel sale idéntico. Es exactamente por lo que
        // pickObject acierta en los dos backends con una sola fórmula.
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        px[i] = imagePos + glm::vec2((ndc.x * 0.5f + 0.5f) * imageSize.x,
                                     (ndc.y * 0.5f + 0.5f) * imageSize.y);
    }

    for (int i = 0; i < 4; ++i)
        outCorners[i] = px[i];
    return true;
}

// Gizmo del canvas en modo MUNDO: el cuadrilátero de su plano, proyectado, que
// es lo único que enseña dónde está y con qué inclinación. El rect 2D del área
// útil que pinta drawCanvasGizmo no significa nada aquí.
//
// Función libre y no método porque necesita la `view` de la cámara —para el
// billboard y para la proyección— y quien la tiene es draw(); meterla por el
// parámetro de drawCanvasGizmo obligaría a tocar el header.
static void drawWorldCanvasGizmo(EditorContext& ctx, const glm::mat4& cameraView,
                                 const glm::vec2& imagePos, const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCanvas() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    const CanvasComponent& c = *ctx.selected->getCanvas();
    if (c.renderMode != UiCanvasRenderMode::World)
        return;

    // En modo World el área útil es EXACTAMENTE referenceResolution: applyTo no
    // deja que scaleMode, el safe area ni el aspect ratio la toquen.
    const glm::vec2 tam = c.referenceResolution;
    if (tam.x <= 0.0f || tam.y <= 0.0f)
        return;

    // Cámara del frame, la misma receta que pickObject: en edición 45° fijos +
    // el Y-flip de Vulkan; en Play manda el CameraComponent de la escena. El
    // near/far NO entra en la cuenta —en una perspectiva solo toca la Z, x/y/w
    // salen de fov y aspect—, así que los genéricos de aquí valen aunque el
    // Renderer use m_cameraDistance.
    //
    // LIMITACIÓN CONOCIDA: el grabado usa la proyección JITTEREADA
    // (taaJitteredProj en los dos backends) y aquí no hay forma de pedirla sin
    // una virtual nueva en EditorRenderer. Con TAA encendido el gizmo puede
    // quedar hasta medio píxel del cartel; sin TAA, taaJitteredProj es la
    // proyección a secas y coinciden.
    const float aspect = ctx.renderer->viewportAspect();
    glm::mat4 view = cameraView;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan Y flip, igual que el Renderer
    if (ctx.isPlaying && ctx.scene)
    {
        if (GameObject* cam = ctx.scene->findCamera())
        {
            view = CameraComponent::viewFromWorld(cam->worldTransform);
            proj = cam->getCameraComponent()->projectionMatrix(aspect);
        }
    }

    // La MISMA matriz de modelo que el grabado, y con la MISMA `view`: el
    // billboard sale de ella, así que darle otra separaría el gizmo del cartel
    // en cuanto girase la cámara.
    const glm::mat4 mvp =
        proj * view * uiWorldCanvasMatrix(c, tam, ctx.selected->worldTransform, view);

    glm::vec2 esq[4];
    if (!projectWorldCanvasCorners(mvp, tam, imagePos, imageSize, esq))
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = IM_COL32(80, 200, 255, 220);   // el mismo azul del gizmo 2D
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec2& a = esq[i];
        const glm::vec2& b = esq[(i + 1) % 4];
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), color, 2.0f);
    }
    // Marca en la esquina (0,0) del canvas. Sin ella, un canvas visto POR
    // DETRÁS pinta el mismo cuadrilátero y no hay manera de saber que está del
    // revés.
    dl->AddCircleFilled(ImVec2(esq[0].x, esq[0].y), 4.0f, color);
}

void ViewportPanel::drawCanvasGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCanvas() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // Un canvas de MUNDO no tiene área útil en píxeles de pantalla que enseñar:
    // de él se encarga drawWorldCanvasGizmo, que lo proyecta. Dibujar además el
    // rect 2D pintaría un recuadro en un sitio que no tiene nada que ver.
    if (ctx.selected->getCanvas()->renderMode == UiCanvasRenderMode::World)
        return;

    // Lo que dejó el último buildDrawData del canvas vivo: origen en píxeles
    // del render y tamaño del área útil = referencia * escala. Aquí no se
    // vuelve a resolver nada.
    const UiCanvas& canvas = ctx.renderer->uiCanvas();
    const glm::vec2 origin = canvas.uiOrigin();
    const glm::vec2 size   = canvas.referenceSize() * canvas.uiScale();
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;

    // El canvas se resuelve en píxeles de SALIDA, y la salida es exactamente
    // esta imagen (el panel dicta su tamaño), así que van 1:1. Antes se dividía
    // por renderWidth/renderHeight —el render INTERNO—, y con SSAA eso movía el
    // recuadro a la mitad o al doble del área real.
    const ImVec2 p0{ imagePos.x + origin.x, imagePos.y + origin.y };
    const ImVec2 p1{ p0.x + size.x, p0.y + size.y };
    ImGui::GetWindowDrawList()->AddRect(p0, p1, IM_COL32(80, 200, 255, 220), 0.0f, 0, 2.0f);
}

// SIN USO desde que los trece gizmos de widget de abajo pasaron por
// EditorRenderer::findUiNode, que recorre TODOS los canvas y no solo el de
// pantalla (este envoltorio solo miraba el que le dieran, así que un widget
// dentro de un canvas de MUNDO se quedaba sin gizmo en silencio). Se deja
// porque borrar símbolos no es de esta tarea; findUiNodeIn, lo que envuelve,
// sigue vivo en UiCanvas.h y lo usa el Renderer.
static const UiElement* findUiNodeNamed(const UiCanvas& canvas, const std::string& wanted)
{
    return findUiNodeIn(canvas.root(), wanted);
}

// Rect + ejes del nodo vivo que se le pase. Compartido por el gizmo del Button y
// el del Text: lo único que cambia entre los dos es de qué nodo salen.
static void drawUiNodeGizmo(EditorContext& ctx, const UiElement* node,
                            const glm::vec2& imagePos, const glm::vec2& imageSize)
{
    if (!node || !node->rectValid) return;

    // LIMITACIÓN CONOCIDA: en un canvas de MUNDO este rect NO se puede dibujar
    // aquí, y dibujarlo de todas formas sería peor que no dibujar nada.
    //
    // screenPos/screenSize los deja buildDrawData en el espacio en el que se
    // construyó el canvas, y un canvas de mundo se construye a su
    // referenceResolution (Renderer.cpp:1654, D3D12Renderer.cpp:6261) con
    // applyTo forzando ConstantPixelSize y scaleFactor 1 — o sea uiScale = 1 y
    // uiOrigin = (0,0). Son píxeles LOCALES DEL CANVAS, no de pantalla. Sumarlos
    // a imagePos, como hace el código de abajo, pintaría el rect pegado a la
    // esquina superior izquierda del viewport y COMPLETAMENTE QUIETO mientras la
    // cámara vuela, aunque el cartel esté al otro lado de la escena o detrás de
    // ella. Un gizmo que miente es peor que ninguno.
    //
    // Lo que falta para dibujarlo bien es la `view` de la cámara —la necesitan
    // la proyección Y el billboard— y aquí no llega: los trece drawXGizmo están
    // declarados en ViewportPanel.h y solo draw() la tiene. Documentado en
    // Scripts/README.md junto a las otras dos limitaciones del modo mundo.
    //
    // Ojo: esto NO deja sin efecto el paso a EditorRenderer::findUiNode. Ese
    // cambio arregla además el caso de un SEGUNDO canvas de PANTALLA, donde el
    // rect sí va en píxeles de salida y el gizmo sale bien: uiCanvas() devuelve
    // solo el PRIMERO (Renderer.cpp:743), así que sus widgets tampoco tenían
    // gizmo antes.
    if (const CanvasComponent* canvas = owningCanvasOf(ctx.selected))
        if (canvas->renderMode == UiCanvasRenderMode::World)
            return;

    // screenPos/screenSize los deja buildDrawData en píxeles de SALIDA, y la
    // salida es esta misma imagen: van 1:1. Antes se escalaba por
    // imagen/renderWidth (el render INTERNO) y con SSAA el recuadro salía a
    // mitad de tamaño y en mitad de posición. Mismo criterio que
    // drawCanvasAreaGizmo y que pickUiObject.
    const ImVec2 p0{ imagePos.x + node->screenPos.x,
                     imagePos.y + node->screenPos.y };
    const ImVec2 p1{ p0.x + node->screenSize.x,
                     p0.y + node->screenSize.y };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(p0, p1, IM_COL32(255, 160, 40, 230), 0.0f, 0, 2.0f);

    // Ejes desde el PIVOT, que es el punto respecto al que ancla y rota: X a la
    // derecha y Y hacia ABAJO, que es el sentido de +Y en la UI (no el del
    // mundo 3D). Solo dos ejes: un rect no tiene Z.
    const ImVec2 pivot{ p0.x + node->pivot.x * (p1.x - p0.x),
                        p0.y + node->pivot.y * (p1.y - p0.y) };
    const float len = 34.0f;
    dl->AddLine(pivot, ImVec2(pivot.x + len, pivot.y), IM_COL32(220, 60, 60, 255), 2.0f);
    dl->AddLine(pivot, ImVec2(pivot.x, pivot.y + len), IM_COL32(70, 200, 70, 255), 2.0f);
    dl->AddText(ImVec2(pivot.x + len + 2.0f, pivot.y - 7.0f), IM_COL32(220, 60, 60, 255), "X");
    dl->AddText(ImVec2(pivot.x - 4.0f, pivot.y + len + 2.0f), IM_COL32(70, 200, 70, 255), "Y");
    dl->AddCircleFilled(pivot, 3.0f, IM_COL32(255, 160, 40, 255));
}

void ViewportPanel::drawButtonGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasButton() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // El rect sale del nodo VIVO (lo que dejó el último buildDrawData), no de
    // los campos del componente: así el gizmo ya trae aplicadas las anclas, la
    // escala del canvas y el layout, sin recalcular nada aquí.
    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiButtonNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawTextGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                   const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasText() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiTextNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawProgressBarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                          const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasProgressBar() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiProgressBarNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawInputFieldGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasInputField() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiInputFieldNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawDropdownGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasDropdown() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiDropdownNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawScrollViewGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasScrollView() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiScrollViewNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawSliderGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasSlider() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiSliderNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawCheckboxGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCheckbox() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiCheckboxNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawToggleGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasToggle() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiToggleNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawScrollbarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasScrollbar() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiScrollbarNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawPanelGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasPanel() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiPanelNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawImageGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasImage() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiImageNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

void ViewportPanel::drawLayoutGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasLayout() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // Con otro componente de UI en el mismo GameObject, el layout NO tiene nodo
    // propio: escribe en el de aquel, que ya pinta su gizmo. Dibujar otro encima
    // solo duplicaría las líneas.
    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiLayoutNodeName(ctx.selected->id)),
                    imagePos, imageSize);
}

GameObject* ViewportPanel::pickUiObject(EditorContext& ctx, const glm::vec2& mousePx,
                                         const glm::vec2& imageSize) const
{
    if (!ctx.renderer || !ctx.scene || imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return nullptr;

    // El hit test trabaja en píxeles de SALIDA, y la salida es esta misma
    // imagen: el ratón ya llega en ese espacio. Antes se multiplicaba por
    // render/imagen (el render INTERNO), y con SSAA el clic caía al doble de
    // lejos del cursor. Es el mismo espacio en el que el bucle del editor le
    // pasa el ratón a UiCanvas::updateInput.
    const UiElement* hit = ctx.renderer->uiCanvas().hitTest(mousePx);
    if (!hit) return nullptr;

    // El hit test devuelve el nodo más profundo, que puede ser la etiqueta: se
    // sube hasta el primero que sea de un GameObject.
    for (const UiElement* n = hit; n != nullptr; n = n->parent())
    {
        if (const uint64_t owner = uiButtonOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiTextOwnerId(n->name))
            return ctx.scene->findById(owner);
        // El nodo del relleno ("bar:7/Fill") también devuelve su dueño, así que
        // clicar dentro de la parte llena selecciona la barra igual.
        if (const uint64_t owner = uiProgressBarOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiInputFieldOwnerId(n->name))
            return ctx.scene->findById(owner);
        // El Dropdown ANTES que el resto: sus filas son nodos profundos que
        // solo el prefijo "drp:" sabe devolver a su dueno.
        if (const uint64_t owner = uiDropdownOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiSliderOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiScrollbarOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiToggleOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiCheckboxOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiImageOwnerId(n->name))
            return ctx.scene->findById(owner);
        // El Panel el ÚLTIMO de los cinco: es el fondo, así que un widget encima
        // suyo tiene que ganar el clic. Como el hit test devuelve el nodo más
        // profundo y esto sube por los padres, el orden de aquí solo desempata
        // entre nodos del MISMO GameObject.
        if (const uint64_t owner = uiPanelOwnerId(n->name))
            return ctx.scene->findById(owner);
        // El ScrollView el ULTIMO: es un contenedor, y cualquier widget que
        // lleve dentro tiene que ganarle el clic. Como el hit test devuelve el
        // nodo mas profundo y esto sube por los padres, llegar aqui significa
        // que no habia nada mas.
        if (const uint64_t owner = uiScrollViewOwnerId(n->name))
            return ctx.scene->findById(owner);
    }
    return nullptr;
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

void ViewportPanel::draw(EditorContext& ctx, uint64_t viewportTexture, const glm::mat4& cameraView)
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
    // Y el del canvas de MUNDO, que es el mismo gizmo pero proyectado. Va por
    // fuera de la clase porque necesita cameraView, que solo tiene draw():
    // drawCanvasGizmo se aparta en cuanto ve renderMode == World.
    drawWorldCanvasGizmo(ctx, cameraView, glm::vec2(vpPos.x, vpPos.y),
                         glm::vec2(vpSize.x, vpSize.y));
    drawButtonGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawTextGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawProgressBarGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawLayoutGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawInputFieldGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawDropdownGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawScrollViewGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawSliderGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawCheckboxGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawToggleGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawScrollbarGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawPanelGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    drawImageGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    // Hover de la IMAGEN, no de la ventana: con esto un popup o cualquier otra
    // ventana por encima ya no cuenta como clic en la escena.
    const bool imageHovered = ImGui::IsItemHovered();
    // Se publica para el input de la UI de juego (sandbox/src/main.cpp).
    m_imagePos     = glm::vec2(vpPos.x, vpPos.y);
    m_imageHovered = imageHovered;

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
        // La UI se dibuja ENCIMA de la escena, así que un clic sobre un widget
        // es del widget y no de lo que haya detrás. Solo en edición: en Play el
        // clic es del juego (lo consume el updateInput del canvas) y cambiar la
        // selección desde el viewport sería pelearse con él.
        GameObject* uiHit = ctx.isPlaying
                            ? nullptr
                            : pickUiObject(ctx, mousePx, glm::vec2(vpSize.x, vpSize.y));
        ctx.selected = uiHit ? uiHit
                             : pickObject(ctx, cameraView, mousePx, glm::vec2(vpSize.x, vpSize.y));
    }

    ImGui::End();
}

} // namespace DonTopo
