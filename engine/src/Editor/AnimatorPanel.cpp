#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/Command.h"
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
    // Dibujo de SOLO LECTURA de una pista: la forma de la curva, sus keys, los
    // umbrales de las condiciones que leen su parámetro y el playhead del
    // preview. Responde de un vistazo la única pregunta que se le hace a una
    // curva —¿cruza el umbral, y cuándo?—, que con la lista de DragFloat hay
    // que reconstruir a mano. Las keys se siguen editando en la lista: el
    // arrastre sobre el lienzo es la fila C15 del audit.
    void dibujarCurva(const PropertyTrack& pista, float duracion, float tiempoActual,
                      const float* umbrales, int numUmbrales)
    {
        const float ancho = ImGui::GetContentRegionAvail().x;
        if (ancho < 40.0f || duracion <= 0.0f || pista.keys.empty()) return;
        const float alto = 56.0f;
        ImGui::Dummy(ImVec2(ancho, alto));
        const ImVec2  p0 = ImGui::GetItemRectMin();
        const ImVec2  p1 = ImGui::GetItemRectMax();
        ImDrawList*   dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
        dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));

        float lo = 0.0f, hi = 0.0f;
        curveRange(pista, umbrales, numUmbrales, lo, hi);
        auto aY = [&](float v) { return p1.y - (v - lo) / (hi - lo) * (p1.y - p0.y); };
        auto aX = [&](float t) {
            const float x = p0.x + (t / duracion) * (p1.x - p0.x);
            return std::min(std::max(x, p0.x), p1.x);   // una key más allá del clip se queda en el borde
        };

        // Los umbrales van debajo de la curva: lo que interesa es dónde la cruza.
        for (int i = 0; i < numUmbrales; i++)
        {
            const float y = aY(umbrales[i]);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(220, 190, 80, 150));
            char txt[32];
            std::snprintf(txt, sizeof(txt), "%.2f", umbrales[i]);
            dl->AddText(ImVec2(p1.x - 36.0f, y - 15.0f), IM_COL32(220, 190, 80, 200), txt);
        }

        // Muestreada, no unida key con key: así el dibujo sigue siendo el valor
        // real si algún día la interpolación deja de ser lineal.
        constexpr int kMuestras = 64;
        ImVec2 pts[kMuestras + 1];
        for (int s = 0; s <= kMuestras; s++)
        {
            const float t = duracion * (float)s / (float)kMuestras;
            pts[s] = ImVec2(aX(t), aY(samplePropertyTrack(pista, t, 0.0f)));
        }
        dl->AddPolyline(pts, kMuestras + 1, IM_COL32(120, 200, 255, 255), 0, 1.5f);
        for (const auto& k : pista.keys)
            dl->AddCircleFilled(ImVec2(aX(k.time), aY(k.value)), 3.0f, IM_COL32(255, 255, 255, 230));
        if (tiempoActual >= 0.0f)
        {
            const float x = aX(tiempoActual);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 120, 120, 200));
        }

        // Los extremos del rango: sin ellos la altura de la curva es una
        // adivinanza, porque el rango se ajusta a cada pista.
        char txt[32];
        std::snprintf(txt, sizeof(txt), "%.2f", hi);
        dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 1.0f), IM_COL32(150, 150, 160, 200), txt);
        std::snprintf(txt, sizeof(txt), "%.2f", lo);
        dl->AddText(ImVec2(p0.x + 3.0f, p1.y - 16.0f), IM_COL32(150, 150, 160, 200), txt);
    }

    // Lienzo de solo lectura del blend 2D: los puntos con su clip, las aristas
    // de la triangulación y el valor actual de (X, Y). Los puntos son los de
    // stateBlendSamples: el principal y las entradas con clip resuelto.
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

        // Caja de los puntos con un 10 % de margen; el valor se acota a ella.
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
        // Y hacia arriba, como en una gráfica.
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
    // IDs del node editor: tienen que ser != 0 y no colisionar entre nodos,
    // pines y links. Tres slots por estado + un rango aparte pa los links.
    //
    // El slot se indexa por State::editorId (estable, asignado una vez en
    // AnimatorComponent::addState), NUNCA por el índice en el vector de
    // estados: ese índice cambia cuando removeState reindexa tras borrar un
    // estado de en medio, y si el id del canvas fuera el índice, un
    // superviviente heredaría el slot visual (posición/selección) del nodo
    // borrado — imgui-node-editor cachea esas cosas por id, no por contenido.
    int nodeId(int eid)      { return eid * 3 + 1; }
    int inputPinId(int eid)  { return eid * 3 + 2; }
    int outputPinId(int eid) { return eid * 3 + 3; }
    int linkId(int transIdx) { return 100000 + transIdx; }

    // Decodifica el editorId codificado en un id de nodo o de pin (la fórmula
    // es la misma división entera pa las tres variantes de nodeId/inputPinId/
    // outputPinId, así que un solo decode sirve pa las tres).
    int editorIdFromRawId(int rawId) { return (rawId - 1) / 3; }
    bool isOutputPin(int pin) { return (pin - 1) % 3 == 2; }

    // Nodo Any State: ids FUERA del esquema de los estados (eid*3+1..3) y de
    // los links (100000+idx). Se comprueban SIEMPRE antes de decodificar con
    // editorIdFromRawId: pasados por esa fórmula casarían con un editorId
    // (300000) que ningún grafo alcanza, pero isOutputPin los clasificaría
    // mal — por eso esPinDeSalida.
    // Límites del ancho de la columna izquierda (fuentes, capas, IK,
    // parámetros), que el usuario arrastra por el borde. El mínimo deja ver la
    // lista de parámetros, que es el widget más ancho; el máximo evita dejar el
    // lienzo en nada de un tirón.
    const float kAnchoColumnaMin = 200.0f;
    const float kAnchoColumnaMax = 700.0f;
    const float kAnchoAgarre     = 6.0f;

    const int kAnyStateNodeId   = 900001;
    const int kAnyStateOutPinId = 900002;

    bool esPinDeSalida(int pin) { return pin == kAnyStateOutPinId || (pin != kAnyStateNodeId && isOutputPin(pin)); }

    // Un pin (o un nodo) solo trae el editorId estable, y las transiciones
    // guardan índices del vector m_states (no editorIds) porque ese es el
    // contrato de AnimatorComponent::Transition. Este helper hace el puente:
    // decodifica el editorId y lo busca en los estados de la capa que se está
    // editando. Devuelve -1 si ningún estado vivo tiene ese id (no debería
    // pasar: los ids que llegan aquí vienen de nodos/pines dibujados este
    // mismo frame a partir de states(capa) actual).
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

    // Etiquetas de Compare en el orden del enum. Los cuatro se ofrecen tanto
    // para Int como para Float: el == sobre float solo dispara con igualdad
    // binaria exacta (un valor calculado casi nunca la cumple, uno puesto con
    // SetFloat sí), pero recortar el combo escondía media API sin avisar.
    const char* kCompareLabels[] = { ">", "<", "==", "!=" };
}

AnimatorPanel::AnimatorPanel()
{
    ed::Config config;
    // Sin fichero de settings: las posiciones de los nodos viven en el JSON de
    // escena (AnimatorComponent::State::editorPos). Si lo dejáramos por defecto,
    // el node editor escribiría un NodeEditor.json paralelo y habría dos fuentes
    // de verdad peleándose.
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
    // El nodo Any State existe mientras haya al menos un estado.
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
    // Desplegable, como las fuentes de animación, las capas y la IK: las
    // cuatro secciones de la columna se abren y se cierran igual, y el
    // scroll es el de la columna. Antes esto era un hijo con alto propio y
    // su propia barra, que ni cuadraba con el resto ni hacía falta.
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

            // Valor editable in situ: en Play permite provocar una transición a mano
            // sin escribir Lua, que es como se depura un grafo.
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
                    // Un trigger no tiene valor que mostrar: se arma y lo consume la
                    // primera transición que lo mire (ver consumeTriggers).
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
        // Diferido: borrar dentro del for-range invalidaría el iterador.
        if (!toRemove.empty())
        {
            m_graphUndo.setLabel("Quitar parámetro");
            anim->removeParameter(toRemove);
            ctx.pushLog("Animator: parámetro '" + toRemove + "' eliminado");
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
        // El orden coincide con el del enum ParamType, así que el índice del combo
        // castea directo: si el enum crece, esta lista crece con él.
        const char* types[] = { "bool", "trigger", "int", "float" };
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##newparamtype", &m_newParamType, types, IM_ARRAYSIZE(types));
        if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
        {
            m_graphUndo.setLabel("Añadir parámetro");
            anim->addParameter(m_newParamName, (AnimatorComponent::ParamType)m_newParamType);
            ctx.pushLog(std::string("Animator: parámetro '") + m_newParamName + "' añadido");
            m_newParamName[0] = '\0';
        }
    }
    ImGui::PopID();

}

void AnimatorPanel::drawLayerBar(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    ImGui::PushID("capas");
    // El acotado va FUERA del desplegable: la capa seleccionada tiene que
    // seguir siendo válida aunque la sección esté cerrada (el grafo que se
    // dibuja es el suyo).
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
                // Al soltar el foco (Enter, clic fuera): se guarda si no está vacío.
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
                m_nivel = -1;   // el nivel es de la capa que se deja atras
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_renamingLayer = li;
                    m_focusRename   = true;
                    std::snprintf(m_layerNameBuf, sizeof(m_layerNameBuf), "%s", anim->layer(li).name.c_str());
                }
            }
            if (li == 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Capa base: siempre override, peso 1 y todo el cuerpo.");
            ImGui::PopID();
        }

        ImGui::BeginDisabled(anim->layerCount() >= AnimatorComponent::kMaxLayers);
        if (ImGui::Button("+##addLayer"))
        {
            const int n = anim->addLayer("Layer " + std::to_string(anim->layerCount()));
            if (n >= 0)
            {
                m_layer = n;
                m_nivel = -1;   // el nivel es de la capa que se deja atras
                ctx.pushLog("Animator: capa '" + anim->layer(n).name + "' añadida");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_layer == 0);
        if (ImGui::Button("-##removeLayer"))
        {
            ctx.pushLog("Animator: capa '" + anim->layer(m_layer).name + "' quitada");
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

        // La base no tiene peso, modo ni máscara propios.
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
                ImGui::SetTooltip("Override sustituye la pose de los huesos de la máscara;\n"
                                  "Additive le suma la diferencia de cada clip con su primer fotograma.");
            const std::string etiqueta = L.maskBones.empty()
                ? std::string("Mask: todo el cuerpo")
                : "Mask: " + std::to_string(L.maskBones.size()) + " hueso(s)";
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
        // Objetivo y pole son GameObjects de la escena: se listan en preorden,
        // como el panel de jerarquía, y el propio personaje no entra.
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
            if (ImGui::BeginCombo(etiqueta, actual ? actual->name.c_str() : "(ninguno)"))
            {
                if (ImGui::Selectable("(ninguno)", id == 0)) id = 0;
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
            if (ImGui::Combo("Tipo##tipo", &tipo, tipos, 2))
            {
                c.type = (AnimatorComponent::IkType)tipo;
                // La cadena (padre y abuelo) depende del tipo: hay que
                // re-resolverla. rebindClips y no bindClips: puede estar
                // corriendo Play Mode.
                if (mesh) anim->rebindClips(*mesh, nullptr);
            }

            // Hueso: el que mira (look-at) o el EXTREMO de la cadena (two bone).
            const bool roto = c.boneIndex < 0;
            if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("Hueso##hueso", c.boneName.empty() ? "(elige hueso)" : c.boneName.c_str()))
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
                ImGui::SetTooltip("El hueso no existe en el modelo, o la cadena no llega a tres huesos:\n"
                                  "la restricción no se aplica.");

            selectorObjeto("Objetivo##objetivo", c.targetId);
            if (c.type == AnimatorComponent::IkType::TwoBone)
            {
                selectorObjeto("Pole##pole", c.poleId);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hacia dónde apunta el codo o la rodilla.");
            }
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat("Peso##peso", &c.weight, 0.0f, 1.0f, "%.2f");
            if (c.type == AnimatorComponent::IkType::LookAt)
            {
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat3("Eje##eje", &c.aimAxis.x, 0.01f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Eje LOCAL del hueso que apunta al objetivo.");
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Angulo max##ang", &c.maxAngle, 0.0f, 180.0f, "%.0f");
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
            ctx.pushLog("Animator: restriccion de IK anadida");
        }
        ImGui::EndDisabled();
        if (!mesh) ImGui::TextDisabled("Sin mesh skinned no hay huesos que elegir.");
    }
    ImGui::PopID();
}

void AnimatorPanel::drawPropertyClips(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    ImGui::PushID("propclips");
    if (ImGui::CollapsingHeader("Property Clips"))
    {
        ImGui::TextDisabled("Animan el objeto: transform, luz y material.");
        auto& clips = anim->propertyClipsMutable();
        int quitarClip = -1;
        for (int i = 0; i < (int)clips.size(); i++)
        {
            auto& clip = clips[(size_t)i];
            ImGui::PushID(i);
            // Cabecera por clip, como las secciones de la columna. El "###" es
            // lo que la hace utilizable: sin él el ID sale del texto ENTERO, así
            // que al renombrar el clip —o al cambiar el glifo de un botón que
            // dependa de su propio estado— la cabecera pasa a ser otro widget y
            // se pierde si estaba abierta. ImGui abre y cierra por ID, así que
            // cada clip recuerda su estado él solo.
            const std::string etiqueta = (clip.name.empty() ? std::string("(sin nombre)") : clip.name) +
                                         "###clip";
            // AllowOverlap: la cabecera ocupa la fila entera, así que sin el
            // flag ImGui le da el clic a ella (el primer item enviado) y la "x"
            // no se podía pulsar nunca. Mismo patrón que la lista de fuentes.
            const bool abierto = ImGui::CollapsingHeader(etiqueta.c_str(),
                                                         ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
            if (ImGui::SmallButton("x###quitarClip")) quitarClip = i;
            if (abierto)
            {
                char nombre[64];
                std::snprintf(nombre, sizeof(nombre), "%s", clip.name.c_str());
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::InputText("Nombre###nombre", nombre, sizeof(nombre)))
                {
                    clip.name = nombre;
                    // El estado referencia por NOMBRE: renombrar obliga a re-resolver.
                    anim->bindProperties(go, nullptr);
                }
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::DragFloat("Duracion (s)###dur", &clip.duration, 0.01f, 0.001f, 600.0f, "%.3f"))
                {
                    if (clip.duration < 0.001f) clip.duration = 0.001f;
                    // La duración del estado sale de aquí cuando no hay clip de malla.
                    anim->bindProperties(go, nullptr);
                }

                // Dónde va el playhead de ESTE clip, si es que suena ahora: lo
                // dice la misma lista de muestras que se aplica al objeto, así
                // que el preview y el dibujo no pueden discrepar.
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
                    // Destino: una propiedad del objeto o un parámetro Float del
                    // Animator (una curva de clip).
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::BeginCombo("###destino",
                                          pista.target == TrackTarget::Parameter ? "Parametro" : "Propiedad"))
                    {
                        if (ImGui::Selectable("Propiedad", pista.target == TrackTarget::Property))
                        {
                            pista.target = TrackTarget::Property;
                            anim->bindProperties(go, nullptr);
                        }
                        if (ImGui::Selectable("Parametro", pista.target == TrackTarget::Parameter))
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
                        // Solo los Float: una curva no puede escribir otra cosa.
                        // El nombre actual se ve aunque el parámetro ya no exista
                        // (la pista sale en rojo), para no perderlo en silencio.
                        const char* actual = pista.parameterName.empty() ? "(sin parametro)"
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
                                              ? "No hay un parametro Float con ese nombre:\n"
                                                "la curva no se aplica."
                                              : "El objeto no tiene el componente que necesita esta pista:\n"
                                                "no se aplica.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x###pista")) quitarPista = p;

                    // Una curva se lee contra los umbrales de las condiciones
                    // que miran su parámetro; una pista de propiedad no tiene
                    // ninguno que pintar.
                    float umbrales[4];
                    const int numUmbrales = pista.target == TrackTarget::Parameter
                                                ? anim->conditionThresholds(pista.parameterName, umbrales, 4)
                                                : 0;
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
                        // La nueva va al final del clip con el valor que el
                        // objeto tiene ahora: así se autora "desde aquí".
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
                if (ImGui::SmallButton("+ pista"))
                {
                    clip.tracks.push_back(PropertyTrack{});
                    anim->bindProperties(go, nullptr);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+ curva"))
                {
                    // Una curva escribe un parámetro; se crea sobre el primer
                    // Float que haya, y si no hay ninguno sale en rojo hasta que
                    // se declare uno.
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
                ctx.pushLog("Animator: clip de propiedades '" + nuevo.name + "' anadido");
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
    if (ImGui::Button("Todo el cuerpo"))
    {
        L.maskBones.clear();
        cambio = true;
    }
    if (!mesh)
    {
        ImGui::TextDisabled("El GameObject no tiene un mesh skinned.");
    }
    else
    {
        ImGui::TextDisabled("Clic: el hueso y su rama. Ctrl+clic: solo el hueso.");
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
        // La rama de b a v: el hueso y todos sus descendientes.
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
        // Alto según la pantalla (un esqueleto de 60 huesos no cabe en 420 px)
        // y scroll horizontal: cada nivel sangra, y en una rama profunda el
        // nombre se salía por la derecha sin forma de verlo.
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
    // rebindClips y no bindClips: puede estar corriendo Play Mode.
    if (cambio && mesh) anim->rebindClips(*mesh, nullptr);
    ImGui::EndPopup();
}

void AnimatorPanel::drawGraph(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    // Breadcrumb del nivel, encima del lienzo: es la única forma de salir de una
    // caja, y de ver dónde se está cuando el grafo visible no es el de la raíz.
    {
        const auto& sts = anim->states(m_layer);
        if (ImGui::SmallButton("Base###nivelRaiz")) m_nivel = -1;
        std::vector<int> cadena;
        for (int n = m_nivel; n >= 0 && n < (int)sts.size(); n = sts[(size_t)n].parent)
        {
            cadena.push_back(n);
            if ((int)cadena.size() > (int)sts.size()) break;   // jerarquía rota: no colgarse
        }
        for (int k = (int)cadena.size() - 1; k >= 0; k--)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("/");
            ImGui::SameLine();
            ImGui::PushID(cadena[(size_t)k]);
            if (ImGui::SmallButton(sts[(size_t)cadena[(size_t)k]].name.c_str()))
                m_nivel = cadena[(size_t)k];
            ImGui::PopID();
        }
    }

    ed::SetCurrentEditor(m_ctx);
    // El tooltip de un widget de dentro de un nodo NO se puede dibujar aquí:
    // entre ed::Begin y ed::End el ratón y las ventanas van en coordenadas del
    // lienzo, así que saldría desplazado por el pan y el zoom, y sacarlo con
    // ed::Suspend dentro de un nodo rompe el splitter de canales del lienzo
    // (IM_ASSERT en imgui_canvas.cpp: la aplicación se queda clavada al
    // arrastrar un nodo). Se anota el texto y se pinta al final, ya fuera.
    m_tooltipNodo.clear();
    ed::Begin("AnimatorCanvas");
    // Escribiendo en un campo de un nodo (nombre de evento), Supr borraría el
    // nodo seleccionado: imgui-node-editor mira la tecla, no el foco de texto.
    ed::EnableShortcuts(!ImGui::GetIO().WantTextInput);

    // --- Nodos ---
    const auto& states = anim->states(m_layer);
    // El nivel puede haber quedado apuntando a un estado que ya no existe (se
    // borró la caja que se estaba mirando por dentro): se vuelve a la raíz.
    if (m_nivel >= (int)states.size() || (m_nivel >= 0 && !states[(size_t)m_nivel].isSubMachine))
        m_nivel = -1;

    // Fila de pines del nodo: la usan el estado normal y la caja, que se dibuja
    // sin clip, loop ni eventos. Una sola copia para que las dos salgan iguales.
    auto pinesDelNodo = [](int eid, float headerW) {
        // "-> in" a la izquierda, "out ->" empujado al borde derecho del nodo
        // (el ancho de la cabecera es el ancho real del nodo).
        ed::BeginPin(inputPinId(eid), ed::PinKind::Input);
        ImGui::TextUnformatted("-> in");
        ed::EndPin();
        ImGui::SameLine();
        const float inW  = ImGui::CalcTextSize("-> in").x;
        const float outW = ImGui::CalcTextSize("out ->").x;
        const float pad  = headerW - inW - outW;
        // Nodo muy estrecho: un hueco fijo pequeño basta para que no se solapen.
        ImGui::Dummy(ImVec2(pad > 1.0f ? pad : 8.0f, 0.0f));
        ImGui::SameLine();
        ed::BeginPin(outputPinId(eid), ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();
    };
    for (size_t i = 0; i < states.size(); i++)
    {
        // Solo lo de ESTE nivel: los hijos de una caja se ven al entrar en ella.
        if (states[i].parent != m_nivel) continue;
        const int eid = states[i].editorId;
        ed::BeginNode(nodeId(eid));

        // Cabecera (nombre/entry, clip, loop) agrupada para poder medir su ancho
        // y así saber dónde cae el borde derecho del nodo: la fila de pines de
        // abajo lo necesita para separar "-> in" (izquierda) de "out ->" (derecha).
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

        // Una caja no reproduce nada: en vez del clip muestra por dónde se entra
        // y avisa cuando está vacía, que es cuando las transiciones hacia ella
        // no disparan.
        if (states[i].isSubMachine)
        {
            ImGui::TextDisabled("sub-maquina");
            const int ent = states[i].subEntry;
            if (ent >= 0 && ent < (int)states.size())
                ImGui::TextDisabled("entra por: %s", states[(size_t)ent].name.c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "sin entrada: no se puede entrar");
        }
        // clipIndex < 0: el clip del grafo no existe en el modelo (bindClips ya
        // avisó al cargar). Se marca aquí también o el nodo mentiría.
        else if (states[i].clipIndex < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "clip: %s (no existe)", states[i].clipName.c_str());
        else
            ImGui::TextDisabled("clip: %s", states[i].clipName.c_str());

        ImGui::PushID((int)i);

        // Jerarquía: en qué caja está, y —si él es una caja— por dónde se entra.
        // Botones y no combos: una lista dentro del nodo se abre en espacio de
        // canvas (ver drawBlendPickPopup).
        {
            const int pa = states[i].parent;
            const std::string etiqueta =
                std::string("padre: ") +
                (pa >= 0 && pa < (int)states.size() ? states[(size_t)pa].name : std::string("(raiz)")) +
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
                    std::string("entrada: ") +
                    (ent >= 0 && ent < (int)states.size() ? states[(size_t)ent].name : std::string("(ninguna)")) +
                    "###entrada";
                if (ImGui::SmallButton(etEnt.c_str()))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 5;
                }
            }
        }

        // Una caja no reproduce clip, así que loop, velocidad, root motion,
        // blend y eventos no le aplican: se salta todo eso.
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

        // Modo de la raíz. RadioButton y no combo: una lista dentro del nodo
        // se abre en espacio de canvas (ver drawBlendPickPopup).
        {
            using RM = AnimatorComponent::RootMotion;
            int modo = (int)states[i].rootMotion;
            ImGui::TextUnformatted("raiz:");
            ImGui::SameLine();
            if (ImGui::RadioButton("normal##rm", modo == 0)) modo = 0;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "La pose mueve la raiz: el clip se desplaza con su animacion.";
            ImGui::SameLine();
            if (ImGui::RadioButton("bloq.##rm", modo == 1)) modo = 1;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "Clava la traslacion de la raiz a su bind pose: el clip se reproduce en el sitio. La rotacion de la raiz y el resto de huesos animan igual.";
            ImGui::SameLine();
            if (ImGui::RadioButton("root motion##rm", modo == 2)) modo = 2;
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "El avance horizontal de la raiz mueve al GameObject (con Rigidbody dinamico, como velocidad). La Y se queda en la pose y la rotacion no se aplica.";
            if (modo != (int)states[i].rootMotion)
                anim->statesMutable(m_layer)[i].rootMotion = (RM)modo;
        }

        // Velocidad del estado y parámetro float que la multiplica (opcional).
        // DragFloat en vivo: el undo lo recoge el tracker del grafo.
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragFloat("speed", &anim->statesMutable(m_layer)[i].speed, 0.01f, 0.0f, 100.0f, "%.2f");
        if (anim->statesMutable(m_layer)[i].speed < 0.0f) anim->statesMutable(m_layer)[i].speed = 0.0f;
        if (ImGui::IsItemHovered())
            m_tooltipNodo = "Multiplica el ritmo del clip (1 = normal, 0 = congelado).";
        ImGui::SameLine();
        {
            const std::string& sp = states[i].speedParam;
            const std::string etiqueta = "x " + (sp.empty() ? std::string("(ninguno)") : sp) + "##speedparam";
            if (ImGui::Button(etiqueta.c_str()))
            {
                m_blendPickRequested = true;
                m_blendPickEditorId  = eid;
                m_blendPickKind      = 2;
            }
            if (ImGui::IsItemHovered())
                m_tooltipNodo = "Parametro float que multiplica la velocidad (como el Multiplier de Unity).";
        }

        // --- Blend 1D: clips extra con su umbral ---
        // Cada fila abre la lista de TODOS los clips de la malla con un botón y
        // no BeginCombo: la lista se abre fuera del nodo (ver
        // drawBlendPickPopup). Una entrada sin clip resuelto sale en rojo.
        auto& stMut = anim->statesMutable(m_layer)[i];
        if (go->getSkinnedMesh())
        {
            if (!stMut.blendEntries.empty())
            {
                // Solo parámetros float: son los únicos que dan un peso
                // continuo. Que la lista salga vacía es la pista de que hay que
                // declarar uno abajo, en Parameters.
                const std::string paramLabel = stMut.blendParam.empty()
                                               ? std::string("(sin parametro)") : stMut.blendParam;
                if (ImGui::Button(("by: " + paramLabel + "##by").c_str(), ImVec2(140.0f, 0.0f)))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 1;
                }
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragFloat("umbral##clipThr", &stMut.clipThreshold, 0.01f);
                if (ImGui::IsItemHovered())
                    m_tooltipNodo = "Umbral del clip principal del estado.";

                // --- Blend 2D ---
                // Con un segundo parámetro cada clip es un punto (umbral, Y) y
                // suenan los 3 del triángulo donde cae (X, Y).
                bool dosD = !stMut.blendParamY.empty();
                if (ImGui::Checkbox("2D##blend2d", &dosD))
                {
                    if (dosD)
                    {
                        // El primer Float que no sea el de X; si no hay otro, el
                        // mismo (se cambia en el selector). Sin ninguno Float la
                        // casilla no se queda marcada.
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
                    m_tooltipNodo = "Blend 2D: cada clip es un punto (umbral X, umbral Y).";
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
                        m_tooltipNodo = "Umbral Y del clip principal del estado.";
                }
            }

            int quitar = -1;
            for (int k = 0; k < (int)stMut.blendEntries.size(); k++)
            {
                auto& e = stMut.blendEntries[k];
                ImGui::PushID(k);
                const bool roto = e.clipIndex < 0;
                if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                const std::string lbl = e.clipName.empty() ? std::string("(elige clip)") : e.clipName;
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
                // Umbral por encima de todos: la entrada nueva no roba el peso
                // a las que ya estaban hasta que el usuario la mueva.
                float maxT = stMut.clipThreshold;
                for (const auto& e : stMut.blendEntries) maxT = std::max(maxT, e.threshold);
                AnimatorComponent::BlendEntry nueva;
                nueva.threshold = maxT + 1.0f;
                stMut.blendEntries.push_back(nueva);
            }

            if (!stMut.blendEntries.empty() && !stMut.blendParamY.empty())
                drawBlend2DCanvas(*anim, stMut);
        }

        // --- Clip de propiedades del estado ---
        // Lo que permite que un objeto SIN esqueleto tenga estados: aquí se
        // elige qué clip autorado reproduce cada uno.
        if (!anim->propertyClips().empty())
        {
            ImGui::PushID("propclip");
            ImGui::SetNextItemWidth(150.0f);
            const bool roto = !stMut.propertyClipName.empty() && stMut.propertyClipIndex < 0;
            if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            if (ImGui::BeginCombo("##propclip",
                                  stMut.propertyClipName.empty() ? "(sin clip de propiedades)"
                                                                 : stMut.propertyClipName.c_str()))
            {
                if (ImGui::Selectable("(ninguno)", stMut.propertyClipName.empty()))
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

        // --- Eventos del estado ---
        // No dependen de la malla: son instantes del ciclo con nombre, que en
        // Play llegan a Lua como OnAnimationEvent.
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
                m_tooltipNodo = "Instante del ciclo, normalizado (0 = inicio, 1 = final).";
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) quitarEvento = k;
            ImGui::PopID();
        }
        if (quitarEvento >= 0)
            stMut.events.erase(stMut.events.begin() + quitarEvento);
        if (ImGui::Button("+ evento##addEvent", ImVec2(140.0f, 0.0f)))
            stMut.events.push_back({ "", 0.5f });
        ImGui::PopID();
        ImGui::PopID();

        ImGui::EndGroup();
        const float headerW = ImGui::GetItemRectSize().x;

        pinesDelNodo(eid, headerW);

        ed::EndNode();
    }

    // --- Nodo Any State ---
    // Solo si hay estados: sin ellos no hay adónde ir. Solo tiene salida.
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
        // fromState/toState son índices del vector m_states (no editorIds: ese
        // es el contrato de Transition, ver comentario en el header). Hay que
        // convertirlos a editorId antes de construir los ids de pin del canvas.
        const bool desdeAny = from == AnimatorComponent::kAnyState;
        if ((!desdeAny && (from < 0 || from >= (int)states.size())) ||
            to < 0 || to >= (int)states.size()) continue;
        // Un extremo que vive dentro de otra caja se dibuja CONTRA la caja: si
        // no, la transición desaparecería del grafo y parecería no existir.
        auto visible = [&](int estado) {
            int s = estado;
            while (s >= 0 && s < (int)states.size() && states[(size_t)s].parent != m_nivel)
                s = states[(size_t)s].parent;
            return s;
        };
        const int vFrom = desdeAny ? from : visible(from);
        const int vTo   = visible(to);
        // Fuera de esta rama, o los dos extremos en el mismo nodo visible: no
        // hay nada que dibujar en este nivel.
        if ((!desdeAny && vFrom < 0) || vTo < 0 || (!desdeAny && vFrom == vTo)) continue;
        ed::Link(linkId((int)t),
                 desdeAny ? kAnyStateOutPinId : outputPinId(states[vFrom].editorId),
                 inputPinId(states[vTo].editorId));
    }

    // --- Crear links arrastrando de pin a pin ---
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b)
        {
            const int pa = (int)a.Get();
            const int pb = (int)b.Get();
            // El usuario puede arrastrar en cualquier dirección: se normaliza a
            // (salida -> entrada).
            const int outPin = esPinDeSalida(pa) ? pa : pb;
            const int inPin  = esPinDeSalida(pa) ? pb : pa;

            if (esPinDeSalida(outPin) && !esPinDeSalida(inPin) && inPin != kAnyStateNodeId &&
                ed::AcceptNewItem())
            {
                // stateFromPin (índice) en vez de directamente el editorId: las
                // transiciones guardan índices del vector, no editorIds. El pin
                // de Any State no se decodifica: es el centinela.
                const int fromIdx = (outPin == kAnyStateOutPinId)
                                    ? AnimatorComponent::kAnyState
                                    : stateIndexFromPin(*anim, outPin, m_layer);
                const int toIdx   = stateIndexFromPin(*anim, inPin, m_layer);
                if ((fromIdx >= 0 || fromIdx == AnimatorComponent::kAnyState) && toIdx >= 0)
                {
                    AnimatorComponent::Transition tr;
                    tr.fromState = fromIdx;
                    tr.toState   = toIdx;
                    // Sin condiciones no dispara nunca (por diseño): el usuario las
                    // añade con doble clic en el link.
                    m_graphUndo.setLabel("Crear transición");
                    anim->addTransition(tr, m_layer);
                    ctx.pushLog("Animator: transición creada (sin condiciones todavía)");
                }
            }
        }
    }
    ed::EndCreate();

    // --- Borrar nodos y links ---
    if (ed::BeginDelete())
    {
        // Con box-select + Supr, una sola pasada de BeginDelete puede traer
        // varios ids (varios QueryDeletedNode/QueryDeletedLink). Si se borrara
        // cada uno según se acepta, el primer erase reindexa el vector y los
        // ids ya encolados (calculados antes de ese erase) pasan a apuntar a
        // otro elemento o se salen de rango — removeState/removeTransition
        // hacen bounds-check silencioso y ese elemento sobrevive sin más aviso.
        // Por eso se recogen todos los índices primero y se borran después, de
        // atrás hacia adelante: así cada erase solo desplaza índices ya
        // procesados, nunca los que quedan pendientes en esta misma pasada.
        std::vector<int> transitionsToRemove;
        ed::LinkId dl;
        while (ed::QueryDeletedLink(&dl))
            if (ed::AcceptDeletedItem())
                transitionsToRemove.push_back((int)dl.Get() - 100000);

        // El id de nodo que trae QueryDeletedNode es un nodeId(editorId): se
        // decodifica a editorId y se resuelve al índice actual del vector
        // escaneando por editorId (stateIndexFromPin sirve igual aquí pese al
        // nombre — decode + scan es exactamente lo mismo pa un id de nodo que
        // pa uno de pin, la fórmula es la misma división entera).
        std::vector<int> statesToRemove;
        ed::NodeId dn;
        while (ed::QueryDeletedNode(&dn))
        {
            // El nodo Any State no se borra: existe mientras haya estados.
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

        // Se borran los links explícitamente pedidos antes que los estados: un
        // estado borrado se lleva también sus transiciones (removeState las
        // reindexa/purga), así que procesar los links sueltos primero evita
        // pisarse con esa purga automática. El descending-erase sigue siendo
        // necesario con multi-delete: cada removeState desplaza los índices por
        // encima de idx, así que hay que ir de atrás hacia adelante pa que cada
        // erase no invalide los índices ya calculados y pendientes en este mismo
        // vector (statesToRemove son índices tomados ANTES de borrar nada).
        if (!statesToRemove.empty()) m_graphUndo.setLabel("Borrar estado");
        std::sort(statesToRemove.rbegin(), statesToRemove.rend());
        for (int idx : statesToRemove)
            // removeState reindexa las transiciones supervivientes.
            anim->removeState(idx, m_layer);

        // A diferencia de antes (Task 10), YA NO hace falta desvincular
        // m_boundTo aquí: con ids estables por editorId, un superviviente no
        // cambia de id al reindexarse el vector, así que su nodo en el canvas
        // sigue siendo el mismo nodo (mismo id) con su misma posición — no hay
        // slot visual que heredar del borrado. syncPositionsToComponent de más
        // abajo puede leer las posiciones este mismo frame sin corromper nada.
    }
    ed::EndDelete();

    // --- Menús contextuales ---
    ed::Suspend();
    ed::NodeId ctxNode;
    ed::LinkId ctxLink;
    if (ed::ShowNodeContextMenu(&ctxNode))
    {
        ImGui::OpenPopup("node_ctx");
        m_conditionsFor = -1;
        // Miembro plano en vez de ImGui::GetStateStorage(): un solo popup de
        // nodo puede estar abierto a la vez, así que no hace falta la
        // indirección de la state storage de ImGui pa smuggle-ar el índice
        // hasta el popup diferido — un int en el panel llega igual de lejos.
        // ctxNode.Get() decodifica a un editorId, no al índice del vector que
        // "Set as Entry" necesita (setEntryState(idx)) — de ahí el paso por
        // stateIndexFromPin.
        m_nodeCtxTarget = stateIndexFromPin(*anim, (int)ctxNode.Get(), m_layer);
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        // Sin popup intermedio "Edit Conditions...": abrir "conditions" desde
        // dentro del BeginPopup/EndPopup de otro popup (link_ctx) anidaba un
        // OpenPopup dentro de otro, frágil y flaky. Un click derecho en el link
        // abre el editor de condiciones directamente.
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
                ctx.pushLog("Animator: '" + anim->states(m_layer)[idx].name + "' es ahora el estado de entrada");
            }
        }
        ImGui::EndPopup();
    }
    drawConditionsPopup(ctx, go);
    drawBlendPickPopup(go);
    ed::Resume();

    // Doble clic en una caja: se entra a ver lo que tiene dentro. Se consulta
    // ANTES de ed::End, que es donde el lienzo aún tiene el estado del frame.
    if (ed::NodeId doble = ed::GetDoubleClickedNode())
    {
        // nodeId(eid) = eid * 3 + 1 (ver los helpers de arriba): el editorId
        // sale de deshacer esa cuenta.
        const int eid = ((int)doble.Get() - 1) / 3;
        const int idx = anim->stateIndexByEditorId(eid, m_layer);
        if (idx >= 0 && anim->states(m_layer)[(size_t)idx].isSubMachine) m_nivel = idx;
    }

    ed::End();
    // Ya fuera del lienzo: aquí el ratón vuelve a estar en coordenadas de
    // pantalla y el tooltip sale junto al cursor.
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
    // El estado se borró con la lista abierta. La malla solo hace falta para
    // los kinds que listan clips: un objeto sin esqueleto también elige padre
    // y entrada de sub-máquina.
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
        // Padre: la raíz o cualquier caja que no sea él mismo ni esté DENTRO de
        // él (meter una caja dentro de sí misma haría un ciclo y dejaría a sus
        // hijos inalcanzables).
        if (ImGui::Selectable("(raiz)", st.parent < 0)) st.parent = -1;
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
        // Entrada de la caja: cualquiera de sus hijos DIRECTOS. Si no tiene, la
        // caja está vacía y no se puede entrar en ella.
        const auto& sts = anim->states(m_layer);
        bool alguno = false;
        for (int i = 0; i < (int)sts.size(); i++)
        {
            if (sts[(size_t)i].parent != idx) continue;
            alguno = true;
            if (ImGui::Selectable(sts[(size_t)i].name.c_str(), st.subEntry == i)) st.subEntry = i;
        }
        if (!alguno)
            ImGui::TextDisabled("La sub-maquina esta vacia: mete algun estado con 'padre'.");
    }
    else if (m_blendPickKind == 2)
    {
        // Multiplicador de velocidad: "(ninguno)" o cualquier parámetro float.
        if (ImGui::Selectable("(ninguno)", st.speedParam.empty()))
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
            ImGui::TextDisabled("No hay parámetros float: declara uno en Parameters.");
    }
    else if (m_blendPickKind == 0)
    {
        // Lista TODOS los clips de la malla, no solo los que ya usa el grafo:
        // el motor admite cualquiera de ellos, el principal incluido (en un 1D,
        // el mismo clip con otro umbral hace de meseta). La entrada puede haber
        // desaparecido: se quitó con la "x" con el popup abierto.
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
            // El índice y la duración los resuelve rebindClips por nombre; sin
            // esto la entrada quedaría a -1 hasta recargar la escena.
            // rebindClips y no bindClips: puede estar corriendo Play Mode y
            // bindClips reiniciaría el grafo y los parámetros del usuario.
            if (cambio)
                anim->rebindClips(*mesh, nullptr);
        }
    }
    else
    {
        // Parámetro X (kind 1) o Y (kind 3) del blend. Solo parámetros float:
        // son los únicos que dan un peso continuo. Que la lista salga vacía es
        // la pista de que hay que declarar uno en Parameters.
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
            ImGui::TextDisabled("No hay parámetros float: declara uno en Parameters.");
    }
    ImGui::EndPopup();
}

void AnimatorPanel::drawConditionsPopup(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    if (m_conditionsFor < 0 || m_conditionsFor >= (int)anim->transitions(m_layer).size()) return;

    if (!ImGui::BeginPopup("conditions")) return;

    auto& tr = anim->transitionsMutable(m_layer)[m_conditionsFor];

    // Cross-fade de ESTA transición, en segundos. 0 = corte seco, que es lo que
    // hacía el motor antes y lo que traen las escenas viejas.
    ImGui::TextUnformatted("Transition");
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("cross-fade (s)", &tr.duration, 0.01f, 0.0f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Segundos de mezcla con el estado de origen. 0 = corte instantaneo.");
    // DragFloat con min 0 ya lo impide al arrastrar, pero no al teclear un
    // valor: un negativo dejaria blendWeight fuera de [0,1].
    if (tr.duration < 0.0f) tr.duration = 0.0f;

    // Exit time: la transición espera a que el estado de origen llegue a
    // exitTime (normalizado; 1 = fin del clip, >1 cuenta vueltas). Sin
    // condiciones dispara solo por tiempo.
    ImGui::PushID("exit_time");
    ImGui::Checkbox("Has Exit Time", &tr.hasExitTime);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Espera a que el estado de origen llegue a 'exit time'. Sin condiciones, dispara solo por tiempo.");
    if (tr.hasExitTime)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("exit time", &tr.exitTime, 0.01f, 0.0f, 100.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tiempo normalizado: 0.9 = al 90%% del clip, 2.5 = tras dos vueltas y media.");
        if (tr.exitTime < 0.0f) tr.exitTime = 0.0f;
    }
    if (tr.fromState == AnimatorComponent::kAnyState)
    {
        ImGui::Checkbox("Can Transition To Self", &tr.canTransitionToSelf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Si puede volver al estado en el que ya se está. Encendido con un bool, lo reiniciaría cada frame.");
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
            // Los cuatro comparadores para ambos tipos: el evaluador siempre
            // los soportó (ver AnimatorComponent::conditionsMet) y recortar el
            // combo a 2 para Float solo servía para esconderlos.
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
                // El umbral vive en float pa no duplicar el campo; la UI de Int
                // pasa por un int temporal, así que por aquí nunca entra un
                // valor con parte fraccionaria y lo que se ve es exactamente lo
                // que evalúa conditionsMet (que redondea, no trunca, pa cubrir
                // los que llegan de un JSON editado a mano).
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
    // Solo se ofrecen parámetros ya declarados: una condición sobre un parámetro
    // inexistente no dispararía nunca y no habría forma de saber por qué.
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        // Selectable con DontClosePopups en vez de MenuItem: un MenuItem cierra
        // el popup al primer click y el usuario solo podría añadir una condición
        // por apertura. Con esto puede encadenar varias "Add:" seguidas.
        if (ImGui::Selectable(("Add: " + p.name).c_str(), false, ImGuiSelectableFlags_DontClosePopups))
        {
            AnimatorComponent::Condition cond;
            // El tipo del parámetro decide el de la condición 1:1.
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
    // "animation finished" nunca dispara saliendo de un estado en loop (un
    // clip en loop nunca "termina", ver AnimatorComponent::update): se deja
    // el item pero deshabilitado, con tooltip, para que el usuario no arme un
    // link muerto sin saberlo.
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
        ImGui::SetTooltip("El estado de origen está en loop: 'animation finished' nunca dispara aquí. Para salir por tiempo, usa 'Has Exit Time'.");

    ImGui::EndPopup();
}

void AnimatorPanel::importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path)
{
    const SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_animSrcError = "Formato no soportado: " + ext;
        return;
    }

    // Ensayo en seco sobre una copia: el comando no puede decir "no" a medias
    // (AnimationSourceCommand::applyAdd descarta los warnings de
    // addAnimationSource y simplemente no hace nada si falla), y así el error
    // del loader (rig equivocado, fichero sin animaciones) llega al usuario
    // antes de meter nada en el stack de undo.
    SkinnedMesh probe = *mesh;
    std::vector<std::string> warnings;
    const bool ok = addAnimationSource(probe, path, warnings);
    for (const auto& w : warnings) ctx.pushLog("Animator: " + w);

    if (!ok)
    {
        m_animSrcError = warnings.empty() ? ("No se pudieron importar animaciones de " + path)
                                           : warnings.back();
        return;
    }
    m_animSrcError.clear();

    auto cmd = std::make_unique<AnimationSourceCommand>(
        *ctx.scene, ctx.renderer, "Añadir animaciones", go->id,
        /*add=*/true, path, std::vector<std::string>{});
    cmd->execute();
    ctx.undo->push(std::move(cmd));
    ctx.pushLog("Animator: animaciones de '" + path + "' importadas");
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
        // El path como ID en vez del índice s: quitar una fuente reindexa el
        // vector, y si el ID fuera el índice, cada fila posterior heredaría
        // el estado abierto/cerrado (y el m_renamingClip en vuelo, si lo
        // hubiera) de la fila que ocupaba su índice antes del borrado. El
        // path es estable mientras la fuente exista.
        //
        // El path SOLO no basta: dos fuentes pueden compartir path a
        // propósito (reimportar el mismo fichero), y con el mismo ID las dos
        // filas compartirían el estado del TreeNode — expandir una expandiría
        // la otra. Se compone con cuántas fuentes anteriores repiten ese path,
        // que distingue las copias sin depender del índice absoluto.
        int pathOccurrence = 0;
        for (size_t k = 0; k < s; k++)
            if (mesh->animationSources[k].path == src.path) pathOccurrence++;
        ImGui::PushID(src.path.c_str());
        ImGui::PushID(pathOccurrence);

        const std::string file = std::filesystem::path(src.path).filename().string();
        const std::string label = file + "  (" + std::to_string(src.clipNames.size()) + " clips)"
                                + (src.builtin ? "  [modelo]" : "");

        // AllowOverlap: el nodo ocupa la fila entera (SpanAvailWidth) y la "X"
        // se pinta ENCIMA de él. Sin el flag, ImGui da el clic al primer item
        // enviado, el nodo, y la X no hacía nada más que plegar la fila.
        const bool open = ImGui::TreeNodeEx("##src",
                                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                            "%s", label.c_str());

        // La fuente builtin es el FBX del modelo: quitarla dejaría la malla sin
        // el fichero que la creó, así que el botón existe pero deshabilitado
        // (mostrarlo y explicarlo enseña la regla; ocultarlo la esconde).
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        ImGui::BeginDisabled(src.builtin);
        if (ImGui::SmallButton("X")) sourceToRemove = (int)s;
        ImGui::EndDisabled();
        if (src.builtin && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Es el FBX del modelo: sus animaciones no se pueden quitar por separado.");

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

                        // Validación ANTES de ejecutar nada, replicando exactamente
                        // las reglas de rechazo de renameClip (ver
                        // SkinnedMeshAnimations.cpp): nombre vacío, nombre ya usado
                        // por cualquier clip, o nombre idéntico al actual. Antes se
                        // ejecutaba el comando primero y se inferÍa el éxito
                        // escaneando el mesh por el nombre nuevo — eso no podía
                        // distinguir "el rename se aplicó" de "ya existía un clip
                        // con ese nombre", que es justo el motivo por el que
                        // renameClip lo rechaza: un duplicado (p.ej. renombrar
                        // "Walk" a "Idle" cuando "Idle" ya existe) se colaba en el
                        // undo stack como si hubiera funcionado, y el siguiente
                        // Ctrl+Z del usuario no deshacía nada porque el comando no
                        // había mutado nada.
                        bool rechazado = nuevo.empty() || nuevo == clipName;
                        if (!rechazado)
                            for (const auto& c : mesh->animationClips)
                                if (c.name == nuevo) { rechazado = true; break; }

                        if (rechazado)
                        {
                            m_animSrcError = "No se pudo renombrar a '" + nuevo
                                            + "': nombre vacío, duplicado o igual al actual";
                        }
                        else
                        {
                            // Copia del nombre viejo ANTES de execute(): clipName
                            // es una const std::string& que apunta directo al
                            // elemento de src.clipNames, y renameClip lo reescribe
                            // in-place — tras execute() clipName ya vale "nuevo",
                            // así que usarla en el log duplicaría el nombre nuevo
                            // en vez de mostrar qué cambió.
                            const std::string viejo = clipName;
                            auto cmd = std::make_unique<ClipRenameCommand>(
                                *ctx.scene, "Renombrar clip", go->id, viejo, nuevo);
                            cmd->execute();
                            ctx.undo->push(std::move(cmd));
                            m_animSrcError.clear();
                            ctx.pushLog("Animator: clip '" + viejo + "' renombrado a '" + nuevo + "'");
                        }
                        m_renamingClip.clear();
                    }
                    // Clic fuera sin pulsar Enter (InputTextFlags_EnterReturnsTrue
                    // no dispara ahí): se cierra el modo edición sin tocar nada,
                    // ni el mesh ni el undo stack — el usuario se arrepintió.
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
        // Diferido fuera del for: mutar animationSources dentro del propio
        // bucle que lo recorre invalidaría el iterador de este mismo frame.
        const AnimationSource& src = mesh->animationSources[(size_t)sourceToRemove];

        // Ordinal contado DESDE EL FINAL (0 = la última con ese path), no
        // desde el principio: es lo que AnimationSourceCommand::applyRemove
        // espera (ver el comentario largo ahí sobre por qué el escaneo va
        // desde el final). Cuenta cuántas fuentes no-builtin con el mismo
        // path hay POR DELANTE de la fila pulsada: si el usuario quita la
        // primera de dos filas con el mismo path, esto vale 1 (la segunda
        // fila queda por delante), y el comando la salta correctamente en
        // vez de quitar -por backward-scan ciego- la última (el bug que
        // corrige este fix).
        size_t pathOccurrence = 0;
        for (size_t k = (size_t)sourceToRemove + 1; k < mesh->animationSources.size(); k++)
        {
            const auto& later = mesh->animationSources[k];
            if (!later.builtin && later.path == src.path) pathOccurrence++;
        }

        auto cmd = std::make_unique<AnimationSourceCommand>(
            *ctx.scene, ctx.renderer, "Quitar animaciones", go->id,
            /*add=*/false, src.path, src.clipNames, pathOccurrence);
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        // Los estados que usaran esos clips quedan huérfanos a propósito: el
        // grafo es trabajo del usuario y borrarlo por él sería peor que dejarlo
        // avisado. AnimationSourceCommand::applyRemove ya llama a
        // rebindClips en caliente, así que clipIndex queda en -1 en el
        // momento del borrado (no hace falta esperar a una recarga de
        // escena o a un ciclo de Play/Stop).
        ctx.pushLog("Animator: fuente de animación quitada; los estados que la usaran quedan sin clip");
    }

    if (ImGui::Button("Add Animation FBX..."))
    {
        IGFD::FileDialogConfig cfg;
        // "assets", como el diálogo de malla de PropertiesPanel: los FBX de
        // este proyecto viven ahí, y abrir en la raíz del repo obligaría a
        // navegar cada vez.
        cfg.path = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_animSrcDialog->OpenDialog("AddAnimSrcDlg", "Choose Animation FBX", ".fbx", cfg);
        m_animSrcDlgOpen = true;
        // Se captura el id AHORA, al abrir, no ctx.selected al drenar: el
        // diálogo no es modal, así que el usuario puede cambiar de selección
        // mientras elige el fichero — el FBX debe ir a "go" (a quien estaba
        // seleccionado al pulsar el botón), no a quien sea que esté
        // seleccionado cuando el usuario por fin cierra el diálogo.
        m_animSrcDlgTarget = go->id;
    }

    // Drop target: el Content Browser emite "DT_ASSET_PATH" (ver
    // ContentBrowserPanel::BeginDragDropSource), no "CONTENT_BROWSER_ITEM" —
    // mismo id y mismo patrón (payload->Data es char* terminado en '\0',
    // tamaño = fullPath.size()+1) que usa PropertiesPanel::drawMeshSection
    // para su propio drop target de .fbx.
    ImGui::SameLine();
    ImGui::TextDisabled("(o arrastra un .fbx aquí)");
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
        // Resuelve por id (capturado al abrir el diálogo, ver OpenDialog más
        // arriba), no ctx.selected: para cuando el usuario cierra el diálogo
        // la selección puede haber cambiado, y el objeto original puede
        // incluso haberse borrado. Sin este resuelto explícito, un guard tipo
        // "ctx.selected && ctx.selected->hasAnimator()" habría importado el
        // FBX en el GameObject equivocado sin avisar, o lo habría descartado
        // en silencio si el actualmente seleccionado no tuviera Animator.
        GameObject* target = ctx.scene ? ctx.scene->findById(m_animSrcDlgTarget) : nullptr;
        if (!target)
        {
            m_animSrcError = "El GameObject de destino ya no existe en la escena";
        }
        else if (!target->getSkinnedMesh())
        {
            m_animSrcError = "'" + target->name + "' ya no tiene un mesh skinned";
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
    // El cuerpo va en un bloque, no en early-returns: drawAnimationSourceDialog()
    // de más abajo tiene que ejecutarse SIEMPRE, tanto si el panel está cerrado
    // (m_open == false, p.ej. tras pulsar la X de la ventana, que Begin escribe
    // directo en m_open) como si Begin devuelve false (ventana colapsada). Con
    // los early-returns de antes, cerrar o colapsar el panel mientras el
    // diálogo de fichero estaba abierto dejaba m_animSrcDlgOpen (y el estado
    // interno de IGFD) atascados en true para siempre: el diálogo resucitaba
    // solo, sin pedirlo, al reabrir el panel. Begin/End se llaman siempre en
    // pareja pase lo que pase (regla de ImGui), de ahí el End() incondicional
    // dentro del if(m_open). Mismo patrón que PropertiesPanel::draw +
    // drawMeshDialog.
    // Solo hay sesión de undo mientras se dibuja un grafo: panel cerrado,
    // colapsado o sin Animator la descartan (ver el final de la función).
    bool grafoDibujado = false;
    if (m_open)
    {
        if (ImGui::Begin("Animator", &m_open))
        {
            GameObject* go = ctx.selected;
            if (!go || !go->hasAnimator())
            {
                ImGui::TextDisabled("Selecciona un GameObject con componente Animator.");
                ImGui::TextDisabled("Properties > Add > Animator");
                m_boundTo = nullptr;
            }
            else
            {
                // Cambio de objeto vinculado: se comprueba UNA vez aquí arriba
                // (antes de dibujar nada) para poder limpiar m_renamingClip antes
                // de que drawAnimationSources lo lea. Si no, un clip con el mismo
                // nombre en el nuevo GameObject heredaría el modo edición y el
                // buffer del clip del objeto anterior durante un frame.
                const bool selectionChanged = (m_boundTo != go);
                if (selectionChanged) { m_renamingClip.clear(); m_layer = 0; m_nivel = -1; m_renamingLayer = -1; }

                // Undo del grafo: el bracket envuelve TODO lo que puede mutar el
                // componente en este frame, desde drawAnimationSources hasta el
                // popup de condiciones que se dibuja dentro de drawGraph.
                m_graphUndo.beginFrame(go->id, go->getAnimator().get(), ctx.undo->revision());
                grafoDibujado = true;
                const bool historialMovido = ctx.undo->revision() != m_lastUndoRevision;

                // Columna izquierda en un hijo CON SCROLL: fuentes, capas, IK,
                // "Add State" y parámetros. Antes iban sueltos en la ventana y,
                // al crecer (varias capas, varias restricciones de IK), lo de
                // abajo quedaba fuera sin forma de llegar, y de paso aplastaban
                // el lienzo. El lienzo sigue aparte, a la derecha: su rueda y su
                // pan son suyos y este scroll no los toca.
                // El máximo se acota también a la ventana: si se encoge, la
                // columna no puede quedarse más ancha que ella y dejar el
                // lienzo sin sitio.
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

                // --- Añadir estado desde los clips del modelo ---
                const SkinnedMesh* mesh = go->getSkinnedMesh();
                if (!mesh || mesh->animationClips.empty())
                {
                    ImGui::TextDisabled("Sin mesh skinned con animaciones: los estados salen de\n"
                                        "los clips de propiedades (abajo).");
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
                        // El nodo es nuevo: hay que colocarlo en el canvas a mano, el
                        // sync general solo corre al cambiar de objeto.
                        ed::SetCurrentEditor(m_ctx);
                        ed::SetNodePosition(nodeId(eid), ImVec2(st.editorPos.x, st.editorPos.y));
                        ed::SetCurrentEditor(nullptr);
                        ctx.pushLog("Animator: estado '" + st.name + "' añadido");
                    }
                    ImGui::EndCombo();
                }

                auto anim = go->getAnimator();

                // --- Añadir una sub-máquina ---
                // Se crea en el nivel que se está viendo: crear una caja dentro
                // de otra es entrar primero y pulsar aquí.
                if (ImGui::Button("Add Sub-State Machine"))
                {
                    AnimatorComponent::State caja;
                    caja.name         = "Sub-Machine";
                    caja.isSubMachine = true;
                    caja.parent       = m_nivel;
                    const int idx = anim->addState(caja, m_layer);
                    const int eid = anim->states(m_layer)[(size_t)idx].editorId;
                    ed::SetCurrentEditor(m_ctx);
                    ed::SetNodePosition(nodeId(eid), ImVec2(caja.editorPos.x, caja.editorPos.y));
                    ed::SetCurrentEditor(nullptr);
                    ctx.pushLog("Animator: sub-maquina anadida");
                }

                // --- Añadir estado desde un clip de propiedades ---
                // Sin esto, un objeto SIN esqueleto no podía tener ni un estado
                // (la única forma de crearlos eran los clips del modelo), así
                // que su clip de propiedades no lo reproducía nadie.
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
                        // El índice del clip y la duración del estado (que sin
                        // clip de malla sale del de propiedades) los resuelve
                        // bindProperties.
                        anim->bindProperties(go, nullptr);
                        const int eid = anim->states(m_layer)[idx].editorId;
                        ed::SetCurrentEditor(m_ctx);
                        ed::SetNodePosition(nodeId(eid), ImVec2(st.editorPos.x, st.editorPos.y));
                        ed::SetCurrentEditor(nullptr);
                        ctx.pushLog("Animator: estado '" + st.name + "' añadido");
                    }
                    ImGui::EndCombo();
                }

                drawParameterList(ctx, go);
                ImGui::EndChild();
                ImGui::SameLine();

                // Agarre para arrastrar el borde. Un InvisibleButton y no un
                // Separator: hace falta que capture el arrastre (IsItemActive)
                // para seguir moviéndolo aunque el cursor se salga del rect.
                ImGui::InvisibleButton("##agarreColumna",
                                       ImVec2(kAnchoAgarre, ImGui::GetContentRegionAvail().y));
                const bool agarreActivo  = ImGui::IsItemActive();
                const bool agarreEncima  = ImGui::IsItemHovered();
                if (agarreActivo || agarreEncima) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (agarreActivo) m_anchoColumna = std::clamp(m_anchoColumna + ImGui::GetIO().MouseDelta.x,
                                                              kAnchoColumnaMin, anchoMax);
                // Sin pintarlo no se ve dónde agarrar: es invisible por diseño.
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    ImGui::GetColorU32(agarreActivo ? ImGuiCol_SeparatorActive
                                                    : (agarreEncima ? ImGuiCol_SeparatorHovered
                                                                    : ImGuiCol_Separator)));
                ImGui::SameLine();

                ImGui::BeginChild("canvas", ImVec2(0, 0), false);
                // El undo puede haber quitado la capa seleccionada.
                m_layer = std::clamp(m_layer, 0, go->getAnimator()->layerCount() - 1);
                const bool capaCambiada = (m_layer != m_boundLayer);
                if (selectionChanged || historialMovido || capaCambiada)
                {
                    // Cambio de selección: el canvas todavía tiene las posiciones del
                    // objeto anterior. Se vuelca una vez, no cada frame — si no, el
                    // usuario no podría arrastrar los nodos.
                    // Un movimiento del historial (undo/redo, o cualquier push) puede
                    // haber reinsertado estados cuyo editorId el canvas no conoce, así
                    // que las posiciones se vuelcan una vez también en ese caso: no es
                    // solo por un undo, dispara con cualquier movimiento del historial.
                    ed::SetCurrentEditor(m_ctx);
                    syncPositionsFromComponent(go);
                    ed::SetCurrentEditor(nullptr);
                    m_boundTo    = go;
                    m_boundLayer = m_layer;
                }
                drawGraph(ctx, go);
                // Sync inverso cada frame, incondicional (ya no hace falta el guardia
                // de Task 10 que lo saltaba tras un borrado): con editorId estable, un
                // superviviente conserva su id de nodo pase lo que pase con el vector,
                // así que GetNodePosition(nodeId(editorId)) siempre lee la posición
                // del nodo correcto, incluso el mismo frame en que se borró otro nodo.
                ed::SetCurrentEditor(m_ctx);
                syncPositionsToComponent(go);
                ed::SetCurrentEditor(nullptr);

                // Fin del bracket del undo. IsAnyItemActive: mientras un drag
                // siga activo, el gesto no ha terminado y no se apila nada.
                if (auto cmd = m_graphUndo.endFrame(*ctx.scene, go->getAnimator().get(),
                                                    ImGui::IsAnyItemActive(), ctx.undo->revision()))
                {
                    ctx.pushLog("Animator: " + cmd->label());
                    // Sin execute(): el cambio ya está aplicado (contrato de push).
                    ctx.undo->push(std::move(cmd));
                }
                m_lastUndoRevision = ctx.undo->revision();
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    if (!grafoDibujado) m_graphUndo.discard();

    // Incondicional y fuera de la ventana: tiene que drenarse aunque el panel
    // esté cerrado/colapsado o la selección haya cambiado mientras el diálogo
    // estaba abierto (ver comentario grande al principio de esta función).
    drawAnimationSourceDialog(ctx);
}

} // namespace DonTopo
