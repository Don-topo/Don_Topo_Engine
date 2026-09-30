#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/AnimatorCanvasIds.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Blend2D.h"
#include "DonTopo/Core/PropertyTracks.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace ed = ax::NodeEditor;

namespace DonTopo {

namespace {
    // READ-ONLY drawing of a track: the shape of the curve, its keys, the
    // thresholds of the conditions that read its parameter and the playhead of the
    // preview. It answers at a glance the only question asked of a
    // curve (does it cross the threshold, and when?), which with the DragFloat list
    // has to be reconstructed by hand. The keys are still edited in the list: the
    // drag on the canvas is row C15 of the audit.
    // State of the drag of a key, between frames. The track is identified by
    // the ImGui ID of the canvas (unique per clip and track, see the PushID of the
    // call), not by indices: nobody deletes anything within the gesture, but this way
    // there are not three indices to keep coherent.
    struct ArrastreKey
    {
        ImGuiID lienzo = 0;    // 0 = no drag
        int     key    = -1;
        // The range is FROZEN while it lasts: if it were recomputed, moving the key
        // would rescale the drawing and the key would escape from the cursor.
        float   lo = 0.0f, hi = 0.0f;
    };
    ArrastreKey g_arrastre;

    // Returns true if it touched the track (so the caller knows it has to
    // re-resolve and that the undo has something to record).
    bool dibujarCurva(PropertyTrack& pista, float duracion, float tiempoActual,
                      const float* umbrales, int numUmbrales)
    {
        const float ancho = ImGui::GetContentRegionAvail().x;
        if (ancho < 40.0f || duracion <= 0.0f || pista.keys.empty()) return false;
        const float alto = 56.0f;
        // InvisibleButton and not Dummy: it is what captures the click and the drag
        // without fighting with the column scroll. AllowOverlap because the
        // canvas coexists with the widgets of the list below.
        ImGui::InvisibleButton("##curva", ImVec2(ancho, alto),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const ImGuiID idLienzo = ImGui::GetItemID();
        const bool    hover    = ImGui::IsItemHovered();
        const ImVec2  p0 = ImGui::GetItemRectMin();
        const ImVec2  p1 = ImGui::GetItemRectMax();
        ImDrawList*   dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
        dl->AddRect(p0, p1, hover ? IM_COL32(110, 110, 125, 255) : IM_COL32(70, 70, 80, 255));

        float lo = 0.0f, hi = 0.0f;
        curveRange(pista, umbrales, numUmbrales, lo, hi);
        const bool arrastrandoEsta = g_arrastre.lienzo == idLienzo && g_arrastre.key >= 0;
        if (arrastrandoEsta) { lo = g_arrastre.lo; hi = g_arrastre.hi; }
        auto aY = [&](float v) { return p1.y - (v - lo) / (hi - lo) * (p1.y - p0.y); };
        auto aX = [&](float t) {
            const float x = p0.x + (t / duracion) * (p1.x - p0.x);
            return std::min(std::max(x, p0.x), p1.x);   // a key beyond the clip stays at the edge
        };

        // --- Mouse: grab, drag, add and remove keys ---
        bool tocada = false;
        const ImVec2 raton = ImGui::GetIO().MousePos;
        // The key closest to the cursor within the grab radius.
        int cercana = -1;
        {
            float mejor = 7.0f * 7.0f;   // grab radius squared
            for (int k = 0; k < (int)pista.keys.size(); k++)
            {
                const float dx = aX(pista.keys[(size_t)k].time)  - raton.x;
                const float dy = aY(pista.keys[(size_t)k].value) - raton.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < mejor) { mejor = d2; cercana = k; }
            }
        }

        if (hover && cercana >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            g_arrastre = { idLienzo, cercana, lo, hi };
        }
        else if (hover && cercana >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            pista.keys.erase(pista.keys.begin() + cercana);
            tocada = true;
        }
        else if (hover && cercana < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const CurvePoint p = canvasToCurve(raton.x, raton.y, p0.x, p1.x, p0.y, p1.y, duracion, lo, hi);
            pista.keys.push_back({ p.time, p.value });
            tocada = true;
        }

        if (g_arrastre.lienzo == idLienzo)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || g_arrastre.key >= (int)pista.keys.size())
            {
                g_arrastre = {};   // it was released, or the key went away
            }
            else
            {
                const CurvePoint p = canvasToCurve(raton.x, raton.y, p0.x, p1.x, p0.y, p1.y,
                                                   duracion, g_arrastre.lo, g_arrastre.hi);
                PropertyKey& k = pista.keys[(size_t)g_arrastre.key];
                if (k.time != p.time || k.value != p.value) tocada = true;
                // There is no need to reorder: samplePropertyTrack looks for the segment
                // COMPARING times, not by position in the vector.
                k.time  = p.time;
                k.value = p.value;
            }
        }

        // The thresholds go under the curve: what matters is where it crosses them.
        for (int i = 0; i < numUmbrales; i++)
        {
            const float y = aY(umbrales[i]);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(220, 190, 80, 150));
            char txt[32];
            std::snprintf(txt, sizeof(txt), "%.2f", umbrales[i]);
            dl->AddText(ImVec2(p1.x - 36.0f, y - 15.0f), IM_COL32(220, 190, 80, 200), txt);
        }

        // Sampled, not joined key to key: this way the drawing remains the real value
        // if some day the interpolation stops being linear.
        constexpr int kMuestras = 64;
        ImVec2 pts[kMuestras + 1];
        for (int s = 0; s <= kMuestras; s++)
        {
            const float t = duracion * (float)s / (float)kMuestras;
            pts[s] = ImVec2(aX(t), aY(samplePropertyTrack(pista, t, 0.0f)));
        }
        dl->AddPolyline(pts, kMuestras + 1, IM_COL32(120, 200, 255, 255), 0, 1.5f);
        for (int k = 0; k < (int)pista.keys.size(); k++)
        {
            // The one that can be grabbed looks bigger: without that hint, hitting
            // the 7 px radius is done blindly.
            const bool activa = (arrastrandoEsta && g_arrastre.key == k) || (hover && cercana == k);
            dl->AddCircleFilled(ImVec2(aX(pista.keys[(size_t)k].time), aY(pista.keys[(size_t)k].value)),
                                activa ? 5.0f : 3.0f,
                                activa ? IM_COL32(255, 220, 120, 255) : IM_COL32(255, 255, 255, 230));
        }
        if (tiempoActual >= 0.0f)
        {
            const float x = aX(tiempoActual);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 120, 120, 200));
        }

        // The ends of the range: without them the height of the curve is a
        // guess, because the range adjusts to each track.
        char txt[32];
        std::snprintf(txt, sizeof(txt), "%.2f", hi);
        dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 1.0f), IM_COL32(150, 150, 160, 200), txt);
        std::snprintf(txt, sizeof(txt), "%.2f", lo);
        dl->AddText(ImVec2(p0.x + 3.0f, p1.y - 16.0f), IM_COL32(150, 150, 160, 200), txt);

        if (hover && cercana < 0 && !arrastrandoEsta)
            ImGui::SetTooltip("Drag a key to move it.\n"
                              "Double-click: new key.  Right-click on one: removes it.");
        return tocada;
    }

    // Read-only canvas of the 2D blend: the points with their clip, the edges
    // of the triangulation and the current value of (X, Y). The points are those of
    // stateBlendSamples: the main one and the entries with a resolved clip.
    void drawBlend2DCanvas(const AnimatorComponent& anim, const AnimatorComponent::State& st)
    {
        ImGui::PushID("lienzo2d");
        std::vector<glm::vec2>          pts;
        std::vector<const std::string*> nombres;
        pts.push_back({ st.clipThreshold, st.clipThresholdY });
        nombres.push_back(&st.clipName);
        for (const auto& e : st.blendEntries)
            if (e.clipIndex >= 0)
            {
                pts.push_back({ e.threshold, e.thresholdY });
                nombres.push_back(&e.clipName);
            }
        const glm::vec2 valor(anim.getFloat(st.blendParam), anim.getFloat(st.blendParamY));

        // Box of the points with a 10 % margin; the value is clamped to it.
        glm::vec2 lo = pts[0], hi = pts[0];
        for (const auto& p : pts) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
        glm::vec2 lado = glm::max(hi - lo, glm::vec2(1e-3f));
        lo -= lado * 0.1f;
        hi += lado * 0.1f;
        lado = hi - lo;

        const float tam = 160.0f;
        ImGui::Dummy(ImVec2(tam, tam));
        const ImVec2 r0 = ImGui::GetItemRectMin();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        // Y upwards, as in a graph.
        auto aPantalla = [&](glm::vec2 p) {
            const glm::vec2 n = glm::clamp((p - lo) / lado, glm::vec2(0.0f), glm::vec2(1.0f));
            return ImVec2(r0.x + n.x * tam, r0.y + (1.0f - n.y) * tam);
        };
        dl->AddRect(r0, ImVec2(r0.x + tam, r0.y + tam), IM_COL32(90, 90, 90, 255));
        for (const auto& t : triangulate2D(pts))
            dl->AddTriangle(aPantalla(pts[t.a]), aPantalla(pts[t.b]), aPantalla(pts[t.c]),
                            IM_COL32(140, 140, 140, 255));
        for (size_t i = 0; i < pts.size(); i++)
        {
            const ImVec2 p = aPantalla(pts[i]);
            dl->AddCircleFilled(p, 3.0f, IM_COL32(220, 220, 220, 255));
            dl->AddText(ImVec2(p.x + 4.0f, p.y - 14.0f), IM_COL32(200, 200, 200, 255), nombres[i]->c_str());
        }
        dl->AddCircleFilled(aPantalla(valor), 4.0f, IM_COL32(255, 150, 40, 255));
        ImGui::PopID();
    }
}

namespace {
    // Node editor IDs: they have to be != 0 and not collide between nodes,
    // pins and links. Three slots per state + a separate range for the links.
    //
    // The slot is indexed by State::editorId (stable, assigned once in
    // AnimatorComponent::addState), NEVER by the index in the vector of
    // states: that index changes when removeState reindexes after deleting a
    // state in the middle, and if the canvas id were the index, a
    // survivor would inherit the visual slot (position/selection) of the deleted
    // node, since imgui-node-editor caches those things by id, not by content.
    // The canvas ids live in Editor/AnimatorCanvasIds.h: they are the risky piece
    // of the canvas (a decoding failure deletes the wrong node
    // instead of giving an error), so they are kept outside to be able to test them.
    using namespace canvasIds;
    int nodeId(int eid)       { return canvasIds::node(eid); }
    int inputPinId(int eid)   { return canvasIds::inputPin(eid); }
    int outputPinId(int eid)  { return canvasIds::outputPin(eid); }
    int inputPinId2(int eid)  { return canvasIds::inputPin2(eid); }
    int outputPinId2(int eid) { return canvasIds::outputPin2(eid); }
    int linkId(int transIdx)  { return canvasIds::link(transIdx); }
    int editorIdFromRawId(int rawId) { return canvasIds::editorIdFrom(rawId); }
    // Curvature of the secondary pins. The library default is 100;
    // raising it opens up the curve, which is the other half of what separates the
    // return one from the outgoing one (the first is that it starts on another row).
    constexpr float kFuerzaLinkVuelta = 300.0f;

    // Any State node: ids OUTSIDE the scheme of the states (eid*5+1..5) and of
    // the links (100000+idx). They are ALWAYS checked before decoding with
    // editorIdFromRawId: passed through that formula they would match an editorId
    // (180000) that no graph reaches, but isOutputPin would classify them
    // wrongly, that is why esPinDeSalida.
    // Limits of the width of the left column (sources, layers, IK,
    // parameters), which the user drags by the edge. The minimum lets the
    // parameter list be seen, which is the widest widget; the maximum avoids leaving the
    // canvas at nothing in one drag.
    const float kAnchoColumnaMin = 200.0f;
    const float kAnchoColumnaMax = 700.0f;
    const float kAnchoAgarre     = 6.0f;

    const int kAnyStateNodeId   = canvasIds::kAnyStateNode;
    const int kAnyStateOutPinId = canvasIds::kAnyStateOutPin;

    bool esPinDeSalida(int pin) { return pin == kAnyStateOutPinId || (pin != kAnyStateNodeId && isOutputPin(pin)); }

    // A pin (or a node) only carries the stable editorId, and the transitions
    // store indices of the m_states vector (not editorIds) because that is the
    // contract of AnimatorComponent::Transition. This helper makes the bridge:
    // it decodes the editorId and looks it up in the states of the layer being
    // edited. It returns -1 if no live state has that id (it should not
    // happen: the ids that arrive here come from nodes/pins drawn this
    // same frame from the current states(layer)).
    int stateIndexFromPin(const AnimatorComponent& anim, int rawId, int capa)
    {
        return anim.stateIndexByEditorId(editorIdFromRawId(rawId), capa);
    }

    const char* condLabel(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animation finished";
            case AnimatorComponent::ConditionType::Int:               return "int";
            case AnimatorComponent::ConditionType::Float:             return "float";
            default:                                                  return "bool";
        }
    }

    // Compare labels in enum order. All four are offered for both Int and
    // Float: == on a float only fires with exact binary equality
    // (a computed value almost never meets it, one set with SetFloat does), but
    // trimming the combo hid half the API without saying so.
    const char* kCompareLabels[] = { ">", "<", "==", "!=" };
}

AnimatorPanel::AnimatorPanel()
{
    ed::Config config;
    // No settings file: the node positions live in the scene JSON
    // (AnimatorComponent::State::editorPos). If we left the default,
    // the node editor would write a parallel NodeEditor.json and there would be two
    // sources of truth fighting each other.
    config.SettingsFile = nullptr;
    m_ctx = ed::CreateEditor(&config);
    m_animSrcDialog = std::make_unique<IGFD::FileDialog>();
}

AnimatorPanel::~AnimatorPanel()
{
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void AnimatorPanel::syncPositionsFromComponent(GameObject* go)
{
    const auto& states = go->getAnimator()->states(m_layer);
    for (size_t i = 0; i < states.size(); i++)
        ed::SetNodePosition(nodeId(states[i].editorId), ImVec2(states[i].editorPos.x, states[i].editorPos.y));
    // The Any State node exists as long as there is at least one state.
    if (!states.empty())
    {
        const glm::vec2 p = go->getAnimator()->anyStateEditorPos(m_layer);
        ed::SetNodePosition(kAnyStateNodeId, ImVec2(p.x, p.y));
    }
}

void AnimatorPanel::syncPositionsToComponent(GameObject* go)
{
    auto& states = go->getAnimator()->statesMutable(m_layer);
    for (size_t i = 0; i < states.size(); i++)
    {
        const ImVec2 p = ed::GetNodePosition(nodeId(states[i].editorId));
        states[i].editorPos = glm::vec2(p.x, p.y);
    }
    if (!states.empty())
    {
        const ImVec2 p = ed::GetNodePosition(kAnyStateNodeId);
        go->getAnimator()->setAnyStateEditorPos(glm::vec2(p.x, p.y), m_layer);
    }
}

void AnimatorPanel::drawParameterList(EditorContext& ctx, GameObject* go)
{
    // Collapsible, like the animation sources, the layers and the IK: the
    // four sections of the column open and close the same way, and the
    // scroll is the column's. This used to be a child with its own height and
    // its own bar, which neither matched the rest nor was needed.
    ImGui::PushID("params");
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto anim = go->getAnimator();

        std::string toRemove;
        for (const auto& p : anim->parameters())
        {
            ImGui::PushID(p.name.c_str());
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", paramTypeLabel(p.type));
            ImGui::SameLine();

            // Value editable in place: in Play it allows triggering a transition by hand
            // without writing Lua, which is how a graph is debugged.
            ImGui::SetNextItemWidth(70);
            switch (p.type)
            {
                case AnimatorComponent::ParamType::Bool:
                {
                    bool v = anim->getBool(p.name);
                    if (ImGui::Checkbox("##val", &v)) anim->setBool(p.name, v);
                    break;
                }
                case AnimatorComponent::ParamType::Trigger:
                    // A trigger has no value to show: it is armed and consumed by the
                    // first transition that looks at it (see consumeTriggers).
                    if (ImGui::SmallButton("Set")) anim->setTrigger(p.name);
                    break;
                case AnimatorComponent::ParamType::Int:
                {
                    int v = anim->getInt(p.name);
                    if (ImGui::DragInt("##val", &v)) anim->setInt(p.name, v);
                    break;
                }
                case AnimatorComponent::ParamType::Float:
                {
                    float v = anim->getFloat(p.name);
                    if (ImGui::DragFloat("##val", &v, 0.01f)) anim->setFloat(p.name, v);
                    break;
                }
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("X")) toRemove = p.name;
            ImGui::PopID();
        }
        // Deferred: deleting inside the range-for would invalidate the iterator.
        if (!toRemove.empty())
        {
            m_graphUndo.setLabel("Remove parameter");
            anim->removeParameter(toRemove);
            ctx.pushLog("Animator: parameter '" + toRemove + "' removed");
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
        // The order matches that of the ParamType enum, so the combo index
        // casts directly: if the enum grows, this list grows with it.
        const char* types[] = { "bool", "trigger", "int", "float" };
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##newparamtype", &m_newParamType, types, IM_ARRAYSIZE(types));
        if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
        {
            m_graphUndo.setLabel("Add parameter");
            anim->addParameter(m_newParamName, (AnimatorComponent::ParamType)m_newParamType);
            ctx.pushLog(std::string("Animator: parameter '") + m_newParamName + "' added");
            m_newParamName[0] = '\0';
        }
    }
    ImGui::PopID();

}

void AnimatorPanel::drawLayerBar(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    ImGui::PushID("capas");
    // The clamping goes OUTSIDE the collapsible: the selected layer has to
    // remain valid even if the section is closed (the graph that is
    // drawn is its own).
    m_layer = std::clamp(m_layer, 0, anim->layerCount() - 1);
    if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int li = 0; li < anim->layerCount(); li++)
        {
            ImGui::PushID(li);
            if (m_renamingLayer == li)
            {
                if (m_focusRename) { ImGui::SetKeyboardFocusHere(); m_focusRename = false; }
                ImGui::SetNextItemWidth(200.0f);
                ImGui::InputText("##nombre", m_layerNameBuf, sizeof(m_layerNameBuf),
                                 ImGuiInputTextFlags_AutoSelectAll);
                // On losing focus (Enter, click outside): it is saved if it is not empty.
                if (ImGui::IsItemDeactivated())
                {
                    if (m_layerNameBuf[0] != '\0') anim->layerMutable(li).name = m_layerNameBuf;
                    m_renamingLayer = -1;
                }
            }
            else if (ImGui::Selectable(anim->layer(li).name.c_str(), m_layer == li,
                                       ImGuiSelectableFlags_AllowDoubleClick, ImVec2(200.0f, 0.0f)))
            {
                m_layer = li;
                m_nivelId = -1;   // the level belongs to the layer being left behind
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_renamingLayer = li;
                    m_focusRename   = true;
                    std::snprintf(m_layerNameBuf, sizeof(m_layerNameBuf), "%s", anim->layer(li).name.c_str());
                }
            }
            if (li == 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Base layer: always override, weight 1 and the whole body.");
            ImGui::PopID();
        }

        ImGui::BeginDisabled(anim->layerCount() >= AnimatorComponent::kMaxLayers);
        if (ImGui::Button("+##addLayer"))
        {
            const int n = anim->addLayer("Layer " + std::to_string(anim->layerCount()));
            if (n >= 0)
            {
                m_layer = n;
                m_nivelId = -1;   // the level belongs to the layer being left behind
                ctx.pushLog("Animator: layer '" + anim->layer(n).name + "' added");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_layer == 0);
        if (ImGui::Button("-##removeLayer"))
        {
            ctx.pushLog("Animator: layer '" + anim->layer(m_layer).name + "' removed");
            anim->removeLayer(m_layer);
            m_layer = std::min(m_layer, anim->layerCount() - 1);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_layer <= 1);
        if (ImGui::ArrowButton("##layerUp", ImGuiDir_Up))
        {
            anim->moveLayer(m_layer, m_layer - 1);
            m_layer--;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_layer == 0 || m_layer >= anim->layerCount() - 1);
        if (ImGui::ArrowButton("##layerDown", ImGuiDir_Down))
        {
            anim->moveLayer(m_layer, m_layer + 1);
            m_layer++;
        }
        ImGui::EndDisabled();

        // The base has no weight, mode or mask of its own.
        if (m_layer > 0)
        {
            const auto& L = anim->layer(m_layer);
            float peso = L.weight;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("Weight##lw", &peso, 0.0f, 1.0f, "%.2f"))
                anim->setLayerWeight(m_layer, peso);
            int modo = (int)L.mode;
            const char* modos[] = { "Override", "Additive" };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Mode##lm", &modo, modos, 2))
                anim->setLayerMode(m_layer, (AnimatorComponent::LayerMode)modo);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Override replaces the pose of the bones in the mask;\n"
                                  "Additive adds each clip's difference from its first frame.");
            const std::string etiqueta = L.maskBones.empty()
                ? std::string("Mask: whole body")
                : "Mask: " + std::to_string(L.maskBones.size()) + " bone(s)";
            if (ImGui::Button((etiqueta + "##lmask").c_str(), ImVec2(200.0f, 0.0f)))
                ImGui::OpenPopup("layerMask");
            drawLayerMaskPopup(go);
        }
    }
    ImGui::PopID();

}

void AnimatorPanel::drawIkList(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    ImGui::PushID("ik");
    if (ImGui::CollapsingHeader("IK"))
    {
        // Target and pole are scene GameObjects: they are listed in pre-order,
        // like the hierarchy panel, and the character itself is not included.
        std::vector<const GameObject*> objetos;
        std::function<void(const GameObject&)> recorrer = [&](const GameObject& n) {
            for (const auto& h : n.children)
            {
                objetos.push_back(h.get());
                recorrer(*h);
            }
        };
        if (ctx.scene) recorrer(ctx.scene->getRoot());
        auto selectorObjeto = [&](const char* etiqueta, uint64_t& id) {
            const GameObject* actual = nullptr;
            for (const auto* o : objetos)
                if (o->id == id) actual = o;
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo(etiqueta, actual ? actual->name.c_str() : "(none)"))
            {
                if (ImGui::Selectable("(none)", id == 0)) id = 0;
                for (const auto* o : objetos)
                {
                    if (o == go) continue;
                    ImGui::PushID((int)o->id);
                    if (ImGui::Selectable(o->name.c_str(), o->id == id)) id = o->id;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        };

        int quitar = -1;
        auto& lista = anim->ikConstraintsMutable();
        for (int i = 0; i < (int)lista.size(); i++)
        {
            auto& c = lista[i];
            ImGui::PushID(i);
            char nombre[64];
            std::snprintf(nombre, sizeof(nombre), "%s", c.name.c_str());
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputText("##nombre", nombre, sizeof(nombre))) c.name = nombre;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) quitar = i;

            int tipo = (int)c.type;
            const char* tipos[] = { "Look at", "Two bone" };
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Type##tipo", &tipo, tipos, 2))
            {
                c.type = (AnimatorComponent::IkType)tipo;
                // The chain (parent and grandparent) depends on the type: it has to be
                // re-resolved. rebindClips and not bindClips: Play Mode may be
                // running.
                if (mesh) anim->rebindClips(*mesh, nullptr);
            }

            // Bone: the one that looks (look-at) or the END of the chain (two bone).
            const bool roto = c.boneIndex < 0;
            if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("Bone##hueso", c.boneName.empty() ? "(choose bone)" : c.boneName.c_str()))
            {
                if (mesh)
                    for (const auto& n : mesh->skeleton.names)
                        if (ImGui::Selectable(n.c_str(), n == c.boneName))
                        {
                            c.boneName = n;
                            anim->rebindClips(*mesh, nullptr);
                        }
                ImGui::EndCombo();
            }
            if (roto) ImGui::PopStyleColor();
            if (roto && ImGui::IsItemHovered())
                ImGui::SetTooltip("The bone does not exist in the model, or the chain is shorter than three bones:\n"
                                  "the constraint is not applied.");

            selectorObjeto("Target##objetivo", c.targetId);
            if (c.type == AnimatorComponent::IkType::TwoBone)
            {
                selectorObjeto("Pole##pole", c.poleId);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Where the elbow or knee points.");
            }
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat("Weight##peso", &c.weight, 0.0f, 1.0f, "%.2f");
            if (c.type == AnimatorComponent::IkType::LookAt)
            {
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat3("Axis##eje", &c.aimAxis.x, 0.01f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("LOCAL axis of the bone that points at the target.");
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Max angle##ang", &c.maxAngle, 0.0f, 180.0f, "%.0f");
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (quitar >= 0) anim->removeIkConstraint(quitar);

        ImGui::BeginDisabled((int)lista.size() >= AnimatorComponent::kMaxIkConstraints);
        if (ImGui::Button("+ IK##addIk"))
        {
            AnimatorComponent::IkConstraint nueva;
            nueva.name = "IK " + std::to_string(anim->ikConstraints().size() + 1);
            anim->addIkConstraint(nueva);
            ctx.pushLog("Animator: IK constraint added");
        }
        ImGui::EndDisabled();
        if (!mesh) ImGui::TextDisabled("Without a skinned mesh there are no bones to choose.");
    }
    ImGui::PopID();
}

void AnimatorPanel::drawPropertyClips(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    ImGui::PushID("propclips");
    if (ImGui::CollapsingHeader("Property Clips"))
    {
        ImGui::TextDisabled("They animate the object: transform, light and material.");
        auto& clips = anim->propertyClipsMutable();
        int quitarClip = -1;
        for (int i = 0; i < (int)clips.size(); i++)
        {
            auto& clip = clips[(size_t)i];
            ImGui::PushID(i);
            // Header per clip, like the sections of the column. The "###" is
            // what makes it usable: without it the ID comes from the ENTIRE text, so
            // when renaming the clip (or when changing the glyph of a button that
            // depends on its own state) the header becomes another widget and
            // is lost if it was open. ImGui opens and closes by ID, so
            // each clip remembers its state by itself.
            const std::string etiqueta = (clip.name.empty() ? std::string("(unnamed)") : clip.name) +
                                         "###clip";
            // AllowOverlap: the header takes up the whole row, so without the
            // flag ImGui gives the click to it (the first item submitted) and the "x"
            // could never be pressed. Same pattern as the sources list.
            const bool abierto = ImGui::CollapsingHeader(etiqueta.c_str(),
                                                         ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
            if (ImGui::SmallButton("x###quitarClip")) quitarClip = i;
            if (abierto)
            {
                char nombre[64];
                std::snprintf(nombre, sizeof(nombre), "%s", clip.name.c_str());
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::InputText("Name###nombre", nombre, sizeof(nombre)))
                {
                    clip.name = nombre;
                    // The state references by NAME: renaming forces a re-resolve.
                    anim->bindProperties(go, nullptr);
                }
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::DragFloat("Duration (s)###dur", &clip.duration, 0.01f, 0.001f, 600.0f, "%.3f"))
                {
                    if (clip.duration < 0.001f) clip.duration = 0.001f;
                    // The duration of the state comes from here when there is no mesh clip.
                    anim->bindProperties(go, nullptr);
                }

                // Where the playhead of THIS clip is, if it is playing now: the
                // same list of samples that is applied to the object says so,
                // so the preview and the drawing cannot disagree.
                float tiempoClip = -1.0f;
                {
                    AnimatorComponent::PropertySampleRef ms[kMaxLayersPose * kMaxPoseSamplesPerLayer];
                    const int nm = anim->propertySamples(ms, (int)(sizeof(ms) / sizeof(ms[0])));
                    for (int k = 0; k < nm; k++)
                        if (ms[k].clip == i) { tiempoClip = ms[k].time; break; }
                }

                int quitarPista = -1;
                for (int p = 0; p < (int)clip.tracks.size(); p++)
                {
                    auto& pista = clip.tracks[(size_t)p];
                    ImGui::PushID(p);
                    const bool roto = !pista.resolved;
                    if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    // Target: a property of the object or a Float parameter of the
                    // Animator (a clip curve).
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::BeginCombo("###destino",
                                          pista.target == TrackTarget::Parameter ? "Parameter" : "Property"))
                    {
                        if (ImGui::Selectable("Property", pista.target == TrackTarget::Property))
                        {
                            pista.target = TrackTarget::Property;
                            anim->bindProperties(go, nullptr);
                        }
                        if (ImGui::Selectable("Parameter", pista.target == TrackTarget::Parameter))
                        {
                            pista.target = TrackTarget::Parameter;
                            anim->bindProperties(go, nullptr);
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    if (pista.target == TrackTarget::Parameter)
                    {
                        // Only Floats: a curve cannot write anything else.
                        // The current name is shown even if the parameter no longer exists
                        // (the track shows up in red), so as not to lose it silently.
                        const char* actual = pista.parameterName.empty() ? "(no parameter)"
                                                                         : pista.parameterName.c_str();
                        if (ImGui::BeginCombo("###param", actual))
                        {
                            for (const auto& prm : anim->parameters())
                            {
                                if (prm.type != AnimatorComponent::ParamType::Float) continue;
                                if (ImGui::Selectable(prm.name.c_str(), prm.name == pista.parameterName))
                                {
                                    pista.parameterName = prm.name;
                                    anim->bindProperties(go, nullptr);
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    else if (ImGui::BeginCombo("###prop", propertyName(pista.property)))
                    {
                        for (int q = 0; q < (int)PropertyId::Count; q++)
                        {
                            const PropertyId id = (PropertyId)q;
                            if (ImGui::Selectable(propertyName(id), id == pista.property))
                            {
                                pista.property = id;
                                anim->bindProperties(go, nullptr);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (roto) ImGui::PopStyleColor();
                    if (roto && ImGui::IsItemHovered())
                        ImGui::SetTooltip(pista.target == TrackTarget::Parameter
                                              ? "There is no Float parameter with that name:\n"
                                                "the curve is not applied."
                                              : "The object lacks the component this track needs:\n"
                                                "it is not applied.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x###pista")) quitarPista = p;

                    // A curve is read against the thresholds of the conditions
                    // that look at its parameter; a property track has none
                    // to paint.
                    float umbrales[4];
                    const int numUmbrales = pista.target == TrackTarget::Parameter
                                                ? anim->conditionThresholds(pista.parameterName, umbrales, 4)
                                                : 0;
                    // The canvas edits the keys with the mouse. The ID comes from the
                    // PushID(p) of this track, inside the PushID(i) of the clip:
                    // that is what tells one canvas from another when there are
                    // several open at the same time.
                    dibujarCurva(pista, clip.duration, tiempoClip, umbrales, numUmbrales);

                    int quitarKey = -1;
                    for (int k = 0; k < (int)pista.keys.size(); k++)
                    {
                        ImGui::PushID(k);
                        ImGui::SetNextItemWidth(60.0f);
                        ImGui::DragFloat("###t", &pista.keys[(size_t)k].time, 0.01f, 0.0f, 600.0f, "t %.2f");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(70.0f);
                        ImGui::DragFloat("###v", &pista.keys[(size_t)k].value, 0.01f, 0.0f, 0.0f, "v %.3f");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x###key")) quitarKey = k;
                        ImGui::PopID();
                    }
                    if (quitarKey >= 0) pista.keys.erase(pista.keys.begin() + quitarKey);
                    if (ImGui::SmallButton("+ key"))
                    {
                        // The new one goes at the end of the clip with the value the
                        // object has now: this is how you author "from here".
                        PropertyKey nueva;
                        nueva.time  = pista.keys.empty() ? 0.0f : clip.duration;
                        nueva.value = pista.target == TrackTarget::Parameter
                                          ? anim->getFloat(pista.parameterName)
                                          : propertyGet(*go, pista.property);
                        pista.keys.push_back(nueva);
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (quitarPista >= 0) clip.tracks.erase(clip.tracks.begin() + quitarPista);
                if (ImGui::SmallButton("+ track"))
                {
                    clip.tracks.push_back(PropertyTrack{});
                    anim->bindProperties(go, nullptr);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+ curve"))
                {
                    // A curve writes a parameter; it is created on the first
                    // Float there is, and if there is none it shows up in red until
                    // one is declared.
                    PropertyTrack curva;
                    curva.target = TrackTarget::Parameter;
                    for (const auto& prm : anim->parameters())
                        if (prm.type == AnimatorComponent::ParamType::Float) { curva.parameterName = prm.name; break; }
                    clip.tracks.push_back(curva);
                    anim->bindProperties(go, nullptr);
                }
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (quitarClip >= 0)
        {
            anim->removePropertyClip(quitarClip);
            anim->bindProperties(go, nullptr);
        }

        ImGui::BeginDisabled((int)clips.size() >= AnimatorComponent::kMaxPropertyClips);
        if (ImGui::Button("+ clip##addPropClip"))
        {
            PropertyClip nuevo;
            nuevo.name = "Clip " + std::to_string(anim->propertyClips().size() + 1);
            if (anim->addPropertyClip(nuevo) >= 0)
                ctx.pushLog("Animator: property clip '" + nuevo.name + "' added");
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

void AnimatorPanel::drawLayerMaskPopup(GameObject* go)
{
    if (!ImGui::BeginPopup("layerMask")) return;
    auto anim = go->getAnimator();
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    auto& L = anim->layerMutable(m_layer);
    bool cambio = false;
    if (ImGui::Button("Whole body"))
    {
        L.maskBones.clear();
        cambio = true;
    }
    if (!mesh)
    {
        ImGui::TextDisabled("The GameObject has no skinned mesh.");
    }
    else
    {
        ImGui::TextDisabled("Click: the bone and its branch. Ctrl+click: the bone only.");
        const auto& nombres = mesh->skeleton.names;
        const auto& padres  = mesh->skeleton.parentIndex;
        const int   n       = (int)nombres.size();
        std::vector<uint8_t> marcado((size_t)n, 0);
        for (const auto& b : L.maskBones)
            for (int i = 0; i < n; i++)
                if (nombres[(size_t)i] == b) marcado[(size_t)i] = 1;
        std::vector<std::vector<int>> hijos((size_t)n);
        std::vector<int> raices;
        for (int i = 0; i < n; i++)
        {
            const int p = i < (int)padres.size() ? padres[(size_t)i] : -1;
            if (p >= 0 && p < n) hijos[(size_t)p].push_back(i); else raices.push_back(i);
        }
        // The branch from b to v: the bone and all its descendants.
        auto ponerRama = [&](int b, bool v) {
            std::vector<int> pila = { b };
            while (!pila.empty())
            {
                const int x = pila.back();
                pila.pop_back();
                marcado[(size_t)x] = v ? 1 : 0;
                for (int h : hijos[(size_t)x]) pila.push_back(h);
            }
        };
        std::function<void(int)> nodo = [&](int b) {
            ImGui::PushID(b);
            const bool hoja = hijos[(size_t)b].empty();
            bool v = marcado[(size_t)b] != 0;
            if (ImGui::Checkbox("##m", &v))
            {
                if (ImGui::GetIO().KeyCtrl) marcado[(size_t)b] = v ? 1 : 0;
                else                        ponerRama(b, v);
                cambio = true;
            }
            ImGui::SameLine();
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_DefaultOpen;
            if (hoja) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            const bool abierto = ImGui::TreeNodeEx(nombres[(size_t)b].c_str(), fl);
            if (abierto && !hoja)
            {
                for (int h : hijos[(size_t)b]) nodo(h);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        // Height according to the screen (a 60-bone skeleton does not fit in 420 px)
        // and horizontal scroll: each level indents, and in a deep branch the
        // name ran off the right with no way to see it.
        const float alto = std::clamp(ImGui::GetMainViewport()->WorkSize.y * 0.6f, 240.0f, 900.0f);
        ImGui::BeginChild("arbolMascara", ImVec2(420.0f, alto), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (int r : raices) nodo(r);
        ImGui::EndChild();
        if (cambio)
        {
            L.maskBones.clear();
            for (int i = 0; i < n; i++)
                if (marcado[(size_t)i]) L.maskBones.push_back(nombres[(size_t)i]);
        }
    }
    // rebindClips and not bindClips: Play Mode may be running.
    if (cambio && mesh) anim->rebindClips(*mesh, nullptr);
    ImGui::EndPopup();
}

int AnimatorPanel::nivelActual(const AnimatorComponent& anim) const
{
    if (m_nivelId < 0) return -1;
    const int idx = anim.stateIndexByEditorId(m_nivelId, m_layer);
    // The box is no longer there (it was deleted while inside) or stopped being one: root.
    if (idx < 0 || !anim.states(m_layer)[(size_t)idx].isSubMachine) return -1;
    return idx;
}

void AnimatorPanel::drawGraph(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    // The level is resolved ONCE per frame from the editorId: within the
    // frame the indices do not move, and between frames the id survives
    // deletions.
    const int nivel = nivelActual(*anim);
    if (nivel < 0) m_nivelId = -1;   // the box went away: do not leave the id dangling

    // Breadcrumb of the level, above the canvas: it is the only way to leave a
    // box, and to see where you are when the visible graph is not the root one.
    {
        const auto& sts = anim->states(m_layer);
        if (ImGui::SmallButton("Base###nivelRaiz")) m_nivelId = -1;
        std::vector<int> cadena;
        for (int n = nivel; n >= 0 && n < (int)sts.size(); n = sts[(size_t)n].parent)
        {
            cadena.push_back(n);
            if ((int)cadena.size() > (int)sts.size()) break;   // broken hierarchy: do not hang
        }
        for (int k = (int)cadena.size() - 1; k >= 0; k--)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("/");
            ImGui::SameLine();
            ImGui::PushID(cadena[(size_t)k]);
            if (ImGui::SmallButton(sts[(size_t)cadena[(size_t)k]].name.c_str()))
                m_nivelId = sts[(size_t)cadena[(size_t)k]].editorId;
            ImGui::PopID();
        }
    }

    ed::SetCurrentEditor(m_ctx);
    // The tooltip of a widget inside a node can NOT be drawn here:
    // between ed::Begin and ed::End the mouse and the windows are in canvas
    // coordinates, so it would come out displaced by the pan and the zoom, and drawing it with
    // ed::Suspend inside a node breaks the canvas channel splitter
    // (IM_ASSERT in imgui_canvas.cpp: the application gets stuck
    // when dragging a node). The text is noted down and painted at the end, already outside.
    m_tooltipNodo.clear();
    ed::Begin("AnimatorCanvas");
    // When typing in a field of a node (event name), Del would delete the
    // selected node: imgui-node-editor looks at the key, not at the text focus.
    ed::EnableShortcuts(!ImGui::GetIO().WantTextInput);

    // --- Nodes ---
    const auto& states = anim->states(m_layer);
    // The level may have been left pointing to a state that no longer exists (the
    // box that was being looked at from the inside was deleted): we go back to the root.

    // Pin row of the node: used by the normal state and the box, which is drawn
    // without clip, loop or events. A single copy so both come out the same.
    auto pinesDelNodo = [](int eid, float headerW) {
        // "-> in" on the left, "out ->" pushed to the right edge of the node
        // (the width of the header is the real width of the node).
        ed::BeginPin(inputPinId(eid), ed::PinKind::Input);
        ImGui::TextUnformatted("-> in");
        ed::EndPin();
        ImGui::SameLine();
        const float inW  = ImGui::CalcTextSize("-> in").x;
        const float outW = ImGui::CalcTextSize("out ->").x;
        const float pad  = headerW - inW - outW;
        // Very narrow node: a small fixed gap is enough so they do not overlap.
        ImGui::Dummy(ImVec2(pad > 1.0f ? pad : 8.0f, 0.0f));
        ImGui::SameLine();
        ed::BeginPin(outputPinId(eid), ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();

        // SECONDARY pair, for the return transition of a mutual pair. It goes on
        // its own row (below) and with more curvature, which are the two things
        // that separate its curve from the outgoing one: the start and the shape. The
        // curvature is a property of the pin, so it is pushed HERE and not when
        // drawing the link.
        //
        // Without text: they are anchor points, not something the user has
        // to aim at. They are always drawn (not only when there is a mutual pair) so
        // that the node does not change height depending on its transitions, which would
        // make the layout dance when creating or deleting one.
        ed::PushStyleVar(ed::StyleVar_LinkStrength, kFuerzaLinkVuelta);
        ed::BeginPin(inputPinId2(eid), ed::PinKind::Input);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ed::EndPin();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(pad > 1.0f ? pad : 8.0f, 0.0f));
        ImGui::SameLine();
        ed::BeginPin(outputPinId2(eid), ed::PinKind::Output);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ed::EndPin();
        ed::PopStyleVar();
    };
    for (size_t i = 0; i < states.size(); i++)
    {
        // Only what belongs to THIS level: the children of a box are seen when entering it.
        if (states[i].parent != nivel) continue;
        const int eid = states[i].editorId;
        ed::BeginNode(nodeId(eid));

        // Header (name/entry, clip, loop) grouped so its width can be measured
        // and thus know where the right edge of the node falls: the pin row
        // below needs it to separate "-> in" (left) from "out ->" (right).
        ImGui::BeginGroup();

        const bool isEntry = ((int)i == anim->entryState(m_layer));
        if (isEntry)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", states[i].name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(entry)");
        }
        else
        {
            ImGui::TextUnformatted(states[i].name.c_str());
        }

        // A box plays nothing: instead of the clip it shows where it is entered
        // and warns when it is empty, which is when the transitions into it
        // do not fire.
        if (states[i].isSubMachine)
        {
            ImGui::TextDisabled("sub-state machine");
            const int ent = states[i].subEntry;
            if (ent >= 0 && ent < (int)states.size())
                ImGui::TextDisabled("enters through: %s", states[(size_t)ent].name.c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "no entry: it cannot be entered");
        }
        // clipIndex < 0: the graph clip does not exist in the model (bindClips already
        // warned on load). It is marked here too or the node would lie.
        else if (states[i].clipIndex < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "clip: %s (does not exist)", states[i].clipName.c_str());
        else
            ImGui::TextDisabled("clip: %s", states[i].clipName.c_str());

        ImGui::PushID((int)i);

        // Hierarchy: which box it is in, and, if it is a box, where it is entered.
        // Buttons and not combos: a list inside the node opens in canvas space
        // (see drawBlendPickPopup).
        {
            const int pa = states[i].parent;
            const std::string etiqueta =
                std::string("parent: ") +
                (pa >= 0 && pa < (int)states.size() ? states[(size_t)pa].name : std::string("(root)")) +
                "###padre";
            if (ImGui::SmallButton(etiqueta.c_str()))
            {
                m_blendPickRequested = true;
                m_blendPickEditorId  = eid;
                m_blendPickKind      = 4;
            }
            if (states[i].isSubMachine)
            {
                const int ent = states[i].subEntry;
                const std::string etEnt =
                    std::string("entry: ") +
                    (ent >= 0 && ent < (int)states.size() ? states[(size_t)ent].name : std::string("(none)")) +
                    "###entrada";
                if (ImGui::SmallButton(etEnt.c_str()))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 5;
                }
            }
        }

        // A box plays no clip, so loop, speed, root motion,
        // blend and events do not apply to it: all of that is skipped.
        if (states[i].isSubMachine)
        {
            ImGui::PopID();
            ImGui::EndGroup();
            pinesDelNodo(eid, ImGui::GetItemRectSize().x);
            ed::EndNode();
            continue;
        }

        bool loop = states[i].loop;
        if (ImGui::Checkbox("loop", &loop))
            anim->statesMutable(m_layer)[i].loop = loop;

        // Root mode. RadioButton and not combo: a list inside the node
        // opens in canvas space (see drawBlendPickPopup).
        {
            using RM = AnimatorComponent::RootMotion;
            int modo = (int)states[i].rootMotion;
            ImGui::TextUnformatted("root:");
            ImGui::SameLine();
            if (ImGui::RadioButton("normal##rm", modo == 0)) modo = 0;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "The pose moves the root: the clip travels with its animation.";
            ImGui::SameLine();
            if (ImGui::RadioButton("locked##rm", modo == 1)) modo = 1;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "Pins the root's translation to its bind pose: the clip plays in place. The root's rotation and the rest of the bones animate as usual.";
            ImGui::SameLine();
            if (ImGui::RadioButton("root motion##rm", modo == 2)) modo = 2;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "The root's horizontal motion moves the GameObject (as velocity with a dynamic Rigidbody). Y stays in the pose and rotation is not applied.";
            if (modo != (int)states[i].rootMotion)
                anim->statesMutable(m_layer)[i].rootMotion = (RM)modo;
        }

        // Speed of the state and float parameter that multiplies it (optional).
        // Live DragFloat: the undo is picked up by the graph tracker.
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragFloat("speed", &anim->statesMutable(m_layer)[i].speed, 0.01f, 0.0f, 100.0f, "%.2f");
        if (anim->statesMutable(m_layer)[i].speed < 0.0f) anim->statesMutable(m_layer)[i].speed = 0.0f;
        if (ImGui::IsItemHovered())
            m_tooltipNodo = "Multiplies the clip's pace (1 = normal, 0 = frozen).";
        ImGui::SameLine();
        {
            const std::string& sp = states[i].speedParam;
            const std::string etiqueta = "x " + (sp.empty() ? std::string("(none)") : sp) + "##speedparam";
            if (ImGui::Button(etiqueta.c_str()))
            {
                m_blendPickRequested = true;
                m_blendPickEditorId  = eid;
                m_blendPickKind      = 2;
            }
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "Float parameter that multiplies the speed (like Unity's Multiplier).";
        }

        // --- 1D blend: extra clips with their threshold ---
        // Each row opens the list of ALL the clips of the mesh with a button and
        // not BeginCombo: the list opens outside the node (see
        // drawBlendPickPopup). An entry without a resolved clip shows up in red.
        auto& stMut = anim->statesMutable(m_layer)[i];
        if (go->getSkinnedMesh())
        {
            if (!stMut.blendEntries.empty())
            {
                // Only float parameters: they are the only ones that give a continuous
                // weight. The list coming out empty is the hint that one has to be
                // declared below, in Parameters.
                const std::string paramLabel = stMut.blendParam.empty()
                                               ? std::string("(no parameter)") : stMut.blendParam;
                if (ImGui::Button(("by: " + paramLabel + "##by").c_str(), ImVec2(140.0f, 0.0f)))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 1;
                }
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragFloat("threshold##clipThr", &stMut.clipThreshold, 0.01f);
                if (ImGui::IsItemHovered())
                    m_tooltipNodo = "Threshold of the state's main clip.";

                // --- 2D blend ---
                // With a second parameter each clip is a point (threshold, Y) and
                // the 3 of the triangle where (X, Y) falls play.
                bool dosD = !stMut.blendParamY.empty();
                if (ImGui::Checkbox("2D##blend2d", &dosD))
                {
                    if (dosD)
                    {
                        // The first Float that is not the X one; if there is no other, the
                        // same one (it is changed in the selector). Without any Float the
                        // checkbox does not stay checked.
                        std::string elegido;
                        for (const auto& p : anim->parameters())
                        {
                            if (p.type != AnimatorComponent::ParamType::Float) continue;
                            if (elegido.empty()) elegido = p.name;
                            if (p.name != stMut.blendParam) { elegido = p.name; break; }
                        }
                        stMut.blendParamY = elegido;
                    }
                    else
                        stMut.blendParamY.clear();
                }
                if (ImGui::IsItemHovered())
                    m_tooltipNodo = "Blend 2D: each clip is a point (X threshold, Y threshold).";
                if (!stMut.blendParamY.empty())
                {
                    if (ImGui::Button(("Y: " + stMut.blendParamY + "##byY").c_str(), ImVec2(140.0f, 0.0f)))
                    {
                        m_blendPickRequested = true;
                        m_blendPickEditorId  = eid;
                        m_blendPickKind      = 3;
                    }
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::DragFloat("Y##clipThrY", &stMut.clipThresholdY, 0.01f);
                    if (ImGui::IsItemHovered())
                        m_tooltipNodo = "Y threshold of the state's main clip.";
                }
            }

            int quitar = -1;
            for (int k = 0; k < (int)stMut.blendEntries.size(); k++)
            {
                auto& e = stMut.blendEntries[k];
                ImGui::PushID(k);
                const bool roto = e.clipIndex < 0;
                if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                const std::string lbl = e.clipName.empty() ? std::string("(choose clip)") : e.clipName;
                if (ImGui::Button((lbl + "##clip").c_str(), ImVec2(90.0f, 0.0f)))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 0;
                    m_blendPickEntry     = k;
                }
                if (roto) ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(50.0f);
                ImGui::DragFloat("##thr", &e.threshold, 0.01f);
                if (!stMut.blendParamY.empty())
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50.0f);
                    ImGui::DragFloat("##thrY", &e.thresholdY, 0.01f);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) quitar = k;
                ImGui::PopID();
            }
            if (quitar >= 0)
                stMut.blendEntries.erase(stMut.blendEntries.begin() + quitar);

            if (ImGui::Button("+ blend clip##addBlend", ImVec2(140.0f, 0.0f)))
            {
                // Threshold above all the others: the new entry does not steal weight
                // from the ones already there until the user moves it.
                float maxT = stMut.clipThreshold;
                for (const auto& e : stMut.blendEntries) maxT = std::max(maxT, e.threshold);
                AnimatorComponent::BlendEntry nueva;
                nueva.threshold = maxT + 1.0f;
                stMut.blendEntries.push_back(nueva);
            }

            if (!stMut.blendEntries.empty() && !stMut.blendParamY.empty())
                drawBlend2DCanvas(*anim, stMut);
        }

        // --- Property clip of the state ---
        // What allows an object WITHOUT a skeleton to have states: here it is
        // chosen which authored clip each one plays.
        if (!anim->propertyClips().empty())
        {
            ImGui::PushID("propclip");
            ImGui::SetNextItemWidth(150.0f);
            const bool roto = !stMut.propertyClipName.empty() && stMut.propertyClipIndex < 0;
            if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            if (ImGui::BeginCombo("##propclip",
                                  stMut.propertyClipName.empty() ? "(no property clip)"
                                                                 : stMut.propertyClipName.c_str()))
            {
                if (ImGui::Selectable("(none)", stMut.propertyClipName.empty()))
                {
                    stMut.propertyClipName.clear();
                    anim->bindProperties(go, nullptr);
                }
                for (const auto& pc : anim->propertyClips())
                    if (ImGui::Selectable(pc.name.c_str(), pc.name == stMut.propertyClipName))
                    {
                        stMut.propertyClipName = pc.name;
                        anim->bindProperties(go, nullptr);
                    }
                ImGui::EndCombo();
            }
            if (roto) ImGui::PopStyleColor();
            ImGui::PopID();
        }

        // --- State events ---
        // They do not depend on the mesh: they are named instants of the cycle, which in
        // Play reach Lua as OnAnimationEvent.
        ImGui::PushID("eventos");
        int quitarEvento = -1;
        for (int k = 0; k < (int)stMut.events.size(); k++)
        {
            auto& ev = stMut.events[k];
            ImGui::PushID(k);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", ev.name.c_str());
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputText("##evName", buf, sizeof(buf))) ev.name = buf;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            ImGui::DragFloat("##evTime", &ev.time, 0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "Point in the cycle, normalized (0 = start, 1 = end).";
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) quitarEvento = k;
            ImGui::PopID();
        }
        if (quitarEvento >= 0)
            stMut.events.erase(stMut.events.begin() + quitarEvento);
        if (ImGui::Button("+ event##addEvent", ImVec2(140.0f, 0.0f)))
            stMut.events.push_back({ "", 0.5f });
        ImGui::PopID();
        ImGui::PopID();

        ImGui::EndGroup();
        const float headerW = ImGui::GetItemRectSize().x;

        pinesDelNodo(eid, headerW);

        ed::EndNode();
    }

    // --- Any State node ---
    // Only if there are states: without them there is nowhere to go. It only has an output.
    if (!states.empty())
    {
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.30f, 0.20f, 0.45f, 0.90f));
        ed::BeginNode(kAnyStateNodeId);
        ImGui::TextUnformatted("Any State");
        ed::BeginPin(kAnyStateOutPinId, ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();
        ed::EndNode();
        ed::PopStyleColor();
    }

    // --- Links ---
    const auto& transitions = anim->transitions(m_layer);
    for (size_t t = 0; t < transitions.size(); t++)
    {
        const int from = transitions[t].fromState;
        const int to   = transitions[t].toState;
        // fromState/toState are indices of the m_states vector (not editorIds: that
        // is the Transition contract, see the comment in the header). They have to be
        // converted to editorId before building the canvas pin ids.
        const bool desdeAny = from == AnimatorComponent::kAnyState;
        if ((!desdeAny && (from < 0 || from >= (int)states.size())) ||
            to < 0 || to >= (int)states.size()) continue;
        // An end that lives inside another box is drawn AGAINST the box: if
        // not, the transition would disappear from the graph and seem not to exist.
        auto visible = [&](int estado) {
            int s = estado;
            while (s >= 0 && s < (int)states.size() && states[(size_t)s].parent != nivel)
                s = states[(size_t)s].parent;
            return s;
        };
        const int vFrom = desdeAny ? from : visible(from);
        const int vTo   = visible(to);
        // Outside this branch, or both ends on the same visible node: there is
        // nothing to draw at this level.
        if ((!desdeAny && vFrom < 0) || vTo < 0 || (!desdeAny && vFrom == vTo)) continue;

        // Two states linked in BOTH directions: the return curve has
        // to go around both nodes and ends up passing over the outgoing one. The return
        // one hangs from the SECONDARY pin pair, which is on another row and with
        // more curvature, so the two are read separately.
        //
        // "The return one" is the one that appears LATER in the vector: it is decided by
        // order and not by geometry, so that moving a node does not reorder the
        // curves while dragging. The VISIBLE ends at
        // this level are compared, so it also separates two transitions that end in the
        // same box.
        bool esVuelta = false;
        if (!desdeAny)
        {
            for (size_t u = 0; u < t; u++)
            {
                const int uFrom = transitions[u].fromState;
                const int uTo   = transitions[u].toState;
                if (uFrom == AnimatorComponent::kAnyState) continue;
                if (uFrom < 0 || uFrom >= (int)states.size() || uTo < 0 || uTo >= (int)states.size()) continue;
                if (visible(uFrom) == vTo && visible(uTo) == vFrom) { esVuelta = true; break; }
            }
        }
        const int pinSalida = desdeAny ? kAnyStateOutPinId
                                       : (esVuelta ? outputPinId2(states[vFrom].editorId)
                                                   : outputPinId(states[vFrom].editorId));
        const int pinEntrada = esVuelta ? inputPinId2(states[vTo].editorId)
                                        : inputPinId(states[vTo].editorId);
        ed::Link(linkId((int)t), pinSalida, pinEntrada);
    }

    // --- Create links by dragging from pin to pin ---
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b)
        {
            const int pa = (int)a.Get();
            const int pb = (int)b.Get();
            // The user can drag in any direction: it is normalized to
            // (output -> input).
            const int outPin = esPinDeSalida(pa) ? pa : pb;
            const int inPin  = esPinDeSalida(pa) ? pb : pa;

            if (esPinDeSalida(outPin) && !esPinDeSalida(inPin) && inPin != kAnyStateNodeId &&
                ed::AcceptNewItem())
            {
                // stateFromPin (index) instead of directly the editorId: the
                // transitions store vector indices, not editorIds. The Any State
                // pin is not decoded: it is the sentinel.
                const int fromIdx = (outPin == kAnyStateOutPinId)
                                    ? AnimatorComponent::kAnyState
                                    : stateIndexFromPin(*anim, outPin, m_layer);
                const int toIdx   = stateIndexFromPin(*anim, inPin, m_layer);
                if ((fromIdx >= 0 || fromIdx == AnimatorComponent::kAnyState) && toIdx >= 0)
                {
                    AnimatorComponent::Transition tr;
                    tr.fromState = fromIdx;
                    tr.toState   = toIdx;
                    // Without conditions it never fires (by design): the user
                    // adds them by double-clicking the link.
                    m_graphUndo.setLabel("Create transition");
                    anim->addTransition(tr, m_layer);
                    ctx.pushLog("Animator: transition created (no conditions yet)");
                }
            }
        }
    }
    ed::EndCreate();

    // --- Delete nodes and links ---
    if (ed::BeginDelete())
    {
        // With box-select + Del, a single BeginDelete pass can bring
        // several ids (several QueryDeletedNode/QueryDeletedLink). If each one were
        // deleted as it is accepted, the first erase reindexes the vector and the
        // ids already queued (computed before that erase) end up pointing to
        // another element or fall out of range: removeState/removeTransition
        // do a silent bounds-check and that element survives with no further warning.
        // That is why all the indices are collected first and deleted afterwards, from
        // back to front: this way each erase only shifts indices already
        // processed, never the ones still pending in this same pass.
        std::vector<int> transitionsToRemove;
        ed::LinkId dl;
        while (ed::QueryDeletedLink(&dl))
            if (ed::AcceptDeletedItem())
                transitionsToRemove.push_back((int)dl.Get() - 100000);

        // The node id that QueryDeletedNode brings is a nodeId(editorId): it is
        // decoded to editorId and resolved to the current index of the vector by
        // scanning by editorId (stateIndexFromPin works just as well here despite the
        // name: decode + scan is exactly the same for a node id as
        // for a pin one, the formula is the same integer division).
        std::vector<int> statesToRemove;
        ed::NodeId dn;
        while (ed::QueryDeletedNode(&dn))
        {
            // The Any State node is not deleted: it exists as long as there are states.
            if ((int)dn.Get() == kAnyStateNodeId) { ed::RejectDeletedItem(); continue; }
            if (ed::AcceptDeletedItem())
            {
                const int idx = stateIndexFromPin(*anim, (int)dn.Get(), m_layer);
                if (idx >= 0) statesToRemove.push_back(idx);
            }
        }

        std::sort(transitionsToRemove.rbegin(), transitionsToRemove.rend());
        for (int idx : transitionsToRemove)
            anim->removeTransition(idx, m_layer);

        // The explicitly requested links are deleted before the states: a
        // deleted state also takes its transitions with it (removeState
        // reindexes/purges them), so processing the loose links first avoids
        // stepping on that automatic purge. The descending-erase is still
        // necessary with multi-delete: each removeState shifts the indices above
        // idx, so we have to go from back to front so that each
        // erase does not invalidate the indices already computed and pending in this same
        // vector (statesToRemove are indices taken BEFORE deleting anything).
        if (!statesToRemove.empty()) m_graphUndo.setLabel("Delete state");
        std::sort(statesToRemove.rbegin(), statesToRemove.rend());
        for (int idx : statesToRemove)
            // removeState reindexes the surviving transitions.
            anim->removeState(idx, m_layer);

        // Unlike before (Task 10), it is NO LONGER necessary to unlink
        // m_boundTo here: with stable ids by editorId, a survivor does not
        // change id when the vector is reindexed, so its node in the canvas
        // is still the same node (same id) with the same position: there is no
        // visual slot to inherit from the deleted one. syncPositionsToComponent further
        // below can read the positions this same frame without corrupting anything.
    }
    ed::EndDelete();

    // --- Context menus ---
    ed::Suspend();
    ed::NodeId ctxNode;
    ed::LinkId ctxLink;
    if (ed::ShowNodeContextMenu(&ctxNode))
    {
        ImGui::OpenPopup("node_ctx");
        m_conditionsFor = -1;
        // Plain member instead of ImGui::GetStateStorage(): only one node popup
        // can be open at a time, so the indirection of the ImGui state storage is
        // not needed to smuggle the index to the deferred popup: an int in the panel
        // gets just as far.
        // ctxNode.Get() decodes to an editorId, not to the vector index that
        // "Set as Entry" needs (setEntryState(idx)), hence the step through
        // stateIndexFromPin.
        m_nodeCtxTarget = stateIndexFromPin(*anim, (int)ctxNode.Get(), m_layer);
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        // No intermediate "Edit Conditions..." popup: opening "conditions" from
        // inside the BeginPopup/EndPopup of another popup (link_ctx) nested one
        // OpenPopup inside another, fragile and flaky. A right click on the link
        // opens the conditions editor directly.
        m_conditionsFor = (int)ctxLink.Get() - 100000;
        ImGui::OpenPopup("conditions");
    }

    if (ImGui::BeginPopup("node_ctx"))
    {
        const int idx = m_nodeCtxTarget;
        if (idx >= 0 && idx < (int)anim->states(m_layer).size())
        {
            if (ImGui::MenuItem("Set as Entry"))
            {
                anim->setEntryState(idx, m_layer);
                ctx.pushLog("Animator: '" + anim->states(m_layer)[idx].name + "' is now the entry state");
            }
        }
        ImGui::EndPopup();
    }
    drawConditionsPopup(ctx, go);
    drawBlendPickPopup(go);
    ed::Resume();

    // Double click on a box: we enter to see what it has inside. It is queried
    // BEFORE ed::End, which is where the canvas still has the frame state.
    if (ed::NodeId doble = ed::GetDoubleClickedNode())
    {
        // The editorId comes from undoing the nodeId computation (see the helpers
        // above), which is the same for nodes and pins.
        const int eid = editorIdFromRawId((int)doble.Get());
        const int idx = anim->stateIndexByEditorId(eid, m_layer);
        if (idx >= 0 && anim->states(m_layer)[(size_t)idx].isSubMachine)
            m_nivelId = anim->states(m_layer)[(size_t)idx].editorId;
    }

    ed::End();
    // Already outside the canvas: here the mouse is back in screen
    // coordinates and the tooltip comes out next to the cursor.
    if (!m_tooltipNodo.empty()) ImGui::SetTooltip("%s", m_tooltipNodo.c_str());
    ed::SetCurrentEditor(nullptr);
}

void AnimatorPanel::drawBlendPickPopup(GameObject* go)
{
    if (m_blendPickRequested)
    {
        ImGui::OpenPopup("blend_pick");
        m_blendPickRequested = false;
    }
    if (!ImGui::BeginPopup("blend_pick")) return;

    auto anim = go->getAnimator();
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    int idx = -1;
    for (int i = 0; i < (int)anim->states(m_layer).size(); i++)
        if (anim->states(m_layer)[i].editorId == m_blendPickEditorId) { idx = i; break; }
    // The state was deleted with the list open. The mesh is only needed for
    // the kinds that list clips: an object without a skeleton also chooses parent
    // and sub-machine entry.
    const bool necesitaMesh = m_blendPickKind == 0;
    if (idx < 0 || (necesitaMesh && !mesh))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& st = anim->statesMutable(m_layer)[idx];

    if (m_blendPickKind == 4)
    {
        // Parent: the root or any box that is neither itself nor INSIDE
        // itself (putting a box inside itself would make a cycle and leave its
        // children unreachable).
        if (ImGui::Selectable("(root)", st.parent < 0)) st.parent = -1;
        const auto& sts = anim->states(m_layer);
        for (int i = 0; i < (int)sts.size(); i++)
        {
            if (!sts[(size_t)i].isSubMachine) continue;
            if (i == idx || anim->isDescendantOf(i, idx, m_layer)) continue;
            if (ImGui::Selectable(sts[(size_t)i].name.c_str(), st.parent == i)) st.parent = i;
        }
    }
    else if (m_blendPickKind == 5)
    {
        // Entry of the box: any of its DIRECT children. If it has none, the
        // box is empty and cannot be entered.
        const auto& sts = anim->states(m_layer);
        bool alguno = false;
        for (int i = 0; i < (int)sts.size(); i++)
        {
            if (sts[(size_t)i].parent != idx) continue;
            alguno = true;
            if (ImGui::Selectable(sts[(size_t)i].name.c_str(), st.subEntry == i)) st.subEntry = i;
        }
        if (!alguno)
            ImGui::TextDisabled("The sub-state machine is empty: put some state in it with 'parent'.");
    }
    else if (m_blendPickKind == 2)
    {
        // Speed multiplier: "(none)" or any float parameter.
        if (ImGui::Selectable("(none)", st.speedParam.empty()))
            st.speedParam.clear();
        bool alguno = false;
        for (const auto& p : anim->parameters())
        {
            if (p.type != AnimatorComponent::ParamType::Float) continue;
            alguno = true;
            if (ImGui::Selectable(p.name.c_str(), p.name == st.speedParam))
                st.speedParam = p.name;
        }
        if (!alguno)
            ImGui::TextDisabled("No float parameters: declare one in Parameters.");
    }
    else if (m_blendPickKind == 0)
    {
        // It lists ALL the clips of the mesh, not only the ones the graph already uses:
        // the engine accepts any of them, the main one included (in a 1D,
        // the same clip with another threshold acts as a plateau). The entry may have
        // disappeared: it was removed with the "x" with the popup open.
        if (m_blendPickEntry < 0 || m_blendPickEntry >= (int)st.blendEntries.size())
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            auto& e = st.blendEntries[m_blendPickEntry];
            bool cambio = false;
            for (const auto& c : mesh->animationClips)
            {
                if (ImGui::Selectable(c.name.c_str(), c.name == e.clipName))
                {
                    e.clipName = c.name;
                    cambio = true;
                }
            }
            // The index and the duration are resolved by rebindClips by name; without
            // this the entry would stay at -1 until the scene is reloaded.
            // rebindClips and not bindClips: Play Mode may be running and
            // bindClips would restart the graph and the user's parameters.
            if (cambio)
                anim->rebindClips(*mesh, nullptr);
        }
    }
    else
    {
        // X (kind 1) or Y (kind 3) parameter of the blend. Only float parameters:
        // they are the only ones that give a continuous weight. The list coming out empty is
        // the hint that one has to be declared in Parameters.
        std::string& destino = (m_blendPickKind == 3) ? st.blendParamY : st.blendParam;
        bool alguno = false;
        for (const auto& p : anim->parameters())
        {
            if (p.type != AnimatorComponent::ParamType::Float) continue;
            alguno = true;
            if (ImGui::Selectable(p.name.c_str(), p.name == destino))
                destino = p.name;
        }
        if (!alguno)
            ImGui::TextDisabled("No float parameters: declare one in Parameters.");
    }
    ImGui::EndPopup();
}

void AnimatorPanel::drawConditionsPopup(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    if (m_conditionsFor < 0 || m_conditionsFor >= (int)anim->transitions(m_layer).size()) return;

    if (!ImGui::BeginPopup("conditions")) return;

    auto& tr = anim->transitionsMutable(m_layer)[m_conditionsFor];

    // Cross-fade of THIS transition, in seconds. 0 = hard cut, which is what
    // the engine did before and what old scenes bring.
    ImGui::TextUnformatted("Transition");
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("cross-fade (s)", &tr.duration, 0.01f, 0.0f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Seconds of blending with the source state. 0 = instant cut.");
    // DragFloat with min 0 already prevents it when dragging, but not when typing a
    // value: a negative would leave blendWeight outside [0,1].
    if (tr.duration < 0.0f) tr.duration = 0.0f;

    // Exit time: the transition waits for the source state to reach
    // exitTime (normalized; 1 = end of the clip, >1 counts loops). Without
    // conditions it fires by time alone.
    ImGui::PushID("exit_time");
    ImGui::Checkbox("Has Exit Time", &tr.hasExitTime);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Waits for the source state to reach 'exit time'. Without conditions, it fires on time alone.");
    if (tr.hasExitTime)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("exit time", &tr.exitTime, 0.01f, 0.0f, 100.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Normalized time: 0.9 = at 90%% of the clip, 2.5 = after two and a half loops.");
        if (tr.exitTime < 0.0f) tr.exitTime = 0.0f;
    }
    if (tr.fromState == AnimatorComponent::kAnyState)
    {
        ImGui::Checkbox("Can Transition To Self", &tr.canTransitionToSelf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Whether it can go back to the state it is already in. On with a bool, it would restart it every frame.");
    }
    ImGui::PopID();

    ImGui::Separator();
    ImGui::TextUnformatted("Conditions (AND)");
    ImGui::Separator();

    int toRemove = -1;
    for (size_t c = 0; c < tr.conditions.size(); c++)
    {
        ImGui::PushID((int)c);
        auto& cond = tr.conditions[c];
        ImGui::Text("%s", condLabel(cond.type));
        if (cond.type != AnimatorComponent::ConditionType::AnimationFinished)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(cond.paramName.c_str());
        }
        if (cond.type == AnimatorComponent::ConditionType::Bool)
        {
            ImGui::SameLine();
            ImGui::Checkbox("expected", &cond.expected);
        }
        if (cond.type == AnimatorComponent::ConditionType::Int ||
            cond.type == AnimatorComponent::ConditionType::Float)
        {
            const bool isFloat = cond.type == AnimatorComponent::ConditionType::Float;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            int op = (int)cond.compare;
            // The four comparators for both types: the evaluator always
            // supported them (see AnimatorComponent::conditionsMet) and trimming the
            // combo to 2 for Float only served to hide them.
            if (ImGui::Combo("##cmp", &op, kCompareLabels, IM_ARRAYSIZE(kCompareLabels)))
                cond.compare = (AnimatorComponent::Compare)op;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (isFloat)
            {
                ImGui::DragFloat("##thr", &cond.threshold, 0.01f);
            }
            else
            {
                // The threshold lives in a float so as not to duplicate the field; the Int UI
                // goes through a temporary int, so a value with a fractional part never
                // comes in through here and what is seen is exactly what
                // conditionsMet evaluates (which rounds, not truncates, to cover
                // the ones that come from a hand-edited JSON).
                int thr = (int)cond.threshold;
                if (ImGui::DragInt("##thr", &thr)) cond.threshold = (float)thr;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = (int)c;
        ImGui::PopID();
    }
    if (toRemove >= 0) tr.conditions.erase(tr.conditions.begin() + toRemove);

    ImGui::Separator();
    // Only already declared parameters are offered: a condition on a nonexistent
    // parameter would never fire and there would be no way to know why.
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        // Selectable with DontClosePopups instead of MenuItem: a MenuItem closes
        // the popup on the first click and the user could only add one condition
        // per opening. With this they can chain several "Add:" in a row.
        if (ImGui::Selectable(("Add: " + p.name).c_str(), false, ImGuiSelectableFlags_DontClosePopups))
        {
            AnimatorComponent::Condition cond;
            // The parameter type decides the condition one 1:1.
            switch (p.type)
            {
                case AnimatorComponent::ParamType::Trigger:
                    cond.type = AnimatorComponent::ConditionType::Trigger; break;
                case AnimatorComponent::ParamType::Int:
                    cond.type = AnimatorComponent::ConditionType::Int;     break;
                case AnimatorComponent::ParamType::Float:
                    cond.type = AnimatorComponent::ConditionType::Float;   break;
                default:
                    cond.type = AnimatorComponent::ConditionType::Bool;    break;
            }
            cond.paramName = p.name;
            cond.expected  = true;
            tr.conditions.push_back(cond);
        }
        ImGui::PopID();
    }
    // "animation finished" never fires when leaving a looping state (a
    // looping clip never "finishes", see AnimatorComponent::update): the
    // item is kept but disabled, with a tooltip, so the user does not set up a
    // dead link without knowing it.
    const bool fromLoops = tr.fromState >= 0 && tr.fromState < (int)anim->states(m_layer).size()
                            && anim->states(m_layer)[tr.fromState].loop;
    ImGui::BeginDisabled(fromLoops);
    if (ImGui::Selectable("Add: animation finished", false, ImGuiSelectableFlags_DontClosePopups))
    {
        AnimatorComponent::Condition cond;
        cond.type = AnimatorComponent::ConditionType::AnimationFinished;
        tr.conditions.push_back(cond);
    }
    ImGui::EndDisabled();
    if (fromLoops && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The source state loops: 'animation finished' never fires here. To leave on time, use 'Has Exit Time'.");

    ImGui::EndPopup();
}

void AnimatorPanel::importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path)
{
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    const std::string ext = std::filesystem::path(path).extension().string();
    if (!ModelLoader::isSupportedModelExtension(ext))
    {
        m_animSrcError = "Unsupported format: " + ext;
        return;
    }

    // Dry run on a copy: the command cannot say "no" halfway
    // (AnimationSourceCommand::applyAdd discards the warnings of
    // addAnimationSource and simply does nothing if it fails), and this way the loader
    // error (wrong rig, file without animations) reaches the user
    // before anything is put on the undo stack.
    SkinnedMesh probe = *mesh;
    std::vector<std::string> warnings;
    const bool ok = addAnimationSource(probe, path, warnings);
    for (const auto& w : warnings) ctx.pushLog("Animator: " + w);

    if (!ok)
    {
        m_animSrcError = warnings.empty() ? ("Could not import animations from " + path)
                                           : warnings.back();
        return;
    }
    m_animSrcError.clear();

    auto cmd = std::make_unique<AnimationSourceCommand>(
        *ctx.scene, ctx.renderer, "Add animations", go->id,
        /*add=*/true, path, std::vector<std::string>{});
    cmd->execute();
    ctx.undo->push(std::move(cmd));
    ctx.pushLog("Animator: animations from '" + path + "' imported");
}

void AnimatorPanel::drawAnimationSources(EditorContext& ctx, GameObject* go)
{
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    if (!ImGui::CollapsingHeader("Animation Sources", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    int sourceToRemove = -1;
    for (size_t s = 0; s < mesh->animationSources.size(); s++)
    {
        const AnimationSource& src = mesh->animationSources[s];
        // The path as ID instead of the index s: removing a source reindexes the
        // vector, and if the ID were the index, each later row would inherit
        // the open/closed state (and the in-flight m_renamingClip, if there
        // were one) of the row that occupied its index before the deletion. The
        // path is stable while the source exists.
        //
        // The path ALONE is not enough: two sources can share a path on
        // purpose (reimporting the same file), and with the same ID the two
        // rows would share the TreeNode state: expanding one would expand
        // the other. It is composed with how many earlier sources repeat that path,
        // which tells the copies apart without depending on the absolute index.
        int pathOccurrence = 0;
        for (size_t k = 0; k < s; k++)
            if (mesh->animationSources[k].path == src.path) pathOccurrence++;
        ImGui::PushID(src.path.c_str());
        ImGui::PushID(pathOccurrence);

        const std::string file = std::filesystem::path(src.path).filename().string();
        const std::string label = file + "  (" + std::to_string(src.clipNames.size()) + " clips)"
                                + (src.builtin ? "  [model]" : "");

        // AllowOverlap: the node takes up the whole row (SpanAvailWidth) and the "X"
        // is painted ON TOP of it. Without the flag, ImGui gives the click to the first item
        // submitted, the node, and the X did nothing but fold the row.
        const bool open = ImGui::TreeNodeEx("##src",
                                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                            "%s", label.c_str());

        // The builtin source is the model FBX: removing it would leave the mesh without
        // the file that created it, so the button exists but is disabled
        // (showing it and explaining it teaches the rule; hiding it conceals it).
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        ImGui::BeginDisabled(src.builtin);
        if (ImGui::SmallButton("X")) sourceToRemove = (int)s;
        ImGui::EndDisabled();
        if (src.builtin && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("It is the model's FBX: its animations cannot be removed separately.");

        if (open)
        {
            for (const std::string& clipName : src.clipNames)
            {
                ImGui::PushID(clipName.c_str());
                if (m_renamingClip == clipName)
                {
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        const std::string nuevo = m_renameBuf;

                        // Validation BEFORE executing anything, replicating exactly
                        // the rejection rules of renameClip (see
                        // SkinnedMeshAnimations.cpp): empty name, name already used
                        // by any clip, or name identical to the current one. Before, the
                        // command was executed first and success was inferred by
                        // scanning the mesh for the new name; that could not
                        // distinguish "the rename was applied" from "a clip with that name
                        // already existed", which is precisely the reason
                        // renameClip rejects it: a duplicate (e.g. renaming
                        // "Walk" to "Idle" when "Idle" already exists) sneaked into the
                        // undo stack as if it had worked, and the user's next
                        // Ctrl+Z undid nothing because the command had
                        // not mutated anything.
                        bool rechazado = nuevo.empty() || nuevo == clipName;
                        if (!rechazado)
                            for (const auto& c : mesh->animationClips)
                                if (c.name == nuevo) { rechazado = true; break; }

                        if (rechazado)
                        {
                            m_animSrcError = "Could not rename to '" + nuevo
                                            + "': name empty, duplicated or the same as the current one";
                        }
                        else
                        {
                            // Copy of the old name BEFORE execute(): clipName
                            // is a const std::string& that points directly at the
                            // element of src.clipNames, and renameClip rewrites it
                            // in place: after execute() clipName is already "new",
                            // so using it in the log would duplicate the new name
                            // instead of showing what changed.
                            const std::string viejo = clipName;
                            auto cmd = std::make_unique<ClipRenameCommand>(
                                *ctx.scene, "Rename clip", go->id, viejo, nuevo);
                            cmd->execute();
                            ctx.undo->push(std::move(cmd));
                            m_animSrcError.clear();
                            ctx.pushLog("Animator: clip '" + viejo + "' renamed to '" + nuevo + "'");
                        }
                        m_renamingClip.clear();
                    }
                    // Click outside without pressing Enter (InputTextFlags_EnterReturnsTrue
                    // does not fire there): edit mode is closed without touching anything,
                    // neither the mesh nor the undo stack: the user changed their mind.
                    if (ImGui::IsItemDeactivated()) m_renamingClip.clear();
                }
                else
                {
                    ImGui::BulletText("%s", clipName.c_str());
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    {
                        m_renamingClip = clipName;
                        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", clipName.c_str());
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();   // pathOccurrence
        ImGui::PopID();   // src.path
    }

    if (sourceToRemove >= 0)
    {
        // Deferred outside the for: mutating animationSources inside the very
        // loop that walks it would invalidate the iterator of this same frame.
        const AnimationSource& src = mesh->animationSources[(size_t)sourceToRemove];

        // Ordinal counted FROM THE END (0 = the last one with that path), not
        // from the start: it is what AnimationSourceCommand::applyRemove
        // expects (see the long comment there about why the scan goes
        // from the end). It counts how many non-builtin sources with the same
        // path there are AHEAD of the clicked row: if the user removes the
        // first of two rows with the same path, this is 1 (the second
        // row is ahead), and the command skips it correctly instead of
        // removing, by blind backward scan, the last one (the bug that
        // this fix corrects).
        size_t pathOccurrence = 0;
        for (size_t k = (size_t)sourceToRemove + 1; k < mesh->animationSources.size(); k++)
        {
            const auto& later = mesh->animationSources[k];
            if (!later.builtin && later.path == src.path) pathOccurrence++;
        }

        auto cmd = std::make_unique<AnimationSourceCommand>(
            *ctx.scene, ctx.renderer, "Remove animations", go->id,
            /*add=*/false, src.path, src.clipNames, pathOccurrence);
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        // The states that used those clips are left orphaned on purpose: the
        // graph is the user's work and deleting it for them would be worse than leaving it
        // flagged. AnimationSourceCommand::applyRemove already calls
        // rebindClips live, so clipIndex is left at -1 at the
        // moment of deletion (there is no need to wait for a scene reload
        // or a Play/Stop cycle).
        ctx.pushLog("Animator: animation source removed; the states that used it are left without a clip");
    }

    if (ImGui::Button("Add Animation FBX..."))
    {
        IGFD::FileDialogConfig cfg;
        // "assets", like the mesh dialog of PropertiesPanel: the FBX files of
        // this project live there, and opening at the repo root would force
        // navigating every time.
        cfg.path = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_animSrcDialog->OpenDialog("AddAnimSrcDlg", "Choose Animation Source", ModelLoader::supportedModelFilter(), cfg);
        m_animSrcDlgOpen = true;
        // The id is captured NOW, on open, not ctx.selected when draining: the
        // dialog is not modal, so the user can change the selection
        // while choosing the file; the FBX must go to "go" (whoever was
        // selected when the button was pressed), not to whoever happens to be
        // selected when the user finally closes the dialog.
        m_animSrcDlgTarget = go->id;
    }

    // Drop target: the Content Browser emits "DT_ASSET_PATH" (see
    // ContentBrowserPanel::BeginDragDropSource), not "CONTENT_BROWSER_ITEM";
    // same id and same pattern (payload->Data is a '\0'-terminated char*,
    // size = fullPath.size()+1) that PropertiesPanel::drawMeshSection uses
    // for its own model drop target.
    ImGui::SameLine();
    ImGui::TextDisabled("(or drag a model here)");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            importAnimationSource(ctx, go, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }

    if (!m_animSrcError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_animSrcError.c_str());

    ImGui::Separator();
}

void AnimatorPanel::drawAnimationSourceDialog(EditorContext& ctx)
{
    if (!m_animSrcDlgOpen) return;
    if (!m_animSrcDialog->Display("AddAnimSrcDlg")) return;

    if (m_animSrcDialog->IsOk())
    {
        // Resolves by id (captured when opening the dialog, see OpenDialog further
        // up), not ctx.selected: by the time the user closes the dialog
        // the selection may have changed, and the original object may
        // even have been deleted. Without this explicit resolution, a guard like
        // "ctx.selected && ctx.selected->hasAnimator()" would have imported the
        // FBX into the wrong GameObject without warning, or would have silently discarded it
        // if the currently selected one had no Animator.
        GameObject* target = ctx.scene ? ctx.scene->findById(m_animSrcDlgTarget) : nullptr;
        if (!target)
        {
            m_animSrcError = "The target GameObject no longer exists in the scene";
        }
        else if (!target->getSkinnedMesh())
        {
            m_animSrcError = "'" + target->name + "' no longer has a skinned mesh";
        }
        else
        {
            importAnimationSource(ctx, target, m_animSrcDialog->GetFilePathName());
        }
    }

    m_animSrcDialog->Close();
    m_animSrcDlgOpen = false;
}

void AnimatorPanel::draw(EditorContext& ctx)
{
    // The body goes in a block, not in early-returns: drawAnimationSourceDialog()
    // below has to run ALWAYS, whether the panel is closed
    // (m_open == false, e.g. after pressing the window X, which Begin writes
    // directly into m_open) or Begin returns false (collapsed window). With
    // the earlier early-returns, closing or collapsing the panel while the
    // file dialog was open left m_animSrcDlgOpen (and the internal state
    // of IGFD) stuck at true forever: the dialog resurrected
    // by itself, unasked, when reopening the panel. Begin/End are always called as a
    // pair no matter what (an ImGui rule), hence the unconditional End()
    // inside the if(m_open). Same pattern as PropertiesPanel::draw +
    // drawMeshDialog.
    // There is only an undo session while a graph is being drawn: closed panel,
    // collapsed or without an Animator discard it (see the end of the function).
    bool grafoDibujado = false;
    if (m_open)
    {
        if (ImGui::Begin("Animator", &m_open))
        {
            GameObject* go = ctx.selected;
            if (!go || !go->hasAnimator())
            {
                ImGui::TextDisabled("Select a GameObject with an Animator component.");
                ImGui::TextDisabled("Properties > Add > Animator");
                m_boundTo = nullptr;
            }
            else
            {
                // Change of linked object: it is checked ONCE here at the top
                // (before drawing anything) to be able to clear m_renamingClip before
                // drawAnimationSources reads it. Otherwise a clip with the same
                // name in the new GameObject would inherit the edit mode and the
                // buffer of the previous object's clip for one frame.
                const bool selectionChanged = (m_boundTo != go);
                if (selectionChanged) { m_renamingClip.clear(); m_layer = 0; m_nivelId = -1; m_renamingLayer = -1; }

                // Graph undo: the bracket wraps EVERYTHING that can mutate the
                // component in this frame, from drawAnimationSources to the
                // conditions popup that is drawn inside drawGraph.
                m_graphUndo.beginFrame(go->id, go->getAnimator().get(), ctx.undo->revision());
                grafoDibujado = true;
                const bool historialMovido = ctx.undo->revision() != m_lastUndoRevision;

                // Left column in a child WITH SCROLL: sources, layers, IK,
                // "Add State" and parameters. Before, they went loose in the window and,
                // as it grew (several layers, several IK constraints), what was
                // below ended up out of reach with no way to get to it, and incidentally squashed
                // the canvas. The canvas remains separate, on the right: its wheel and its
                // pan are its own and this scroll does not touch them.
                // The maximum is also clamped to the window: if it shrinks, the
                // column cannot stay wider than it and leave the
                // canvas with no room.
                const float anchoMax = std::max(kAnchoColumnaMin,
                                                std::min(kAnchoColumnaMax,
                                                         ImGui::GetContentRegionAvail().x - 120.0f));
                m_anchoColumna = std::clamp(m_anchoColumna, kAnchoColumnaMin, anchoMax);
                ImGui::BeginChild("columnaIzq", ImVec2(m_anchoColumna, 0), false,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                drawAnimationSources(ctx, go);
                drawLayerBar(ctx, go);
                drawIkList(ctx, go);
                drawPropertyClips(ctx, go);

                // --- Add state from the model clips ---
                const SkinnedMesh* mesh = go->getSkinnedMesh();
                if (!mesh || mesh->animationClips.empty())
                {
                    ImGui::TextDisabled("No skinned mesh with animations: the states come from\n"
                                        "the property clips (below).");
                }
                else if (ImGui::BeginCombo("##addstate", "Add State from Clip"))
                {
                    for (size_t i = 0; i < mesh->animationClips.size(); i++)
                    {
                        if (!ImGui::Selectable(mesh->animationClips[i].name.c_str())) continue;
                        AnimatorComponent::State st;
                        st.name           = mesh->animationClips[i].name;
                        st.clipName       = mesh->animationClips[i].name;
                        st.clipIndex      = (int)i;
                        st.duration       = mesh->animationClips[i].duration;
                        st.ticksPerSecond = mesh->animationClips[i].ticksPerSecond;
                        st.editorPos      = glm::vec2(40.0f + 40.0f * (float)go->getAnimator()->states(m_layer).size(),
                                                       40.0f + 30.0f * (float)go->getAnimator()->states(m_layer).size());
                        const int idx = go->getAnimator()->addState(st, m_layer);
                        const int eid = go->getAnimator()->states(m_layer)[idx].editorId;
                        // The node is new: it has to be placed in the canvas by hand, the
                        // general sync only runs when the object changes.
                        ed::SetCurrentEditor(m_ctx);
                        ed::SetNodePosition(nodeId(eid), ImVec2(st.editorPos.x, st.editorPos.y));
                        ed::SetCurrentEditor(nullptr);
                        ctx.pushLog("Animator: state '" + st.name + "' added");
                    }
                    ImGui::EndCombo();
                }

                auto anim = go->getAnimator();

                // --- Add a sub-machine ---
                // It is created at the level being viewed: creating a box inside
                // another one means entering first and pressing here.
                if (ImGui::Button("Add Sub-State Machine"))
                {
                    AnimatorComponent::State caja;
                    caja.name         = "Sub-Machine";
                    caja.isSubMachine = true;
                    caja.parent       = nivelActual(*anim);
                    const int idx = anim->addState(caja, m_layer);
                    const int eid = anim->states(m_layer)[(size_t)idx].editorId;
                    ed::SetCurrentEditor(m_ctx);
                    ed::SetNodePosition(nodeId(eid), ImVec2(caja.editorPos.x, caja.editorPos.y));
                    ed::SetCurrentEditor(nullptr);
                    ctx.pushLog("Animator: sub-state machine added");
                }

                // --- Add state from a property clip ---
                // Without this, an object WITHOUT a skeleton could not have a single state
                // (the only way to create them was the model clips), so
                // its property clip was played by nobody.
                if (!anim->propertyClips().empty() && ImGui::BeginCombo("##addpropstate", "Add State from Property Clip"))
                {
                    for (const auto& pc : anim->propertyClips())
                    {
                        if (!ImGui::Selectable(pc.name.c_str())) continue;
                        AnimatorComponent::State st;
                        st.name             = pc.name;
                        st.propertyClipName = pc.name;
                        st.editorPos        = glm::vec2(40.0f + 40.0f * (float)anim->states(m_layer).size(),
                                                        40.0f + 30.0f * (float)anim->states(m_layer).size());
                        const int idx = anim->addState(st, m_layer);
                        // The clip index and the state duration (which without a
                        // mesh clip comes from the property one) are resolved by
                        // bindProperties.
                        anim->bindProperties(go, nullptr);
                        const int eid = anim->states(m_layer)[idx].editorId;
                        ed::SetCurrentEditor(m_ctx);
                        ed::SetNodePosition(nodeId(eid), ImVec2(st.editorPos.x, st.editorPos.y));
                        ed::SetCurrentEditor(nullptr);
                        ctx.pushLog("Animator: state '" + st.name + "' added");
                    }
                    ImGui::EndCombo();
                }

                drawParameterList(ctx, go);
                ImGui::EndChild();
                ImGui::SameLine();

                // Handle to drag the edge. An InvisibleButton and not a
                // Separator: it has to capture the drag (IsItemActive)
                // to keep moving it even if the cursor leaves the rect.
                ImGui::InvisibleButton("##agarreColumna",
                                       ImVec2(kAnchoAgarre, ImGui::GetContentRegionAvail().y));
                const bool agarreActivo  = ImGui::IsItemActive();
                const bool agarreEncima  = ImGui::IsItemHovered();
                if (agarreActivo || agarreEncima) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (agarreActivo) m_anchoColumna = std::clamp(m_anchoColumna + ImGui::GetIO().MouseDelta.x,
                                                              kAnchoColumnaMin, anchoMax);
                // Without painting it you cannot see where to grab: it is invisible by design.
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    ImGui::GetColorU32(agarreActivo ? ImGuiCol_SeparatorActive
                                                    : (agarreEncima ? ImGuiCol_SeparatorHovered
                                                                    : ImGuiCol_Separator)));
                ImGui::SameLine();

                ImGui::BeginChild("canvas", ImVec2(0, 0), false);
                // The undo may have removed the selected layer.
                m_layer = std::clamp(m_layer, 0, go->getAnimator()->layerCount() - 1);
                const bool capaCambiada = (m_layer != m_boundLayer);
                if (selectionChanged || historialMovido || capaCambiada)
                {
                    // Selection change: the canvas still has the positions of the
                    // previous object. They are dumped once, not every frame, otherwise the
                    // user could not drag the nodes.
                    // A history movement (undo/redo, or any push) may
                    // have reinserted states whose editorId the canvas does not know, so
                    // the positions are dumped once in that case too: it is not
                    // only for an undo, it fires with any history movement.
                    ed::SetCurrentEditor(m_ctx);
                    syncPositionsFromComponent(go);
                    ed::SetCurrentEditor(nullptr);
                    m_boundTo    = go;
                    m_boundLayer = m_layer;
                }
                drawGraph(ctx, go);
                // Reverse sync every frame, unconditional (the Task 10 guard that skipped it
                // after a deletion is no longer needed): with a stable editorId, a
                // survivor keeps its node id no matter what happens to the vector,
                // so GetNodePosition(nodeId(editorId)) always reads the position
                // of the right node, even in the same frame in which another node was deleted.
                ed::SetCurrentEditor(m_ctx);
                syncPositionsToComponent(go);
                ed::SetCurrentEditor(nullptr);

                // When the gesture ends, the graph goes through the same sanitizing pass
                // as loading (A10): this panel writes the vectors bare through
                // statesMutable/transitionsMutable, so it is here (where it is known that it has just
                // changed and before the undo snapshot freezes it) that we have to
                // check that what was written is representable. A healthy graph does not change, so
                // this does not dirty the diff that decides whether there is a command.
                const bool gestoActivo = ImGui::IsAnyItemActive();
                if (!gestoActivo)
                    if (auto& anim = go->getAnimator()) anim->sanitizeGraph(m_layer, nullptr);

                // End of the undo bracket. IsAnyItemActive: while a drag
                // is still active, the gesture has not finished and nothing is stacked.
                if (auto cmd = m_graphUndo.endFrame(*ctx.scene, go->getAnimator().get(),
                                                    gestoActivo, ctx.undo->revision()))
                {
                    ctx.pushLog("Animator: " + cmd->label());
                    // Without execute(): the change is already applied (push contract).
                    ctx.undo->push(std::move(cmd));
                }
                m_lastUndoRevision = ctx.undo->revision();
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    if (!grafoDibujado) m_graphUndo.discard();

    // Unconditional and outside the window: it has to be drained even if the panel
    // is closed/collapsed or the selection changed while the dialog
    // was open (see the big comment at the start of this function).
    drawAnimationSourceDialog(ctx);
}

} // namespace DonTopo
