#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Una etiqueta de la UI 2D como componente de GameObject, con el MISMO
    // contrato que CanvasComponent y ButtonComponent: SOLO DATOS. No guarda ni
    // un UiElement ni la fuente — el árbol vivo lo sigue teniendo el Renderer
    // (Renderer::uiCanvas()), y quien dibuja lo reconstruye/actualiza cada frame
    // con syncUiWidgets(). Así lo que se ve en Play y en el juego exportado sale
    // de la ESCENA y no de un árbol cableado a mano.
    //
    // Los nombres, los defaults y el significado son EXACTAMENTE los del núcleo:
    //   - el bloque de rect es de UiElement (UiCanvas.h), el mismo que replica
    //     ButtonComponent,
    //   - el resto son TODOS los campos de Text (UiWidgets.h), salvo `font`.
    // Este componente no interpreta ni clampa nada.
    //
    // fontPath es lo ÚNICO que no es un campo del núcleo: el núcleo guarda un
    // puntero a un recurso de GPU, que no se serializa. La resuelve el sync
    // contra el Renderer, no el componente.
    class TextComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   // relleno del glyph
            bool      visible = true;

            // --- Texto (Text) -------------------------------------------------
            std::string text;
            std::string fontPath;   // TTF; vacía = la fuente por defecto
            float       fontSize = 16.0f;

            float     outlineWidth = 0.0f;
            glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

            glm::vec2 shadowOffset{0.0f, 0.0f};
            glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};

            UiTextAlign    align    = UiTextAlign::Left;
            UiTextVAlign   vAlign   = UiTextVAlign::Top;
            UiTextOverflow overflow = UiTextOverflow::Overflow;
            bool           wordWrap = false;

            float boldStrength = 0.08f;
            float italicSkew   = 0.25f;

            // Vuelca el rect y el texto en el nodo vivo. NO toca `font` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Text& t) const
            {
                t.anchorMin = anchorMin;
                t.anchorMax = anchorMax;
                t.pivot     = pivot;
                t.position  = position;
                t.size      = size;
                t.color     = color;
                t.visible   = visible;

                t.text         = text;
                t.fontSize     = fontSize;
                t.outlineWidth = outlineWidth;
                t.outlineColor = outlineColor;
                t.shadowOffset = shadowOffset;
                t.shadowColor  = shadowColor;
                t.align        = align;
                t.vAlign       = vAlign;
                t.overflow     = overflow;
                t.wordWrap     = wordWrap;
                t.boldStrength = boldStrength;
                t.italicSkew   = italicSkew;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const TextComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       outlineWidth == o.outlineWidth && outlineColor == o.outlineColor &&
                       shadowOffset == o.shadowOffset && shadowColor == o.shadowColor &&
                       align == o.align && vAlign == o.vAlign &&
                       overflow == o.overflow && wordWrap == o.wordWrap &&
                       boldStrength == o.boldStrength && italicSkew == o.italicSkew;
            }
            bool operator!=(const TextComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Text dentro del canvas. Mismo papel que
    // uiButtonNodeName (única forma de volver del árbol de UI al GameObject) y
    // con prefijo DISTINTO a propósito: un GameObject puede llevar Button y Text
    // a la vez, y dos nodos hermanos con el mismo nombre harían que el gizmo y
    // el picking cogieran el que no toca.
    inline std::string uiTextNodeName(uint64_t ownerId)
    {
        return "txt:" + std::to_string(ownerId);
    }

    // Inversa de uiTextNodeName. Devuelve 0 si el nombre no es de un Text: 0 no
    // es un id válido de GameObject.
    inline uint64_t uiTextOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("txt:", 0) != 0) return 0;
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

    // Los widgets de UI de una escena, una lista por tipo y la jerarquía
    // aplanada. Lo rellena Scene::collectUiWidgets y lo consume syncUiWidgets.
    //
    // Una struct y no N parámetros sueltos porque los tipos de widget CRECEN:
    // con una lista por parámetro, cada widget nuevo cambiaba la firma de las
    // dos funciones y de sus ~76 puntos de llamada, y un parámetro opcional
    // olvidado no daba error de compilación sino un widget que no aparecía.
    // Aquí un campo nuevo no rompe a nadie, y lo que sí rompe (renombrar) lo
    // caza el compilador.
    struct UiWidgetLists
    {
        std::vector<std::pair<uint64_t, const ButtonComponent*>>      buttons;
        std::vector<std::pair<uint64_t, const TextComponent*>>        texts;
        std::vector<std::pair<uint64_t, const ProgressBarComponent*>> bars;
        std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
        std::vector<std::pair<uint64_t, const PanelComponent*>>       panels;
        std::vector<std::pair<uint64_t, const ImageComponent*>>       images;
        // NO const, al reves que los demas: el slider es interactivo y sus
        // handlers escriben `value` EN EL COMPONENTE (que es lo que se
        // serializa y lo que lee el editor), no en el nodo del canvas.
        std::vector<std::pair<uint64_t, SliderComponent*>>             sliders;
        // Los tres tambien NO const, y por lo mismo: sus handlers escriben el
        // valor EN EL COMPONENTE.
        std::vector<std::pair<uint64_t, CheckboxComponent*>>           checkboxes;
        std::vector<std::pair<uint64_t, ToggleComponent*>>             toggles;
        std::vector<std::pair<uint64_t, ScrollbarComponent*>>          scrollbars;

        // La JERARQUÍA de la escena aplanada a (id, id del padre), en PRE-ORDEN
        // y con 0 para "cuelga de la raíz". VACÍA = sin jerarquía: todo cuelga
        // de la raíz, que es lo que hacía el sync antes de que existiera.
        std::vector<std::pair<uint64_t, uint64_t>> parents;

        void clear()
        {
            buttons.clear();
            texts.clear();
            bars.clear();
            layouts.clear();
            panels.clear();
            images.clear();
            sliders.clear();
            checkboxes.clear();
            toggles.clear();
            scrollbars.clear();
            parents.clear();
        }
    };

    // Lo que el sync tiene que recordar ENTRE frames, todo junto y en manos de
    // quien dibuja (una por bucle). Sin esto habría que recrear el árbol entero
    // cada frame, que además de tirar la caché de vértices reiniciaría el
    // fundido de cada botón en cada frame.
    //
    // UNA sola caché para TODOS los widgets, y no una por tipo: la raíz del
    // canvas se reconstruye con clearChildren(), así que quien la limpia tiene
    // que ser dueño de todos sus hijos. Dos syncs independientes sobre la misma
    // raíz se borrarían los nodos el uno al otro cada vez que uno reconstruyera.
    struct UiWidgetSyncCache
    {
        // Firma del último frame: los ids de GameObject que había, EN ORDEN. Si
        // cambia (en cualquiera de las dos listas), el subárbol se reconstruye
        // entero; si no, se actualiza en sitio. Se comparan las listas completas
        // y no los tamaños porque dos cambios que se compensan (uno fuera, otro
        // dentro) dejan el mismo tamaño.
        std::vector<uint64_t> buttonIds;
        std::vector<uint64_t> barIds;
        std::vector<uint64_t> textIds;
        std::vector<uint64_t> layoutIds;
        std::vector<uint64_t> panelIds;
        std::vector<uint64_t> imageIds;
        std::vector<uint64_t> sliderIds;
        std::vector<uint64_t> checkboxIds;
        std::vector<uint64_t> toggleIds;
        std::vector<uint64_t> scrollbarIds;

        // La jerarquía con la que se montó el árbol, aplanada a (id, padre) y en
        // el mismo orden que llegó. Cambiarla mueve nodos de sitio, así que se
        // compara igual que las tres listas: si no cuadra, se reconstruye.
        std::vector<std::pair<uint64_t, uint64_t>> parents;

        // Punteros a los nodos vivos, en el mismo orden que los ids. Son
        // estables mientras nadie llame a clearChildren(): UiElement::add mueve
        // los unique_ptr del vector, no los objetos apuntados.
        std::vector<Button*> buttonNodes;
        std::vector<Text*>   buttonLabels;   // nullptr = ese botón no tiene etiqueta
        std::vector<Text*>   textNodes;
        // La barra son SIEMPRE dos nodos: el fondo y el hijo del relleno. Que el
        // del relleno exista aunque el valor sea 0 (con drawable a false) es lo
        // que mantiene constante la FORMA del subárbol: si apareciera y
        // desapareciera habría que reconstruir la raíz al cruzar el 0.
        std::vector<ProgressBar*> barNodes;
        std::vector<Panel*>       barFills;

        // El nodo al que escribe cada layout: su contenedor propio si el
        // GameObject no tiene ningún otro componente de UI, y si no el nodo de
        // aquel. Con layoutOwnsRect a false ese nodo tiene OTRO dueño, así que
        // de aquí solo salen los campos de layout y nunca el rect.
        std::vector<UiElement*> layoutNodes;
        std::vector<char>       layoutOwnsRect;

        // Panel e Image: un nodo cada uno, sin hijos propios.
        std::vector<Panel*> panelNodes;
        std::vector<Image*> imageNodes;

        // El slider son SIEMPRE tres nodos: la pista y sus hijos relleno y asa.
        // Que los tres existan pase lo que pase con el valor es lo que mantiene
        // constante la FORMA del subarbol (mismo motivo que el relleno de la
        // ProgressBar): si aparecieran y desaparecieran habria que reconstruir
        // la raiz al cruzar los extremos.
        std::vector<Slider*>    sliderNodes;
        std::vector<UiElement*> sliderFills;
        std::vector<UiElement*> sliderHandles;

        // Los otros tres interactivos son DOS nodos cada uno: el rect que recibe
        // el raton y el hijo que ensena el estado (la marca, el mando, el asa).
        // Que el hijo exista pase lo que pase con el valor es lo que mantiene
        // constante la FORMA del subarbol.
        std::vector<Checkbox*>  checkboxNodes;
        std::vector<UiElement*> checkboxChecks;
        std::vector<Toggle*>    toggleNodes;
        std::vector<UiElement*> toggleKnobs;
        std::vector<Scrollbar*> scrollbarNodes;
        std::vector<UiElement*> scrollbarHandles;

        // Copia de lo que se volcó la última vez, en el mismo orden. Lo que no
        // ha cambiado no se vuelve a volcar NI se ensucia: escribir los campos
        // sin ensuciar deja el nodo clavado (el canvas se copia los vértices
        // cacheados), y ensuciar siempre tira la caché entera cada frame.
        std::vector<ButtonComponent>      buttonPrev;
        std::vector<TextComponent>        textPrev;
        std::vector<ProgressBarComponent> barPrev;
        std::vector<LayoutComponent>      layoutPrev;
        std::vector<PanelComponent>       panelPrev;
        std::vector<ImageComponent>       imagePrev;
        std::vector<SliderComponent>      sliderPrev;
        std::vector<CheckboxComponent>    checkboxPrev;
        std::vector<ToggleComponent>      togglePrev;
        std::vector<ScrollbarComponent>   scrollbarPrev;

        // Recursos de GPU por ruta. Sin esta caché una ruta de atlas cargaría un
        // atlas NUEVO cada frame (Renderer::loadUiAtlas no cachea por ruta) y se
        // comería la memoria de vídeo en segundos. Una ruta que falla se cachea
        // como nullptr: reintentarla cada frame sería leer un fichero roto 60
        // veces por segundo.
        std::unordered_map<std::string, UiTextureAtlas*> atlases;
        std::unordered_map<std::string, UiFont*>         fonts;
    };

    // Vuelca los widgets de la escena en el canvas vivo. Las listas de
    // UiWidgetLists van en orden de recorrido de la escena y traen el id de cada
    // GameObject dueño.
    //
    // Loader es cualquier cosa con loadUiAtlas(path) y loadUiFont(path) — o sea
    // el Renderer. Es un template para no meter Renderer.h en un header de UI:
    // el componente no sabe de Vulkan.
    //
    // El orden de montaje dentro de un GameObject es paneles, imágenes, barras,
    // botones y textos: el último hermano manda, así que un Text suelto que se
    // solape con un botón o con una barra se dibuja encima (una barra con
    // etiqueta es justo eso, dos componentes en el mismo GameObject), y el Panel
    // queda debajo de todo, que es lo que quiere un fondo.
    //
    // w.parents es la JERARQUÍA de la escena aplanada a (id, id del padre), en
    // PRE-ORDEN y con 0 para "cuelga de la raíz". Con ella, los nodos de un
    // GameObject cuelgan del nodo PRINCIPAL de su padre —el Button si lo tiene,
    // si no la ProgressBar, si no el Image, si no el Panel, si no el Text—, que
    // es el que aporta el rect contra el que anclarse. Eso es lo que hace que el
    // padre COLOQUE, RECORTE y ATENÚE a sus hijos, cosa que con el árbol plano
    // de antes no podía.
    //
    // VACÍA, todo cuelga de la raíz, que es exactamente lo que hacía antes de
    // que esto existiera. El padre que no aparezca en parents (o que no tenga
    // ningún componente de UI) tampoco cuenta: su hijo sube a la raíz en vez de
    // desaparecer.
    template <class Loader>
    inline void syncUiWidgets(const UiWidgetLists& w, UiCanvas& canvas,
                              UiWidgetSyncCache& cache, Loader& loader)
    {
        const auto& buttons = w.buttons;
        const auto& texts   = w.texts;
        const auto& bars    = w.bars;
        const auto& lays    = w.layouts;
        const auto& panels  = w.panels;
        const auto& images  = w.images;
        const auto& sliders    = w.sliders;
        const auto& checkboxes = w.checkboxes;
        const auto& toggles    = w.toggles;
        const auto& scrollbars = w.scrollbars;
        // Puntero y no referencia: vacía significa "sin jerarquía", que es un
        // camino de montaje DISTINTO (todo a la raíz) y no una jerarquía de cero
        // elementos.
        const std::vector<std::pair<uint64_t, uint64_t>>* parents =
            w.parents.empty() ? nullptr : &w.parents;

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

        // ¿Cambió el CONJUNTO de widgets? Solo entonces se reconstruye.
        bool rebuild = cache.buttonIds.size() != buttons.size() ||
                       cache.textIds.size() != texts.size() ||
                       cache.barIds.size() != bars.size() ||
                       cache.layoutIds.size() != lays.size() ||
                       cache.panelIds.size() != panels.size() ||
                       cache.imageIds.size() != images.size() ||
                       cache.sliderIds.size() != sliders.size() ||
                       cache.checkboxIds.size() != checkboxes.size() ||
                       cache.toggleIds.size() != toggles.size() ||
                       cache.scrollbarIds.size() != scrollbars.size();
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
            if (cache.buttonIds[i] != buttons[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < texts.size(); i++)
            if (cache.textIds[i] != texts[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < bars.size(); i++)
            if (cache.barIds[i] != bars[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < lays.size(); i++)
            if (cache.layoutIds[i] != lays[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < panels.size(); i++)
            if (cache.panelIds[i] != panels[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < images.size(); i++)
            if (cache.imageIds[i] != images[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < sliders.size(); i++)
            if (cache.sliderIds[i] != sliders[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < checkboxes.size(); i++)
            if (cache.checkboxIds[i] != checkboxes[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < toggles.size(); i++)
            if (cache.toggleIds[i] != toggles[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < scrollbars.size(); i++)
            if (cache.scrollbarIds[i] != scrollbars[i].first) rebuild = true;

        // Un botón que gana o pierde etiqueta (texto vacío <-> no vacío) cambia
        // la FORMA del subárbol, y eso también obliga a reconstruir.
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
        {
            const bool wantsLabel = !buttons[i].second->text.empty();
            if (wantsLabel != (cache.buttonLabels[i] != nullptr)) rebuild = true;
        }

        // Y mover un GameObject de padre cambia de dónde cuelga su nodo, que es
        // la forma del árbol tanto como añadir o quitar un widget.
        {
            const size_t nuevos = parents ? parents->size() : 0;
            if (cache.parents.size() != nuevos) rebuild = true;
            for (size_t i = 0; !rebuild && i < nuevos; i++)
                if (cache.parents[i] != (*parents)[i]) rebuild = true;
        }

        if (rebuild)
        {
            // clear() y no root().clearChildren(): el canvas guarda punteros de
            // estado a nodos concretos (el que tiene el ratón encima, el
            // pulsado, el del foco, el del último click) y esos nodos son justo
            // los que se acaban de destruir. clearChildren() los dejaba
            // colgando, y el siguiente updateInput los desreferenciaba: si el
            // asignador reutilizaba la dirección, el canvas se creía que el
            // nodo NUEVO ya estaba hovered y no volvía a marcarlo.
            canvas.clear();

            // Los vectores se DIMENSIONAN y se escriben por índice, no con
            // push_back: la fase de actualización indexa por la posición en las
            // listas de entrada, y con jerarquía los nodos se crean en el orden
            // del árbol, que es otro.
            cache.buttonIds.assign(buttons.size(), 0ull);
            cache.buttonNodes.assign(buttons.size(), nullptr);
            cache.buttonLabels.assign(buttons.size(), nullptr);
            cache.buttonPrev.assign(buttons.size(), ButtonComponent{});
            cache.textIds.assign(texts.size(), 0ull);
            cache.textNodes.assign(texts.size(), nullptr);
            cache.textPrev.assign(texts.size(), TextComponent{});
            cache.barIds.assign(bars.size(), 0ull);
            cache.barNodes.assign(bars.size(), nullptr);
            cache.barFills.assign(bars.size(), nullptr);
            cache.barPrev.assign(bars.size(), ProgressBarComponent{});
            cache.layoutIds.assign(lays.size(), 0ull);
            cache.layoutNodes.assign(lays.size(), nullptr);
            cache.layoutOwnsRect.assign(lays.size(), (char)0);
            cache.layoutPrev.assign(lays.size(), LayoutComponent{});
            cache.panelIds.assign(panels.size(), 0ull);
            cache.panelNodes.assign(panels.size(), nullptr);
            cache.panelPrev.assign(panels.size(), PanelComponent{});
            cache.imageIds.assign(images.size(), 0ull);
            cache.imageNodes.assign(images.size(), nullptr);
            cache.imagePrev.assign(images.size(), ImageComponent{});
            cache.sliderIds.assign(sliders.size(), 0ull);
            cache.sliderNodes.assign(sliders.size(), nullptr);
            cache.sliderFills.assign(sliders.size(), nullptr);
            cache.sliderHandles.assign(sliders.size(), nullptr);
            cache.sliderPrev.assign(sliders.size(), SliderComponent{});
            cache.checkboxIds.assign(checkboxes.size(), 0ull);
            cache.checkboxNodes.assign(checkboxes.size(), nullptr);
            cache.checkboxChecks.assign(checkboxes.size(), nullptr);
            cache.checkboxPrev.assign(checkboxes.size(), CheckboxComponent{});
            cache.toggleIds.assign(toggles.size(), 0ull);
            cache.toggleNodes.assign(toggles.size(), nullptr);
            cache.toggleKnobs.assign(toggles.size(), nullptr);
            cache.togglePrev.assign(toggles.size(), ToggleComponent{});
            cache.scrollbarIds.assign(scrollbars.size(), 0ull);
            cache.scrollbarNodes.assign(scrollbars.size(), nullptr);
            cache.scrollbarHandles.assign(scrollbars.size(), nullptr);
            cache.scrollbarPrev.assign(scrollbars.size(), ScrollbarComponent{});

            // Dónde está cada GameObject en cada lista, para poder montarlo
            // cuando toque su turno en el recorrido del árbol.
            std::unordered_map<uint64_t, size_t> idxButton, idxBar, idxText, idxLayout,
                                                 idxPanel, idxImage, idxSlider,
                                                 idxCheckbox, idxToggle, idxScrollbar;
            for (size_t i = 0; i < buttons.size(); i++) idxButton[buttons[i].first] = i;
            for (size_t i = 0; i < bars.size();    i++) idxBar[bars[i].first]       = i;
            for (size_t i = 0; i < texts.size();   i++) idxText[texts[i].first]     = i;
            for (size_t i = 0; i < lays.size();    i++) idxLayout[lays[i].first]    = i;
            for (size_t i = 0; i < panels.size();  i++) idxPanel[panels[i].first]   = i;
            for (size_t i = 0; i < images.size();  i++) idxImage[images[i].first]   = i;
            for (size_t i = 0; i < sliders.size(); i++) idxSlider[sliders[i].first] = i;
            for (size_t i = 0; i < checkboxes.size(); i++) idxCheckbox[checkboxes[i].first] = i;
            for (size_t i = 0; i < toggles.size();    i++) idxToggle[toggles[i].first]      = i;
            for (size_t i = 0; i < scrollbars.size(); i++) idxScrollbar[scrollbars[i].first] = i;

            // Nodo del que cuelgan los HIJOS de cada GameObject.
            std::unordered_map<uint64_t, UiElement*> principal;

            auto creaBoton = [&](size_t i, UiElement& padre)
            {
                const auto& entry = buttons[i];
                const std::string nombre = uiButtonNodeName(entry.first);
                Button& b = padre.add<Button>(nombre);
                cache.buttonIds[i]   = entry.first;
                cache.buttonNodes[i] = &b;
                cache.buttonLabels[i] = entry.second->text.empty()
                                            ? nullptr
                                            : &b.add<Text>(nombre + "/Label");
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado: un nodo recién creado ya nace sucio, pero los
                // campos hay que escribirlos igual.
                cache.buttonPrev[i].text = "\x01(sin volcar)";

                // Handlers de script. Se instalan AQUÍ, en el único sitio que
                // crea nodos, porque clear() se acaba de llevar por delante los
                // del árbol anterior: el dueño del callback es el componente y
                // el nodo solo tiene un weak_ptr a él, así que un botón que
                // pierde su componente deja de disparar en vez de llamar a un
                // objeto muerto.
                std::weak_ptr<UiButtonRuntime> rt = entry.second->callbacks.ptr;
                b.onClick = [rt](UiEvent&) {
                    if (auto p = rt.lock(); p && p->onClick) p->onClick();
                };
                b.onDoubleClick = [rt](UiEvent&) {
                    if (auto p = rt.lock(); p && p->onDoubleClick) p->onDoubleClick();
                };
                return &b;
            };

            auto creaBarra = [&](size_t i, UiElement& padre)
            {
                const auto& entry = bars[i];
                const std::string nombre = uiProgressBarNodeName(entry.first);
                ProgressBar& p = padre.add<ProgressBar>(nombre);
                // El relleno es un hijo y no un hermano: así su rect se cuenta
                // en píxeles desde la esquina del fondo y no hay que rehacer a
                // mano las anclas ni la escala del canvas.
                Panel& f = p.add<Panel>(nombre + "/Fill");
                cache.barIds[i]   = entry.first;
                cache.barNodes[i] = &p;
                cache.barFills[i] = &f;
                cache.barPrev[i].backgroundPath = "\x01(sin volcar)";
                return &p;
            };

            auto creaPanel = [&](size_t i, UiElement& padre)
            {
                const auto& entry = panels[i];
                Panel& p = padre.add<Panel>(uiPanelNodeName(entry.first));
                cache.panelIds[i]   = entry.first;
                cache.panelNodes[i] = &p;
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado: un nodo recién creado ya nace sucio, pero los
                // campos hay que escribirlos igual.
                cache.panelPrev[i].sprite = "\x01(sin volcar)";
                return &p;
            };

            auto creaImagen = [&](size_t i, UiElement& padre)
            {
                const auto& entry = images[i];
                Image& im = padre.add<Image>(uiImageNodeName(entry.first));
                cache.imageIds[i]   = entry.first;
                cache.imageNodes[i] = &im;
                cache.imagePrev[i].sprite = "\x01(sin volcar)";
                return &im;
            };

            auto creaSlider = [&](size_t i, UiElement& padre)
            {
                const auto& entry = sliders[i];
                const std::string nombre = uiSliderNodeName(entry.first);
                Slider& s = padre.add<Slider>(nombre);
                // Relleno y asa como HIJOS y no hermanos: así sus rects se
                // cuentan en píxeles desde la esquina de la pista y no hay que
                // rehacer a mano las anclas ni la escala del canvas.
                Panel& f = s.add<Panel>(nombre + "/Fill");
                Panel& h = s.add<Panel>(nombre + "/Handle");
                cache.sliderIds[i]     = entry.first;
                cache.sliderNodes[i]   = &s;
                cache.sliderFills[i]   = &f;
                cache.sliderHandles[i] = &h;
                cache.sliderPrev[i].backgroundSprite = "\x01(sin volcar)";

                // Input. Se instala AQUÍ, en el único sitio que crea nodos,
                // porque clear() se acaba de llevar por delante los del árbol
                // anterior. El weak_ptr es al runtime del COMPONENTE: si el
                // componente muere, el handler deja de escribir en vez de tocar
                // un objeto muerto. `pista` sí puede ser un puntero crudo — el
                // handler es un miembro DE ESE NODO y muere con él.
                std::weak_ptr<UiSliderRuntime> rt = entry.second->callbacks.ptr;
                auto desdeElRaton = [rt, pista = &s](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    SliderComponent& c = *p->owner;
                    if (!c.interactable) return;
                    // Sin rect resuelto (nodo invisible o recortado a cero) no
                    // hay nada contra lo que medir el ratón.
                    if (!pista->rectValid) return;

                    const glm::vec2 local = e.mousePos - pista->screenPos;
                    const float t  = c.normalizedFromLocal(local, pista->screenSize);
                    const float nv = c.valueFromNormalized(t);
                    if (nv == c.value) return;
                    c.value = nv;
                    if (p->onValueChanged) p->onValueChanged(nv);
                };
                // Down Y Drag: la pista entera es zona de clic (como en Unity),
                // no solo el asa, y el arrastre sigue al ratón aunque salga del
                // rect (el canvas mantiene el destino del botón pulsado).
                s.onMouseDown = desdeElRaton;
                s.onDrag      = desdeElRaton;
                return &s;
            };

            auto creaCheckbox = [&](size_t i, UiElement& padre)
            {
                const auto& entry = checkboxes[i];
                const std::string nombre = uiCheckboxNodeName(entry.first);
                Checkbox& c = padre.add<Checkbox>(nombre);
                Panel&    m = c.add<Panel>(nombre + "/Check");
                cache.checkboxIds[i]    = entry.first;
                cache.checkboxNodes[i]  = &c;
                cache.checkboxChecks[i] = &m;
                cache.checkboxPrev[i].backgroundSprite = "\x01(sin volcar)";

                std::weak_ptr<UiCheckboxRuntime> rt = entry.second->callbacks.ptr;
                c.onClick = [rt](UiEvent&)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    CheckboxComponent& comp = *p->owner;
                    if (!comp.interactable) return;
                    comp.isOn = !comp.isOn;
                    if (p->onValueChanged) p->onValueChanged(comp.isOn);
                };
                return &c;
            };

            auto creaToggle = [&](size_t i, UiElement& padre)
            {
                const auto& entry = toggles[i];
                const std::string nombre = uiToggleNodeName(entry.first);
                Toggle& t = padre.add<Toggle>(nombre);
                Panel&  k = t.add<Panel>(nombre + "/Knob");
                cache.toggleIds[i]   = entry.first;
                cache.toggleNodes[i] = &t;
                cache.toggleKnobs[i] = &k;
                cache.togglePrev[i].backgroundSprite = "\x01(sin volcar)";

                std::weak_ptr<UiToggleRuntime> rt = entry.second->callbacks.ptr;
                t.onClick = [rt](UiEvent&)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    ToggleComponent& comp = *p->owner;
                    if (!comp.interactable) return;
                    comp.isOn = !comp.isOn;
                    if (p->onValueChanged) p->onValueChanged(comp.isOn);
                };
                return &t;
            };

            auto creaScrollbar = [&](size_t i, UiElement& padre)
            {
                const auto& entry = scrollbars[i];
                const std::string nombre = uiScrollbarNodeName(entry.first);
                Scrollbar& s = padre.add<Scrollbar>(nombre);
                Panel&     h = s.add<Panel>(nombre + "/Handle");
                cache.scrollbarIds[i]     = entry.first;
                cache.scrollbarNodes[i]   = &s;
                cache.scrollbarHandles[i] = &h;
                cache.scrollbarPrev[i].backgroundSprite = "\x01(sin volcar)";

                std::weak_ptr<UiScrollbarRuntime> rt = entry.second->callbacks.ptr;
                auto desdeElRaton = [rt, canal = &s](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    ScrollbarComponent& comp = *p->owner;
                    if (!comp.interactable || !canal->rectValid) return;

                    const glm::vec2 local = e.mousePos - canal->screenPos;
                    const float nv = comp.valueFromLocal(local, canal->screenSize);
                    if (nv == comp.value) return;
                    comp.value = nv;
                    if (p->onValueChanged) p->onValueChanged(nv);
                };
                s.onMouseDown = desdeElRaton;
                s.onDrag      = desdeElRaton;

                // La rueda tambien mueve la barra: sin esto, una lista con
                // scrollbar solo se podria mover arrastrando, que no es lo que
                // espera nadie. + es hacia el principio (rueda hacia arriba).
                s.onScroll = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    ScrollbarComponent& comp = *p->owner;
                    if (!comp.interactable || e.scrollDelta == 0.0f) return;

                    const float nv = comp.snapValue(comp.value - e.scrollDelta * comp.scrollStep);
                    if (nv == comp.value) return;
                    comp.value = nv;
                    if (p->onValueChanged) p->onValueChanged(nv);
                    // Consumido: si sigue burbujeando, un ScrollView que la
                    // contenga se desplazaria a la vez y el contenido saltaria el
                    // doble por muesca.
                    e.consumed = true;
                };
                return &s;
            };

            auto creaTexto = [&](size_t i, UiElement& padre)
            {
                const auto& entry = texts[i];
                Text& t = padre.add<Text>(uiTextNodeName(entry.first));
                cache.textIds[i]   = entry.first;
                cache.textNodes[i] = &t;
                cache.textPrev[i].text = "\x01(sin volcar)";
                return &t;
            };

            // El layout de un GameObject: si no hay otro componente de UI monta
            // un contenedor propio (no dibujable) y ese pasa a ser su nodo; si lo
            // hay, se limita a apuntar al de aquel y NO monta nada. `compartido`
            // es justo ese nodo ajeno, o nullptr.
            auto montaLayout = [&](size_t i, uint64_t id, UiElement& padre,
                                   UiElement* compartido) -> UiElement*
            {
                UiElement* destino = compartido;
                if (destino == nullptr)
                    destino = &padre.add<Panel>(uiLayoutNodeName(id));

                cache.layoutIds[i]       = id;
                cache.layoutNodes[i]     = destino;
                cache.layoutOwnsRect[i]  = compartido == nullptr ? (char)1 : (char)0;
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado, igual que en el botón y en la barra.
                cache.layoutPrev[i].columns = 0xFFFFFFFFu;
                return compartido == nullptr ? destino : nullptr;
            };

            // Todos los componentes de UN GameObject, en el orden en el que se
            // dibujan: la barra debajo, el botón encima y el texto el último.
            // El nodo del que colgarán sus hijos es el PRINCIPAL: el botón si lo
            // hay, si no la barra, si no el texto.
            auto montaGameObject = [&](uint64_t id, UiElement& padre)
            {
                UiElement* panel  = nullptr;
                UiElement* imagen = nullptr;
                UiElement* deslid = nullptr;
                UiElement* casill = nullptr;
                UiElement* interr = nullptr;
                UiElement* scroll = nullptr;
                UiElement* barra  = nullptr;
                UiElement* boton  = nullptr;
                UiElement* texto  = nullptr;
                UiElement* caja   = nullptr;

                const auto itLayout = idxLayout.find(id);
                const bool tieneWidget = idxBar.count(id) != 0 || idxButton.count(id) != 0 ||
                                         idxText.count(id) != 0 || idxPanel.count(id) != 0 ||
                                         idxImage.count(id) != 0 || idxSlider.count(id) != 0 ||
                                         idxCheckbox.count(id) != 0 || idxToggle.count(id) != 0 ||
                                         idxScrollbar.count(id) != 0;

                // El contenedor PRIMERO: es el que aporta el rect y del que
                // colgarán los hijos. Solo cuando no hay ningún widget en el
                // mismo GameObject — con uno, el rect ya tiene dueño.
                if (itLayout != idxLayout.end() && !tieneWidget)
                    caja = montaLayout(itLayout->second, id, padre, nullptr);

                // De abajo arriba: el panel es el fondo, y el texto el que tiene
                // que quedar encima de todo.
                if (auto it = idxPanel.find(id);  it != idxPanel.end())  panel  = creaPanel(it->second, padre);
                if (auto it = idxImage.find(id);  it != idxImage.end())  imagen = creaImagen(it->second, padre);
                if (auto it = idxSlider.find(id); it != idxSlider.end()) deslid = creaSlider(it->second, padre);
                if (auto it = idxScrollbar.find(id); it != idxScrollbar.end()) scroll = creaScrollbar(it->second, padre);
                if (auto it = idxToggle.find(id);   it != idxToggle.end())   interr = creaToggle(it->second, padre);
                if (auto it = idxCheckbox.find(id); it != idxCheckbox.end()) casill = creaCheckbox(it->second, padre);
                if (auto it = idxBar.find(id);    it != idxBar.end())    barra = creaBarra(it->second, padre);
                if (auto it = idxButton.find(id); it != idxButton.end()) boton = creaBoton(it->second, padre);
                if (auto it = idxText.find(id);   it != idxText.end())   texto = creaTexto(it->second, padre);

                // El PRINCIPAL es el que aporta el rect del que colgaran los
                // hijos: gana el widget interactivo, y el Panel (que es fondo)
                // pierde contra todos.
                UiElement* princ = boton  ? boton
                                 : deslid ? deslid
                                 : scroll ? scroll
                                 : interr ? interr
                                 : casill ? casill
                                 : barra  ? barra
                                 : imagen ? imagen
                                 : panel  ? panel
                                 : (texto ? texto : caja);
                principal[id] = princ;

                // Con widget en el mismo GameObject, el layout escribe en el nodo
                // principal de aquel: sus hijos ya cuelgan de ahí, así que es el
                // que tiene que colocarlos.
                if (itLayout != idxLayout.end() && tieneWidget)
                    montaLayout(itLayout->second, id, padre, princ);
            };

            if (parents != nullptr)
            {
                // En PRE-ORDEN: cuando llega el turno de un hijo, su padre ya
                // está montado y su nodo principal existe.
                for (const auto& rel : *parents)
                {
                    UiElement* padre = &canvas.root();
                    if (rel.second != 0)
                    {
                        auto it = principal.find(rel.second);
                        // Un padre sin ningún componente de UI (o que no llegó en
                        // parents) no puede sostener a nadie: el hijo sube a la
                        // raíz en vez de desaparecer del árbol.
                        if (it != principal.end() && it->second != nullptr) padre = it->second;
                    }
                    montaGameObject(rel.first, *padre);
                }

                // Lo que esté en las listas pero no en parents se monta en la
                // raíz: perder un widget por un desajuste de las dos entradas
                // sería un fallo mudo.
                for (const auto& entry : buttons)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : bars)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : texts)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : panels)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : images)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : sliders)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : checkboxes)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : toggles)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : scrollbars)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : lays)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
            }
            else
            {
                // Sin jerarquía: TODO cuelga de la raíz y en el orden de
                // siempre —botones, barras y textos—, que es lo que esperan las
                // escenas montadas antes de que la jerarquía existiera. Los
                // paneles y las imágenes van DELANTE, que es donde va un fondo:
                // no tienen orden heredado que respetar, son posteriores.
                for (size_t i = 0; i < panels.size();  i++) creaPanel(i, canvas.root());
                for (size_t i = 0; i < images.size();  i++) creaImagen(i, canvas.root());
                for (size_t i = 0; i < sliders.size(); i++) creaSlider(i, canvas.root());
                for (size_t i = 0; i < scrollbars.size(); i++) creaScrollbar(i, canvas.root());
                for (size_t i = 0; i < toggles.size();    i++) creaToggle(i, canvas.root());
                for (size_t i = 0; i < checkboxes.size(); i++) creaCheckbox(i, canvas.root());
                for (size_t i = 0; i < buttons.size(); i++) creaBoton(i, canvas.root());
                for (size_t i = 0; i < bars.size();    i++) creaBarra(i, canvas.root());
                for (size_t i = 0; i < texts.size();   i++) creaTexto(i, canvas.root());

                // Sin jerarquía nada cuelga de nadie, así que el contenedor no
                // coloca a ningún hijo; se monta igual porque el resto del
                // sistema (gizmo, picking, el volcado de abajo) cuenta con que su
                // nodo existe. Compartiendo GameObject con un widget, el layout
                // apunta al nodo de aquel, como en el camino con jerarquía.
                for (size_t i = 0; i < lays.size(); i++)
                {
                    const uint64_t id = lays[i].first;
                    UiElement* compartido = nullptr;
                    if (auto it = idxButton.find(id); it != idxButton.end())
                        compartido = cache.buttonNodes[it->second];
                    else if (auto itb = idxBar.find(id); itb != idxBar.end())
                        compartido = cache.barNodes[itb->second];
                    else if (auto its = idxSlider.find(id); its != idxSlider.end())
                        compartido = cache.sliderNodes[its->second];
                    else if (auto itsb = idxScrollbar.find(id); itsb != idxScrollbar.end())
                        compartido = cache.scrollbarNodes[itsb->second];
                    else if (auto ittg = idxToggle.find(id); ittg != idxToggle.end())
                        compartido = cache.toggleNodes[ittg->second];
                    else if (auto itck = idxCheckbox.find(id); itck != idxCheckbox.end())
                        compartido = cache.checkboxNodes[itck->second];
                    else if (auto iti = idxImage.find(id); iti != idxImage.end())
                        compartido = cache.imageNodes[iti->second];
                    else if (auto itp = idxPanel.find(id); itp != idxPanel.end())
                        compartido = cache.panelNodes[itp->second];
                    else if (auto itt = idxText.find(id); itt != idxText.end())
                        compartido = cache.textNodes[itt->second];

                    montaLayout(i, id, canvas.root(), compartido);
                }
            }

            cache.parents = parents ? *parents : std::vector<std::pair<uint64_t, uint64_t>>{};
        }

        for (size_t i = 0; i < buttons.size(); i++)
        {
            const ButtonComponent& src = *buttons[i].second;

            // Camino de vuelta: el estado lo resuelve updateInput en el NODO, y
            // sin publicarlo aquí un script no tendría forma de leerlo. Va antes
            // del corte por "no ha cambiado" porque el estado cambia sin que
            // cambie ni un campo del componente (basta pasar el ratón por
            // encima), y copiarlo no ensucia el nodo.
            if (auto rt = src.callbacks.ptr) rt->state = cache.buttonNodes[i]->state;

            if (src == cache.buttonPrev[i]) continue;   // nada que tocar este frame

            Button& b = *cache.buttonNodes[i];
            src.applyTo(b);
            b.atlas = resolveAtlas(src.atlasPath);
            // Los campos son públicos y se tocan a pelo, así que ensuciar es
            // responsabilidad de quien escribe. DirtyAll y no un subconjunto:
            // aquí se ha reescrito el nodo entero (rect, color, sprite y quad).
            b.markDirty(UiElement::DirtyAll);

            if (Text* label = cache.buttonLabels[i])
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
            cache.buttonPrev[i] = src;
        }

        for (size_t i = 0; i < panels.size(); i++)
        {
            const PanelComponent& src = *panels[i].second;
            if (src == cache.panelPrev[i]) continue;   // nada que tocar este frame

            Panel& p = *cache.panelNodes[i];
            src.applyTo(p);
            p.atlas = resolveAtlas(src.atlasPath);
            // Ensuciar es responsabilidad de quien escribe los campos. DirtyAll
            // porque aquí se reescribe el nodo entero (rect, color y sprite).
            p.markDirty(UiElement::DirtyAll);
            cache.panelPrev[i] = src;
        }

        for (size_t i = 0; i < images.size(); i++)
        {
            const ImageComponent& src = *images[i].second;
            if (src == cache.imagePrev[i]) continue;

            Image& im = *cache.imageNodes[i];
            src.applyTo(im);
            im.atlas = resolveAtlas(src.atlasPath);
            im.markDirty(UiElement::DirtyAll);
            cache.imagePrev[i] = src;
        }

        for (size_t i = 0; i < sliders.size(); i++)
        {
            SliderComponent& src = *sliders[i].second;

            // El dueño se reapunta SIEMPRE, cambie o no el componente: es por
            // donde el handler del nodo escribe el valor, y va antes del corte
            // por "no ha cambiado" porque un componente que no cambia es
            // justamente el caso en el que sigue haciendo falta.
            if (auto rt = src.callbacks.ptr) rt->owner = &src;

            if (src == cache.sliderPrev[i]) continue;   // nada que tocar este frame

            Slider&    s = *cache.sliderNodes[i];
            UiElement& f = *cache.sliderFills[i];
            UiElement& h = *cache.sliderHandles[i];
            src.applyTo(s);
            src.applyToFill(f);
            src.applyToHandle(h);
            // UN solo atlas para las tres partes: los sprites son nombres de
            // sub-rect dentro de él, así que una carga y no tres.
            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            s.atlas = atlas;
            f.atlas = atlas;
            h.atlas = atlas;
            // Ensuciar es responsabilidad de quien escribe los campos. Los TRES
            // nodos: la pista puede haberse movido, y el relleno y el asa cambian
            // de rect con el valor.
            s.markDirty(UiElement::DirtyAll);
            f.markDirty(UiElement::DirtyAll);
            h.markDirty(UiElement::DirtyAll);
            cache.sliderPrev[i] = src;
        }

        for (size_t i = 0; i < checkboxes.size(); i++)
        {
            CheckboxComponent& src = *checkboxes[i].second;
            // El dueno se reapunta SIEMPRE, cambie o no el componente: es por
            // donde el handler del nodo escribe, y un componente que no cambia es
            // justamente el caso en el que sigue haciendo falta.
            if (auto rt = src.callbacks.ptr) rt->owner = &src;
            if (src == cache.checkboxPrev[i]) continue;

            Checkbox&  c = *cache.checkboxNodes[i];
            UiElement& m = *cache.checkboxChecks[i];
            src.applyTo(c);
            src.applyToCheck(m);
            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            c.atlas = atlas;
            m.atlas = atlas;
            c.markDirty(UiElement::DirtyAll);
            m.markDirty(UiElement::DirtyAll);
            cache.checkboxPrev[i] = src;
        }

        for (size_t i = 0; i < toggles.size(); i++)
        {
            ToggleComponent& src = *toggles[i].second;
            if (auto rt = src.callbacks.ptr) rt->owner = &src;
            if (src == cache.togglePrev[i]) continue;

            Toggle&    t = *cache.toggleNodes[i];
            UiElement& k = *cache.toggleKnobs[i];
            src.applyTo(t);
            src.applyToKnob(k);
            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            t.atlas = atlas;
            k.atlas = atlas;
            t.markDirty(UiElement::DirtyAll);
            k.markDirty(UiElement::DirtyAll);
            cache.togglePrev[i] = src;
        }

        for (size_t i = 0; i < scrollbars.size(); i++)
        {
            ScrollbarComponent& src = *scrollbars[i].second;
            if (auto rt = src.callbacks.ptr) rt->owner = &src;
            if (src == cache.scrollbarPrev[i]) continue;

            Scrollbar& s = *cache.scrollbarNodes[i];
            UiElement& h = *cache.scrollbarHandles[i];
            src.applyTo(s);
            src.applyToHandle(h);
            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            s.atlas = atlas;
            h.atlas = atlas;
            s.markDirty(UiElement::DirtyAll);
            h.markDirty(UiElement::DirtyAll);
            cache.scrollbarPrev[i] = src;
        }

        for (size_t i = 0; i < bars.size(); i++)
        {
            const ProgressBarComponent& src = *bars[i].second;
            if (src == cache.barPrev[i]) continue;   // nada que tocar este frame

            ProgressBar& p = *cache.barNodes[i];
            Panel&       f = *cache.barFills[i];
            src.applyTo(p);
            // Cada parte con su imagen, y el atlas del componente como fallback
            // de la que no tenga ruta propia. resolveAtlas cachea por RUTA, así
            // que dos partes con el mismo fichero cuestan una sola carga — y con
            // las rutas vacías ni se llama al loader (cargar un atlas es
            // síncrono y en el frame del "Add Component" se nota como un parón).
            UiTextureAtlas* comun = resolveAtlas(src.atlasPath);
            p.atlas = src.backgroundPath.empty() ? comun : resolveAtlas(src.backgroundPath);
            src.applyToFill(f);
            f.atlas = src.fillPath.empty() ? comun : resolveAtlas(src.fillPath);
            // Ensuciar es responsabilidad de quien escribe los campos. Los DOS
            // nodos: el fondo puede haberse movido y el relleno cambia de rect
            // con el valor. DirtyAll porque aquí se reescribe el nodo entero.
            p.markDirty(UiElement::DirtyAll);
            f.markDirty(UiElement::DirtyAll);
            cache.barPrev[i] = src;
        }

        for (size_t i = 0; i < texts.size(); i++)
        {
            const TextComponent& src = *texts[i].second;
            if (src == cache.textPrev[i]) continue;

            Text& t = *cache.textNodes[i];
            src.applyTo(t);
            // La fuente se resuelve SOLO si hay algo que escribir. Cargar una es
            // FreeType + bake del atlas + subida a GPU, todo síncrono: hacerlo
            // en el frame del "Add Component" se nota como un parón, y un Text
            // recién añadido está vacío y no dibujaría ni un glyph con ella.
            // Mismo criterio que el Button, que sin texto ni monta la etiqueta.
            t.font = src.text.empty()
                         ? nullptr
                         : resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                            : src.fontPath);
            // Mismo criterio que la etiqueta del botón: sin fuente no se pinta
            // un rectángulo liso donde debería haber letras.
            t.drawable = (t.font != nullptr);
            t.markDirty(UiElement::DirtyAll);
            cache.textPrev[i] = src;
        }

        // El layout va el ÚLTIMO: cuando comparte nodo con un widget, aquel ya
        // ha reescrito su rect este frame y esto solo añade los campos de
        // colocación encima. Al revés, un botón que cambia de color borraría el
        // layout hasta el siguiente cambio del componente.
        for (size_t i = 0; i < lays.size(); i++)
        {
            const LayoutComponent& src = *lays[i].second;
            if (src == cache.layoutPrev[i]) continue;

            UiElement& e = *cache.layoutNodes[i];
            src.applyTo(e, cache.layoutOwnsRect[i] != 0);
            // DirtyAll y no solo DirtyLayout: con rect propio aquí se ha
            // reescrito el nodo entero.
            e.markDirty(UiElement::DirtyAll);
            cache.layoutPrev[i] = src;
        }
    }
}
