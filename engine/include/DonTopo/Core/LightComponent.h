#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo
{
    // Luz de escena: el GameObject que la lleva aporta un point, spot,
    // directional o area a la iluminación del frame.
    //
    // NO guarda posición ni dirección: las dos salen del worldTransform del
    // GameObject dueño (posición = columna 3, dirección = -Z local), igual que
    // CameraComponent — mover o rotar el objeto mueve la luz. Sin invariante de
    // unicidad por escena: caben varias del mismo tipo, y Scene se queda con
    // las primeras MAX_LIGHTS en orden de escena.
    //
    // Data pura: sin Vulkan y sin conocer GameObject, misma regla que
    // ReflectionProbeComponent y CameraComponent (la dependencia va Core ->
    // resto). De UniformBufferObject.h solo usa el enum LightType, que es el
    // mismo valor que viaja en direction.w del UBO.
    //
    // Header-only a propósito: son escalares con clamp, y así no hace falta
    // añadir un .cpp a la lista de fuentes de DonTopoCore.
    class LightComponent
    {
        public:
            LightComponent() = default;

            LightType getType() const { return m_type; }
            void      setType(LightType t) { m_type = t; }

            // rgb sin premultiplicar por la intensidad: el shader multiplica.
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

            // Alcance del point/spot. La directional lo ignora (no atenúa) y el
            // area usa su ancho/2 como radio.
            float getRange() const { return m_range; }
            void  setRange(float r)
            {
                if (r < 0.01f)     r = 0.01f;
                if (r > 100000.0f) r = 100000.0f;
                m_range = r;
            }

            // Cono del spot, en GRADOS de semiángulo. Los clamps viven aquí (y
            // no en la UI) pa que un .scene editado a mano tampoco pueda
            // instalar un cono invertido: el interior nunca pasa del exterior.
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

            // Lado del rectángulo del area light. La aproximación del shader la
            // trata como un point de radio ancho/2, así que también hace de
            // alcance.
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
            // Defaults a la escala de este repo (primitivas de 50 unidades, la
            // cámara del sandbox a z=300), no a los de Unity.
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
