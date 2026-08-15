#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo {

    // Estado escalar de calidad y efectos del render: peso del ambiente y
    // parametros de bloom, SSAO, SSR, niebla, anti-aliasing y Forward+.
    //
    // Vive aparte porque NADA de esto depende de la API grafica: son valores
    // que viajan por push constant o por UBO y que los dos backends -Vulkan
    // (Renderer) y DirectX 12 (D3D12Renderer)- necesitan igual. Compartiendo
    // esta base, el menu de opciones de un juego exportado y los paneles del
    // editor hablan de los mismos get/set sea cual sea el backend, en vez de
    // duplicar los pares de metodos y sus valores por defecto.
    //
    // Aqui SOLO entra lo que se asigna y se lee. Cualquier setter que ademas
    // dispare trabajo -recrear targets, marcar recursos sucios, limpiar una
    // imagen- se queda en el backend, que es quien sabe que hay que rehacer.
    class RendererState {
        public:
            // Peso global del ambiente IBL. 1.0 = el entorno tal cual lo da el
            // cubemap. Viaja por el UBO, asi que cambia en el frame siguiente
            // sin recomputar nada.
            void  setAmbientIntensity(float v) { m_ambientIntensity = v; }
            float ambientIntensity() const     { return m_ambientIntensity; }
            // Interruptor global: apagarlo NO destruye el IBL precomputado, solo
            // manda 0 en el UBO. Se puede encender otra vez sin recomputar nada.
            void  setAmbientEnabled(bool v) { m_ambientEnabled = v; }
            bool  ambientEnabled() const    { return m_ambientEnabled; }

            // ── Bloom ────────────────────────────────────────────────────────
            // Bloom HDR. Los tres viajan por push constant de los pipelines del
            // bloom (NO por el UBO: ahi solo quedaban 2 floats y el bloque esta
            // declarado en 5 shaders), asi que cambian en el frame siguiente sin
            // recrear nada. intensity = 0 deja la imagen exactamente igual que
            // antes de la feature.
            // El interruptor, como los parámetros: el mismo panel enciende el
            // efecto en los dos backends. Vulkan añade su propia lógica al
            // apagarlo (suelta la cadena de imágenes), pero el valor vive aquí.
            bool  bloomEnabled() const        { return m_bloomEnabled; }
            void  setBloomEnabledFlag(bool v) { m_bloomEnabled = v; }
            void  setBloomThreshold(float v) { m_bloomThreshold = v; }
            float bloomThreshold() const     { return m_bloomThreshold; }
            void  setBloomKnee(float v)      { m_bloomKnee = v; }
            float bloomKnee() const          { return m_bloomKnee; }
            void  setBloomIntensity(float v) { m_bloomIntensity = v; }
            float bloomIntensity() const     { return m_bloomIntensity; }

            // ── SSAO ─────────────────────────────────────────────────────────
            // Los cuatro parametros viajan por push constant del pipeline del
            // SSAO (NO por el UBO: solo quedaban dos floats y el bloque esta
            // declarado en 5 shaders), asi que cambian en el frame siguiente
            // sin recrear nada.
            // El interruptor, como los parámetros: el mismo panel enciende el
            // efecto en los dos backends. Vulkan añade su propia lógica al
            // apagarlo (dejar el mapa en la identidad), pero el valor vive aquí.
            bool  ssaoEnabled() const        { return m_ssaoEnabled; }
            void  setSsaoEnabledFlag(bool v) { m_ssaoEnabled = v; }
            void  setSsaoRadius(float v)     { m_ssaoRadius = v; }
            float ssaoRadius() const         { return m_ssaoRadius; }
            void  setSsaoBias(float v)       { m_ssaoBias = v; }
            float ssaoBias() const           { return m_ssaoBias; }
            void  setSsaoIntensity(float v)  { m_ssaoIntensity = v; }
            float ssaoIntensity() const      { return m_ssaoIntensity; }
            void  setSsaoPower(float v)      { m_ssaoPower = v; }
            float ssaoPower() const          { return m_ssaoPower; }

            // ── SSR ──────────────────────────────────────────────────────────
            // SSR (reflejos en espacio de pantalla). Interruptor global; ademas
            // cada GameObject lleva su propia fuerza, y con el interruptor puesto
            // pero NINGUN objeto marcado tampoco se graba nada. Los parametros
            // viajan por push constant propia (SsrPush), no por el UBO.
            void  setSsrEnabled(bool v)      { m_ssrEnabled = v; }
            bool  ssrEnabled() const         { return m_ssrEnabled; }
            void  setSsrMaxDistance(float v) { m_ssrMaxDistance = v; }
            float ssrMaxDistance() const     { return m_ssrMaxDistance; }
            void  setSsrThickness(float v)   { m_ssrThickness = v; }
            float ssrThickness() const       { return m_ssrThickness; }
            void  setSsrMaxSteps(int v)      { m_ssrMaxSteps = v; }
            int   ssrMaxSteps() const        { return m_ssrMaxSteps; }
            void  setSsrEdgeFade(float v)    { m_ssrEdgeFade = v; }
            float ssrEdgeFade() const        { return m_ssrEdgeFade; }
            void  setSsrIntensity(float v)   { m_ssrIntensity = v; }
            float ssrIntensity() const       { return m_ssrIntensity; }

            // ── Niebla volumetrica ───────────────────────────────────────────
            // Niebla volumetrica: exponencial por altura con in-scattering de
            // la luz key. Interruptor global; apagado no graba ni un comando y
            // la imagen sale identica. Los parametros viajan por push constant
            // propia (FogPush), no por el UBO.
            void  setFogEnabled(bool v)         { m_fogEnabled = v; }
            bool  fogEnabled() const            { return m_fogEnabled; }
            void  setFogDensity(float v)        { m_fogDensity = v; }
            float fogDensity() const            { return m_fogDensity; }
            void  setFogHeightFalloff(float v)  { m_fogHeightFalloff = v; }
            float fogHeightFalloff() const      { return m_fogHeightFalloff; }
            void  setFogBaseHeight(float v)     { m_fogBaseHeight = v; }
            float fogBaseHeight() const         { return m_fogBaseHeight; }
            void  setFogScatter(const glm::vec3& v) { m_fogScatter = v; }
            const glm::vec3& fogScatter() const { return m_fogScatter; }
            void  setFogAnisotropy(float v)     { m_fogAnisotropy = v; }
            float fogAnisotropy() const         { return m_fogAnisotropy; }
            void  setFogSteps(int v)            { m_fogSteps = v; }
            int   fogSteps() const              { return m_fogSteps; }

            // ── Motion blur ──────────────────────────────────────────────────
            // Motion blur de camara por reproyeccion: la velocidad de cada pixel
            // sale del depth mas la matriz del frame anterior, la misma que ya
            // usa el TAA. Interruptor global; apagado no graba ni un dispatch y
            // la imagen sale identica. Los parametros viajan por push constant
            // propia (MotionBlurPush), no por el UBO.
            void  setMotionBlurEnabled(bool v)     { m_motionBlurEnabled = v; }
            bool  motionBlurEnabled() const        { return m_motionBlurEnabled; }
            void  setMotionBlurIntensity(float v)  { m_motionBlurIntensity = v; }
            float motionBlurIntensity() const      { return m_motionBlurIntensity; }
            void  setMotionBlurMaxRadius(float v)  { m_motionBlurMaxRadius = v; }
            float motionBlurMaxRadius() const      { return m_motionBlurMaxRadius; }
            void  setMotionBlurSamples(int v)      { m_motionBlurSamples = v; }
            int   motionBlurSamples() const        { return m_motionBlurSamples; }

            // ── Anti-aliasing ────────────────────────────────────────────────
            // Los parametros de cada modo viajan por push constant y surten
            // efecto en el frame siguiente sin recrear nada. El modo activo, en
            // cambio, lo elige el backend: cambiarlo puede exigir recrear
            // recursos.
            // FXAA
            void  setFxaaSubpix(float v)          { m_fxaaSubpix = v; }
            float fxaaSubpix() const              { return m_fxaaSubpix; }
            void  setFxaaEdgeThreshold(float v)   { m_fxaaEdgeThreshold = v; }
            float fxaaEdgeThreshold() const       { return m_fxaaEdgeThreshold; }
            void  setFxaaEdgeThresholdMin(float v){ m_fxaaEdgeThresholdMin = v; }
            float fxaaEdgeThresholdMin() const    { return m_fxaaEdgeThresholdMin; }
            // TAA: peso del historial (0 = solo el frame actual, sin acumulacion)
            // y amplitud del jitter de subpixel en pixeles.
            void  setTaaFeedback(float v)         { m_taaFeedback = v; }
            float taaFeedback() const             { return m_taaFeedback; }
            void  setTaaJitterScale(float v)      { m_taaJitterScale = v; }
            float taaJitterScale() const          { return m_taaJitterScale; }

            // ── Forward+ ─────────────────────────────────────────────────────
            // Culling de luces en GPU. Modos EXCLUYENTES. Off deja el frame
            // exactamente como antes de la feature: ni un dispatch, y pbr.frag
            // recorre las MAX_LIGHTS del UBO como siempre.
            // Anti-aliasing. El modo y las muestras de MSAA los pide el usuario
            // desde el mismo panel para los dos backends; lo que cada uno tenga
            // CONSTRUIDO ahora mismo (imágenes, pipelines) es cosa suya, porque
            // cambiarlo exige recrear recursos con la GPU en reposo.
            enum class AaMode : int
            {
                None = 0,
                Fxaa = 1,
                Ssaa = 2,
                Msaa = 3,
                Taa  = 4,
            };
            // Modo alambre: lo enciende el menú View del editor y vale para los
            // dos backends, así que el valor vive aquí.
            bool isWireframeMode() const        { return m_wireframeMode; }
            void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }

            AaMode aaMode() const            { return m_aaMode; }
            void   setAaModeFlag(AaMode m)   { m_aaMode = m; }
            int    msaaSamples() const       { return m_msaaSamples; }
            void   setMsaaSamplesFlag(int v) { m_msaaSamples = v; }

            enum class FpMode : int
            {
                Off       = 0,
                Tiled     = 1,  // rejilla 2D de tiles de 16x16 con el maximo de profundidad del tile
                Clustered = 2,  // rejilla 3D de 64x64 pixeles x 24 cortes logaritmicos en Z
            };
            // Cambia en el frame SIGUIENTE y no recrea nada: los dos modos
            // comparten buffers (dimensionados al mayor de las dos rejillas), asi
            // que lo unico que cambia es lo que se graba y el bloque de
            // parametros. El valor que manda durante un frame se congela en
            // m_fpActiveMode justo despues de la UI.
            void   setForwardPlusMode(FpMode mode) { m_fpMode = mode; }
            FpMode forwardPlusMode() const         { return m_fpMode; }
            // Radio por defecto de TODAS las luces que no traigan el suyo. Es el
            // dato que el culling necesita y que Light no lleva: meterlo en el
            // struct cambiaria el layout std140 del UBO, que declaran 5 shaders.
            void  setForwardPlusLightRadius(float v) { m_fpLightRadius = v; }
            float forwardPlusLightRadius() const     { return m_fpLightRadius; }

        protected:
            float                           m_ambientIntensity{1.0f};
            bool                            m_ambientEnabled{true};

            float                           m_bloomThreshold                    = 1.0f;
            float                           m_bloomKnee                         = 0.5f;
            float                           m_bloomIntensity                    = 0.05f;

            bool                            m_wireframeMode                     = false;
            AaMode                          m_aaMode                            = AaMode::None;
            int                             m_msaaSamples                       = 4;
            bool                            m_bloomEnabled                      = true;
            bool                            m_ssaoEnabled                       = false;
            float                           m_ssaoRadius                        = 0.5f;
            float                           m_ssaoBias                          = 0.025f;
            float                           m_ssaoIntensity                     = 1.0f;
            float                           m_ssaoPower                         = 1.0f;

            bool                            m_ssrEnabled                        = false;
            float                           m_ssrMaxDistance                    = 8.0f;
            float                           m_ssrThickness                      = 0.5f;
            int                             m_ssrMaxSteps                       = 32;
            float                           m_ssrEdgeFade                       = 0.1f;
            float                           m_ssrIntensity                      = 1.0f;

            bool                            m_motionBlurEnabled                 = false;
            float                           m_motionBlurIntensity               = 1.0f;
            float                           m_motionBlurMaxRadius               = 32.0f;
            int                             m_motionBlurSamples                 = 12;

            bool                            m_fogEnabled                        = false;
            float                           m_fogDensity                        = 0.02f;
            float                           m_fogHeightFalloff                  = 0.02f;
            float                           m_fogBaseHeight                     = 0.0f;
            glm::vec3                       m_fogScatter                        {0.6f, 0.7f, 0.9f};
            float                           m_fogAnisotropy                     = 0.6f;
            int                             m_fogSteps                          = 32;

            // Valores del preset de calidad de PC de FXAA 3.11.
            float                           m_fxaaSubpix                        = 0.75f;
            float                           m_fxaaEdgeThreshold                 = 0.166f;
            float                           m_fxaaEdgeThresholdMin              = 0.0833f;

            float                           m_taaFeedback                       = 0.9f;
            float                           m_taaJitterScale                    = 1.0f;

            FpMode                          m_fpMode                            = FpMode::Off;
            float                           m_fpLightRadius                     = 2000.0f;
    };
}
