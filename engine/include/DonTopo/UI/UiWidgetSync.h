#pragma once
// The machinery that converts the scene's UI components into the canvas's
// live tree. It lived in TextComponent.h by historical accident: the first
// widget that needed it was Text, and it stayed there. It has nothing to do with
// the text component.
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
    // A scene's UI widgets, one list per type and the flattened
    // hierarchy. Filled in by Scene::collectUiWidgets and consumed by syncUiWidgets.
    //
    // A struct and not N loose parameters because the widget types GROW:
    // with one list per parameter, each new widget changed the signature of the
    // two functions and of their ~76 call sites, and a forgotten optional parameter gave
    // no compile error but a widget that did not show up.
    // Here a new field breaks nobody, and what does break (renaming) is
    // caught by the compiler.
    struct UiWidgetLists
    {
        std::vector<std::pair<uint64_t, const ButtonComponent*>>      buttons;
        std::vector<std::pair<uint64_t, const TextComponent*>>        texts;
        std::vector<std::pair<uint64_t, const ProgressBarComponent*>> bars;
        std::vector<std::pair<uint64_t, const LayoutComponent*>>      layouts;
        std::vector<std::pair<uint64_t, const PanelComponent*>>       panels;
        std::vector<std::pair<uint64_t, const ImageComponent*>>       images;
        // NOT const, unlike the others: the slider is interactive and its
        // handlers write `value` IN THE COMPONENT (which is what gets
        // serialized and what the editor reads), not in the canvas node.
        std::vector<std::pair<uint64_t, SliderComponent*>>             sliders;
        // The three also NOT const, for the same reason: their handlers write the
        // value IN THE COMPONENT.
        std::vector<std::pair<uint64_t, CheckboxComponent*>>           checkboxes;
        std::vector<std::pair<uint64_t, ToggleComponent*>>             toggles;
        std::vector<std::pair<uint64_t, ScrollbarComponent*>>          scrollbars;
        std::vector<std::pair<uint64_t, InputFieldComponent*>>         inputFields;
        std::vector<std::pair<uint64_t, DropdownComponent*>>           dropdowns;
        std::vector<std::pair<uint64_t, ScrollViewComponent*>>         scrollViews;

        // The scene's HIERARCHY flattened to (id, parent id), in PRE-ORDER
        // and with 0 for "hangs from the root". EMPTY = no hierarchy: everything hangs
        // from the root, which is what the sync did before it existed.
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

    // A scene canvas with EVERYTHING that hangs from it. It is what
    // Scene::collectCanvases produces and what the Renderer consumes.
    //
    // The widgets are grouped PER CANVAS and not in a common bag: with a single
    // canvas it made no difference, but with two, putting them all in the first one paints the
    // pause menu on top of the HUD without anything saying so.
    struct UiCanvasBinding
    {
        uint64_t               ownerId = 0;         // the Canvas's GameObject
        const CanvasComponent* canvas  = nullptr;
        // From the canvas's GameObject. Only World mode reads it; on screen it
        // means nothing (screen UI is not in the world).
        glm::mat4               worldTransform{1.0f};
        UiWidgetLists           widgets;
    };

    // What the sync has to remember BETWEEN frames, all together and in the hands of
    // whoever draws (one per loop). Without this the whole tree would have to be recreated
    // every frame, which besides throwing away the vertex cache would restart the
    // fade of every button on every frame.
    //
    // A SINGLE cache for ALL the widgets, and not one per type: the canvas
    // root is rebuilt with clearChildren(), so whoever clears it
    // has to own all its children. Two independent syncs over the same
    // root would delete each other's nodes every time one of them rebuilt.
    struct UiWidgetSyncCache
    {
        // Signature of the last frame: the GameObject ids that there were, IN ORDER. If it
        // changes (in any of the lists), the subtree is rebuilt
        // whole; if not, it is updated in place. The complete lists are compared
        // and not the sizes because two changes that compensate each other (one out, another
        // in) leave the same size.
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

        // The hierarchy the tree was assembled with, flattened to (id, parent) and in
        // the same order it arrived. Changing it moves nodes around, so it is
        // compared like the three lists: if it does not match, it is rebuilt.
        std::vector<std::pair<uint64_t, uint64_t>> parents;

        // Pointers to the live nodes, in the same order as the ids. They are
        // stable as long as nobody calls clearChildren(): UiElement::add moves
        // the unique_ptrs of the vector, not the pointed-to objects.
        std::vector<Button*> buttonNodes;
        std::vector<Text*>   buttonLabels;   // nullptr = that button has no label
        std::vector<Text*>   textNodes;
        // The bar is ALWAYS two nodes: the background and the fill child. That the
        // fill one exists even if the value is 0 (with drawable false) is what
        // keeps the SHAPE of the subtree constant: if it appeared and
        // disappeared the root would have to be rebuilt when crossing 0.
        std::vector<ProgressBar*> barNodes;
        std::vector<Panel*>       barFills;

        // The node each layout writes to: its own container if the
        // GameObject has no other UI component, and otherwise that one's node.
        // With layoutOwnsRect false that node has ANOTHER owner, so
        // only the layout fields come from here and never the rect.
        std::vector<UiElement*> layoutNodes;
        std::vector<char>       layoutOwnsRect;

        // Panel and Image: one node each, without children of their own.
        std::vector<Panel*> panelNodes;
        std::vector<Image*> imageNodes;

        // The slider is ALWAYS three nodes: the track and its fill and handle children.
        // That all three exist no matter what happens to the value is what keeps the
        // SHAPE of the subtree constant (same reason as the ProgressBar's
        // fill): if they appeared and disappeared the root would have to be
        // rebuilt when crossing the extremes.
        std::vector<Slider*>    sliderNodes;
        std::vector<UiElement*> sliderFills;
        std::vector<UiElement*> sliderHandles;

        // The other three interactive ones are TWO nodes each: the rect that receives
        // the mouse and the child that shows the state (the mark, the knob, the handle).
        // That the child exists no matter what happens to the value is what keeps the
        // SHAPE of the subtree constant.
        std::vector<Checkbox*>  checkboxNodes;
        std::vector<UiElement*> checkboxChecks;
        std::vector<Toggle*>    toggleNodes;
        std::vector<UiElement*> toggleKnobs;
        std::vector<Scrollbar*> scrollbarNodes;
        std::vector<UiElement*> scrollbarHandles;

        // The field is three nodes: the box, the text and the caret.
        std::vector<InputField*> inputFieldNodes;
        std::vector<Text*>       inputFieldTexts;
        std::vector<UiElement*>  inputFieldCarets;

        // The dropdown is four plus TWO per option (the row and its label).
        // It is the only one whose subtree changes SHAPE with the data, so the
        // count of options it was assembled with is stored to know when it has
        // to be rebuilt.
        std::vector<Dropdown*>   dropdownNodes;
        std::vector<Text*>       dropdownLabels;
        std::vector<UiElement*>  dropdownArrows;
        std::vector<UiElement*>  dropdownLists;
        std::vector<std::vector<UiElement*>> dropdownItems;
        std::vector<std::vector<Text*>>      dropdownItemLabels;
        std::vector<size_t>      dropdownOptionCounts;

        // The view is two: the viewport (which clips and receives the wheel) and the
        // content (which moves and from which the scene's children hang).
        std::vector<ScrollView*> scrollViewNodes;
        std::vector<UiElement*>  scrollViewContents;

        // Copy of what was dumped last time, in the same order. What has not
        // changed is not dumped again NOR dirtied: writing the fields
        // without dirtying leaves the node stuck (the canvas copies the cached
        // vertices), and always dirtying throws away the whole cache every frame.
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

        // GPU resources by path. Without this cache an atlas path would load a
        // NEW atlas every frame (Renderer::loadUiAtlas does not cache by path) and would
        // eat the video memory in seconds. A path that fails is cached
        // as nullptr: retrying it every frame would be reading a broken file 60
        // times per second.
        std::unordered_map<std::string, UiTextureAtlas*> atlases;
        std::unordered_map<std::string, UiFont*>         fonts;
    };

    // A LIVE canvas of the Renderer: its tree, its sync cache and what has to be
    // known to draw it. One per CanvasComponent in the scene.
    struct UiCanvasSlot
    {
        uint64_t           ownerId = 0;
        UiCanvas           canvas;
        UiWidgetSyncCache  cache;
        UiDrawData         drawData;
        UiCanvasRenderMode mode = UiCanvasRenderMode::ScreenSpace;
        glm::mat4          model{1.0f};
        bool               depthTest = true;

        // A copy BY VALUE of the component and of its GameObject's transform. Not
        // a pointer to the scene's component: the slot lives BETWEEN frames and the
        // scene may have deleted that GameObject.
        //
        // They are needed because the model matrix (uiWorldCanvasMatrix) needs
        // the camera's VIEW for the billboard, and the view is only known at
        // record time, not in syncUiCanvases. The WHOLE component is copied
        // and not just worldScale/billboard on purpose: a new world field
        // in CanvasComponent arrives on its own, without anybody having to remember
        // to add it here (forgetting it would give no error, only a setting that does
        // nothing).
        CanvasComponent    component{};
        glm::mat4          worldTransform{1.0f};
        // Distance to the eye, to sort the world ones from far to near.
        float              viewDepth = 0.0f;
    };

    // Reorders `slots` so that they match `bindings` one to one, pairing by
    // ownerId. The slots that survive KEEP their tree and their cache: without this,
    // reordering the canvases in the hierarchy would rebuild trees that have not
    // changed, and that shows up as flicker.
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
                nuevos.push_back(std::move(*it));   // takes the tree and cache along
            }
            else
            {
                auto s = std::make_unique<UiCanvasSlot>();
                s->ownerId = b.ownerId;
                nuevos.push_back(std::move(s));
            }
        }
        // What remains in `slots` belongs to canvases that are no longer there: it is destroyed on
        // leaving the scope, and with it its tree and its cache.
        slots = std::move(nuevos);
    }

    // The WORLD canvases in paint order: from far to near. They go with alpha,
    // so painting them the other way round blends badly. Against the geometry the depth
    // buffer rules; among themselves, this rules.
    //
    // The SCREEN ones do not come in: those go in their own pass, without depth and in
    // tree order.
    inline void sortWorldCanvasesBackToFront(
        const std::vector<std::unique_ptr<UiCanvasSlot>>& slots,
        const glm::mat4& view, std::vector<UiCanvasSlot*>& out)
    {
        out.clear();
        for (const auto& s : slots)
        {
            if (!s || s->mode != UiCanvasRenderMode::World) continue;
            const glm::vec3 pos = glm::vec3(s->model[3]);
            // +z forward: the view leaves the eye looking at -Z, so it is
            // negated so that "bigger" means "farther".
            s->viewDepth = -(view * glm::vec4(pos, 1.0f)).z;
            out.push_back(s.get());
        }
        std::sort(out.begin(), out.end(),
                  [](const UiCanvasSlot* a, const UiCanvasSlot* b) {
                      return a->viewDepth > b->viewDepth;   // far first
                  });
    }

    // The SCREEN canvases in INPUT PRIORITY order: the topmost
    // first. Topmost = the LAST one drawn, because the UI pass walks
    // the slots in order and each canvas is painted over the previous one. That is: this
    // order is the drawing order REVERSED, and it is not an aesthetic detail: it is what
    // decides which button gets the click when two canvases overlap, and it has
    // to be the SAME one the editor uses to select by clicking (otherwise the
    // click would select a different object from the one seen on top).
    //
    // The WORLD ones do not come in: they cannot be clicked (known limitation, see
    // Scripts/README.md), so putting them here would only steal the pointer from
    // the screen ones.
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

    // The canvas of ONE specific GameObject, by ownerId, or nullptr if that object
    // has no live canvas. It does NOT filter by mode: the filter is up to whoever
    // asks (the canvas gizmo already exits earlier on its own if it is a world one),
    // and a search that silently skipped the world ones would be a trap
    // for the next one who uses it.
    //
    // It is needed because uiCanvas() returns the FIRST screen canvas, not that
    // of the object being asked about: with it, the gizmo of a SECOND screen
    // canvas drew the first one's rect, a gizmo that lies, which is worse
    // than drawing nothing.
    inline const UiCanvas* findCanvasByOwner(
        const std::vector<std::unique_ptr<UiCanvasSlot>>& slots, uint64_t ownerId)
    {
        for (const auto& s : slots)
            if (s && s->ownerId == ownerId) return &s->canvas;
        return nullptr;
    }

    // What has to be sized for the UI frame, counted over each slot's
    // ALREADY built drawData.
    struct UiFrameTotals
    {
        // ALL the frame's canvases, world AND screen: they share ONE single
        // pair of buffers, so this is the total that has to be reserved.
        uint32_t vertices = 0;
        uint32_t indices  = 0;
        // Only the SCREEN ones: they are the only ones that open the UI pass. With the
        // frame total it would also open with only world canvases alive, and it
        // would be a whole pass without a single draw inside.
        uint32_t screenVertices = 0;
        uint32_t screenIndices  = 0;
    };

    // The UI frame's count, without GPU and in a single place. Sizing with
    // half (only the screen ones, which is what the D3D12 backend did
    // when the world ones did not exist) leaves the rest outside the buffer, and there the
    // uiCursorFits guard discards them SILENTLY: not an error, not a warning from
    // any validation layer, not a canvas on screen.
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

    // Dumps the scene's widgets into the live canvas. The UiWidgetLists lists
    // go in scene traversal order and carry the id of each owning
    // GameObject.
    //
    // Loader is anything with loadUiAtlas(path) and loadUiFont(path), that is
    // the Renderer. It is a template so as not to put Renderer.h in a UI header:
    // the component knows nothing about Vulkan.
    //
    // The assembly order inside a GameObject is panels, images, bars,
    // buttons and texts: the last sibling rules, so a loose Text that
    // overlaps a button or a bar is drawn on top (a bar with a
    // label is exactly that, two components on the same GameObject), and the Panel
    // stays below everything, which is what a background wants.
    //
    // w.parents is the scene HIERARCHY flattened to (id, parent id), in
    // PRE-ORDER and with 0 for "hangs from the root". With it, the nodes of a
    // GameObject hang from its parent's MAIN node (the Button if it has one,
    // otherwise the ProgressBar, otherwise the Image, otherwise the Panel, otherwise the Text),
    // which is the one that provides the rect to anchor against. That is what makes the
    // parent PLACE, CLIP and DIM its children, which with the flat tree
    // from before it could not.
    //
    // EMPTY, everything hangs from the root, which is exactly what it did before
    // this existed. A parent that does not appear in parents (or that has no
    // UI component) does not count either: its child goes up to the root instead of
    // disappearing.
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
        // Pointer and not reference: empty means "no hierarchy", which is a
        // DIFFERENT assembly path (everything to the root) and not a hierarchy of zero
        // elements.
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

        // Did the widget SET change? Only then is it rebuilt.
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

        // One more OPTION in a dropdown is one more NODE: that is the shape of the
        // subtree, just like adding a widget. Changing an option's TEXT
        // is not, and that is why the count is compared and not the content.
        for (size_t i = 0; !rebuild && i < dropdowns.size(); i++)
            if (cache.dropdownOptionCounts[i] != dropdowns[i].second->options.size())
                rebuild = true;

        // A button that gains or loses a label (empty text <-> non-empty) changes
        // the SHAPE of the subtree, and that also forces a rebuild.
        for (size_t i = 0; !rebuild && i < buttons.size(); i++)
        {
            const bool wantsLabel = !buttons[i].second->text.empty();
            if (wantsLabel != (cache.buttonLabels[i] != nullptr)) rebuild = true;
        }

        // And moving a GameObject to another parent changes where its node hangs from, which is
        // the shape of the tree just as much as adding or removing a widget.
        {
            const size_t nuevos = parents ? parents->size() : 0;
            if (cache.parents.size() != nuevos) rebuild = true;
            for (size_t i = 0; !rebuild && i < nuevos; i++)
                if (cache.parents[i] != (*parents)[i]) rebuild = true;
        }

        if (rebuild)
        {
            // clear() and not root().clearChildren(): the canvas keeps state pointers
            // to specific nodes (the one with the mouse over it, the
            // pressed one, the focused one, the last click's) and those nodes are exactly
            // the ones that were just destroyed. clearChildren() left them
            // dangling, and the next updateInput dereferenced them: if the
            // allocator reused the address, the canvas believed the NEW
            // node was already hovered and did not mark it again.
            canvas.clear();

            // The vectors are SIZED and written by index, not with
            // push_back: the update phase indexes by position in the
            // input lists, and with hierarchy the nodes are created in tree
            // order, which is a different one.
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

            // Where each GameObject is in each list, so it can be assembled
            // when its turn comes in the tree traversal.
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

            // Node from which each GameObject's CHILDREN hang.
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
                // A component that cannot equal any real one forces the
                // first dump: a newly created node is born dirty, but the
                // fields have to be written all the same.
                cache.buttonPrev[i].text = "\x01(not synced)";

                // Script handlers. They are installed HERE, in the only place that
                // creates nodes, because clear() just took away those of the
                // previous tree: the owner of the callback is the component and
                // the node only has a weak_ptr to it, so a button that
                // loses its component stops firing instead of calling a
                // dead object.
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
                // The fill is a child and not a sibling: this way its rect is counted
                // in pixels from the background's corner and there is no need to redo
                // the anchors or the canvas scale by hand.
                Panel& f = p.add<Panel>(nombre + "/Fill");
                cache.barIds[i]   = entry.first;
                cache.barNodes[i] = &p;
                cache.barFills[i] = &f;
                cache.barPrev[i].backgroundPath = "\x01(not synced)";
                return &p;
            };

            auto creaPanel = [&](size_t i, UiElement& padre)
            {
                const auto& entry = panels[i];
                Panel& p = padre.add<Panel>(uiPanelNodeName(entry.first));
                cache.panelIds[i]   = entry.first;
                cache.panelNodes[i] = &p;
                // A component that cannot equal any real one forces the
                // first dump: a newly created node is born dirty, but the
                // fields have to be written all the same.
                cache.panelPrev[i].sprite = "\x01(not synced)";
                return &p;
            };

            auto creaImagen = [&](size_t i, UiElement& padre)
            {
                const auto& entry = images[i];
                Image& im = padre.add<Image>(uiImageNodeName(entry.first));
                cache.imageIds[i]   = entry.first;
                cache.imageNodes[i] = &im;
                cache.imagePrev[i].sprite = "\x01(not synced)";
                return &im;
            };

            auto creaSlider = [&](size_t i, UiElement& padre)
            {
                const auto& entry = sliders[i];
                const std::string nombre = uiSliderNodeName(entry.first);
                Slider& s = padre.add<Slider>(nombre);
                // Fill and handle as CHILDREN and not siblings: this way their rects are
                // counted in pixels from the track's corner and there is no need to
                // redo the anchors or the canvas scale by hand.
                Panel& f = s.add<Panel>(nombre + "/Fill");
                Panel& h = s.add<Panel>(nombre + "/Handle");
                cache.sliderIds[i]     = entry.first;
                cache.sliderNodes[i]   = &s;
                cache.sliderFills[i]   = &f;
                cache.sliderHandles[i] = &h;
                cache.sliderPrev[i].backgroundSprite = "\x01(not synced)";

                // Input. It is installed HERE, in the only place that creates nodes,
                // because clear() just took away those of the previous tree. The
                // weak_ptr is to the COMPONENT's runtime: if the component dies, the
                // handler stops writing instead of touching a dead object. `pista` can
                // be a raw pointer: the handler is a member OF THAT NODE and dies with it.
                std::weak_ptr<UiSliderRuntime> rt = entry.second->callbacks.ptr;
                auto desdeElRaton = [rt, pista = &s](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    SliderComponent& c = *p->owner;
                    if (!c.interactable) return;
                    // Without a resolved rect (invisible node or clipped to zero) there is
                    // nothing to measure the mouse against.
                    if (!pista->rectValid) return;

                    const glm::vec2 local = e.mousePos - pista->screenPos;
                    const float t  = c.normalizedFromLocal(local, pista->screenSize);
                    const float nv = c.valueFromNormalized(t);
                    if (nv == c.value) return;
                    c.value = nv;
                    if (p->onValueChanged) p->onValueChanged(nv);
                };
                // Down AND Drag: the whole track is a click zone (as in Unity),
                // not just the handle, and the drag follows the mouse even if it leaves the
                // rect (the canvas keeps the pressed button's target).
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
                cache.checkboxPrev[i].backgroundSprite = "\x01(not synced)";

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
                cache.togglePrev[i].backgroundSprite = "\x01(not synced)";

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
                cache.scrollbarPrev[i].backgroundSprite = "\x01(not synced)";

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

                // The wheel also moves the bar: without this, a list with a
                // scrollbar could only be moved by dragging, which is not what anybody
                // expects. + is toward the start (wheel up).
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
                    // Consumed: if it kept bubbling, a ScrollView that
                    // contains it would scroll at the same time and the content would jump
                    // double per notch.
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
                cache.inputFieldPrev[i].placeholder = "\x01(not synced)";

                std::weak_ptr<UiInputFieldRuntime> rt = entry.second->callbacks.ptr;

                // Text: the only path through which a character enters. The canvas
                // only delivers it to the element with FOCUS, so nothing needs
                // checking here beyond the component itself.
                f.onTextInput = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    InputFieldComponent& comp = *p->owner;
                    if (!comp.interactable || comp.readOnly) return;
                    if (!comp.insertCodepoint(e.codepoint)) return;
                    if (p->onValueChanged) p->onValueChanged(comp.text);
                };

                // Editing keys. Left/Right/Home/End/Backspace/Delete are
                // CONSUMED: otherwise the canvas's directional navigation would
                // take the focus to another widget in the middle of a word.
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
                            // Enter is NOT consumed: it closes the editing and lets
                            // the canvas go on with its own thing (submitFocused), which is
                            // what activates an "Accept" button with the gamepad.
                            if (p->onEndEdit) p->onEndEdit(comp.text);
                            break;
                        default: break;
                    }
                    if (cambio && p->onValueChanged) p->onValueChanged(comp.text);
                };

                // Losing focus also closes the editing: it is when a
                // form validates, and not everybody presses Enter.
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
                cache.dropdownPrev[i].backgroundSprite = "\x01(not synced)";

                std::weak_ptr<UiDropdownRuntime> rt = entry.second->callbacks.ptr;

                // The box opens and closes.
                d.onClick = [rt](UiEvent&)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    DropdownComponent& comp = *p->owner;
                    if (!comp.interactable) return;
                    comp.isOpen = !comp.isOpen;
                };

                // One row per option, each with its label. The index is
                // captured by value: it is what makes each row know which one it is
                // without looking itself up in the list.
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
                        // Choosing CLOSES, always: otherwise the list would stay
                        // open covering what is below after every choice.
                        comp.isOpen = false;
                        if (comp.value == indice) { e.consumed = true; return; }
                        comp.value = indice;
                        if (p->onValueChanged) p->onValueChanged(indice);
                        // Consumed: without this the click bubbles up to the box and
                        // its onClick would OPEN the list again in the same frame.
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
                cache.scrollViewPrev[i].backgroundSprite = "\x01(not synced)";

                std::weak_ptr<UiScrollViewRuntime> rt = entry.second->callbacks.ptr;
                v.onScroll = [rt](UiEvent& e)
                {
                    auto p = rt.lock();
                    if (!p || !p->owner) return;
                    ScrollViewComponent& comp = *p->owner;
                    if (e.scrollDelta == 0.0f) return;

                    const glm::vec2 rango = comp.scrollRange();
                    // Without travel it neither moves NOR notifies: content that fits
                    // whole does not scroll, and notifying of a change that has not happened
                    // would make a script work for nothing on every notch.
                    const bool ejeY = comp.vertical && rango.y > 0.0f;
                    const bool ejeX = !ejeY && comp.horizontal && rango.x > 0.0f;
                    if (!ejeY && !ejeX) return;

                    // The wheel UP (positive delta) goes up the list, that
                    // is the normalized position goes down.
                    const float rangoEje = ejeY ? rango.y : rango.x;
                    const float delta = -e.scrollDelta * comp.scrollSensitivity / rangoEje;

                    glm::vec2 np = comp.normalizedPosition;
                    float& eje = ejeY ? np.y : np.x;
                    const float antes = std::clamp(eje, 0.0f, 1.0f);
                    eje = std::clamp(antes + delta, 0.0f, 1.0f);
                    if (eje == antes) return;

                    comp.normalizedPosition = np;
                    if (p->onValueChanged) p->onValueChanged(np.x, np.y);
                    // Consumed: if it bubbled, a view inside another would move
                    // both with the same notch.
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
                cache.textPrev[i].text = "\x01(not synced)";
                return &t;
            };

            // A GameObject's layout: if there is no other UI component it assembles
            // its own container (not drawable) and that becomes its node; if
            // there is, it just points to that one's and assembles NOTHING. `compartido`
            // is exactly that foreign node, or nullptr.
            auto montaLayout = [&](size_t i, uint64_t id, UiElement& padre,
                                   UiElement* compartido) -> UiElement*
            {
                UiElement* destino = compartido;
                if (destino == nullptr)
                    destino = &padre.add<Panel>(uiLayoutNodeName(id));

                cache.layoutIds[i]       = id;
                cache.layoutNodes[i]     = destino;
                cache.layoutOwnsRect[i]  = compartido == nullptr ? (char)1 : (char)0;
                // A component that cannot equal any real one forces the
                // first dump, same as in the button and the bar.
                cache.layoutPrev[i].columns = 0xFFFFFFFFu;
                return compartido == nullptr ? destino : nullptr;
            };

            // All the components of ONE GameObject, in the order they are
            // drawn: the bar below, the button on top and the text last.
            // The node its children will hang from is the MAIN one: the button if there
            // is one, otherwise the bar, otherwise the text.
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

                // The container FIRST: it is the one that provides the rect and from which the
                // children will hang. Only when there is no widget on the
                // same GameObject; with one, the rect already has an owner.
                if (itLayout != idxLayout.end() && !tieneWidget)
                    caja = montaLayout(itLayout->second, id, padre, nullptr);

                // From bottom to top: the panel is the background, and the text is the one that has
                // to stay on top of everything.
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

                // The MAIN one is the one that provides the rect the children will hang
                // from: the interactive widget wins, and the Panel (which is a background)
                // loses against all of them.
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

                // The ScrollView is the EXCEPTION: its children hang from the CONTENT
                // and not from the viewport. Hanging from the viewport, scrolling would not drag
                // them along and the scroll would be useless. It wins over
                // any other widget on the same GameObject: it is the only one that
                // has an opinion on where what is inside goes.
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

                // With a widget on the same GameObject, the layout writes into that one's
                // main node: its children already hang from there, so it is the
                // one that has to place them.
                if (itLayout != idxLayout.end() && tieneWidget)
                    montaLayout(itLayout->second, id, padre, princ);
            };

            if (parents != nullptr)
            {
                // In PRE-ORDER: when a child's turn comes, its parent is already
                // assembled and its main node exists.
                for (const auto& rel : *parents)
                {
                    UiElement* padre = &canvas.root();
                    if (rel.second != 0)
                    {
                        auto it = principal.find(rel.second);
                        // A parent without any UI component (or that did not arrive in
                        // parents) cannot hold anyone: the child goes up to the
                        // root instead of disappearing from the tree.
                        if (it != principal.end() && it->second != nullptr) padre = it->second;
                    }
                    montaGameObject(rel.first, *padre);
                }

                // Whatever is in the lists but not in parents is assembled at the
                // root: losing a widget through a mismatch between the two inputs
                // would be a silent failure.
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
                // Without hierarchy: EVERYTHING hangs from the root and in the usual
                // order (buttons, bars and texts), which is what the scenes assembled before
                // the hierarchy existed expect. Panels and images go IN FRONT, which is
                // where a background goes: they have no inherited order to respect, they are
                // later additions.
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

                // Without hierarchy nothing hangs from anything, so the container does not
                // place any children; it is assembled all the same because the rest of the
                // system (gizmo, picking, the dump below) counts on its
                // node existing. Sharing a GameObject with a widget, the layout
                // points to that one's node, as in the path with hierarchy.
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

            // The way back: the state is resolved by updateInput on the NODE, and
            // without publishing it here a script would have no way to read it. It goes before
            // the "has not changed" cut because the state changes without
            // a single component field changing (hovering the mouse over it
            // is enough), and copying it does not dirty the node.
            if (auto rt = src.callbacks.ptr) rt->state = cache.buttonNodes[i]->state;

            if (src == cache.buttonPrev[i]) continue;   // nothing to touch this frame

            Button& b = *cache.buttonNodes[i];
            src.applyTo(b);
            b.atlas = resolveAtlas(src.atlasPath);
            // The fields are public and touched bare, so dirtying is the
            // responsibility of whoever writes. DirtyAll and not a subset:
            // here the whole node has been rewritten (rect, color, sprite and quad).
            b.markDirty(UiElement::DirtyAll);

            if (Text* label = cache.buttonLabels[i])
            {
                src.applyToLabel(*label);
                // Without a path the default font is used: a text that is not seen
                // looks like an engine bug, not a field left unfilled.
                label->font = resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                               : src.fontPath);
                // Without a font the emitter would draw the label as its base's
                // quad, that is a flat rectangle COVERING the button. Better to
                // paint nothing and let the button show.
                label->drawable = (label->font != nullptr);
                label->markDirty(UiElement::DirtyAll);
            }
            cache.buttonPrev[i] = src;
        }

        for (size_t i = 0; i < panels.size(); i++)
        {
            const PanelComponent& src = *panels[i].second;
            if (src == cache.panelPrev[i]) continue;   // nothing to touch this frame

            Panel& p = *cache.panelNodes[i];
            src.applyTo(p);
            p.atlas = resolveAtlas(src.atlasPath);
            // Dirtying is the responsibility of whoever writes the fields. DirtyAll
            // because here the whole node is rewritten (rect, color and sprite).
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

            // The owner is ALWAYS re-pointed, whether the component changes or not: it is
            // where the node's handler writes the value, and it goes before the cut
            // for "has not changed" because a component that does not change is
            // precisely the case in which it is still needed.
            if (auto rt = src.callbacks.ptr) rt->owner = &src;

            if (src == cache.sliderPrev[i]) continue;   // nothing to touch this frame

            Slider&    s = *cache.sliderNodes[i];
            UiElement& f = *cache.sliderFills[i];
            UiElement& h = *cache.sliderHandles[i];
            src.applyTo(s);
            src.applyToFill(f);
            src.applyToHandle(h);
            // A SINGLE atlas for the three parts: the sprites are sub-rect names
            // inside it, so one load and not three.
            UiTextureAtlas* atlas = resolveAtlas(src.atlasPath);
            s.atlas = atlas;
            f.atlas = atlas;
            h.atlas = atlas;
            // Dirtying is the responsibility of whoever writes the fields. The THREE
            // nodes: the track may have moved, and the fill and handle change
            // rect with the value.
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

            // The caret blinks with TIME and focus, which change without a
            // single component field changing: that is why this goes BEFORE the cut
            // for "has not changed" and is compared separately.
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
            // The font is resolved ONLY if there is something to write: loading it is
            // FreeType + bake + GPU upload, and an empty field without a placeholder would not
            // draw a single glyph with it.
            t.font = t.text.empty()
                         ? nullptr
                         : resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                            : src.fontPath);
            t.drawable = (t.font != nullptr);

            // The caret is placed by MEASURING the prefix with the font: without this
            // we would have to assume all letters are the same width, and in a
            // proportional font the caret would end up far from where it is typed.
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

            // The rows. The number ALWAYS matches: changing it forces a
            // rebuild, so nothing has to be created or destroyed here.
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
            // The owner is ALWAYS re-pointed, whether the component changes or not: it is
            // where the node's handler writes, and a component that does not change is
            // precisely the case in which it is still needed.
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
            if (src == cache.barPrev[i]) continue;   // nothing to touch this frame

            ProgressBar& p = *cache.barNodes[i];
            Panel&       f = *cache.barFills[i];
            src.applyTo(p);
            // Each part with its image, and the component's atlas as a fallback
            // for any that has no path of its own. resolveAtlas caches by PATH, so
            // two parts with the same file cost a single load, and with
            // empty paths the loader is not even called (loading an atlas is
            // synchronous and in the "Add Component" frame it shows up as a stall).
            UiTextureAtlas* comun = resolveAtlas(src.atlasPath);
            p.atlas = src.backgroundPath.empty() ? comun : resolveAtlas(src.backgroundPath);
            src.applyToFill(f);
            f.atlas = src.fillPath.empty() ? comun : resolveAtlas(src.fillPath);
            // Dirtying is the responsibility of whoever writes the fields. The TWO
            // nodes: the background may have moved and the fill changes rect
            // with the value. DirtyAll because here the whole node is rewritten.
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
            // The font is resolved ONLY if there is something to write. Loading one is
            // FreeType + atlas bake + GPU upload, all synchronous: doing it
            // in the "Add Component" frame shows up as a stall, and a newly
            // added Text is empty and would not draw a single glyph with it.
            // Same criterion as the Button, which without text does not even assemble the label.
            t.font = src.text.empty()
                         ? nullptr
                         : resolveFont(src.fontPath.empty() ? std::string(kDefaultUiFontPath)
                                                            : src.fontPath);
            // Same criterion as the button's label: without a font a flat
            // rectangle is not painted where there should be letters.
            t.drawable = (t.font != nullptr);
            t.markDirty(UiElement::DirtyAll);
            cache.textPrev[i] = src;
        }

        // The layout goes LAST: when it shares a node with a widget, that one has already
        // rewritten its rect this frame and this only adds the placement fields
        // on top. The other way round, a button that changes color would erase the
        // layout until the component's next change.
        for (size_t i = 0; i < lays.size(); i++)
        {
            const LayoutComponent& src = *lays[i].second;
            if (src == cache.layoutPrev[i]) continue;

            UiElement& e = *cache.layoutNodes[i];
            src.applyTo(e, cache.layoutOwnsRect[i] != 0);
            // DirtyAll and not just DirtyLayout: with its own rect the whole node
            // has been rewritten here.
            e.markDirty(UiElement::DirtyAll);
            cache.layoutPrev[i] = src;
        }
    }
}
