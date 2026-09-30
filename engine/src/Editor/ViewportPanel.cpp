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
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/CanvasComponent.h"   // uiWorldCanvasMatrix, UiCanvasRenderMode
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Core/TransformDecompose.h"
#include <cstdio>
#include <memory>
#include <string>
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo {

namespace {

// LOCAL bbox of the mesh of go, the same per-vertex computation already used by
// selectionAxisScale/focusSelected. false if go has no mesh or the mesh is
// empty: those objects do not take part in picking.
//
// A SkinnedMesh leaves Mesh::vertices EMPTY (its geometry lives in
// skinnedVertices), so we have to look there or animated characters could not
// be picked. It is the bind pose: the animation is applied by the skinning compute
// on the GPU and there is no evaluated pose here, same as the skinned culling of
// the Renderer, which also uses a bind pose bound.
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

// Bounding sphere in world space from the local bbox, scaled by the maximum scale
// of the worldTransform (the radius cannot depend on the axis, so the largest one
// wins). It is ONLY the quick rejection of picking: for a flat and huge mesh
// (the floor) the sphere is gigantic and swallows the camera, so what decides the
// hit is rayAabbLocal, not this.
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

// Ray/AABB intersection in the object's LOCAL space (the ray is brought there with
// the inverse of the worldTransform), which in world space is the object's oriented
// box. It returns the entry point in WORLD space: with different scales per axis
// the local t is not a distance, so the comparison between objects is done outside,
// with the real distance to the camera. The entry face is ignored if the camera
// is inside (t < 0 on the entry axis): then the hit is the origin.
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
            // Ray parallel to this pair of planes: it is either inside the slab or
            // it never intersects.
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
        return false; // the whole box is behind the camera

    const float t = tEnter >= 0.0f ? tEnter : 0.0f; // camera inside the box
    hitWorld = glm::vec3(world * glm::vec4(o + d * t, 1.0f));
    return true;
}

// Ray/sphere intersection. dir is NORMALIZED, so t comes out in world units and
// can be compared between objects. With the camera inside the sphere it returns
// t = 0 (hit at the origin itself): the object that wraps the camera is the
// closest possible, not one behind it.
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
    return false; // the whole sphere is behind the camera
}

} // namespace

float ViewportPanel::selectionAxisScale(GameObject* node) const
{
    constexpr float kFallback = 50.0f;
    // 2.0 instead of 1.3: with 1.3 it only stuck out a little from the mesh and it was
    // hard to see; this way the visible stretch outside the object is as long as its
    // own half-size.
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

glm::mat4 localFromWorld(const glm::mat4& parentWorld, const glm::mat4& newWorld)
{
    // The order matters and is not symmetric: worldTransform = parentWorld * local,
    // so solving for the local requires the inverse ON THE LEFT. With the
    // multiplication the other way round (newWorld * inverse(parentWorld)) a rotated
    // parent sends the child somewhere else, and with an identity parent (the
    // easy test case) both forms give the same result and the error does not show.
    return glm::inverse(parentWorld) * newWorld;
}

void applyLocalTransform(Scene& scene, uint64_t id, const glm::mat4& t)
{
    GameObject* obj = scene.findById(id);
    if (!obj)
        return;
    obj->localTransform = t;
    obj->updateWorldTransforms(obj->parent ? obj->parent->worldTransform : glm::mat4(1.0f));
    // Moving the transform does NOT move the PhysX actor: without the teleport the
    // object is seen where it should be and collides where it was, and fails silently.
    if (auto col = obj->anyCollider())
        col->teleport(obj->worldTransform);
}

const char* gizmoChannelLabel(GizmoMode mode)
{
    switch (mode)
    {
        case GizmoMode::Rotate: return "Rotation";
        case GizmoMode::Scale:  return "Scale";
        default:                return "Position";
    }
}

glm::vec3 gizmoLoggedValue(GizmoMode mode, const glm::mat4& localTransform)
{
    if (mode == GizmoMode::Translate)
        return glm::vec3(localTransform[3]);   // the fourth column, without decomposing

    // decomposeTransform (the repo wrapper), NOT bare glm::decompose. The glm one
    // returns false for a singular matrix (a scale of 0, which is where ImGuizmo lets
    // Scale mode get to) and does NOT write any of its outputs, so the stack padding
    // bytes would go to the log (0xCDCDCDCD = -1.07e8 in Debug; in Release, whatever
    // was there). The wrapper always writes the three: position from the fourth
    // column, scale from the column lengths, and an IDENTITY rotation when there is
    // none to extract.
    glm::vec3 pos, escala;
    glm::quat rot;
    decomposeTransform(localTransform, &pos, &rot, &escala);

    // Degrees, not radians: the inspector shows degrees and the two log lines
    // (the gizmo one and the Properties one) have to be comparable.
    return mode == GizmoMode::Rotate ? glm::degrees(glm::eulerAngles(rot)) : escala;
}

void gizmoImGuizmoEnums(GizmoMode mode, int& outOperation, int& outSpace)
{
    switch (mode)
    {
        case GizmoMode::Rotate:
            outOperation = ImGuizmo::ROTATE;
            outSpace     = ImGuizmo::LOCAL;
            break;
        case GizmoMode::Scale:
            outOperation = ImGuizmo::SCALE;
            outSpace     = ImGuizmo::LOCAL;
            break;
        case GizmoMode::Translate:
        default:
            outOperation = ImGuizmo::TRANSLATE;
            outSpace     = ImGuizmo::WORLD;
            break;
    }
}

void ViewportPanel::drawTransformGizmo(EditorContext& ctx, const glm::mat4& cameraView,
                                        const glm::vec2& imagePos, const glm::vec2& imageSize)
{
    // Same gates as the rest of the editing: without a selection there is nothing to
    // move, and with the loading modal in flight the scene is not touched.
    if (!ctx.selected || ctx.editingLocked || !ctx.renderer)
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // The frame camera, the same recipe as pickObject and worldCanvasMvp: in
    // edit mode a fixed 45 degrees, in Play the scene's CameraComponent rules.
    //
    // BUT WITHOUT the Vulkan `proj[1][1] *= -1`. It is not an arbitrary exception:
    // the Renderer flips the projection because Vulkan NDC has Y pointing down
    // (and D3D12 reaches the same image by another route, with a NEGATIVE-height
    // viewport). ImGuizmo draws with neither: it projects on the CPU and maps to
    // pixels with `y = 1 - y`, that is the OpenGL convention, with Y pointing up.
    // Passing it the flipped matrix gives a vertically mirrored gizmo: detached from
    // the object except at the exact center of the screen, and dragging in Y
    // backwards.
    //
    // The canvas gizmos next to this one do use the flipped one because their mapping
    // to pixels does NOT have the `1 - y`: there the two negations cancel out. Here
    // there is only one.
    const float aspect = ctx.renderer->viewportAspect();
    glm::mat4 view = cameraView;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    bool ortho = false;
    if (ctx.isPlaying && ctx.scene)
    {
        if (GameObject* cam = ctx.scene->findCamera())
        {
            const CameraComponent& cc = *cam->getCameraComponent();
            view = CameraComponent::viewFromWorld(cam->worldTransform);
            proj = cc.projectionMatrix(aspect);
            proj[1][1] *= -1.0f;   // undoes the flip that projectionMatrix has just put in
            ortho = cc.getMode() == CameraComponent::ProjectionMode::Orthographic;
        }
    }

    // The manipulator draws into the draw list of THIS window (and not its own,
    // which BeginFrame leaves pointing to a full-screen window): without this the
    // gizmo would be painted over the panels docked on top of the viewport.
    ImGuizmo::SetOrthographic(ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imagePos.x, imagePos.y, imageSize.x, imageSize.y);

    // Which operation and in which space, decided in gizmoImGuizmoEnums (that is where
    // the reason for the space not being the same in the three modes is).
    int op = 0, espacio = 0;
    gizmoImGuizmoEnums(m_gizmoMode, op, espacio);

    // ImGuizmo manipulates WORLD. The local is recomposed afterwards.
    glm::mat4 world = ctx.selected->worldTransform;
    const bool moved = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                            (ImGuizmo::OPERATION)op, (ImGuizmo::MODE)espacio,
                                            glm::value_ptr(world));
    const bool usando = ImGuizmo::IsUsing();

    // ENTRY edge: the previous state of the whole drag is stored. If it were
    // captured on every frame with movement, the `before` would be that of the
    // previous frame and undoing would go back one pixel.
    if (usando && !m_gizmoUsing)
    {
        m_gizmoBefore = ctx.selected->localTransform;
        m_gizmoId     = ctx.selected->id;
        m_gizmoName   = ctx.selected->name;
        // The mode too, and it is not read on release: the W/E/R shortcuts stay alive
        // during the drag, so pressing E in the middle of a rotation would leave the log
        // talking about the wrong channel.
        m_gizmoModeAtGrab = m_gizmoMode;
    }

    if (moved)
    {
        const glm::mat4 parentWorld = ctx.selected->parent ? ctx.selected->parent->worldTransform
                                                           : glm::mat4(1.0f);
        ctx.selected->localTransform = localFromWorld(parentWorld, world);
        // The world is recomputed here and we do not wait for the main loop traverse:
        // that one has already run this frame, and the next frame's manipulator reads
        // worldTransform again. Without this the gizmo would drag itself one frame
        // behind the mouse.
        ctx.selected->updateWorldTransforms(parentWorld);
        // And the collider separately: moving the transform does NOT move the PhysX
        // actor, and the collision fails silently. teleport (setGlobalPose) and not
        // syncTransform (setKinematicTarget, only valid on kinematic), same as in
        // PropertiesPanel.
        if (auto col = ctx.selected->anyCollider())
            col->teleport(ctx.selected->worldTransform);
    }

    // EXIT edge: one drag, one command. The object is resolved by id (it may have
    // disappeared between the click and the release) and not by the current
    // ctx.selected, which may have changed.
    if (!usando && m_gizmoUsing)
    {
        const bool tieneUndo = ctx.scene && ctx.undo;
        GameObject* go = tieneUndo ? ctx.scene->findById(m_gizmoId) : nullptr;
        // A drag that ends where it started (a click without moving) is not an
        // edit: stacking it would leave a Ctrl+Z that does nothing visible.
        if (go && go->localTransform != m_gizmoBefore)
        {
            Scene* scene       = ctx.scene;
            const uint64_t id  = m_gizmoId;
            const glm::mat4 before = m_gizmoBefore;
            const glm::mat4 after  = go->localTransform;
            ctx.undo->push(std::make_unique<PropertyCommand<glm::mat4>>(
                "Transform of '" + m_gizmoName + "'", before, after,
                [scene, id](const glm::mat4& t) { applyLocalTransform(*scene, id, t); }));

            // Exact same form as the line PropertiesPanel emits when editing the same value
            // by hand, channel included: "Rotation de 'X'
            // cambiado a (0.00, 90.00, 0.00)".
            char buf[64];
            const glm::vec3 v = gizmoLoggedValue(m_gizmoModeAtGrab, after);
            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
            if (ctx.pushLog)
                ctx.pushLog(std::string(gizmoChannelLabel(m_gizmoModeAtGrab)) + " of '" +
                            m_gizmoName + "' changed to " + std::string(buf));
        }
    }

    m_gizmoUsing = usando;
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

    // Attenuation of the 3D AudioClip: inner sphere (min, full volume) and
    // outer sphere (max, silence). Only for the selected object and only if the clip
    // is 3D: in 2D FMOD does not attenuate by distance and the spheres would lie.
    if (ctx.selected->hasAudioClip() && ctx.selected->getAudioClip()->getIs3D())
    {
        // Magenta: neither the yellow of the colliders, nor the cyan of the camera, nor
        // the orange of the lights.
        const glm::vec3 kAudioColor(1.0f, 0.2f, 0.8f);

        // Basis with NORMALIZED axes: FMOD distances are world units, so the GameObject
        // scale cannot stretch the spheres (unlike the colliders, which do scale with
        // the object). Degenerate basis (a scale of 0 from Properties) -> identity, so as
        // not to put NaN in the gizmo vertex buffer.
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

    // Reverb zone: the same two spheres (inside min it is at full, between min and
    // max it fades) in another color so it is not confused with the attenuation of
    // an AudioClip, which is drawn the same way.
    if (ctx.selected->hasReverbZone())
    {
        const glm::vec3 kReverbColor(0.2f, 0.9f, 0.9f);
        glm::mat4 basis(1.0f);
        basis[3] = ctx.selected->worldTransform[3];
        const ReverbZoneComponent& zone = *ctx.selected->getReverbZone();
        Gizmos::drawWireSphere(basis, glm::vec3(0.0f), zone.getMinDistance(), kReverbColor);
        Gizmos::drawWireSphere(basis, glm::vec3(0.0f), zone.getMaxDistance(), kReverbColor);
    }
}

void ViewportPanel::drawCameraGizmo(EditorContext& ctx)
{
    // Only in edit mode: in Play we are already looking THROUGH that camera, drawing its
    // own frustum adds nothing (and would block the view from inside).
    if (ctx.isPlaying || !ctx.scene || !ctx.renderer)
        return;

    GameObject* cam = ctx.scene->findCamera();
    if (!cam) return;

    // The aspect comes from the Renderer (the render target one), not from the size of
    // this ImGui window: it has to be EXACTLY the one the projection will use when
    // Play is pressed, or the wireframe would draw a framing that is later not met.
    const glm::mat4 viewProj =
        cam->getCameraComponent()->projectionMatrix(ctx.renderer->viewportAspect()) *
        CameraComponent::viewFromWorld(cam->worldTransform);

    // Cyan: different from the yellow of the colliders, so they are not confused.
    const glm::vec3 kCameraGizmoColor(0.0f, 1.0f, 1.0f);
    // true: this viewProj comes from CameraComponent::projectionMatrix, which uses
    // *_ZO (near->z_ndc=0) for Vulkan, not glm's default NO convention.
    Gizmos::drawFrustum(viewProj, kCameraGizmoColor, /*depthZeroToOne=*/true);
}

void ViewportPanel::drawLightGizmos(EditorContext& ctx)
{
    // The View menu flag rules: each Gizmos::drawX already checks it inside,
    // but checking it here saves walking the whole scene.
    if (!ctx.scene || !Gizmos::isEnabled())
        return;

    // Orange: neither the yellow of the colliders nor the cyan of the camera.
    const glm::vec3 kLightColor(1.0f, 0.8f, 0.2f);

    ctx.scene->traverse([&](GameObject* go) {
        if (!go->hasLight()) return;
        const LightComponent& lc = *go->getLight();

        // Same basis that Scene::collectLights uses to send the light to the shader:
        // position in column 3 and local -Z as direction. The axes go
        // NORMALIZED: the gizmo measures in world units, so the GameObject scale
        // must not stretch it (unlike the colliders, which do scale with
        // the object).
        const glm::vec3 pos   = glm::vec3(go->worldTransform[3]);
        glm::vec3 right = glm::vec3(go->worldTransform[0]);
        glm::vec3 up    = glm::vec3(go->worldTransform[1]);
        glm::vec3 fwd   = -glm::vec3(go->worldTransform[2]);
        // Degenerate basis (a scale of 0 from Properties): normalize would give
        // NaN and the gizmo vertex buffer would fill with garbage.
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
                // Four generatrices of the outer cone (top/bottom/left/
                // right): with the edge angle, which is where the spot
                // fades out completely.
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
                // It has no range: the length is only for seeing it, it does not mean
                // how far it reaches (it reaches everywhere).
                Gizmos::drawRay(pos, fwd, 500.0f, kLightColor);
                break;

            case LightType::Area:
            {
                // drawWirePlane draws a 10x10 unit grid on the XZ plane of the matrix passed to
                // it, so the basis is assembled so that that plane is the rectangle's (its normal
                // is fwd) and scaled to width x height.
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

// The GameObject whose Canvas rules over `go`: the NEAREST ancestor that has
// one, and `go` itself counts. It is EXACTLY the rule of
// Scene::collectCanvases (a nested canvas opens its own binding and CUTS the
// anchoring chain), so the gizmo looks at the same canvas the sync sends the
// widget to. nullptr if it hangs from none: the editor prevents that
// (uiComponentsAvailable), but a hand-made scene can bring it.
//
// It returns the GameObject and not the component because BOTH are needed: the
// component for worldScale/billboard and its worldTransform to place the
// canvas in the world.
//
// It is not static on purpose, for the same reason as projectWorldCanvasCorners:
// dt_camera_tests declares it by hand to be able to test the rule without a GUI.
const GameObject* owningCanvasObject(const GameObject* go)
{
    for (const GameObject* n = go; n != nullptr; n = n->parent)
        if (n->hasCanvas())
            return n;
    return nullptr;
}

// Projects the four corners of a RECT inside a WORLD canvas to pixels of the
// viewport image. `mvp` is proj*view*model (the same chain the backend
// records) and the rect is in CANVAS pixels, which is the space uiWorldCanvasMatrix
// starts from. They come out in the order min, (max.x,min.y), max, (min.x,max.y);
// the minimum is the TOP-left corner, because the canvas Y grows downwards and
// uiWorldCanvasMatrix negates it.
//
// The whole canvas is NOT a separate case: it is this same function with
// (0,0)-(referenceResolution). The widgets inside pass their own rect. A
// degenerate rect (min == max) gives the same point four times, which is how the
// widget gizmo gets its projected pivot.
//
// It returns false (and leaves `outCorners` UNTOUCHED) if any corner has w <= 0:
// it is behind the camera plane, and dividing by that w mirrors it to the other
// side. The quadrilateral would come out crossed or shot off to infinity and
// nothing would say so, because here there is no GPU clipping to rely on: it is
// ImGui painting the lines it is given. That is why the rejection is EXPLICIT. The
// comparison is written as !(w > 0) so that a NaN (degenerate matrix) also falls
// on the rejection side.
//
// Outside the frame but IN FRONT is accepted: the criterion is the sign of w, not
// whether the rect fits in the image. Clipping here would leave without a gizmo
// exactly the canvas that peeks half over the edge, which is when it is
// looked for the most.
//
// It is not static on purpose: it is the only part of the gizmo that can be tested
// without a window, and dt_camera_tests declares it by hand to link it (the editor
// has no public header to put it in).
bool projectWorldCanvasCorners(const glm::mat4& mvp,
                               const glm::vec2& rectMin, const glm::vec2& rectMax,
                               const glm::vec2& imagePos, const glm::vec2& imageSize,
                               glm::vec2 outCorners[4])
{
    const glm::vec2 esquinas[4] = {
        glm::vec2(rectMin.x, rectMin.y),
        glm::vec2(rectMax.x, rectMin.y),
        glm::vec2(rectMax.x, rectMax.y),
        glm::vec2(rectMin.x, rectMax.y),
    };

    // Into an intermediate and not directly into outCorners: if a corner rejects when
    // others have already been computed, the caller cannot be left with a mix of new
    // and old corners. It is tested by
    // test_world_canvas_gizmo_rechaza_esquina_detras_de_la_camara, whose fixture
    // is deliberately built so that the one that rejects is NOT the first.
    glm::vec2 px[4];
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec4 clip = mvp * glm::vec4(esquinas[i].x, esquinas[i].y, 0.0f, 1.0f);
        if (!(clip.w > 0.0f))
            return false;

        // NDC -> image pixel. The Y goes WITHOUT inverting because the projection
        // passed in has the Vulkan Y-flip baked in (ndc.y = -1 at the top),
        // the same criterion as pickObject. In D3D12 the backend projection
        // does NOT carry that flip and its NDC has +1 at the top: the two inversions
        // cancel out and the pixel comes out identical. It is exactly why
        // pickObject gets it right in both backends with a single formula.
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        px[i] = imagePos + glm::vec2((ndc.x * 0.5f + 0.5f) * imageSize.x,
                                     (ndc.y * 0.5f + 0.5f) * imageSize.y);
    }

    for (int i = 0; i < 4; ++i)
        outCorners[i] = px[i];
    return true;
}

// MVP of a WORLD canvas: proj*view*model, the same chain the backend
// records. `canvasObj` is the GameObject that carries the Canvas. false if that
// canvas is not a world one or if its resolution is degenerate.
//
// BOTH gizmos use it (the canvas one and the one of each widget inside) so that
// they cannot disagree: the widget has to land on the same quadrilateral that is
// painted for its canvas.
static bool worldCanvasMvp(EditorContext& ctx, const GameObject* canvasObj,
                           const glm::mat4& cameraView, glm::mat4& outMvp, glm::vec2& outTam)
{
    if (!canvasObj || !canvasObj->hasCanvas() || !ctx.renderer)
        return false;

    const CanvasComponent& c = *canvasObj->getCanvas();
    if (c.renderMode != UiCanvasRenderMode::World)
        return false;

    // In World mode the usable area is EXACTLY referenceResolution: applyTo does not
    // let scaleMode, the safe area or the aspect ratio touch it, and
    // buildDrawData is called with exactly that (Renderer.cpp:1654,
    // D3D12Renderer.cpp:6261).
    outTam = c.referenceResolution;
    if (outTam.x <= 0.0f || outTam.y <= 0.0f)
        return false;

    // Frame camera, the same recipe as pickObject: in edit mode a fixed 45 degrees +
    // the Vulkan Y-flip; in Play the scene's CameraComponent rules. The
    // near/far does NOT enter the computation (in a perspective it only affects Z, x/y/w
    // come from fov and aspect), so the generic ones here are valid even if the
    // Renderer uses m_cameraDistance.
    //
    // KNOWN LIMITATION: the recording uses the JITTERED projection
    // (taaJitteredProj in both backends) and there is no way to ask for it here
    // without a new virtual in EditorRenderer. With TAA on the gizmo can
    // end up up to half a pixel from the billboard; without TAA, taaJitteredProj is
    // the plain projection and they match.
    const float aspect = ctx.renderer->viewportAspect();
    glm::mat4 view = cameraView;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan Y flip, same as the Renderer
    if (ctx.isPlaying && ctx.scene)
    {
        if (GameObject* cam = ctx.scene->findCamera())
        {
            view = CameraComponent::viewFromWorld(cam->worldTransform);
            proj = cam->getCameraComponent()->projectionMatrix(aspect);
        }
    }

    // The SAME model matrix as the recording, and with the SAME `view`: the
    // billboard comes from it, so giving it another would separate the gizmo from the
    // billboard as soon as the camera turned.
    outMvp = proj * view * uiWorldCanvasMatrix(c, outTam, canvasObj->worldTransform, view);
    return true;
}

// Paints the quadrilateral of an already projected rect. Shared by the canvas
// gizmo and the widget one: the only thing that changes between the two is the color.
static void strokeProjectedQuad(ImDrawList* dl, const glm::vec2 esq[4], ImU32 color)
{
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec2& a = esq[i];
        const glm::vec2& b = esq[(i + 1) % 4];
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), color, 2.0f);
    }
}

// Canvas gizmo in WORLD mode: the quadrilateral of its plane, projected, which
// is the only thing that shows where it is and with what tilt. The 2D usable-area
// rect that drawCanvasGizmo paints means nothing here.
//
// Free function and not a method because it needs the camera `view` and the one
// that has it is draw().
static void drawWorldCanvasGizmo(EditorContext& ctx, const glm::mat4& cameraView,
                                 const glm::vec2& imagePos, const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCanvas() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    glm::mat4 mvp;
    glm::vec2 tam;
    if (!worldCanvasMvp(ctx, ctx.selected, cameraView, mvp, tam))
        return;

    // The whole canvas is the rect (0,0)-(referenceResolution) of the same
    // function that the widgets use.
    glm::vec2 esq[4];
    if (!projectWorldCanvasCorners(mvp, glm::vec2(0.0f), tam, imagePos, imageSize, esq))
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = IM_COL32(80, 200, 255, 220);   // the same blue as the 2D gizmo
    strokeProjectedQuad(dl, esq, color);
    // Mark at the (0,0) corner of the canvas. Without it, a canvas seen FROM
    // BEHIND paints the same quadrilateral and there is no way to know that it is
    // backwards.
    dl->AddCircleFilled(ImVec2(esq[0].x, esq[0].y), 4.0f, color);
}

void ViewportPanel::drawCanvasGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCanvas() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // A WORLD canvas has no usable area in screen pixels to show:
    // drawWorldCanvasGizmo takes care of it, projecting it. Also drawing the 2D
    // rect would paint a box in a place that has nothing to do with it.
    if (ctx.selected->getCanvas()->renderMode == UiCanvasRenderMode::World)
        return;

    // What the last buildDrawData of the live canvas left: origin in render
    // pixels and usable-area size = reference * scale. Nothing is
    // resolved again here.
    // The canvas OF THE SELECTED OBJECT, not uiCanvas() (which is the FIRST
    // screen one): with that one, selecting a SECOND screen canvas painted the
    // rect of the first, that is a gizmo that lies about where what has been
    // selected is. With a single canvas they coincide, which is what made it silent.
    // Without a live canvas (the sync has not run yet) nothing is drawn, which is
    // better than drawing another one's.
    const UiCanvas* canvas = ctx.renderer->uiCanvasOf(ctx.selected->id);
    if (!canvas)
        return;
    const glm::vec2 origin = canvas->uiOrigin();
    const glm::vec2 size   = canvas->referenceSize() * canvas->uiScale();
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;

    // The canvas is resolved in OUTPUT pixels, and the output is exactly
    // this image (the panel dictates its size), so they go 1:1. It used to divide
    // by renderWidth/renderHeight (the INTERNAL render), and with SSAA that moved the
    // box to half or double the real area.
    const ImVec2 p0{ imagePos.x + origin.x, imagePos.y + origin.y };
    const ImVec2 p1{ p0.x + size.x, p0.y + size.y };
    ImGui::GetWindowDrawList()->AddRect(p0, p1, IM_COL32(80, 200, 255, 220), 0.0f, 0, 2.0f);
}

// UNUSED since the thirteen widget gizmos below went through
// EditorRenderer::findUiNode, which walks ALL the canvases and not only the
// screen one (this wrapper only looked at the one it was given, so a widget
// inside a WORLD canvas was left without a gizmo silently). It is kept
// because deleting symbols is not part of this task; findUiNodeIn, which it wraps,
// is still alive in UiCanvas.h and the Renderer uses it.
static const UiElement* findUiNodeNamed(const UiCanvas& canvas, const std::string& wanted)
{
    return findUiNodeIn(canvas.root(), wanted);
}

// Rect + axes of the live node passed to it. Shared by the thirteen widget
// gizmos: the only thing that changes between them is which node they come from.
//
// Two paths, and which one is taken is decided by the canvas that OWNS the widget:
//   - SCREEN canvas: the rect goes 1:1 over the image, as always.
//   - WORLD canvas: it has to be projected, and that is why `cameraView` is needed.
static void drawUiNodeGizmo(EditorContext& ctx, const UiElement* node,
                            const glm::mat4& cameraView,
                            const glm::vec2& imagePos, const glm::vec2& imageSize)
{
    if (!node || !node->rectValid) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 kRectColor  = IM_COL32(255, 160, 40, 230);
    const ImU32 kPivotColor = IM_COL32(255, 160, 40, 255);
    const ImU32 kXColor     = IM_COL32(220,  60,  60, 255);
    const ImU32 kYColor     = IM_COL32( 70, 200,  70, 255);
    const float len = 34.0f;

    // -- WORLD canvas -------------------------------------------------------
    // The rect of this node is NOT in screen pixels: in a world canvas
    // buildDrawData is called with the referenceResolution (Renderer.cpp:1654,
    // D3D12Renderer.cpp:6261) and applyTo forces ConstantPixelSize/scaleFactor 1,
    // so the canvas scale stays at 1 and its origin at (0,0). With that,
    // `screenPos = origin + worldPos * scale` (UiSpriteBatch.cpp:1328) returns
    // worldPos BIT FOR BIT, that is the canvas LOCAL pixels: exactly the space
    // uiWorldCanvasMatrix starts from. It goes in as is, without conversion.
    //
    // Adding it to imagePos as the screen path does would paint the rect stuck
    // to the corner of the viewport and still while the camera flies.
    if (const GameObject* canvasObj = owningCanvasObject(ctx.selected))
    {
        glm::mat4 mvp;
        glm::vec2 tam;
        if (worldCanvasMvp(ctx, canvasObj, cameraView, mvp, tam))
        {
            const glm::vec2 rectMin = node->screenPos;
            const glm::vec2 rectMax = node->screenPos + node->screenSize;

            glm::vec2 esq[4];
            // Some corner behind the camera: NOTHING is drawn, same as in
            // the canvas gizmo. Half a figure backwards is worse than none.
            if (!projectWorldCanvasCorners(mvp, rectMin, rectMax, imagePos, imageSize, esq))
                return;
            strokeProjectedQuad(dl, esq, kRectColor);

            // Pivot and axes, projected the same way as the rect. The trick is to ask for a
            // tiny rect that STARTS at the pivot: corner 0 is the projected pivot
            // and corners 1 and 3 give the directions of +X and +Y right there,
            // with the perspective already applied. Interpolating the four
            // corners of the big rect would give something else: under perspective the
            // on-screen interpolation is not the canvas one.
            const glm::vec2 pivotCanvas = rectMin + node->pivot * node->screenSize;
            const glm::vec2 paso        = glm::max(node->screenSize * 0.01f, glm::vec2(1.0f));

            glm::vec2 base[4];
            if (projectWorldCanvasCorners(mvp, pivotCanvas, pivotCanvas + paso,
                                          imagePos, imageSize, base))
            {
                const ImVec2 pivot{ base[0].x, base[0].y };
                // Axes of FIXED LENGTH on screen, as in the 2D path: what
                // the gizmo shows is the ORIENTATION, not the size. An axis seen
                // edge-on vanishes when projected and normalizing a zero would give
                // NaN: in that case that axis is not painted.
                const glm::vec2 ejes[2] = { base[1] - base[0], base[3] - base[0] };
                const ImU32     cols[2] = { kXColor, kYColor };
                const char*     nombre[2] = { "X", "Y" };
                for (int e = 0; e < 2; ++e)
                {
                    const float largo2 = glm::dot(ejes[e], ejes[e]);
                    if (largo2 < 1e-12f) continue;
                    const glm::vec2 d = ejes[e] * glm::inversesqrt(largo2) * len;
                    const ImVec2 punta{ pivot.x + d.x, pivot.y + d.y };
                    dl->AddLine(pivot, punta, cols[e], 2.0f);
                    dl->AddText(ImVec2(punta.x + 2.0f, punta.y - 7.0f), cols[e], nombre[e]);
                }
                dl->AddCircleFilled(pivot, 3.0f, kPivotColor);
            }
            return;
        }

        // worldCanvasMvp has said no and the canvas IS a world one: the only way
        // left is a degenerate referenceResolution (some component
        // <= 0, and the dragVec2 of PropertiesPanel lets it go down to 0). In that state
        // the canvas draws nothing and neither does its own gizmo. Falling into the
        // screen branch would paint the rect stuck to the corner of the viewport and still
        // while the camera flies, which is EXACTLY the failure this path
        // exists to avoid. No gizmo is the coherent thing.
        if (canvasObj->getCanvas()->renderMode == UiCanvasRenderMode::World)
            return;
    }

    // screenPos/screenSize are left by buildDrawData in OUTPUT pixels, and the
    // output is this same image: they go 1:1. It used to be scaled by
    // image/renderWidth (the INTERNAL render) and with SSAA the box came out at
    // half size and at half position. Same criterion as
    // drawCanvasAreaGizmo and as pickUiObject.
    const ImVec2 p0{ imagePos.x + node->screenPos.x,
                     imagePos.y + node->screenPos.y };
    const ImVec2 p1{ p0.x + node->screenSize.x,
                     p0.y + node->screenSize.y };

    dl->AddRect(p0, p1, kRectColor, 0.0f, 0, 2.0f);

    // Axes from the PIVOT, which is the point it anchors and rotates around: X to the
    // right and Y DOWNWARDS, which is the direction of +Y in the UI (not the one of the
    // 3D world). Only two axes: a rect has no Z.
    const ImVec2 pivot{ p0.x + node->pivot.x * (p1.x - p0.x),
                        p0.y + node->pivot.y * (p1.y - p0.y) };
    dl->AddLine(pivot, ImVec2(pivot.x + len, pivot.y), kXColor, 2.0f);
    dl->AddLine(pivot, ImVec2(pivot.x, pivot.y + len), kYColor, 2.0f);
    dl->AddText(ImVec2(pivot.x + len + 2.0f, pivot.y - 7.0f), kXColor, "X");
    dl->AddText(ImVec2(pivot.x - 4.0f, pivot.y + len + 2.0f), kYColor, "Y");
    dl->AddCircleFilled(pivot, 3.0f, kPivotColor);
}

void ViewportPanel::drawButtonGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasButton() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // The rect comes from the LIVE node (what the last buildDrawData left), not from
    // the component fields: this way the gizmo already has the anchors, the canvas
    // scale and the layout applied, without recomputing anything here.
    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiButtonNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawTextGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                   const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasText() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiTextNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawProgressBarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                          const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasProgressBar() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiProgressBarNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawInputFieldGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasInputField() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiInputFieldNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawDropdownGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasDropdown() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiDropdownNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawScrollViewGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasScrollView() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiScrollViewNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawSliderGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasSlider() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiSliderNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawCheckboxGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasCheckbox() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiCheckboxNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawToggleGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasToggle() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiToggleNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawScrollbarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasScrollbar() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiScrollbarNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawPanelGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasPanel() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiPanelNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawImageGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                    const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasImage() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiImageNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

void ViewportPanel::drawLayoutGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                                     const glm::vec2& imageSize)
{
    if (!ctx.selected || !ctx.selected->hasLayout() || !ctx.renderer || !Gizmos::isEnabled())
        return;
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return;

    // With another UI component on the same GameObject, the layout has NO node of
    // its own: it writes into that one's, which already paints its gizmo. Drawing
    // another one on top would only duplicate the lines.
    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiLayoutNodeName(ctx.selected->id)), m_cameraView,
                    imagePos, imageSize);
}

GameObject* ViewportPanel::pickUiObject(EditorContext& ctx, const glm::vec2& mousePx,
                                         const glm::vec2& imageSize) const
{
    if (!ctx.renderer || !ctx.scene || imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        return nullptr;

    // The hit test works in OUTPUT pixels, and the output is this same
    // image: the mouse already arrives in that space. It used to be multiplied by
    // render/image (the INTERNAL render), and with SSAA the click landed twice as
    // far from the cursor. It is the same space in which the editor loop passes
    // the mouse to UiCanvas::updateInput.
    // ALL the screen canvases and in the SAME priority order as the
    // input (topmost first, see dispatchUiInput): if a different order were tried
    // here, clicking in the viewport would select an object different from the one
    // the user sees on top, and the one that does respond to the mouse in Play would be the other.
    // With uiCanvas() (the FIRST screen canvas) a widget of a second
    // canvas could not be selected by clicking, without a single warning.
    std::vector<UiCanvas*> canvases;
    ctx.renderer->screenUiCanvases(canvases);

    const UiElement* hit = nullptr;
    for (UiCanvas* c : canvases)
    {
        if (!c) continue;
        if ((hit = c->hitTest(mousePx)) != nullptr) break;
    }
    if (!hit) return nullptr;

    // The hit test returns the deepest node, which may be the label: we
    // climb up to the first one that belongs to a GameObject.
    for (const UiElement* n = hit; n != nullptr; n = n->parent())
    {
        if (const uint64_t owner = uiButtonOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiTextOwnerId(n->name))
            return ctx.scene->findById(owner);
        // The fill node ("bar:7/Fill") also returns its owner, so
        // clicking inside the filled part selects the bar all the same.
        if (const uint64_t owner = uiProgressBarOwnerId(n->name))
            return ctx.scene->findById(owner);
        if (const uint64_t owner = uiInputFieldOwnerId(n->name))
            return ctx.scene->findById(owner);
        // The Dropdown BEFORE the rest: its rows are deep nodes that
        // only the "drp:" prefix knows how to return to their owner.
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
        // The Panel the LAST of the five: it is the background, so a widget on top of
        // it has to win the click. Since the hit test returns the deepest node and this
        // climbs through the parents, the order here only breaks ties
        // between nodes of the SAME GameObject.
        if (const uint64_t owner = uiPanelOwnerId(n->name))
            return ctx.scene->findById(owner);
        // The ScrollView the LAST: it is a container, and any widget
        // it carries inside has to win the click over it. Since the hit test returns the
        // deepest node and this climbs through the parents, getting here means
        // there was nothing else.
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

    // Aspect of the render target, the same one the Renderer uses to build the
    // frame projection (and that drawCameraGizmo already uses); if the panel is
    // what dictates that size, it matches imageSize.
    const float aspect = ctx.renderer ? ctx.renderer->viewportAspect()
                                      : imageSize.x / imageSize.y;

    // Frame camera, same as Renderer::currentFrameCamera: in Play the scene's
    // CameraComponent rules (its projectionMatrix already has the Vulkan Y-flip and
    // z=[0,1]); in edit mode, the editor fly camera, whose projection is a fixed
    // 45 degrees + Y-flip. The editor near/far comes from a private state of the
    // Renderer, but the DIRECTION of the ray through a pixel does not depend on
    // the planes, only on fov/aspect/Y-flip: that is why generic ones are valid here.
    glm::mat4 view = cameraView;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan Y flip, same as the Renderer
    if (ctx.isPlaying)
    {
        if (GameObject* cam = ctx.scene->findCamera())
        {
            view = CameraComponent::viewFromWorld(cam->worldTransform);
            proj = cam->getCameraComponent()->projectionMatrix(aspect);
        }
    }

    // Mouse NDC inside the IMAGE. With the Y-flip inside the projection,
    // y = -1 is the TOP edge of the image, which is exactly the direction in
    // which the mouse pixel grows: nothing to invert here.
    const float ndcX = (mousePx.x / imageSize.x) * 2.0f - 1.0f;
    const float ndcY = (mousePx.y / imageSize.y) * 2.0f - 1.0f;

    const glm::mat4 invViewProj = glm::inverse(proj * view);
    // z=1 is the far plane in both depth conventions (ZO of
    // CameraComponent and [-1,1] of the editor projection), so this point
    // is valid for both without rebuilding anything by hand.
    const glm::vec4 farH = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::fabs(farH.w) < 1e-9f)
        return nullptr;

    // The origin is the real camera position (inverse of the view), not the
    // point on the near plane: this way t is distance to the camera and comparing t
    // between objects really orders by closeness.
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 origin  = glm::vec3(invView[3]);
    const glm::vec3 target  = glm::vec3(farH) / farH.w;
    const glm::vec3 delta   = target - origin;
    if (glm::length(delta) < 1e-6f)
        return nullptr;
    const glm::vec3 dir = glm::normalize(delta);

    GameObject* best     = nullptr;
    float       bestDist = 0.0f;
    // Pre-order: GameObject::traverse visits the node and then its children. Between
    // two hits the one closest to the camera wins; the tie is broken by the
    // first one visited.
    ctx.scene->traverse([&](GameObject* go) {
        glm::vec3 bMin, bMax;
        if (!localBounds(go, bMin, bMax))
            return;

        // Quick rejection by sphere; the real hit is given by the box.
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
    // ImGuizmo frame start. It goes FIRST and unconditionally (even
    // with the panel closed) because it is what rotates the hover flag of the
    // previous frame; skipping it for a frame would leave IsOver() returning the value of the
    // last frame in which it was called.
    //
    // Until this manipulator existed there was no call to BeginFrame
    // in the whole repo, and the `ImGuizmo::IsOver() || ImGuizmo::IsUsing()` that
    // guards the picking below was ALWAYS false: IsOver() relies on a
    // flag that only BeginFrame and Manipulate write, and IsUsing() on a state
    // that only Manipulate turns on. It was a door that closed nothing.
    ImGuizmo::BeginFrame();

    // Outline of the selected object. It is ALWAYS set and unconditionally, up
    // here: if it were done only when there is a selection, the Renderer would keep
    // the index of the previous object when deselecting and would keep
    // highlighting it. An object without a mesh has no render index (-1 in both
    // fields), so it draws nothing either.
    if (ctx.renderer)
    {
        ctx.renderer->setOutlineTarget(
            ctx.selected ? ctx.selected->staticRenderIndex  : -1,
            ctx.selected ? ctx.selected->skinnedRenderIndex : -1);
    }

    // Editing veto while the loading modal is active: the manipulation gizmo
    // (ImGuizmo) moves/rotates/scales the selected object, so it is
    // skipped. drawCameraGizmo is only debug-draw (it does not edit), it is always kept.
    if (!ctx.editingLocked)
        drawSelectionGizmo(ctx);
    drawCameraGizmo(ctx);
    // Also in Play: the light is still a scene object that has to be
    // placeable while the game runs.
    drawLightGizmos(ctx);

    if (!m_open)
    {
        // Without this, closing Viewport would leave m_hovered at its last
        // value (possibly true) and the camera mouse-look in
        // sandbox/src/main.cpp would keep responding with the panel hidden.
        m_hovered = false;
        return;
    }
    // The frame view, for the widget gizmos of a WORLD canvas: they
    // need it to project and do not receive it as a parameter. It is published here,
    // like m_imagePos and m_contentWidth further below.
    m_cameraView = cameraView;

    ImGui::Begin("Viewport", &m_open);
    m_hovered = ImGui::IsWindowHovered();
    ImVec2 vpPos  = ImGui::GetCursorScreenPos();
    ImVec2 vpSize = ImGui::GetContentRegionAvail();
    // It is published so that the Renderer renders at EXACTLY this size: this way the
    // image is drawn 1:1 and does not go through ImGui rescaling.
    m_contentWidth  = (uint32_t)(vpSize.x > 0.0f ? vpSize.x : 0.0f);
    m_contentHeight = (uint32_t)(vpSize.y > 0.0f ? vpSize.y : 0.0f);
    ImGui::Image((ImTextureID)(intptr_t)viewportTexture, vpSize);
    // Usable area of the selected Canvas, right over the image: it is 2D, so
    // it goes with the ImGui draw list and not with Gizmos (which draws in the world).
    drawCanvasGizmo(ctx, glm::vec2(vpPos.x, vpPos.y), glm::vec2(vpSize.x, vpSize.y));
    // And the one of the WORLD canvas, which is the same gizmo but projected. It goes
    // outside the class because it needs cameraView, which only draw() has:
    // drawCanvasGizmo steps aside as soon as it sees renderMode == World.
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
    // The manipulator, the last of those that paint over the image so that its
    // arrows end up above everything else. It is also the only one that edits.
    //
    // It adds no ImGui items (it only draws and asks for the mouse capture), so
    // the IsItemHovered on the next line still refers to the Image.
    drawTransformGizmo(ctx, cameraView, glm::vec2(vpPos.x, vpPos.y),
                        glm::vec2(vpSize.x, vpSize.y));
    // Hover of the IMAGE, not of the window: with this a popup or any other
    // window on top no longer counts as a click on the scene.
    const bool imageHovered = ImGui::IsItemHovered();
    // It is published for the game UI input (sandbox/src/main.cpp).
    m_imagePos     = glm::vec2(vpPos.x, vpPos.y);
    m_imageHovered = imageHovered;

    // Unity/Godot style axis gizmo (top-right corner): world axes
    // projected by the real camera rotation (3x3 part of the view
    // matrix), so it rotates with it. Clicking a ball reorients the camera
    // to look along that axis (via ctx.onAxisSelected).
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

    // Paints the axis farthest from the camera first so that the nearest one ends up on top.
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
                // Marks the click as consumed by the axis gizmo: reorienting
                // the camera must not also change the selection.
                axisBallClicked = true;
                if (ctx.onAxisSelected)
                    ctx.onAxisSelected(axes[i].world);
            }
        }
    }
    drawList->AddCircleFilled(center, 3.0f, IM_COL32(200, 200, 200, 255));

    // Click selection in the scene. Gates, in this order: the click lands
    // on the image (not on another window nor on the axis gizmo), no
    // ImGui widget is active (slider drag, drag&drop...), the manipulation
    // gizmo is neither hovered nor in use, and there is no loading modal. With no
    // hit, the selection goes to nullptr, same as a click on an empty area
    // of the Scene panel.
    const bool gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (clicked && imageHovered && !axisBallClicked && !gizmoBusy &&
        !ImGui::IsAnyItemActive() && !ctx.editingLocked)
    {
        const glm::vec2 mousePx(mouse.x - vpPos.x, mouse.y - vpPos.y);
        // The UI is drawn ON TOP of the scene, so a click on a widget
        // belongs to the widget and not to whatever is behind it. Only in edit mode: in Play the
        // click belongs to the game (the canvas updateInput consumes it) and changing the
        // selection from the viewport would be fighting with it.
        GameObject* uiHit = ctx.isPlaying
                            ? nullptr
                            : pickUiObject(ctx, mousePx, glm::vec2(vpSize.x, vpSize.y));
        ctx.selected = uiHit ? uiHit
                             : pickObject(ctx, cameraView, mousePx, glm::vec2(vpSize.x, vpSize.y));
    }

    ImGui::End();
}

} // namespace DonTopo
