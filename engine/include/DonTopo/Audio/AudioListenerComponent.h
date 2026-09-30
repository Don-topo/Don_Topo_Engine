#pragma once

namespace DonTopo
{
    // Ear of the scene: the GameObject that carries it marks from where 3D
    // audio is heard. At most ONE per scene — the invariant is enforced by
    // Scene::findAudioListener (the first in pre-order wins) and by the gate of the
    // Properties "Add" menu, not by this class.
    //
    // It does NOT store position or orientation: both come from the worldTransform of the
    // owner GameObject (position = column 3, forward = local -Z, up = local
    // +Y), just like CameraComponent and LightComponent — moving or rotating the
    // object moves the listener.
    //
    // Without a listener in the scene the AudioClips play ALL THE SAME: 3D audio is then heard
    // from the camera, a fallback that the host paths resolve every
    // frame (sandbox/src/main.cpp, runtime/main.cpp), and the Log says so once
    // on entering Play. There is no gate that prevents playing: there was one, but
    // it only covered playOnAwake and neither Lua's AudioClip:Play nor the inspector's
    // Play button consulted it. It cannot live inside AudioManager/AudioClipComponent
    // (those two are tested without a scene), so enforcing it would force repeating it
    // in the four playback paths.
    //
    // Header-only on purpose: it only carries a bool, and this way there is no need to add a
    // .cpp to the DonTopoCore source list.
    class AudioListenerComponent
    {
        public:
            AudioListenerComponent() = default;

            bool getEnabled() const { return m_enabled; }
            void setEnabled(bool e) { m_enabled = e; }

        private:
            bool m_enabled = true;
    };
}
