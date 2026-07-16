#include "DonTopo/Core/CameraComponent.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace DonTopo
{
    namespace
    {
        // Separación mínima entre near y far (y mínimo absoluto de near):
        // glm::perspective diverge con near == 0 y con near == far.
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
        // Se clampa contra el far actual en vez de empujar far: un setter no
        // debe cambiar el otro campo a espaldas del caller. Ojo al cargar de
        // JSON: hay que llamar a setFar ANTES que a setNear (ver Scene.cpp).
        m_near = glm::clamp(n, kMinNear, m_far - kMinNear);
    }

    void CameraComponent::setFar(float f)
    {
        m_far = std::max(f, m_near + kMinNear);
    }

    glm::mat4 CameraComponent::projectionMatrix(float aspect) const
    {
        // Aspect degenerado (viewport de ancho/alto 0 al minimizar la ventana)
        // metería NaN en la matriz; 1.0 es un repliegue inocuo pa ese frame.
        if (!(aspect > 0.0f))
            aspect = 1.0f;

        glm::mat4 proj;
        if (m_mode == ProjectionMode::Orthographic)
        {
            const float halfHeight = m_orthographicSize;
            const float halfWidth  = halfHeight * aspect;
            proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, m_near, m_far);
        }
        else
        {
            proj = glm::perspective(glm::radians(m_fov), aspect, m_near, m_far);
        }
        proj[1][1] *= -1.0f; // Vulkan Y flip
        return proj;
    }

    glm::mat4 CameraComponent::viewFromWorld(const glm::mat4& world)
    {
        const glm::vec3 right   = glm::vec3(world[0]);
        const glm::vec3 up      = glm::vec3(world[1]);
        const glm::vec3 forward = glm::vec3(world[2]);

        // Base degenerada (algún eje con escala 0): invertir daría NaN y
        // ensuciaría todo el frame. La identidad al menos deja ver algo.
        if (glm::length(right) < 1e-6f || glm::length(up) < 1e-6f || glm::length(forward) < 1e-6f)
            return glm::mat4(1.0f);

        glm::mat4 unscaled(1.0f);
        unscaled[0] = glm::vec4(glm::normalize(right), 0.0f);
        unscaled[1] = glm::vec4(glm::normalize(up), 0.0f);
        unscaled[2] = glm::vec4(glm::normalize(forward), 0.0f);
        unscaled[3] = world[3]; // posición intacta
        return glm::inverse(unscaled);
    }
}
