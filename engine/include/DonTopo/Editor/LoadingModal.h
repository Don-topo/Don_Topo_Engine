#pragma once

namespace DonTopo
{
    // Progress overlay for scene loads. It does NOT freeze the window: the
    // application keeps drawing frames, so Windows never flags it as "not
    // responding". What it vetoes is editing, not rendering.
    class LoadingModal
    {
        public:
            void begin(int total);
            void update(int pending);
            bool active() const { return m_active; }

            // Draws the overlay. Returns true if the user pressed Cancel.
            bool draw();

        private:
            bool m_active = false;
            int  m_total  = 0;
            int  m_done   = 0;
    };
}
