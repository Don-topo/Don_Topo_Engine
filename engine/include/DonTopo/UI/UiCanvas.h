#pragma once

// Jerarquía de la UI de juego, al estilo Unity: Canvas -> Panel -> hijos.
//
// Todo en PÍXELES y con el origen (0,0) arriba a la izquierda, +X a la derecha
// y +Y hacia abajo. La posición de un nodo es relativa a la esquina superior
// izquierda de su padre, y la escala del padre se acumula en el hijo.
//
// Panel e Image son el MISMO nodo dibujable de momento: un panel es un nodo sin
// atlas (color plano) y una imagen es el mismo nodo con atlas y sprite. Ni
// Button ni Text existen todavía.
//
// Esto vive en DonTopoCore, no en el editor: el juego exportado dibuja el mismo
// canvas con el mismo código.

#include "DonTopo/UI/UiSpriteBatch.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DonTopo
{
    class UiNode
    {
    public:
        explicit UiNode(std::string nodeName = {}) : name(std::move(nodeName)) {}

        std::string name;

        glm::vec2 position{0.0f, 0.0f};   // px, relativa al padre
        glm::vec2 size{0.0f, 0.0f};       // px, antes de la escala heredada
        glm::vec2 scale{1.0f, 1.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

        bool visible  = true;
        // Un nodo puede ser solo un contenedor (agrupa y recorta) sin pintar.
        bool drawable = true;

        const UiTextureAtlas* atlas = nullptr;
        std::string sprite;

        // Recorta A ESTE NODO y a sus descendientes contra su propio rect. El
        // scissor resultante es la INTERSECCIÓN con el que ya venía del padre,
        // nunca un reemplazo.
        bool clipChildren = false;

        UiNode& addChild(std::string childName = {});
        void clearChildren() { m_children.clear(); }

        const std::vector<std::unique_ptr<UiNode>>& children() const { return m_children; }

    private:
        std::vector<std::unique_ptr<UiNode>> m_children;
    };

    class UiCanvas
    {
    public:
        UiCanvas();

        UiNode&       root()       { return m_root; }
        const UiNode& root() const { return m_root; }

        bool visible() const { return m_visible; }
        void setVisible(bool v) { m_visible = v; }

        // Vacía la jerarquía. El canvas vuelve a costar cero.
        void clear();

        // width/height son el tamaño del render en píxeles: fijan el scissor
        // raíz y la ortográfica.
        void buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const;

    private:
        UiNode m_root{"Canvas"};
        bool   m_visible = true;
    };
}
