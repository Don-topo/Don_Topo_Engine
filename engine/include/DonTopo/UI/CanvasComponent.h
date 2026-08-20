#pragma once
#include "DonTopo/UI/UiCanvas.h"

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
}
