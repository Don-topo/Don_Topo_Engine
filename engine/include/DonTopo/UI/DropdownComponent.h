#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class DropdownComponent;

    // Lo que un desplegable tiene EN VIVO y no se serializa. Mismo papel y
    // mismos motivos que UiButtonRuntime.
    struct UiDropdownRuntime
    {
        std::function<void(int)> onValueChanged;
        DropdownComponent*       owner = nullptr;
    };

    struct UiDropdownCallbackSlot
    {
        std::shared_ptr<UiDropdownRuntime> ptr = std::make_shared<UiDropdownRuntime>();

        UiDropdownCallbackSlot() = default;
        UiDropdownCallbackSlot(const UiDropdownCallbackSlot&) {}
        UiDropdownCallbackSlot& operator=(const UiDropdownCallbackSlot&) { return *this; }
        UiDropdownCallbackSlot(UiDropdownCallbackSlot&&) = default;
        UiDropdownCallbackSlot& operator=(UiDropdownCallbackSlot&&) = default;

        bool operator==(const UiDropdownCallbackSlot&) const { return true; }
    };

    // Un desplegable de la UI 2D como componente de GameObject, con el MISMO
    // contrato que el resto: SOLO DATOS. El Dropdown del núcleo (UiWidgets.h) es
    // un stub SIN campos, así que el widget se monta por COMPOSICIÓN: la caja es
    // el nodo raíz (de tipo Dropdown) y de ella cuelgan la etiqueta, la flecha y
    // la lista, con una fila por opción.
    //
    // Es el único cuyo subárbol CAMBIA DE FORMA con los datos: una opción más es
    // un nodo más, así que el sync tiene que reconstruir cuando cambia el NÚMERO
    // de opciones (no cuando cambia su texto). Abrir y cerrar NO cambia la forma
    // —la lista existe siempre y solo se apaga—, que es lo que evita reconstruir
    // el canvas entero en cada click.
    class DropdownComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{200.0f, 32.0f};     // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color de la CAJA
            bool      visible = true;

            bool interactable = true;

            // --- Opciones -------------------------------------------------------
            std::vector<std::string> options;

            // Índice de la seleccionada. Sin clamp AQUÍ a propósito (el
            // componente no interpreta nada, mismo criterio que el resto): quien
            // LEE es el que aguanta un índice fuera de rango, y para eso está
            // selectedLabel().
            int value = 0;

            // Estado VIVO: si se guardara, una escena podría abrirse con la lista
            // desplegada tapando el menú. Entra en operator== —que es lo que hace
            // que el sync vuelva a volcar al abrir— pero NO en el JSON.
            bool isOpen = false;

            // --- Lista ----------------------------------------------------------
            float    itemHeight      = 24.0f;
            // 0 = todas. El alto de la lista se acota a esto para que un combo de
            // cincuenta idiomas no ocupe tres pantallas.
            uint32_t maxVisibleItems = 6;

            glm::vec4 listColor{0.12f, 0.12f, 0.12f, 1.0f};
            glm::vec4 itemColor{0.18f, 0.18f, 0.18f, 1.0f};
            glm::vec4 itemSelectedColor{0.25f, 0.45f, 0.7f, 1.0f};
            glm::vec4 arrowColor{1.0f, 1.0f, 1.0f, 1.0f};

            // --- Texto ----------------------------------------------------------
            std::string fontPath;   // TTF; vacía = la fuente por defecto
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            float       padding  = 6.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string arrowSprite;
            std::string itemSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiDropdownCallbackSlot callbacks;

            // Texto de la opción elegida, o vacío si el índice no apunta a
            // ninguna. Un índice fuera de rango no es un fallo del que haya que
            // salir: una escena editada a mano puede traer value 99 con dos
            // opciones, y eso no puede reventar.
            std::string selectedLabel() const
            {
                if (value < 0 || value >= (int)options.size()) return std::string();
                return options[(size_t)value];
            }

            // Filas que se enseñan de golpe.
            int visibleItemCount() const
            {
                const int total = (int)options.size();
                if (maxVisibleItems == 0) return total;
                return std::min(total, (int)maxVisibleItems);
            }

            float listHeight() const { return itemHeight * (float)visibleItemCount(); }

            // Vuelca el rect y la caja en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Dropdown& d) const
            {
                d.anchorMin = anchorMin;
                d.anchorMax = anchorMax;
                d.pivot     = pivot;
                d.position  = position;
                d.size      = size;
                d.color     = color;
                d.visible   = visible;
                d.sprite    = backgroundSprite;
                d.raycastTarget = true;
            }

            void applyToLabel(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f);
                t.anchorMax = glm::vec2(0.0f);
                t.pivot     = glm::vec2(0.0f);
                t.position  = glm::vec2(padding, 0.0f);
                // Deja sitio a la flecha por la derecha.
                t.size      = glm::vec2(std::max(size.x - 2.0f * padding - size.y, 0.0f), size.y);
                t.text      = selectedLabel();
                t.fontSize  = fontSize;
                t.color     = textColor;
                t.vAlign    = UiTextVAlign::Middle;
                t.overflow  = UiTextOverflow::Ellipsis;
                t.visible   = true;
                t.raycastTarget = false;
            }

            // La flecha es un cuadrado pegado al borde derecho. Sin sprite es un
            // quad de color: no se dibuja un triángulo porque el batcher emite
            // quads y una flecha de verdad es arte, no geometría.
            void applyToArrow(UiElement& a) const
            {
                const float lado = std::max(size.y * 0.4f, 0.0f);
                a.anchorMin = glm::vec2(0.0f);
                a.anchorMax = glm::vec2(0.0f);
                a.pivot     = glm::vec2(0.0f);
                a.position  = glm::vec2(std::max(size.x - padding - lado, 0.0f),
                                        (size.y - lado) * 0.5f);
                a.size      = glm::vec2(lado, lado);
                a.color     = arrowColor;
                a.sprite    = arrowSprite;
                a.visible   = true;
                a.drawable  = lado > 0.0f;
                a.raycastTarget = false;
            }

            // La lista cuelga JUSTO DEBAJO de la caja.
            void applyToList(UiElement& l) const
            {
                l.anchorMin = glm::vec2(0.0f);
                l.anchorMax = glm::vec2(0.0f);
                l.pivot     = glm::vec2(0.0f);
                l.position  = glm::vec2(0.0f, size.y);
                l.size      = glm::vec2(size.x, listHeight());
                l.color     = listColor;
                l.visible   = isOpen;
                l.drawable  = listHeight() > 0.0f;
                // Recorta a las filas que no quepan en maxVisibleItems.
                l.clipChildren  = true;
                // La lista en sí no recibe el ratón: lo reciben las filas. Así un
                // click en el hueco sobrante no elige nada en vez de elegir la
                // fila que hubiera debajo.
                l.raycastTarget = false;
            }

            void applyToItem(UiElement& fila, Text& etiqueta, int index) const
            {
                fila.anchorMin = glm::vec2(0.0f);
                fila.anchorMax = glm::vec2(0.0f);
                fila.pivot     = glm::vec2(0.0f);
                fila.position  = glm::vec2(0.0f, itemHeight * (float)index);
                fila.size      = glm::vec2(size.x, itemHeight);
                fila.color     = (index == value) ? itemSelectedColor : itemColor;
                fila.sprite    = itemSprite;
                fila.visible   = true;
                fila.raycastTarget = true;

                etiqueta.anchorMin = glm::vec2(0.0f);
                etiqueta.anchorMax = glm::vec2(0.0f);
                etiqueta.pivot     = glm::vec2(0.0f);
                etiqueta.position  = glm::vec2(padding, 0.0f);
                etiqueta.size      = glm::vec2(std::max(size.x - 2.0f * padding, 0.0f), itemHeight);
                etiqueta.text      = (index >= 0 && index < (int)options.size())
                                         ? options[(size_t)index] : std::string();
                etiqueta.fontSize  = fontSize;
                etiqueta.color     = textColor;
                etiqueta.vAlign    = UiTextVAlign::Middle;
                etiqueta.overflow  = UiTextOverflow::Ellipsis;
                etiqueta.visible   = true;
                // La etiqueta NO recibe el ratón o se comería el click de su
                // propia fila: el hit test devuelve el nodo más profundo.
                etiqueta.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar.
            bool operator==(const DropdownComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       options == o.options && value == o.value && isOpen == o.isOpen &&
                       itemHeight == o.itemHeight && maxVisibleItems == o.maxVisibleItems &&
                       listColor == o.listColor && itemColor == o.itemColor &&
                       itemSelectedColor == o.itemSelectedColor && arrowColor == o.arrowColor &&
                       fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && padding == o.padding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       arrowSprite == o.arrowSprite && itemSprite == o.itemSprite;
            }
            bool operator!=(const DropdownComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Dropdown dentro del canvas.
    inline std::string uiDropdownNodeName(uint64_t ownerId)
    {
        return "drp:" + std::to_string(ownerId);
    }

    // Inversa de uiDropdownNodeName. Devuelve 0 si el nombre no es de un
    // desplegable. El corte por '/' hace que la etiqueta, la flecha, la lista y
    // sus filas devuelvan también a su dueño.
    inline uint64_t uiDropdownOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("drp:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 4; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }
}
