#pragma once
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"

#include <glm/glm.hpp>
#include <functional>

namespace DonTopo
{
    // Los widgets de los ajustes de render, cada uno con su undo y su guardado.
    //
    // Un ajuste de render bien hecho tiene que hacer CUATRO cosas: dibujar el
    // widget, aplicar el valor, registrar el comando de deshacer y persistir en
    // el project.json. Cada control repetia las tres primeras y ninguno hacia la
    // cuarta (H49), y de ahi salieron estos wrappers.
    //
    // Vivian dentro de EditorUI, con su estado de arrastre entre los miembros de
    // la clase. Salen aqui con el panel de Rendering (H58): son suyos y no del
    // editor entero, y asi el panel no necesita conocer a EditorUI para nada.
    //
    // Van por std::function y no por template: asi el cuerpo se queda en el .cpp
    // y este header no arrastra ImGui a todo el que lo incluya. Son 41 llamadas
    // dentro de un panel que solo se dibuja estando abierto, o sea que el coste
    // de la indireccion no se mide.
    class RenderSettingControls
    {
        public:
            RenderSettingControls() = default;

            // El UndoManager y el callback de guardado llegan por el
            // EditorContext, que se recibe en cada draw(). Se refrescan aqui en
            // vez de en el constructor porque el ESTADO DE ARRASTRE de abajo
            // tiene que sobrevivir entre frames: reconstruir el objeto cada
            // frame perderia el valor de inicio del arrastre, y deshacer
            // devolveria al penultimo pixel en vez de a donde se empezo.
            //
            // `persist` guarda los ajustes del proyecto: se llama al aplicar y
            // tambien desde el comando de deshacer, para que deshacer un ajuste
            // deje el project.json como estaba.
            void bind(UndoManager* undo, std::function<void()> persist)
            {
                m_undo    = undo;
                m_persist = std::move(persist);
            }

            // Los de ARRASTRE registran el valor del INICIO del arrastre, no el
            // del frame en que se suelta: si no, deshacer un arrastre largo
            // devolveria al penultimo pixel. Y el previo se lee ANTES de dibujar
            // porque SliderFloat ya salta en el mismo frame del click.
            void sliderFloat(const char* label, float lo, float hi, const char* fmt,
                             const std::function<float()>& get,
                             const std::function<void(float)>& set);
            void sliderInt(const char* label, int lo, int hi,
                           const std::function<int()>& get,
                           const std::function<void(int)>& set);
            // Devuelve el valor VIGENTE tras el click, que es lo que los
            // llamantes usan acto seguido para el BeginDisabled de los sliders
            // de su efecto.
            bool checkbox(const char* label,
                          const std::function<bool()>& get,
                          const std::function<void(bool)>& set);
            void colorEdit3(const char* label,
                            const std::function<glm::vec3()>& get,
                            const std::function<void(const glm::vec3&)>& set);

            // Para los controles con forma propia (los Combo, los RadioButton
            // del MSAA, el slider del SSAA y el boton Wireframe): el llamante
            // dibuja y aplica, esto solo registra. No hace nada si before ==
            // after.
            template <typename T>
            void pushUndo(const char* label, const T& before, const T& after,
                          std::function<void(const T&)> set)
            {
                if (before == after || !m_undo) return;
                // dirtiesScene = false: estos ajustes viven en el project.json,
                // no en la escena. Marcarla sucia sacaria el modal de cambios
                // sin guardar por haber movido un slider de bloom.
                m_undo->push(makeRenderSettingCommand<T>(label, before, after, std::move(set),
                                                         m_persist),
                             /*dirtiesScene=*/false);
                if (m_persist) m_persist();
            }

        private:
            UndoManager*          m_undo = nullptr;
            std::function<void()> m_persist;

            // Estado del arrastre en curso. El id distingue QUE widget se esta
            // arrastrando: sin el, empezar a arrastrar un segundo slider sin
            // soltar el primero mezclaria los valores de inicio.
            //
            // ColorEdit3 no propaga IsItemActivated al grupo, asi que el
            // arranque se detecta por FLANCO de IsItemActive y no por el evento.
            unsigned int m_activeId    = 0;
            float        m_beginScalar = 0.0f;
            int          m_beginInt    = 0;
            glm::vec3    m_beginColor{0.0f};
    };
}
