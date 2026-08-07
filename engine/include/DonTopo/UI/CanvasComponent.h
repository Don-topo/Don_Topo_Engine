#pragma once
#include "DonTopo/UI/UiCanvas.h"

namespace DonTopo
{
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

            // Vuelca los 10 campos en el canvas vivo. No toca ni el árbol ni la
            // visibilidad: solo la resolución.
            void applyTo(UiCanvas& canvas) const
            {
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
