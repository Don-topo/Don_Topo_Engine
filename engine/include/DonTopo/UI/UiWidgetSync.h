#pragma once
// La maquinaria que convierte los componentes de UI de la escena en el árbol
// vivo del canvas. Vivía en TextComponent.h por accidente histórico: el primer
// widget que la necesitó fue el Text, y se quedó ahí. No tiene nada que ver con
// el componente de texto.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
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
        std::vector<std::pair<uint64_t, InputFieldComponent*>>         inputFields;
        std::vector<std::pair<uint64_t, DropdownComponent*>>           dropdowns;
        std::vector<std::pair<uint64_t, ScrollViewComponent*>>         scrollViews;

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
            inputFields.clear();
            dropdowns.clear();
            scrollViews.clear();
            parents.clear();
        }
    };

    // Un canvas de la escena con TODO lo que le cuelga. Es lo que
    // Scene::collectCanvases produce y lo que el Renderer consume.
    //
    // Los widgets van agrupados POR CANVAS y no en un saco común: con un solo
    // canvas daba igual, pero con dos, meterlos todos en el primero pinta el menú
    // de pausa encima del HUD sin que nada lo diga.
    struct UiCanvasBinding
    {
        uint64_t               ownerId = 0;         // GameObject del Canvas
        const CanvasComponent* canvas  = nullptr;
        // Del GameObject del canvas. Solo lo lee el modo World; en pantalla no
        // significa nada (la UI de pantalla no está en el mundo).
        glm::mat4               worldTransform{1.0f};
        UiWidgetLists           widgets;
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
        std::vector<uint64_t> inputFieldIds;
        std::vector<uint64_t> dropdownIds;
        std::vector<uint64_t> scrollViewIds;

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

        // El campo son tres nodos: la caja, el texto y el cursor.
        std::vector<InputField*> inputFieldNodes;
        std::vector<Text*>       inputFieldTexts;
        std::vector<UiElement*>  inputFieldCarets;

        // El desplegable son cuatro mas DOS por opcion (la fila y su etiqueta).
        // Es el unico cuyo subarbol cambia de FORMA con los datos, asi que la
        // cuenta de opciones con la que se monto se guarda para saber cuando hay
        // que reconstruir.
        std::vector<Dropdown*>   dropdownNodes;
        std::vector<Text*>       dropdownLabels;
        std::vector<UiElement*>  dropdownArrows;
        std::vector<UiElement*>  dropdownLists;
        std::vector<std::vector<UiElement*>> dropdownItems;
        std::vector<std::vector<Text*>>      dropdownItemLabels;
        std::vector<size_t>      dropdownOptionCounts;

        // La vista son dos: el viewport (que recorta y recibe la rueda) y el
        // contenido (que se mueve y del que cuelgan los hijos de la escena).
        std::vector<ScrollView*> scrollViewNodes;
        std::vector<UiElement*>  scrollViewContents;

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
        std::vector<InputFieldComponent>  inputFieldPrev;
        std::vector<DropdownComponent>    dropdownPrev;
        std::vector<ScrollViewComponent>  scrollViewPrev;

        // Recursos de GPU por ruta. Sin esta caché una ruta de atlas cargaría un
        // atlas NUEVO cada frame (Renderer::loadUiAtlas no cachea por ruta) y se
        // comería la memoria de vídeo en segundos. Una ruta que falla se cachea
        // como nullptr: reintentarla cada frame sería leer un fichero roto 60
        // veces por segundo.
        std::unordered_map<std::string, UiTextureAtlas*> atlases;
        std::unordered_map<std::string, UiFont*>         fonts;
    };

    // Un canvas VIVO del Renderer: su árbol, su caché de sync y lo que hay que
    // saber para dibujarlo. Uno por CanvasComponent de la escena.
    struct UiCanvasSlot
    {
        uint64_t           ownerId = 0;
        UiCanvas           canvas;
        UiWidgetSyncCache  cache;
        UiDrawData         drawData;
        UiCanvasRenderMode mode = UiCanvasRenderMode::ScreenSpace;
        glm::mat4          model{1.0f};
        bool               depthTest = true;

        // Copia POR VALOR del componente y del transform de su GameObject. No
        // un puntero al componente de la escena: el slot vive ENTRE frames y la
        // escena puede haber borrado ese GameObject.
        //
        // Hacen falta porque la matriz de modelo (uiWorldCanvasMatrix) necesita
        // la VISTA de la cámara para el billboard, y la vista solo se conoce en
        // el momento de grabar, no en syncUiCanvases. Se copia el componente
        // ENTERO y no solo worldScale/billboard a propósito: un campo de mundo
        // nuevo en CanvasComponent llega solo, sin que nadie tenga que acordarse
        // de añadirlo aquí (olvidarlo no daría error, solo un ajuste que no hace
        // nada).
        CanvasComponent    component{};
        glm::mat4          worldTransform{1.0f};
        // Distancia al ojo, para ordenar los de mundo de lejos a cerca.
        float              viewDepth = 0.0f;
    };

    // Reordena `slots` para que casen uno a uno con `bindings`, emparejando por
    // ownerId. Los slots que sobreviven CONSERVAN su árbol y su caché: sin esto,
    // reordenar los canvas en la jerarquía reconstruiría árboles que no han
    // cambiado, y eso se ve como un parpadeo.
    inline void matchUiCanvasSlots(const std::vector<UiCanvasBinding>& bindings,
                                   std::vector<std::unique_ptr<UiCanvasSlot>>& slots)
    {
        std::vector<std::unique_ptr<UiCanvasSlot>> nuevos;
        nuevos.reserve(bindings.size());

        for (const UiCanvasBinding& b : bindings)
        {
            auto it = std::find_if(slots.begin(), slots.end(),
                [&](const std::unique_ptr<UiCanvasSlot>& s) {
                    return s && s->ownerId == b.ownerId;
                });

            if (it != slots.end())
            {
                nuevos.push_back(std::move(*it));   // se lleva árbol y caché
            }
            else
            {
                auto s = std::make_unique<UiCanvasSlot>();
                s->ownerId = b.ownerId;
                nuevos.push_back(std::move(s));
            }
        }
        // Lo que quede en `slots` es de canvas que ya no están: se destruye al
        // salir del scope, y con ello su árbol y su caché.
        slots = std::move(nuevos);
    }

    // Los canvas de MUNDO en orden de pintado: de lejos a cerca. Van con alpha,
    // así que pintarlos al revés mezcla mal. Contra la geometría manda el depth
    // buffer; entre ellos, manda esto.
    //
    // Los de PANTALLA no entran: esos van en su propio pase, sin profundidad y en
    // el orden del árbol.
    inline void sortWorldCanvasesBackToFront(
        const std::vector<std::unique_ptr<UiCanvasSlot>>& slots,
        const glm::mat4& view, std::vector<UiCanvasSlot*>& out)
    {
        out.clear();
        for (const auto& s : slots)
        {
            if (!s || s->mode != UiCanvasRenderMode::World) continue;
            const glm::vec3 pos = glm::vec3(s->model[3]);
            // +z hacia delante: la vista deja el ojo mirando a -Z, así que se
            // niega para que "más grande" signifique "más lejos".
            s->viewDepth = -(view * glm::vec4(pos, 1.0f)).z;
            out.push_back(s.get());
        }
        std::sort(out.begin(), out.end(),
                  [](const UiCanvasSlot* a, const UiCanvasSlot* b) {
                      return a->viewDepth > b->viewDepth;   // lejos primero
                  });
    }

    // Los canvas de PANTALLA en orden de PRIORIDAD DE INPUT: el de más arriba
    // primero. Arriba = el ÚLTIMO que se dibuja, porque el pase de UI recorre
    // los slots en orden y cada canvas se pinta sobre el anterior. O sea: este
    // orden es el de dibujado AL REVÉS, y no es un detalle estético — es lo que
    // decide qué botón se lleva el clic cuando dos canvas se solapan, y tiene
    // que ser el MISMO que use el editor para seleccionar clicando (si no, el
    // clic seleccionaría un objeto distinto del que se ve encima).
    //
    // Los de MUNDO no entran: no se pueden clicar (limitación conocida, ver
    // Scripts/README.md), así que meterlos aquí solo les robaría el puntero a
    // los de pantalla.
    inline void screenCanvasesTopFirst(const std::vector<std::unique_ptr<UiCanvasSlot>>& slots,
                                       std::vector<UiCanvas*>& out)
    {
        out.clear();
        for (auto it = slots.rbegin(); it != slots.rend(); ++it)
        {
            const std::unique_ptr<UiCanvasSlot>& s = *it;
            if (!s || s->mode != UiCanvasRenderMode::ScreenSpace) continue;
            out.push_back(&s->canvas);
        }
    }

    // Lo que hay que dimensionar para el frame de UI, contado sobre el drawData
    // YA construido de cada slot.
    struct UiFrameTotals
    {
        // TODOS los canvas del frame, de mundo Y de pantalla: comparten UN solo
        // par de buffers, así que este es el total que hay que reservar.
        uint32_t vertices = 0;
        uint32_t indices  = 0;
        // Solo los de PANTALLA: son los únicos que abren el pase de UI. Con el
        // total del frame se abriría también con solo canvas de mundo vivos, y
        // sería un pase entero sin un draw dentro.
        uint32_t screenVertices = 0;
        uint32_t screenIndices  = 0;
    };

    // La cuenta del frame de UI, sin GPU y en un solo sitio. Dimensionar con la
    // mitad —solo los de pantalla, que es lo que hacía el backend de D3D12
    // cuando no existían los de mundo— deja al resto fuera del buffer, y ahí la
    // guarda uiCursorFits los descarta EN SILENCIO: ni un error, ni un aviso de
    // ninguna capa de validación, ni un canvas en pantalla.
    inline UiFrameTotals uiFrameTotals(const std::vector<std::unique_ptr<UiCanvasSlot>>& slots)
    {
        UiFrameTotals t;
        for (const auto& s : slots)
        {
            if (!s) continue;
            const uint32_t v = (uint32_t)s->drawData.vertices.size();
            const uint32_t i = (uint32_t)s->drawData.indices.size();
            t.vertices += v;
            t.indices  += i;
            if (s->mode == UiCanvasRenderMode::ScreenSpace)
            {
                t.screenVertices += v;
                t.screenIndices  += i;
            }
        }
        return t;
    }

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
        const auto& scrollbars  = w.scrollbars;
        const auto& inputFields = w.inputFields;
        const auto& dropdowns   = w.dropdowns;
        const auto& scrollViews = w.scrollViews;
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
                       cache.scrollbarIds.size() != scrollbars.size() ||
                       cache.inputFieldIds.size() != inputFields.size() ||
                       cache.dropdownIds.size() != dropdowns.size() ||
                       cache.scrollViewIds.size() != scrollViews.size();
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
        for (size_t i = 0; !rebuild && i < inputFields.size(); i++)
            if (cache.inputFieldIds[i] != inputFields[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < dropdowns.size(); i++)
            if (cache.dropdownIds[i] != dropdowns[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < scrollViews.size(); i++)
            if (cache.scrollViewIds[i] != scrollViews[i].first) rebuild = true;

        // Una OPCION mas en un desplegable es un NODO mas: eso es la forma del
        // subarbol, igual que anadir un widget. Cambiar el TEXTO de una opcion no
        // lo es, y por eso se compara la cuenta y no el contenido.
        for (size_t i = 0; !rebuild && i < dropdowns.size(); i++)
            if (cache.dropdownOptionCounts[i] != dropdowns[i].second->options.size())
                rebuild = true;

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
            cache.inputFieldIds.assign(inputFields.size(), 0ull);
            cache.inputFieldNodes.assign(inputFields.size(), nullptr);
            cache.inputFieldTexts.assign(inputFields.size(), nullptr);
            cache.inputFieldCarets.assign(inputFields.size(), nullptr);
            cache.inputFieldPrev.assign(inputFields.size(), InputFieldComponent{});
            cache.dropdownIds.assign(dropdowns.size(), 0ull);
            cache.dropdownNodes.assign(dropdowns.size(), nullptr);
            cache.dropdownLabels.assign(dropdowns.size(), nullptr);
            cache.dropdownArrows.assign(dropdowns.size(), nullptr);
            cache.dropdownLists.assign(dropdowns.size(), nullptr);
            cache.dropdownItems.assign(dropdowns.size(), {});
            cache.dropdownItemLabels.assign(dropdowns.size(), {});
            cache.dropdownOptionCounts.assign(dropdowns.size(), 0);
            cache.dropdownPrev.assign(dropdowns.size(), DropdownComponent{});
            cache.scrollViewIds.assign(scrollViews.size(), 0ull);
            cache.scrollViewNodes.assign(scrollViews.size(), nullptr);
            cache.scrollViewContents.assign(scrollViews.size(), nullptr);
            cache.scrollViewPrev.assign(scrollViews.size(), ScrollViewComponent{});

            // Dónde está cada GameObject en cada lista, para poder montarlo
            // cuando toque su turno en el recorrido del árbol.
            std::unordered_map<uint64_t, size_t> idxButton, idxBar, idxText, idxLayout,
                                                 idxPanel, idxImage, idxSlider,
                                                 idxCheckbox, idxToggle, idxScrollbar,
                                                 idxInputField, idxDropdown, idxScrollView;
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
            for (size_t i = 0; i < inputFields.size(); i++) idxInputField[inputFields[i].first] = i;
            for (size_t i = 0; i < dropdowns.size();   i++) idxDropdown[dropdowns[i].first]     = i;
            for (size_t i = 0; i < scrollViews.size(); i++) idxScrollView[scrollViews[i].first] = i;

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


            auto creaInputField = [&](size_t i, UiElement& padre)
            {
                const auto& entry = inputFields[i];
                const std::string nombre = uiInputFieldNodeName(entry.first);
                InputField& f = padre.add<InputField>(nombre);
                Text&       t = f.add<Text>(nombre + "/Text");
                Panel&      c = f.add<Panel>(nombre + "/Caret");
                cache.inputFieldIds[i]    = entry.first;
                cache.inputFieldNodes[i]  = &f;
                cache.inputFieldTexts[i]  = &t;
                cache.inputFieldCarets[i] = &c;
                cache.inputFieldPrev[i].placeholder = "\x01(sin volcar)";

                std::weak_ptr<UiInputFieldRuntime> rt = entry.second->callbacks.ptr;

                // Texto: el unico camino por el que entra un caracter. El canvas
                // solo lo entrega al elemento con FOCO, asi que aqui no hace
                // falta comprobar nada mas que el propio componente.
                f.onTextInput = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    InputFieldComponent& comp = *p->owner;
                    if (!comp.interactable || comp.readOnly) return;
                    if (!comp.insertCodepoint(e.codepoint)) return;
                    if (p->onValueChanged) p->onValueChanged(comp.text);
                };

                // Teclas de edicion. Left/Right/Home/End/Backspace/Delete se
                // CONSUMEN: si no, la navegacion direccional del canvas se
                // llevaria el foco a otro widget en mitad de una palabra.
                f.onKeyDown = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    InputFieldComponent& comp = *p->owner;
                    if (!comp.interactable) return;

                    bool cambio = false;
                    switch (e.key)
                    {
                        case UiKey::Backspace: cambio = comp.backspace();     e.consumed = true; break;
                        case UiKey::Delete:    cambio = comp.deleteForward(); e.consumed = true; break;
                        case UiKey::Left:      comp.moveCaret(-1);            e.consumed = true; break;
                        case UiKey::Right:     comp.moveCaret(1);             e.consumed = true; break;
                        case UiKey::Home:      comp.caretHome();              e.consumed = true; break;
                        case UiKey::End:       comp.caretEnd();               e.consumed = true; break;
                        case UiKey::Enter:
                            // Enter NO se consume: cierra la edicion y deja que
                            // el canvas siga con lo suyo (submitFocused), que es
                            // lo que activa un boton de "Aceptar" con el mando.
                            if (p->onEndEdit) p->onEndEdit(comp.text);
                            break;
                        default: break;
                    }
                    if (cambio && p->onValueChanged) p->onValueChanged(comp.text);
                };

                // Perder el foco tambien cierra la edicion: es cuando un
                // formulario valida, y no todo el mundo pulsa Enter.
                f.onBlur = [rt](UiEvent&)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    if (p->onEndEdit) p->onEndEdit(p->owner->text);
                };
                return &f;
            };

            auto creaDropdown = [&](size_t i, UiElement& padre)
            {
                const auto& entry = dropdowns[i];
                const std::string nombre = uiDropdownNodeName(entry.first);
                Dropdown& d = padre.add<Dropdown>(nombre);
                Text&     l = d.add<Text>(nombre + "/Label");
                Panel&    a = d.add<Panel>(nombre + "/Arrow");
                Panel&    li = d.add<Panel>(nombre + "/List");

                cache.dropdownIds[i]    = entry.first;
                cache.dropdownNodes[i]  = &d;
                cache.dropdownLabels[i] = &l;
                cache.dropdownArrows[i] = &a;
                cache.dropdownLists[i]  = &li;
                cache.dropdownItems[i].clear();
                cache.dropdownItemLabels[i].clear();
                cache.dropdownOptionCounts[i] = entry.second->options.size();
                cache.dropdownPrev[i].backgroundSprite = "\x01(sin volcar)";

                std::weak_ptr<UiDropdownRuntime> rt = entry.second->callbacks.ptr;

                // La caja abre y cierra.
                d.onClick = [rt](UiEvent&)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    DropdownComponent& comp = *p->owner;
                    if (!comp.interactable) return;
                    comp.isOpen = !comp.isOpen;
                };

                // Una fila por opcion, cada una con su etiqueta. El indice se
                // captura por valor: es lo que hace que cada fila sepa cual es
                // sin buscarse a si misma en la lista.
                for (size_t k = 0; k < entry.second->options.size(); k++)
                {
                    const std::string nf = nombre + "/List/Item" + std::to_string(k);
                    Panel& fila = li.add<Panel>(nf);
                    Text&  et   = fila.add<Text>(nf + "/Label");
                    cache.dropdownItems[i].push_back(&fila);
                    cache.dropdownItemLabels[i].push_back(&et);

                    const int indice = (int)k;
                    fila.onClick = [rt, indice](UiEvent& e)
                    {
                        auto p = rt.lock();
                        if (!p || !p->owner) return;
                        DropdownComponent& comp = *p->owner;
                        if (!comp.interactable) return;
                        // Elegir CIERRA, siempre: si no, la lista se quedaria
                        // abierta tapando lo de debajo tras cada eleccion.
                        comp.isOpen = false;
                        if (comp.value == indice) { e.consumed = true; return; }
                        comp.value = indice;
                        if (p->onValueChanged) p->onValueChanged(indice);
                        // Consumido: sin esto el click burbujea hasta la caja y
                        // su onClick volveria a ABRIR la lista en el mismo frame.
                        e.consumed = true;
                    };
                }
                return &d;
            };

            auto creaScrollView = [&](size_t i, UiElement& padre)
            {
                const auto& entry = scrollViews[i];
                const std::string nombre = uiScrollViewNodeName(entry.first);
                ScrollView& v = padre.add<ScrollView>(nombre);
                Panel&      c = v.add<Panel>(nombre + "/Content");
                cache.scrollViewIds[i]      = entry.first;
                cache.scrollViewNodes[i]    = &v;
                cache.scrollViewContents[i] = &c;
                cache.scrollViewPrev[i].backgroundSprite = "\x01(sin volcar)";

                std::weak_ptr<UiScrollViewRuntime> rt = entry.second->callbacks.ptr;
                v.onScroll = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    ScrollViewComponent& comp = *p->owner;
                    if (e.scrollDelta == 0.0f) return;

                    const glm::vec2 rango = comp.scrollRange();
                    // Sin recorrido no se mueve NI avisa: un contenido que cabe
                    // entero no scrollea, y avisar de un cambio que no ha pasado
                    // haria trabajar a un script en balde en cada muesca.
                    const bool ejeY = comp.vertical && rango.y > 0.0f;
                    const bool ejeX = !ejeY && comp.horizontal && rango.x > 0.0f;
                    if (!ejeY && !ejeX) return;

                    // La rueda hacia ARRIBA (delta positivo) sube por la lista, o
                    // sea que baja la posicion normalizada.
                    const float rangoEje = ejeY ? rango.y : rango.x;
                    const float delta = -e.scrollDelta * comp.scrollSensitivity / rangoEje;

                    glm::vec2 np = comp.normalizedPosition;
                    float& eje = ejeY ? np.y : np.x;
                    const float antes = std::clamp(eje, 0.0f, 1.0f);
                    eje = std::clamp(antes + delta, 0.0f, 1.0f);
                    if (eje == antes) return;

                    comp.normalizedPosition = np;
                    if (p->onValueChanged) p->onValueChanged(np.x, np.y);
                    // Consumido: si burbujeara, una vista dentro de otra moveria
                    // las dos con la misma muesca.
                    e.consumed = true;
                };
                return &v;
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
                UiElement* campo  = nullptr;
                UiElement* combo  = nullptr;
                UiElement* vista  = nullptr;
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
                                         idxScrollbar.count(id) != 0 || idxInputField.count(id) != 0 ||
                                         idxDropdown.count(id) != 0 || idxScrollView.count(id) != 0;

                // El contenedor PRIMERO: es el que aporta el rect y del que
                // colgarán los hijos. Solo cuando no hay ningún widget en el
                // mismo GameObject — con uno, el rect ya tiene dueño.
                if (itLayout != idxLayout.end() && !tieneWidget)
                    caja = montaLayout(itLayout->second, id, padre, nullptr);

                // De abajo arriba: el panel es el fondo, y el texto el que tiene
                // que quedar encima de todo.
                if (auto it = idxPanel.find(id);  it != idxPanel.end())  panel  = creaPanel(it->second, padre);
                if (auto it = idxImage.find(id);  it != idxImage.end())  imagen = creaImagen(it->second, padre);
                if (auto it = idxScrollView.find(id); it != idxScrollView.end()) vista = creaScrollView(it->second, padre);
                if (auto it = idxSlider.find(id); it != idxSlider.end()) deslid = creaSlider(it->second, padre);
                if (auto it = idxInputField.find(id); it != idxInputField.end()) campo = creaInputField(it->second, padre);
                if (auto it = idxDropdown.find(id);  it != idxDropdown.end())  combo = creaDropdown(it->second, padre);
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
                                 : campo  ? campo
                                 : combo  ? combo
                                 : deslid ? deslid
                                 : scroll ? scroll
                                 : interr ? interr
                                 : casill ? casill
                                 : barra  ? barra
                                 : imagen ? imagen
                                 : panel  ? panel
                                 : (texto ? texto : caja);

                // El ScrollView es la EXCEPCION: sus hijos cuelgan del CONTENIDO
                // y no del viewport. Colgando del viewport, desplazarse no los
                // arrastraria y el scroll no serviria de nada. Gana sobre
                // cualquier otro widget del mismo GameObject: es el unico que
                // tiene una opinion sobre donde va lo de dentro.
                if (vista != nullptr)
                {
                    if (auto it = idxScrollView.find(id); it != idxScrollView.end())
                        princ = cache.scrollViewContents[it->second];
                }
                else if (princ == nullptr)
                {
                    princ = vista;
                }
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
                for (const auto& entry : inputFields)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : dropdowns)
                    if (principal.find(entry.first) == principal.end())
                        montaGameObject(entry.first, canvas.root());
                for (const auto& entry : scrollViews)
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
                for (size_t i = 0; i < scrollViews.size(); i++) creaScrollView(i, canvas.root());
                for (size_t i = 0; i < sliders.size(); i++) creaSlider(i, canvas.root());
                for (size_t i = 0; i < inputFields.size(); i++) creaInputField(i, canvas.root());
                for (size_t i = 0; i < dropdowns.size();   i++) creaDropdown(i, canvas.root());
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
                    else if (auto itif = idxInputField.find(id); itif != idxInputField.end())
                        compartido = cache.inputFieldNodes[itif->second];
                    else if (auto itdd = idxDropdown.find(id); itdd != idxDropdown.end())
                        compartido = cache.dropdownNodes[itdd->second];
                    else if (auto itsv = idxScrollView.find(id); itsv != idxScrollView.end())
                        compartido = cache.scrollViewNodes[itsv->second];
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

        for (size_t i = 0; i < inputFields.size(); i++)
        {
            InputFieldComponent& src = *inputFields[i].second;
            if (auto rt = src.callbacks.ptr) rt->owner = &src;

            InputField& f = *cache.inputFieldNodes[i];
            Text&       t = *cache.inputFieldTexts[i];
            UiElement&  c = *cache.inputFieldCarets[i];

            // El cursor parpadea con el TIEMPO y el foco, que cambian sin que
            // cambie ni un campo del componente: por eso esto va ANTES del corte
            // por "no ha cambiado" y se compara aparte.
            const bool enfocado = (canvas.focused() == &f);
            const bool fase = src.caretBlinkRate > 0.0f
                                  ? (((int)(canvas.lastTimeSeconds() / src.caretBlinkRate)) % 2) == 0
                                  : true;
            const bool verCaret = enfocado && fase && src.interactable;

            const bool sinCambios = (src == cache.inputFieldPrev[i]) &&
                                    (c.drawable == verCaret);
            if (sinCambios) continue;

            src.applyTo(f);
            src.applyToText(t);
            // La fuente se resuelve SOLO si hay algo que escribir: cargarla es
            // FreeType + bake + subida a GPU, y un campo vacio sin placeholder no
            // dibujaria ni un glyph con ella.
            t.font = t.text.empty()
                         ? nullptr
                         : resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                            : src.fontPath);
            t.drawable = (t.font != nullptr);

            // El cursor se coloca MIDIENDO el prefijo con la fuente: sin esto
            // habria que suponer que todas las letras miden lo mismo, y en una
            // fuente proporcional el cursor acabaria lejos de donde se escribe.
            float caretX = 0.0f;
            if (t.font != nullptr)
            {
                const float escala = t.font->scaleFor(src.fontSize);
                const std::vector<uint32_t> cps = UiFont::decodeUtf8(src.displayText());
                const int hasta = std::clamp(src.caretPos, 0, (int)cps.size());
                uint32_t anterior = 0;
                for (int k = 0; k < hasta; k++)
                {
                    if (anterior != 0) caretX += t.font->kerning(anterior, cps[(size_t)k]) * escala;
                    if (const UiGlyph* g = t.font->findGlyph(cps[(size_t)k]))
                        caretX += g->advance * escala;
                    anterior = cps[(size_t)k];
                }
            }
            src.applyToCaret(c, caretX, verCaret);

            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            f.atlas = atlas;
            f.markDirty(UiElement::DirtyAll);
            t.markDirty(UiElement::DirtyAll);
            c.markDirty(UiElement::DirtyAll);
            cache.inputFieldPrev[i] = src;
        }

        for (size_t i = 0; i < dropdowns.size(); i++)
        {
            DropdownComponent& src = *dropdowns[i].second;
            if (auto rt = src.callbacks.ptr) rt->owner = &src;
            if (src == cache.dropdownPrev[i]) continue;

            Dropdown&  d  = *cache.dropdownNodes[i];
            Text&      l  = *cache.dropdownLabels[i];
            UiElement& a  = *cache.dropdownArrows[i];
            UiElement& li = *cache.dropdownLists[i];

            src.applyTo(d);
            src.applyToLabel(l);
            src.applyToArrow(a);
            src.applyToList(li);

            UiFont* fuente = resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                              : src.fontPath);
            l.font     = l.text.empty() ? nullptr : fuente;
            l.drawable = (l.font != nullptr);

            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            d.atlas = atlas;
            a.atlas = atlas;

            d.markDirty(UiElement::DirtyAll);
            l.markDirty(UiElement::DirtyAll);
            a.markDirty(UiElement::DirtyAll);
            li.markDirty(UiElement::DirtyAll);

            // Las filas. El numero SIEMPRE cuadra: cambiarlo obliga a
            // reconstruir, asi que aqui no hay que crear ni destruir nada.
            for (size_t k = 0; k < cache.dropdownItems[i].size(); k++)
            {
                UiElement& fila = *cache.dropdownItems[i][k];
                Text&      et   = *cache.dropdownItemLabels[i][k];
                src.applyToItem(fila, et, (int)k);
                fila.atlas = atlas;
                et.font     = et.text.empty() ? nullptr : fuente;
                et.drawable = (et.font != nullptr);
                fila.markDirty(UiElement::DirtyAll);
                et.markDirty(UiElement::DirtyAll);
            }
            cache.dropdownPrev[i] = src;
        }

        for (size_t i = 0; i < scrollViews.size(); i++)
        {
            ScrollViewComponent& src = *scrollViews[i].second;
            if (auto rt = src.callbacks.ptr) rt->owner = &src;
            if (src == cache.scrollViewPrev[i]) continue;

            ScrollView& v = *cache.scrollViewNodes[i];
            UiElement&  c = *cache.scrollViewContents[i];
            src.applyTo(v);
            src.applyToContent(c);
            v.atlas = resolveAtlas(src.atlasPath);
            v.markDirty(UiElement::DirtyAll);
            c.markDirty(UiElement::DirtyAll);
            cache.scrollViewPrev[i] = src;
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
