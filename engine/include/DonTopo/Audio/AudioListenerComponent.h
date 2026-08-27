#pragma once

namespace DonTopo
{
    // Oído de la escena: el GameObject que lo lleva marca desde dónde se oye el
    // audio 3D. Como mucho UNO por escena — el invariante lo imponen
    // Scene::findAudioListener (gana el primero en pre-orden) y el gate del menú
    // "Add" de Properties, no esta clase.
    //
    // NO guarda posición ni orientación: las dos salen del worldTransform del
    // GameObject dueño (posición = columna 3, forward = -Z local, up = +Y
    // local), igual que CameraComponent y LightComponent — mover o rotar el
    // objeto mueve el listener.
    //
    // Sin listener en la escena los AudioClip suenan IGUAL: el audio 3D se oye
    // entonces desde la cámara, fallback que resuelven las rutas de host cada
    // frame (sandbox/src/main.cpp, runtime/main.cpp), y el Log lo dice una vez
    // al entrar en Play. No hay ningún gate que impida reproducir: lo hubo, pero
    // solo cubría playOnAwake y ni AudioClip:Play de Lua ni el botón Play del
    // inspector lo consultaban. Dentro de AudioManager/AudioClipComponent no
    // puede vivir (esas dos se prueban sin escena), así que imponerlo obligaría
    // a repetirlo en las cuatro rutas de reproducción.
    //
    // Header-only a propósito: solo lleva un bool, y así no hace falta añadir un
    // .cpp a la lista de fuentes de DonTopoCore.
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
