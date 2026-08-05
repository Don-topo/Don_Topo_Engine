#pragma once

// Jerarquía de la UI de juego, al estilo Unity: Canvas -> Panel -> hijos.
//
// Todo en PÍXELES y con el origen (0,0) arriba a la izquierda, +X a la derecha
// y +Y hacia abajo. La posición de un nodo es relativa a la esquina superior
// izquierda de su padre, y la escala del padre se acumula en el hijo.
//
// UiElement es la base de TODOS los widgets: los tipos concretos (Panel, Image,
// Button, ...) viven en UiWidgets.h y de momento no añaden ni un campo ni un
// comportamiento propio, solo su typeName(). Un panel es un elemento sin atlas
// (color plano) y una imagen es el mismo elemento con atlas y sprite.
//
// Esto vive en DonTopoCore, no en el editor: el juego exportado dibuja el mismo
// canvas con el mismo código.

#include "DonTopo/UI/UiSpriteBatch.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace DonTopo
{
    class UiElement
    {
    public:
        explicit UiElement(std::string nodeName = {}) : name(std::move(nodeName)) {}
        virtual ~UiElement() = default;

        // Identidad del tipo sin RTTI ni enum paralelo: cada derivado devuelve
        // su literal y no hay nada más que mantener sincronizado.
        virtual const char* typeName() const { return "UiElement"; }

        std::string name;

        glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla dentro del padre
        glm::vec2 size{0.0f, 0.0f};       // px, antes de la escala heredada
        glm::vec2 scale{1.0f, 1.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

        // Normalizados 0..1. anchor es el punto DEL PADRE desde el que cuenta
        // position; pivot es el punto DE ESTE elemento que cae ahí. {0,0} y
        // {0,0} = esquina superior izquierda contra esquina superior izquierda.
        glm::vec2 anchor{0.0f, 0.0f};
        glm::vec2 pivot{0.0f, 0.0f};

        // Radianes. SE ALMACENA PERO NO SE APLICA: un quad rotado obliga a
        // decidir qué scissor usa un clipChildren rotado, y eso es otra fase.
        float rotation = 0.0f;

        // Se multiplica por la del padre por todo el árbol y acaba en el alfa
        // del color del vértice.
        float opacity = 1.0f;

        bool visible  = true;
        // Para el input futuro: NO afecta al dibujado.
        bool enabled  = true;
        // Un elemento puede ser solo un contenedor (agrupa y recorta) sin pintar.
        bool drawable = true;

        const UiTextureAtlas* atlas = nullptr;
        std::string sprite;

        // Recorta A ESTE ELEMENTO y a sus descendientes contra su propio rect.
        // El scissor resultante es la INTERSECCIÓN con el que ya venía del
        // padre, nunca un reemplazo.
        bool clipChildren = false;

        // Único modo de crear hijos: el árbol es dueño de ellos y devuelve el
        // tipo concreto, no la base.
        template <class T = UiElement>
        T& add(std::string childName = {})
        {
            static_assert(std::is_base_of<UiElement, T>::value,
                          "Un hijo del canvas tiene que derivar de UiElement");
            m_children.push_back(std::make_unique<T>(std::move(childName)));
            return static_cast<T&>(*m_children.back());
        }

        void clearChildren() { m_children.clear(); }

        const std::vector<std::unique_ptr<UiElement>>& children() const { return m_children; }

    private:
        std::vector<std::unique_ptr<UiElement>> m_children;
    };

    class UiCanvas
    {
    public:
        UiCanvas();

        UiElement&       root()       { return m_root; }
        const UiElement& root() const { return m_root; }

        bool visible() const { return m_visible; }
        void setVisible(bool v) { m_visible = v; }

        // Vacía la jerarquía. El canvas vuelve a costar cero.
        void clear();

        // width/height son el tamaño del render en píxeles: fijan el scissor
        // raíz y la ortográfica.
        void buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const;

    private:
        UiElement m_root{"Canvas"};
        bool   m_visible = true;
    };
}
