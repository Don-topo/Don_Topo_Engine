#pragma once
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Un botón de la UI 2D como componente de GameObject, con el MISMO contrato
    // que CanvasComponent: SOLO DATOS. No guarda ni un UiElement ni el atlas ni
    // la fuente — el árbol vivo lo sigue teniendo el Renderer
    // (Renderer::uiCanvas()), y quien dibuja lo reconstruye/actualiza cada frame
    // con syncUiButtons(). Así lo que se ve en Play y en el juego exportado sale
    // de la ESCENA y no de un árbol cableado a mano.
    //
    // Los nombres, los defaults y el significado son EXACTAMENTE los del núcleo:
    //   - el bloque de rect y el de sprite son de UiElement (UiCanvas.h),
    //   - el bloque de estados es de Button (UiWidgets.h),
    //   - el bloque de texto es de Text (UiWidgets.h), porque Button NO tiene
    //     texto: la etiqueta es un HIJO Text que monta el sync.
    // Este componente no interpreta ni clampa nada.
    //
    // Las dos rutas (atlasPath, fontPath) son lo ÚNICO que no es un campo del
    // núcleo: el núcleo guarda punteros a recursos de GPU, que no se serializan.
    // Las resuelve el sync contra el Renderer, no el componente.
    class ButtonComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
            bool      visible = true;

            // Ruta del atlas (PNG) y nombre del sprite base dentro de él. Vacías
            // = botón de color plano, que es lo que dibuja UiElement sin atlas.
            std::string atlasPath;
            std::string sprite;

            // --- Estados (Button) ---------------------------------------------
            bool               interactable = true;
            bool               selected     = false;
            UiButtonTransition transition   = UiButtonTransition::ColorTint;

            glm::vec4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 hoverColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 pressedColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 disabledColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 selectedColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Nombres del MISMO atlas del botón. Uno vacío deja el sprite como
            // esté (es el contrato de Button, no una decisión de aquí).
            std::string normalSprite;
            std::string hoverSprite;
            std::string pressedSprite;
            std::string disabledSprite;
            std::string selectedSprite;

            float fadeDuration = 0.1f;   // segundos del fundido de Animation

            // --- Etiqueta (hijo Text) -----------------------------------------
            std::string text;
            std::string fontPath;                        // TTF; vacía = sin texto
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            UiTextAlign textAlign = UiTextAlign::Center;

            // Vuelca el rect y los estados en el botón vivo. NO toca ni el atlas
            // (es un puntero a GPU: lo resuelve el sync) ni los campos que
            // escribe el propio canvas (state, fadeFrom, fadeStartTime,
            // stateReady): pisarlos cada frame mataría el fundido.
            void applyTo(Button& b) const
            {
                b.anchorMin    = anchorMin;
                b.anchorMax    = anchorMax;
                b.pivot        = pivot;
                b.position     = position;
                b.size         = size;
                b.color        = color;
                b.visible      = visible;
                b.sprite       = sprite;

                b.interactable = interactable;
                b.selected     = selected;
                b.transition   = transition;

                b.normalColor   = normalColor;
                b.hoverColor    = hoverColor;
                b.pressedColor  = pressedColor;
                b.disabledColor = disabledColor;
                b.selectedColor = selectedColor;

                b.normalSprite   = normalSprite;
                b.hoverSprite    = hoverSprite;
                b.pressedSprite  = pressedSprite;
                b.disabledSprite = disabledSprite;
                b.selectedSprite = selectedSprite;

                b.fadeDuration = fadeDuration;
            }

            // La etiqueta ocupa el rect ENTERO del botón (anclada a las cuatro
            // esquinas, sin márgenes): así el texto sigue al botón al cambiarle
            // el tamaño sin un segundo juego de campos que mantener.
            void applyToLabel(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f, 0.0f);
                t.anchorMax = glm::vec2(1.0f, 1.0f);
                t.pivot     = glm::vec2(0.0f, 0.0f);
                t.position  = glm::vec2(0.0f, 0.0f);
                t.text      = text;
                t.fontSize  = fontSize;
                t.color     = textColor;
                t.align     = textAlign;
                t.visible   = visible;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const ButtonComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       atlasPath == o.atlasPath && sprite == o.sprite &&
                       interactable == o.interactable && selected == o.selected &&
                       transition == o.transition &&
                       normalColor == o.normalColor && hoverColor == o.hoverColor &&
                       pressedColor == o.pressedColor && disabledColor == o.disabledColor &&
                       selectedColor == o.selectedColor &&
                       normalSprite == o.normalSprite && hoverSprite == o.hoverSprite &&
                       pressedSprite == o.pressedSprite && disabledSprite == o.disabledSprite &&
                       selectedSprite == o.selectedSprite &&
                       fadeDuration == o.fadeDuration &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && textAlign == o.textAlign;
            }
            bool operator!=(const ButtonComponent& o) const { return !(*this == o); }
    };

    // Fuente que se usa cuando el botón tiene texto y NADIE ha puesto una ruta.
    // Un texto sin fuente no se dibuja (el emisor cae al quad de la base), así
    // que sin este fallback escribir en el campo Text no se vería hasta buscar
    // un TTF a mano.
    //
    // Va DENTRO del proyecto y no a una fuente del sistema a propósito: el juego
    // exportado se lleva los assets del proyecto, no los de la máquina que
    // exportó, y una ruta tipo C:/Windows/Fonts/... deja el texto invisible en
    // cualquier otro PC. Relativa al directorio de trabajo, igual que el resto
    // de assets. El exportador la empaqueta cuando algún botón tiene texto sin
    // fuente propia (GameExporter::collectSceneAssets).
    inline constexpr const char* kDefaultUiFontPath =
        "assets/DancingScript-VariableFont_wght.ttf";

    // Nombre del nodo vivo de un botón dentro del canvas. Es la ÚNICA forma de
    // volver del árbol de UI al GameObject (el árbol no guarda punteros a la
    // escena), así que la convención vive aquí y no repetida en cada caller:
    // la usa el sync para crear los nodos y el editor para el gizmo y el
    // picking. La etiqueta cuelga como "<nombre>/Label".
    inline std::string uiButtonNodeName(uint64_t ownerId)
    {
        return "go:" + std::to_string(ownerId);
    }

    // Inversa de uiButtonNodeName, tolerante con el sufijo "/Label" (el hit
    // test devuelve el nodo más profundo, que puede ser la etiqueta). Devuelve
    // 0 si el nombre no es de un botón: 0 no es un id válido de GameObject.
    inline uint64_t uiButtonOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("go:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 3; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }

    // Lo que el sync tiene que recordar ENTRE frames, todo junto y en manos de
    // quien dibuja (una por bucle). Sin esto habría que recrear el árbol entero
    // cada frame, que además de tirar la caché de vértices reiniciaría el
    // fundido de cada botón en cada frame.
    struct UiButtonSyncCache
    {
        // Firma del último frame: los ids de GameObject que había, EN ORDEN. Si
        // cambia, el subárbol se reconstruye entero; si no, se actualiza en
        // sitio. Se compara la lista completa y no el tamaño porque dos cambios
        // que se compensan (uno fuera, otro dentro) dejan el mismo tamaño.
        std::vector<uint64_t> ids;

        // Punteros a los nodos vivos, en el mismo orden que ids. Son estables
        // mientras nadie llame a clearChildren(): UiElement::add mueve los
        // unique_ptr del vector, no los objetos apuntados.
        std::vector<Button*> nodes;
        std::vector<Text*>   labels;   // nullptr = ese botón no tiene etiqueta

        // Copia de lo que se volcó la última vez, en el mismo orden. Lo que no
        // ha cambiado no se vuelve a volcar NI se ensucia: escribir los campos
        // sin ensuciar deja el botón clavado (el canvas se copia los vértices
        // cacheados), y ensuciar siempre tira la caché entera cada frame.
        std::vector<ButtonComponent> prev;

        // Recursos de GPU por ruta. Sin esta caché una ruta de atlas cargaría un
        // atlas NUEVO cada frame (Renderer::loadUiAtlas no cachea por ruta) y se
        // comería la memoria de vídeo en segundos. Una ruta que falla se cachea
        // como nullptr: reintentarla cada frame sería leer un fichero roto 60
        // veces por segundo.
        std::unordered_map<std::string, UiTextureAtlas*> atlases;
        std::unordered_map<std::string, UiFont*>         fonts;
    };

    // Vuelca los ButtonComponent de la escena en el canvas vivo. `buttons` va en
    // orden de recorrido de la escena y trae el id de cada GameObject dueño.
    //
    // Loader es cualquier cosa con loadUiAtlas(path) y loadUiFont(path) — o sea
    // el Renderer. Es un template para no meter Renderer.h en un header de UI:
    // el componente no sabe de Vulkan.
    template <class Loader>
    inline void syncUiButtons(const std::vector<std::pair<uint64_t, const ButtonComponent*>>& buttons,
                              UiCanvas& canvas, UiButtonSyncCache& cache, Loader& loader)
    {
        auto resolveAtlas = [&](const std::string& path) -> UiTextureAtlas*
        {
            if (path.empty()) return nullptr;
            auto it = cache.atlases.find(path);
            if (it != cache.atlases.end()) return it->second;
            UiTextureAtlas* atlas = loader.loadUiAtlas(path);
            cache.atlases.emplace(path, atlas);
            return atlas;
        };
        auto resolveFont = [&](const std::string& path) -> UiFont*
        {
            if (path.empty()) return nullptr;
            auto it = cache.fonts.find(path);
            if (it != cache.fonts.end()) return it->second;
            UiFont* font = loader.loadUiFont(path);
            cache.fonts.emplace(path, font);
            return font;
        };

        // ¿Cambió el CONJUNTO de botones? Solo entonces se reconstruye.
        bool rebuild = cache.ids.size() != buttons.size();
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
            if (cache.ids[i] != buttons[i].first) rebuild = true;

        // Un botón que gana o pierde etiqueta (texto vacío <-> no vacío) cambia
        // la FORMA del subárbol, y eso también obliga a reconstruir.
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
        {
            const bool wantsLabel = !buttons[i].second->text.empty();
            if (wantsLabel != (cache.labels[i] != nullptr)) rebuild = true;
        }

        if (rebuild)
        {
            canvas.root().clearChildren();
            cache.ids.clear();
            cache.nodes.clear();
            cache.labels.clear();
            cache.prev.clear();
            for (const auto& entry : buttons)
            {
                const std::string nombre = uiButtonNodeName(entry.first);
                Button& b = canvas.root().add<Button>(nombre);
                cache.ids.push_back(entry.first);
                cache.nodes.push_back(&b);
                cache.labels.push_back(entry.second->text.empty()
                                           ? nullptr
                                           : &b.add<Text>(nombre + "/Label"));
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado: un nodo recién creado ya nace sucio, pero los
                // campos hay que escribirlos igual.
                ButtonComponent nunca;
                nunca.text = "\x01(sin volcar)";
                cache.prev.push_back(nunca);
            }
        }

        for (size_t i = 0; i < buttons.size(); i++)
        {
            const ButtonComponent& src = *buttons[i].second;
            if (src == cache.prev[i]) continue;   // nada que tocar este frame

            Button& b = *cache.nodes[i];
            src.applyTo(b);
            b.atlas = resolveAtlas(src.atlasPath);
            // Los campos son públicos y se tocan a pelo, así que ensuciar es
            // responsabilidad de quien escribe. DirtyAll y no un subconjunto:
            // aquí se ha reescrito el nodo entero (rect, color, sprite y quad).
            b.markDirty(UiElement::DirtyAll);

            if (Text* label = cache.labels[i])
            {
                src.applyToLabel(*label);
                // Sin ruta se usa la fuente por defecto: un texto que no se ve
                // parece un bug del motor, no un campo sin rellenar.
                label->font = resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                               : src.fontPath);
                // Sin fuente el emisor dibujaría la etiqueta como el quad de su
                // base, o sea un rectángulo liso TAPANDO el botón. Mejor no
                // pintar nada y que se vea el botón.
                label->drawable = (label->font != nullptr);
                label->markDirty(UiElement::DirtyAll);
            }
            cache.prev[i] = src;
        }
    }
}
