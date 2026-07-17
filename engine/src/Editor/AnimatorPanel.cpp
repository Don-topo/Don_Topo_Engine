#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace DonTopo {

namespace {
    // IDs del node editor: tienen que ser != 0 y no colisionar entre nodos,
    // pines y links. Tres slots por estado + un rango aparte pa los links.
    int nodeId(int stateIdx)      { return stateIdx * 3 + 1; }
    int inputPinId(int stateIdx)  { return stateIdx * 3 + 2; }
    int outputPinId(int stateIdx) { return stateIdx * 3 + 3; }
    int linkId(int transIdx)      { return 100000 + transIdx; }

    // Inverso de outputPinId/inputPinId.
    int stateFromPin(int pin) { return (pin - 1) / 3; }
    bool isOutputPin(int pin) { return (pin - 1) % 3 == 2; }

    const char* condLabel(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animation finished";
            default:                                                  return "bool";
        }
    }
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
}

AnimatorPanel::~AnimatorPanel()
{
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void AnimatorPanel::syncPositionsFromComponent(GameObject* go)
{
    const auto& states = go->getAnimator()->states();
    for (size_t i = 0; i < states.size(); i++)
        ed::SetNodePosition(nodeId((int)i), ImVec2(states[i].editorPos.x, states[i].editorPos.y));
}

void AnimatorPanel::syncPositionsToComponent(GameObject* go)
{
    auto& states = go->getAnimator()->statesMutable();
    for (size_t i = 0; i < states.size(); i++)
    {
        const ImVec2 p = ed::GetNodePosition(nodeId((int)i));
        states[i].editorPos = glm::vec2(p.x, p.y);
    }
}

void AnimatorPanel::drawParameterList(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    ImGui::BeginChild("params", ImVec2(200, 0), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    std::string toRemove;
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", p.type == AnimatorComponent::ParamType::Trigger ? "(trigger)" : "(bool)");
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = p.name;
        ImGui::PopID();
    }
    // Diferido: borrar dentro del for-range invalidaría el iterador.
    if (!toRemove.empty())
    {
        anim->removeParameter(toRemove);
        ctx.pushLog("Animator: parámetro '" + toRemove + "' eliminado");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
    const char* types[] = { "bool", "trigger" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##newparamtype", &m_newParamType, types, 2);
    if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
    {
        anim->addParameter(m_newParamName,
            m_newParamType == 1 ? AnimatorComponent::ParamType::Trigger
                                : AnimatorComponent::ParamType::Bool);
        ctx.pushLog(std::string("Animator: parámetro '") + m_newParamName + "' añadido");
        m_newParamName[0] = '\0';
    }

    ImGui::EndChild();
}

void AnimatorPanel::drawGraph(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    ed::SetCurrentEditor(m_ctx);
    ed::Begin("AnimatorCanvas");

    // --- Nodos ---
    const auto& states = anim->states();
    for (size_t i = 0; i < states.size(); i++)
    {
        ed::BeginNode(nodeId((int)i));

        const bool isEntry = ((int)i == anim->entryState());
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

        // clipIndex < 0: el clip del grafo no existe en el modelo (bindClips ya
        // avisó al cargar). Se marca aquí también o el nodo mentiría.
        if (states[i].clipIndex < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "clip: %s (no existe)", states[i].clipName.c_str());
        else
            ImGui::TextDisabled("clip: %s", states[i].clipName.c_str());

        ImGui::PushID((int)i);
        bool loop = states[i].loop;
        if (ImGui::Checkbox("loop", &loop))
            anim->statesMutable()[i].loop = loop;
        ImGui::PopID();

        ed::BeginPin(inputPinId((int)i), ed::PinKind::Input);
        ImGui::TextUnformatted("-> in");
        ed::EndPin();
        ImGui::SameLine();
        ed::BeginPin(outputPinId((int)i), ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();

        ed::EndNode();
    }

    // --- Links ---
    const auto& transitions = anim->transitions();
    for (size_t t = 0; t < transitions.size(); t++)
    {
        if (transitions[t].fromState < 0 || transitions[t].toState < 0) continue;
        ed::Link(linkId((int)t),
                 outputPinId(transitions[t].fromState),
                 inputPinId(transitions[t].toState));
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
            const int outPin = isOutputPin(pa) ? pa : pb;
            const int inPin  = isOutputPin(pa) ? pb : pa;

            if (isOutputPin(outPin) && !isOutputPin(inPin) && ed::AcceptNewItem())
            {
                AnimatorComponent::Transition tr;
                tr.fromState = stateFromPin(outPin);
                tr.toState   = stateFromPin(inPin);
                // Sin condiciones no dispara nunca (por diseño): el usuario las
                // añade con doble clic en el link.
                anim->addTransition(tr);
                ctx.pushLog("Animator: transición creada (sin condiciones todavía)");
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

        std::vector<int> statesToRemove;
        ed::NodeId dn;
        while (ed::QueryDeletedNode(&dn))
            if (ed::AcceptDeletedItem())
                statesToRemove.push_back(((int)dn.Get() - 1) / 3);

        std::sort(transitionsToRemove.rbegin(), transitionsToRemove.rend());
        for (int idx : transitionsToRemove)
            anim->removeTransition(idx);

        // Se borran los links explícitamente pedidos antes que los estados: un
        // estado borrado se lleva también sus transiciones (removeState las
        // reindexa/purga), así que procesar los links sueltos primero evita
        // pisarse con esa purga automática.
        std::sort(statesToRemove.rbegin(), statesToRemove.rend());
        for (int idx : statesToRemove)
            // removeState reindexa las transiciones supervivientes.
            anim->removeState(idx);

        if (!transitionsToRemove.empty() || !statesToRemove.empty())
        {
            // Los ids posicionales del canvas (nodeId/linkId) ya no
            // corresponden al vector reindexado que queda tras el borrado: no
            // se puede fiar la posición de vuelta este mismo frame (haría
            // que syncPositionsToComponent escribiera la posición del nodo
            // borrado en el estado que ahora ocupa su índice). Se desvincula
            // el objeto pa que el próximo frame vuelque de nuevo las
            // posiciones correctas desde el componente antes de leer nada
            // del canvas — y de paso, el chequeo "m_boundTo == go" de más
            // abajo se salta el sync inverso de este frame.
            m_boundTo = nullptr;
        }
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
        m_nodeCtxTarget = ((int)ctxNode.Get() - 1) / 3;
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        m_conditionsFor = (int)ctxLink.Get() - 100000;
        ImGui::OpenPopup("link_ctx");
    }

    if (ImGui::BeginPopup("node_ctx"))
    {
        const int idx = m_nodeCtxTarget;
        if (idx >= 0 && idx < (int)anim->states().size())
        {
            if (ImGui::MenuItem("Set as Entry"))
            {
                anim->setEntryState(idx);
                ctx.pushLog("Animator: '" + anim->states()[idx].name + "' es ahora el estado de entrada");
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("link_ctx"))
    {
        if (ImGui::MenuItem("Edit Conditions...")) ImGui::OpenPopup("conditions");
        ImGui::EndPopup();
    }
    drawConditionsPopup(ctx, go);
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

void AnimatorPanel::drawConditionsPopup(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    if (m_conditionsFor < 0 || m_conditionsFor >= (int)anim->transitions().size()) return;

    if (!ImGui::BeginPopup("conditions")) return;

    auto& tr = anim->transitionsMutable()[m_conditionsFor];
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
        if (ImGui::MenuItem(("Add: " + p.name).c_str()))
        {
            AnimatorComponent::Condition cond;
            cond.type = (p.type == AnimatorComponent::ParamType::Trigger)
                          ? AnimatorComponent::ConditionType::Trigger
                          : AnimatorComponent::ConditionType::Bool;
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
    const bool fromLoops = tr.fromState >= 0 && tr.fromState < (int)anim->states().size()
                            && anim->states()[tr.fromState].loop;
    ImGui::BeginDisabled(fromLoops);
    if (ImGui::MenuItem("Add: animation finished"))
    {
        AnimatorComponent::Condition cond;
        cond.type = AnimatorComponent::ConditionType::AnimationFinished;
        tr.conditions.push_back(cond);
    }
    ImGui::EndDisabled();
    if (fromLoops && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("El estado de origen está en loop: 'animation finished' nunca dispara aquí (un clip en loop nunca termina).");

    ImGui::EndPopup();
}

void AnimatorPanel::draw(EditorContext& ctx)
{
    if (!m_open) return;
    if (!ImGui::Begin("Animator", &m_open)) { ImGui::End(); return; }

    GameObject* go = ctx.selected;
    if (!go || !go->hasAnimator())
    {
        ImGui::TextDisabled("Selecciona un GameObject con componente Animator.");
        ImGui::TextDisabled("Properties > Add > Animator");
        m_boundTo = nullptr;
        ImGui::End();
        return;
    }

    // --- Añadir estado desde los clips del modelo ---
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh || mesh->animationClips.empty())
    {
        ImGui::TextDisabled("El GameObject no tiene un mesh skinned con animaciones.");
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
            st.editorPos      = glm::vec2(40.0f + 40.0f * (float)go->getAnimator()->states().size(),
                                           40.0f + 30.0f * (float)go->getAnimator()->states().size());
            const int idx = go->getAnimator()->addState(st);
            // El nodo es nuevo: hay que colocarlo en el canvas a mano, el sync
            // general solo corre al cambiar de objeto.
            ed::SetCurrentEditor(m_ctx);
            ed::SetNodePosition(nodeId(idx), ImVec2(st.editorPos.x, st.editorPos.y));
            ed::SetCurrentEditor(nullptr);
            ctx.pushLog("Animator: estado '" + st.name + "' añadido");
        }
        ImGui::EndCombo();
    }

    drawParameterList(ctx, go);
    ImGui::SameLine();

    ImGui::BeginChild("canvas", ImVec2(0, 0), false);
    if (m_boundTo != go)
    {
        // Cambio de selección: el canvas todavía tiene las posiciones del objeto
        // anterior. Se vuelca una vez, no cada frame — si no, el usuario no
        // podría arrastrar los nodos.
        ed::SetCurrentEditor(m_ctx);
        syncPositionsFromComponent(go);
        ed::SetCurrentEditor(nullptr);
        m_boundTo = go;
    }
    drawGraph(ctx, go);
    // Si drawGraph aceptó un borrado este frame, puso m_boundTo a nullptr
    // (Finding 1): los ids del canvas ya no cuadran con el vector reindexado,
    // así que leerlos de vuelta escribiría posiciones ajenas en editorPos. Se
    // salta el sync inverso ese frame; el de arriba (m_boundTo != go) lo
    // corrige en el siguiente volcando otra vez desde el componente.
    if (m_boundTo == go)
    {
        ed::SetCurrentEditor(m_ctx);
        syncPositionsToComponent(go);
        ed::SetCurrentEditor(nullptr);
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace DonTopo
