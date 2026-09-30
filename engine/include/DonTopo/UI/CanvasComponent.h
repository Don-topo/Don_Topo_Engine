#pragma once
#include "DonTopo/UI/UiCanvas.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace DonTopo
{
    // Where the canvas is drawn. ScreenSpace is the usual: an orthographic in
    // output pixels, on top of everything. World places it IN THE SCENE, with the
    // camera's perspective and covered by any geometry in front of it.
    enum class UiCanvasRenderMode { ScreenSpace, World };

    // How a world canvas is oriented relative to the camera. YawOnly rotates only
    // around the world's vertical: it is what a health bar wants, which
    // must not tip over when viewed from above. Full faces it completely, which is what
    // an icon wants.
    enum class UiBillboard { None, YawOnly, Full };

    // The RESOLUTION settings of the 2D UI as a GameObject component: a
    // GameObject with Canvas is the only place the UI hangs from. It stores neither the
    // widget tree nor a UiCanvas of its own. The live canvas is still held by
    // the Renderer (Renderer::uiCanvas()), and whoever draws copies these fields there
    // every frame with applyTo, just as lights are collected every frame. This way
    // what is seen in Play and in the exported game comes from the SCENE and not from a
    // hand-wired canvas.
    //
    // The names, defaults and meaning are EXACTLY those of UiCanvas:
    // this component neither interprets nor clamps anything (UiCanvas already takes care of that
    // when resolving the usable area). Public fields for the same reason: it is the same POD.
    class CanvasComponent
    {
        public:
            UiScaleMode   scaleMode           = UiScaleMode::ConstantPixelSize;
            float         scaleFactor         = 1.0f;               // multiplies all three modes
            glm::vec2     referenceResolution{1920.0f, 1080.0f};    // ScaleWithScreenSize
            UiScreenMatch screenMatch         = UiScreenMatch::MatchWidthOrHeight;
            float         matchWidthOrHeight  = 0.5f;               // 0 = width, 1 = height
            float         screenDpi           = 0.0f;               // 0 = unknown
            float         fallbackDpi         = 96.0f;              // the one used if it is not known
            float         referenceDpi        = 96.0f;              // ConstantPhysicalSize
            UiSafeArea    safeArea{};                               // in real pixels
            float         aspectRatio         = 0.0f;               // 0 = off

            // --- Draw mode ----------------------------------------------------
            UiCanvasRenderMode renderMode = UiCanvasRenderMode::ScreenSpace;

            // --- World only ----------------------------------------------------
            // In World mode the usable area is EXACTLY referenceResolution: there is
            // no screen to adjust to, so scaleMode, screenMatch,
            // matchWidthOrHeight, the three DPIs, safeArea and aspectRatio ARE NOT
            // READ. They are not hidden in the editor: the nuance is documented.
            float       worldScale = 0.001f;   // world units per canvas PIXEL
            UiBillboard billboard  = UiBillboard::None;
            // When false the canvas is always drawn on top, going through walls: it is
            // what a health bar that must not be lost from sight wants.
            bool        depthTest  = true;

            // Dumps the fields into the live canvas. It touches neither the tree nor
            // the visibility: only the resolution.
            void applyTo(UiCanvas& canvas) const
            {
                if (renderMode == UiCanvasRenderMode::World)
                {
                    // A world canvas does not adjust to any screen: its usable area
                    // is its reference resolution and that is it. Dumping the
                    // scaleMode or the safe area here would make the sign change
                    // size when resizing the window, which is exactly what a
                    // world object must NOT do.
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

    // MODEL matrix of a world canvas: from canvas pixels to world
    // units. A free function and deliberately not a method: it needs the camera's view
    // for the billboard, and the component has no reason to know about
    // cameras. Here and not in the Renderer so it can be tested without GPU.
    //
    // The canvas grows DOWNWARD and the world UPWARD, so the Y is
    // NEGATED. And the canvas is centered on the object: its pixel (w/2, h/2) falls
    // exactly on the GameObject's position.
    inline glm::mat4 uiWorldCanvasMatrix(const CanvasComponent& c, glm::vec2 canvasSize,
                                         const glm::mat4& worldTransform, const glm::mat4& view)
    {
        const float s = c.worldScale;

        // Base of the object: its transform, or one that faces the camera if there is a
        // billboard. The POSITION always comes from the transform; what the billboard
        // replaces is the rotation (and with it the object's scale, which on a
        // facing canvas means nothing).
        glm::mat4 base = worldTransform;
        if (c.billboard != UiBillboard::None)
        {
            const glm::vec3 pos = glm::vec3(worldTransform[3]);

            // The CAMERA's axes come from the inverse of the view: the rows of
            // the rotational part of `view` are its axes in the world.
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
            else   // YawOnly: rotates only around the WORLD's vertical
            {
                arriba = glm::vec3(0.0f, 1.0f, 0.0f);
                // Project the camera's "backward" onto the horizontal
                // plane. Looking straight up or down the vector vanishes: in that
                // case any orientation works, and a fixed one is taken instead of
                // normalizing a zero (which would give NaN and erase the whole canvas).
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

        // Pixels -> units, with the Y negated, and centered.
        glm::mat4 local(1.0f);
        local[0][0] =  s;
        local[1][1] = -s;
        local[2][2] =  s;
        local[3]    = glm::vec4(-canvasSize.x * 0.5f * s, canvasSize.y * 0.5f * s, 0.0f, 1.0f);

        return base * local;
    }
}
