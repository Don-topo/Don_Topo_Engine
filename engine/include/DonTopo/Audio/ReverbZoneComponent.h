#pragma once
#include <string>

namespace DonTopo
{
    // Sound ambience shaped like a sphere: inside it, everything that plays takes
    // the preset's reverb. It is Unity's Reverb Zone.
    //
    // Unlike the AudioClip, this component does NOT wrap any FMOD resource:
    // the live zone (an FMOD::Reverb3D) is created and destroyed by
    // AudioManager, paired by the GameObject id. This way the component
    // remains a handful of serializable data and can be copied, while
    // the native resource has a single owner.
    //
    // The mixing between overlapping zones is done by FMOD, not by us: that is why the
    // component only contributes radii and preset.
    //
    // There can be SEVERAL per scene, unlike the Audio Listener. FMOD has
    // a cap on simultaneous 3D instances and the manager warns when it is exceeded.
    class ReverbZoneComponent
    {
        public:
            // FMOD Core preset names (FMOD_PRESET_*), lowercase. They are
            // saved by NAME in the scene, never by index: adding a
            // preset to the list cannot change the ambience of an already
            // saved scene.
            //
            // The 20-odd from FMOD are not all here, only the ones actually requested;
            // extending the list is adding one line in AudioManager.
            const std::string& getPreset() const { return m_preset; }
            void setPreset(std::string preset) { m_preset = std::move(preset); }

            // Within minDistance the reverb is applied fully; between min and max
            // it fades out. Outside max there is no effect. Same scheme as the
            // attenuation of a 3D AudioClip, and the same two-sphere gizmo.
            float getMinDistance() const { return m_minDistance; }
            float getMaxDistance() const { return m_maxDistance; }
            void setMinDistance(float d);
            void setMaxDistance(float d);

            bool getEnabled() const { return m_enabled; }
            void setEnabled(bool e) { m_enabled = e; }

        private:
            std::string m_preset = "room";
            // Ranges at this repo's scale (50-unit primitives), like
            // the AudioClip ones.
            float m_minDistance = 50.0f;
            float m_maxDistance = 200.0f;
            bool  m_enabled = true;
    };
}
