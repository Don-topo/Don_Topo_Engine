#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo
{
    // Scene light: the GameObject that carries it contributes a point, spot,
    // directional or area light to the frame's lighting.
    //
    // It does NOT store position or direction: both come from the worldTransform of the
    // owner GameObject (position = column 3, direction = local -Z), just like
    // CameraComponent — moving or rotating the object moves the light. No
    // per-scene uniqueness invariant: several of the same type fit, and Scene keeps the
    // first MAX_LIGHTS in scene order.
    //
    // Pure data: no Vulkan and no knowledge of GameObject, same rule as
    // ReflectionProbeComponent and CameraComponent (the dependency goes Core ->
    // the rest). From UniformBufferObject.h it only uses the LightType enum, which is the
    // same value that travels in direction.w of the UBO.
    //
    // Header-only on purpose: they are scalars with clamp, and this way there is no need to
    // add a .cpp to the DonTopoCore source list.
    class LightComponent
    {
        public:
            LightComponent() = default;

            LightType getType() const { return m_type; }
            void      setType(LightType t) { m_type = t; }

            // rgb not premultiplied by the intensity: the shader multiplies.
            const glm::vec3& getColor() const { return m_color; }
            void setColor(const glm::vec3& c)
            {
                m_color = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
            }

            float getIntensity() const { return m_intensity; }
            void  setIntensity(float i)
            {
                if (i < 0.0f)   i = 0.0f;
                if (i > 100.0f) i = 100.0f;
                m_intensity = i;
            }

            // Range of the point/spot. The directional ignores it (no attenuation) and the
            // area uses its width/2 as the radius.
            float getRange() const { return m_range; }
            void  setRange(float r)
            {
                if (r < 0.01f)     r = 0.01f;
                if (r > 100000.0f) r = 100000.0f;
                m_range = r;
            }

            // Spot cone, in DEGREES of half-angle. The clamps live here (and
            // not in the UI) so that a hand-edited .scene cannot install an
            // inverted cone either: the inner one never exceeds the outer one.
            float getInnerAngle() const { return m_innerAngle; }
            void  setInnerAngle(float deg)
            {
                if (deg < 0.0f)  deg = 0.0f;
                if (deg > 89.9f) deg = 89.9f;
                m_innerAngle = deg;
                if (m_outerAngle < m_innerAngle) m_outerAngle = m_innerAngle;
            }

            float getOuterAngle() const { return m_outerAngle; }
            void  setOuterAngle(float deg)
            {
                if (deg < 0.0f)  deg = 0.0f;
                if (deg > 89.9f) deg = 89.9f;
                m_outerAngle = deg;
                if (m_innerAngle > m_outerAngle) m_innerAngle = m_outerAngle;
            }

            // Side of the area light rectangle. The shader approximation
            // treats it as a point of radius width/2, so it also acts as
            // range.
            float getAreaWidth() const { return m_areaWidth; }
            void  setAreaWidth(float w)
            {
                if (w < 0.01f)     w = 0.01f;
                if (w > 100000.0f) w = 100000.0f;
                m_areaWidth = w;
            }

            float getAreaHeight() const { return m_areaHeight; }
            void  setAreaHeight(float h)
            {
                if (h < 0.01f)     h = 0.01f;
                if (h > 100000.0f) h = 100000.0f;
                m_areaHeight = h;
            }

        private:
            // Defaults to this repo's scale (50-unit primitives, the
            // sandbox camera at z=300), not Unity's.
            LightType m_type       = LightType::Point;
            glm::vec3 m_color      {1.0f, 1.0f, 1.0f};
            float     m_intensity  = 1.0f;
            float     m_range      = 300.0f;
            float     m_innerAngle = 20.0f;
            float     m_outerAngle = 30.0f;
            float     m_areaWidth  = 100.0f;
            float     m_areaHeight = 100.0f;
    };
}
