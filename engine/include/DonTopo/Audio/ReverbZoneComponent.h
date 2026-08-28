#pragma once
#include <string>

namespace DonTopo
{
    // Ambiente sonoro con forma de esfera: dentro de él, todo lo que suene coge
    // la reverberación del preset. Es la Reverb Zone de Unity.
    //
    // Al contrario que el AudioClip, este componente NO envuelve ningún recurso
    // de FMOD: la zona viva (un FMOD::Reverb3D) la crea y la destruye
    // AudioManager, emparejándola por el id del GameObject. Así el componente
    // sigue siendo un puñado de datos serializables y se puede copiar, mientras
    // que el recurso nativo tiene un único dueño.
    //
    // La mezcla entre zonas solapadas la hace FMOD, no nosotros: por eso el
    // componente solo aporta radios y preset.
    //
    // Puede haber VARIAS por escena, a diferencia del Audio Listener. FMOD tiene
    // un tope de instancias 3D simultáneas y el manager avisa cuando se pasa.
    class ReverbZoneComponent
    {
        public:
            // Nombres de preset de FMOD Core (FMOD_PRESET_*), en minúsculas. Se
            // guardan por NOMBRE en la escena, nunca por índice: añadir un
            // preset a la lista no puede cambiar el ambiente de una escena ya
            // guardada.
            //
            // No están los 20 y pico de FMOD, solo los que se piden de verdad;
            // ampliar la lista es añadir una línea en AudioManager.
            const std::string& getPreset() const { return m_preset; }
            void setPreset(std::string preset) { m_preset = std::move(preset); }

            // Dentro de minDistance la reverb se aplica a tope; entre min y max
            // se desvanece. Fuera de max no hay efecto. Mismo esquema que la
            // atenuación de un AudioClip 3D, y mismo gizmo de dos esferas.
            float getMinDistance() const { return m_minDistance; }
            float getMaxDistance() const { return m_maxDistance; }
            void setMinDistance(float d);
            void setMaxDistance(float d);

            bool getEnabled() const { return m_enabled; }
            void setEnabled(bool e) { m_enabled = e; }

        private:
            std::string m_preset = "room";
            // Rangos a la escala de este repo (primitivas de 50 unidades), como
            // los del AudioClip.
            float m_minDistance = 50.0f;
            float m_maxDistance = 200.0f;
            bool  m_enabled = true;
    };
}
