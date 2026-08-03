#pragma once

namespace DonTopo
{
    // Reflection Probe: sonda que captura el entorno desde la posición de su
    // GameObject y sustituye al IBL global (irradiancia + prefiltrado) en los
    // objetos que caen dentro de su radio de influencia.
    //
    // NO guarda posición: la da el worldTransform del GameObject dueño, igual
    // que CameraComponent — mover el objeto mueve la sonda. Tampoco guarda el
    // cubemap: ése vive en el Renderer (recurso GPU) y se reconstruye con un
    // bake, que es un EVENTO y nunca un pass del frame.
    //
    // Data pura: sin Vulkan y sin conocer GameObject, misma regla que
    // CameraComponent y Rigidbody (la dependencia va Core -> resto).
    //
    // Header-only a propósito: son dos floats con clamp, y así no hace falta
    // añadir un .cpp a la lista de fuentes de DonTopoCore.
    class ReflectionProbeComponent
    {
        public:
            ReflectionProbeComponent() = default;

            // Los clamps viven aquí (y no en la UI) pa que un .scene editado a
            // mano tampoco pueda instalar una sonda degenerada.
            float getRadius() const { return m_radius; }
            void  setRadius(float r)
            {
                if (r < 1.0f)      r = 1.0f;
                if (r > 100000.0f) r = 100000.0f;
                m_radius = r;
            }

            // Peso del entorno capturado. Se hornea en el propio cubemap durante
            // el bake (push constant de los dos .comp de convolución), no llega
            // por el bloque UBO: ése lo declaran 5 shaders y std140 desplazaría
            // en silencio todo lo que va detrás.
            float getIntensity() const { return m_intensity; }
            void  setIntensity(float i)
            {
                if (i < 0.0f) i = 0.0f;
                if (i > 8.0f) i = 8.0f;
                m_intensity = i;
            }

        private:
            // Defaults a la escala de este repo (primitivas de 50 unidades, la
            // cámara del sandbox a z=300), no a los de Unity.
            float m_radius    = 300.0f;
            float m_intensity = 1.0f;
    };
}
