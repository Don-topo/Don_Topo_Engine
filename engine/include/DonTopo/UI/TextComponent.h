#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
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
                       align == o.align && overflow == o.overflow && wordWrap == o.wordWrap &&
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

        // Copia de lo que se volcó la última vez, en el mismo orden. Lo que no
        // ha cambiado no se vuelve a volcar NI se ensucia: escribir los campos
        // sin ensuciar deja el nodo clavado (el canvas se copia los vértices
        // cacheados), y ensuciar siempre tira la caché entera cada frame.
        std::vector<ButtonComponent>      buttonPrev;
        std::vector<TextComponent>        textPrev;
        std::vector<ProgressBarComponent> barPrev;

        // Recursos de GPU por ruta. Sin esta caché una ruta de atlas cargaría un
        // atlas NUEVO cada frame (Renderer::loadUiAtlas no cachea por ruta) y se
        // comería la memoria de vídeo en segundos. Una ruta que falla se cachea
        // como nullptr: reintentarla cada frame sería leer un fichero roto 60
        // veces por segundo.
        std::unordered_map<std::string, UiTextureAtlas*> atlases;
        std::unordered_map<std::string, UiFont*>         fonts;
    };

    // Vuelca los widgets de la escena en el canvas vivo. Las tres listas van en
    // orden de recorrido de la escena y traen el id de cada GameObject dueño.
    //
    // Loader es cualquier cosa con loadUiAtlas(path) y loadUiFont(path) — o sea
    // el Renderer. Es un template para no meter Renderer.h en un header de UI:
    // el componente no sabe de Vulkan.
    //
    // El orden de montaje es botones, barras y textos: el último hermano manda,
    // así que un Text suelto que se solape con un botón o con una barra se
    // dibuja encima (una barra con etiqueta es justo eso, dos componentes en el
    // mismo GameObject).
    template <class Loader>
    inline void syncUiWidgets(const std::vector<std::pair<uint64_t, const ButtonComponent*>>& buttons,
                              const std::vector<std::pair<uint64_t, const TextComponent*>>& texts,
                              const std::vector<std::pair<uint64_t, const ProgressBarComponent*>>& bars,
                              UiCanvas& canvas, UiWidgetSyncCache& cache, Loader& loader)
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

        // ¿Cambió el CONJUNTO de widgets? Solo entonces se reconstruye.
        bool rebuild = cache.buttonIds.size() != buttons.size() ||
                       cache.textIds.size() != texts.size() ||
                       cache.barIds.size() != bars.size();
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
            if (cache.buttonIds[i] != buttons[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < texts.size(); i++)
            if (cache.textIds[i] != texts[i].first) rebuild = true;
        for (size_t i = 0; !rebuild && i < bars.size(); i++)
            if (cache.barIds[i] != bars[i].first) rebuild = true;

        // Un botón que gana o pierde etiqueta (texto vacío <-> no vacío) cambia
        // la FORMA del subárbol, y eso también obliga a reconstruir.
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
        {
            const bool wantsLabel = !buttons[i].second->text.empty();
            if (wantsLabel != (cache.buttonLabels[i] != nullptr)) rebuild = true;
        }

        if (rebuild)
        {
            canvas.root().clearChildren();
            cache.buttonIds.clear();
            cache.buttonNodes.clear();
            cache.buttonLabels.clear();
            cache.buttonPrev.clear();
            cache.textIds.clear();
            cache.textNodes.clear();
            cache.textPrev.clear();
            cache.barIds.clear();
            cache.barNodes.clear();
            cache.barFills.clear();
            cache.barPrev.clear();

            for (const auto& entry : buttons)
            {
                const std::string nombre = uiButtonNodeName(entry.first);
                Button& b = canvas.root().add<Button>(nombre);
                cache.buttonIds.push_back(entry.first);
                cache.buttonNodes.push_back(&b);
                cache.buttonLabels.push_back(entry.second->text.empty()
                                                 ? nullptr
                                                 : &b.add<Text>(nombre + "/Label"));
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado: un nodo recién creado ya nace sucio, pero los
                // campos hay que escribirlos igual.
                ButtonComponent nunca;
                nunca.text = "\x01(sin volcar)";
                cache.buttonPrev.push_back(nunca);
            }

            for (const auto& entry : bars)
            {
                const std::string nombre = uiProgressBarNodeName(entry.first);
                ProgressBar& p = canvas.root().add<ProgressBar>(nombre);
                // El relleno es un hijo y no un hermano: así su rect se cuenta
                // en píxeles desde la esquina del fondo y no hay que rehacer a
                // mano las anclas ni la escala del canvas.
                Panel& f = p.add<Panel>(nombre + "/Fill");
                cache.barIds.push_back(entry.first);
                cache.barNodes.push_back(&p);
                cache.barFills.push_back(&f);
                // Un componente que no puede ser igual a ninguno real fuerza el
                // primer volcado, igual que con el Button.
                ProgressBarComponent nunca;
                nunca.backgroundPath = "\x01(sin volcar)";
                cache.barPrev.push_back(nunca);
            }

            for (const auto& entry : texts)
            {
                Text& t = canvas.root().add<Text>(uiTextNodeName(entry.first));
                cache.textIds.push_back(entry.first);
                cache.textNodes.push_back(&t);
                TextComponent nunca;
                nunca.text = "\x01(sin volcar)";
                cache.textPrev.push_back(nunca);
            }
        }

        for (size_t i = 0; i < buttons.size(); i++)
        {
            const ButtonComponent& src = *buttons[i].second;
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
    }
}
