#pragma once

namespace DonTopo
{
    // Reflection Probe: a probe that captures the environment from the position of its
    // GameObject and replaces the global IBL (irradiance + prefilter) on the
    // objects that fall inside its radius of influence.
    //
    // It does NOT store position: it is given by the worldTransform of the owner GameObject, just
    // like CameraComponent — moving the object moves the probe. It does not store the
    // cubemap either: that lives in the Renderer (GPU resource) and is rebuilt with a
    // bake, which is an EVENT and never a frame pass.
    //
    // Pure data: no Vulkan and no knowledge of GameObject, same rule as
    // CameraComponent and Rigidbody (the dependency goes Core -> the rest).
    //
    // Header-only on purpose: they are two floats with clamp, and this way there is no need to
    // add a .cpp to the DonTopoCore source list.
    class ReflectionProbeComponent
    {
        public:
            ReflectionProbeComponent() = default;

            // The clamps live here (and not in the UI) so that a hand-edited .scene
            // cannot install a degenerate probe either.
            float getRadius() const { return m_radius; }
            void  setRadius(float r)
            {
                if (r < 1.0f)      r = 1.0f;
                if (r > 100000.0f) r = 100000.0f;
                m_radius = r;
            }

            // Weight of the captured environment. It is baked into the cubemap itself during
            // the bake (push constant of the two convolution .comp files), it does not arrive
            // through the UBO block: that one is declared by 5 shaders and std140 would silently shift
            // everything that comes after it.
            float getIntensity() const { return m_intensity; }
            void  setIntensity(float i)
            {
                if (i < 0.0f) i = 0.0f;
                if (i > 8.0f) i = 8.0f;
                m_intensity = i;
            }

        private:
            // Defaults to this repo's scale (50-unit primitives, the
            // sandbox camera at z=300), not Unity's.
            float m_radius    = 300.0f;
            float m_intensity = 1.0f;
    };
}
