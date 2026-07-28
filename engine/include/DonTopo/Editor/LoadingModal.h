#pragma once

namespace DonTopo
{
    // Overlay de progreso para las cargas de escena. NO congela la ventana: la
    // aplicación sigue pintando frames, así que Windows nunca la marca como "no
    // responde". Lo que veta es la edición, no el render.
    class LoadingModal
    {
        public:
            void begin(int total);
            void update(int pending);
            bool active() const { return m_active; }

            // Dibuja el overlay. Devuelve true si el usuario pulsó Cancel.
            bool draw();

        private:
            bool m_active = false;
            int  m_total  = 0;
            int  m_done   = 0;
    };
}
