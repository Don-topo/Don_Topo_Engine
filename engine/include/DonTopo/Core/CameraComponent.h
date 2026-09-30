#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    // Game camera component (equivalent to Unity Camera). It does NOT store
    // position or orientation: they are given by the worldTransform of the owner GameObject,
    // so moving the object moves the camera. It does not store aspect ratio either —
    // it is dictated by the viewport (Renderer::viewportAspect), so that resizing the
    // window does not distort the image (there is no letterboxing today).
    //
    // Pure data: no Vulkan and no knowledge of GameObject, same rule as Rigidbody
    // (the dependency goes Core -> the rest, never the other way).
    //
    // The component builds its own matrices on purpose: the Renderer (in
    // Play) and the frustum gizmo (in editing) have to match, or the drawn
    // wireframe would lie about what is seen when pressing Play.
    class CameraComponent
    {
        public:
            enum class ProjectionMode { Perspective, Orthographic };

            CameraComponent() = default;

            ProjectionMode getMode() const { return m_mode; }
            void setMode(ProjectionMode mode) { m_mode = mode; }

            // The clamps live here (and not in the UI) so that a hand-edited .scene
            // cannot install a degenerate projection either.
            float getFov() const { return m_fov; }              // degrees, perspective only
            void  setFov(float degrees);
            float getOrthographicSize() const { return m_orthographicSize; } // world half-height, orthographic only
            void  setOrthographicSize(float size);
            float getNear() const { return m_near; }
            void  setNear(float n);
            float getFar() const { return m_far; }
            void  setFar(float f);

            // Projection for the given aspect, with the Vulkan Y-flip ALREADY applied
            // (proj[1][1] *= -1): both of its consumers need it, and leaving it
            // to the caller invited one of the two to forget it.
            glm::mat4 projectionMatrix(float aspect) const;

            // View of a camera placed in world, looking along local -Z (same
            // convention as glm::lookAt and DonTopo::Camera, whose default yaw
            // of -90° gives front = (0,0,-1)). It normalizes the axes before
            // inverting: a scaled GameObject would bake its scale into the view and
            // distort the image. static because it does not depend on the state of the
            // component — it is shared by Renderer, gizmo and tests.
            static glm::mat4 viewFromWorld(const glm::mat4& world);

        private:
            ProjectionMode m_mode = ProjectionMode::Perspective;
            // Defaults to this repo's scale (50-unit primitives,
            // sandbox places the camera at z=300), not Unity's (0.3/1000).
            float m_fov              = 45.0f;
            float m_orthographicSize = 100.0f;
            float m_near             = 1.0f;
            float m_far              = 2000.0f;
    };
}
