#pragma once

namespace DonTopo
{
    // Un SliderFloat cuyo valor NO se escribe en el dato mientras se arrastra:
    // se enseña, se previsualiza y se entrega entero al soltar. Es el caso de
    // Metallic/Roughness del Material, que durante el arrastre solo empujan a la
    // GPU y escriben el Material (con su comando de undo) una sola vez al final.
    //
    // Existe porque el patron de siempre -una local inicializada desde el dato,
    // el widget encima, y el commit en IsItemDeactivatedAfterEdit leyendo esa
    // local- NO sirve cuando el dato no se escribe en vivo. El frame en que se
    // suelta el raton, SliderBehaviorT solo llama a ClearActiveID() y no toca el
    // valor, asi que la local vale lo que decia el dato ANTES del arrastre: el
    // commit veia "no ha cambiado nada" y no pasaba nada, mientras la GPU se
    // quedaba con lo ultimo que se le empujo. El objeto se veia bien y el
    // slider, el undo y el .scene seguian en el valor viejo.
    //
    // Los sliders que SI escriben en vivo (Audio Clip, los ajustes de render)
    // no lo necesitan: para ellos el dato ya lleva el valor al soltar.
    //
    // El valor pendiente vive aqui, entre frames, igual que m_ssaaPendingFactor
    // en RenderingPanel. Una sola instancia vale para varios sliders: solo un
    // widget de ImGui puede estar activo a la vez, y el id dice cual.
    //
    // Sin ImGui en el header, como RenderSettingControls: el cuerpo va al .cpp.
    class DeferredSliderFloat
    {
        public:
            struct Result
            {
                bool  activated = false;   // se empezo a arrastrar este frame
                bool  active    = false;   // se esta arrastrando (value = bajo el cursor)
                bool  committed = false;   // se solto tras editar: value es el final
                bool  cancelled = false;   // se solto sin editar
                float begin     = 0.0f;    // el dato ANTES del clic (no el del salto)
                float value     = 0.0f;    // lo que enseña el widget este frame
            };

            // `current` es el valor del dato, leido antes de dibujar: SliderFloat
            // salta al valor bajo el cursor en el mismo frame del clic, y `begin`
            // tiene que ser el de antes de ese salto.
            Result draw(const char* label, float current, float lo, float hi, const char* fmt);

        private:
            unsigned int m_activeId        = 0;
            int          m_lastActiveFrame = -1;
            float        m_begin           = 0.0f;
            float        m_pending         = 0.0f;
    };
}
