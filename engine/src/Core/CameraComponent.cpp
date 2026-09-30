#include "DonTopo/Core/CameraComponent.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace DonTopo
{
    namespace
    {
        // Minimum separation between near and far (and absolute minimum of near):
        // glm::perspective diverges with near == 0 and with near == far.
        constexpr float kMinNear = 0.001f;
    }

    void CameraComponent::setFov(float degrees)
    {
        m_fov = glm::clamp(degrees, 1.0f, 179.0f);
    }

    void CameraComponent::setOrthographicSize(float size)
    {
        m_orthographicSize = std::max(size, kMinNear);
    }

    void CameraComponent::setNear(float n)
    {
        // It is clamped against the current far instead of pushing far: a setter must
        // not change the other field behind the caller's back. Careful when loading from
        // JSON: setFar has to be called BEFORE setNear (see Scene.cpp).
        m_near = glm::clamp(n, kMinNear, m_far - kMinNear);
    }

    void CameraComponent::setFar(float f)
    {
        m_far = std::max(f, m_near + kMinNear);
    }

    glm::mat4 CameraComponent::projectionMatrix(float aspect) const
    {
        // A degenerate aspect (viewport of width/height 0 when minimizing the window)
        // would put NaN in the matrix; 1.0 is a harmless fallback for that frame.
        if (!(aspect > 0.0f))
            aspect = 1.0f;

        // *_ZO (zero-to-one) and not *_NO (glm default without
        // GLM_FORCE_DEPTH_ZERO_TO_ONE): Vulkan clips 0 <= z_clip <= w_clip,
        // but glm's NO convention is meant for OpenGL and sends near to
        // z_ndc=-1. In orthographic (w=1) that directly cuts off the near half
        // of the whole near/far range, not just a thin margin as in
        // perspective. Same criterion as the shadow matrix in Renderer.cpp.
        glm::mat4 proj;
        if (m_mode == ProjectionMode::Orthographic)
        {
            const float halfHeight = m_orthographicSize;
            const float halfWidth  = halfHeight * aspect;
            proj = glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, m_near, m_far);
        }
        else
        {
            proj = glm::perspectiveRH_ZO(glm::radians(m_fov), aspect, m_near, m_far);
        }
        proj[1][1] *= -1.0f; // Vulkan Y flip
        return proj;
    }

    glm::mat4 CameraComponent::viewFromWorld(const glm::mat4& world)
    {
        const glm::vec3 right   = glm::vec3(world[0]);
        const glm::vec3 up      = glm::vec3(world[1]);
        const glm::vec3 forward = glm::vec3(world[2]);

        // Degenerate basis (some axis with scale 0): inverting would give NaN and
        // would dirty the whole frame. The identity at least lets you see something.
        if (glm::length(right) < 1e-6f || glm::length(up) < 1e-6f || glm::length(forward) < 1e-6f)
            return glm::mat4(1.0f);

        glm::mat4 unscaled(1.0f);
        unscaled[0] = glm::vec4(glm::normalize(right), 0.0f);
        unscaled[1] = glm::vec4(glm::normalize(up), 0.0f);
        unscaled[2] = glm::vec4(glm::normalize(forward), 0.0f);
        unscaled[3] = world[3]; // position intact
        return glm::inverse(unscaled);
    }
}
