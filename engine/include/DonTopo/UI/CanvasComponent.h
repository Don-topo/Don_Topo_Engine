#pragma once
#include "DonTopo/UI/UiCanvas.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace DonTopo
{
    // Dónde se dibuja el canvas. ScreenSpace es lo de siempre: una ortográfica en
    // píxeles de salida, encima de todo. World lo coloca EN LA ESCENA, con la
    // perspectiva de la cámara y tapado por la geometría que tenga delante.
    enum class UiCanvasRenderMode { ScreenSpace, World };

    // Cómo se orienta un canvas de mundo respecto a la cámara. YawOnly gira solo
    // alrededor de la vertical del mundo: es lo que quiere una barra de vida, que no
    // debe tumbarse al mirar desde arriba. Full lo encara del todo, que es lo que
    // quiere un icono.
    enum class UiBillboard { None, YawOnly, Full };

    // Los ajustes de RESOLUCIÓN de la UI 2D como componente de GameObject: un
    // GameObject con Canvas es el único sitio del que cuelga la UI. No guarda el
    // árbol de widgets ni un UiCanvas propio — el canvas vivo lo sigue teniendo
    // el Renderer (Renderer::uiCanvas()), y quien dibuja copia estos campos ahí
    // cada frame con applyTo, igual que las luces se recolectan cada frame. Así
    // lo que se ve en Play y en el juego exportado sale de la ESCENA y no de un
    // canvas cableado a mano.
    //
    // Los nombres, los defaults y el significado son EXACTAMENTE los de UiCanvas:
    // este componente no interpreta ni clampa nada (de eso ya se encarga UiCanvas
    // al resolver el área útil). Campos públicos por lo mismo: es el mismo POD.
    class CanvasComponent
    {
        public:
            UiScaleMode   scaleMode           = UiScaleMode::ConstantPixelSize;
            float         scaleFactor         = 1.0f;               // multiplica a los tres modos
            glm::vec2     referenceResolution{1920.0f, 1080.0f};    // ScaleWithScreenSize
            UiScreenMatch screenMatch         = UiScreenMatch::MatchWidthOrHeight;
            float         matchWidthOrHeight  = 0.5f;               // 0 = ancho, 1 = alto
            float         screenDpi           = 0.0f;               // 0 = desconocido
            float         fallbackDpi         = 96.0f;              // el que se usa si no se sabe
            float         referenceDpi        = 96.0f;              // ConstantPhysicalSize
            UiSafeArea    safeArea{};                               // en píxeles reales
            float         aspectRatio         = 0.0f;               // 0 = apagado

            // --- Modo de dibujado ---------------------------------------------
            UiCanvasRenderMode renderMode = UiCanvasRenderMode::ScreenSpace;

            // --- Solo World ----------------------------------------------------
            // En modo World el área útil es EXACTAMENTE referenceResolution: no
            // hay pantalla a la que ajustarse, así que scaleMode, screenMatch,
            // matchWidthOrHeight, los tres DPI, safeArea y aspectRatio NO SE
            // LEEN. No se esconden en el editor: se documenta el matiz.
            float       worldScale = 0.001f;   // unidades de mundo por PÍXEL de canvas
            UiBillboard billboard  = UiBillboard::None;
            // A false el canvas se dibuja siempre encima, atravesando paredes: es
            // lo que quiere una barra de vida que no debe perderse de vista.
            bool        depthTest  = true;

            // Vuelca los campos en el canvas vivo. No toca ni el árbol ni la
            // visibilidad: solo la resolución.
            void applyTo(UiCanvas& canvas) const
            {
                if (renderMode == UiCanvasRenderMode::World)
                {
                    // Un canvas de mundo no se ajusta a ninguna pantalla: su área
                    // útil es su resolución de referencia y punto. Volcar aquí el
                    // scaleMode o el safe area haría que el cartel cambiara de
                    // tamaño al redimensionar la ventana, que es justo lo que un
                    // objeto del mundo NO debe hacer.
                    canvas.scaleMode           = UiScaleMode::ConstantPixelSize;
                    canvas.scaleFactor         = 1.0f;
                    canvas.referenceResolution = referenceResolution;
                    canvas.screenMatch         = UiScreenMatch::MatchWidthOrHeight;
                    canvas.matchWidthOrHeight  = 0.5f;
                    canvas.screenDpi           = 0.0f;
                    canvas.fallbackDpi         = 96.0f;
                    canvas.referenceDpi        = 96.0f;
                    canvas.safeArea            = UiSafeArea{};
                    canvas.aspectRatio         = 0.0f;
                    return;
                }

                canvas.scaleMode           = scaleMode;
                canvas.scaleFactor         = scaleFactor;
                canvas.referenceResolution = referenceResolution;
                canvas.screenMatch         = screenMatch;
                canvas.matchWidthOrHeight  = matchWidthOrHeight;
                canvas.screenDpi           = screenDpi;
                canvas.fallbackDpi         = fallbackDpi;
                canvas.referenceDpi        = referenceDpi;
                canvas.safeArea            = safeArea;
                canvas.aspectRatio         = aspectRatio;
            }
    };

    // Matriz de MODELO de un canvas de mundo: de píxeles del canvas a unidades
    // del mundo. Función libre y no método a propósito — necesita la vista de la
    // cámara para el billboard, y el componente no tiene por qué saber de
    // cámaras. Aquí y no en el Renderer para poder probarla sin GPU.
    //
    // El canvas crece hacia ABAJO y el mundo hacia ARRIBA, así que la Y va
    // NEGADA. Y el canvas se centra en el objeto: su píxel (w/2, h/2) cae
    // exactamente en la posición del GameObject.
    inline glm::mat4 uiWorldCanvasMatrix(const CanvasComponent& c, glm::vec2 canvasSize,
                                         const glm::mat4& worldTransform, const glm::mat4& view)
    {
        const float s = c.worldScale;

        // Base del objeto: su transform, o una que mire a la cámara si hay
        // billboard. La POSICIÓN siempre sale del transform; lo que el billboard
        // sustituye es la rotación (y con ella la escala del objeto, que en un
        // canvas encarado no significa nada).
        glm::mat4 base = worldTransform;
        if (c.billboard != UiBillboard::None)
        {
            const glm::vec3 pos = glm::vec3(worldTransform[3]);

            // Los ejes de la CÁMARA salen de la inversa de la vista: las filas de
            // la parte rotacional de `view` son sus ejes en el mundo.
            const glm::vec3 camDerecha = glm::vec3(view[0][0], view[1][0], view[2][0]);
            const glm::vec3 camArriba  = glm::vec3(view[0][1], view[1][1], view[2][1]);
            const glm::vec3 camAtras   = glm::vec3(view[0][2], view[1][2], view[2][2]);

            glm::vec3 derecha, arriba, adelante;
            if (c.billboard == UiBillboard::Full)
            {
                derecha  = camDerecha;
                arriba   = camArriba;
                adelante = camAtras;
            }
            else   // YawOnly: gira solo alrededor de la vertical del MUNDO
            {
                arriba = glm::vec3(0.0f, 1.0f, 0.0f);
                // Proyectar el "hacia atrás" de la cámara sobre el plano
                // horizontal. Mirando en vertical justa el vector se anula: en ese
                // caso vale cualquier orientación, y se coge una fija en vez de
                // normalizar un cero (que daría NaN y borraría el canvas entero).
                glm::vec3 plano(camAtras.x, 0.0f, camAtras.z);
                const float largo2 = glm::dot(plano, plano);
                adelante = (largo2 > 1e-8f) ? plano * glm::inversesqrt(largo2)
                                            : glm::vec3(0.0f, 0.0f, 1.0f);
                derecha  = glm::normalize(glm::cross(arriba, adelante));
            }

            base = glm::mat4(1.0f);
            base[0] = glm::vec4(derecha,  0.0f);
            base[1] = glm::vec4(arriba,   0.0f);
            base[2] = glm::vec4(adelante, 0.0f);
            base[3] = glm::vec4(pos,      1.0f);
        }

        // Píxeles -> unidades, con la Y negada, y centrado.
        glm::mat4 local(1.0f);
        local[0][0] =  s;
        local[1][1] = -s;
        local[2][2] =  s;
        local[3]    = glm::vec4(-canvasSize.x * 0.5f * s, canvasSize.y * 0.5f * s, 0.0f, 1.0f);

        return base * local;
    }
}
