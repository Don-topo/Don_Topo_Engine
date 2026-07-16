#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    // Componente de cámara de juego (equivalente a Unity Camera). NO guarda
    // posición ni orientación: las da el worldTransform del GameObject dueño,
    // así que mover el objeto mueve la cámara. Tampoco guarda aspect ratio —
    // lo dicta el viewport (Renderer::viewportAspect), pa que redimensionar la
    // ventana no deforme la imagen (no hay letterboxing hoy).
    //
    // Data pura: sin Vulkan y sin conocer GameObject, misma regla que Rigidbody
    // (la dependencia va Core -> resto, nunca al revés).
    //
    // El componente construye sus propias matrices a propósito: el Renderer (en
    // Play) y el gizmo de frustum (en edición) tienen que coincidir, o el
    // wireframe dibujado mentiría sobre lo que se ve al dar a Play.
    class CameraComponent
    {
        public:
            enum class ProjectionMode { Perspective, Orthographic };

            CameraComponent() = default;

            ProjectionMode getMode() const { return m_mode; }
            void setMode(ProjectionMode mode) { m_mode = mode; }

            // Los clamps viven aquí (y no en la UI) pa que un .scene editado a
            // mano tampoco pueda instalar una proyección degenerada.
            float getFov() const { return m_fov; }              // grados, solo perspectiva
            void  setFov(float degrees);
            float getOrthographicSize() const { return m_orthographicSize; } // semi-altura mundo, solo ortográfica
            void  setOrthographicSize(float size);
            float getNear() const { return m_near; }
            void  setNear(float n);
            float getFar() const { return m_far; }
            void  setFar(float f);

            // Proyección pa el aspect dado, con el Y-flip de Vulkan YA aplicado
            // (proj[1][1] *= -1): sus dos consumidores lo necesitan, y dejárselo
            // al caller invitaba a que uno de los dos se lo olvidara.
            glm::mat4 projectionMatrix(float aspect) const;

            // View de una cámara colocada en world, mirando a -Z local (misma
            // convención que glm::lookAt y que DonTopo::Camera, cuyo yaw por
            // defecto de -90° da front = (0,0,-1)). Normaliza los ejes antes de
            // invertir: un GameObject escalado hornearía su escala en la view y
            // deformaría la imagen. static porque no depende del estado del
            // componente — la comparten Renderer, gizmo y tests.
            static glm::mat4 viewFromWorld(const glm::mat4& world);

        private:
            ProjectionMode m_mode = ProjectionMode::Perspective;
            // Defaults a la escala de este repo (primitivas de 50 unidades,
            // sandbox coloca la cámara a z=300), no a los de Unity (0.3/1000).
            float m_fov              = 45.0f;
            float m_orthographicSize = 100.0f;
            float m_near             = 1.0f;
            float m_far              = 2000.0f;
    };
}
